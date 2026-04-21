#ifndef ANT_FFI_H
#define ANT_FFI_H

#include "types.h"

ant_value_t ffi_library(ant_t *js);

ant_value_t ffi_library_close(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_library_define(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_library_call(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_pointer_address(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_pointer_is_null(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_pointer_read(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_pointer_write(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_pointer_offset(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_pointer_free(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_callback_address(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_callback_close(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_function_address(ant_t *js, ant_value_t *args, int nargs);
ant_value_t ffi_function_call(ant_t *js, ant_value_t *args, int nargs);

void ffi_library_finalize(ant_t *js, ant_object_t *obj);
void ffi_function_finalize(ant_t *js, ant_object_t *obj);
void ffi_pointer_finalize(ant_t *js, ant_object_t *obj);
void ffi_callback_finalize(ant_t *js, ant_object_t *obj);

#endif
