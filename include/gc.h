#ifndef GC_H
#define GC_H

#include <types.h>
#include <stddef.h>

#define ROPE_FLAG              (1ULL << 63)
#define ROPE_DEPTH_SHIFT       56
#define ROPE_DEPTH_MASK        0x7FULL
#define ROPE_MAX_DEPTH         64
#define ROPE_FLATTEN_THRESHOLD (32 * 1024)

typedef struct {
  jsoff_t header;
  jsval_t left;
  jsval_t right;
  jsval_t cached;
} rope_node_t;

#define GC_FWD_LOAD_FACTOR    70
#define GC_ROOTS_INITIAL_CAP  32
#define GC_CB(ret, name, arg) ret (*name)(void *ctx, arg old)

size_t js_gc_compact(ant_t *js);

#define GC_CTX       void *ctx
#define GC_FWD_OFF   GC_CB(jsoff_t, fwd_off, jsoff_t)
#define GC_FWD_VAL   GC_CB(jsval_t, fwd_val, jsval_t)
#define GC_WEAK_OFF  GC_CB(jsoff_t, weak_off, jsoff_t)
#define GC_OP_VAL    void (*op_val)(void *ctx, jsval_t *val)

#define GC_FWD_ARGS     GC_FWD_VAL, GC_CTX
#define GC_RESERVE_ARGS ant_t *js, GC_FWD_OFF, GC_FWD_ARGS
#define GC_UPDATE_ARGS  ant_t *js, GC_FWD_OFF, GC_WEAK_OFF, GC_FWD_ARGS
#define GC_OP_VAL_ARGS  GC_OP_VAL, GC_CTX

void js_gc_maybe(ant_t *js);
void js_gc_throttle(bool enabled);

#define js_gc_safepoint(js) do { \
  (js)->gc_safe = true;          \
  js_gc_maybe(js);               \
  (js)->gc_safe = false;          \
} while (0)

#endif