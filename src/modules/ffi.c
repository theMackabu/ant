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
#include "cfunc.h"
#include "common.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "silver/vm.h"

#include "modules/buffer.h"
#include "modules/ffi.h"

#if defined(__APPLE__)
#define FFI_LIBRARY_SUFFIX "dylib"
#elif defined(_WIN32)
#define FFI_LIBRARY_SUFFIX "dll"
#else
#define FFI_LIBRARY_SUFFIX "so"
#endif

static constexpr size_t FFI_MAX_ARGS = 255;
static constexpr size_t FFI_INLINE_ARGS = 16;

#define ANT_FFI_KIND(kind, tag, label, parent)                     \
  static constexpr uint32_t ffi_##kind##_tag = Ant_NativeTag(tag); \
  static const char ffi_##kind##_label[] = label;
#include "ffi/kinds.h"

typedef enum {
#define ANT_FFI_TYPE(id, key, name, ffi, ctype, kind) FFI_VALUE_##id,
#include "ffi/types.h"
  FFI_VALUE_UNKNOWN,
} ffi_value_type_id_t;

typedef struct {
  const char *name;
  ffi_type *ffi;
  uint8_t name_len;
  uint8_t size;
} ffi_type_info_t;

#define FFI_SIZE_NONE(ctype) 0
#define FFI_SIZE_STR(ctype)  1
#define FFI_SIZE_INT(ctype)  sizeof(ctype)
#define FFI_SIZE_WIDE        FFI_SIZE_INT
#define FFI_SIZE_REAL        FFI_SIZE_INT
#define FFI_SIZE_PTR         FFI_SIZE_INT

static const ffi_type_info_t ffi_type_table[] = {
#define ANT_FFI_TYPE(id, key, name, ffi, ctype, kind) \
  [FFI_VALUE_##id] = { name, ffi, sizeof(name) - 1, FFI_SIZE_##kind(ctype) },
#include "ffi/types.h"
};

typedef enum {
  FFI_REGION_SIZED = 1u << 0,
  FFI_REGION_OWNED = 1u << 1,
  FFI_REGION_SOURCED = 1u << 2,
} ffi_region_flags_t;

typedef struct {
  ffi_type **ffi_arg_types;
  ffi_value_type_id_t *args;
  size_t arg_count;
  ffi_value_type_id_t returns;
  bool variadic;
} ffi_signature_t;

typedef struct {
  ant_value_t obj;
  void *handle;
  bool closed;
} ffi_library_handle_t;

typedef struct {
  ffi_library_handle_t *library;
  ffi_signature_t signature;
  ffi_cif cif;
  void *func_ptr;
  char *symbol_name;
} ffi_function_handle_t;

typedef struct {
  uint8_t *ptr;
  size_t size;
  size_t ref_count;
  bool size_known;
  bool owned;
  bool sourced;
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
  int64_t i64;
  double f64;
  void *ptr;
  ffi_arg raw;
} ffi_value_box_t;

typedef struct {
  ffi_value_box_t *values;
  void **avalues;
  void **owned;
  ffi_type **atypes;
  size_t owned_count;
  size_t pending;
  void *heap;
  ffi_value_box_t inline_values[FFI_INLINE_ARGS];
  void *inline_avalues[FFI_INLINE_ARGS];
  void *inline_owned[FFI_INLINE_ARGS];
  ffi_type *inline_atypes[FFI_INLINE_ARGS];
} ffi_frame_t;

#define ANT_FFI_KIND(kind, tag, label, parent)                                \
  static inline ffi_##kind##_handle_t *ffi_##kind##_data(ant_value_t value) { \
    return js_get_native(value, ffi_##kind##_tag);                            \
  }
#include "ffi/kinds.h"

#define FFI_ERR_TYPE(...) js_mkerr_typed(js, JS_ERR_TYPE, __VA_ARGS__)
#define FFI_ERR_RANGE(...) js_mkerr_typed(js, JS_ERR_RANGE, __VA_ARGS__)

#define FFI_RECEIVER(kind, var)                                   \
  ffi_##kind##_handle_t *var = ffi_##kind##_data(call_this); \
  if (!var) return ffi_receiver_error(js, ffi_##kind##_label)

#define FFI_POINTER_RECEIVER(var) \
  FFI_RECEIVER(pointer, var);     \
  ffi_pointer_sync(js, call_this, var)

#define FFI_WRAP(kind, handle) \
  ffi_wrap(js, js->builtins.ffi_##kind##_proto, (handle), ffi_##kind##_tag, ffi_##kind##_finalize)

static __attribute__((noinline)) ant_value_t ffi_receiver_error(ant_t *js, const char *label) {
  return FFI_ERR_TYPE("Expected an %s", label);
}

static inline bool ffi_is_nullish(ant_value_t value) {
  return is_null(value) || is_undefined(value);
}

static inline bool ffi_type_is_data(ffi_value_type_id_t type) {
  return type < FFI_VALUE_UNKNOWN && ffi_type_table[type].size;
}

static inline int32_t ffi_to_int32(double number) {
  return number >= INT32_MIN && number <= INT32_MAX ? (int32_t)number : js_to_int32(number);
}

static inline ant_value_t ffi_to_number(ant_t *js, ant_value_t value, double *out) {
  if (vtype(value) == kTypeNumber) {
    *out = Ant_Value_AsNumberUnchecked(value);
    return Ant_Value_Undefined;
  }

  *out = js_to_number(js, value);
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : Ant_Value_Undefined;
}

static ffi_value_type_id_t ffi_type_from_value(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeString) return FFI_VALUE_UNKNOWN;

  size_t len = 0;
  const char *name = js_getstr(js, value, &len);
  if (!name) return FFI_VALUE_UNKNOWN;

  for (ffi_value_type_id_t id = 0; id < FFI_VALUE_UNKNOWN; id++) {
    const ffi_type_info_t *info = &ffi_type_table[id];
    if (info->name_len == len && memcmp(info->name, name, len) == 0) return id;
  }

  return FFI_VALUE_UNKNOWN;
}

static ffi_value_type_id_t ffi_infer_variadic_type(ant_value_t value) {
  switch (vtype(value)) {
    case kTypeNumber: {
      double number = Ant_Value_AsNumberUnchecked(value);
      return number == ffi_to_int32(number) ? FFI_VALUE_INT : FFI_VALUE_DOUBLE;
    }
    case kTypeBool:   return FFI_VALUE_INT;
    case kTypeString: return FFI_VALUE_STRING;
    default:          return FFI_VALUE_POINTER;
  }
}

static void ffi_signature_cleanup(ffi_signature_t *signature) {
  free(signature->ffi_arg_types);
  *signature = (ffi_signature_t){ .returns = FFI_VALUE_UNKNOWN };
}

static ant_value_t ffi_signature_fail(ffi_signature_t *signature, ant_value_t error) {
  ffi_signature_cleanup(signature);
  return error;
}

static ant_value_t ffi_get_index(ant_t *js, ant_value_t list, size_t index) {
  if (vtype(list) == kTypeArray) return js_arr_get(js, list, (ant_offset_t)index);

  char key[24];
  snprintf(key, sizeof(key), "%zu", index);
  return js_get(js, list, key);
}

static ant_value_t ffi_parse_signature(ant_t *js, ant_value_t value, bool for_callback, ffi_signature_t *out) {
  *out = (ffi_signature_t){ .returns = FFI_VALUE_UNKNOWN };
  if (!is_object_type(value))
    return FFI_ERR_TYPE("FFI signature must be [returnType, argTypes] or { returns, args }");

  ant_value_t returns_val = js_get(js, value, "returns");
  ant_value_t args_val = js_get(js, value, "args");

  if (is_undefined(returns_val) || is_undefined(args_val)) {
    returns_val = js_get(js, value, "0");
    args_val = js_get(js, value, "1");
  }

  if (vtype(returns_val) != kTypeString) return FFI_ERR_TYPE("FFI return type must be a string");
  if (!is_object_type(args_val)) return FFI_ERR_TYPE("FFI argument list must be an array-like object");

  ffi_value_type_id_t returns = ffi_type_from_value(js, returns_val);
  if (returns == FFI_VALUE_UNKNOWN || returns == FFI_VALUE_SPREAD) return FFI_ERR_TYPE("Unsupported FFI return type");
  if (for_callback && returns == FFI_VALUE_STRING) return FFI_ERR_TYPE("FFICallback does not support string return values");
  out->returns = returns;

  ant_value_t len_val = js_get(js, args_val, "length");
  double len = vtype(len_val) == kTypeNumber ? Ant_Value_AsNumberUnchecked(len_val) : 0;
  
  if (!(len >= 0 && len <= (double)FFI_MAX_ARGS))
    return FFI_ERR_RANGE("FFI signatures support at most %zu arguments", FFI_MAX_ARGS);

  size_t count = (size_t)len;
  if (count == 0) return Ant_Value_Undefined;

  out->ffi_arg_types = malloc(count * (sizeof(*out->ffi_arg_types) + sizeof(*out->args)));
  if (!out->ffi_arg_types) return Ant_Error_CreateOutOfMemory(js);
  out->args = (ffi_value_type_id_t *)(out->ffi_arg_types + count);

  for (size_t i = 0; i < count; i++) {
    ffi_value_type_id_t type = ffi_type_from_value(js, ffi_get_index(js, args_val, i));

    if (type == FFI_VALUE_SPREAD) {
      if (for_callback) return ffi_signature_fail(out, FFI_ERR_TYPE("Variadic signatures are not supported here"));
      if (i + 1 != count) return ffi_signature_fail(out, FFI_ERR_TYPE("FFI spread marker must be the last argument type"));
      out->variadic = true;
      break;
    }

    if (!ffi_type_is_data(type)) return ffi_signature_fail(out, FFI_ERR_TYPE("Unsupported FFI argument type"));
    out->args[i] = type;
    out->ffi_arg_types[i] = ffi_type_table[type].ffi;
    out->arg_count = i + 1;
  }

  return Ant_Value_Undefined;
}

static bool ffi_prep_signature(ffi_cif *cif, ffi_signature_t *signature) {
  return ffi_prep_cif(
    cif, FFI_DEFAULT_ABI,
    (unsigned int)signature->arg_count,
    ffi_type_table[signature->returns].ffi,
    signature->arg_count ? signature->ffi_arg_types : NULL
  ) == FFI_OK;
}

static void ffi_library_close_handle(ffi_library_handle_t *library) {
  if (library->closed) return;
  library->closed = true;
  if (library->handle) dlclose(library->handle);
  library->handle = NULL;
}

static void ffi_callback_close_handle(ffi_callback_handle_t *callback) {
  if (callback->closed) return;
  callback->closed = true;
  if (callback->closure) ffi_closure_free(callback->closure);
  callback->closure = NULL;
  callback->code_ptr = NULL;
  ffi_signature_cleanup(&callback->signature);
}

static void ffi_pointer_region_release(ffi_pointer_region_t *region) {
  if (region->ref_count > 1) {
    region->ref_count--;
    return;
  }

  if (region->owned) free(region->ptr);
  free(region);
}

static bool ffi_pointer_region_free(ffi_pointer_region_t *region) {
  if (!region->owned || region->freed) return false;
  free(region->ptr);
  region->ptr = NULL;
  region->freed = true;
  region->size = 0;
  region->size_known = true;
  return true;
}

static void ffi_library_destroy(ffi_library_handle_t *library) {
  ffi_library_close_handle(library);
  free(library);
}

static void ffi_function_destroy(ffi_function_handle_t *function) {
  free(function->symbol_name);
  ffi_signature_cleanup(&function->signature);
  free(function);
}

static void ffi_pointer_destroy(ffi_pointer_handle_t *pointer) {
  ffi_pointer_region_release(pointer->region);
  free(pointer);
}

static void ffi_callback_destroy(ffi_callback_handle_t *callback) {
  ffi_callback_close_handle(callback);
  free(callback);
}

#define ANT_FFI_KIND(kind, tag, label, parent)                \
  void ffi_##kind##_finalize(ant_t *js, ant_object_t *obj) {  \
    ant_value_t value = js_obj_from_ptr(obj);                 \
    ffi_##kind##_handle_t *handle = ffi_##kind##_data(value); \
    if (!handle) return;                                      \
    ffi_##kind##_destroy(handle);                             \
    js_clear_native(value, ffi_##kind##_tag);                 \
  }
#include "ffi/kinds.h"

static ant_value_t ffi_wrap(ant_t *js, ant_value_t proto, void *handle, uint32_t tag, js_finalizer_fn finalize) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (proto) js_set_proto_init(obj, proto);
  js_set_native(obj, handle, tag);
  js_set_finalizer(obj, finalize);
  return obj;
}

static inline uint8_t *ffi_pointer_address_raw(const ffi_pointer_handle_t *handle) {
  uint8_t *base = handle->region->ptr;
  return base ? base + handle->byte_offset : NULL;
}

static inline void ffi_pointer_sync(ant_t *js, ant_value_t obj, const ffi_pointer_handle_t *handle) {
  ffi_pointer_region_t *region = handle->region;
  if (!region->sourced) return;

  const uint8_t *bytes = NULL;
  buffer_source_get_bytes(js, js_get_slot(obj, SLOT_DATA), &bytes, &region->size);
  region->ptr = (uint8_t *)bytes;
}

static ant_value_t ffi_make_pointer(ant_t *js, ffi_pointer_region_t *region, size_t byte_offset) {
  ffi_pointer_handle_t *handle = malloc(sizeof(*handle));
  if (!handle) {
    if (!region->ref_count) ffi_pointer_region_release(region);
    return Ant_Error_CreateOutOfMemory(js);
  }

  *handle = (ffi_pointer_handle_t){ .region = region, .byte_offset = byte_offset };
  region->ref_count++;
  return FFI_WRAP(pointer, handle);
}

static ant_value_t ffi_pointer_new(ant_t *js, void *ptr, size_t size, unsigned flags) {
  ffi_pointer_region_t *region = malloc(sizeof(*region));
  if (!region) {
    if (flags & FFI_REGION_OWNED) free(ptr);
    return Ant_Error_CreateOutOfMemory(js);
  }

  *region = (ffi_pointer_region_t){
    .ptr = ptr,
    .size = size,
    .size_known = flags & FFI_REGION_SIZED,
    .owned = flags & FFI_REGION_OWNED,
    .sourced = flags & FFI_REGION_SOURCED,
  };

  return ffi_make_pointer(js, region, 0);
}

static ant_value_t ffi_make_pointer_or_null(ant_t *js, void *ptr) {
  return ptr ? ffi_pointer_new(js, ptr, 0, 0) : Ant_Value_Null;
}

static ant_value_t ffi_pointer_from_js(ant_t *js, ant_value_t value, void **out) {
  *out = NULL;
  if (ffi_is_nullish(value)) return Ant_Value_Undefined;

  ffi_pointer_handle_t *pointer = ffi_pointer_data(value);
  if (pointer) {
    ffi_pointer_sync(js, value, pointer);
    const ffi_pointer_region_t *region = pointer->region;

    if (region->freed) return FFI_ERR_TYPE("FFIPointer has been freed");
    if (region->sourced && !region->ptr) return FFI_ERR_TYPE("FFIPointer source buffer is detached");
    if (region->sourced && pointer->byte_offset > region->size) return FFI_ERR_RANGE("Pointer offset is out of bounds");

    *out = ffi_pointer_address_raw(pointer);
    return Ant_Value_Undefined;
  }

  ffi_callback_handle_t *callback = ffi_callback_data(value);
  if (!callback) return FFI_ERR_TYPE("Pointer arguments require FFIPointer, FFICallback, null, or undefined");
  if (callback->closed) return FFI_ERR_TYPE("FFICallback has been closed");

  *out = callback->code_ptr;
  return Ant_Value_Undefined;
}

static ant_value_t ffi_pointer_to_c(ant_t *js, ant_value_t value, void *dst) {
  void *ptr = NULL;
  ant_value_t error = ffi_pointer_from_js(js, value, &ptr);
  if (!is_err(error)) memcpy(dst, &ptr, sizeof(ptr));
  return error;
}

static ant_value_t ffi_copy_js_string(ant_t *js, ant_value_t value, char **out, size_t *len_out) {
  ant_value_t str = js_tostring_val(js, value);
  if (is_err(str)) return str;

  size_t len = 0;
  const char *src = js_getstr(js, str, &len);
  if (!src) return FFI_ERR_TYPE("Invalid string value");

  char *copy = malloc(len + 1);
  if (!copy) return Ant_Error_CreateOutOfMemory(js);

  memcpy(copy, src, len);
  copy[len] = '\0';
  *out = copy;
  if (len_out) *len_out = len;

  return Ant_Value_Undefined;
}

#define FFI_STORE_NUM(id, ctype, convert)               \
  case FFI_VALUE_##id: {                                \
    double num_ = 0;                                    \
    ant_value_t err_ = ffi_to_number(js, value, &num_); \
    if (is_err(err_)) return err_;                      \
    ctype v_ = (ctype)convert(num_);                    \
    memcpy(dst, &v_, sizeof(v_));                       \
    return err_;                                        \
  }

#define FFI_TO_C_INT(id, ctype)  FFI_STORE_NUM(id, ctype, ffi_to_int32)
#define FFI_TO_C_WIDE(id, ctype) FFI_STORE_NUM(id, ctype, )
#define FFI_TO_C_REAL(id, ctype) FFI_STORE_NUM(id, ctype, )
#define FFI_TO_C_PTR(id, ctype)  case FFI_VALUE_##id: return ffi_pointer_to_c(js, value, dst);
#define FFI_TO_C_STR(id, ctype)
#define FFI_TO_C_NONE(id, ctype)

static ant_value_t ffi_value_to_c(ant_t *js, ant_value_t value, ffi_value_type_id_t type, void *dst) {
  switch (type) {
#define ANT_FFI_TYPE(id, key, name, ffi, ctype, kind) FFI_TO_C_##kind(id, ctype)
#include "ffi/types.h"
    default: return FFI_ERR_TYPE("Unsupported FFI argument conversion");
  }
}

#define FFI_LOAD(ctype) ctype v_; memcpy(&v_, src, sizeof(v_))

#define FFI_FROM_C_INT(id, ctype)  case FFI_VALUE_##id: { FFI_LOAD(ctype); return Ant_Value_FromNumber((double)v_); }
#define FFI_FROM_C_WIDE            FFI_FROM_C_INT
#define FFI_FROM_C_REAL            FFI_FROM_C_INT
#define FFI_FROM_C_PTR(id, ctype)  case FFI_VALUE_##id: { FFI_LOAD(ctype); return ffi_make_pointer_or_null(js, v_); }
#define FFI_FROM_C_STR(id, ctype)  case FFI_VALUE_##id: { FFI_LOAD(ctype); return v_ ? js_mkstr(js, v_, strlen(v_)) : Ant_Value_Null; }
#define FFI_FROM_C_NONE(id, ctype)

static ant_value_t ffi_value_from_c(ant_t *js, const void *src, ffi_value_type_id_t type) {
  switch (type) {
#define ANT_FFI_TYPE(id, key, name, ffi, ctype, kind) FFI_FROM_C_##kind(id, ctype)
#include "ffi/types.h"
    default: return Ant_Value_Undefined;
  }
}

static ant_value_t ffi_pointer_check(ant_t *js, const ffi_pointer_handle_t *handle, size_t size, const char *op) {
  const ffi_pointer_region_t *region = handle->region;

  if (region->freed) return FFI_ERR_TYPE("FFIPointer has been freed");
  if (!region->ptr && region->sourced) return FFI_ERR_TYPE("FFIPointer source buffer is detached");
  if (!region->ptr) return FFI_ERR_TYPE("Cannot %s through a null pointer", op);
  if (!region->size_known) return Ant_Value_Undefined;

  if (handle->byte_offset > region->size) return FFI_ERR_RANGE("Pointer offset is out of bounds");
  if (size > region->size - handle->byte_offset)
    return FFI_ERR_RANGE("FFIPointer %s would exceed the tracked allocation", op);

  return Ant_Value_Undefined;
}

static ant_value_t ffi_pointer_load(ant_t *js, const ffi_pointer_handle_t *handle, ffi_value_type_id_t type) {
  ant_value_t error = ffi_pointer_check(js, handle, ffi_type_table[type].size, "read");
  if (is_err(error)) return error;

  const char *addr = (const char *)ffi_pointer_address_raw(handle);
  if (type != FFI_VALUE_STRING) return ffi_value_from_c(js, addr, type);

  const ffi_pointer_region_t *region = handle->region;
  if (!region->size_known) return js_mkstr(js, addr, strlen(addr));

  const char *nul = memchr(addr, '\0', region->size - handle->byte_offset);
  if (!nul) return FFI_ERR_RANGE("String read exceeded the tracked allocation");

  return js_mkstr(js, addr, (size_t)(nul - addr));
}

static ant_value_t ffi_pointer_store(
  ant_t *js, ant_value_t obj, const ffi_pointer_handle_t *handle,
  ffi_value_type_id_t type, ant_value_t value
) {
  ffi_value_box_t box;
  size_t size = ffi_type_table[type].size;

  ant_value_t error = ffi_value_to_c(js, value, type, &box);
  if (is_err(error)) return error;

  ffi_pointer_sync(js, obj, handle);
  error = ffi_pointer_check(js, handle, size, "write");
  if (is_err(error)) return error;

  memcpy(ffi_pointer_address_raw(handle), &box, size);
  return error;
}

static ant_value_t ffi_pointer_store_string(
  ant_t *js, ant_value_t obj, const ffi_pointer_handle_t *handle, ant_value_t value
) {
  ant_value_t str = js_tostring_val(js, value);
  if (is_err(str)) return str;

  size_t len = 0;
  const char *src = js_getstr(js, str, &len);
  if (!src) return FFI_ERR_TYPE("Invalid string value");

  ffi_pointer_sync(js, obj, handle);
  ant_value_t error = ffi_pointer_check(js, handle, len + 1, "write");
  if (is_err(error)) return error;

  uint8_t *addr = ffi_pointer_address_raw(handle);
  memcpy(addr, src, len);
  addr[len] = '\0';

  return error;
}

static ant_value_t ffi_make_function(
  ant_t *js, ffi_library_handle_t *library,
  const char *symbol_name, ffi_signature_t *signature, void *func_ptr
) {
  ffi_function_handle_t *handle = malloc(sizeof(*handle));
  char *name = strdup(symbol_name);

  if (!handle || !name) {
    free(handle);
    free(name);
    ffi_signature_cleanup(signature);
    return Ant_Error_CreateOutOfMemory(js);
  }

  *handle = (ffi_function_handle_t){
    .library = library,
    .signature = *signature,
    .func_ptr = func_ptr,
    .symbol_name = name,
  };

  if (!signature->variadic && !ffi_prep_signature(&handle->cif, &handle->signature)) {
    ffi_function_destroy(handle);
    return FFI_ERR_TYPE("Failed to prepare FFI call interface");
  }

  ant_value_t obj = FFI_WRAP(function, handle);
  js_set_slot(obj, SLOT_CFUNC, Ant_Function_Create(ffi_function_call));
  js_set_slot_wb(js, obj, SLOT_ENTRIES, library->obj);

  return js_obj_to_func(js, obj);
}

static bool ffi_frame_init(ffi_frame_t *frame, size_t argc) {
  frame->owned_count = 0;
  frame->pending = 0;
  frame->heap = NULL;

  if (argc <= FFI_INLINE_ARGS) {
    frame->values = frame->inline_values;
    frame->avalues = frame->inline_avalues;
    frame->owned = frame->inline_owned;
    frame->atypes = frame->inline_atypes;
    return true;
  }

  frame->heap = malloc(argc * (sizeof(*frame->values) + sizeof(void *) * 2 + sizeof(*frame->atypes)));
  if (!frame->heap) return false;

  frame->values = frame->heap;
  frame->avalues = (void **)(frame->values + argc);
  frame->owned = frame->avalues + argc;
  frame->atypes = (ffi_type **)(frame->owned + argc);

  return true;
}

static void ffi_frame_release(ffi_frame_t *frame) {
  for (size_t i = 0; i < frame->owned_count; i++) free(frame->owned[i]);
  free(frame->heap);
}

static ant_value_t ffi_frame_arg(ant_t *js, ffi_frame_t *frame, size_t i, ant_value_t value, ffi_value_type_id_t type) {
  void *dst = &frame->values[i];
  frame->avalues[i] = dst;
  frame->atypes[i] = ffi_type_table[type].ffi;

  if (type == FFI_VALUE_POINTER || (type == FFI_VALUE_STRING && (ffi_pointer_data(value) || ffi_callback_data(value)))) {
    frame->avalues[i] = NULL;
    frame->pending++;
    return Ant_Value_Undefined;
  }

  if (type != FFI_VALUE_STRING) return ffi_value_to_c(js, value, type, dst);

  char *copy = NULL;
  ant_value_t error = ffi_copy_js_string(js, value, &copy, NULL);
  if (is_err(error)) return error;

  frame->owned[frame->owned_count++] = copy;
  memcpy(dst, &copy, sizeof(copy));

  return error;
}

static ant_value_t ffi_function_invoke(ant_t *js, ffi_function_handle_t *function, ffi_frame_t *frame, size_t argc) {
  const ffi_signature_t *signature = &function->signature;
  if (function->library->closed)
    return FFI_ERR_TYPE("FFIFunction '%s' belongs to a closed library", function->symbol_name);

  ffi_cif variadic_cif;
  ffi_cif *cif = &function->cif;

  if (signature->variadic) {
    cif = &variadic_cif;
    if (ffi_prep_cif_var(
      cif, FFI_DEFAULT_ABI,
      (unsigned int)signature->arg_count, (unsigned int)argc,
      ffi_type_table[signature->returns].ffi, frame->atypes
    ) != FFI_OK) return FFI_ERR_TYPE("Failed to prepare variadic FFI call");
  }

  ffi_value_box_t result = {0};
  ffi_call(cif, function->func_ptr, &result, frame->avalues);
  return ffi_value_from_c(js, &result, signature->returns);
}

ant_value_t ffi_function_call(ant_params_t) {
  ffi_function_handle_t *function = ffi_function_data(call_func);
  if (!function) return ffi_receiver_error(js, ffi_function_label);

  const ffi_signature_t *signature = &function->signature;
  size_t argc = (size_t)nargs;

  if (signature->variadic ? argc < signature->arg_count : argc != signature->arg_count) return FFI_ERR_TYPE(
    "FFIFunction '%s' expects %s%zu arguments, got %d",
    function->symbol_name, signature->variadic ? "at least " : "",
    signature->arg_count, nargs
  );

  ffi_frame_t frame;
  if (!ffi_frame_init(&frame, argc)) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t out = Ant_Value_Undefined;
  for (size_t i = 0; i < argc && !is_err(out); i++) {
    ffi_value_type_id_t type = i < signature->arg_count ? signature->args[i] : ffi_infer_variadic_type(args[i]);
    out = ffi_frame_arg(js, &frame, i, args[i], type);
  }

  for (size_t i = 0; i < argc && frame.pending && !is_err(out); i++) {
    if (frame.avalues[i]) continue;
    frame.avalues[i] = &frame.values[i];
    frame.pending--;
    out = ffi_pointer_to_c(js, args[i], &frame.values[i]);
  }

  if (!is_err(out)) out = ffi_function_invoke(js, function, &frame, argc);
  ffi_frame_release(&frame);

  return out;
}

static void ffi_callback_report(ant_t *js, ant_value_t error, const char *reason) {
  if (is_err(error)) js_take_thrown(js, error);
  fprintf(stderr, "ant:ffi callback %s; returning a zero value\n", reason);
}

#define FFI_CALLBACK_BAIL(error, reason) do { \
  ffi_callback_report(js, (error), (reason)); \
  return;                                     \
} while (0)

static void ffi_callback_trampoline(ffi_cif *cif, void *ret, void **args, void *user_data) {
  ffi_callback_handle_t *callback = user_data;
  if (!callback || callback->closed || !callback->js) return;

  ant_t *js = callback->js;
  const ffi_signature_t *signature = &callback->signature;
  memset(ret, 0, ffi_type_table[signature->returns].size);

  if (!pthread_equal(pthread_self(), callback->owner_thread))
    FFI_CALLBACK_BAIL(Ant_Value_Undefined, "invoked off the JS thread");
  if (gc_object_is_dead(js, js_obj_ptr(callback->owner_obj))) return;

  ant_value_t js_args[FFI_MAX_ARGS];
  for (size_t i = 0; i < signature->arg_count; i++) {
    js_args[i] = ffi_value_from_c(js, args[i], signature->args[i]);
    if (is_err(js_args[i])) FFI_CALLBACK_BAIL(js_args[i], "argument conversion failed");
  }

  ant_value_t fn = js_get_slot(callback->owner_obj, SLOT_DATA);
  if (!is_callable(fn)) FFI_CALLBACK_BAIL(Ant_Value_Undefined, "target is no longer callable");

  ant_value_t result = sv_vm_call(js->vm, js, fn, Ant_Value_Undefined, js_args, (int)signature->arg_count, NULL, Ant_Value_Undefined);
  if (is_err(result)) FFI_CALLBACK_BAIL(result, "threw an exception");
  if (signature->returns == FFI_VALUE_VOID) return;

  ant_value_t error = ffi_value_to_c(js, result, signature->returns, ret);
  if (is_err(error)) FFI_CALLBACK_BAIL(error, "returned an incompatible value");
}

static const ant_cfunc_meta_t ffi_library_methods[] = {
  { ffi_library_define, "define", 2, 0 },
  { ffi_library_call,   "call",   1, 0 },
  { ffi_library_close,  "close",  0, 0 },
};

static const ant_cfunc_meta_t ffi_function_methods[] = {
  { ffi_function_address, "address", 0, 0 },
};

static const ant_cfunc_meta_t ffi_pointer_methods[] = {
  { ffi_pointer_address, "address", 0, 0 },
  { ffi_pointer_is_null, "isNull",  0, 0 },
  { ffi_pointer_read,    "read",    1, 0 },
  { ffi_pointer_write,   "write",   2, 0 },
  { ffi_pointer_offset,  "offset",  1, 0 },
  { ffi_pointer_free,    "free",    0, 0 },
};

static const ant_cfunc_meta_t ffi_callback_methods[] = {
  { ffi_callback_address, "address", 0, 0 },
  { ffi_callback_close,   "close",   0, 0 },
};

static ant_value_t ffi_make_proto(
  ant_t *js, ant_value_t parent, ant_value_t tag,
  const ant_cfunc_meta_t *methods, size_t count
) {
  ant_value_t proto = Ant_Object_CreateBare(js);
  if (is_object_type(parent)) js_set_proto_init(proto, parent);
  js_set_tostringtag(js, proto, tag);
  js_define_methods(js, proto, methods, count, ANT_PROP_ATTR_METHOD);
  return proto;
}

static void ffi_init_prototypes(ant_t *js) {
  if (js->builtins.ffi_library_proto) return;
#define ANT_FFI_KIND(kind, tag, label, parent)                \
  js->builtins.ffi_##kind##_proto = ffi_make_proto(           \
    js, js->sym.parent, Ant_String_FromLiteral(js, label),    \
    ffi_##kind##_methods, countof(ffi_##kind##_methods)       \
  );
#include "ffi/kinds.h"
}

static ant_value_t ffi_dlopen(ant_params_t) {
  if (nargs < 1) return FFI_ERR_TYPE("dlopen(path) requires a library path");
  ffi_init_prototypes(js);

  ant_value_t path_val = js_tostring_val(js, args[0]);
  if (is_err(path_val)) return path_val;

  const char *path = js_getstr(js, path_val, NULL);
  if (!path) return FFI_ERR_TYPE("Invalid library path");

  void *dl = dlopen(path, RTLD_LAZY);
  if (!dl) return FFI_ERR_TYPE("Failed to load library: %s", dlerror());

  ffi_library_handle_t *library = calloc(1, sizeof(*library));
  if (!library) {
    dlclose(dl);
    return Ant_Error_CreateOutOfMemory(js);
  }

  library->handle = dl;
  library->obj = FFI_WRAP(library, library);
  return library->obj;
}

ant_value_t ffi_library_close(ant_params_t) {
  FFI_RECEIVER(library, library);
  ffi_library_close_handle(library);
  return call_this;
}

ant_value_t ffi_library_define(ant_params_t) {
  FFI_RECEIVER(library, library);
  if (library->closed) return FFI_ERR_TYPE("FFILibrary is closed");
  if (nargs < 2 || vtype(args[0]) != kTypeString)
    return FFI_ERR_TYPE("define(name, signature) requires a symbol name and signature");

  ffi_signature_t signature;
  ant_value_t error = ffi_parse_signature(js, args[1], false, &signature);
  if (is_err(error)) return error;
  if (library->closed) return ffi_signature_fail(&signature, FFI_ERR_TYPE("FFILibrary is closed"));

  const char *symbol_name = js_getstr(js, args[0], NULL);
  void *sym = dlsym(library->handle, symbol_name);
  if (!sym) return ffi_signature_fail(&signature, FFI_ERR_TYPE("Symbol '%s' was not found", symbol_name));

  ant_value_t fn = ffi_make_function(js, library, symbol_name, &signature, sym);
  if (is_err(fn)) return fn;

  js_set(js, call_this, ffi_function_data(fn)->symbol_name, fn);
  return fn;
}

ant_value_t ffi_library_call(ant_params_t) {
  FFI_RECEIVER(library, library);
  if (library->closed) return FFI_ERR_TYPE("FFILibrary is closed");
  if (nargs < 1 || vtype(args[0]) != kTypeString) return FFI_ERR_TYPE("call(name, ...args) requires a symbol name");

  ant_value_t fn = js_get(js, call_this, js_getstr(js, args[0], NULL));
  if (!is_callable(fn)) return FFI_ERR_TYPE("Symbol '%s' has not been defined", js_getstr(js, args[0], NULL));

  return sv_vm_call(js->vm, js, fn, Ant_Value_Undefined, args + 1, nargs - 1, NULL, Ant_Value_Undefined);
}

ant_value_t ffi_function_address(ant_params_t) {
  FFI_RECEIVER(function, function);
  return Ant_Value_FromNumber((double)(uintptr_t)function->func_ptr);
}

static ant_value_t ffi_alloc_memory(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeNumber) return FFI_ERR_TYPE("alloc(size) requires a numeric size");

  double size = Ant_Value_AsNumberUnchecked(args[0]);
  if (!(size >= 1 && size <= (double)PTRDIFF_MAX)) return FFI_ERR_RANGE("alloc(size) requires a positive size");

  size_t bytes = (size_t)size;
  void *ptr = malloc(bytes);
  if (!ptr) return Ant_Error_CreateOutOfMemory(js);

  return ffi_pointer_new(js, ptr, bytes, FFI_REGION_SIZED | FFI_REGION_OWNED);
}

static ant_value_t ffi_pointer_value(ant_params_t) {
  ant_value_t value = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (ffi_is_nullish(value)) return ffi_pointer_new(js, NULL, 0, 0);
  if (ffi_pointer_data(value)) return value;

  if (ffi_callback_data(value)) {
    void *ptr = NULL;
    ant_value_t error = ffi_pointer_from_js(js, value, &ptr);
    return is_err(error) ? error : ffi_make_pointer_or_null(js, ptr);
  }

  const uint8_t *bytes = NULL;
  size_t len = 0;

  if (buffer_source_get_bytes(js, value, &bytes, &len)) {
    ant_value_t out = ffi_pointer_new(js, (void *)bytes, len, FFI_REGION_SIZED | FFI_REGION_SOURCED);
    if (!is_err(out)) js_set_slot_wb(js, out, SLOT_DATA, value);
    return out;
  }

  char *copy = NULL;
  ant_value_t error = ffi_copy_js_string(js, value, &copy, &len);
  if (is_err(error)) return error;

  return ffi_pointer_new(js, copy, len + 1, FFI_REGION_SIZED | FFI_REGION_OWNED);
}

ant_value_t ffi_pointer_address(ant_params_t) {
  FFI_POINTER_RECEIVER(handle);
  return Ant_Value_FromNumber((double)(uintptr_t)ffi_pointer_address_raw(handle));
}

ant_value_t ffi_pointer_is_null(ant_params_t) {
  FFI_POINTER_RECEIVER(handle);
  return Ant_Value_FromBool(ffi_pointer_address_raw(handle) == NULL);
}

ant_value_t ffi_pointer_read(ant_params_t) {
  FFI_POINTER_RECEIVER(handle);

  ffi_value_type_id_t type = nargs > 0 ? ffi_type_from_value(js, args[0]) : FFI_VALUE_POINTER;
  if (!ffi_type_is_data(type)) return FFI_ERR_TYPE("Unsupported FFIPointer.read() type");

  return ffi_pointer_load(js, handle, type);
}

ant_value_t ffi_pointer_write(ant_params_t) {
  FFI_RECEIVER(pointer, handle);
  if (nargs < 2) return FFI_ERR_TYPE("FFIPointer.write(type, value) requires a type and value");

  ffi_value_type_id_t type = ffi_type_from_value(js, args[0]);
  if (!ffi_type_is_data(type)) return FFI_ERR_TYPE("Unsupported FFIPointer.write() type");

  ant_value_t error = type == FFI_VALUE_STRING
    ? ffi_pointer_store_string(js, call_this, handle, args[1])
    : ffi_pointer_store(js, call_this, handle, type, args[1]);

  return is_err(error) ? error : call_this;
}

ant_value_t ffi_pointer_offset(ant_params_t) {
  FFI_POINTER_RECEIVER(handle);
  if (nargs < 1 || vtype(args[0]) != kTypeNumber)
    return FFI_ERR_TYPE("FFIPointer.offset(bytes) requires a numeric byte offset");

  double bytes = Ant_Value_AsNumberUnchecked(args[0]);
  if (!(bytes >= 0)) return FFI_ERR_RANGE("FFIPointer.offset() requires a non-negative offset");

  ffi_pointer_region_t *region = handle->region;
  size_t offset = handle->byte_offset + (size_t)bytes;
  if (region->size_known && offset > region->size) return FFI_ERR_RANGE("FFIPointer.offset() is out of bounds");

  ant_value_t out = ffi_make_pointer(js, region, offset);
  if (!is_err(out)) js_set_slot_wb(js, out, SLOT_DATA, js_get_slot(call_this, SLOT_DATA));

  return out;
}

ant_value_t ffi_pointer_free(ant_params_t) {
  FFI_RECEIVER(pointer, handle);
  if (!handle->region->owned) return FFI_ERR_TYPE("Only owned FFIPointers can be freed");
  if (!ffi_pointer_region_free(handle->region)) return FFI_ERR_TYPE("FFIPointer has already been freed");
  return call_this;
}

static ant_value_t ffi_callback_discard(ffi_callback_handle_t *callback, ant_value_t error) {
  ffi_callback_destroy(callback);
  return error;
}

static ant_value_t ffi_create_callback(ant_params_t) {
  if (nargs < 2) return FFI_ERR_TYPE("callback(signature, fn) requires a signature and function");

  bool fn_first = is_callable(args[0]) && is_object_type(args[1]);
  if (!fn_first && !(is_object_type(args[0]) && is_callable(args[1])))
    return FFI_ERR_TYPE("callback(signature, fn) requires a signature object and callable function");

  ant_value_t fn = args[fn_first ? 0 : 1];
  ant_value_t signature_val = args[fn_first ? 1 : 0];

  ffi_init_prototypes(js);
  ffi_callback_handle_t *callback = calloc(1, sizeof(*callback));
  if (!callback) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t error = ffi_parse_signature(js, signature_val, true, &callback->signature);
  if (is_err(error)) return ffi_callback_discard(callback, error);

  if (!ffi_prep_signature(&callback->cif, &callback->signature))
    return ffi_callback_discard(callback, FFI_ERR_TYPE("Failed to prepare FFICallback signature"));

  callback->closure = ffi_closure_alloc(sizeof(*callback->closure), &callback->code_ptr);
  if (!callback->closure) return ffi_callback_discard(callback, js_mkerr(js, "Failed to allocate FFICallback closure"));

  if (ffi_prep_closure_loc(
    callback->closure, &callback->cif,
    ffi_callback_trampoline, callback, callback->code_ptr
  ) != FFI_OK) return ffi_callback_discard(callback, FFI_ERR_TYPE("Failed to prepare FFICallback closure"));

  callback->js = js;
  callback->owner_thread = pthread_self();
  callback->owner_obj = FFI_WRAP(callback, callback);
  js_set_slot_wb(js, callback->owner_obj, SLOT_DATA, fn);

  return callback->owner_obj;
}

ant_value_t ffi_callback_address(ant_params_t) {
  FFI_RECEIVER(callback, callback);
  return Ant_Value_FromNumber((double)(uintptr_t)callback->code_ptr);
}

ant_value_t ffi_callback_close(ant_params_t) {
  FFI_RECEIVER(callback, callback);
  ffi_callback_close_handle(callback);
  return call_this;
}

static const ant_cfunc_meta_t ffi_exports[] = {
  { ffi_dlopen,          "dlopen",   1, 0 },
  { ffi_alloc_memory,    "alloc",    1, 0 },
  { ffi_pointer_value,   "pointer",  1, 0 },
  { ffi_create_callback, "callback", 2, 0 },
};

ant_value_t ffi_library(ant_t *js) {
  ffi_init_prototypes(js);

  ant_value_t ffi_obj = Ant_Object_CreateBare(js);
  js_define_methods(js, ffi_obj, ffi_exports, countof(ffi_exports), ANT_PROP_ATTR_DEFAULT);
  js_set(js, ffi_obj, "suffix", Ant_String_FromLiteral(js, FFI_LIBRARY_SUFFIX));

  ant_value_t ffi_types = Ant_Object_CreateBare(js);
#define ANT_FFI_TYPE(id, key, name, ffi, ctype, kind) js_set(js, ffi_types, #key, Ant_String_FromLiteral(js, name));
#include "ffi/types.h"

  js_set(js, ffi_obj, "FFIType", ffi_types);
  js_set_tostringtag(js, ffi_obj, Ant_String_FromLiteral(js, "FFI"));

  return ffi_obj;
}
