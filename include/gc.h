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

#define GC_FWD_LOAD_FACTOR 70
#define GC_ROOTS_INITIAL_CAP 32

#define GC_FWD_ARGS jsval_t (*fwd_val)(void *ctx, jsval_t old), void *ctx
#define GC_UPDATE_ARGS ant_t *js, jsoff_t (*fwd_off)(void *ctx, jsoff_t old), GC_FWD_ARGS
#define GC_OP_VAL_ARGS void (*op_val)(void *ctx, jsval_t *val), void *ctx

void js_maybe_gc(ant_t *js);
size_t js_gc_compact(ant_t *js);

#endif