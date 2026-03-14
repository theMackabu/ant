#include "gc.h"
#include "arena.h"
#include "internal.h"
#include "common.h"
#include "sugar.h"
#include "silver/engine.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <setjmp.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define ANT_HAS_ASAN 1
#endif
#endif

#ifndef ANT_HAS_ASAN
#define ANT_HAS_ASAN 0
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static inline void *gc_mmap(size_t size) {
  return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static inline void gc_munmap(void *ptr, size_t size) {
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

uint32_t gc_epoch_counter;
static uint8_t *gc_scratch_buf = NULL;

static size_t gc_scratch_size = 0;
static time_t gc_last_run_time = 0;

static bool gc_throttled = false;
void js_gc_throttle(bool enabled) { gc_throttled = enabled; }

typedef struct {
  ant_offset_t *old_offs;
  ant_offset_t *new_offs;
  size_t count;
  size_t capacity;
  size_t mask;
} gc_forward_table_t;

typedef struct {
  ant_offset_t *items;
  size_t count;
  size_t capacity;
} gc_work_queue_t;

typedef struct {
  ant_t *js;
  uint8_t *new_mem;
  uint8_t *mark_bits;
  gc_forward_table_t fwd;
  gc_work_queue_t work;
  ant_offset_t new_brk;
  ant_offset_t new_size;
  bool failed;
} gc_ctx_t;

static ant_value_t gc_update_val(gc_ctx_t *ctx, ant_value_t val);
static ant_value_t gc_apply_val(gc_ctx_t *ctx, ant_value_t val);

/* inlined helpers from ant.c */
static inline bool gc_func_const_count_sane(int n) {
  return n >= 0 && n <= (1 << 20);
}

static inline bool gc_upvalue_is_closed(const sv_upvalue_t *uv) {
  return uv && uv->location == &uv->closed;
}

static inline bool gc_is_tagged(ant_value_t v) {
  return v > NANBOX_PREFIX;
}

static inline uint8_t gc_vtype(ant_value_t v) {
  return gc_is_tagged(v) ? ((v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK) : 255;
}

static inline size_t gc_vdata(ant_value_t v) {
  return (size_t)(v & NANBOX_DATA_MASK);
}

static inline ant_value_t gc_mkval(uint8_t type, uint64_t data) {
  return NANBOX_PREFIX 
    | ((ant_value_t)(type & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT)
    | (data & NANBOX_DATA_MASK);
}

static inline void mark_set(gc_ctx_t *ctx, ant_offset_t off) {
  ant_offset_t idx = off >> 3;
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

static inline bool gc_stack_word_readable(uintptr_t addr) {
#if ANT_HAS_ASAN
  return __asan_region_is_poisoned((void *)addr, sizeof(uint64_t)) == NULL;
#else
  return true;
#endif
}

static inline bool gc_off_has_bytes(ant_offset_t off, ant_offset_t need, ant_offset_t brk) {
  return off <= brk && need <= brk - off;
}

static void gc_update_func_constants(gc_ctx_t *ctx, sv_func_t *func, int depth) {
  if (!func || depth > 1024) return;
  if (func->gc_epoch == gc_epoch_counter) return;
  
  func->gc_epoch = gc_epoch_counter;
  if (!func->constants || !gc_func_const_count_sane(func->const_count)) return;

  for (int i = 0; i < func->const_count; i++) {
    ant_value_t c = func->constants[i];

    if (vtype(c) == T_CFUNC) {
      sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(c);
      gc_update_func_constants(ctx, child, depth + 1);
      continue;
    }

    func->constants[i] = gc_update_val(ctx, c);
  }
}

static inline void gc_update_closure(gc_ctx_t *ctx, sv_closure_t *closure) {
  if (!closure) return;
  if (closure->gc_epoch == gc_epoch_counter) return;
  
  closure->gc_epoch = gc_epoch_counter;
  closure->func_obj = gc_update_val(ctx, closure->func_obj);
  closure->bound_this = gc_update_val(ctx, closure->bound_this);
  closure->bound_args = gc_update_val(ctx, closure->bound_args);
  closure->super_val = gc_update_val(ctx, closure->super_val);

  if (closure->bound_argv && closure->bound_argc > 0) {
    for (int i = 0; i < closure->bound_argc; i++)
    closure->bound_argv[i] = gc_update_val(ctx, closure->bound_argv[i]);
  }

  if (!closure->func) return;
  gc_update_func_constants(ctx, closure->func, 0);
  
  if (!closure->upvalues) return;
  int n = closure->func->upvalue_count;
  if (n <= 0 || n > (int)UINT16_MAX) return;
  
  for (int i = 0; i < n; i++) {
    sv_upvalue_t *uv = closure->upvalues[i];
    if (!gc_upvalue_is_closed(uv)) continue;
    if (uv->gc_epoch == gc_epoch_counter) continue;
    uv->gc_epoch = gc_epoch_counter;
    uv->closed = gc_update_val(ctx, uv->closed);
  }
}

static bool fwd_init(gc_forward_table_t *fwd, size_t estimated) {
  size_t cap = next_pow2(estimated < 64 ? 64 : estimated);
  size_t size = cap * sizeof(ant_offset_t);
  
  fwd->old_offs = (ant_offset_t *)gc_mmap(size);
  fwd->new_offs = (ant_offset_t *)gc_mmap(size);
  
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
  size_t new_size = new_cap * sizeof(ant_offset_t);
  size_t old_size = fwd->capacity * sizeof(ant_offset_t);
  
  ant_offset_t *new_old = (ant_offset_t *)gc_mmap(new_size);
  ant_offset_t *new_new = (ant_offset_t *)gc_mmap(new_size);
  
  if (!new_old || !new_new) {
    if (new_old) gc_munmap(new_old, new_size);
    if (new_new) gc_munmap(new_new, new_size);
    return false;
  }
  
  for (size_t i = 0; i < new_cap; i++) new_old[i] = FWD_EMPTY;
  
  for (size_t i = 0; i < fwd->capacity; i++) {
    ant_offset_t key = fwd->old_offs[i];
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

static inline bool fwd_add(gc_forward_table_t *fwd, ant_offset_t old_off, ant_offset_t new_off) {
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

static inline ant_offset_t fwd_lookup(gc_forward_table_t *fwd, ant_offset_t old_off) {
  size_t h = (old_off >> 3) & fwd->mask;
  for (size_t i = 0; i < fwd->capacity; i++) {
    ant_offset_t key = fwd->old_offs[h];
    if (key == FWD_EMPTY) return (ant_offset_t)~0;
    if (key == old_off) return fwd->new_offs[h];
    h = (h + 1) & fwd->mask;
  }
  return (ant_offset_t)~0;
}

static void fwd_free(gc_forward_table_t *fwd) {
  size_t size = fwd->capacity * sizeof(ant_offset_t);
  if (fwd->old_offs) gc_munmap(fwd->old_offs, size);
  if (fwd->new_offs) gc_munmap(fwd->new_offs, size);
  
  fwd->old_offs = NULL;
  fwd->new_offs = NULL;
  fwd->count = 0;
  fwd->capacity = 0;
}

static bool work_init(gc_work_queue_t *work, size_t initial) {
  size_t size = initial * sizeof(ant_offset_t);
  work->items = (ant_offset_t *)gc_mmap(size);
  if (!work->items) return false;
  work->count = 0;
  work->capacity = initial;
  return true;
}

static inline bool work_push(gc_work_queue_t *work, ant_offset_t off) {
  if (work->count >= work->capacity) {
    size_t new_cap = work->capacity * 2;
    size_t new_size = new_cap * sizeof(ant_offset_t);
    size_t old_size = work->capacity * sizeof(ant_offset_t);
    
    ant_offset_t *new_items = (ant_offset_t *)gc_mmap(new_size);
    if (!new_items) return false;
    
    memcpy(new_items, work->items, work->count * sizeof(ant_offset_t));
    gc_munmap(work->items, old_size);
    
    work->items = new_items;
    work->capacity = new_cap;
  }
  
  work->items[work->count++] = off;
  return true;
}

static inline ant_offset_t work_pop(gc_work_queue_t *work) {
  if (work->count == 0) return (ant_offset_t)~0;
  return work->items[--work->count];
}

static void work_free(gc_work_queue_t *work) {
  size_t size = work->capacity * sizeof(ant_offset_t);
  if (work->items) gc_munmap(work->items, size);
  
  work->items = NULL;
  work->count = 0;
  work->capacity = 0;
}

static inline ant_offset_t gc_loadoff(uint8_t *mem, ant_offset_t off) {
  ant_offset_t val;
  memcpy(&val, &mem[off], sizeof(val));
  return val;
}

static inline ant_value_t gc_loadval(uint8_t *mem, ant_offset_t off) {
  ant_value_t val;
  memcpy(&val, &mem[off], sizeof(val));
  return val;
}

static inline void gc_saveoff(uint8_t *mem, ant_offset_t off, ant_offset_t val) {
  memcpy(&mem[off], &val, sizeof(val));
}

static inline void gc_saveval(uint8_t *mem, ant_offset_t off, ant_value_t val) {
  memcpy(&mem[off], &val, sizeof(val));
}

static ant_offset_t gc_alloc(gc_ctx_t *ctx, size_t size) {
  size = (size + 7) / 8 * 8;
  if (ctx->new_brk + size > ctx->new_size) {
    ctx->failed = true;
    return (ant_offset_t)~0;
  }
  ant_offset_t off = ctx->new_brk;
  ctx->new_brk += (ant_offset_t)size;
  return off;
}


static ant_offset_t gc_copy_string(gc_ctx_t *ctx, ant_offset_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (ant_offset_t)~0) return new_off;
  
  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & 3) != T_STR) return old_off;
  
  bool is_rope_str = (header & ROPE_FLAG) != 0;
  ant_offset_t size;
  
  if (is_rope_str) {
    size = sizeof(rope_node_t);
  } else size = esize(header);
  
  if (size == (ant_offset_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (ant_offset_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  
  if (is_rope_str) {
    ant_value_t left, right, cached;
    memcpy(&left, &ctx->js->mem[old_off + offsetof(rope_node_t, left)], sizeof(ant_value_t));
    memcpy(&right, &ctx->js->mem[old_off + offsetof(rope_node_t, right)], sizeof(ant_value_t));
    memcpy(&cached, &ctx->js->mem[old_off + offsetof(rope_node_t, cached)], sizeof(ant_value_t));
    
    ant_value_t new_left = gc_update_val(ctx, left);
    ant_value_t new_right = gc_update_val(ctx, right);
    ant_value_t new_cached = gc_update_val(ctx, cached);
    
    memcpy(&ctx->new_mem[new_off + offsetof(rope_node_t, left)], &new_left, sizeof(ant_value_t));
    memcpy(&ctx->new_mem[new_off + offsetof(rope_node_t, right)], &new_right, sizeof(ant_value_t));
    memcpy(&ctx->new_mem[new_off + offsetof(rope_node_t, cached)], &new_cached, sizeof(ant_value_t));
  }
  
  return new_off;
}

typedef struct {
  ant_offset_t total_aligned;
  ant_offset_t limbs_off;
  uint32_t limb_count;
  uint8_t sign;
} gc_bigint_meta_t;

static bool gc_read_bigint_meta(
  gc_ctx_t *ctx,
  ant_offset_t old_off,
  ant_offset_t old_brk,
  gc_bigint_meta_t *meta
) {
  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & GC_BIGINT_HEADER_LOW_MASK) != 0) return false;

  ant_offset_t payload = header >> GC_BIGINT_HEADER_SHIFT;
  const ant_offset_t fixed_payload = (ant_offset_t)(sizeof(uint32_t) + sizeof(uint32_t));
  if (payload < fixed_payload + (ant_offset_t)sizeof(uint32_t)) return false;

  ant_offset_t total = payload + (ant_offset_t)sizeof(ant_offset_t);
  total = (total + 7) & ~(ant_offset_t)7;
  if (!gc_off_has_bytes(old_off, total, old_brk)) return false;

  ant_offset_t sign_off = old_off + (ant_offset_t)sizeof(ant_offset_t);
  uint8_t sign = ctx->js->mem[sign_off];
  if (sign > 1) return false;

  ant_offset_t count_off = old_off + (ant_offset_t)sizeof(ant_offset_t) + (ant_offset_t)sizeof(uint32_t);
  uint32_t limb_count = 0;
  memcpy(&limb_count, &ctx->js->mem[count_off], sizeof(limb_count));
  if (limb_count == 0) return false;

  ant_offset_t expected_payload = fixed_payload + (ant_offset_t)limb_count * (ant_offset_t)sizeof(uint32_t);
  if (expected_payload != payload) return false;

  ant_offset_t limbs_off = count_off + (ant_offset_t)sizeof(uint32_t);
  uint32_t top_limb = 0;
  memcpy(
    &top_limb,
    &ctx->js->mem[limbs_off + (ant_offset_t)(limb_count - 1) * (ant_offset_t)sizeof(uint32_t)],
    sizeof(top_limb)
  );
  if (top_limb == 0 && limb_count > 1) return false;

  if (limb_count == 1) {
    uint32_t limb0 = 0;
    memcpy(&limb0, &ctx->js->mem[limbs_off], sizeof(limb0));
    if (limb0 == 0 && sign != 0) return false;
  }

  if (meta) {
    meta->total_aligned = total;
    meta->limbs_off = limbs_off;
    meta->limb_count = limb_count;
    meta->sign = sign;
  }

  return true;
}

static ant_offset_t gc_copy_bigint(gc_ctx_t *ctx, ant_offset_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (ant_offset_t)~0) return new_off;

  gc_bigint_meta_t meta;
  if (!gc_read_bigint_meta(ctx, old_off, ctx->js->brk, &meta)) return old_off;
  
  new_off = gc_alloc(ctx, (size_t)meta.total_aligned);
  if (new_off == (ant_offset_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], (size_t)meta.total_aligned);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  
  return new_off;
}

static ant_offset_t gc_copy_symbol(gc_ctx_t *ctx, ant_offset_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (ant_offset_t)~0) return new_off;
  
  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);
  size_t total = (header >> GC_SYM_HEADER_SHIFT) + sizeof(ant_offset_t);
  total = (total + 7) / 8 * 8;
  
  new_off = gc_alloc(ctx, total);
  if (new_off == (ant_offset_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], total);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  
  return new_off;
}

static ant_offset_t gc_reserve_object(gc_ctx_t *ctx, ant_offset_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (ant_offset_t)~0) return new_off;
  
  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & 3) != T_OBJ) return old_off;
  
  ant_offset_t size = esize(header);
  if (size == (ant_offset_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (ant_offset_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  if (!work_push(&ctx->work, old_off)) ctx->failed = true;
  
  return new_off;
}

static ant_offset_t gc_reserve_prop(gc_ctx_t *ctx, ant_offset_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (ant_offset_t)~0) return new_off;
  
  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & 3) != T_PROP) return old_off;
  
  ant_offset_t size = esize(header);
  if (size == (ant_offset_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (ant_offset_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  if (!work_push(&ctx->work, old_off)) ctx->failed = true;
  
  return new_off;
}

static void gc_process_prop(gc_ctx_t *ctx, ant_offset_t old_off) {
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off == (ant_offset_t)~0) return;
  
  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);
  ant_offset_t next_prop = header & ~(3ULL | FLAGMASK);
  
  if (next_prop != 0 && next_prop < ctx->js->brk) {
    ant_offset_t new_next = gc_reserve_prop(ctx, next_prop);
    ant_offset_t new_header = (new_next & ~3ULL) | (header & (3ULL | FLAGMASK));
    gc_saveoff(ctx->new_mem, new_off, new_header);
  }
  
  bool is_slot = (header & SLOTMASK) != 0;
  
  if (!is_slot) {
    ant_offset_t key_off = gc_loadoff(ctx->js->mem, old_off + sizeof(ant_offset_t));
    if (key_off < ctx->js->brk) {
      ant_offset_t key_hdr = gc_loadoff(ctx->js->mem, key_off); ant_offset_t new_key;
      if ((key_hdr & 3) == T_STR) new_key = gc_copy_string(ctx, key_off);
      else new_key = gc_copy_symbol(ctx, key_off);
      gc_saveoff(ctx->new_mem, new_off + sizeof(ant_offset_t), new_key);
    }
  }
  
  ant_value_t val = gc_loadval(ctx->js->mem, old_off + sizeof(ant_offset_t) + sizeof(ant_offset_t));
  if (!is_slot) goto update_val;

  ant_offset_t slot_id = gc_loadoff(ctx->js->mem, old_off + sizeof(ant_offset_t));
  if (slot_id != (ant_offset_t)SLOT_DENSE_BUF) goto update_val;

  ant_offset_t old_doff = (ant_offset_t)tod(val);
  if (old_doff == 0 || old_doff >= ctx->js->brk) goto update_val;

  ant_offset_t cap = gc_loadoff(ctx->js->mem, old_doff);
  ant_offset_t len = gc_loadoff(ctx->js->mem, old_doff + sizeof(ant_offset_t));
  ant_offset_t buf_size = (ant_offset_t)(sizeof(ant_offset_t) * 2 + sizeof(ant_value_t) * cap);
  ant_offset_t new_doff = gc_alloc(ctx, buf_size);
  if (new_doff == (ant_offset_t)~0) return;

  memcpy(&ctx->new_mem[new_doff], &ctx->js->mem[old_doff], buf_size);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_offset_t voff = new_doff + (ant_offset_t)(sizeof(ant_offset_t) * 2 + sizeof(ant_value_t) * i);
    ant_value_t v = gc_loadval(ctx->new_mem, voff);
    if (v != T_EMPTY) {
      ant_value_t nv = gc_update_val(ctx, v);
      gc_saveval(ctx->new_mem, voff, nv);
    }
  }
  
  gc_saveval(
    ctx->new_mem,
    new_off + sizeof(ant_offset_t) + sizeof(ant_offset_t), 
    tov((double)new_doff)
  ); return;

  update_val: {
    ant_value_t new_val = gc_update_val(ctx, val);
    gc_saveval(ctx->new_mem, new_off + sizeof(ant_offset_t) + sizeof(ant_offset_t), new_val);
  }
}

static void gc_process_object(gc_ctx_t *ctx, ant_offset_t old_off) {
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off == (ant_offset_t)~0) return;
  
  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);
  
  ant_offset_t first_prop = header & ~(3ULL | FLAGMASK);
  if (first_prop != 0 && first_prop < ctx->js->brk) {
    ant_offset_t new_first = gc_reserve_prop(ctx, first_prop);
    ant_offset_t new_header = (new_first & ~3ULL) | (header & (3ULL | FLAGMASK));
    gc_saveoff(ctx->new_mem, new_off, new_header);
  }
  
  ant_offset_t parent_off = gc_loadoff(ctx->js->mem, old_off + sizeof(ant_offset_t));
  if (parent_off < ctx->js->brk) {
    ant_offset_t new_parent = gc_reserve_object(ctx, parent_off);
    gc_saveoff(ctx->new_mem, new_off + sizeof(ant_offset_t), new_parent);
  }
  
  ant_offset_t tail_off = gc_loadoff(ctx->js->mem, old_off + sizeof(ant_offset_t) + sizeof(ant_offset_t));
  if (tail_off != 0 && tail_off < ctx->js->brk) {
    ant_offset_t new_tail = fwd_lookup(&ctx->fwd, tail_off);
    if (new_tail == (ant_offset_t)~0) new_tail = gc_reserve_prop(ctx, tail_off);
    gc_saveoff(ctx->new_mem, new_off + sizeof(ant_offset_t) + sizeof(ant_offset_t), new_tail);
  }
}

static void gc_drain_work_queue(gc_ctx_t *ctx) {
  ant_offset_t off;
  while ((off = work_pop(&ctx->work)) != (ant_offset_t)~0) {
    ant_offset_t header = gc_loadoff(ctx->js->mem, off);
    switch (header & 3) {
      case T_OBJ:  gc_process_object(ctx, off); break;
      case T_PROP: gc_process_prop(ctx, off); break;
      default: break;
    }
  }
}

static ant_value_t gc_update_val(gc_ctx_t *ctx, ant_value_t val) {
  if (!gc_is_tagged(val)) return val;
  
  uint8_t type = gc_vtype(val);
  ant_offset_t old_off = (ant_offset_t)gc_vdata(val);
  
  switch (type) {
    case T_OBJ:
    case T_ARR:
    case T_PROMISE:
    case T_GENERATOR: {
      if (old_off >= ctx->js->brk) return val;
      ant_offset_t new_off = gc_reserve_object(ctx, old_off);
      if (new_off != (ant_offset_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case T_STR: {
      if (old_off >= ctx->js->brk) return val;
      ant_offset_t new_off = gc_copy_string(ctx, old_off);
      if (new_off != (ant_offset_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case T_PROP: {
      if (old_off >= ctx->js->brk) return val;
      ant_offset_t new_off = gc_reserve_prop(ctx, old_off);
      if (new_off != (ant_offset_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case T_BIGINT: {
      if (old_off >= ctx->js->brk) return val;
      ant_offset_t new_off = gc_copy_bigint(ctx, old_off);
      if (new_off != (ant_offset_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case T_SYMBOL: {
      if (old_off >= ctx->js->brk) return val;
      ant_offset_t new_off = gc_copy_symbol(ctx, old_off);
      if (new_off != (ant_offset_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case T_FUNC:
    case T_CLOSURE: {
      gc_update_closure(ctx, (sv_closure_t *)(uintptr_t)gc_vdata(val));
      return val;
    }
    default: break;
  }
  
  return val;
}

static ant_offset_t gc_fwd_off_callback(void *ctx_ptr, ant_offset_t old_off) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  if (old_off == 0) return 0;
  if (old_off >= ctx->js->brk) return old_off;
  
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (ant_offset_t)~0) return new_off;
  
  static const void *dispatch[] = { &&l_obj, &&l_prop, &&l_str, &&l_default };
  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);
  goto *dispatch[header & 3];
  
  l_obj:     new_off = gc_reserve_object(ctx, old_off); goto l_done;
  l_prop:    new_off = gc_reserve_prop(ctx, old_off);   goto l_done;
  l_str:     new_off = gc_copy_string(ctx, old_off);    goto l_done;
  l_default: return old_off;
  
  l_done: return (new_off != (ant_offset_t)~0) ? new_off : old_off;
}

static ant_value_t gc_fwd_val_callback(void *ctx_ptr, ant_value_t val) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  return gc_update_val(ctx, val);
}

static ant_offset_t gc_apply_off_callback(void *ctx_ptr, ant_offset_t old_off) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  if (old_off == 0) return 0;
  if (old_off >= ctx->js->brk) return old_off;
  
  ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
  return (new_off != (ant_offset_t)~0) ? new_off : old_off;
}

static ant_value_t gc_apply_val(gc_ctx_t *ctx, ant_value_t val) {
  if (!gc_is_tagged(val)) return val;
  
  uint8_t type = gc_vtype(val);
  if (type == T_FUNC || type == T_CLOSURE) return val;

  ant_offset_t old_off = (ant_offset_t)gc_vdata(val);
  if (old_off >= ctx->js->brk) return val;
  
  switch (type) {
    case T_OBJ:
    case T_ARR:
    case T_PROMISE:
    case T_GENERATOR:
    case T_STR:
    case T_PROP:
    case T_BIGINT:
    case T_SYMBOL: {
      ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
      if (new_off != (ant_offset_t)~0) return gc_mkval(type, new_off);
      break;
    }
    default: break;
  }
  
  return val;
}

static ant_value_t gc_apply_val_callback(void *ctx_ptr, ant_value_t val) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  return gc_apply_val(ctx, val);
}

static ant_offset_t gc_weak_off_callback(void *ctx_ptr, ant_offset_t old_off) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  if (old_off >= ctx->js->brk) return old_off;
  return fwd_lookup(&ctx->fwd, old_off);
}

static inline bool gc_get_stack_bounds(uintptr_t base, uintptr_t sp, uintptr_t *lo, uintptr_t *hi) {
  if (base == 0 || sp == 0) return false;

  uintptr_t minp = (sp < base) ? sp : base;
  uintptr_t maxp = (sp < base) ? base : sp;

  uintptr_t aligned_lo = (minp + (uintptr_t)sizeof(uint64_t) - 1u) & ~((uintptr_t)sizeof(uint64_t) - 1u);
  uintptr_t aligned_hi = maxp & ~((uintptr_t)sizeof(uint64_t) - 1u);

  if (aligned_lo >= aligned_hi) return false;
  *lo = aligned_lo;
  *hi = aligned_hi;
  return true;
}

static inline bool gc_stack_word_valid(gc_ctx_t *ctx, uint8_t type, ant_offset_t old_off, ant_offset_t old_brk) {
  if (old_off == 0 || old_off >= old_brk) return false;
  if (!gc_off_has_bytes(old_off, (ant_offset_t)sizeof(ant_offset_t), old_brk)) return false;

  ant_offset_t header = gc_loadoff(ctx->js->mem, old_off);

  switch (type) {
    case T_OBJ:
    case T_ARR:
    case T_PROMISE:
    case T_GENERATOR: {
      if ((header & 3) != T_OBJ) return false;
      ant_offset_t size = esize(header);
      return size != (ant_offset_t)~0 && gc_off_has_bytes(old_off, size, old_brk);
    }
    case T_PROP: {
      if ((header & 3) != T_PROP) return false;
      ant_offset_t size = esize(header);
      return size != (ant_offset_t)~0 && gc_off_has_bytes(old_off, size, old_brk);
    }
    case T_STR: {
      if ((header & 3) != T_STR) return false;
      if ((header & ROPE_FLAG) != 0)
        return gc_off_has_bytes(old_off, (ant_offset_t)sizeof(rope_node_t), old_brk);
      ant_offset_t size = esize(header);
      return size != (ant_offset_t)~0 && gc_off_has_bytes(old_off, size, old_brk);
    }
    case T_BIGINT: {
      return gc_read_bigint_meta(ctx, old_off, old_brk, NULL);
    }
    case T_SYMBOL: {
      if ((header & GC_SYM_HEADER_LOW_MASK) != 0) return false;
      ant_offset_t payload = header >> GC_SYM_HEADER_SHIFT;
      if (payload < GC_SYM_HEAP_FIXED) return false;
      
      ant_offset_t total = payload + (ant_offset_t)sizeof(ant_offset_t);
      total = (total + 7) & ~(ant_offset_t)7;
      return gc_off_has_bytes(old_off, total, old_brk);
    }
    default: return false;
  }
}

__attribute__((noinline))
static void gc_scan_stack_reserve(gc_ctx_t *ctx) {
  jmp_buf jb;
  if (setjmp(jb) != 0) return;

  volatile uint8_t sp_marker = 0; uintptr_t lo, hi;
  if (!gc_get_stack_bounds((uintptr_t)ctx->js->cstk.base, (uintptr_t)&sp_marker, &lo, &hi)) return;

  ant_offset_t old_brk = ctx->js->brk;

  for (uintptr_t addr = lo; addr < hi; addr += sizeof(uint64_t)) {
    if (!gc_stack_word_readable(addr)) continue;
    uint64_t w;
    memcpy(&w, (void *)addr, sizeof(w));

    if (w <= NANBOX_PREFIX) continue;

    uint8_t type = (w >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
    if (type > T_FFI) continue;
    if (!((1u << type) & GC_HEAP_TYPE_MASK)) continue;

    ant_offset_t old_off = (ant_offset_t)(w & NANBOX_DATA_MASK);
    if (old_off == 0 || old_off >= old_brk) continue;
    if (!gc_stack_word_valid(ctx, type, old_off, old_brk)) continue;

    if (fwd_lookup(&ctx->fwd, old_off) != (ant_offset_t)~0) continue;
    gc_update_val(ctx, w);
  }
}

__attribute__((noinline))
static void gc_scan_stack_update(gc_ctx_t *ctx) {
  jmp_buf jb;
  if (setjmp(jb) != 0) return;

  volatile uint8_t sp_marker = 0; uintptr_t lo, hi;
  if (!gc_get_stack_bounds((uintptr_t)ctx->js->cstk.base, (uintptr_t)&sp_marker, &lo, &hi)) return;

  ant_offset_t old_brk = ctx->js->brk;

  for (uintptr_t addr = lo; addr < hi; addr += sizeof(uint64_t)) {
    if (!gc_stack_word_readable(addr)) continue;
    uint64_t w;
    memcpy(&w, (void *)addr, sizeof(w));

    if (w <= NANBOX_PREFIX) continue;

    uint8_t type = (w >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
    if (type > T_FFI) continue;
    if (!((1u << type) & GC_HEAP_TYPE_MASK)) continue;

    ant_offset_t old_off = (ant_offset_t)(w & NANBOX_DATA_MASK);
    if (old_off == 0 || old_off >= old_brk) continue;
    if (!gc_stack_word_valid(ctx, type, old_off, old_brk)) continue;

    ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
    if (new_off == (ant_offset_t)~0 || new_off == old_off) continue;

    uint64_t updated = gc_mkval(type, new_off);
    memcpy((void *)addr, &updated, sizeof(updated));
  }
}

static void gc_scan_range_reserve(gc_ctx_t *ctx, uintptr_t lo, uintptr_t hi) {
  ant_offset_t old_brk = ctx->js->brk;
  for (uintptr_t addr = lo; addr < hi; addr += sizeof(uint64_t)) {
    if (!gc_stack_word_readable(addr)) continue;
    uint64_t w;
    memcpy(&w, (void *)addr, sizeof(w));
    if (w <= NANBOX_PREFIX) continue;
    uint8_t type = (w >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
    if (type > T_FFI) continue;
    if (!((1u << type) & GC_HEAP_TYPE_MASK)) continue;
    ant_offset_t old_off = (ant_offset_t)(w & NANBOX_DATA_MASK);
    if (old_off == 0 || old_off >= old_brk) continue;
    if (!gc_stack_word_valid(ctx, type, old_off, old_brk)) continue;
    if (fwd_lookup(&ctx->fwd, old_off) != (ant_offset_t)~0) continue;
    gc_update_val(ctx, w);
  }
}

static void gc_scan_range_update(gc_ctx_t *ctx, uintptr_t lo, uintptr_t hi) {
  ant_offset_t old_brk = ctx->js->brk;
  for (uintptr_t addr = lo; addr < hi; addr += sizeof(uint64_t)) {
    if (!gc_stack_word_readable(addr)) continue;
    uint64_t w;
    memcpy(&w, (void *)addr, sizeof(w));
    if (w <= NANBOX_PREFIX) continue;
    uint8_t type = (w >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
    if (type > T_FFI) continue;
    if (!((1u << type) & GC_HEAP_TYPE_MASK)) continue;
    ant_offset_t old_off = (ant_offset_t)(w & NANBOX_DATA_MASK);
    if (old_off == 0 || old_off >= old_brk) continue;
    if (!gc_stack_word_valid(ctx, type, old_off, old_brk)) continue;
    ant_offset_t new_off = fwd_lookup(&ctx->fwd, old_off);
    if (new_off == (ant_offset_t)~0 || new_off == old_off) continue;
    uint64_t updated = gc_mkval(type, new_off);
    memcpy((void *)addr, &updated, sizeof(updated));
  }
}

static inline void gc_scan_mco_stack(
  gc_ctx_t *ctx, mco_coro *mco, mco_coro *skip,
  void (*scan)(gc_ctx_t *, uintptr_t, uintptr_t)
) {
  if (!mco || mco == skip) return;
  if (!mco->stack_base || mco->stack_size == 0) return;
  uintptr_t lo = (uintptr_t)mco->stack_base;
  uintptr_t hi = lo + mco->stack_size;
  scan(ctx, lo, hi);
}

static void gc_scan_other_stacks_reserve(gc_ctx_t *ctx) {
  mco_coro *running = mco_running();
  if (!running) return;

  for (mco_coro *a = running->prev_co; a; a = a->prev_co)
    gc_scan_mco_stack(ctx, a, running, gc_scan_range_reserve);

  for (coroutine_t *c = pending_coroutines.head; c; c = c->next)
    gc_scan_mco_stack(ctx, c->mco, running, gc_scan_range_reserve);

  if (ctx->js->cstk.main_base && ctx->js->cstk.main_lo) {
    uintptr_t lo, hi;
    if (gc_get_stack_bounds(
      (uintptr_t)ctx->js->cstk.main_base, 
      (uintptr_t)ctx->js->cstk.main_lo, &lo, &hi)
    ) gc_scan_range_reserve(ctx, lo, hi);
  }
}

static void gc_scan_other_stacks_update(gc_ctx_t *ctx) {
  mco_coro *running = mco_running();
  if (!running) return;

  for (mco_coro *a = running->prev_co; a; a = a->prev_co)
    gc_scan_mco_stack(ctx, a, running, gc_scan_range_update);

  for (coroutine_t *c = pending_coroutines.head; c; c = c->next)
    gc_scan_mco_stack(ctx, c->mco, running, gc_scan_range_update);

  if (ctx->js->cstk.main_base && ctx->js->cstk.main_lo) {
    uintptr_t lo, hi;
    if (gc_get_stack_bounds(
      (uintptr_t)ctx->js->cstk.main_base,
      (uintptr_t)ctx->js->cstk.main_lo, &lo, &hi)
    ) gc_scan_range_update(ctx, lo, hi);
  }
}

static void gc_forward_func_cb(void *ctx_ptr, sv_func_t *func) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  gc_update_func_constants(ctx, func, 0);
}

static void gc_compact(ant_t *js) {
  js->needs_gc = false;
  return;
  
  if (!js || js->brk == 0) return;
  if (js->brk < 2 * 1024 * 1024) return;

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
    ) return;
  }
  
  if (now != (time_t)-1) gc_last_run_time = now;
  size_t new_size = js->size;
  
  if (new_size > gc_scratch_size) {
    if (gc_scratch_buf) gc_munmap(gc_scratch_buf, gc_scratch_size);
    gc_scratch_buf = (uint8_t *)gc_mmap(new_size);
    gc_scratch_size = gc_scratch_buf ? new_size : 0;
  }
  
  if (!gc_scratch_buf) return;
  uint8_t *new_mem = gc_scratch_buf;
  
  size_t bitmap_size = (js->brk / 8 + 7) / 8 + 1;
  uint8_t *mark_bits = (uint8_t *)calloc(1, bitmap_size);
  if (!mark_bits) return;
  
  size_t estimated_objs = js->brk / 64;
  if (estimated_objs < 256) estimated_objs = 256;
  
  gc_ctx_t ctx;
  ctx.js = js;
  ctx.new_mem = new_mem;
  ctx.new_brk = NANBOX_HEAP_OFFSET;
  ctx.new_size = (ant_offset_t)new_size;
  ctx.mark_bits = mark_bits;
  
  if (!fwd_init(&ctx.fwd, estimated_objs)) {
    free(mark_bits); return;
  }
  
  if (!work_init(&ctx.work, estimated_objs / 4 < 64 ? 64 : estimated_objs / 4)) {
    fwd_free(&ctx.fwd);
    free(mark_bits); return;
  }
  
  ctx.failed = false;
  gc_epoch_counter++;
  
  if (js->brk > 0) {
    ant_offset_t header_at_0 = gc_loadoff(js->mem, 0);
    if ((header_at_0 & 3) == T_OBJ) gc_reserve_object(&ctx, 0);
  }
  
  js_gc_reserve_roots(js, 
    gc_fwd_off_callback,
    gc_fwd_val_callback, 
    &ctx
  ); gc_drain_work_queue(&ctx);
  
  gc_scan_stack_reserve(&ctx);
  gc_scan_other_stacks_reserve(&ctx);
  gc_drain_work_queue(&ctx);
  
  if (ctx.failed) {
    free(mark_bits);
    work_free(&ctx.work);
    fwd_free(&ctx.fwd); return;
  }

  js_gc_visit_frame_funcs(js, gc_forward_func_cb, &ctx);
  gc_drain_work_queue(&ctx);

  js_gc_update_roots(js, 
    gc_apply_off_callback,
    gc_weak_off_callback,
    gc_apply_val_callback, 
  &ctx);
  
  gc_scan_stack_update(&ctx);
  gc_scan_other_stacks_update(&ctx);
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
  
  if (new_brk < old_size * 3 / 4 && old_size > ARENA_GROW_INCREMENT) {
    size_t target = ((new_brk * 3 / 2 + ARENA_GROW_INCREMENT - 1) / ARENA_GROW_INCREMENT) * ARENA_GROW_INCREMENT;
    if (target < ARENA_GROW_INCREMENT) target = ARENA_GROW_INCREMENT;
    if (target < old_size) { ant_arena_decommit(js->mem, old_size, target); js->size = (ant_offset_t)target; }
  }
}

void js_gc_maybe(ant_t *js) {
#ifdef ANT_JIT
  if (js->jit_active_depth > 0) return;
#endif
  ant_offset_t thresh = js->brk / 4;
  
  ant_offset_t min_thresh = gc_throttled 
    ? 8 * 1024 * 1024 
    : 2 * 1024 * 1024;
    
  ant_offset_t max_thresh = gc_throttled 
    ? 64 * 1024 * 1024 
    : 16 * 1024 * 1024;
  
  if (thresh < min_thresh) thresh = min_thresh;
  if (thresh > max_thresh) thresh = max_thresh;

  if (js->gc_alloc_since > thresh || js->needs_gc) {
    gc_compact(js);
    js->gc_alloc_since = 0;
  }
}
