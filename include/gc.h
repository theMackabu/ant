#ifndef GC_H
#define GC_H

#include <types.h>
#include <stdbool.h>

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

#define GC_CTX       void *ctx
#define GC_FWD_OFF   GC_CB(jsoff_t, fwd_off, jsoff_t)
#define GC_FWD_VAL   GC_CB(jsval_t, fwd_val, jsval_t)
#define GC_WEAK_OFF  GC_CB(jsoff_t, weak_off, jsoff_t)
#define GC_OP_VAL    void (*op_val)(void *ctx, jsval_t *val)

#define GC_FWD_ARGS     GC_FWD_VAL, GC_CTX
#define GC_RESERVE_ARGS ant_t *js, GC_FWD_OFF, GC_FWD_ARGS
#define GC_UPDATE_ARGS  ant_t *js, GC_FWD_OFF, GC_WEAK_OFF, GC_FWD_ARGS
#define GC_OP_VAL_ARGS  GC_OP_VAL, GC_CTX

#define FWD_EMPTY     ((jsoff_t)~0)
#define FWD_TOMBSTONE ((jsoff_t)~1)

#define GC_BIGINT_HEADER_SHIFT 4
#define GC_SYM_HEADER_SHIFT    4

#define GC_BIGINT_HEADER_LOW_MASK ((jsoff_t)((1u << GC_BIGINT_HEADER_SHIFT) - 1u))
#define GC_SYM_HEADER_LOW_MASK    ((jsoff_t)((1u << GC_SYM_HEADER_SHIFT) - 1u))
#define GC_SYM_HEAP_FIXED         (sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uintptr_t))

#ifdef _WIN32
#define RELEASE_PAGES(p, sz) VirtualAlloc(p, sz, MEM_RESET, PAGE_READWRITE)
#elif defined(__APPLE__)
#define RELEASE_PAGES(p, sz) madvise(p, sz, MADV_FREE)
#else
#define RELEASE_PAGES(p, sz) madvise(p, sz, MADV_DONTNEED)
#endif

#define GC_HEAP_TYPE_MASK ( \
  (1u << T_OBJ) | (1u << T_PROP) | (1u << T_STR) | \
  (1u << T_ARR) | (1u << T_PROMISE) | (1u << T_BIGINT) | (1u << T_GENERATOR) | \
  (1u << T_SYMBOL))

extern uint32_t gc_epoch_counter;

void js_gc_maybe(ant_t *js);
void js_gc_throttle(bool enabled);

#endif