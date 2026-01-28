#ifndef GC_H
#define GC_H

#include <types.h>

#define GC_FWD_LOAD_FACTOR 70
#define GC_ROOTS_INITIAL_CAP 32

#define GC_FWD_ARGS jsval_t (*fwd_val)(void *ctx, jsval_t old), void *ctx
#define GC_UPDATE_ARGS ant_t *js, jsoff_t (*fwd_off)(void *ctx, jsoff_t old), GC_FWD_ARGS
#define GC_OP_VAL_ARGS void (*op_val)(void *ctx, jsval_t *val), void *ctx

#endif