#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ffi.h>
#include <stdint.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "ant.h"
#include "ptr.h"
#include "internal.h"
#include "modules/bigint.h"
#include "modules/cjit.h"

#ifdef ANT_JIT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmacro-redefined"
#include "c2mir/c2mir.h"
#include "mir-gen.h"
#pragma GCC diagnostic pop
#endif

#ifdef ANT_JIT
typedef struct {
  const char *data;
  size_t len;
  size_t offset;
} ant_c_source_t;

typedef enum {
  ANT_C_RETURNS_STATUS,
  ANT_C_RETURNS_STRING,
  ANT_C_RETURNS_NUMBER,
} ant_c_returns_t;

typedef enum {
  ANT_C_ARG_INT8,
  ANT_C_ARG_UINT8,
  ANT_C_ARG_INT16,
  ANT_C_ARG_UINT16,
  ANT_C_ARG_INT32,
  ANT_C_ARG_UINT32,
  ANT_C_ARG_INT64,
  ANT_C_ARG_UINT64,
  ANT_C_ARG_FLOAT,
  ANT_C_ARG_DOUBLE,
} ant_c_arg_type_t;

typedef union {
  int8_t i8;
  uint8_t u8;
  int16_t i16;
  uint16_t u16;
  int32_t i32;
  uint32_t u32;
  int64_t i64;
  uint64_t u64;
  float f32;
  double f64;
  const char *string;
  ffi_arg raw;
} ant_c_arg_value_t;

typedef struct {
  const char *name;
  ant_c_arg_type_t type;
  ffi_type *ffi;
  MIR_type_t mir;
} ant_c_arg_mapping_t;

static constexpr uint32_t ANT_C_FUNCTION_NATIVE_TAG = 0x434a4954u;
static constexpr size_t ANT_C_MAX_ARGS = 16;

typedef struct {
  const char *entry_name;
  const char *returns_name;
  ant_c_returns_t returns;
  ant_c_arg_type_t return_type;
  ffi_type *ffi_return_type;
  MIR_type_t mir_return_type;
  bool return_function;
  size_t arg_count;
  ant_c_arg_type_t arg_types[ANT_C_MAX_ARGS];
  ffi_type *ffi_arg_types[ANT_C_MAX_ARGS];
  MIR_type_t mir_arg_types[ANT_C_MAX_ARGS];
} ant_c_signature_t;

typedef struct {
  MIR_context_t ctx;
  MIR_item_t entry_item;
  ffi_cif cif;
  ffi_type **ffi_arg_types;
  ant_c_arg_type_t *arg_types;
  ant_c_returns_t returns;
  ant_c_arg_type_t return_type;
  size_t arg_count;
  char *entry_name;
} ant_c_function_t;

static const ant_c_arg_mapping_t ANT_C_ARG_MAPPINGS[] = {
  {"int8",   ANT_C_ARG_INT8,   &ffi_type_sint8,  MIR_T_I8},
  {"uint8",  ANT_C_ARG_UINT8,  &ffi_type_uint8,  MIR_T_U8},
  {"int16",  ANT_C_ARG_INT16,  &ffi_type_sint16, MIR_T_I16},
  {"uint16", ANT_C_ARG_UINT16, &ffi_type_uint16, MIR_T_U16},
  {"int",    ANT_C_ARG_INT32,  &ffi_type_sint32, MIR_T_I32},
  {"int32",  ANT_C_ARG_INT32,  &ffi_type_sint32, MIR_T_I32},
  {"uint32", ANT_C_ARG_UINT32, &ffi_type_uint32, MIR_T_U32},
  {"int64",  ANT_C_ARG_INT64,  &ffi_type_sint64, MIR_T_I64},
  {"uint64", ANT_C_ARG_UINT64, &ffi_type_uint64, MIR_T_U64},
  {"float",  ANT_C_ARG_FLOAT,  &ffi_type_float,  MIR_T_F},
  {"double", ANT_C_ARG_DOUBLE, &ffi_type_double, MIR_T_D},
};

static bool ant_c_append(
  char **buffer, size_t *length, size_t *capacity,
  const char *data, size_t data_length
) {
  if (data_length > SIZE_MAX - *length - 1) return false;
  size_t required = *length + data_length + 1;
  
  if (required > *capacity) {
    size_t next_capacity = *capacity ? *capacity : 256;
    while (next_capacity < required) {
      if (next_capacity > SIZE_MAX / 2) {
        next_capacity = required;
        break;
      }
      next_capacity *= 2;
    }
    
    char *next = realloc(*buffer, next_capacity);
    if (!next) return false;
    *buffer = next;
    *capacity = next_capacity;
  }

  memcpy(*buffer + *length, data, data_length);
  *length += data_length;
  (*buffer)[*length] = '\0';
  
  return true;
}

static ant_value_t ant_c_template_source(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_ARR)
    return js_mkerr(js, "Ant.unsafe.c must be used as a tagged template");

  ant_value_t raw = js_get(js, args[0], "raw");
  if (is_err(raw)) return raw;
  if (vtype(raw) != T_ARR)
    return js_mkerr(js, "Ant.unsafe.c must be used as a tagged template");

  ant_offset_t literal_count = js_arr_len(js, raw);
  if ((size_t)literal_count != (size_t)nargs)
    return js_mkerr(js, "Ant.unsafe.c received an invalid template object");

  char *buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  
  for (ant_offset_t i = 0; i < literal_count; i++) {
    ant_value_t literal = js_arr_get(js, raw, i);
    if (vtype(literal) != T_STR) {
      free(buffer);
      return js_mkerr(js, "Ant.unsafe.c received an invalid template object");
    }

    size_t part_length;
    const char *part = js_getstr(js, literal, &part_length);
    if (!ant_c_append(&buffer, &length, &capacity, part, part_length)) {
      free(buffer);
      return js_mkerr(js, "Out of memory");
    }

    if (i + 1 >= literal_count) continue;
    ant_value_t value = js_template_to_string(js, args[i + 1]);
    if (is_err(value)) {
      free(buffer);
      return value;
    }
    part = js_getstr(js, value, &part_length);
    if (!ant_c_append(&buffer, &length, &capacity, part, part_length)) {
      free(buffer);
      return js_mkerr(js, "Out of memory");
    }
  }

  ant_value_t source = js_mkstr(js, buffer ? buffer : "", length);
  free(buffer);
  
  return source;
}

static int ant_c_getc(void *data) {
  ant_c_source_t *source = data;
  if (source->offset >= source->len) return EOF;
  return (unsigned char)source->data[source->offset++];
}

static void *ant_c_import_resolver(const char *name) {
#ifdef _WIN32
  const char *modules[] = {NULL, "ucrtbase.dll", "msvcrt.dll", "kernel32.dll"};
  for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
    HMODULE module = modules[i] ? GetModuleHandleA(modules[i]) : GetModuleHandleA(NULL);
    if (!module) continue;
    FARPROC symbol = GetProcAddress(module, name);
    if (symbol) return (void *)(uintptr_t)symbol;
  }
  return NULL;
#else
  return dlsym(RTLD_DEFAULT, name);
#endif
}

static char *ant_c_read_diagnostics(FILE *file) {
  if (!file || fflush(file) != 0 || fseek(file, 0, SEEK_END) != 0) return NULL;
  long size = ftell(file);
  if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) return NULL;

  char *diagnostics = malloc((size_t)size + 1);
  if (!diagnostics) return NULL;
  size_t read = fread(diagnostics, 1, (size_t)size, file);

  diagnostics[read] = '\0';
  return diagnostics;
}

static void ant_c_load_modules(MIR_context_t ctx) {
  for (
    MIR_module_t module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx)); 
    module != NULL; module = DLIST_NEXT(MIR_module_t, module)
  ) MIR_load_module(ctx, module);
}

static bool ant_c_pointer_sized_type(MIR_type_t type) {
#if MIR_PTR64
  return type == MIR_T_I64 || type == MIR_T_U64;
#else
  return type == MIR_T_I32 || type == MIR_T_U32;
#endif
}

static bool ant_c_identifier(const char *name, size_t length) {
  if (length == 0) return false;
  unsigned char first = (unsigned char)name[0];
  
  if (
    first != '_' && (first < 'A' || first > 'Z') && 
    (first < 'a' || first > 'z')
  ) return false;

  for (size_t i = 1; i < length; i++) {
    unsigned char c = (unsigned char)name[i];
    if (
      c != '_' && (c < 'A' || c > 'Z') && 
      (c < 'a' || c > 'z') && 
      (c < '0' || c > '9')
    ) return false;
  }
  
  return true;
}

static char *ant_c_string_checked_source(
  const char *source, size_t source_length,
  const ant_c_signature_t *signature, size_t *checked_length
) {
  static const char prefix[] = "\n_Static_assert(_Generic((";
  static const char call_start[] = ")(";
  
  static const char suffix[] =
    "), char *: 1, default: 0), "
    "\"Ant.unsafe.c() string entry must return char *\");\n";

  char *buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;

  if (!ant_c_append(&buffer, &length, &capacity, source, source_length)
    || !ant_c_append(&buffer, &length, &capacity, prefix, sizeof(prefix) - 1)
    || !ant_c_append(
      &buffer, &length, &capacity,
      signature->entry_name, strlen(signature->entry_name)
    ) || !ant_c_append(&buffer, &length, &capacity, call_start, sizeof(call_start) - 1)) goto oom;

  size_t arg_count = signature->return_function ? signature->arg_count : 0;
  for (size_t i = 0; i < arg_count; i++) 
    if ((i > 0 && !ant_c_append(&buffer, &length, &capacity, ", ", 2)) || 
    !ant_c_append(&buffer, &length, &capacity, "0", 1)) goto oom;

  if (!ant_c_append(&buffer, &length, &capacity, suffix, sizeof(suffix) - 1)) goto oom;
  *checked_length = length;
  
  return buffer;

oom:
  free(buffer);
  return NULL;
}

static bool ant_c_parse_type(
  const char *name, ant_c_arg_type_t *type,
  ffi_type **ffi_type_out, MIR_type_t *mir_type_out
) {
  for (size_t i = 0; i < sizeof(ANT_C_ARG_MAPPINGS) / sizeof(ANT_C_ARG_MAPPINGS[0]); i++) {
    const ant_c_arg_mapping_t *mapping = &ANT_C_ARG_MAPPINGS[i];
    if (strcmp(name, mapping->name) != 0) continue;
    if (type) *type = mapping->type;
    if (ffi_type_out) *ffi_type_out = mapping->ffi;
    if (mir_type_out) *mir_type_out = mapping->mir;
    return true;
  }
  return false;
}

static ant_value_t ant_c_parse_signature(
  ant_t *js, ant_value_t options_value, ant_c_signature_t *signature
) {
  *signature = (ant_c_signature_t){
    .entry_name = "main",
    .returns_name = "int",
    .returns = ANT_C_RETURNS_STATUS,
    .return_type = ANT_C_ARG_INT32,
    .ffi_return_type = &ffi_type_sint32,
    .mir_return_type = MIR_T_I32,
  };
  
  if (vtype(options_value) == T_UNDEF) return js_mkundef();
  if (!is_object_type(options_value))
    return js_mkerr(js, "Ant.unsafe.c() options must be an object");

  ant_value_t entry_value = js_get(js, options_value, "entry");
  if (is_err(entry_value)) return entry_value;
  if (vtype(entry_value) != T_STR)
    return js_mkerr(js, "Ant.unsafe.c() options.entry must be a string");

  size_t entry_length;
  signature->entry_name = js_getstr(js, entry_value, &entry_length);
  if (!ant_c_identifier(signature->entry_name, entry_length))
    return js_mkerr(js, "Ant.unsafe.c() options.entry must be a C identifier");

  ant_value_t returns_value = js_get(js, options_value, "returns");
  if (is_err(returns_value)) return returns_value;
  if (vtype(returns_value) != T_STR)
    return js_mkerr(js, "Ant.unsafe.c() options.returns must be a supported type");

  signature->returns_name = js_getstr(js, returns_value, NULL);
  if (strcmp(signature->returns_name, "string") == 0) {
    signature->returns = ANT_C_RETURNS_STRING;
    signature->ffi_return_type = &ffi_type_pointer;
  } else {
    signature->returns = ANT_C_RETURNS_NUMBER;
    if (!ant_c_parse_type(
      signature->returns_name, &signature->return_type,
      &signature->ffi_return_type, &signature->mir_return_type
    )) return js_mkerr(js, "Ant.unsafe.c() options.returns has an unsupported type");
  }

  ant_value_t args_value = js_get(js, options_value, "args");
  if (is_err(args_value) || vtype(args_value) == T_UNDEF) return args_value;
  if (!is_object_type(args_value))
    return js_mkerr(js, "Ant.unsafe.c() options.args must be an array");

  ant_value_t length_value = js_get(js, args_value, "length");
  if (is_err(length_value)) return length_value;
  if (vtype(length_value) != T_NUM)
    return js_mkerr(js, "Ant.unsafe.c() options.args must be an array");

  double length = js_getnum(length_value);
  if (length < 0 || length > (double)ANT_C_MAX_ARGS || length != (double)(size_t)length)
    return js_mkerr(
      js, "Ant.unsafe.c() options.args supports at most %zu arguments", ANT_C_MAX_ARGS
    );

  signature->arg_count = (size_t)length;
  signature->return_function = true;
  
  for (size_t i = 0; i < signature->arg_count; i++) {
    char index[24];
    snprintf(index, sizeof(index), "%zu", i);
    ant_value_t type_value = js_get(js, args_value, index);
    
    if (is_err(type_value)) return type_value;
    if (vtype(type_value) != T_STR || !ant_c_parse_type(
      js_getstr(js, type_value, NULL), &signature->arg_types[i],
      &signature->ffi_arg_types[i], &signature->mir_arg_types[i]
    )) return js_mkerr(js, "Ant.unsafe.c() options.args[%zu] has an unsupported type", i);
  }
  return js_mkundef();
}

static bool ant_c_signature_matches(
  MIR_func_t entry_func, const ant_c_signature_t *signature
) {
  if (signature->returns == ANT_C_RETURNS_STATUS) return
    (entry_func->nargs == 0 || entry_func->nargs == 2 || entry_func->nargs == 3)
    && !entry_func->vararg_p && entry_func->nres == 1
    && entry_func->res_types[0] == MIR_T_I32;

  size_t arg_count = signature->return_function ? signature->arg_count : 0;
  if (entry_func->nargs != arg_count || entry_func->vararg_p || entry_func->nres != 1) return false;
  
  for (size_t i = 0; i < arg_count; i++)
    if (VARR_GET(MIR_var_t, entry_func->vars, i).type != signature->mir_arg_types[i]) return false;

  return signature->returns == ANT_C_RETURNS_STRING
    ? ant_c_pointer_sized_type(entry_func->res_types[0])
    : entry_func->res_types[0] == signature->mir_return_type;
}

static ant_value_t ant_c_signature_error(
  ant_t *js, const ant_c_signature_t *signature
) {
  size_t arg_count = signature->return_function ? signature->arg_count : 0;
  if (signature->returns == ANT_C_RETURNS_STRING) return js_mkerr(
    js, "Ant.unsafe.c() string entry \"%s\" must return char * and match %zu configured arguments",
    signature->entry_name, arg_count
  );
  if (signature->returns == ANT_C_RETURNS_NUMBER) return js_mkerr(
    js, "Ant.unsafe.c() entry \"%s\" must match configured signature with return type \"%s\" and %zu arguments",
    signature->entry_name, signature->returns_name, arg_count
  );
  return js_mkerr(js, "Ant.unsafe.c() main must return int and accept 0, 2, or 3 arguments");
}

static ant_value_t ant_c_result_to_js(
  ant_t *js, ant_c_returns_t returns,
  ant_c_arg_type_t return_type, const ant_c_arg_value_t *result
) {
  if (returns == ANT_C_RETURNS_STRING) return result->string
    ? js_mkstr(js, result->string, strlen(result->string))
    : js_mknull();

  switch (return_type) {
    case ANT_C_ARG_INT8: return js_mknum((double)result->i8);
    case ANT_C_ARG_UINT8: return js_mknum((double)result->u8);
    case ANT_C_ARG_INT16: return js_mknum((double)result->i16);
    case ANT_C_ARG_UINT16: return js_mknum((double)result->u16);
    case ANT_C_ARG_INT32: return js_mknum((double)result->i32);
    case ANT_C_ARG_UINT32: return js_mknum((double)result->u32);
    case ANT_C_ARG_INT64: return bigint_from_int64(js, result->i64);
    case ANT_C_ARG_UINT64: return bigint_from_uint64(js, result->u64);
    case ANT_C_ARG_FLOAT: return js_mknum((double)result->f32);
    case ANT_C_ARG_DOUBLE: return js_mknum(result->f64);
  }

  return js_mkerr(js, "Ant.unsafe.c() has an unsupported return type");
}

static void ant_c_function_destroy(ant_c_function_t *function) {
  if (!function) return;
  MIR_gen_finish(function->ctx);
  c2mir_finish(function->ctx);
  MIR_finish(function->ctx);
  free(function->ffi_arg_types);
  free(function->arg_types);
  free(function->entry_name);
  free(function);
}

static void ant_c_function_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_c_function_t *function = js_get_native(value, ANT_C_FUNCTION_NATIVE_TAG);
  if (!function) return;
  ant_c_function_destroy(function);
  js_clear_native(value, ANT_C_FUNCTION_NATIVE_TAG);
}

static ant_value_t ant_c_function_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_c_function_t *function = js_get_native(js->current_func, ANT_C_FUNCTION_NATIVE_TAG);
  if (!function) return js_mkerr(js, "Ant.unsafe.c() compiled function is no longer available");
  
  if (nargs != (int)function->arg_count) return js_mkerr(js, 
    "Ant.unsafe.c() entry \"%s\" expects %zu arguments, got %d", 
    function->entry_name, function->arg_count, nargs
  );

  ant_c_arg_value_t values[ANT_C_MAX_ARGS];
  void *ffi_args[ANT_C_MAX_ARGS];
  memset(values, 0, sizeof(values));

  for (size_t i = 0; i < function->arg_count; i++) {
    ffi_args[i] = &values[i];

    if (function->arg_types[i] == ANT_C_ARG_INT64) {
      if (vtype(args[i]) != T_BIGINT
        || !bigint_to_int64_wrapping(js, args[i], &values[i].i64)) return js_mkerr(
        js, "Ant.unsafe.c() entry \"%s\" argument %zu must be a bigint",
        function->entry_name, i + 1
      );
      continue;
    }

    if (function->arg_types[i] == ANT_C_ARG_UINT64) {
      if (vtype(args[i]) != T_BIGINT
        || !bigint_to_uint64_wrapping(js, args[i], &values[i].u64)) return js_mkerr(
        js, "Ant.unsafe.c() entry \"%s\" argument %zu must be a bigint",
        function->entry_name, i + 1
      );
      continue;
    }

    if (vtype(args[i]) != T_NUM) return js_mkerr(
      js, "Ant.unsafe.c() entry \"%s\" argument %zu must be a number",
      function->entry_name, i + 1
    );

    double number = js_getnum(args[i]);
    switch (function->arg_types[i]) {
      case ANT_C_ARG_INT8: values[i].i8 = (int8_t)number; break;
      case ANT_C_ARG_UINT8: values[i].u8 = (uint8_t)number; break;
      case ANT_C_ARG_INT16: values[i].i16 = (int16_t)number; break;
      case ANT_C_ARG_UINT16: values[i].u16 = (uint16_t)number; break;
      case ANT_C_ARG_INT32: values[i].i32 = (int32_t)number; break;
      case ANT_C_ARG_UINT32: values[i].u32 = (uint32_t)number; break;
      case ANT_C_ARG_INT64:
      case ANT_C_ARG_UINT64: break;
      case ANT_C_ARG_FLOAT: values[i].f32 = (float)number; break;
      case ANT_C_ARG_DOUBLE: values[i].f64 = number; break;
    }
  }

  ant_c_arg_value_t result = {0};
  
  ffi_call(
    &function->cif, function->entry_item->addr, &result,
    function->arg_count == 0 ? NULL : ffi_args
  );

  return ant_c_result_to_js(js, function->returns, function->return_type, &result);
}

static ant_value_t ant_c_make_function(
  ant_t *js, MIR_context_t ctx, MIR_item_t entry_item,
  const char *entry_name, ant_c_returns_t returns,
  ant_c_arg_type_t return_type, ffi_type *ffi_return_type, size_t arg_count,
  ant_c_arg_type_t *arg_types, ffi_type **ffi_arg_types
) {
  ant_c_function_t *function = calloc(1, sizeof(*function));
  
  if (!function) {
    MIR_gen_finish(ctx);
    c2mir_finish(ctx);
    MIR_finish(ctx);
    return js_mkerr(js, "Out of memory");
  }

  function->ctx = ctx;
  function->entry_item = entry_item;
  function->returns = returns;
  function->return_type = return_type;
  function->arg_count = arg_count;
  function->entry_name = strdup(entry_name);
  
  if (arg_count > 0) {
    function->arg_types = malloc(arg_count * sizeof(*function->arg_types));
    function->ffi_arg_types = malloc(arg_count * sizeof(*function->ffi_arg_types));
  }
  
  if (!function->entry_name || (arg_count > 0 && (!function->arg_types || !function->ffi_arg_types))) {
    ant_c_function_destroy(function);
    return js_mkerr(js, "Out of memory");
  }
  
  if (arg_count > 0) {
    memcpy(function->arg_types, arg_types, arg_count * sizeof(*arg_types));
    memcpy(function->ffi_arg_types, ffi_arg_types, arg_count * sizeof(*ffi_arg_types));
  }

  if (ffi_prep_cif(
    &function->cif, FFI_DEFAULT_ABI, (unsigned int)arg_count, 
    ffi_return_type, function->ffi_arg_types) != FFI_OK
  ) {
    ant_c_function_destroy(function);
    return js_mkerr(js, "Ant.unsafe.c() failed to prepare the entry call interface");
  }

  ant_value_t obj = js_newobj(js);
  js_set_slot(obj, SLOT_CFUNC, js_mkfun(ant_c_function_call));
  js_set_native(obj, function, ANT_C_FUNCTION_NATIVE_TAG);
  js_set_finalizer(obj, ant_c_function_finalize);
  
  return js_obj_to_func(js, obj);
}

static ant_value_t ant_c_call_configured_entry(
  ant_t *js, MIR_item_t entry_item, const ant_c_signature_t *signature
) {
  ffi_cif cif;
  if (ffi_prep_cif(
    &cif, FFI_DEFAULT_ABI, 0, signature->ffi_return_type, NULL
  ) != FFI_OK) return js_mkerr(
    js, "Ant.unsafe.c() failed to prepare the entry call interface"
  );

  ant_c_arg_value_t native_result = {0};
  ffi_call(&cif, entry_item->addr, &native_result, NULL);
  return ant_c_result_to_js(
    js, signature->returns, signature->return_type, &native_result
  );
}

static ant_value_t ant_c_call_status_entry(
  ant_t *js, MIR_item_t entry_item, MIR_func_t entry_func
) {
  if (entry_func->nargs == 0) {
    int (*entry)(void) = (int (*)(void))entry_item->addr;
    return js_mknum((double)entry());
  }

  char *entry_argv[] = {"Ant.unsafe.c", NULL};
  if (entry_func->nargs == 2) {
    int (*entry)(int, char **) = (int (*)(int, char **))entry_item->addr;
    return js_mknum((double)entry(1, entry_argv));
  }

  int (*entry)(int, char **, char **) = (int (*)(int, char **, char **))entry_item->addr;
  return js_mknum((double)entry(1, entry_argv, NULL));
}

static ant_value_t ant_c_compile(ant_t *js, ant_value_t source_value, ant_value_t options_value) {
  if (vtype(source_value) != T_STR)
    return js_mkerr(js, "Ant.unsafe.c must be used as a tagged template");

  ant_c_signature_t signature;
  ant_value_t signature_error = ant_c_parse_signature(js, options_value, &signature);
  if (is_err(signature_error)) return signature_error;

  size_t source_len;
  const char *source_data = js_getstr(js, source_value, &source_len);
  char *checked_source = NULL;
  
  if (signature.returns == ANT_C_RETURNS_STRING) {
    checked_source = ant_c_string_checked_source(
      source_data, source_len, &signature, &source_len
    );
    if (!checked_source) return js_mkerr(js, "Out of memory");
    source_data = checked_source;
  }
  
  ant_c_source_t source = {.data = source_data, .len = source_len, .offset = 0};
  FILE *diagnostic_file = tmpfile();

  MIR_context_t ctx = MIR_init();
  c2mir_init(ctx);

  struct c2mir_options options = {0};
  options.message_file = diagnostic_file ? diagnostic_file : stderr;
  int compiled = c2mir_compile(ctx, &options, ant_c_getc, &source, "<Ant.unsafe.c>", NULL);
  free(checked_source);

  if (!compiled) {
    char *diagnostics = ant_c_read_diagnostics(diagnostic_file);
    c2mir_finish(ctx);
    MIR_finish(ctx);
    if (diagnostic_file) fclose(diagnostic_file);

    ant_value_t error = diagnostics && diagnostics[0]
      ? js_mkerr(js, "Ant.unsafe.c() compilation failed:\n%s", diagnostics)
      : js_mkerr(js, "Ant.unsafe.c() compilation failed");

    free(diagnostics);
    return error;
  }

  ant_c_load_modules(ctx);
  MIR_item_t entry_item = MIR_get_global_item(ctx, signature.entry_name);

  if (!entry_item || entry_item->item_type != MIR_func_item) {
    c2mir_finish(ctx);
    MIR_finish(ctx);
    if (diagnostic_file) fclose(diagnostic_file);
    return signature.returns == ANT_C_RETURNS_STATUS
      ? js_mkerr(js, "Ant.unsafe.c() source must define main()")
      : js_mkerr(
        js, "Ant.unsafe.c() entry \"%s\" was not found", signature.entry_name
      );
  }

  MIR_func_t entry_func = MIR_get_item_func(ctx, entry_item);
  if (!ant_c_signature_matches(entry_func, &signature)) {
    c2mir_finish(ctx);
    MIR_finish(ctx);
    if (diagnostic_file) fclose(diagnostic_file);
    return ant_c_signature_error(js, &signature);
  }

  MIR_gen_init(ctx);
  MIR_gen_set_optimize_level(ctx, 1);
  MIR_link(ctx, MIR_set_gen_interface, ant_c_import_resolver);

  if (signature.return_function) {
    if (diagnostic_file) fclose(diagnostic_file);
    return ant_c_make_function(
      js, ctx, entry_item, signature.entry_name, signature.returns,
      signature.return_type, signature.ffi_return_type, signature.arg_count,
      signature.arg_types, signature.ffi_arg_types
    );
  }

  ant_value_t result = signature.returns == ANT_C_RETURNS_STATUS
    ? ant_c_call_status_entry(js, entry_item, entry_func)
    : ant_c_call_configured_entry(js, entry_item, &signature);

  MIR_gen_finish(ctx);
  c2mir_finish(ctx);
  MIR_finish(ctx);
  if (diagnostic_file) fclose(diagnostic_file);
  return result;
}

static ant_value_t ant_c_configured_tag(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = ant_c_template_source(js, args, nargs);
  if (is_err(source)) return source;
  return ant_c_compile(js, source, js_get_slot(js->current_func, SLOT_DATA));
}

static ant_value_t js_unsafe_c(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs >= 1 && vtype(args[0]) == T_ARR) {
    ant_value_t source = ant_c_template_source(js, args, nargs);
    if (is_err(source)) return source;
    return ant_c_compile(js, source, js_mkundef());
  }

  if (nargs == 1 && is_object_type(args[0])) {
    ant_value_t tag = js_newobj(js);
    js_set_slot(tag, SLOT_CFUNC, js_mkfun(ant_c_configured_tag));
    js_set_slot_wb(js, tag, SLOT_DATA, args[0]);
    return js_obj_to_func(js, tag);
  }

  return js_mkerr(js, "Ant.unsafe.c must be used as a tagged template or called with options");
}
#else
static ant_value_t js_unsafe_c(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr(js, "Ant.unsafe.c() requires a JIT-enabled Ant build");
}
#endif

void init_cjit_module(ant_t *js, ant_value_t unsafe_obj) {
  js_set(js, unsafe_obj, "c", js_mkfun_arity(js_unsafe_c, 1));
}
