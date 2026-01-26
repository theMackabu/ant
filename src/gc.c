#include "arena.h"
#include "internal.h"
#include "common.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MCO_API extern
#include "minicoro.h"

#define GC_MIN_HEAP_SIZE (64 * 1024)
#define GC_SHRINK_THRESHOLD 4
#define GC_FWD_LOAD_FACTOR 70

#define FWD_EMPTY ((jsoff_t)~0)
#define FWD_TOMBSTONE ((jsoff_t)~1)

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
  jsoff_t idx = off >> 2;
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
  fwd->old_offs = (jsoff_t *)ANT_GC_MALLOC(cap * sizeof(jsoff_t));
  fwd->new_offs = (jsoff_t *)ANT_GC_MALLOC(cap * sizeof(jsoff_t));
  if (!fwd->old_offs || !fwd->new_offs) {
    if (fwd->old_offs) ANT_GC_FREE(fwd->old_offs);
    if (fwd->new_offs) ANT_GC_FREE(fwd->new_offs);
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
  jsoff_t *new_old = (jsoff_t *)ANT_GC_MALLOC(new_cap * sizeof(jsoff_t));
  jsoff_t *new_new = (jsoff_t *)ANT_GC_MALLOC(new_cap * sizeof(jsoff_t));
  if (!new_old || !new_new) {
    if (new_old) ANT_GC_FREE(new_old);
    if (new_new) ANT_GC_FREE(new_new);
    return false;
  }
  for (size_t i = 0; i < new_cap; i++) new_old[i] = FWD_EMPTY;
  
  for (size_t i = 0; i < fwd->capacity; i++) {
    jsoff_t key = fwd->old_offs[i];
    if (key == FWD_EMPTY || key == FWD_TOMBSTONE) continue;
    size_t h = (key >> 2) & new_mask;
    while (new_old[h] != FWD_EMPTY) h = (h + 1) & new_mask;
    new_old[h] = key;
    new_new[h] = fwd->new_offs[i];
  }
  
  ANT_GC_FREE(fwd->old_offs);
  ANT_GC_FREE(fwd->new_offs);
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
  size_t h = (old_off >> 2) & fwd->mask;
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
  size_t h = (old_off >> 2) & fwd->mask;
  for (size_t i = 0; i < fwd->capacity; i++) {
    jsoff_t key = fwd->old_offs[h];
    if (key == FWD_EMPTY) return (jsoff_t)~0;
    if (key == old_off) return fwd->new_offs[h];
    h = (h + 1) & fwd->mask;
  }
  return (jsoff_t)~0;
}

static void fwd_free(gc_forward_table_t *fwd) {
  if (fwd->old_offs) ANT_GC_FREE(fwd->old_offs);
  if (fwd->new_offs) ANT_GC_FREE(fwd->new_offs);
  fwd->old_offs = NULL;
  fwd->new_offs = NULL;
  fwd->count = 0;
  fwd->capacity = 0;
}

static bool work_init(gc_work_queue_t *work, size_t initial) {
  work->items = (jsoff_t *)ANT_GC_MALLOC(initial * sizeof(jsoff_t));
  if (!work->items) return false;
  work->count = 0;
  work->capacity = initial;
  return true;
}

static inline bool work_push(gc_work_queue_t *work, jsoff_t off) {
  if (work->count >= work->capacity) {
    size_t new_cap = work->capacity * 2;
    jsoff_t *new_items = (jsoff_t *)ANT_GC_MALLOC(new_cap * sizeof(jsoff_t));
    if (!new_items) return false;
    memcpy(new_items, work->items, work->count * sizeof(jsoff_t));
    ANT_GC_FREE(work->items);
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
  if (work->items) ANT_GC_FREE(work->items);
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
  size = (size + 3) / 4 * 4;
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
  
  jsoff_t size = esize(header);
  if (size == (jsoff_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (jsoff_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  if (!fwd_add(&ctx->fwd, old_off, new_off)) ctx->failed = true;
  mark_set(ctx, old_off);
  
  return new_off;
}

static jsoff_t gc_copy_bigint(gc_ctx_t *ctx, jsoff_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (jsoff_t)~0) return new_off;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  size_t total = (header >> 4) + sizeof(jsoff_t);
  total = (total + 3) / 4 * 4;
  
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
  
  jsoff_t next_prop = header & ~(3U | FLAGMASK);
  if (next_prop != 0 && next_prop < ctx->js->brk) {
    jsoff_t new_next = gc_reserve_prop(ctx, next_prop);
    jsoff_t new_header = (new_next & ~3U) | (header & (3U | FLAGMASK));
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
  
  jsoff_t first_prop = header & ~(3U | FLAGMASK);
  if (first_prop != 0 && first_prop < ctx->js->brk) {
    jsoff_t new_first = gc_reserve_prop(ctx, first_prop);
    jsoff_t new_header = (new_first & ~3U) | (header & (3U | FLAGMASK));
    gc_saveoff(ctx->new_mem, new_off, new_header);
  }
  
  jsoff_t parent_off = gc_loadoff(ctx->js->mem, old_off + sizeof(jsoff_t));
  if (parent_off != 0 && parent_off < ctx->js->brk) {
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

size_t js_gc_compact(ant_t *js) {
  if (!js || js->brk == 0) return 0;
  
  mco_coro *running = mco_running();
  int in_coroutine = (running != NULL && running->stack_base != NULL);
  if (in_coroutine || js_has_pending_coroutines()) {
    ANT_GC_COLLECT();
    return 0;
  }
  
  size_t old_brk = js->brk;
  size_t old_size = js->size;
  size_t new_size = old_size;
  
  uint8_t *new_mem = (uint8_t *)ANT_GC_MALLOC(new_size);
  if (!new_mem) return 0;
  memset(new_mem, 0, new_size);
  
  size_t bitmap_size = (js->brk / 4 + 7) / 8 + 1;
  uint8_t *mark_bits = (uint8_t *)calloc(1, bitmap_size);
  if (!mark_bits) {
    ANT_GC_FREE(new_mem);
    return 0;
  }
  
  size_t estimated_objs = js->brk / 64;
  if (estimated_objs < 256) estimated_objs = 256;
  
  gc_ctx_t ctx;
  ctx.js = js;
  ctx.new_mem = new_mem;
  ctx.new_brk = 0;
  ctx.new_size = (jsoff_t)new_size;
  ctx.mark_bits = mark_bits;
  
  if (!fwd_init(&ctx.fwd, estimated_objs)) {
    ANT_GC_FREE(new_mem);
    free(mark_bits);
    return 0;
  }
  
  if (!work_init(&ctx.work, estimated_objs / 4 < 64 ? 64 : estimated_objs / 4)) {
    fwd_free(&ctx.fwd);
    ANT_GC_FREE(new_mem);
    free(mark_bits);
    return 0;
  }
  
  ctx.failed = false;
    
  if (js->brk > 0) {
    jsoff_t header_at_0 = gc_loadoff(js->mem, 0);
    if ((header_at_0 & 3) == T_OBJ) gc_reserve_object(&ctx, 0);
  }
  
  jsoff_t scope_off = (jsoff_t)gc_vdata(js->scope);
  if (scope_off < js->brk) {
    (void)gc_reserve_object(&ctx, scope_off);
  }
  
  (void)gc_update_val(&ctx, js->this_val);
  (void)gc_update_val(&ctx, js->module_ns);
  (void)gc_update_val(&ctx, js->current_func);
  (void)gc_update_val(&ctx, js->thrown_value);
  (void)gc_update_val(&ctx, js->tval);
  js_gc_reserve_roots(js, gc_fwd_off_callback, gc_fwd_val_callback, &ctx);
  
  gc_drain_work_queue(&ctx);
  
  if (ctx.failed) {
    free(mark_bits);
    work_free(&ctx.work);
    fwd_free(&ctx.fwd);
    ANT_GC_FREE(new_mem);
    return 0;
  }
    
  js->scope = gc_apply_val(&ctx, js->scope);
  js->this_val = gc_apply_val(&ctx, js->this_val);
  js->module_ns = gc_apply_val(&ctx, js->module_ns);
  js->current_func = gc_apply_val(&ctx, js->current_func);
  js->thrown_value = gc_apply_val(&ctx, js->thrown_value);
  js->tval = gc_apply_val(&ctx, js->tval);
  js_gc_update_roots(js, gc_apply_off_callback, gc_apply_val_callback, &ctx);
  
  uint8_t *old_mem = js->mem;
  js->mem = new_mem;
  js->brk = ctx.new_brk;
  
  ANT_GC_FREE(old_mem);
  
  size_t used = ctx.new_brk;
  size_t shrunk_size = used * 2;
  if (shrunk_size < GC_MIN_HEAP_SIZE) shrunk_size = GC_MIN_HEAP_SIZE;
  shrunk_size = (shrunk_size + 7) & ~7;
  
  if (old_size >= GC_SHRINK_THRESHOLD * shrunk_size && shrunk_size < old_size) {
    uint8_t *shrunk_mem = (uint8_t *)ANT_GC_MALLOC(shrunk_size);
    if (shrunk_mem) {
      memcpy(shrunk_mem, js->mem, used);
      memset(shrunk_mem + used, 0, shrunk_size - used);
      ANT_GC_FREE(js->mem);
      js->mem = shrunk_mem;
      js->size = (jsoff_t)shrunk_size;
    }
  }
  
  free(mark_bits);
  work_free(&ctx.work);
  fwd_free(&ctx.fwd);
  ANT_GC_COLLECT();
  
  return (old_brk > ctx.new_brk ? old_brk - ctx.new_brk : 0);
}
