#ifndef GC_H
#define GC_H

#include <types.h>
#include <stddef.h>

#define ROPE_FLAG              (1ULL << 63)
#define ROPE_DEPTH_SHIFT       56
#define ROPE_DEPTH_MASK        0x7FULL
#define ROPE_MAX_DEPTH         64
#define ROPE_FLATTEN_THRESHOLD (32 * 1024)

// Generational GC: Nursery configuration
#define NURSERY_SIZE_DEFAULT   (8 * 1024 * 1024)  // 8MB default nursery
#define NURSERY_SIZE_MIN       (1 * 1024 * 1024)  // 1MB minimum
#define NURSERY_SIZE_MAX       (64 * 1024 * 1024) // 64MB maximum

// Use high bit of offset to distinguish nursery vs old gen
// Nursery offsets have bit 47 set (within 48-bit address space)
#define NURSERY_BIT            (1ULL << 47)
#define NURSERY_OFF_MASK       (~NURSERY_BIT)

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
#define GC_OP_OFF    void (*op_off)(void *ctx, jsoff_t *off)

#define GC_FWD_ARGS        GC_FWD_VAL, GC_CTX
#define GC_RESERVE_ARGS    ant_t *js, GC_FWD_OFF, GC_FWD_ARGS
#define GC_UPDATE_ARGS     ant_t *js, GC_FWD_OFF, GC_WEAK_OFF, GC_FWD_ARGS
#define GC_OP_VAL_ARGS     GC_OP_VAL, GC_CTX
#define GC_OP_VALOFF_ARGS  GC_OP_VAL, GC_OP_OFF, GC_CTX

void js_gc_maybe(ant_t *js);
void js_gc_throttle(bool enabled);

// Generational GC: Nursery operations
bool nursery_init(ant_t *js, size_t size);
void nursery_free(ant_t *js);
void nursery_scavenge(ant_t *js);
void nursery_enable(ant_t *js);
jsoff_t nursery_alloc(ant_t *js, size_t size);

// Generational GC: Remembered set operations  
void rs_add(ant_t *js, jsval_t obj);
void rs_clear(ant_t *js);

// Nursery pointer detection
static inline bool is_nursery_off(jsoff_t off) {
  return (off & NURSERY_BIT) != 0;
}

static inline jsoff_t nursery_raw_off(jsoff_t off) {
  return off & NURSERY_OFF_MASK;
}

#endif