#ifndef ANT_FFI_H
#define ANT_FFI_H

#include "gc.h"
#include "types.h"

jsval_t ffi_library(ant_t *js);
jsval_t ffi_call_by_index(ant_t *js, unsigned int func_index, jsval_t *args, int nargs);

void ffi_gc_update_roots(GC_OP_VAL_ARGS);

#endif