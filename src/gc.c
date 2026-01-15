#include "arena.h"
#include "internal.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MCO_API extern
#include "minicoro.h"

#define GC_MIN_HEAP_SIZE (64 * 1024)
#define GC_SHRINK_THRESHOLD 4

typedef struct gc_forward_node {
  jsoff_t old_off;
  jsoff_t new_off;
  struct gc_forward_node *next;
} gc_forward_node_t;

typedef struct {
  gc_forward_node_t *buckets[JS_HASH_SIZE];
} gc_forward_table_t;

typedef struct {
  struct js *js;
  uint8_t *new_mem;
  jsoff_t new_brk;
  jsoff_t new_size;
  uint8_t *mark_bits;
  gc_forward_table_t fwd;
} gc_ctx_t;

static jsoff_t gc_copy_entity(gc_ctx_t *ctx, jsoff_t old_off);
static jsval_t gc_update_val(gc_ctx_t *ctx, jsval_t val);

static inline void mark_set(gc_ctx_t *ctx, jsoff_t off) {
  jsoff_t idx = off >> 2;
  ctx->mark_bits[idx >> 3] |= (1 << (idx & 7));
}

static inline size_t fwd_hash(jsoff_t off) {
  return (off >> 2) & (JS_HASH_SIZE - 1);
}

static void fwd_init(gc_forward_table_t *fwd) {
  memset(fwd->buckets, 0, sizeof(fwd->buckets));
}

static void fwd_add(gc_forward_table_t *fwd, jsoff_t old_off, jsoff_t new_off) {
  size_t h = fwd_hash(old_off);
  gc_forward_node_t *node = ANT_GC_MALLOC(sizeof(gc_forward_node_t));
  node->old_off = old_off;
  node->new_off = new_off;
  node->next = fwd->buckets[h];
  fwd->buckets[h] = node;
}

static jsoff_t fwd_lookup(gc_forward_table_t *fwd, jsoff_t old_off) {
  size_t h = fwd_hash(old_off);
  for (gc_forward_node_t *n = fwd->buckets[h]; n; n = n->next) {
    if (n->old_off == old_off) return n->new_off;
  }
  return (jsoff_t)~0;
}

static void fwd_free(gc_forward_table_t *fwd) {
  for (size_t i = 0; i < JS_HASH_SIZE; i++) {
    gc_forward_node_t *n = fwd->buckets[i];
    while (n) {
      gc_forward_node_t *next = n->next;
      ANT_GC_FREE(n);
      n = next;
    }
    fwd->buckets[i] = NULL;
  }
}

static inline jsoff_t gc_loadoff(uint8_t *mem, jsoff_t off) {
  jsoff_t val;
  memcpy(&val, &mem[off], sizeof(val)); return val;
}

static inline jsval_t gc_loadval(uint8_t *mem, jsoff_t off) {
  jsval_t val;
  memcpy(&val, &mem[off], sizeof(val)); return val;
}

static inline void gc_saveoff(uint8_t *mem, jsoff_t off, jsoff_t val) {
  memcpy(&mem[off], &val, sizeof(val));
}

static inline void gc_saveval(uint8_t *mem, jsoff_t off, jsval_t val) {
  memcpy(&mem[off], &val, sizeof(val));
}

static jsoff_t gc_esize(jsoff_t w) {
  jsoff_t cleaned = w & ~FLAGMASK;
  switch (cleaned & 3U) {
    case JS_T_OBJ:  return (jsoff_t)(sizeof(jsoff_t) + sizeof(jsoff_t));
    case JS_T_PROP: return (jsoff_t)(sizeof(jsoff_t) + sizeof(jsoff_t) + sizeof(jsval_t));
    case JS_T_STR:  return (jsoff_t)(sizeof(jsoff_t) + ((cleaned >> 2) + 3) / 4 * 4);
    default:        return (jsoff_t)~0U;
  }
}

static jsoff_t gc_alloc(gc_ctx_t *ctx, size_t size) {
  size = (size + 3) / 4 * 4;
  if (ctx->new_brk + size > ctx->new_size) {
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
  if ((header & 3) != JS_T_STR) return old_off;
  
  jsoff_t size = gc_esize(header);
  if (size == (jsoff_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (jsoff_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  
  fwd_add(&ctx->fwd, old_off, new_off);
  mark_set(ctx, old_off);
  
  return new_off;
}

static jsoff_t gc_copy_prop(gc_ctx_t *ctx, jsoff_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (jsoff_t)~0) return new_off;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & 3) != JS_T_PROP) {
    return old_off;
  }
  
  jsoff_t size = gc_esize(header);
  if (size == (jsoff_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (jsoff_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  
  fwd_add(&ctx->fwd, old_off, new_off);
  mark_set(ctx, old_off);
  
  jsoff_t next_prop = header & ~(3U | FLAGMASK);
  if (next_prop != 0 && next_prop < ctx->js->brk) {
    jsoff_t new_next = gc_copy_prop(ctx, next_prop);
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
    
    jsval_t val = gc_loadval(ctx->js->mem, old_off + sizeof(jsoff_t) + sizeof(jsoff_t));
    jsval_t new_val = gc_update_val(ctx, val);
    gc_saveval(ctx->new_mem, new_off + sizeof(jsoff_t) + sizeof(jsoff_t), new_val);
  } else {
    jsval_t val = gc_loadval(ctx->js->mem, old_off + sizeof(jsoff_t) + sizeof(jsoff_t));
    jsval_t new_val = gc_update_val(ctx, val);
    gc_saveval(ctx->new_mem, new_off + sizeof(jsoff_t) + sizeof(jsoff_t), new_val);
  }
  
  return new_off;
}

static jsoff_t gc_copy_object(gc_ctx_t *ctx, jsoff_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
  if (new_off != (jsoff_t)~0) return new_off;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  if ((header & 3) != JS_T_OBJ) return old_off;
  
  jsoff_t size = gc_esize(header);
  if (size == (jsoff_t)~0) return old_off;
  
  new_off = gc_alloc(ctx, size);
  if (new_off == (jsoff_t)~0) return old_off;
  
  memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], size);
  
  fwd_add(&ctx->fwd, old_off, new_off);
  mark_set(ctx, old_off);
  
  jsoff_t first_prop = header & ~(3U | FLAGMASK);
  if (first_prop != 0 && first_prop < ctx->js->brk) {
    jsoff_t new_first = gc_copy_prop(ctx, first_prop);
    jsoff_t new_header = (new_first & ~3U) | (header & (3U | FLAGMASK));
    gc_saveoff(ctx->new_mem, new_off, new_header);
  }
  
  jsoff_t parent_off = gc_loadoff(ctx->js->mem, old_off + sizeof(jsoff_t));
  if (parent_off != 0 && parent_off < ctx->js->brk) {
    jsoff_t new_parent = gc_copy_object(ctx, parent_off);
    gc_saveoff(ctx->new_mem, new_off + sizeof(jsoff_t), new_parent);
  }
  
  return new_off;
}

static jsoff_t gc_copy_entity(gc_ctx_t *ctx, jsoff_t old_off) {
  if (old_off >= ctx->js->brk) return old_off;
  
  jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
  switch (header & 3) {
    case JS_T_OBJ:  return gc_copy_object(ctx, old_off);
    case JS_T_PROP: return gc_copy_prop(ctx, old_off);
    case JS_T_STR:  return gc_copy_string(ctx, old_off);
    default:        return old_off;
  }
}

static jsval_t gc_update_val(gc_ctx_t *ctx, jsval_t val) {
  if (!gc_is_tagged(val)) return val;
  
  uint8_t type = gc_vtype(val);
  jsoff_t old_off = (jsoff_t)gc_vdata(val);
  
  switch (type) {
    case JS_V_OBJ:
    case JS_V_FUNC:
    case JS_V_ARR:
    case JS_V_PROMISE:
    case JS_V_GENERATOR: {
      if (old_off >= ctx->js->brk) return val;
      jsoff_t new_off = gc_copy_object(ctx, old_off);
      if (new_off != (jsoff_t)~0) return gc_mkval(type, new_off);
      break;
    }
    case JS_V_STR: {
      if (old_off >= ctx->js->brk) return val;
      jsoff_t new_off = gc_copy_string(ctx, old_off);
      if (new_off != (jsoff_t)~0) {
        return gc_mkval(type, new_off);
      }
      break;
    }
    case JS_V_PROP: {
      if (old_off >= ctx->js->brk) return val;
      jsoff_t new_off = gc_copy_prop(ctx, old_off);
      if (new_off != (jsoff_t)~0) {
        return gc_mkval(type, new_off);
      }
      break;
    }
    case JS_V_BIGINT: {
      if (old_off >= ctx->js->brk) return val;
      jsoff_t new_off = fwd_lookup(&ctx->fwd, old_off);
      if (new_off != (jsoff_t)~0) {
        return gc_mkval(type, new_off);
      }
      jsoff_t header = gc_loadoff(ctx->js->mem, old_off);
      size_t total = (header >> 4) + sizeof(jsoff_t);
      total = (total + 3) / 4 * 4;
      new_off = gc_alloc(ctx, total);
      if (new_off != (jsoff_t)~0) {
        memcpy(&ctx->new_mem[new_off], &ctx->js->mem[old_off], total);
        fwd_add(&ctx->fwd, old_off, new_off);
        mark_set(ctx, old_off);
        return gc_mkval(type, new_off);
      }
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
  
  new_off = gc_copy_object(ctx, old_off);
  return (new_off != (jsoff_t)~0) ? new_off : old_off;
}

static jsval_t gc_fwd_val_callback(void *ctx_ptr, jsval_t val) {
  gc_ctx_t *ctx = (gc_ctx_t *)ctx_ptr;
  return gc_update_val(ctx, val);
}

size_t js_gc_compact(struct js *js) {
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
  if (!mark_bits) return 0;
  
  gc_ctx_t ctx;
  ctx.js = js;
  ctx.new_mem = new_mem;
  ctx.new_brk = 0;
  ctx.new_size = (jsoff_t)new_size;
  ctx.mark_bits = mark_bits;
  fwd_init(&ctx.fwd);
  
  if (js->brk > 0) {
    jsoff_t header_at_0 = gc_loadoff(js->mem, 0);
    if ((header_at_0 & 3) == JS_T_OBJ) gc_copy_object(&ctx, 0);
  }
  
  jsoff_t scope_off = (jsoff_t)gc_vdata(js->scope);
  if (scope_off < js->brk) {
    jsoff_t new_scope = gc_copy_object(&ctx, scope_off);
    js->scope = gc_mkval(JS_V_OBJ, new_scope);
  }
  
  js->this_val = gc_update_val(&ctx, js->this_val);
  js->module_ns = gc_update_val(&ctx, js->module_ns);
  js->current_func = gc_update_val(&ctx, js->current_func);
  js->thrown_value = gc_update_val(&ctx, js->thrown_value);
  js->tval = gc_update_val(&ctx, js->tval);
  js_gc_update_roots(js, gc_fwd_off_callback, gc_fwd_val_callback, &ctx);
  
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
  
  jsoff_t free_space = js->size - js->brk;
  if (free_space < js->lwm || js->lwm == 0) {
    js->lwm = free_space;
  }
  
  free(mark_bits);
  fwd_free(&ctx.fwd);
  ANT_GC_COLLECT();
  
  return (old_brk > ctx.new_brk ? old_brk - ctx.new_brk : 0);
}
