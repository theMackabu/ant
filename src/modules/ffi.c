#ifdef _WIN32
#include <windows.h>
#define dlopen(name, flags) ((void*)LoadLibraryA(name))
#define dlsym(handle, name) ((void*)GetProcAddress((HMODULE)(handle), (name)))
#define dlclose(handle) FreeLibrary((HMODULE)(handle))
#define dlerror() "LoadLibrary failed"
#define RTLD_LAZY 0
#else
#include <dlfcn.h>
#endif
#include <ffi.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <utarray.h>
#include <uthash.h>

#include "errors.h"
#include "internal.h"

#include "modules/ffi.h"
#include "modules/symbol.h"

typedef struct ffi_lib {
  char name[256];
  void *handle;
  jsval_t js_obj;
  UT_hash_handle hh;
} ffi_lib_t;

typedef struct ffi_func {
  char name[256];
  void *func_ptr;
  ffi_cif cif;
  ffi_type **arg_types;
  ffi_type *ret_type;
  char ret_type_str[32];
  int arg_count;
  bool is_variadic;
  UT_hash_handle hh;
} ffi_func_t;

typedef struct ffi_ptr {
  void *ptr;
  size_t size;
  bool is_managed;
  uint64_t ptr_key;
  UT_hash_handle hh;
} ffi_ptr_t;

typedef struct ffi_callback {
  ffi_closure *closure;
  void *code_ptr;
  ffi_cif cif;
  ffi_type **arg_types;
  ffi_type *ret_type;
  char ret_type_str[32];
  char **arg_type_strs;
  int arg_count;
  struct js *js;
  jsval_t js_func;
  uint64_t cb_key;
  UT_hash_handle hh;
} ffi_callback_t;

static ffi_lib_t *ffi_libraries = NULL;
static ffi_ptr_t *ffi_pointers = NULL;
static ffi_callback_t *ffi_callbacks = NULL;
static UT_array *ffi_functions_array = NULL;

static pthread_mutex_t ffi_libraries_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ffi_functions_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ffi_pointers_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ffi_callbacks_mutex = PTHREAD_MUTEX_INITIALIZER;

static const UT_icd ffi_func_icd = {
  .sz = sizeof(ffi_func_t *),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

static jsval_t ffi_dlopen(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_define(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_lib_call(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_call_function(struct js *js, ffi_func_t *func, jsval_t *args, int nargs);
static jsval_t ffi_alloc_memory(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_free_memory(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_read_memory(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_write_memory(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_get_pointer(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_create_callback(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_free_callback(struct js *js, jsval_t *args, int nargs);
static jsval_t ffi_read_ptr(struct js *js, jsval_t *args, int nargs);

static ffi_type *get_ffi_type(const char *type_str);
static void *js_to_ffi_value(struct js *js, jsval_t val, ffi_type *type, void *buffer);
static jsval_t ffi_to_js_value(struct js *js, void *val, ffi_type *type, const char *type_str);
static void ffi_callback_handler(ffi_cif *cif, void *ret, void **args, void *user_data);

jsval_t ffi_library(struct js *js) {
  jsval_t ffi_obj = js_mkobj(js);

  js_set(js, ffi_obj, "dlopen", js_mkfun(ffi_dlopen));
  js_set(js, ffi_obj, "alloc", js_mkfun(ffi_alloc_memory));
  js_set(js, ffi_obj, "free", js_mkfun(ffi_free_memory));
  js_set(js, ffi_obj, "read", js_mkfun(ffi_read_memory));
  js_set(js, ffi_obj, "write", js_mkfun(ffi_write_memory));
  js_set(js, ffi_obj, "pointer", js_mkfun(ffi_get_pointer));
  js_set(js, ffi_obj, "callback", js_mkfun(ffi_create_callback));
  js_set(js, ffi_obj, "freeCallback", js_mkfun(ffi_free_callback));
  js_set(js, ffi_obj, "readPtr", js_mkfun(ffi_read_ptr));

  const char *suffix;
#ifdef __APPLE__
  suffix = "dylib";
#elif defined(__linux__)
  suffix = "so";
#elif defined(_WIN32)
  suffix = "dll";
#else
  suffix = "so";
#endif
  js_set(js, ffi_obj, "suffix", js_mkstr(js, suffix, strlen(suffix)));

  jsval_t ffi_types = js_mkobj(js);
  js_set(js, ffi_types, "void", js_mkstr(js, "void", 4));
  js_set(js, ffi_types, "int8", js_mkstr(js, "int8", 4));
  js_set(js, ffi_types, "int16", js_mkstr(js, "int16", 5));
  js_set(js, ffi_types, "int", js_mkstr(js, "int", 3));
  js_set(js, ffi_types, "int64", js_mkstr(js, "int64", 5));
  js_set(js, ffi_types, "uint8", js_mkstr(js, "uint8", 5));
  js_set(js, ffi_types, "uint16", js_mkstr(js, "uint16", 6));
  js_set(js, ffi_types, "uint64", js_mkstr(js, "uint64", 6));
  js_set(js, ffi_types, "float", js_mkstr(js, "float", 5));
  js_set(js, ffi_types, "double", js_mkstr(js, "double", 6));
  js_set(js, ffi_types, "pointer", js_mkstr(js, "pointer", 7));
  js_set(js, ffi_types, "string", js_mkstr(js, "string", 6));
  js_set(js, ffi_types, "spread", js_mkstr(js, "...", 3));
  js_set(js, ffi_obj, "FFIType", ffi_types);
  js_set(js, ffi_obj, get_toStringTag_sym_key(), js_mkstr(js, "FFI", 3));

  return ffi_obj;
}

static void ffi_init_array(void) {
  static int initialized = 0;
  if (initialized)
    return;
  initialized = 1;
  if (!ffi_functions_array)
    utarray_new(ffi_functions_array, &ffi_func_icd);
}

static jsval_t ffi_dlopen(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "dlopen() requires library name string");
  }

  ffi_init_array();

  size_t lib_name_len;
  const char *lib_name = js_getstr(js, args[0], &lib_name_len);

  pthread_mutex_lock(&ffi_libraries_mutex);

  ffi_lib_t *lib = NULL;
  HASH_FIND_STR(ffi_libraries, lib_name, lib);
  if (lib) {
    jsval_t result = lib->js_obj;
    pthread_mutex_unlock(&ffi_libraries_mutex);
    return result;
  }

  pthread_mutex_unlock(&ffi_libraries_mutex);

  void *handle = dlopen(lib_name, RTLD_LAZY);
  if (!handle) {
    return js_mkerr(js, "Failed to load library: %s", dlerror());
  }

  lib = (ffi_lib_t *)malloc(sizeof(ffi_lib_t));
  if (!lib) {
    dlclose(handle);
    return js_mkerr(js, "Out of memory");
  }

  strncpy(lib->name, lib_name, sizeof(lib->name) - 1);
  lib->name[sizeof(lib->name) - 1] = '\0';
  lib->handle = handle;

  lib->js_obj = js_mkobj(js);
  js_set(js, lib->js_obj, "__lib_ptr", js_mknum((double)(uint64_t)lib));
  js_set(js, lib->js_obj, "define", js_mkfun(ffi_define));
  js_set(js, lib->js_obj, "call", js_mkfun(ffi_lib_call));

  pthread_mutex_lock(&ffi_libraries_mutex);
  HASH_ADD_STR(ffi_libraries, name, lib);
  pthread_mutex_unlock(&ffi_libraries_mutex);

  return lib->js_obj;
}

static jsval_t ffi_define(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "define() requires function name string and signature");
  }

  jsval_t this_obj = js_getthis(js);
  jsval_t lib_ptr_val = js_get(js, this_obj, "__lib_ptr");
  if (vtype(lib_ptr_val) != T_NUM) {
    return js_mkerr(js, "Invalid library object");
  }

  size_t func_name_len;
  const char *func_name = js_getstr(js, args[0], &func_name_len);

  jsval_t sig = args[1];
  int sig_type = vtype(sig);
  if (sig_type == T_STR || sig_type == T_NUM || sig_type == T_NULL ||
      sig_type == T_UNDEF) {
    return js_mkerr(js,
                    "Signature must be an array [returnType, [argTypes...]] or "
                    "an object {args: [...], returns: type}");
  }

  const char *ret_type_str;
  jsval_t arg_types_arr;
  int arg_count;

  jsval_t returns_val = js_get(js, sig, "returns");
  jsval_t args_val = js_get(js, sig, "args");

  if (vtype(returns_val) != T_UNDEF && vtype(args_val) != T_UNDEF) {
    if (vtype(returns_val) != T_STR) {
      return js_mkerr(js, "Return type must be a string");
    }
    ret_type_str = js_getstr(js, returns_val, NULL);
    arg_types_arr = args_val;

    if (vtype(arg_types_arr) == T_STR || vtype(arg_types_arr) == T_NUM ||
        vtype(arg_types_arr) == T_NULL ||
        vtype(arg_types_arr) == T_UNDEF) {
      return js_mkerr(js, "Argument types must be an array");
    }

    jsval_t length_val = js_get(js, arg_types_arr, "length");
    arg_count = (int)js_getnum(length_val);
  } else {
    jsval_t ret_type_val = js_get(js, sig, "0");
    if (vtype(ret_type_val) != T_STR) {
      return js_mkerr(js, "Return type must be a string");
    }

    ret_type_str = js_getstr(js, ret_type_val, NULL);
    arg_types_arr = js_get(js, sig, "1");

    int arg_arr_type = vtype(arg_types_arr);
    if (arg_arr_type == T_STR || arg_arr_type == T_NUM ||
        arg_arr_type == T_NULL || arg_arr_type == T_UNDEF) {
      return js_mkerr(js, "Argument types must be an array");
    }

    jsval_t length_val = js_get(js, arg_types_arr, "length");
    arg_count = (int)js_getnum(length_val);
  }

  ffi_type *ret_type = get_ffi_type(ret_type_str);
  if (!ret_type) {
    return js_mkerr(js, "Unknown return type: %s", ret_type_str);
  }

  ffi_lib_t *lib = (ffi_lib_t *)(uint64_t)js_getnum(lib_ptr_val);
  void *func_ptr = dlsym(lib->handle, func_name);
  if (!func_ptr) {
    return js_mkerr(js, "Function '%s' not found", func_name);
  }

  ffi_func_t *func = (ffi_func_t *)malloc(sizeof(ffi_func_t));
  if (!func) {
    return js_mkerr(js, "Out of memory");
  }

  strncpy(func->name, func_name, sizeof(func->name) - 1);
  func->name[sizeof(func->name) - 1] = '\0';
  func->func_ptr = func_ptr;
  func->ret_type = ret_type;
  strncpy(func->ret_type_str, ret_type_str, sizeof(func->ret_type_str) - 1);
  func->ret_type_str[sizeof(func->ret_type_str) - 1] = '\0';
  func->arg_count = arg_count;
  func->is_variadic = false;

  if (arg_count > 0) {
    func->arg_types = (ffi_type **)malloc(sizeof(ffi_type *) * arg_count);
    if (!func->arg_types) {
      free(func);
      return js_mkerr(js, "Out of memory");
    }

    for (int i = 0; i < arg_count; i++) {
      char idx_str[16];
      snprintf(idx_str, sizeof(idx_str), "%d", i);
      jsval_t arg_type_val = js_get(js, arg_types_arr, idx_str);

      if (vtype(arg_type_val) != T_STR) {
        free(func->arg_types);
        free(func);
        return js_mkerr(js, "Argument type must be a string");
      }

      const char *arg_type_str = js_getstr(js, arg_type_val, NULL);

      if (strcmp(arg_type_str, "...") == 0) {
        func->is_variadic = true;
        func->arg_count = i;
        break;
      }

      func->arg_types[i] = get_ffi_type(arg_type_str);
      if (!func->arg_types[i]) {
        free(func->arg_types);
        free(func);
        return js_mkerr(js, "Unknown argument type: %s", arg_type_str);
      }
    }
  } else {
    func->arg_types = NULL;
  }

  if (!func->is_variadic) {
    ffi_status status =
        ffi_prep_cif(&func->cif, FFI_DEFAULT_ABI, func->arg_count, ret_type, func->arg_types);
    if (status != FFI_OK) {
      if (func->arg_types)
        free(func->arg_types);
      free(func);
      return js_mkerr(js, "Failed to prepare function call (status=%d, argc=%d)", status, func->arg_count);
    }
  }

  pthread_mutex_lock(&ffi_functions_mutex);
  utarray_push_back(ffi_functions_array, &func);
  unsigned int func_index = utarray_len(ffi_functions_array) - 1;
  pthread_mutex_unlock(&ffi_functions_mutex);

  js_set(js, this_obj, func_name, js_mkffi(func_index));

  return js_mkundef();
}

static jsval_t ffi_lib_call(struct js *js, jsval_t *args, int nargs) {
  jsval_t lib_obj = js_getthis(js);
  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr(js, "call() requires function name string");

  size_t func_name_len;
  const char *func_name = js_getstr(js, args[0], &func_name_len);

  jsval_t ffi_val = js_get(js, lib_obj, func_name);
  int func_index = js_getffi(ffi_val);
  if (func_index < 0) {
    return js_mkerr(js, "Function '%s' not defined", func_name);
  }

  return ffi_call_by_index(js, (unsigned int)func_index, args + 1, nargs - 1);
}

static jsval_t ffi_call_function(struct js *js, ffi_func_t *func, jsval_t *args,int nargs) {
  if (!func->is_variadic && nargs != func->arg_count) {
    return js_mkerr(js, "Function '%s' expects %d arguments, got %d", func->name, func->arg_count, nargs);
  }

  if (func->is_variadic && nargs < func->arg_count) {
    return js_mkerr(js, "Function '%s' expects at least %d arguments, got %d", func->name, func->arg_count, nargs);
  }

#define MAX_ARGS 32
  void *arg_values_buf[MAX_ARGS];
  ffi_arg arg_buffers_buf[MAX_ARGS];

  int actual_arg_count = func->is_variadic ? nargs : func->arg_count;

  if (actual_arg_count > MAX_ARGS) {
    return js_mkerr(js, "Too many arguments");
  }

  void **arg_values = arg_values_buf;
  memset(arg_values, 0, sizeof(arg_values_buf));

  for (int i = 0; i < actual_arg_count; i++) {
    arg_values[i] = &arg_buffers_buf[i];
    memset(arg_values[i], 0, sizeof(ffi_arg));

    ffi_type *arg_type;
    if (i < func->arg_count) {
      arg_type = func->arg_types[i];
    } else arg_type = &ffi_type_sint32;

    js_to_ffi_value(js, args[i], arg_type, arg_values[i]);
  }

  ffi_arg result;
  memset(&result, 0, sizeof(result));

  if (func->is_variadic) {
#define MAX_VARIADIC_ARGS 32
    ffi_type *all_arg_types[MAX_VARIADIC_ARGS];

    if (actual_arg_count > MAX_VARIADIC_ARGS) {
      return js_mkerr(js, "Too many variadic arguments");
    }

    for (int i = 0; i < func->arg_count; i++) all_arg_types[i] = func->arg_types[i];
    for (int i = func->arg_count; i < actual_arg_count; i++) all_arg_types[i] = &ffi_type_sint32;

    ffi_cif call_cif;
    ffi_status status =
        ffi_prep_cif_var(&call_cif, FFI_DEFAULT_ABI, func->arg_count,
                         actual_arg_count, func->ret_type, all_arg_types);

    if (status != FFI_OK) {
      return js_mkerr(js, "Failed to prepare variadic call CIF (status=%d)", status);
    }

    ffi_call(&call_cif, func->func_ptr, &result, arg_values);
  } else {
    ffi_call(&func->cif, func->func_ptr, &result, arg_values);
  }

  return ffi_to_js_value(js, &result, func->ret_type, func->ret_type_str);
}

jsval_t ffi_call_by_index(struct js *js, unsigned int func_index, jsval_t *args, int nargs) {
  pthread_mutex_lock(&ffi_functions_mutex);
  if (func_index >= utarray_len(ffi_functions_array)) {
    pthread_mutex_unlock(&ffi_functions_mutex);
    return js_mkerr(js, "Invalid FFI function index");
  }
  
  ffi_func_t *func = *(ffi_func_t **)utarray_eltptr(ffi_functions_array, func_index);
  pthread_mutex_unlock(&ffi_functions_mutex);
  
  return ffi_call_function(js, func, args, nargs);
}

static jsval_t ffi_alloc_memory(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_NUM) {
    return js_mkerr(js, "alloc() requires size");
  }

  size_t size = (size_t)js_getnum(args[0]);
  if (size == 0) {
    return js_mkerr(js, "alloc() requires non-zero size");
  }

  void *ptr = malloc(size);
  if (!ptr) {
    return js_mkerr(js, "alloc() failed to allocate memory");
  }

  ffi_ptr_t *ffi_ptr = (ffi_ptr_t *)malloc(sizeof(ffi_ptr_t));
  if (!ffi_ptr) {
    free(ptr);
    return js_mkerr(js, "Out of memory");
  }

  ffi_ptr->ptr = ptr;
  ffi_ptr->size = size;
  ffi_ptr->is_managed = true;
  ffi_ptr->ptr_key = (uint64_t)ptr;

  pthread_mutex_lock(&ffi_pointers_mutex);
  HASH_ADD(hh, ffi_pointers, ptr_key, sizeof(uint64_t), ffi_ptr);
  pthread_mutex_unlock(&ffi_pointers_mutex);

  return js_mknum((double)ffi_ptr->ptr_key);
}

static jsval_t ffi_free_memory(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_NUM) {
    return js_mkerr(js, "free() requires pointer");
  }

  uint64_t ptr_key = (uint64_t)js_getnum(args[0]);

  pthread_mutex_lock(&ffi_pointers_mutex);
  ffi_ptr_t *ffi_ptr = NULL;
  HASH_FIND(hh, ffi_pointers, &ptr_key, sizeof(uint64_t), ffi_ptr);

  if (!ffi_ptr) {
    pthread_mutex_unlock(&ffi_pointers_mutex);
    return js_mkerr(js, "Invalid pointer");
  }

  if (ffi_ptr->is_managed) {
    free(ffi_ptr->ptr);
  }

  HASH_DEL(ffi_pointers, ffi_ptr);
  free(ffi_ptr);
  pthread_mutex_unlock(&ffi_pointers_mutex);

  return js_mkundef();
}

static jsval_t ffi_read_memory(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_NUM || vtype(args[1]) != T_STR) {
    return js_mkerr(js, "read() requires pointer and type");
  }

  uint64_t ptr_key = (uint64_t)js_getnum(args[0]);

  pthread_mutex_lock(&ffi_pointers_mutex);
  ffi_ptr_t *ffi_ptr = NULL;
  HASH_FIND(hh, ffi_pointers, &ptr_key, sizeof(uint64_t), ffi_ptr);

  if (!ffi_ptr) {
    pthread_mutex_unlock(&ffi_pointers_mutex);
    return js_mkerr(js, "Invalid pointer");
  }

  void *ptr = ffi_ptr->ptr;
  pthread_mutex_unlock(&ffi_pointers_mutex);

  const char *type_str = js_getstr(js, args[1], NULL);
  ffi_type *type = get_ffi_type(type_str);
  if (!type) {
    return js_mkerr(js, "Unknown type: %s", type_str);
  }

  return ffi_to_js_value(js, ptr, type, type_str);
}

static jsval_t ffi_write_memory(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 3 || vtype(args[0]) != T_NUM || vtype(args[1]) != T_STR) {
    return js_mkerr(js, "write() requires pointer, type, and value");
  }

  uint64_t ptr_key = (uint64_t)js_getnum(args[0]);

  pthread_mutex_lock(&ffi_pointers_mutex);
  ffi_ptr_t *ffi_ptr = NULL;
  HASH_FIND(hh, ffi_pointers, &ptr_key, sizeof(uint64_t), ffi_ptr);

  if (!ffi_ptr) {
    pthread_mutex_unlock(&ffi_pointers_mutex);
    return js_mkerr(js, "Invalid pointer");
  }

  void *ptr = ffi_ptr->ptr;
  pthread_mutex_unlock(&ffi_pointers_mutex);

  const char *type_str = js_getstr(js, args[1], NULL);
  ffi_type *type = get_ffi_type(type_str);
  if (!type) {
    return js_mkerr(js, "Unknown type: %s", type_str);
  }

  js_to_ffi_value(js, args[2], type, ptr);

  return js_mkundef();
}

static jsval_t ffi_get_pointer(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_NUM) return js_mkerr(js, "pointer() requires pointer");
  uint64_t ptr_key = (uint64_t)js_getnum(args[0]);

  pthread_mutex_lock(&ffi_pointers_mutex);
  ffi_ptr_t *ffi_ptr = NULL;
  HASH_FIND(hh, ffi_pointers, &ptr_key, sizeof(uint64_t), ffi_ptr);
  bool exists = ffi_ptr != NULL;
  pthread_mutex_unlock(&ffi_pointers_mutex);

  return exists ? js_mktrue() : js_mkfalse();
}

static jsval_t ffi_read_ptr(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != T_NUM || vtype(args[1]) != T_STR) {
    return js_mkerr(js, "readPtr() requires pointer and type");
  }

  void *ptr = (void *)(uint64_t)js_getnum(args[0]);
  if (!ptr) return js_mknull();

  const char *type_str = js_getstr(js, args[1], NULL);

  if (strcmp(type_str, "string") == 0) {
    const char *str = (const char *)ptr;
    return js_mkstr(js, str, strlen(str));
  }

  ffi_type *type = get_ffi_type(type_str);
  if (!type) return js_mkerr(js, "Unknown type: %s", type_str);

  return ffi_to_js_value(js, ptr, type, type_str);
}

static void ffi_callback_handler(ffi_cif *cif, void *ret, void **args, void *user_data) {
  (void)cif;
  ffi_callback_t *cb = (ffi_callback_t *)user_data;
  struct js *js = cb->js;

  jsval_t js_args[32];
  int arg_count = cb->arg_count > 32 ? 32 : cb->arg_count;

  for (int i = 0; i < arg_count; i++) {
    js_args[i] = ffi_to_js_value(js, args[i], cb->arg_types[i], cb->arg_type_strs[i]);
  }

  jsval_t result = js_call(js, cb->js_func, js_args, arg_count);

  if (cb->ret_type != &ffi_type_void) {
    if (cb->ret_type == &ffi_type_sint8) {
      *(int8_t *)ret = (int8_t)js_getnum(result);
    } else if (cb->ret_type == &ffi_type_sint16) {
      *(int16_t *)ret = (int16_t)js_getnum(result);
    } else if (cb->ret_type == &ffi_type_sint32) {
      *(int32_t *)ret = (int32_t)js_getnum(result);
    } else if (cb->ret_type == &ffi_type_sint64) {
      *(int64_t *)ret = (int64_t)js_getnum(result);
    } else if (cb->ret_type == &ffi_type_uint8) {
      *(uint8_t *)ret = (uint8_t)js_getnum(result);
    } else if (cb->ret_type == &ffi_type_uint16) {
      *(uint16_t *)ret = (uint16_t)js_getnum(result);
    } else if (cb->ret_type == &ffi_type_uint64) {
      *(uint64_t *)ret = (uint64_t)js_getnum(result);
    } else if (cb->ret_type == &ffi_type_float) {
      *(float *)ret = (float)js_getnum(result);
    } else if (cb->ret_type == &ffi_type_double) {
      *(double *)ret = js_getnum(result);
    } else if (cb->ret_type == &ffi_type_pointer) {
      if (vtype(result) == T_NUM) {
        *(void **)ret = (void *)(uint64_t)js_getnum(result);
      } else {
        *(void **)ret = NULL;
      }
    }
  }
}

static jsval_t ffi_create_callback(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "callback() requires function and signature");

  jsval_t js_func = args[0];
  jsval_t sig = args[1];
  int sig_type = vtype(sig);
  if (sig_type == T_STR || sig_type == T_NUM || sig_type == T_NULL || sig_type == T_UNDEF) {
    return js_mkerr(js, "Signature must be an object {args: [...], returns: type}");
  }

  const char *ret_type_str;
  jsval_t arg_types_arr;
  int arg_count;

  jsval_t returns_val = js_get(js, sig, "returns");
  jsval_t args_val = js_get(js, sig, "args");

  if (vtype(returns_val) != T_UNDEF && vtype(args_val) != T_UNDEF) {
    if (vtype(returns_val) != T_STR) return js_mkerr(js, "Return type must be a string");
    ret_type_str = js_getstr(js, returns_val, NULL);
    arg_types_arr = args_val;
    jsval_t length_val = js_get(js, arg_types_arr, "length");
    arg_count = (int)js_getnum(length_val);
  } else {
    jsval_t ret_type_val = js_get(js, sig, "0");
    if (vtype(ret_type_val) != T_STR) return js_mkerr(js, "Return type must be a string");
    ret_type_str = js_getstr(js, ret_type_val, NULL);
    arg_types_arr = js_get(js, sig, "1");
    jsval_t length_val = js_get(js, arg_types_arr, "length");
    arg_count = (int)js_getnum(length_val);
  }

  ffi_type *ret_type = get_ffi_type(ret_type_str);
  if (!ret_type) return js_mkerr(js, "Unknown return type: %s", ret_type_str);

  ffi_callback_t *cb = (ffi_callback_t *)malloc(sizeof(ffi_callback_t));
  if (!cb) return js_mkerr(js, "Out of memory");

  cb->js = js;
  cb->js_func = js_func;
  cb->ret_type = ret_type;
  strncpy(cb->ret_type_str, ret_type_str, sizeof(cb->ret_type_str) - 1);
  cb->ret_type_str[sizeof(cb->ret_type_str) - 1] = '\0';
  cb->arg_count = arg_count;

  if (arg_count > 0) {
    cb->arg_types = (ffi_type **)malloc(sizeof(ffi_type *) * arg_count);
    cb->arg_type_strs = (char **)malloc(sizeof(char *) * arg_count);
    if (!cb->arg_types || !cb->arg_type_strs) {
      if (cb->arg_types) free(cb->arg_types);
      if (cb->arg_type_strs) free(cb->arg_type_strs);
      free(cb);
      return js_mkerr(js, "Out of memory");
    }

    for (int i = 0; i < arg_count; i++) {
      char idx_str[16];
      snprintf(idx_str, sizeof(idx_str), "%d", i);
      jsval_t arg_type_val = js_get(js, arg_types_arr, idx_str);

      if (vtype(arg_type_val) != T_STR) {
        for (int j = 0; j < i; j++) free(cb->arg_type_strs[j]);
        free(cb->arg_types);
        free(cb->arg_type_strs);
        free(cb);
        return js_mkerr(js, "Argument type must be a string");
      }

      const char *arg_type_str = js_getstr(js, arg_type_val, NULL);
      cb->arg_types[i] = get_ffi_type(arg_type_str);
      if (!cb->arg_types[i]) {
        for (int j = 0; j < i; j++) free(cb->arg_type_strs[j]);
        free(cb->arg_types);
        free(cb->arg_type_strs);
        free(cb);
        return js_mkerr(js, "Unknown argument type: %s", arg_type_str);
      }
      cb->arg_type_strs[i] = strdup(arg_type_str);
    }
  } else {
    cb->arg_types = NULL;
    cb->arg_type_strs = NULL;
  }

  ffi_status status = ffi_prep_cif(&cb->cif, FFI_DEFAULT_ABI, arg_count, ret_type, cb->arg_types);
  if (status != FFI_OK) {
    for (int i = 0; i < arg_count; i++) free(cb->arg_type_strs[i]);
    if (cb->arg_types) free(cb->arg_types);
    if (cb->arg_type_strs) free(cb->arg_type_strs);
    free(cb);
    return js_mkerr(js, "Failed to prepare callback CIF (status=%d)", status);
  }

  cb->closure = (ffi_closure *)ffi_closure_alloc(sizeof(ffi_closure), &cb->code_ptr);
  if (!cb->closure) {
    for (int i = 0; i < arg_count; i++) free(cb->arg_type_strs[i]);
    if (cb->arg_types) free(cb->arg_types);
    if (cb->arg_type_strs) free(cb->arg_type_strs);
    free(cb);
    return js_mkerr(js, "Failed to allocate closure");
  }

  status = ffi_prep_closure_loc(cb->closure, &cb->cif, ffi_callback_handler, cb, cb->code_ptr);
  if (status != FFI_OK) {
    ffi_closure_free(cb->closure);
    for (int i = 0; i < arg_count; i++) free(cb->arg_type_strs[i]);
    if (cb->arg_types) free(cb->arg_types);
    if (cb->arg_type_strs) free(cb->arg_type_strs);
    free(cb);
    return js_mkerr(js, "Failed to prepare closure (status=%d)", status);
  }

  cb->cb_key = (uint64_t)cb->code_ptr;

  pthread_mutex_lock(&ffi_callbacks_mutex);
  HASH_ADD(hh, ffi_callbacks, cb_key, sizeof(uint64_t), cb);
  pthread_mutex_unlock(&ffi_callbacks_mutex);

  return js_mknum((double)cb->cb_key);
}

static jsval_t ffi_free_callback(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_NUM) {
    return js_mkerr(js, "freeCallback() requires callback pointer");
  }

  uint64_t cb_key = (uint64_t)js_getnum(args[0]);

  pthread_mutex_lock(&ffi_callbacks_mutex);
  ffi_callback_t *cb = NULL;
  HASH_FIND(hh, ffi_callbacks, &cb_key, sizeof(uint64_t), cb);

  if (!cb) {
    pthread_mutex_unlock(&ffi_callbacks_mutex);
    return js_mkerr(js, "Invalid callback pointer");
  }

  HASH_DEL(ffi_callbacks, cb);
  pthread_mutex_unlock(&ffi_callbacks_mutex);

  ffi_closure_free(cb->closure);
  for (int i = 0; i < cb->arg_count; i++) free(cb->arg_type_strs[i]);
  if (cb->arg_types) free(cb->arg_types);
  if (cb->arg_type_strs) free(cb->arg_type_strs);
  free(cb);

  return js_mkundef();
}

typedef enum {
  JS_FFI_VOID = 0,
  JS_FFI_INT8,
  JS_FFI_INT16,
  JS_FFI_INT,
  JS_FFI_INT64,
  JS_FFI_UINT8,
  JS_FFI_UINT16,
  JS_FFI_UINT64,
  JS_FFI_FLOAT,
  JS_FFI_DOUBLE,
  JS_FFI_POINTER,
  JS_FFI_STRING,
  JS_FFI_UNKNOWN,
  JS_FFI_COUNT
} js_ffi_type_id;

static js_ffi_type_id get_ffi_type_id(const char *type_str) {
  if (!type_str) return JS_FFI_UNKNOWN;

  switch (type_str[0]) {
    case 'v': if (strcmp(type_str, "void") == 0) return JS_FFI_VOID; break;
    case 'i':
      if (type_str[1] == 'n' && type_str[2] == 't') {
        if (type_str[3] == '\0') return JS_FFI_INT;
        if (strcmp(type_str + 3, "8") == 0) return JS_FFI_INT8;
        if (strcmp(type_str + 3, "16") == 0) return JS_FFI_INT16;
        if (strcmp(type_str + 3, "64") == 0) return JS_FFI_INT64;
      }
      break;
    case 'u':
      if (type_str[1] == 'i' && type_str[2] == 'n' && type_str[3] == 't') {
        if (strcmp(type_str + 4, "8") == 0) return JS_FFI_UINT8;
        if (strcmp(type_str + 4, "16") == 0) return JS_FFI_UINT16;
        if (strcmp(type_str + 4, "64") == 0) return JS_FFI_UINT64;
      }
      break;
    case 'f': if (strcmp(type_str, "float") == 0) return JS_FFI_FLOAT; break;
    case 'd': if (strcmp(type_str, "double") == 0) return JS_FFI_DOUBLE; break;
    case 'p': if (strcmp(type_str, "pointer") == 0) return JS_FFI_POINTER; break;
    case 's': if (strcmp(type_str, "string") == 0) return JS_FFI_STRING; break;
  }
  return JS_FFI_UNKNOWN;
}

static ffi_type *get_ffi_type(const char *type_str) {
  static ffi_type *type_map[] = {
    [JS_FFI_VOID]    = &ffi_type_void,
    [JS_FFI_INT8]    = &ffi_type_sint8,
    [JS_FFI_INT16]   = &ffi_type_sint16,
    [JS_FFI_INT]     = &ffi_type_sint32,
    [JS_FFI_INT64]   = &ffi_type_sint64,
    [JS_FFI_UINT8]   = &ffi_type_uint8,
    [JS_FFI_UINT16]  = &ffi_type_uint16,
    [JS_FFI_UINT64]  = &ffi_type_uint64,
    [JS_FFI_FLOAT]   = &ffi_type_float,
    [JS_FFI_DOUBLE]  = &ffi_type_double,
    [JS_FFI_POINTER] = &ffi_type_pointer,
    [JS_FFI_STRING]  = &ffi_type_pointer,
    [JS_FFI_UNKNOWN] = NULL,
  };

  js_ffi_type_id id = get_ffi_type_id(type_str);
  return type_map[id];
}

static void *js_to_ffi_value(struct js *js, jsval_t val, ffi_type *type, void *buffer) {
  static const void *dispatch[] = {
    &&do_sint8, &&do_sint16, &&do_sint32, &&do_sint64,
    &&do_uint8, &&do_uint16, &&do_uint64,
    &&do_float, &&do_double, &&do_pointer, &&do_done
  };

  int idx;
  if (type == &ffi_type_sint8)        idx = 0;
  else if (type == &ffi_type_sint16)  idx = 1;
  else if (type == &ffi_type_sint32)  idx = 2;
  else if (type == &ffi_type_sint64)  idx = 3;
  else if (type == &ffi_type_uint8)   idx = 4;
  else if (type == &ffi_type_uint16)  idx = 5;
  else if (type == &ffi_type_uint64)  idx = 6;
  else if (type == &ffi_type_float)   idx = 7;
  else if (type == &ffi_type_double)  idx = 8;
  else if (type == &ffi_type_pointer) idx = 9;
  else                                idx = 10;

  goto *dispatch[idx];

do_sint8: {
    int8_t v = (int8_t)js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_sint16: {
    int16_t v = (int16_t)js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_sint32: {
    int32_t v = (int32_t)js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_sint64: {
    int64_t v = (int64_t)js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_uint8: {
    uint8_t v = (uint8_t)js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_uint16: {
    uint16_t v = (uint16_t)js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_uint64: {
    uint64_t v = (uint64_t)js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_float: {
    float v = (float)js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_double: {
    double v = js_getnum(val);
    memcpy(buffer, &v, sizeof(v));
    goto do_done;
  }
do_pointer: {
    if (vtype(val) == T_STR) {
      size_t str_len;
      const char *str = js_getstr(js, val, &str_len);
      void *ptr = (void *)str;
      memcpy(buffer, &ptr, sizeof(ptr));
    } else {
      void *ptr = (void *)(uint64_t)js_getnum(val);
      memcpy(buffer, &ptr, sizeof(ptr));
    }
    goto do_done;
  }
do_done:
  return buffer;
}

static jsval_t ffi_to_js_value(struct js *js, void *val, ffi_type *type, const char *type_str) {
  static const void *dispatch[] = {
    &&ret_void, &&ret_sint8, &&ret_sint16, &&ret_sint32, &&ret_sint64,
    &&ret_uint8, &&ret_uint16, &&ret_uint64,
    &&ret_float, &&ret_double, &&ret_pointer, &&ret_undef
  };

  int idx;
  if (type == &ffi_type_void)         idx = 0;
  else if (type == &ffi_type_sint8)   idx = 1;
  else if (type == &ffi_type_sint16)  idx = 2;
  else if (type == &ffi_type_sint32)  idx = 3;
  else if (type == &ffi_type_sint64)  idx = 4;
  else if (type == &ffi_type_uint8)   idx = 5;
  else if (type == &ffi_type_uint16)  idx = 6;
  else if (type == &ffi_type_uint64)  idx = 7;
  else if (type == &ffi_type_float)   idx = 8;
  else if (type == &ffi_type_double)  idx = 9;
  else if (type == &ffi_type_pointer) idx = 10;
  else                                idx = 11;

  goto *dispatch[idx];

ret_void:
  return js_mkundef();
ret_sint8:
  return js_mknum((double)(*(int8_t *)val));
ret_sint16:
  return js_mknum((double)(*(int16_t *)val));
ret_sint32:
  return js_mknum((double)(*(int32_t *)val));
ret_sint64:
  return js_mknum((double)(*(int64_t *)val));
ret_uint8:
  return js_mknum((double)(*(uint8_t *)val));
ret_uint16:
  return js_mknum((double)(*(uint16_t *)val));
ret_uint64:
  return js_mknum((double)(*(uint64_t *)val));
ret_float:
  return js_mknum((double)(*(float *)val));
ret_double:
  return js_mknum((double)(*(double *)val));
ret_pointer: {
    void *ptr = *(void **)val;
    if (type_str && strcmp(type_str, "string") == 0 && ptr) {
      const char *str = (const char *)ptr;
      if (str && strlen(str) < 1024) return js_mkstr(js, str, strlen(str));
    }
    return js_mknum((double)(uint64_t)ptr);
  }
ret_undef:
  return js_mkundef();
}

void ffi_gc_update_roots(GC_OP_VAL_ARGS) {
  pthread_mutex_lock(&ffi_callbacks_mutex);
  ffi_callback_t *cb, *tmp;
  HASH_ITER(hh, ffi_callbacks, cb, tmp) {
    op_val(ctx, &cb->js_func);
  }
  pthread_mutex_unlock(&ffi_callbacks_mutex);
}
