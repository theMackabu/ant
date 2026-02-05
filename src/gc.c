#include "gc.h"
#include "arena.h"
#include "internal.h"
#include "common.h"
#include "modules/timer.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static inline void *gc_mmap(size_t size) {
  return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static inline void gc_munmap(void *ptr, size_t size) {
  (void)size;
  VirtualFree(ptr, 0, MEM_RELEASE);
}
#else
#include <sys/mman.h>
static inline void *gc_mmap(size_t size) {
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return (p == MAP_FAILED) ? NULL : p;
}
static inline void gc_munmap(void *ptr, size_t size) {
  munmap(ptr, size);
}
#endif

#define MCO_API extern
#include "minicoro.h"

static uint8_t *gc_scratch_buf = NULL;
static size_t gc_scratch_size = 0;
static time_t gc_last_run_time = 0;

static bool gc_throttled = false;
void js_gc_throttle(bool enabled) { gc_throttled = enabled; }

#define FWD_EMPTY ((jsoff_t)~0)
#define FWD_TOMBSTONE ((jsoff_t)~1)

#ifdef _WIN32
#define RELEASE_PAGES(p, sz) VirtualAlloc(p, sz, MEM_RESET, PAGE_READWRITE)
#else
#define RELEASE_PAGES(p, sz) madvise(p, sz, MADV_DONTNEED)
#endif

typedef struct {
  jsoff_t *old_offs;
  jsoff_t *new_offs;
  size_t count;
  size_t capacity;
  size_t mask;
} gc_forward_table_t;

typedef struct {
  jsoff_t *items;
  size_t count;
  size_t capacity;
} gc_work_queue_t;

typedef struct {
  ant_t *js;
  uint8_t *new_mem;
  uint8_t *mark_bits;
  gc_forward_table_t fwd;
  gc_work_queue_t work;
  jsoff_t new_brk;
  jsoff_t new_size;
  bool failed;
} gc_ctx_t;

static jsval_t gc_update_val(gc_ctx_t *ctx, jsval_t val);

static inline void mark_set(gc_ctx_t *ctx, jsoff_t off) {
  jsoff_t idx = off >> 3;
  ctx->mark_bits[idx >> 3] |= (1 << (idx & 7));
}

static inline size_t next_pow2(size_t n) {
  n--;
  n |= n >> 1; n |= n >> 2; n |= n >> 4;
  n |= n >> 8; n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFF
  n |= n >> 32;
#endif
  return n + 1;
}

static bool fwd_init(gc_forward_table_t *fwd, size_t estimated) {
  size_t cap = next_pow2(estimated < 64 ? 64 : estimated);
  size_t size = cap * sizeof(jsoff_t);
  
  fwd->old_offs = (jsoff_t *)gc_mmap(size);
  fwd->new_offs = (jsoff_t *)gc_mmap(size);
  
  if (!fwd->old_offs || !fwd->new_offs) {
    if (fwd->old_offs) gc_munmap(fwd->old_offs, size);
    if (fwd->new_offs) gc_munmap(fwd->new_offs, size);
    return false;
  }
  
  for (size_t i = 0; i < cap; i++) fwd->old_offs[i] = FWD_EMPTY;
  
  fwd->count = 0;
  fwd->capacity = cap;
  fwd->mask = cap - 1;
  
  return true;
}

static bool fwd_grow(gc_forward_table_t *fwd) {
  size_t new_cap = fwd->capacity * 2;
  size_t new_mask = new_cap - 1;
  size_t new_size = new_cap * sizeof(jsoff_t);
  size_t old_size = fwd->capacity * sizeof(jsoff_t);
  
  jsoff_t *new_old = (jsoff_t *)gc_mmap(new_size);
  jsoff_t *new_new = (jsoff_t *)gc_mmap(new_size);
  
  if (!new_old || !new_new) {
    if (new_old) gc_munmap(new_old, new_size);
    if (new_new) gc_munmap(new_new, new_size);
    return false;
  }
  
  for (size_t i = 0; i < new_cap; i++) new_old[i] = FWD_EMPTY;
  
  for (size_t i = 0; i < fwd->capacity; i++) {
    jsoff_t key = fwd->old_offs[i];
    if (key == FWD_EMPTY || key == FWD_TOMBSTONE) continue;
    size_t h = (key >> 3) & new_mask;
    while (new_old[h] != FWD_EMPTY) h = (h + 1) & new_mask;
    new_old[h] = key;
    new_new[h] = fwd->new_offs[i];
  }
  
  gc_munmap(fwd->old_offs, old_size);
  gc_munmap(fwd->new_offs, old_size);
  
  fwd->old_offs = new_old;
  fwd->new_offs = new_new;
  fwd->capacity = new_cap;
  fwd->mask = new_mask;
  
  return true;
}

static inline bool fwd_add(gc_forward_table_t *fwd, jsoff_t old_off, jsoff_t new_off) {
  if (fwd->count * 100 >= fwd->capacity * GC_FWD_LOAD_FACTOR) {
    if (!fwd_grow(fwd)) return false;
  }
  
  size_t h = (old_off >> 3) & fwd->mask;
  while (fwd->old_offs[h] != FWD_EMPTY && fwd->old_offs[h] != FWD_TOMBSTONE) {
    if (fwd->old_offs[h] == old_off) {
      fwd->new_offs[h] = new_off;
      return true;
    }
    h = (h + 1) & fwd->mask;
  }
  
  fwd->old_offs[h] = old_off;
  fwd->new_offs[h] = new_off;
  fwd->count++;
  
  return true;
}

static inline jsoff_t fwd_lookup(gc_forward_table_t *fwd, jsoff_t old_off) {
  size_t h = (old_off >> 3) & fwd->mask;
  for (size_t i = 0; i < fwd->capacity; i++) {
    jsoff_t key = fwd->old_offs[h];
    if (key == FWD_EMPTY) return (jsoff_t)~0;
    if (key == old_off) return fwd->new_offs[h];
    h = (h + 1) & fwd->mask;
  }
  return (jsoff_t)~0;
}

static void fwd_free(gc_forward_table_t *fwd) {
  size_t size = fwd->capacity * sizeof(jsoff_t);
  if (fwd->old_offs) gc_munmap(fwd->old_offs, size);
  if (fwd->new_offs) gc_munmap(fwd->new_offs, size);
  
  fwd->old_offs = NULL;
  fwd->new_offs = NULL;
  fwd->count = 0;
  fwd->capacity = 0;
}

static bool work_init(gc_work_queue_t *work, size_t initial) {
  size_t size = initial * sizeof(jsoff_t);
  work->items = (jsoff_t *)gc_mmap(size);
  if (!work->items) return false;
  work->count = 0;
  work->capacity = initial;
  return true;
}

static inline bool work_push(gc_work_queue_t *work, jsoff_t off) {
  if (work->count >= work->capacity) {
    size_t new_cap = work->capacity * 2;
    size_t new_size = new_cap * sizeof(jsoff_t);
    size_t old_size = work->capacity * sizeof(jsoff_t);
    
    jsoff_t *new_items = (jsoff_t *)gc_mmap(new_size);
    if (!new_items) return false;
    
    memcpy(new_items, work->items, work->count * sizeof(jsoff_t));
    gc_munmap(work->items, old_size);
    
    work->items = new_items;
    work->capacity = new_cap;
  }
  
  work->items[work->count++] = off;
  return true;
}

static inline jsoff_t work_pop(gc_work_queue_t *work) {
  if (work->count == 0) return (jsoff_t)~0;
  return work->items[--work->count];
}

static void work_free(gc_work_queue_t *work) {
  size_t size = work->capacity * sizeof(jsoff_t);
  if (work->items) gc_munmap(work->items, size);
  
  work->items = NULL;
  work->count = 0;
  work->capacity = 0;
}

static inline jsoff_t gc_loadoff(uint8_t *mem, jsoff_t off) {
  jsoff_t val;
  memcpy(&val, &mem[off], sizeof(val));
  return val;
}

static inline jsval_t gc_loadval(uint8_t *mem, jsoff_t off) {
  jsval_t val;
  memcpy(&val, &mem[off], sizeof(val));
  return val;
}

static inline void gc_saveoff(uint8_t *mem, jsoff_t off, jsoff_t val) {
  memcpy(&mem[off], &val, sizeof(val));
}

static inline void gc_saveval(uint8_t *mem, jsoff_t off, jsval_t val) {
  memcpy(&mem[off], &val, sizeof(val));
}

static jsoff_t gc_alloc(gc_ctx_t *ctx, size_t size) {
  size = (size + 7) / 8 * 8;
  if (ctx->new_brk + size > ctx->new_size) {
    ctx->failed = true;
    return (jsoff_t)~0;
  }
  jsoff_t off = ctx->new_brk;
  ctx->new_brk += (jsoff_t)size;
  return off;
}

static inline bool gc_is_tagged(jsval_t v) {
  return (v >> 53) == NANBOX_PREFIX_CHK;
}

static inline uint8_t gc_vtype(jsval_t v) {
  return gc_is_tagged(v) ? ((v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK) : 255;
}

static inline size_t gc_vdata(jsval_t v) {
  return (size_t)(v & NANBOX_DATA_MASK);
}

static inline jsval_t gc_mkval(uint8_t type, uint64_t data) {
  return NANBOX_PREFIX | ((jsval_t)(type & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT) | (data & NANBOX_DATA_MASK);
}

static jsoff_t gc_copy_string(gc_ctx_t *ctx, jsoff_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (jsoff_t)~0) return new_off;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & 3) != T_STR) return old_off;
  
  bool is_rope_str = (header & ROPE_FLAG) != 0;
  jsoff_t size;
  
  if (is_rope_str) {
    size = sizeof(rope_node_t);
  } else size = esize(header);
  
  if (size == (jsoff_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (jsoff_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  
  if (is_rope_str) {
    jsval_t left, right, cached;
    memcpy(&left, &ctx->js->mem[old_off + offsetof(rope_node_t, left)], sizeof(jsval_t));
    memcpy(&right, &ctx->js->mem[old_off + offsetof(rope_node_t, right)], sizeof(jsval_t));
    memcpy(&cached, &ctx->js->mem[old_off + offsetof(rope_node_t, cached)], sizeof(jsval_t));
    
    jsval_t new_left = gc_update_val(ctx, left);
    jsval_t new_right = gc_update_val(ctx, right);
    jsval_t new_cached = gc_update_val(ctx, cached);
    
    memcpy(&ctx->new_mem[new_off + offsetof(rope_node_t, left)], &new_left, sizeof(jsval_t));
    memcpy(&ctx->new_mem[new_off + offsetof(rope_node_t, right)], &new_right, sizeof(jsval_t));
    memcpy(&ctx->new_mem[new_off + offsetof(rope_node_t, cached)], &new_cached, sizeof(jsval_t));
  }
  
  return new_off;
}

static jsoff_t gc_copy_bigint(gc_ctx_t *ctx, jsoff_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (jsoff_t)~0) return new_off;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  size_t total = (header >> 4) + sizeof(jsoff_t);
  total = (total + 7) / 8 * 8;
  
  new_off = gc_alloc(ctx, total);
  if (new_off == (jsoff_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], total);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  
  return new_off;
}

static jsoff_t gc_reserve_object(gc_ctx_t *ctx, jsoff_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (jsoff_t)~0) return new_off;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & 3) != T_OBJ) return old_off;
  
  jsoff_t size = esize(header);
  if (size == (jsoff_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (jsoff_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  if (!work_push(&ctx->work, old_off)) ctx->failed = true;
  
  return new_off;
}

static jsoff_t gc_reserve_prop(gc_ctx_t *ctx, jsoff_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (jsoff_t)~0) return new_off;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & 3) != T_PROP) return old_off;
  
  jsoff_t size = esize(header);
  if (size == (jsoff_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (jsoff_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  if (!work_push(&ctx->work, old_off)) ctx->failed = true;
  
  return new_off;
}

static void gc_process_prop(gc_ctx_t *ctx, jsoff_t old_off) {
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off == (jsoff_t)~0) return;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  
  jsoff_t next_prop = header & ~(3ULL | FLAGMASK);
  if (next_prop != 0 && next_prop < ctx->js->brk) {
    jsoff_t new_next = gc_reserve_prop(ctx, next_prop);
    jsoff_t new_header = (new_next & ~3ULL) | (header & (3ULL | FLAGMASK));
    gc_saveoff(ctx->new_mem, new_off, new_header);
  }
  
  bool is_slot = (header & SLOTMASK) != 0;
  
  if (!is_slot) {
    jsoff_t key_off = gc_loadoff(ctx->js->mem, old_off + sizeof(jsoff_t));
    if (key_off < ctx->js->brk) {
      jsoff_t new_key = gc_copy_string(ctx, key_off);
      gc_saveoff(ctx->new_mem, new_off + sizeof(jsoff_t), new_key);
    }
  }
  
  jsval_t val = gc_loadval(ctx->js->mem, old_off + sizeof(jsoff_t) + sizeof(jsoff_t));
  jsval_t new_val = gc_update_val(ctx, val);
  gc_saveval(ctx->new_mem, new_off + sizeof(jsoff_t) + sizeof(jsoff_t), new_val);
}

static void gc_process_object(gc_ctx_t *ctx, jsoff_t old_off) {
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off == (jsoff_t)~0) return;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  
  jsoff_t first_prop = header & ~(3ULL | FLAGMASK);
  if (first_prop != 0 && first_prop < ctx->js->brk) {
    jsoff_t new_first = gc_reserve_prop(ctx, first_prop);
    jsoff_t new_header = (new_first & ~3ULL) | (header & (3ULL | FLAGMASK));
    gc_saveoff(ctx->new_mem, new_off, new_header);
  }
  
  jsoff_t parent_off = gc_loadoff(ctx->js->mem, old_off + sizeof(jsoff_t));
  if (parent_off < ctx->js->brk) {
    jsoff_t new_parent = gc_reserve_object(ctx, parent_off);
    gc_saveoff(ctx->new_mem, new_off + sizeof(jsoff_t), new_parent);
  }
  
  jsoff_t tail_off = gc_loadoff(ctx->js->mem, old_off + sizeof(jsoff_t) + sizeof(jsoff_t));
  if (tail_off != 0 && tail_off < ctx->js->brk) {
    jsoff_t new_tail = fwd_lookup(&ctx->fwd, tail_off);
    if (new_tail == (jsoff_t)~0) new_tail = gc_reserve_prop(ctx, tail_off);
    gc_saveoff(ctx->new_mem, new_off + sizeof(jsoff_t) + sizeof(jsoff_t), new_tail);
  }
}

static void gc_drain_work_queue(gc_ctx_t *ctx) {
  jsoff_t off;
  while ((off = work_pop(&ctx->work)) != (jsoff_t)~0) {
    jsoff_t header = gc_loadoff(ctx->js->mem, off);
    switch (header & 3) {
      case T_OBJ:  gc_process_object(ctx, off); break;
      case T_PROP: gc_process_prop(ctx, off); break;
      default: break;
    }
  }
}

static jsval_t gc_update_val(gc_ctx_t *ctx, jsval_t val) {
  if (!gc_is_tagged(val)) return val;
  
  uint8_t type = gc_vtype(val);
  jsoff_t old_off = (jsoff_t)gc_vdata(val);
  
  switch (type) {
    case T_OBJ:
    case T_FUNC:
    case T_ARR:
    case T_PROMISE:
    case T_GENERATOR: {
      if (old_off >= ctx->js->brk) return val;
      jsoff_t new_off = gc_reserve_object(ctx, old_off);
      if (new_off != (jsoff_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case T_STR: {
      if (old_off >= ctx->js->brk) return val;
      jsoff_t new_off = gc_copy_string(ctx, old_off);
      if (new_off != (jsoff_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case T_PROP: {
      if (old_off >= ctx->js->brk) return val;
      jsoff_t new_off = gc_reserve_prop(ctx, old_off);
      if (new_off != (jsoff_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case T_BIGINT: {
      if (old_off >= ctx->js->brk) return val;
      jsoff_t new_off = gc_copy_bigint(ctx, old_off);
      if (new_off != (jsoff_t)~0) return gc_mkval(type, new_off);
      break;
    }
    default: break;
  }
  
  return val;
}

static jsoff_t gc_fwd_off_callback(void *ctx_ptr, jsoff_t old_off) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  if (old_off == 0) return 0;
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (jsoff_t)~0) return new_off;
  
  static const void *dispatch[] = { &&l_obj, &&l_prop, &&l_str, &&l_default };
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  goto *dispatch[header & 3];
  
  l_obj:     new_off = gc_reserve_object(ctx, old_off); goto l_done;
  l_prop:    new_off = gc_reserve_prop(ctx, old_off);   goto l_done;
  l_str:     new_off = gc_copy_string(ctx, old_off);    goto l_done;
  l_default: return old_off;
  
  l_done: return (new_off != (jsoff_t)~0) ? new_off : old_off;
}

static jsval_t gc_fwd_val_callback(void *ctx_ptr, jsval_t val) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  return gc_update_val(ctx, val);
}

static jsoff_t gc_apply_off_callback(void *ctx_ptr, jsoff_t old_off) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  if (old_off == 0) return 0;
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  return (new_off != (jsoff_t)~0) ? new_off : old_off;
}

static jsval_t gc_apply_val(gc_ctx_t *ctx, jsval_t val) {
  if (!gc_is_tagged(val)) return val;
  
  uint8_t type = gc_vtype(val);
  jsoff_t old_off = (jsoff_t)gc_vdata(val);
  if (old_off >= ctx->js->brk) return val;
  
  switch (type) {
    case T_OBJ:
    case T_FUNC:
    case T_ARR:
    case T_PROMISE:
    case T_GENERATOR:
    case T_STR:
    case T_PROP:
    case T_BIGINT: {
      jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
      if (new_off != (jsoff_t)~0) return gc_mkval(type, new_off);
      break;
    }
    default: break;
  }
  
  return val;
}

static jsval_t gc_apply_val_callback(void *ctx_ptr, jsval_t val) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  return gc_apply_val(ctx, val);
}

static jsoff_t gc_weak_off_callback(void *ctx_ptr, jsoff_t old_off) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  if (old_off >= ctx->js->brk) return old_off;
  return fwd_lookup(&ctx->fwd, old_off);
}

size_t js_gc_compact(ant_t *js) {
  if (!js || js->brk == 0) return 0;
  if (js->brk < 2 * 1024 * 1024) return 0;

  mco_coro *running = mco_running();
  int in_coroutine = (running != NULL && running->stack_base != NULL);
  
  if (in_coroutine) {
    js->needs_gc = true;
    return 0;
  }

  time_t now = time(NULL);
  if (now != (time_t)-1 && gc_last_run_time != 0) {
    double elapsed = difftime(now, gc_last_run_time);
    double cooldown;
    
    if (js->brk > 64 * 1024 * 1024) cooldown = 0.5;
    else if (js->brk > 16 * 1024 * 1024) cooldown = 1.0;
    else if (js->brk > 4 * 1024 * 1024) cooldown = 2.0;
    else cooldown = 4.0;
    
    if (elapsed >= 0.0 
      && elapsed < cooldown 
      && js->gc_alloc_since < js->brk / 4
    ) return 0;
  }
  if (now != (time_t)-1) gc_last_run_time = now;
  
  size_t old_brk = js->brk;
  size_t new_size = js->size;
  
  if (new_size > gc_scratch_size) {
    if (gc_scratch_buf) gc_munmap(gc_scratch_buf, gc_scratch_size);
    gc_scratch_buf = (uint8_t *)gc_mmap(new_size);
    gc_scratch_size = gc_scratch_buf ? new_size : 0;
  }
  
  if (!gc_scratch_buf) return 0;
  uint8_t *new_mem = gc_scratch_buf;
  
  size_t bitmap_size = (js->brk / 8 + 7) / 8 + 1;
  uint8_t *mark_bits = (uint8_t *)calloc(1, bitmap_size);
  if (!mark_bits) return 0;
  
  size_t estimated_objs = js->brk / 64;
  if (estimated_objs < 256) estimated_objs = 256;
  
  gc_ctx_t ctx;
  ctx.js = js;
  ctx.new_mem = new_mem;
  ctx.new_brk = 0;
  ctx.new_size = (jsoff_t)new_size;
  ctx.mark_bits = mark_bits;
  
  if (!fwd_init(&ctx.fwd, estimated_objs)) {
    free(mark_bits);
    return 0;
  }
  
  if (!work_init(&ctx.work, estimated_objs / 4 < 64 ? 64 : estimated_objs / 4)) {
    fwd_free(&ctx.fwd);
    free(mark_bits);
    return 0;
  }
  
  ctx.failed = false;
    
  if (js->brk > 0) {
    jsoff_t header_at_0 = gc_loadoff(js->mem, 0);
    if ((header_at_0 & 3) == T_OBJ) gc_reserve_object(&ctx, 0);
  }
  
  js_gc_reserve_roots(js, 
    gc_fwd_off_callback,
    gc_fwd_val_callback, 
    &ctx
  ); gc_drain_work_queue(&ctx);
  
  if (ctx.failed) {
    free(mark_bits);
    work_free(&ctx.work);
    fwd_free(&ctx.fwd);
    return 0;
  }
  
  js_gc_update_roots(js, 
    gc_apply_off_callback,
    gc_weak_off_callback,
    gc_apply_val_callback, 
  &ctx);
  
  memcpy(js->mem, new_mem, ctx.new_brk);
  js->brk = ctx.new_brk;
  
  free(mark_bits);
  work_free(&ctx.work);
  fwd_free(&ctx.fwd);
  
  if (gc_scratch_buf && gc_scratch_size > 0) {
    RELEASE_PAGES(gc_scratch_buf, gc_scratch_size);
  }
  
  size_t new_brk = ctx.new_brk;
  size_t old_size = js->size;
  
  if (new_brk < old_size / 2 && old_size > ARENA_GROW_INCREMENT) {
    size_t target = ((new_brk * 2 + ARENA_GROW_INCREMENT - 1) / ARENA_GROW_INCREMENT) * ARENA_GROW_INCREMENT;
    if (target < ARENA_GROW_INCREMENT) target = ARENA_GROW_INCREMENT;
    if (target < old_size) { ant_arena_decommit(js->mem, old_size, target); js->size = (jsoff_t)target; }
  }
  
  return (old_brk > new_brk ? old_brk - new_brk : 0);
}

void js_gc_maybe(ant_t *js) {
  // Nursery scavenge (fast, frequent) - do this first
  if (js->nursery_full && js->nursery) {
    nursery_scavenge(js);
    js->nursery_full = false;
  }
  
  jsoff_t thresh = js->brk / 4;
  
  jsoff_t min_thresh = gc_throttled 
    ? 8 * 1024 * 1024 
    : 2 * 1024 * 1024;
    
  jsoff_t max_thresh = gc_throttled 
    ? 64 * 1024 * 1024 
    : 16 * 1024 * 1024;
  
  if (thresh < min_thresh) thresh = min_thresh;
  if (thresh > max_thresh) thresh = max_thresh;

  if (js->gc_alloc_since > thresh || js->needs_gc) {
    js->needs_gc = false;
    if (js_gc_compact(js) > 0) js->gc_alloc_since = 0;
  }
}

// ============================================================================
// Generational GC: Nursery Implementation
// ============================================================================

bool nursery_init(ant_t *js, size_t size) {
  if (size < NURSERY_SIZE_MIN) size = NURSERY_SIZE_MIN;
  if (size > NURSERY_SIZE_MAX) size = NURSERY_SIZE_MAX;
  
  js->nursery = (uint8_t *)gc_mmap(size);
  if (!js->nursery) return false;
  
  js->nursery_ptr = 0;
  js->nursery_size = (jsoff_t)size;
  js->nursery_full = false;
  js->nursery_enabled = false;  // enabled after snapshot loads
  
  js->remembered_set = NULL;
  js->rs_count = 0;
  js->rs_cap = 0;
  
  return true;
}

void nursery_enable(ant_t *js) {
  if (js->nursery) {
    js->nursery_enabled = true;
  }
}

void nursery_free(ant_t *js) {
  if (js->nursery) {
    gc_munmap(js->nursery, js->nursery_size);
    js->nursery = NULL;
    js->nursery_ptr = 0;
    js->nursery_size = 0;
    js->nursery_enabled = false;
  }
  
  if (js->remembered_set) {
    free(js->remembered_set);
    js->remembered_set = NULL;
    js->rs_count = 0;
    js->rs_cap = 0;
  }
}

// Add object to remembered set (old→young reference)
void rs_add(ant_t *js, jsval_t obj) {
  if (js->rs_count >= js->rs_cap) {
    size_t new_cap = js->rs_cap == 0 ? 64 : js->rs_cap * 2;
    jsval_t *new_set = (jsval_t *)realloc(js->remembered_set, new_cap * sizeof(jsval_t));
    if (!new_set) return;
    js->remembered_set = new_set;
    js->rs_cap = new_cap;
  }
  js->remembered_set[js->rs_count++] = obj;
}

void rs_clear(ant_t *js) {
  js->rs_count = 0;
}

// Nursery allocation with bump pointer (very fast)
jsoff_t nursery_alloc(ant_t *js, size_t size) {
  size = (size + 7) & ~(size_t)7;  // align to 8 bytes
  
  if (js->nursery_ptr + size > js->nursery_size) {
    js->nursery_full = true;
    return ~(jsoff_t)0;  // signal nursery full
  }
  
  jsoff_t off = js->nursery_ptr;
  js->nursery_ptr += (jsoff_t)size;
  
  return off | NURSERY_BIT;  // tag as nursery pointer
}

// Forward a nursery pointer to old gen, return new offset
static jsoff_t scavenge_copy_object(ant_t *js, jsoff_t nursery_off, jsoff_t *fwd_table, size_t fwd_cap) {
  jsoff_t raw_off = nursery_raw_off(nursery_off);
  
  // Check if already forwarded
  if (raw_off < fwd_cap && fwd_table[raw_off >> 3] != 0) {
    return fwd_table[raw_off >> 3];
  }
  
  // Get object header from nursery
  jsoff_t header;
  memcpy(&header, &js->nursery[raw_off], sizeof(header));
  
  // Calculate size based on type
  size_t size;
  uint8_t type = header & 3;
  
  if (type == T_STR) {
    if (header & ROPE_FLAG) {
      size = sizeof(rope_node_t);
    } else {
      size = esize(header);
    }
  } else {
    size = esize(header);
  }
  
  if (size == (size_t)~0 || size == 0) {
    size = 24;  // minimum object size
  }
  
  size = (size + 7) & ~(size_t)7;
  
  // Allocate in old gen
  jsoff_t available = js->size - js->brk;
  if (size > available) {
    return ~(jsoff_t)0;  // OOM
  }
  
  jsoff_t new_off = js->brk;
  memcpy(&js->mem[new_off], &js->nursery[raw_off], size);
  js->brk += (jsoff_t)size;
  
  // Install forwarding pointer
  if (raw_off < fwd_cap) {
    fwd_table[raw_off >> 3] = new_off;
  }
  
  return new_off;
}

// Context for scavenge_op_val callback
typedef struct {
  ant_t *js;
  jsoff_t *fwd_table;
  size_t fwd_cap;
} scavenge_ctx_t;

static jsval_t scavenge_update_val(ant_t *js, jsval_t val, jsoff_t *fwd_table, size_t fwd_cap);

// Wrapper callbacks for js_gc_scavenge_roots during scavenge
static void scavenge_op_val(void *ctx, jsval_t *val) {
  scavenge_ctx_t *sctx = (scavenge_ctx_t *)ctx;
  *val = scavenge_update_val(sctx->js, *val, sctx->fwd_table, sctx->fwd_cap);
}

static void scavenge_op_off(void *ctx, jsoff_t *off) {
  scavenge_ctx_t *sctx = (scavenge_ctx_t *)ctx;
  if (!is_nursery_off(*off)) return;
  jsoff_t new_off = scavenge_copy_object(sctx->js, *off, sctx->fwd_table, sctx->fwd_cap);
  if (new_off != ~(jsoff_t)0) *off = new_off;
}

// Recursively scan the scope chain and update parent pointers + properties
static void scavenge_scan_scope_chain(ant_t *js, jsoff_t scope_off, jsoff_t *fwd_table, size_t fwd_cap, int depth);

// Scan all properties of an old-gen object for nursery references
static void scavenge_scan_object_props(ant_t *js, jsoff_t obj_off, jsoff_t *fwd_table, size_t fwd_cap) {
  if (obj_off >= js->brk || is_nursery_off(obj_off)) return;
  
  jsoff_t header = gc_loadoff(js->mem, obj_off);
  jsoff_t first_prop = header & ~(3ULL | FLAGMASK);
  
  // If first property is in nursery, copy it and update object header
  if (first_prop != 0 && is_nursery_off(first_prop)) {
    jsoff_t new_first = scavenge_copy_object(js, first_prop, fwd_table, fwd_cap);
    if (new_first != ~(jsoff_t)0) {
      jsoff_t new_header = (new_first & ~3ULL) | (header & (3ULL | FLAGMASK));
      gc_saveoff(js->mem, obj_off, new_header);
      first_prop = new_first;
    }
  }
  
  jsoff_t prop_off = first_prop;
  while (prop_off != 0 && prop_off < js->brk && !is_nursery_off(prop_off)) {
    jsoff_t prop_header = gc_loadoff(js->mem, prop_off);
    jsoff_t next_prop = prop_header & ~(3ULL | FLAGMASK);
    
    // If next property is in nursery, copy it and update current prop's header
    if (next_prop != 0 && is_nursery_off(next_prop)) {
      jsoff_t new_next = scavenge_copy_object(js, next_prop, fwd_table, fwd_cap);
      if (new_next != ~(jsoff_t)0) {
        jsoff_t new_prop_header = (new_next & ~3ULL) | (prop_header & (3ULL | FLAGMASK));
        gc_saveoff(js->mem, prop_off, new_prop_header);
        next_prop = new_next;
      }
    }
    
    // Update property key if it points to nursery
    jsoff_t key_off = gc_loadoff(js->mem, prop_off + sizeof(jsoff_t));
    if (is_nursery_off(key_off)) {
      jsoff_t new_key = scavenge_copy_object(js, key_off, fwd_table, fwd_cap);
      if (new_key != ~(jsoff_t)0) {
        gc_saveoff(js->mem, prop_off + sizeof(jsoff_t), new_key);
      }
    }
    
    // Update property value if it points to nursery
    jsval_t prop_val = gc_loadval(js->mem, prop_off + sizeof(jsoff_t) * 2);
    jsval_t new_val = scavenge_update_val(js, prop_val, fwd_table, fwd_cap);
    if (new_val != prop_val) {
      gc_saveval(js->mem, prop_off + sizeof(jsoff_t) * 2, new_val);
    }
    
    prop_off = next_prop;
  }
}

// Recursively scan the scope chain and update parent pointers + properties
static void scavenge_scan_scope_chain(ant_t *js, jsoff_t scope_off, jsoff_t *fwd_table, size_t fwd_cap, int depth) {
  if (depth > 255) return;  // prevent infinite recursion
  if (scope_off == 0 || is_nursery_off(scope_off) || scope_off >= js->brk) return;
  
  // Scan this scope's properties
  scavenge_scan_object_props(js, scope_off, fwd_table, fwd_cap);
  
  // Get parent scope offset (stored at scope_off + sizeof(jsoff_t))
  jsoff_t parent_off = gc_loadoff(js->mem, scope_off + sizeof(jsoff_t));
  
  // If parent is in nursery, copy it and update the pointer
  if (parent_off != 0 && is_nursery_off(parent_off)) {
    jsoff_t new_parent = scavenge_copy_object(js, parent_off, fwd_table, fwd_cap);
    if (new_parent != ~(jsoff_t)0) {
      gc_saveoff(js->mem, scope_off + sizeof(jsoff_t), new_parent);
      parent_off = new_parent;
    }
  }
  
  // Recurse into parent scope
  if (parent_off != 0 && !is_nursery_off(parent_off) && parent_off < js->brk) {
    scavenge_scan_scope_chain(js, parent_off, fwd_table, fwd_cap, depth + 1);
  }
}

// Update a jsval_t if it points to nursery
static jsval_t scavenge_update_val(ant_t *js, jsval_t val, jsoff_t *fwd_table, size_t fwd_cap) {
  if ((val >> 53) != NANBOX_PREFIX_CHK) return val;  // not a tagged value
  
  uint8_t type = (val >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
  jsoff_t off = (jsoff_t)(val & NANBOX_DATA_MASK);
  
  if (!is_nursery_off(off)) return val;  // not in nursery
  
  switch (type) {
    case T_OBJ:
    case T_FUNC:
    case T_ARR:
    case T_PROMISE:
    case T_GENERATOR:
    case T_STR:
    case T_PROP:
    case T_BIGINT: {
      jsoff_t new_off = scavenge_copy_object(js, off, fwd_table, fwd_cap);
      if (new_off != ~(jsoff_t)0) {
        return NANBOX_PREFIX | ((jsval_t)(type & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT) | (new_off & NANBOX_DATA_MASK);
      }
      break;
    }
    default:
      break;
  }
  
  return val;
}

// Scavenge nursery: copy live objects to old gen
void nursery_scavenge(ant_t *js) {
  if (!js->nursery || js->nursery_ptr == 0) {
    js->nursery_full = false;
    return;
  }
  
  mco_coro *running = mco_running();
  if (running != NULL && running->stack_base != NULL) {
    return;
  }
  
  // Allocate forwarding table (indexed by nursery offset >> 3)
  size_t fwd_cap = js->nursery_ptr + 8;
  jsoff_t *fwd_table = (jsoff_t *)calloc(fwd_cap >> 3, sizeof(jsoff_t));
  if (!fwd_table) {
    js->nursery_ptr = 0;
    rs_clear(js);
    return;
  }
  
  jsoff_t scan_start = js->brk;
  
  // 1. Scan direct roots and copy nursery objects
  // Process remembered set (old gen objects pointing to nursery)
  for (size_t i = 0; i < js->rs_count; i++) {
    jsval_t old_obj = js->remembered_set[i];
    jsoff_t obj_off = (jsoff_t)(old_obj & NANBOX_DATA_MASK);
    
    if (is_nursery_off(obj_off)) continue;  // skip nursery objects in rs
    if (obj_off >= js->brk) continue;
    
    // Scan object's properties for nursery references
    jsoff_t header = gc_loadoff(js->mem, obj_off);
    jsoff_t prop_off = header & ~(3ULL | FLAGMASK);
    
    while (prop_off != 0 && prop_off < js->brk) {
      jsoff_t prop_header = gc_loadoff(js->mem, prop_off);
      
      // Update property value if it points to nursery
      jsval_t prop_val = gc_loadval(js->mem, prop_off + sizeof(jsoff_t) * 2);
      jsval_t new_val = scavenge_update_val(js, prop_val, fwd_table, fwd_cap);
      if (new_val != prop_val) {
        gc_saveval(js->mem, prop_off + sizeof(jsoff_t) * 2, new_val);
      }
      
      prop_off = prop_header & ~(3ULL | FLAGMASK);
    }
  }
  
  // 2. Scan all runtime roots (scope stacks, for_let_stack, this_stack,
  //    coroutines, promise registry, timers, modules, etc.)
  scavenge_ctx_t sctx = { .js = js, .fwd_table = fwd_table, .fwd_cap = fwd_cap };
  js_gc_scavenge_roots(js, scavenge_op_val, scavenge_op_off, &sctx);
  
  // Scan global object properties for nursery references
  jsoff_t global_off = (jsoff_t)(js->global & NANBOX_DATA_MASK);
  scavenge_scan_object_props(js, global_off, fwd_table, fwd_cap);
  
  // Scan the entire scope chain (current scope + all parents)
  jsoff_t scope_off = (jsoff_t)(js->scope & NANBOX_DATA_MASK);
  if (!is_nursery_off(scope_off)) {
    scavenge_scan_scope_chain(js, scope_off, fwd_table, fwd_cap, 0);
  }
  
  // 3. Cheney scan: process copied objects for more references
  jsoff_t scan = scan_start;
  int obj_count = 0;
  while (scan < js->brk) {
    jsoff_t header = gc_loadoff(js->mem, scan);
    uint8_t type = header & 3;
    size_t obj_size;
    
    // Handle rope nodes specially (strings with ROPE_FLAG set)
    if (type == T_STR && (header & ROPE_FLAG)) {
      obj_size = sizeof(rope_node_t);
    } else {
      obj_size = esize(header);
    }

    if (obj_size == (size_t)~0) obj_size = 24;
    obj_size = (obj_size + 7) & ~(size_t)7;
    obj_count++;
    
    if (type == T_OBJ || type == T_PROP) {
      // Update first prop/next prop
      jsoff_t prop_ref = header & ~(3ULL | FLAGMASK);
      if (is_nursery_off(prop_ref)) {
        jsoff_t new_ref = scavenge_copy_object(js, prop_ref, fwd_table, fwd_cap);
        if (new_ref != ~(jsoff_t)0) {

          header = (new_ref & ~3ULL) | (header & (3ULL | FLAGMASK));
          gc_saveoff(js->mem, scan, header);
        }
      }
      
      if (type == T_OBJ) {
        // Update parent
        jsoff_t parent = gc_loadoff(js->mem, scan + sizeof(jsoff_t));
        if (is_nursery_off(parent)) {
          jsoff_t new_parent = scavenge_copy_object(js, parent, fwd_table, fwd_cap);
          if (new_parent != ~(jsoff_t)0) {
            gc_saveoff(js->mem, scan + sizeof(jsoff_t), new_parent);
          }
        }
        
        // Update tail
        jsoff_t tail = gc_loadoff(js->mem, scan + sizeof(jsoff_t) * 2);
        if (is_nursery_off(tail)) {
          jsoff_t new_tail = scavenge_copy_object(js, tail, fwd_table, fwd_cap);
          if (new_tail != ~(jsoff_t)0) {
            gc_saveoff(js->mem, scan + sizeof(jsoff_t) * 2, new_tail);
          }
        }
      } else if (type == T_PROP) {
        // Update property key (string offset stored at scan + sizeof(jsoff_t))
        jsoff_t key_off = gc_loadoff(js->mem, scan + sizeof(jsoff_t));
        if (is_nursery_off(key_off)) {
          jsoff_t new_key = scavenge_copy_object(js, key_off, fwd_table, fwd_cap);
          if (new_key != ~(jsoff_t)0) {
            gc_saveoff(js->mem, scan + sizeof(jsoff_t), new_key);
          }
        }
        // Update property value
        jsval_t prop_val = gc_loadval(js->mem, scan + sizeof(jsoff_t) * 2);
        jsval_t new_val = scavenge_update_val(js, prop_val, fwd_table, fwd_cap);
        if (new_val != prop_val) {
          gc_saveval(js->mem, scan + sizeof(jsoff_t) * 2, new_val);
        }
      }
    } else if (type == T_STR && (header & ROPE_FLAG)) {
      // Rope node - update left/right/cached
      jsval_t left = gc_loadval(js->mem, scan + offsetof(rope_node_t, left));
      jsval_t right = gc_loadval(js->mem, scan + offsetof(rope_node_t, right));
      jsval_t cached = gc_loadval(js->mem, scan + offsetof(rope_node_t, cached));
      
      jsval_t new_left = scavenge_update_val(js, left, fwd_table, fwd_cap);
      jsval_t new_right = scavenge_update_val(js, right, fwd_table, fwd_cap);
      jsval_t new_cached = scavenge_update_val(js, cached, fwd_table, fwd_cap);
      
      if (new_left != left) gc_saveval(js->mem, scan + offsetof(rope_node_t, left), new_left);
      if (new_right != right) gc_saveval(js->mem, scan + offsetof(rope_node_t, right), new_right);
      if (new_cached != cached) gc_saveval(js->mem, scan + offsetof(rope_node_t, cached), new_cached);
    }
    
    scan += obj_size;
  }
  

  
  // 4. Reset nursery
  js->nursery_ptr = 0;
  js->nursery_full = false;
  rs_clear(js);
  
  // Debug: check scope chain depth and global_scope_stack
  {
    jsoff_t dbg_off = (jsoff_t)(js->scope & NANBOX_DATA_MASK);
    int chain_depth = 0;
    while (dbg_off != 0 && chain_depth < 50) {
      if (is_nursery_off(dbg_off) || dbg_off >= js->brk) break;
      dbg_off = gc_loadoff(js->mem, dbg_off + sizeof(jsoff_t));
      chain_depth++;
    }
    int stack_len = global_scope_stack ? (int)utarray_len(global_scope_stack) : 0;
    fprintf(stderr, "SCAVENGE: scope chain depth=%d, stack_len=%d, scope=0x%llx, global=0x%llx\n",
      chain_depth, stack_len,
      (unsigned long long)(js->scope & NANBOX_DATA_MASK),
      (unsigned long long)(js->global & NANBOX_DATA_MASK));
    if (stack_len > 0) {
      jsoff_t *first = (jsoff_t *)utarray_eltptr(global_scope_stack, 0);
      fprintf(stderr, "SCAVENGE: scope_stack[0]=0x%llx\n", (unsigned long long)*first);
    }
  }
  // Debug: verify no nursery refs remain in scope chain after scavenge
  {
    jsoff_t dbg_off = (jsoff_t)(js->scope & NANBOX_DATA_MASK);
    int dbg_depth = 0;
    while (dbg_off != 0 && dbg_depth < 50) {
      if (is_nursery_off(dbg_off)) {
        fprintf(stderr, "POST-SCAVENGE BUG: scope chain[%d] is nursery 0x%llx\n", dbg_depth, (unsigned long long)dbg_off);
        break;
      }
      if (dbg_off >= js->brk) {
        fprintf(stderr, "POST-SCAVENGE BUG: scope chain[%d] OOB 0x%llx (brk=0x%llx)\n", dbg_depth, (unsigned long long)dbg_off, (unsigned long long)js->brk);
        break;
      }
      jsoff_t hdr = gc_loadoff(js->mem, dbg_off);
      jsoff_t fp = hdr & ~(3ULL | FLAGMASK);
      if (fp != 0 && is_nursery_off(fp)) {
        fprintf(stderr, "POST-SCAVENGE BUG: scope[%d] first_prop nursery 0x%llx\n", dbg_depth, (unsigned long long)fp);
      }
      jsoff_t pp = fp;
      int pi = 0;
      while (pp != 0 && !is_nursery_off(pp) && pp < js->brk && pi < 500) {
        jsoff_t ph = gc_loadoff(js->mem, pp);
        jsoff_t np = ph & ~(3ULL | FLAGMASK);
        if (np != 0 && is_nursery_off(np))
          fprintf(stderr, "POST-SCAVENGE BUG: scope[%d].prop[%d] next nursery 0x%llx\n", dbg_depth, pi, (unsigned long long)np);
        jsoff_t koff = gc_loadoff(js->mem, pp + sizeof(jsoff_t));
        if (is_nursery_off(koff))
          fprintf(stderr, "POST-SCAVENGE BUG: scope[%d].prop[%d] key nursery 0x%llx\n", dbg_depth, pi, (unsigned long long)koff);
        jsval_t pv = gc_loadval(js->mem, pp + sizeof(jsoff_t) * 2);
        jsoff_t pv_off = (jsoff_t)(pv & NANBOX_DATA_MASK);
        if ((pv >> 53) == NANBOX_PREFIX_CHK && is_nursery_off(pv_off))
          fprintf(stderr, "POST-SCAVENGE BUG: scope[%d].prop[%d] val nursery 0x%llx\n", dbg_depth, pi, (unsigned long long)pv_off);
        pp = np;
        pi++;
      }
      dbg_off = gc_loadoff(js->mem, dbg_off + sizeof(jsoff_t));
      dbg_depth++;
    }
    // Also check global_scope_stack entries
    if (global_scope_stack) {
      jsoff_t *sp = NULL;
      int si = 0;
      while ((sp = (jsoff_t *)utarray_next(global_scope_stack, sp)) != NULL) {
        if (is_nursery_off(*sp))
          fprintf(stderr, "POST-SCAVENGE BUG: scope_stack[%d] nursery 0x%llx\n", si, (unsigned long long)*sp);
        si++;
      }
    }
  }
  
  free(fwd_table);
}
