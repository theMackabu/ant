#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define dlopen(name, flags) ((void *)LoadLibraryA(name))
#define dlsym(handle, name) ((void *)GetProcAddress((HMODULE)(handle), (name)))
#define dlclose(handle) FreeLibrary((HMODULE)(handle))
#define dlerror() "LoadLibrary failed"
#define RTLD_LAZY 0
#else
#include <dlfcn.h>
#endif

#include <ffi.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "silver/engine.h"

#include "modules/buffer.h"
#include "modules/ffi.h"
#include "modules/symbol.h"

enum {
  FFI_LIBRARY_NATIVE_TAG  = 0x4646494cu, // FFIL
  FFI_FUNCTION_NATIVE_TAG = 0x46464946u, // FFIF
  FFI_POINTER_NATIVE_TAG  = 0x46464950u, // FFIP
  FFI_CALLBACK_NATIVE_TAG = 0x46464943u, // FFIC
};

typedef enum {
  FFI_VALUE_VOID = 0,
  FFI_VALUE_INT8,
  FFI_VALUE_INT16,
  FFI_VALUE_INT,
  FFI_VALUE_INT64,
  FFI_VALUE_UINT8,
  FFI_VALUE_UINT16,
  FFI_VALUE_UINT64,
  FFI_VALUE_FLOAT,
  FFI_VALUE_DOUBLE,
  FFI_VALUE_POINTER,
  FFI_VALUE_STRING,
  FFI_VALUE_SPREAD,
  FFI_VALUE_UNKNOWN,
} ffi_value_type_id_t;

typedef struct {
  ffi_value_type_id_t id;
  ffi_type *ffi_type;
  const char *name;
} ffi_marshaled_type_t;

typedef struct {
  ffi_marshaled_type_t returns;
  ffi_marshaled_type_t *args;
  ffi_type **ffi_arg_types;
  size_t arg_count;
  size_t fixed_arg_count;
  bool variadic;
} ffi_signature_t;

typedef struct {
  ant_value_t obj;
  void *handle;
  char *path;
  bool closed;
} ffi_library_handle_t;

typedef struct {
  ffi_library_handle_t *library;
  ffi_signature_t signature;
  ffi_cif cif;
  void *func_ptr;
  char *symbol_name;
} ffi_function_handle_t;

typedef struct ffi_pointer_region_s {
  uint8_t *ptr;
  size_t size;
  size_t ref_count;
  bool size_known;
  bool owned;
  bool freed;
} ffi_pointer_region_t;

typedef struct {
  ffi_pointer_region_t *region;
  size_t byte_offset;
} ffi_pointer_handle_t;

typedef struct {
  ant_t *js;
  ant_value_t owner_obj;
  ffi_signature_t signature;
  ffi_cif cif;
  ffi_closure *closure;
  void *code_ptr;
  pthread_t owner_thread;
  bool closed;
} ffi_callback_handle_t;

typedef union {
  int8_t i8;
  int16_t i16;
  int32_t i32;
  int64_t i64;
  uint8_t u8;
  uint16_t u16;
  uint32_t u32;
  uint64_t u64;
  float f32;
  double f64;
  void *ptr;
  ffi_arg raw;
} ffi_value_box_t;

static ant_value_t g_ffi_library_proto  = 0;
static ant_value_t g_ffi_function_proto = 0;
static ant_value_t g_ffi_pointer_proto  = 0;
static ant_value_t g_ffi_callback_proto = 0;

static inline bool ffi_is_nullish(ant_value_t value) {
  return is_null(value) || is_undefined(value);
}

static ffi_marshaled_type_t ffi_marshaled_type_unknown(void) {
  ffi_marshaled_type_t type = {0};
  type.id = FFI_VALUE_UNKNOWN;
  type.ffi_type = NULL;
  type.name = NULL;
  return type;
}

static ffi_marshaled_type_t ffi_marshaled_type_make(ffi_value_type_id_t id, const char *name) {
  ffi_marshaled_type_t type = ffi_marshaled_type_unknown();
  type.id = id;
  type.name = name;

  switch (id) {
    case FFI_VALUE_VOID:    type.ffi_type = &ffi_type_void; break;
    case FFI_VALUE_INT8:    type.ffi_type = &ffi_type_sint8; break;
    case FFI_VALUE_INT16:   type.ffi_type = &ffi_type_sint16; break;
    case FFI_VALUE_INT:     type.ffi_type = &ffi_type_sint32; break;
    case FFI_VALUE_INT64:   type.ffi_type = &ffi_type_sint64; break;
    case FFI_VALUE_UINT8:   type.ffi_type = &ffi_type_uint8; break;
    case FFI_VALUE_UINT16:  type.ffi_type = &ffi_type_uint16; break;
    case FFI_VALUE_UINT64:  type.ffi_type = &ffi_type_uint64; break;
    case FFI_VALUE_FLOAT:   type.ffi_type = &ffi_type_float; break;
    case FFI_VALUE_DOUBLE:  type.ffi_type = &ffi_type_double; break;
    case FFI_VALUE_POINTER: type.ffi_type = &ffi_type_pointer; break;
    case FFI_VALUE_STRING:  type.ffi_type = &ffi_type_pointer; break;
    case FFI_VALUE_SPREAD:
    case FFI_VALUE_UNKNOWN:
      type.ffi_type = NULL;
      break;
  }

  return type;
}

static ffi_value_type_id_t ffi_type_id_from_name(const char *name) {
  if (!name) return FFI_VALUE_UNKNOWN;
  if (strcmp(name, "void") == 0)    return FFI_VALUE_VOID;
  if (strcmp(name, "int8") == 0)    return FFI_VALUE_INT8;
  if (strcmp(name, "int16") == 0)   return FFI_VALUE_INT16;
  if (strcmp(name, "int") == 0)     return FFI_VALUE_INT;
  if (strcmp(name, "int64") == 0)   return FFI_VALUE_INT64;
  if (strcmp(name, "uint8") == 0)   return FFI_VALUE_UINT8;
  if (strcmp(name, "uint16") == 0)  return FFI_VALUE_UINT16;
  if (strcmp(name, "uint64") == 0)  return FFI_VALUE_UINT64;
  if (strcmp(name, "float") == 0)   return FFI_VALUE_FLOAT;
  if (strcmp(name, "double") == 0)  return FFI_VALUE_DOUBLE;
  if (strcmp(name, "pointer") == 0) return FFI_VALUE_POINTER;
  if (strcmp(name, "string") == 0)  return FFI_VALUE_STRING;
  if (strcmp(name, "...") == 0)     return FFI_VALUE_SPREAD;
  return FFI_VALUE_UNKNOWN;
}

static ffi_marshaled_type_t ffi_marshaled_type_from_value(ant_t *js, ant_value_t value) {
  if (vtype(value) != T_STR) return ffi_marshaled_type_unknown();
  return ffi_marshaled_type_make(
    ffi_type_id_from_name(js_getstr(js, value, NULL)), 
    js_getstr(js, value, NULL)
  );
}

static void ffi_signature_cleanup(ffi_signature_t *signature) {
  if (!signature) return;
  free(signature->args);
  free(signature->ffi_arg_types);
  signature->args = NULL;
  signature->ffi_arg_types = NULL;
  signature->arg_count = 0;
  signature->fixed_arg_count = 0;
  signature->variadic = false;
  signature->returns = ffi_marshaled_type_unknown();
}

static bool ffi_parse_signature(
  ant_t *js,
  ant_value_t value,
  bool allow_variadic,
  bool allow_string_return,
  ffi_signature_t *out,
  ant_value_t *error_out
) {
  ant_value_t returns_val = js_mkundef();
  ant_value_t args_val = js_mkundef();
  size_t arg_count = 0;

  memset(out, 0, sizeof(*out));
  out->returns = ffi_marshaled_type_unknown();
  if (error_out) *error_out = js_mkundef();

  if (!is_object_type(value)) {
    if (error_out) *error_out = js_mkerr_typed(
      js, JS_ERR_TYPE,
      "FFI signature must be [returnType, argTypes] or { returns, args }"
    );
    return false;
  }

  returns_val = js_get(js, value, "returns");
  args_val = js_get(js, value, "args");

  if (vtype(returns_val) == T_UNDEF || vtype(args_val) == T_UNDEF) {
    returns_val = js_get(js, value, "0");
    args_val = js_get(js, value, "1");
  }

  if (vtype(returns_val) != T_STR) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "FFI return type must be a string");
    return false;
  }

  if (!is_object_type(args_val)) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "FFI argument list must be an array-like object");
    return false;
  }

  out->returns = ffi_marshaled_type_from_value(js, returns_val);
  if (out->returns.id == FFI_VALUE_UNKNOWN || out->returns.id == FFI_VALUE_SPREAD) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported FFI return type");
    return false;
  }

  if (!allow_string_return && out->returns.id == FFI_VALUE_STRING) {
    if (error_out) *error_out = js_mkerr_typed(
      js, JS_ERR_TYPE,
      "FFICallback does not support string return values"
    );
    return false;
  }

  ant_value_t len_val = js_get(js, args_val, "length");
  arg_count = vtype(len_val) == T_NUM ? (size_t)js_getnum(len_val) : 0;

  if (arg_count > 0) {
    out->args = calloc(arg_count, sizeof(*out->args));
    out->ffi_arg_types = calloc(arg_count, sizeof(*out->ffi_arg_types));
    if (!out->args || !out->ffi_arg_types) {
      free(out->args);
      free(out->ffi_arg_types);
      out->args = NULL;
      out->ffi_arg_types = NULL;
      if (error_out) *error_out = js_mkerr(js, "Out of memory");
      return false;
    }
  }

  out->arg_count = arg_count;
  out->fixed_arg_count = arg_count;
  out->variadic = false;

  for (size_t i = 0; i < arg_count; i++) {
    char idx[32];
    ant_value_t arg_val;
    ffi_marshaled_type_t arg_type;
    
    snprintf(idx, sizeof(idx), "%zu", i);
    arg_val = js_get(js, args_val, idx);
    arg_type = ffi_marshaled_type_from_value(js, arg_val);
    
    if (arg_type.id == FFI_VALUE_UNKNOWN) {
      ffi_signature_cleanup(out);
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported FFI argument type");
      return false;
    }
    
    if (arg_type.id == FFI_VALUE_SPREAD) {
      if (!allow_variadic) {
        ffi_signature_cleanup(out);
        if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Variadic signatures are not supported here");
        return false;
      }
      
      if (i + 1 != arg_count) {
        ffi_signature_cleanup(out);
        if (error_out) *error_out = js_mkerr_typed(
          js, JS_ERR_TYPE,
          "FFI spread marker must be the last argument type"
        );
        return false;
      }
      
      out->variadic = true;
      out->fixed_arg_count = i;
      out->arg_count = i;
      break;
    }
    
    out->args[i] = arg_type;
    out->ffi_arg_types[i] = arg_type.ffi_type;
  }

  return true;
}

static ffi_marshaled_type_t ffi_infer_variadic_type(ant_value_t value) {
  if (vtype(value) == T_STR) return ffi_marshaled_type_make(FFI_VALUE_STRING, "string");
  if (ffi_is_nullish(value)) return ffi_marshaled_type_make(FFI_VALUE_POINTER, "pointer");
  
  if (is_object_type(value) && js_check_native_tag(value, FFI_POINTER_NATIVE_TAG))
    return ffi_marshaled_type_make(FFI_VALUE_POINTER, "pointer");
    
  if (is_object_type(value) && js_check_native_tag(value, FFI_CALLBACK_NATIVE_TAG))
    return ffi_marshaled_type_make(FFI_VALUE_POINTER, "pointer");
    
  if (vtype(value) == T_NUM) {
    double number = js_getnum(value);
    double truncated = js_to_int32(number);
    if (number == truncated) return ffi_marshaled_type_make(FFI_VALUE_INT, "int");
    return ffi_marshaled_type_make(FFI_VALUE_DOUBLE, "double");
  }
  
  if (vtype(value) == T_BOOL) return ffi_marshaled_type_make(FFI_VALUE_INT, "int");
  return ffi_marshaled_type_make(FFI_VALUE_POINTER, "pointer");
}

static ffi_library_handle_t *ffi_library_data(ant_value_t value) {
  if (!js_check_native_tag(value, FFI_LIBRARY_NATIVE_TAG)) return NULL;
  return (ffi_library_handle_t *)js_get_native_ptr(value);
}

static ffi_function_handle_t *ffi_function_data(ant_value_t value) {
  if (!js_check_native_tag(value, FFI_FUNCTION_NATIVE_TAG)) return NULL;
  return (ffi_function_handle_t *)js_get_native_ptr(value);
}

static ffi_pointer_handle_t *ffi_pointer_data(ant_value_t value) {
  if (!js_check_native_tag(value, FFI_POINTER_NATIVE_TAG)) return NULL;
  return (ffi_pointer_handle_t *)js_get_native_ptr(value);
}

static ffi_callback_handle_t *ffi_callback_data(ant_value_t value) {
  if (!js_check_native_tag(value, FFI_CALLBACK_NATIVE_TAG)) return NULL;
  return (ffi_callback_handle_t *)js_get_native_ptr(value);
}

static void ffi_library_close_handle(ffi_library_handle_t *library) {
  if (!library || library->closed) return;
  library->closed = true;
  if (library->handle) dlclose(library->handle);
  library->handle = NULL;
}

static void ffi_pointer_region_release(ffi_pointer_region_t *region) {
  if (!region) return;
  if (region->ref_count > 0) region->ref_count--;
  if (region->ref_count != 0) return;
  if (region->owned && !region->freed && region->ptr) free(region->ptr);
  free(region);
}

static bool ffi_pointer_region_free(ffi_pointer_region_t *region) {
  if (!region || !region->owned || region->freed) return false;
  if (region->ptr) free(region->ptr);
  region->ptr = NULL;
  region->freed = true;
  region->size = 0;
  region->size_known = true;
  return true;
}

static void ffi_pointer_close_handle(ffi_pointer_handle_t *handle, bool free_region_memory) {
  if (!handle) return;
  if (handle->region && free_region_memory) (void)ffi_pointer_region_free(handle->region);
  ffi_pointer_region_release(handle->region);
  handle->region = NULL;
}

static void ffi_callback_close_handle(ffi_callback_handle_t *callback) {
  if (!callback || callback->closed) return;
  callback->closed = true;
  if (callback->closure) ffi_closure_free(callback->closure);
  callback->closure = NULL;
  callback->code_ptr = NULL;
  ffi_signature_cleanup(&callback->signature);
}

static uint8_t *ffi_pointer_address_raw(ffi_pointer_handle_t *handle) {
  if (!handle || !handle->region || handle->region->freed || !handle->region->ptr) return NULL;
  return handle->region->ptr + handle->byte_offset;
}

static bool ffi_pointer_ensure_readable(
  ant_t *js,
  ffi_pointer_handle_t *handle,
  size_t size,
  const char *op,
  ant_value_t *error_out
) {
  size_t remaining = 0;

  if (error_out) *error_out = js_mkundef();
  if (!handle || !handle->region) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid FFIPointer");
    return false;
  }

  if (handle->region->freed) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "FFIPointer has been freed");
    return false;
  }

  if (!handle->region->ptr) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot %s through a null pointer", op);
    return false;
  }

  if (handle->region->size_known) {
    if (handle->byte_offset > handle->region->size) {
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_RANGE, "Pointer offset is out of bounds");
      return false;
    }
    
    remaining = handle->region->size - handle->byte_offset;
    if (size > remaining) {
      if (error_out) *error_out = js_mkerr_typed(
        js, JS_ERR_RANGE,
        "FFIPointer %s would read past the tracked allocation",
        op
      );
      return false;
    }
  }

  return true;
}

static ant_value_t ffi_make_pointer(ant_t *js, ffi_pointer_region_t *region, size_t byte_offset) {
  ant_value_t obj = js_mkobj(js);
  ffi_pointer_handle_t *handle = calloc(1, sizeof(*handle));

  if (!handle) {
    ffi_pointer_region_release(region);
    return js_mkerr(js, "Out of memory");
  }

  handle->region = region;
  handle->byte_offset = byte_offset;
  if (region) region->ref_count++;

  if (g_ffi_pointer_proto) js_set_proto_init(obj, g_ffi_pointer_proto);
  js_set_native_ptr(obj, handle);
  js_set_native_tag(obj, FFI_POINTER_NATIVE_TAG);
  js_set_finalizer(obj, ffi_pointer_finalize);

  return obj;
}

static ant_value_t ffi_make_pointer_from_raw(ant_t *js, void *ptr) {
  ffi_pointer_region_t *region = calloc(1, sizeof(*region));
  if (!region) return js_mkerr(js, "Out of memory");
  region->ptr = (uint8_t *)ptr;
  region->owned = false;
  region->freed = false;
  region->size_known = false;
  return ffi_make_pointer(js, region, 0);
}

static ant_value_t ffi_make_pointer_or_null(ant_t *js, void *ptr) {
  if (!ptr) return js_mknull();
  return ffi_make_pointer_from_raw(js, ptr);
}

static bool ffi_pointer_from_js(
  ant_t *js,
  ant_value_t value,
  void **out,
  ant_value_t *error_out
) {
  ffi_pointer_handle_t *ptr_handle = NULL;
  ffi_callback_handle_t *cb_handle = NULL;

  if (error_out) *error_out = js_mkundef();
  if (out) *out = NULL;

  if (ffi_is_nullish(value)) return true;

  ptr_handle = ffi_pointer_data(value);
  if (ptr_handle) {
    if (ptr_handle->region && ptr_handle->region->freed) {
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "FFIPointer has been freed");
      return false;
    }
    if (out) *out = (void *)ffi_pointer_address_raw(ptr_handle);
    return true;
  }

  cb_handle = ffi_callback_data(value);
  if (cb_handle) {
    if (cb_handle->closed) {
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "FFICallback has been closed");
      return false;
    }
    if (out) *out = cb_handle->code_ptr;
    return true;
  }

  if (error_out) *error_out = js_mkerr_typed(
    js, JS_ERR_TYPE,
    "Pointer arguments require FFIPointer, FFICallback, null, or undefined"
  );
  
  return false;
}

static bool ffi_copy_js_string(
  ant_t *js,
  ant_value_t value,
  char **out,
  size_t *len_out,
  ant_value_t *error_out
) {
  ant_value_t str_val = js_tostring_val(js, value);
  const char *src = NULL;
  size_t len = 0;
  char *copy = NULL;

  if (error_out) *error_out = js_mkundef();
  if (out) *out = NULL;
  if (len_out) *len_out = 0;

  if (is_err(str_val)) {
    if (error_out) *error_out = str_val;
    return false;
  }

  src = js_getstr(js, str_val, &len);
  if (!src) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid string value");
    return false;
  }

  copy = malloc(len + 1);
  if (!copy) {
    if (error_out) *error_out = js_mkerr(js, "Out of memory");
    return false;
  }

  memcpy(copy, src, len);
  copy[len] = '\0';

  if (out) *out = copy;
  if (len_out) *len_out = len;
  
  return true;
}

static bool ffi_value_to_c(
  ant_t *js,
  ant_value_t value,
  ffi_marshaled_type_t type,
  ffi_value_box_t *box,
  void **scratch_alloc,
  ant_value_t *error_out
) {
  void *ptr = NULL;
  char *str_copy = NULL;

  if (error_out) *error_out = js_mkundef();
  if (scratch_alloc) *scratch_alloc = NULL;

  switch (type.id) {
    case FFI_VALUE_INT8:   box->i8 = (int8_t)js_getnum(value); return true;
    case FFI_VALUE_INT16:  box->i16 = (int16_t)js_getnum(value); return true;
    case FFI_VALUE_INT:    box->i32 = (int32_t)js_getnum(value); return true;
    case FFI_VALUE_INT64:  box->i64 = (int64_t)js_getnum(value); return true;
    case FFI_VALUE_UINT8:  box->u8 = (uint8_t)js_getnum(value); return true;
    case FFI_VALUE_UINT16: box->u16 = (uint16_t)js_getnum(value); return true;
    case FFI_VALUE_UINT64: box->u64 = (uint64_t)js_getnum(value); return true;
    case FFI_VALUE_FLOAT:  box->f32 = (float)js_getnum(value); return true;
    case FFI_VALUE_DOUBLE: box->f64 = js_getnum(value); return true;
    case FFI_VALUE_POINTER:
      if (!ffi_pointer_from_js(js, value, &ptr, error_out)) return false;
      box->ptr = ptr;
      return true;
    case FFI_VALUE_STRING:
      if (!ffi_is_nullish(value) && ffi_pointer_from_js(js, value, &ptr, NULL)) {
        box->ptr = ptr;
        return true;
      }
      if (!ffi_copy_js_string(js, value, &str_copy, NULL, error_out)) return false;
      if (scratch_alloc) *scratch_alloc = str_copy;
      box->ptr = str_copy;
      return true;
    case FFI_VALUE_VOID:
    case FFI_VALUE_SPREAD:
    case FFI_VALUE_UNKNOWN:
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported FFI argument conversion");
      return false;
  }

  return false;
}

static ant_value_t ffi_value_from_c(ant_t *js, const void *value, ffi_marshaled_type_t type) {
  switch (type.id) {
    case FFI_VALUE_VOID:    return js_mkundef();
    case FFI_VALUE_INT8:    return js_mknum((double)*(const int8_t *)value);
    case FFI_VALUE_INT16:   return js_mknum((double)*(const int16_t *)value);
    case FFI_VALUE_INT:     return js_mknum((double)*(const int32_t *)value);
    case FFI_VALUE_INT64:   return js_mknum((double)*(const int64_t *)value);
    case FFI_VALUE_UINT8:   return js_mknum((double)*(const uint8_t *)value);
    case FFI_VALUE_UINT16:  return js_mknum((double)*(const uint16_t *)value);
    case FFI_VALUE_UINT64:  return js_mknum((double)*(const uint64_t *)value);
    case FFI_VALUE_FLOAT:   return js_mknum((double)*(const float *)value);
    case FFI_VALUE_DOUBLE:  return js_mknum(*(const double *)value);
    case FFI_VALUE_POINTER: return ffi_make_pointer_or_null(js, *(void *const *)value);
    case FFI_VALUE_STRING: {
      const char *str = *(const char *const *)value;
      return str ? js_mkstr(js, str, strlen(str)) : js_mknull();
    }
    case FFI_VALUE_SPREAD:
    case FFI_VALUE_UNKNOWN:
      return js_mkundef();
  }

  return js_mkundef();
}

static size_t ffi_marshaled_type_size(ffi_marshaled_type_t type) {
  switch (type.id) {
    case FFI_VALUE_STRING: return 1;
    case FFI_VALUE_VOID:
    case FFI_VALUE_SPREAD:
    case FFI_VALUE_UNKNOWN:
      return 0;
    default:
      return type.ffi_type ? type.ffi_type->size : 0;
  }
}

static void ffi_zero_return_value(void *ret, ffi_marshaled_type_t type) {
  size_t size = ffi_marshaled_type_size(type);
  if (size > 0) memset(ret, 0, size);
}

static ant_value_t ffi_read_from_pointer(ant_t *js, ffi_pointer_handle_t *handle, ffi_marshaled_type_t type) {
  ant_value_t err = js_mkundef();
  uint8_t *addr = ffi_pointer_address_raw(handle);

  if (type.id == FFI_VALUE_STRING) {
    size_t len = 0;
    if (!ffi_pointer_ensure_readable(js, handle, 1, "read", &err)) return err;
    if (handle->region && handle->region->size_known) {
      size_t remaining = handle->region->size - handle->byte_offset;
      const char *nul = memchr(addr, '\0', remaining);
      if (!nul) return js_mkerr_typed(js, JS_ERR_RANGE, "String read exceeded the tracked allocation");
      len = (size_t)(nul - (const char *)addr);
    } else len = strlen((const char *)addr);
    return js_mkstr(js, addr, len);
  }

  if (type.id == FFI_VALUE_POINTER) {
    if (!ffi_pointer_ensure_readable(js, handle, sizeof(void *), "read", &err)) return err;
    return ffi_make_pointer_or_null(js, *(void **)addr);
  }

  if (!ffi_pointer_ensure_readable(js, handle, ffi_marshaled_type_size(type), "read", &err)) return err;
  return ffi_value_from_c(js, addr, type);
}

static ant_value_t ffi_write_to_pointer(
  ant_t *js,
  ffi_pointer_handle_t *handle,
  ffi_marshaled_type_t type,
  ant_value_t value
) {
  ant_value_t err = js_mkundef();
  uint8_t *addr = ffi_pointer_address_raw(handle);
  ffi_value_box_t box;
  void *scratch = NULL;

  memset(&box, 0, sizeof(box));

  if (type.id == FFI_VALUE_STRING) {
    char *copy = NULL;
    size_t len = 0;
    if (!ffi_copy_js_string(js, value, &copy, &len, &err)) return err;
    if (!ffi_pointer_ensure_readable(js, handle, len + 1, "write", &err)) {
      free(copy);
      return err;
    }
    memcpy(addr, copy, len + 1);
    free(copy);
    return js_getthis(js);
  }

  if (!ffi_pointer_ensure_readable(js, handle, ffi_marshaled_type_size(type), "write", &err)) return err;
  if (!ffi_value_to_c(js, value, type, &box, &scratch, &err)) return err;

  switch (type.id) {
    case FFI_VALUE_INT8:   memcpy(addr, &box.i8, sizeof(box.i8)); break;
    case FFI_VALUE_INT16:  memcpy(addr, &box.i16, sizeof(box.i16)); break;
    case FFI_VALUE_INT:    memcpy(addr, &box.i32, sizeof(box.i32)); break;
    case FFI_VALUE_INT64:  memcpy(addr, &box.i64, sizeof(box.i64)); break;
    case FFI_VALUE_UINT8:  memcpy(addr, &box.u8, sizeof(box.u8)); break;
    case FFI_VALUE_UINT16: memcpy(addr, &box.u16, sizeof(box.u16)); break;
    case FFI_VALUE_UINT64: memcpy(addr, &box.u64, sizeof(box.u64)); break;
    case FFI_VALUE_FLOAT:  memcpy(addr, &box.f32, sizeof(box.f32)); break;
    case FFI_VALUE_DOUBLE: memcpy(addr, &box.f64, sizeof(box.f64)); break;
    case FFI_VALUE_POINTER: memcpy(addr, &box.ptr, sizeof(box.ptr)); break;
    default: break;
  }

  free(scratch);
  return js_getthis(js);
}

static ant_value_t ffi_make_function(ant_t *js, ffi_library_handle_t *library, const char *symbol_name, ffi_signature_t *signature, void *func_ptr) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t fn = 0;
  ffi_function_handle_t *handle = calloc(1, sizeof(*handle));

  if (!handle) {
    ffi_signature_cleanup(signature);
    return js_mkerr(js, "Out of memory");
  }

  handle->library = library;
  handle->signature = *signature;
  handle->func_ptr = func_ptr;
  handle->symbol_name = strdup(symbol_name);
  if (!handle->symbol_name) {
    free(handle);
    ffi_signature_cleanup(signature);
    return js_mkerr(js, "Out of memory");
  }

  if (!handle->signature.variadic) if (
    ffi_prep_cif(
    &handle->cif,
    FFI_DEFAULT_ABI,
    (unsigned int)handle->signature.arg_count,
    handle->signature.returns.ffi_type,
    handle->signature.arg_count > 0 ? handle->signature.ffi_arg_types : NULL
  ) != FFI_OK) {
    free(handle->symbol_name);
    ffi_signature_cleanup(&handle->signature);
    free(handle);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to prepare FFI call interface");
  }

  if (g_ffi_function_proto) js_set_proto_init(obj, g_ffi_function_proto);
  js_set_slot(obj, SLOT_CFUNC, js_mkfun(ffi_function_call));
  js_set_native_ptr(obj, handle);
  js_set_native_tag(obj, FFI_FUNCTION_NATIVE_TAG);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, library ? library->obj : js_mkundef());
  js_set_finalizer(obj, ffi_function_finalize);

  fn = js_obj_to_func(obj);
  return fn;
}

void ffi_library_finalize(ant_t *js, ant_object_t *obj) {
  ffi_library_handle_t *library;
  (void)js;
  if (obj->native.tag != FFI_LIBRARY_NATIVE_TAG) return;
  
  library = (ffi_library_handle_t *)obj->native.ptr;
  if (!library) return;
  
  ffi_library_close_handle(library);
  free(library->path);
  free(library);
  obj->native.ptr = NULL;
  obj->native.tag = 0;
}

void ffi_function_finalize(ant_t *js, ant_object_t *obj) {
  ffi_function_handle_t *handle;
  (void)js;
  if (obj->native.tag != FFI_FUNCTION_NATIVE_TAG) return;
  
  handle = (ffi_function_handle_t *)obj->native.ptr;
  if (!handle) return;
  
  free(handle->symbol_name);
  ffi_signature_cleanup(&handle->signature);
  free(handle);
  obj->native.ptr = NULL;
  obj->native.tag = 0;
}

void ffi_pointer_finalize(ant_t *js, ant_object_t *obj) {
  ffi_pointer_handle_t *handle;
  (void)js;
  if (obj->native.tag != FFI_POINTER_NATIVE_TAG) return;
  
  handle = (ffi_pointer_handle_t *)obj->native.ptr;
  if (!handle) return;
  
  ffi_pointer_close_handle(handle, false);
  free(handle);
  obj->native.ptr = NULL;
  obj->native.tag = 0;
}

void ffi_callback_finalize(ant_t *js, ant_object_t *obj) {
  ffi_callback_handle_t *handle;
  (void)js;
  if (obj->native.tag != FFI_CALLBACK_NATIVE_TAG) return;
  
  handle = (ffi_callback_handle_t *)obj->native.ptr;
  if (!handle) return;
  
  ffi_callback_close_handle(handle);
  free(handle);
  obj->native.ptr = NULL;
  obj->native.tag = 0;
}

static bool ffi_callback_result_to_c(
  ffi_callback_handle_t *callback,
  ant_value_t result,
  void *ret,
  ant_value_t *error_out
) {
  ffi_value_box_t box;
  ant_value_t err = js_mkundef();
  void *scratch = NULL;

  memset(&box, 0, sizeof(box));
  if (error_out) *error_out = js_mkundef();
  if (callback->signature.returns.id == FFI_VALUE_VOID) return true;

  if (!ffi_value_to_c(callback->js, result, callback->signature.returns, &box, &scratch, &err)) {
    if (error_out) *error_out = err;
    free(scratch);
    return false;
  }

  switch (callback->signature.returns.id) {
    case FFI_VALUE_INT8:   memcpy(ret, &box.i8, sizeof(box.i8)); break;
    case FFI_VALUE_INT16:  memcpy(ret, &box.i16, sizeof(box.i16)); break;
    case FFI_VALUE_INT:    memcpy(ret, &box.i32, sizeof(box.i32)); break;
    case FFI_VALUE_INT64:  memcpy(ret, &box.i64, sizeof(box.i64)); break;
    case FFI_VALUE_UINT8:  memcpy(ret, &box.u8, sizeof(box.u8)); break;
    case FFI_VALUE_UINT16: memcpy(ret, &box.u16, sizeof(box.u16)); break;
    case FFI_VALUE_UINT64: memcpy(ret, &box.u64, sizeof(box.u64)); break;
    case FFI_VALUE_FLOAT:  memcpy(ret, &box.f32, sizeof(box.f32)); break;
    case FFI_VALUE_DOUBLE: memcpy(ret, &box.f64, sizeof(box.f64)); break;
    case FFI_VALUE_POINTER:
      memcpy(ret, &box.ptr, sizeof(box.ptr));
      break;
    default:
      free(scratch);
      return false;
  }

  free(scratch);
  return true;
}

static void ffi_callback_trampoline(ffi_cif *cif, void *ret, void **args, void *user_data) {
  ffi_callback_handle_t *callback = (ffi_callback_handle_t *)user_data;
  ant_value_t js_args[32];
  ant_value_t fn = js_mkundef();
  ant_value_t result = js_mkundef();
  size_t argc = 0;

  (void)cif;
  if (!callback || callback->closed || !callback->js) return;
  ffi_zero_return_value(ret, callback->signature.returns);
  argc = callback->signature.arg_count;

  if (!pthread_equal(pthread_self(), callback->owner_thread)) {
    fprintf(stderr, "ant:ffi callback invoked off the JS thread; returning a zero value\n");
    return;
  }

  if (argc > 32) argc = 32;
  for (size_t i = 0; i < argc; i++) {
    js_args[i] = ffi_value_from_c(callback->js, args[i], callback->signature.args[i]);
  }

  fn = js_get_slot(callback->owner_obj, SLOT_DATA);
  if (!is_callable(fn)) {
    fprintf(stderr, "ant:ffi callback target is no longer callable; returning a zero value\n");
    return;
  }

  result = sv_vm_call(callback->js->vm, callback->js, fn, js_mkundef(), js_args, (int)argc, NULL, false);
  if (is_err(result)) {
    fprintf(stderr, "ant:ffi callback threw an exception; returning a zero value\n");
    callback->js->thrown_exists = 0;
    return;
  }

  if (!ffi_callback_result_to_c(callback, result, ret, NULL)) {
    fprintf(stderr, "ant:ffi callback returned an incompatible value; returning a zero value\n");
  }
}

static void ffi_init_prototypes(ant_t *js) {
  ant_value_t function_proto = 0;

  if (g_ffi_library_proto) return;

  function_proto = js_get_slot(js_glob(js), SLOT_FUNC_PROTO);
  if (vtype(function_proto) == T_UNDEF) function_proto = js_get_ctor_proto(js, "Function", 8);

  g_ffi_library_proto = js_mkobj(js);
  g_ffi_function_proto = js_mkobj(js);
  g_ffi_pointer_proto = js_mkobj(js);
  g_ffi_callback_proto = js_mkobj(js);

  if (is_object_type(js->sym.object_proto)) {
    js_set_proto_init(g_ffi_library_proto, js->sym.object_proto);
    js_set_proto_init(g_ffi_pointer_proto, js->sym.object_proto);
    js_set_proto_init(g_ffi_callback_proto, js->sym.object_proto);
  }

  if (is_object_type(function_proto)) js_set_proto_init(g_ffi_function_proto, function_proto);

  js_set_sym(js, g_ffi_library_proto, get_toStringTag_sym(), ANT_STRING("FFILibrary"));
  js_set_sym(js, g_ffi_function_proto, get_toStringTag_sym(), ANT_STRING("FFIFunction"));
  js_set_sym(js, g_ffi_pointer_proto, get_toStringTag_sym(), ANT_STRING("FFIPointer"));
  js_set_sym(js, g_ffi_callback_proto, get_toStringTag_sym(), ANT_STRING("FFICallback"));

  js_set(js, g_ffi_library_proto, "define", js_mkfun(ffi_library_define));
  js_set(js, g_ffi_library_proto, "call", js_mkfun(ffi_library_call));
  js_set(js, g_ffi_library_proto, "close", js_mkfun(ffi_library_close));

  js_set(js, g_ffi_pointer_proto, "address", js_mkfun(ffi_pointer_address));
  js_set(js, g_ffi_pointer_proto, "isNull", js_mkfun(ffi_pointer_is_null));
  js_set(js, g_ffi_pointer_proto, "read", js_mkfun(ffi_pointer_read));
  js_set(js, g_ffi_pointer_proto, "write", js_mkfun(ffi_pointer_write));
  js_set(js, g_ffi_pointer_proto, "offset", js_mkfun(ffi_pointer_offset));
  js_set(js, g_ffi_pointer_proto, "free", js_mkfun(ffi_pointer_free));

  js_set(js, g_ffi_callback_proto, "address", js_mkfun(ffi_callback_address));
  js_set(js, g_ffi_callback_proto, "close", js_mkfun(ffi_callback_close));

  js_set(js, g_ffi_function_proto, "address", js_mkfun(ffi_function_address));
}

static ant_value_t ffi_dlopen(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t path_val = js_mkundef();
  const char *path = NULL;
  
  size_t path_len = 0;
  void *dl = NULL;
  
  ffi_library_handle_t *library = NULL;
  ant_value_t obj = 0;

  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "dlopen(path) requires a library path");
  ffi_init_prototypes(js);

  path_val = js_tostring_val(js, args[0]);
  if (is_err(path_val) || vtype(path_val) != T_STR) return path_val;
  path = js_getstr(js, path_val, &path_len);
  if (!path) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid library path");

  dl = dlopen(path, RTLD_LAZY);
  if (!dl) return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to load library: %s", dlerror());

  library = calloc(1, sizeof(*library));
  if (!library) {
    dlclose(dl);
    return js_mkerr(js, "Out of memory");
  }

  library->path = malloc(path_len + 1);
  if (!library->path) {
    dlclose(dl);
    free(library);
    return js_mkerr(js, "Out of memory");
  }

  memcpy(library->path, path, path_len);
  library->path[path_len] = '\0';
  library->handle = dl;
  library->closed = false;

  obj = js_mkobj(js);
  if (g_ffi_library_proto) js_set_proto_init(obj, g_ffi_library_proto);
  library->obj = obj;
  js_set_native_ptr(obj, library);
  js_set_native_tag(obj, FFI_LIBRARY_NATIVE_TAG);
  js_set_finalizer(obj, ffi_library_finalize);
  return obj;
}

ant_value_t ffi_library_close(ant_t *js, ant_value_t *args, int nargs) {
  ffi_library_handle_t *library = ffi_library_data(js_getthis(js));
  (void)args;
  (void)nargs;
  if (!library) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFILibrary");
  ffi_library_close_handle(library);
  return js_getthis(js);
}

ant_value_t ffi_library_define(ant_t *js, ant_value_t *args, int nargs) {
  ffi_library_handle_t *library = ffi_library_data(js_getthis(js));
  ffi_signature_t signature;
  
  ant_value_t error = js_mkundef();
  ant_value_t fn = js_mkundef();
  
  const char *symbol_name = NULL;
  void *sym = NULL;
  memset(&signature, 0, sizeof(signature));

  if (!library) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFILibrary");
  if (library->closed) return js_mkerr_typed(js, JS_ERR_TYPE, "FFILibrary is closed");
  
  if (nargs < 2 || vtype(args[0]) != T_STR) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "define(name, signature) requires a symbol name and signature");
  }

  if (!ffi_parse_signature(js, args[1], true, true, &signature, &error)) return error;

  symbol_name = js_getstr(js, args[0], NULL);
  sym = dlsym(library->handle, symbol_name);
  
  if (!sym) {
    ffi_signature_cleanup(&signature);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol '%s' was not found", symbol_name);
  }

  fn = ffi_make_function(js, library, symbol_name, &signature, sym);
  if (is_err(fn)) {
    ffi_signature_cleanup(&signature);
    return fn;
  }

  js_set(js, js_getthis(js), symbol_name, fn);
  return fn;
}

ant_value_t ffi_library_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_mkundef();
  ffi_library_handle_t *library = ffi_library_data(js_getthis(js));

  if (!library) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFILibrary");
  if (library->closed) return js_mkerr_typed(js, JS_ERR_TYPE, "FFILibrary is closed");
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "call(name, ...args) requires a symbol name");
  }

  fn = js_get(js, js_getthis(js), js_getstr(js, args[0], NULL));
  if (!is_callable(fn)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol '%s' has not been defined", js_getstr(js, args[0], NULL));
  }

  return sv_vm_call(js->vm, js, fn, js_mkundef(), args + 1, nargs - 1, NULL, false);
}

ant_value_t ffi_function_call(ant_t *js, ant_value_t *args, int nargs) {
  ffi_function_handle_t *function = ffi_function_data(js->current_func);
  ffi_type **call_types = NULL;
  ffi_value_box_t *values = NULL;
  
  void **call_args = NULL;
  void **scratch = NULL;
  
  ffi_marshaled_type_t *dynamic_types = NULL;
  ffi_cif call_cif;
  ffi_value_box_t result;
  ant_value_t error = js_mkundef();
  
  int status = FFI_OK;
  size_t actual_argc = 0;

  memset(&result, 0, sizeof(result));

  if (!function) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFIFunction");
  if (!function->library || function->library->closed) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "FFIFunction '%s' belongs to a closed library", function->symbol_name);
  }

  if (!function->signature.variadic && nargs != (int)function->signature.arg_count) return js_mkerr_typed(
    js, JS_ERR_TYPE,
    "FFIFunction '%s' expects %zu arguments, got %d",
    function->symbol_name,
    function->signature.arg_count,
    nargs
  );

  if (function->signature.variadic && nargs < (int)function->signature.fixed_arg_count) return js_mkerr_typed(
    js, JS_ERR_TYPE,
    "FFIFunction '%s' expects at least %zu arguments, got %d",
    function->symbol_name,
    function->signature.fixed_arg_count,
    nargs
  );

  actual_argc = (size_t)nargs;
  if (actual_argc > 0) {
    call_types = calloc(actual_argc, sizeof(*call_types));
    values = calloc(actual_argc, sizeof(*values));
    call_args = calloc(actual_argc, sizeof(*call_args));
    scratch = calloc(actual_argc, sizeof(*scratch));
    dynamic_types = calloc(actual_argc, sizeof(*dynamic_types));
    if (!call_types || !values || !call_args || !scratch || !dynamic_types) {
      error = js_mkerr(js, "Out of memory");
      goto cleanup;
    }
  }

  for (size_t i = 0; i < actual_argc; i++) {
    ffi_marshaled_type_t type = i < function->signature.fixed_arg_count
      ? function->signature.args[i]
      : ffi_infer_variadic_type(args[i]);
    
    dynamic_types[i] = type;
    call_types[i] = type.ffi_type;
    call_args[i] = &values[i];
    
    if (!ffi_value_to_c(js, args[i], type, &values[i], &scratch[i], &error)) goto cleanup;
  }

  if (function->signature.variadic) {
    status = ffi_prep_cif_var(
      &call_cif,
      FFI_DEFAULT_ABI,
      (unsigned int)function->signature.fixed_arg_count,
      (unsigned int)actual_argc,
      function->signature.returns.ffi_type,
      call_types
    );
    
    if (status != FFI_OK) {
      error = js_mkerr_typed(js, JS_ERR_TYPE, "Failed to prepare variadic FFI call");
      goto cleanup;
    }
    
    ffi_call(&call_cif, function->func_ptr, &result, call_args);
  } else ffi_call(&function->cif, function->func_ptr, &result, call_args);

cleanup:
  if (vtype(error) != T_UNDEF) {
    size_t i;
    for (i = 0; i < actual_argc; i++) free(scratch ? scratch[i] : NULL);
    free(dynamic_types);
    free(scratch);
    free(call_args);
    free(values);
    free(call_types);
    return error;
  }

  {
    ant_value_t out = ffi_value_from_c(js, &result, function->signature.returns);
    size_t i;
    for (i = 0; i < actual_argc; i++) free(scratch ? scratch[i] : NULL);
    free(dynamic_types);
    free(scratch);
    free(call_args);
    free(values);
    free(call_types);
    return out;
  }
}

ant_value_t ffi_function_address(ant_t *js, ant_value_t *args, int nargs) {
  ffi_function_handle_t *function = ffi_function_data(js_getthis(js));
  (void)args;
  (void)nargs;
  if (!function) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFIFunction");
  return js_mknum((double)(uintptr_t)function->func_ptr);
}

static ant_value_t ffi_alloc_memory(ant_t *js, ant_value_t *args, int nargs) {
  ffi_pointer_region_t *region = NULL;
  size_t size = 0;

  if (nargs < 1 || vtype(args[0]) != T_NUM) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "alloc(size) requires a numeric size");
  }

  size = (size_t)js_getnum(args[0]);
  if (size == 0) return js_mkerr_typed(js, JS_ERR_RANGE, "alloc(size) requires a positive size");

  region = calloc(1, sizeof(*region));
  if (!region) return js_mkerr(js, "Out of memory");

  region->ptr = malloc(size);
  if (!region->ptr) {
    free(region);
    return js_mkerr(js, "Out of memory");
  }

  region->size = size;
  region->owned = true;
  region->freed = false;
  region->size_known = true;
  return ffi_make_pointer(js, region, 0);
}

static ant_value_t ffi_pointer_value(ant_t *js, ant_value_t *args, int nargs) {
  ffi_pointer_region_t *region = NULL;
  ant_value_t error = js_mkundef();
  char *str_copy = NULL;
  size_t str_len = 0;
  const uint8_t *buffer_bytes = NULL;
  size_t buffer_len = 0;

  if (nargs < 1 || ffi_is_nullish(args[0])) return ffi_make_pointer_from_raw(js, NULL);
  if (ffi_pointer_data(args[0])) return args[0];

  if (ffi_callback_data(args[0])) {
    void *ptr = NULL;
    if (!ffi_pointer_from_js(js, args[0], &ptr, &error)) return error;
    return ffi_make_pointer_or_null(js, ptr);
  }

  region = calloc(1, sizeof(*region));
  if (!region) return js_mkerr(js, "Out of memory");

  if (buffer_source_get_bytes(js, args[0], &buffer_bytes, &buffer_len)) {
    region->ptr = (uint8_t *)buffer_bytes;
    region->size = buffer_len;
    region->size_known = true;
    region->owned = false;
    region->freed = false;
    return ffi_make_pointer(js, region, 0);
  }

  if (!ffi_copy_js_string(js, args[0], &str_copy, &str_len, &error)) {
    free(region);
    return error;
  }

  region->ptr = (uint8_t *)str_copy;
  region->size = str_len + 1;
  region->size_known = true;
  region->owned = true;
  region->freed = false;
  return ffi_make_pointer(js, region, 0);
}

ant_value_t ffi_pointer_address(ant_t *js, ant_value_t *args, int nargs) {
  ffi_pointer_handle_t *handle = ffi_pointer_data(js_getthis(js));
  (void)args;
  (void)nargs;
  if (!handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFIPointer");
  return js_mknum((double)(uintptr_t)ffi_pointer_address_raw(handle));
}

ant_value_t ffi_pointer_is_null(ant_t *js, ant_value_t *args, int nargs) {
  ffi_pointer_handle_t *handle = ffi_pointer_data(js_getthis(js));
  (void)args;
  (void)nargs;
  if (!handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFIPointer");
  return js_bool(ffi_pointer_address_raw(handle) == NULL);
}

ant_value_t ffi_pointer_read(ant_t *js, ant_value_t *args, int nargs) {
  ffi_pointer_handle_t *handle = ffi_pointer_data(js_getthis(js));
  ffi_marshaled_type_t type;

  if (!handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFIPointer");
  type = nargs > 0 ? ffi_marshaled_type_from_value(js, args[0]) : ffi_marshaled_type_make(FFI_VALUE_POINTER, "pointer");
  if (type.id == FFI_VALUE_UNKNOWN || type.id == FFI_VALUE_SPREAD || type.id == FFI_VALUE_VOID) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported FFIPointer.read() type");
  }
  return ffi_read_from_pointer(js, handle, type);
}

ant_value_t ffi_pointer_write(ant_t *js, ant_value_t *args, int nargs) {
  ffi_pointer_handle_t *handle = ffi_pointer_data(js_getthis(js));
  ffi_marshaled_type_t type;

  if (!handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFIPointer");
  if (nargs < 2) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "FFIPointer.write(type, value) requires a type and value");
  }

  type = ffi_marshaled_type_from_value(js, args[0]);
  if (type.id == FFI_VALUE_UNKNOWN || type.id == FFI_VALUE_SPREAD || type.id == FFI_VALUE_VOID) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported FFIPointer.write() type");
  }

  return ffi_write_to_pointer(js, handle, type, args[1]);
}

ant_value_t ffi_pointer_offset(ant_t *js, ant_value_t *args, int nargs) {
  ffi_pointer_handle_t *handle = ffi_pointer_data(js_getthis(js));
  ffi_pointer_handle_t *next = NULL;
  ant_value_t out = 0;
  size_t offset = 0;

  if (!handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFIPointer");
  if (nargs < 1 || vtype(args[0]) != T_NUM) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "FFIPointer.offset(bytes) requires a numeric byte offset");
  }

  if (js_getnum(args[0]) < 0) return js_mkerr_typed(js, JS_ERR_RANGE, "FFIPointer.offset() requires a non-negative offset");
  offset = (size_t)js_getnum(args[0]);

  if (handle->region && handle->region->size_known && handle->byte_offset + offset > handle->region->size) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "FFIPointer.offset() is out of bounds");
  }

  out = ffi_make_pointer(js, handle->region, handle->byte_offset + offset);
  next = ffi_pointer_data(out);
  if (!next) return out;
  return out;
}

ant_value_t ffi_pointer_free(ant_t *js, ant_value_t *args, int nargs) {
  ffi_pointer_handle_t *handle = ffi_pointer_data(js_getthis(js));
  (void)args;
  (void)nargs;
  if (!handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFIPointer");
  if (!handle->region || !handle->region->owned) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Only owned FFIPointers can be freed");
  }
  if (!ffi_pointer_region_free(handle->region)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "FFIPointer has already been freed");
  }
  return js_getthis(js);
}

static ant_value_t ffi_create_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t signature_val = js_mkundef();
  ant_value_t fn = js_mkundef();
  ant_value_t obj = 0;
  ffi_callback_handle_t *callback = NULL;
  ant_value_t error = js_mkundef();

  if (nargs < 2) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "callback(signature, fn) requires a signature and function");
  }

  if (is_callable(args[0]) && is_object_type(args[1])) {
    fn = args[0];
    signature_val = args[1];
  } else if (is_object_type(args[0]) && is_callable(args[1])) {
    signature_val = args[0];
    fn = args[1];
  } else return js_mkerr_typed(js, JS_ERR_TYPE, "callback(signature, fn) requires a signature object and callable function");

  ffi_init_prototypes(js);

  callback = calloc(1, sizeof(*callback));
  if (!callback) return js_mkerr(js, "Out of memory");

  if (!ffi_parse_signature(js, signature_val, false, false, &callback->signature, &error)) {
    free(callback);
    return error;
  }

  if (ffi_prep_cif(
        &callback->cif,
        FFI_DEFAULT_ABI,
        (unsigned int)callback->signature.arg_count,
        callback->signature.returns.ffi_type,
        callback->signature.arg_count > 0 ? callback->signature.ffi_arg_types : NULL
      ) != FFI_OK) {
    ffi_signature_cleanup(&callback->signature);
    free(callback);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to prepare FFICallback signature");
  }

  callback->closure = ffi_closure_alloc(sizeof(*callback->closure), &callback->code_ptr);
  if (!callback->closure) {
    ffi_signature_cleanup(&callback->signature);
    free(callback);
    return js_mkerr(js, "Failed to allocate FFICallback closure");
  }

  if (ffi_prep_closure_loc(callback->closure, &callback->cif, ffi_callback_trampoline, callback, callback->code_ptr) != FFI_OK) {
    ffi_closure_free(callback->closure);
    ffi_signature_cleanup(&callback->signature);
    free(callback);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to prepare FFICallback closure");
  }

  obj = js_mkobj(js);
  if (g_ffi_callback_proto) js_set_proto_init(obj, g_ffi_callback_proto);
  callback->js = js;
  callback->owner_obj = obj;
  callback->owner_thread = pthread_self();
  callback->closed = false;
  js_set_native_ptr(obj, callback);
  js_set_native_tag(obj, FFI_CALLBACK_NATIVE_TAG);
  js_set_slot_wb(js, obj, SLOT_DATA, fn);
  js_set_finalizer(obj, ffi_callback_finalize);
  return obj;
}

ant_value_t ffi_callback_address(ant_t *js, ant_value_t *args, int nargs) {
  ffi_callback_handle_t *callback = ffi_callback_data(js_getthis(js));
  (void)args;
  (void)nargs;
  if (!callback) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFICallback");
  return js_mknum((double)(uintptr_t)callback->code_ptr);
}

ant_value_t ffi_callback_close(ant_t *js, ant_value_t *args, int nargs) {
  ffi_callback_handle_t *callback = ffi_callback_data(js_getthis(js));
  (void)args;
  (void)nargs;
  if (!callback) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected an FFICallback");
  ffi_callback_close_handle(callback);
  return js_getthis(js);
}

ant_value_t ffi_library(ant_t *js) {
  ant_value_t ffi_obj = js_mkobj(js);
  ant_value_t ffi_types = js_mkobj(js);
  const char *suffix = "so";

  ffi_init_prototypes(js);

  js_set(js, ffi_obj, "dlopen", js_mkfun(ffi_dlopen));
  js_set(js, ffi_obj, "alloc", js_mkfun(ffi_alloc_memory));
  js_set(js, ffi_obj, "pointer", js_mkfun(ffi_pointer_value));
  js_set(js, ffi_obj, "callback", js_mkfun(ffi_create_callback));

#ifdef __APPLE__
  suffix = "dylib";
#elif defined(_WIN32)
  suffix = "dll";
#endif

  js_set(js, ffi_obj, "suffix", js_mkstr(js, suffix, strlen(suffix)));

  js_set(js, ffi_types, "void", ANT_STRING("void"));
  js_set(js, ffi_types, "int8", ANT_STRING("int8"));
  js_set(js, ffi_types, "int16", ANT_STRING("int16"));
  js_set(js, ffi_types, "int", ANT_STRING("int"));
  js_set(js, ffi_types, "int64", ANT_STRING("int64"));
  js_set(js, ffi_types, "uint8", ANT_STRING("uint8"));
  js_set(js, ffi_types, "uint16", ANT_STRING("uint16"));
  js_set(js, ffi_types, "uint64", ANT_STRING("uint64"));
  js_set(js, ffi_types, "float", ANT_STRING("float"));
  js_set(js, ffi_types, "double", ANT_STRING("double"));
  js_set(js, ffi_types, "pointer", ANT_STRING("pointer"));
  js_set(js, ffi_types, "string", ANT_STRING("string"));
  js_set(js, ffi_types, "spread", ANT_STRING("..."));
  
  js_set(js, ffi_obj, "FFIType", ffi_types);
  js_set_sym(js, ffi_obj, get_toStringTag_sym(), ANT_STRING("FFI"));
  
  return ffi_obj;
}
