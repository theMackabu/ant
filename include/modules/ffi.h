#ifndef ANT_FFI_H
#define ANT_FFI_H

#include "types.h"

ant_value_t ffi_library(ant_t *js);
ant_value_t ffi_call_by_index(ant_t *js, unsigned int func_index, ant_value_t *args, int nargs);

#endif