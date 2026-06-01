#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <wasm_c_api.h>
#include <wasm_export.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundef"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#include <wasm_c_api_internal.h>
#include <wasm_runtime.h>
#pragma clang diagnostic pop

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "gc/modules.h"
#include "silver/engine.h"
#include "modules/buffer.h"
#include "modules/wasm.h"
#include "modules/wasi.h"

typedef struct {
  wasm_store_t *store;
  wasm_module_t *module;
  uint8_t *bytes;
  size_t bytes_len;
} wasm_module_handle_t;

typedef struct {
  wasm_instance_t *instance;
  wasm_extern_vec_t exports;
  wasm_func_t **host_funcs;
  size_t host_func_count;
  wasm_global_t **host_globals;
  size_t host_global_count;
} wasm_instance_handle_t;

typedef enum {
  WASM_EXTERN_WRAP_GLOBAL = 1,
  WASM_EXTERN_WRAP_MEMORY,
  WASM_EXTERN_WRAP_TABLE,
} wasm_extern_wrap_kind_t;

typedef struct {
  wasm_extern_wrap_kind_t kind;
  wasm_store_t *store;
  bool own_handle;
  bool use_cached_value;
  bool standalone_table;
  wasm_valkind_t standalone_table_element;
  ant_value_t memory_buffer;
  wasm_val_t cached_value;
  uint32_t standalone_table_size;
  uint32_t standalone_table_max_size;
  union {
    wasm_global_t *global;
    wasm_memory_t *memory;
    wasm_table_t *table;
  } as;
} wasm_extern_handle_t;

typedef struct {
  ant_t *js;
  wasm_store_t *store;
  ant_value_t owner;
  ant_value_t fn;
} wasm_import_func_env_t;

typedef struct {
  wasm_func_t *func;
  bool own_func;
} wasm_func_handle_t;

typedef struct {
  WASMModuleInstanceCommon *runtime_instance;
  ant_value_t owner;
} wasm_instance_owner_t;

enum {
  WASM_FUNC_STATE_TAG = 0x57465354u, // WFST
  WASM_MODULE_NATIVE_TAG = 0x574d4f44u, // WMOD
  WASM_INSTANCE_NATIVE_TAG = 0x57494e53u, // WINS
  WASM_EXTERN_NATIVE_TAG = 0x57455854u // WEXT
};

static size_t g_wasm_import_env_count = 0;
static size_t g_wasm_import_env_cap   = 0;

static ant_value_t g_wasm_module_proto    = 0;
static ant_value_t g_wasm_instance_proto  = 0;
static ant_value_t g_wasm_global_proto    = 0;
static ant_value_t g_wasm_memory_proto    = 0;
static ant_value_t g_wasm_table_proto     = 0;
static ant_value_t g_wasm_tag_proto       = 0;
static ant_value_t g_wasm_exception_proto = 0;

static ant_value_t g_wasm_compileerror_proto   = 0;
static ant_value_t g_wasm_linkerror_proto      = 0;
static ant_value_t g_wasm_runtimeerror_proto   = 0;
static ant_value_t g_wasm_pending_import_throw = 0;

static wasm_engine_t *g_wasm_engine                = NULL;
static wasm_import_func_env_t **g_wasm_import_envs = NULL;
static bool g_wasm_pending_import_throw_exists     = false;
static wasm_instance_owner_t *g_wasm_instance_owners = NULL;
static size_t g_wasm_instance_owner_count = 0;
static size_t g_wasm_instance_owner_cap = 0;

static void wasm_clear_pending_import_throw(void) {
  g_wasm_pending_import_throw_exists = false;
  g_wasm_pending_import_throw = js_mkundef();
}

static void wasm_set_pending_import_throw(ant_value_t value) {
  g_wasm_pending_import_throw_exists = true;
  g_wasm_pending_import_throw = value;
}

static ant_value_t wasm_consume_pending_import_throw(void) {
  ant_value_t value = g_wasm_pending_import_throw_exists
    ? g_wasm_pending_import_throw
    : js_mkundef();
  wasm_clear_pending_import_throw();
  return value;
}

static void wasm_register_import_env(wasm_import_func_env_t *env) {
  if (g_wasm_import_env_count == g_wasm_import_env_cap) {
    size_t new_cap = g_wasm_import_env_cap ? g_wasm_import_env_cap * 2 : 16;
    wasm_import_func_env_t **new_arr = realloc(g_wasm_import_envs, new_cap * sizeof(*new_arr));
    if (!new_arr) return;
    g_wasm_import_envs = new_arr;
    g_wasm_import_env_cap = new_cap;
  }
  g_wasm_import_envs[g_wasm_import_env_count++] = env;
}

static void wasm_unregister_import_env(wasm_import_func_env_t *env) {
  for (size_t i = 0; i < g_wasm_import_env_count; i++) {
    if (g_wasm_import_envs[i] != env) continue;
    g_wasm_import_envs[i] = g_wasm_import_envs[--g_wasm_import_env_count];
    return;
  }
}

static void wasm_import_func_env_finalizer(void *env_ptr) {
  wasm_import_func_env_t *env = (wasm_import_func_env_t *)env_ptr;
  wasm_unregister_import_env(env);
  free(env);
}

static void wasm_delete_owned_globals(wasm_global_t **globals, size_t count) {
  if (!globals) return;
  for (size_t i = 0; i < count; i++)
    if (globals[i]) wasm_global_delete(globals[i]);
}

static bool wasm_register_instance_owner(wasm_instance_t *instance, ant_value_t owner) {
  if (!instance || !instance->inst_comm_rt || !is_object_type(owner)) return true;

  if (g_wasm_instance_owner_count == g_wasm_instance_owner_cap) {
    size_t new_cap = g_wasm_instance_owner_cap ? g_wasm_instance_owner_cap * 2 : 16;
    wasm_instance_owner_t *new_arr = realloc(g_wasm_instance_owners, new_cap * sizeof(*new_arr));
    if (!new_arr) return false;
    g_wasm_instance_owners = new_arr;
    g_wasm_instance_owner_cap = new_cap;
  }

  g_wasm_instance_owners[g_wasm_instance_owner_count++] = (wasm_instance_owner_t){
    .runtime_instance = instance->inst_comm_rt,
    .owner = owner,
  };
  return true;
}

static void wasm_unregister_instance_owner(wasm_instance_t *instance) {
  WASMModuleInstanceCommon *runtime_instance = instance ? instance->inst_comm_rt : NULL;
  if (!runtime_instance) return;

  for (size_t i = 0; i < g_wasm_instance_owner_count; i++) {
    if (g_wasm_instance_owners[i].runtime_instance != runtime_instance) continue;
    g_wasm_instance_owners[i] = g_wasm_instance_owners[--g_wasm_instance_owner_count];
    return;
  }
}

static ant_value_t wasm_find_instance_owner(WASMModuleInstanceCommon *runtime_instance) {
  if (!runtime_instance) return js_mkundef();
  for (size_t i = 0; i < g_wasm_instance_owner_count; i++) {
    if (g_wasm_instance_owners[i].runtime_instance == runtime_instance)
      return g_wasm_instance_owners[i].owner;
  }
  return js_mkundef();
}

static ant_value_t wasm_wrap_func(
  ant_t *js, wasm_func_t *func, 
  ant_value_t owner, bool own_func
);

static bool ensure_wasm_engine(void) {
  if (g_wasm_engine) return true;
  g_wasm_engine = wasm_engine_new();
  return g_wasm_engine != NULL;
}

static size_t wasm_name_len(const wasm_name_t *name) {
  if (!name || !name->data) return 0;
  if (name->size > 0 && name->data[name->size - 1] == '\0') return name->size - 1;
  return name->size;
}

static const char *wasm_extern_kind_name(wasm_externkind_t kind) {
switch (kind) {
  case WASM_EXTERN_FUNC: return "function";
  case WASM_EXTERN_GLOBAL: return "global";
  case WASM_EXTERN_TABLE: return "table";
  case WASM_EXTERN_MEMORY: return "memory";
  default: return "unknown";
}}

static wasm_valkind_t wasm_valkind_from_string(const char *name, size_t len, bool *ok) {
  *ok = true;
  if (len == 3 && !memcmp(name, "i32", 3)) return WASM_I32;
  if (len == 3 && !memcmp(name, "i64", 3)) return WASM_I64;
  if (len == 3 && !memcmp(name, "f32", 3)) return WASM_F32;
  if (len == 3 && !memcmp(name, "f64", 3)) return WASM_F64;
  if (len == 9 && !memcmp(name, "externref", 9)) return WASM_EXTERNREF;
  if (len == 7 && !memcmp(name, "funcref", 7)) return WASM_FUNCREF;
  *ok = false;
  return WASM_I32;
}

static wasm_module_handle_t *wasm_module_handle(ant_value_t value) {
  if (!js_check_brand(value, BRAND_WASM_MODULE)) return NULL;
  return (wasm_module_handle_t *)js_get_native(value, WASM_MODULE_NATIVE_TAG);
}

static wasm_instance_handle_t *wasm_instance_handle(ant_value_t value) {
  if (!js_check_brand(value, BRAND_WASM_INSTANCE)) return NULL;
  return (wasm_instance_handle_t *)js_get_native(value, WASM_INSTANCE_NATIVE_TAG);
}

static wasm_extern_handle_t *wasm_extern_handle(ant_value_t value, wasm_extern_wrap_kind_t kind) {
  if ((kind == WASM_EXTERN_WRAP_GLOBAL && !js_check_brand(value, BRAND_WASM_GLOBAL))
      || (kind == WASM_EXTERN_WRAP_MEMORY && !js_check_brand(value, BRAND_WASM_MEMORY))
      || (kind == WASM_EXTERN_WRAP_TABLE && !js_check_brand(value, BRAND_WASM_TABLE))) {
    return NULL;
  }

  wasm_extern_handle_t *handle = (wasm_extern_handle_t *)js_get_native(value, WASM_EXTERN_NATIVE_TAG);
  return handle && handle->kind == kind ? handle : NULL;
}

static ant_value_t wasm_make_error(ant_t *js, ant_value_t proto, const char *name, const char *message) {
  ant_value_t err = js_make_error_silent(js, JS_ERR_TYPE, message ? message : "");
  if (vtype(err) != T_OBJ) return err;
  js_set(js, err, "name", js_mkstr(js, name, strlen(name)));
  if (is_object_type(proto)) js_set_proto_init(err, proto);
  return err;
}

static ant_value_t wasm_make_compile_error(ant_t *js, const char *message) {
  return wasm_make_error(js, g_wasm_compileerror_proto, "CompileError", message);
}

static ant_value_t wasm_make_link_error(ant_t *js, const char *message) {
  return wasm_make_error(js, g_wasm_linkerror_proto, "LinkError", message);
}

static ant_value_t wasm_make_runtime_error(ant_t *js, const char *message) {
  return wasm_make_error(js, g_wasm_runtimeerror_proto, "RuntimeError", message);
}

static ant_value_t wasm_error_value(ant_t *js, ant_value_t value) {
  if (is_err(value) && js->thrown_exists)
    return js->thrown_value;
  return value;
}

static void wasm_reject_with_error(ant_t *js, ant_value_t promise, ant_value_t error) {
  js_reject_promise(js, promise, error);
}

static bool wasm_buffer_source_to_vec(ant_t *js, ant_value_t value, wasm_byte_vec_t *out, char *error_buf, size_t error_buf_len) {
  const uint8_t *bytes = NULL;
  size_t len = 0;
  memset(out, 0, sizeof(*out));

  if (!buffer_source_get_bytes(js, value, &bytes, &len)) {
    snprintf(error_buf, error_buf_len, "Expected a BufferSource");
    return false;
  }

  wasm_byte_vec_new_uninitialized(out, len);
  if (len > 0 && !out->data) {
    snprintf(error_buf, error_buf_len, "Out of memory");
    return false;
  }

  if (len > 0) memcpy(out->data, bytes, len);
  return true;
}

static ant_value_t wasm_value_from_i64(ant_t *js, int64_t value) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%" PRId64, value);
  if (n < 0) return js_mkerr(js, "Failed to convert i64");
  return js_mkbigint(js, buf, (size_t)n, value < 0);
}

static ant_value_t wasm_value_to_js(ant_t *js, const wasm_val_t *value) {
  switch (value->kind) {
    case WASM_I32: return js_mknum((double)value->of.i32);
    case WASM_I64: return wasm_value_from_i64(js, value->of.i64);
    case WASM_F32: return js_mknum((double)value->of.f32);
    case WASM_F64: return js_mknum(value->of.f64);
    case WASM_EXTERNREF:
      return value->of.ref ? js_mkundef() : js_mknull();
    case WASM_FUNCREF: {
      wasm_func_t *func;
      if (!value->of.ref) return js_mknull();
      func = wasm_ref_as_func(value->of.ref);
      if (!func) return js_mkundef();
      return wasm_wrap_func(js, func, js_mkundef(), true);
    }
    default: return js_mkundef();
  }
}

static bool js_value_to_i64(ant_t *js, ant_value_t value, int64_t *out) {
  if (vtype(value) == T_BIGINT) {
    ant_value_t str_val = js_tostring_val(js, value);
    if (is_err(str_val) || vtype(str_val) != T_STR) return false;
    const char *str = js_str(js, str_val);
    if (!str) return false;
    char *end = NULL;
    long long parsed = strtoll(str, &end, 10);
    if (!end || *end != '\0') return false;
    *out = (int64_t)parsed;
    return true;
  }

  double d = js_to_number(js, value);
  if (!isfinite(d)) return false;
  *out = (int64_t)d;
  return true;
}

static bool js_value_to_wasm(ant_t *js, ant_value_t value, wasm_valkind_t kind, wasm_val_t *out) {
  memset(out, 0, sizeof(*out));
  out->kind = kind;

  switch (kind) {
    case WASM_I32:
      out->of.i32 = (int32_t)js_to_number(js, value);
      return true;
    case WASM_I64:
      return js_value_to_i64(js, value, &out->of.i64);
    case WASM_F32:
      out->of.f32 = (float)js_to_number(js, value);
      return true;
    case WASM_F64:
      out->of.f64 = js_to_number(js, value);
      return true;
    case WASM_EXTERNREF:
      out->of.ref = NULL;
      return vtype(value) == T_NULL || vtype(value) == T_UNDEF;
    case WASM_FUNCREF: {
      ant_value_t state;
      wasm_func_handle_t *handle;
      if (vtype(value) == T_NULL || vtype(value) == T_UNDEF) {
        out->of.ref = NULL;
        return true;
      }
      if (!is_callable(value)) return false;
      state = js_get_slot(value, SLOT_DATA);
      if (!is_object_type(state)) return false;
      handle = (wasm_func_handle_t *)js_get_native(state, WASM_FUNC_STATE_TAG);
      if (!handle || !handle->func) return false;
      out->of.ref = wasm_func_as_ref(handle->func);
      return out->of.ref != NULL;
    }
    default: return false;
  }
}

static ant_value_t wasm_js_from_result_vec(ant_t *js, wasm_val_vec_t *results) {
  if (!results || results->size == 0) return js_mkundef();
  if (results->size == 1) return wasm_value_to_js(js, &results->data[0]);

  ant_value_t arr = js_mkarr(js);
  for (size_t i = 0; i < results->size; i++) {
    js_arr_push(js, arr, wasm_value_to_js(js, &results->data[i]));
  }
  return arr;
}

static ant_value_t wasm_trap_to_error(ant_t *js, wasm_trap_t *trap) {
  wasm_message_t message = WASM_EMPTY_VEC;
  wasm_trap_message(trap, &message);

  ant_value_t err = wasm_make_runtime_error(js, message.data ? message.data : "WebAssembly trap");

  wasm_byte_vec_delete(&message);
  wasm_trap_delete(trap);
  return err;
}

static ant_value_t wasm_wrap_module(ant_t *js, wasm_store_t *store, wasm_module_t *module) {
  wasm_module_handle_t *handle = calloc(1, sizeof(*handle));
  if (!handle) {
    if (module) wasm_module_delete(module);
    if (store) wasm_store_delete(store);
    return js_mkerr(js, "out of memory");
  }

  handle->store = store;
  handle->module = module;

  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, g_wasm_module_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_WASM_MODULE));
  js_set_native(obj, handle, WASM_MODULE_NATIVE_TAG);
  return obj;
}

static void wasm_module_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  wasm_module_handle_t *handle = (wasm_module_handle_t *)js_get_native(value, WASM_MODULE_NATIVE_TAG);
  if (!handle) return;
  if (handle->module) wasm_module_delete(handle->module);
  if (handle->store) wasm_store_delete(handle->store);
  free(handle->bytes);
  
  free(handle);
  js_clear_native(value, WASM_MODULE_NATIVE_TAG);
}

static ant_value_t wasm_wrap_instance(ant_t *js, wasm_instance_handle_t *handle, ant_value_t module_ref) {
  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, g_wasm_instance_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_WASM_INSTANCE));
  js_set_native(obj, handle, WASM_INSTANCE_NATIVE_TAG);
  js_set_slot_wb(js, obj, SLOT_CTOR, module_ref);
  return obj;
}

static void wasm_instance_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  wasm_instance_handle_t *handle = (wasm_instance_handle_t *)js_get_native(value, WASM_INSTANCE_NATIVE_TAG);
  if (!handle) return;
  wasm_unregister_instance_owner(handle->instance);
  for (size_t j = 0; j < handle->host_func_count; j++) 
    if (handle->host_funcs[j]) wasm_func_delete(handle->host_funcs[j]);
  for (size_t j = 0; j < handle->host_global_count; j++)
    if (handle->host_globals[j]) wasm_global_delete(handle->host_globals[j]);
  
  free(handle->host_funcs);
  free(handle->host_globals);
  wasm_extern_vec_delete(&handle->exports);
  free(handle);
  js_clear_native(value, WASM_INSTANCE_NATIVE_TAG);
}

static ant_value_t wasm_wrap_extern_object(ant_t *js, wasm_extern_wrap_kind_t kind, ant_value_t proto, int brand, wasm_store_t *store, bool own_handle, void *ptr, ant_value_t owner) {
  wasm_extern_handle_t *handle = calloc(1, sizeof(*handle));
  if (!handle) return js_mkerr(js, "out of memory");

  handle->kind = kind;
  handle->store = store;
  handle->own_handle = own_handle;
  handle->memory_buffer = js_mkundef();
  handle->cached_value = (wasm_val_t)WASM_INIT_VAL;

  switch (kind) {
    case WASM_EXTERN_WRAP_GLOBAL: handle->as.global = (wasm_global_t *)ptr; break;
    case WASM_EXTERN_WRAP_MEMORY: handle->as.memory = (wasm_memory_t *)ptr; break;
    case WASM_EXTERN_WRAP_TABLE: handle->as.table = (wasm_table_t *)ptr; break;
  }

  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(brand));
  js_set_native(obj, handle, WASM_EXTERN_NATIVE_TAG);
  if (is_object_type(owner)) js_set_slot_wb(js, obj, SLOT_ENTRIES, owner);
  return obj;
}

static void wasm_extern_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  wasm_extern_handle_t *handle = (wasm_extern_handle_t *)js_get_native(value, WASM_EXTERN_NATIVE_TAG);
  if (!handle) return;

  if (handle->own_handle) {
  switch (handle->kind) {
    case WASM_EXTERN_WRAP_GLOBAL:
      if (handle->as.global) wasm_global_delete(handle->as.global);
      break;
    case WASM_EXTERN_WRAP_MEMORY:
      if (handle->as.memory) wasm_memory_delete(handle->as.memory);
      break;
    case WASM_EXTERN_WRAP_TABLE:
      if (handle->as.table) wasm_table_delete(handle->as.table);
      break;
    }
    if (handle->store) wasm_store_delete(handle->store);
  }

  free(handle);
  js_clear_native(value, WASM_EXTERN_NATIVE_TAG);
}

static ant_value_t js_wasm_exported_func_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  wasm_func_handle_t *handle;
  wasm_func_t *func;

  handle = is_object_type(state) 
    ? (wasm_func_handle_t *)js_get_native(state, WASM_FUNC_STATE_TAG)
    : NULL;
  func = handle ? handle->func : NULL;
  if (!func) return js_mkerr(js, "Invalid WebAssembly function");

  wasm_functype_t *type = wasm_func_type(func);
  if (!type) return js_mkerr(js, "Failed to inspect WebAssembly function");

  const wasm_valtype_vec_t *params = wasm_functype_params(type);
  const wasm_valtype_vec_t *results_t = wasm_functype_results(type);

  wasm_val_vec_t wasm_args = WASM_EMPTY_VEC;
  wasm_val_vec_t wasm_results = WASM_EMPTY_VEC;
  wasm_trap_t *trap = NULL;
  ant_value_t result = js_mkundef();

  wasm_val_vec_new_uninitialized(&wasm_args, params ? params->size : 0);
  wasm_val_vec_new_uninitialized(&wasm_results, results_t ? results_t->size : 0);

  for (size_t i = 0; params && i < params->size; i++) {
    if ((int)i >= nargs) {
      wasm_val_vec_delete(&wasm_args);
      wasm_val_vec_delete(&wasm_results);
      wasm_functype_delete(type);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Missing WebAssembly argument");
    }

    if (!js_value_to_wasm(js, args[i], wasm_valtype_kind(params->data[i]), &wasm_args.data[i])) {
      wasm_val_vec_delete(&wasm_args);
      wasm_val_vec_delete(&wasm_results);
      wasm_functype_delete(type);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported WebAssembly argument type");
    }
  }

  wasm_clear_pending_import_throw();
  trap = wasm_func_call(func, &wasm_args, &wasm_results);
  
  if (trap) {
    if (g_wasm_pending_import_throw_exists) {
      result = wasm_consume_pending_import_throw();
      js_mark_errorlike_no_stack(js, result);
      wasm_val_vec_delete(&wasm_args);
      wasm_val_vec_delete(&wasm_results);
      wasm_functype_delete(type);
      wasm_trap_delete(trap);
      return js_throw(js, result);
    }
    
    result = wasm_trap_to_error(js, trap);
    wasm_val_vec_delete(&wasm_args);
    wasm_val_vec_delete(&wasm_results);
    wasm_functype_delete(type);
    return js_throw(js, result);
  }
  
  wasm_clear_pending_import_throw();
  result = wasm_js_from_result_vec(js, &wasm_results);

  wasm_val_vec_delete(&wasm_args);
  wasm_val_vec_delete(&wasm_results);
  wasm_functype_delete(type);
  
  return result;
}

static void wasm_func_state_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  wasm_func_handle_t *handle = (wasm_func_handle_t *)js_get_native(value, WASM_FUNC_STATE_TAG);
  if (!handle) return;

  if (handle->own_func && handle->func) wasm_func_delete(handle->func);
  free(handle);
  js_clear_native(value, WASM_FUNC_STATE_TAG);
}

static ant_value_t wasm_wrap_func(ant_t *js, wasm_func_t *func, ant_value_t owner, bool own_func) {
  wasm_func_handle_t *handle;
  ant_value_t state;

  if (!func) return js_mkundef();

  handle = calloc(1, sizeof(*handle));
  if (!handle) {
    if (own_func) wasm_func_delete(func);
    return js_mkerr(js, "out of memory");
  }

  handle->func = func;
  handle->own_func = own_func;

  state = js_mkobj(js);
  js_set_native(state, handle, WASM_FUNC_STATE_TAG);
  
  if (is_object_type(owner)) js_set_slot_wb(js, state, SLOT_ENTRIES, owner);
  js_set_finalizer(state, wasm_func_state_finalize);

  return js_heavy_mkfun(js, js_wasm_exported_func_call, state);
}

static ant_value_t wasm_wrap_export_value(ant_t *js, ant_value_t instance_obj, const wasm_exporttype_t *export_type, wasm_extern_t *external) {
  switch (wasm_extern_kind(external)) {
    case WASM_EXTERN_FUNC: {
      return wasm_wrap_func(js, wasm_extern_as_func(external), instance_obj, false);
    }
    case WASM_EXTERN_GLOBAL: {
      ant_value_t obj = wasm_wrap_extern_object(
        js, WASM_EXTERN_WRAP_GLOBAL, g_wasm_global_proto, BRAND_WASM_GLOBAL,
        NULL, false, wasm_extern_as_global(external), instance_obj
      );
      if (vtype(obj) == T_OBJ) js_set_finalizer(obj, wasm_extern_finalize);
      return obj;
    }
    case WASM_EXTERN_MEMORY: {
      ant_value_t obj = wasm_wrap_extern_object(
        js, WASM_EXTERN_WRAP_MEMORY, g_wasm_memory_proto, BRAND_WASM_MEMORY,
        NULL, false, wasm_extern_as_memory(external), instance_obj
      );
      if (vtype(obj) == T_OBJ) js_set_finalizer(obj, wasm_extern_finalize);
      return obj;
    }
    case WASM_EXTERN_TABLE: {
      ant_value_t obj = wasm_wrap_extern_object(
        js, WASM_EXTERN_WRAP_TABLE, g_wasm_table_proto, BRAND_WASM_TABLE,
        NULL, false, wasm_extern_as_table(external), instance_obj
      );
      if (vtype(obj) == T_OBJ) js_set_finalizer(obj, wasm_extern_finalize);
      return obj;
    }
    default:
      (void)export_type;
      return js_mkundef();
  }
}

static ant_value_t wasm_module_from_bytes(ant_t *js, ant_value_t value, ant_value_t *out_module) {
  wasm_byte_vec_t binary = WASM_EMPTY_VEC;
  wasm_store_t *store = NULL;
  wasm_module_t *module = NULL;
  
  char error_buf[128] = {0};
  bool suppress_wasi_warning = false;

  *out_module = js_mkundef();

  if (!ensure_wasm_engine()) return js_mkerr(js, "Failed to initialize WebAssembly engine");
  if (!(store = wasm_store_new(g_wasm_engine))) return js_mkerr(js, "Failed to create WebAssembly store");

  if (!wasm_buffer_source_to_vec(js, value, &binary, error_buf, sizeof(error_buf))) {
    wasm_store_delete(store);
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s", error_buf);
  }

  suppress_wasi_warning = wasi_bytes_need_wasi_command_warning_suppression(
    (const uint8_t *)binary.data, binary.size
  );
  
  if (suppress_wasi_warning) wasm_runtime_set_log_level(WASM_LOG_LEVEL_ERROR);
  module = wasm_module_new(store, &binary);
  
  if (suppress_wasi_warning) wasm_runtime_set_log_level(WASM_LOG_LEVEL_WARNING);
  
  if (!module) {
    wasm_byte_vec_delete(&binary);
    wasm_store_delete(store);
    return wasm_make_compile_error(js, "Failed to compile WebAssembly module");
  }

  *out_module = wasm_wrap_module(js, store, module);
  if (vtype(*out_module) == T_OBJ) {
    wasm_module_handle_t *handle = wasm_module_handle(*out_module);
    if (handle && binary.size > 0) {
      handle->bytes = malloc(binary.size);
      if (handle->bytes) {
        memcpy(handle->bytes, binary.data, binary.size);
        handle->bytes_len = binary.size;
      }
    }
    js_set_finalizer(*out_module, wasm_module_finalize);
    js_set_slot_wb(js, *out_module, SLOT_MAP, value);
  }
  wasm_byte_vec_delete(&binary);
  return js_mkundef();
}

static ant_value_t js_wasm_module_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t module = js_mkundef();
  ant_value_t err;

  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Module constructor requires 'new'");
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Module requires a BufferSource");

  err = wasm_module_from_bytes(js, args[0], &module);
  if (is_err(err)) return err;
  if (vtype(module) != T_OBJ) return js_throw(js, wasm_error_value(js, err));
  return module;
}

static ant_value_t wasm_module_type_descriptors(ant_t *js, ant_value_t module_obj, bool imports) {
  wasm_module_handle_t *handle = wasm_module_handle(module_obj);
  if (!handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Module");

  ant_value_t arr = js_mkarr(js);
  if (imports) {
    wasm_importtype_vec_t vec = WASM_EMPTY_VEC;
    wasm_module_imports(handle->module, &vec);
    for (size_t i = 0; i < vec.size; i++) {
      const wasm_importtype_t *entry = vec.data[i];
      const wasm_name_t *module_name = wasm_importtype_module(entry);
      const wasm_name_t *name = wasm_importtype_name(entry);
      const wasm_externtype_t *type = wasm_importtype_type(entry);

      ant_value_t item = js_mkobj(js);
      js_set(js, item, "module", js_mkstr(js, module_name->data, wasm_name_len(module_name)));
      js_set(js, item, "name", js_mkstr(js, name->data, wasm_name_len(name)));
      js_set(js, item, "kind", js_mkstr(js, wasm_extern_kind_name(wasm_externtype_kind(type)), strlen(wasm_extern_kind_name(wasm_externtype_kind(type)))));
      js_arr_push(js, arr, item);
    }
    wasm_importtype_vec_delete(&vec);
  } else {
    wasm_exporttype_vec_t vec = WASM_EMPTY_VEC;
    wasm_module_exports(handle->module, &vec);
    for (size_t i = 0; i < vec.size; i++) {
      const wasm_exporttype_t *entry = vec.data[i];
      const wasm_name_t *name = wasm_exporttype_name(entry);
      const wasm_externtype_t *type = wasm_exporttype_type(entry);
      
      ant_value_t item = js_mkobj(js);
      js_set(js, item, "name", js_mkstr(js, name->data, wasm_name_len(name)));
      js_set(js, item, "kind", js_mkstr(js, wasm_extern_kind_name(wasm_externtype_kind(type)), strlen(wasm_extern_kind_name(wasm_externtype_kind(type)))));
      js_arr_push(js, arr, item);
    }
    wasm_exporttype_vec_delete(&vec);
  }

  return arr;
}

static ant_value_t js_wasm_module_imports(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Module.imports requires 1 argument");
  return wasm_module_type_descriptors(js, args[0], true);
}

static ant_value_t js_wasm_module_exports(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Module.exports requires 1 argument");
  return wasm_module_type_descriptors(js, args[0], false);
}

static bool wasm_read_leb_u32(const uint8_t *bytes, size_t len, size_t *offset, uint32_t *out) {
  uint32_t result = 0;
  uint32_t shift = 0;

  while (*offset < len && shift < 35) {
    uint8_t byte = bytes[(*offset)++];
    result |= (uint32_t)(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
  }

  return false;
}

static ant_value_t js_wasm_module_custom_sections(ant_t *js, ant_value_t *args, int nargs) {
  wasm_module_handle_t *handle;
  ant_value_t name_val, result;
  const char *wanted;
  size_t wanted_len = 0;
  size_t offset = 8;

  if (nargs < 2)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Module.customSections requires 2 arguments");
  handle = wasm_module_handle(args[0]);
  if (!handle)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Module");

  name_val = js_tostring_val(js, args[1]);
  if (is_err(name_val)) return name_val;
  wanted = js_getstr(js, name_val, &wanted_len);
  result = js_mkarr(js);

  if (!handle->bytes || handle->bytes_len < 8) return result;
  while (offset < handle->bytes_len) {
    uint8_t section_id = handle->bytes[offset++];
    uint32_t section_size, name_len;
    size_t section_start, section_end, name_start, data_start;
    ArrayBufferData *buffer;

    if (!wasm_read_leb_u32(handle->bytes, handle->bytes_len, &offset, &section_size)) break;
    section_start = offset;
    section_end = section_start + section_size;
    if (section_end < section_start || section_end > handle->bytes_len) break;

    if (section_id == 0) {
      offset = section_start;
      if (!wasm_read_leb_u32(handle->bytes, section_end, &offset, &name_len)) break;
      name_start = offset;
      data_start = name_start + name_len;
      if (data_start <= section_end && name_len == wanted_len
          && memcmp(handle->bytes + name_start, wanted, wanted_len) == 0) {
        buffer = create_array_buffer_data(section_end - data_start);
        if (!buffer) return js_mkerr(js, "out of memory");
        memcpy(buffer->data, handle->bytes + data_start, section_end - data_start);
        js_arr_push(js, result, create_arraybuffer_obj(js, buffer));
      }
    }

    offset = section_end;
  }

  return result;
}

static ant_value_t js_wasm_instance_exports_getter(ant_t *js, ant_value_t *args, int nargs) {
  if (!js_check_brand(js->this_val, BRAND_WASM_INSTANCE)) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Instance");
  return js_get_slot(js->this_val, SLOT_ENTRIES);
}

static ant_value_t wasm_property_get_nested(ant_t *js, ant_value_t base, const wasm_name_t *name) {
  if (!is_object_type(base)) return js_mkundef();
  return js_getprop_fallback(js, base, name && name->data ? name->data : "");
}

static wasm_trap_t *wasm_import_func_callback(void *env_ptr, const wasm_val_vec_t *args, wasm_val_vec_t *results) {
  wasm_import_func_env_t *env = (wasm_import_func_env_t *)env_ptr;
  ant_t *js = env->js;
  
  ant_value_t *js_args = NULL;
  ant_value_t result = js_mkundef();
  wasm_message_t trap_msg = WASM_EMPTY_VEC;

  if (args && args->size > 0) {
    js_args = calloc(args->size, sizeof(*js_args));
    if (!js_args) {
      wasm_name_new_from_string_nt(&trap_msg, "Out of memory");
      wasm_trap_t *trap = wasm_trap_new(env->store, &trap_msg);
      wasm_byte_vec_delete(&trap_msg);
      return trap;
    }
  }

  for (size_t i = 0; args && i < args->size; i++) {
    js_args[i] = wasm_value_to_js(js, &args->data[i]);
  }

  result = sv_vm_call(
    js->vm, js, env->fn, js_mkundef(), 
    js_args, args ? (int)args->size : 0, NULL, false
  );
  free(js_args);

  if (is_err(result)) {
    ant_value_t thrown = js->thrown_exists ? js->thrown_value : result;
    wasm_set_pending_import_throw(thrown);
    
    const char *msg = "WebAssembly import threw";
    if (vtype(js->thrown_value) == T_OBJ) {
      const char *message = get_str_prop(js, js->thrown_value, "message", 7, NULL);
      if (message && *message) msg = message;
    }
    
    wasm_name_new_from_string_nt(&trap_msg, msg);
    wasm_trap_t *trap = wasm_trap_new(env->store, &trap_msg);
    wasm_byte_vec_delete(&trap_msg);
    
    return trap;
  }

  if (results && results->size > 0) {
  if (results->size == 1) {
    if (!js_value_to_wasm(js, result, results->data[0].kind, &results->data[0])) {
      wasm_name_new_from_string_nt(&trap_msg, "Unsupported import return value");
      wasm_trap_t *trap = wasm_trap_new(env->store, &trap_msg);
      wasm_byte_vec_delete(&trap_msg);
      return trap;
    }
  } else {
    if (vtype(result) != T_ARR) {
      wasm_name_new_from_string_nt(&trap_msg, "Expected an array for multi-value return");
      wasm_trap_t *trap = wasm_trap_new(env->store, &trap_msg);
      wasm_byte_vec_delete(&trap_msg);
      return trap;
    }

    for (size_t i = 0; i < results->size; i++) {
      ant_value_t item = js_arr_get(js, result, (ant_offset_t)i);
      if (!js_value_to_wasm(js, item, results->data[i].kind, &results->data[i])) {
        wasm_name_new_from_string_nt(&trap_msg, "Unsupported import return value");
        wasm_trap_t *trap = wasm_trap_new(env->store, &trap_msg);
        wasm_byte_vec_delete(&trap_msg);
        return trap;
      }
    }
  }}

  return NULL;
}

static ant_value_t wasm_instantiate_module(ant_t *js, ant_value_t module_obj, ant_value_t import_obj, ant_value_t *out_instance) {
  wasm_module_handle_t *module_handle = wasm_module_handle(module_obj);
  wasm_importtype_vec_t import_types = WASM_EMPTY_VEC;
  wasm_exporttype_vec_t export_types = WASM_EMPTY_VEC;
  
  wasm_extern_t **imports = NULL;
  wasm_func_t **owned_host_funcs = NULL;
  wasm_global_t **owned_host_globals = NULL;
  
  size_t owned_host_func_count = 0;
  size_t owned_host_global_count = 0;
  wasm_extern_vec_t exports = WASM_EMPTY_VEC;
  
  wasm_trap_t *trap = NULL;
  wasm_instance_t *instance = NULL;
  ant_value_t instance_obj = js_mkundef();
  ant_value_t exports_obj = js_mkobj(js);
  *out_instance = js_mkundef();

  if (!module_handle) 
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Module");

  if (wasi_module_has_wasi_imports(module_handle->module)
      && wasi_module_is_command_or_reactor(module_handle->module)) {
    ant_value_t wasi_opts = is_object_type(import_obj) ? js_get(js, import_obj, "wasi") : js_mkundef();
    if (!is_object_type(import_obj) || is_object_type(wasi_opts)) {
      ant_value_t bytes_src = js_get_slot(module_obj, SLOT_MAP);
      wasm_byte_vec_t binary = WASM_EMPTY_VEC;
      char wasi_err[128] = {0};
      if (!wasm_buffer_source_to_vec(js, bytes_src, &binary, wasi_err, sizeof(wasi_err))) {
        return js_mkerr(js, "WASI: cannot extract module bytes");
      }
      *out_instance = wasi_instantiate(js, (const uint8_t *)binary.data, binary.size, module_obj, wasi_opts);
      wasm_byte_vec_delete(&binary);
      return is_err(*out_instance) ? *out_instance : js_mkundef();
    }
  }

  wasm_module_imports(module_handle->module, &import_types);
  if (import_types.size > 0) {
    imports = calloc(import_types.size, sizeof(*imports));
    owned_host_funcs = calloc(import_types.size, sizeof(*owned_host_funcs));
    owned_host_globals = calloc(import_types.size, sizeof(*owned_host_globals));
    if (!imports || !owned_host_funcs || !owned_host_globals) {
      free(imports);
      free(owned_host_funcs);
      free(owned_host_globals);
      wasm_importtype_vec_delete(&import_types);
      return js_mkerr(js, "out of memory");
    }
  }

  for (size_t i = 0; i < import_types.size; i++) {
    const wasm_importtype_t *import_type = import_types.data[i];
    const wasm_name_t *module_name = wasm_importtype_module(import_type);
    const wasm_name_t *field_name = wasm_importtype_name(import_type);
    const wasm_externtype_t *extern_type = wasm_importtype_type(import_type);
    
    ant_value_t namespace_obj = wasm_property_get_nested(js, import_obj, module_name);
    ant_value_t value = wasm_property_get_nested(js, namespace_obj, field_name);
    wasm_externkind_t kind = wasm_externtype_kind(extern_type);

    if (kind == WASM_EXTERN_FUNC) {
      const wasm_functype_t *func_type = wasm_externtype_as_functype_const(extern_type);
      wasm_import_func_env_t *env = NULL;

      if (!is_callable(value)) {
        free(imports);
        free(owned_host_funcs);
        wasm_delete_owned_globals(owned_host_globals, owned_host_global_count);
        free(owned_host_globals);
        wasm_importtype_vec_delete(&import_types);
        return wasm_make_link_error(js, "Missing function import");
      }

      env = calloc(1, sizeof(*env));
      if (!env) {
        free(imports);
        free(owned_host_funcs);
        wasm_delete_owned_globals(owned_host_globals, owned_host_global_count);
        free(owned_host_globals);
        wasm_importtype_vec_delete(&import_types);
        return js_mkerr(js, "out of memory");
      }

      env->js = js;
      env->store = module_handle->store;
      env->owner = module_obj;
      env->fn = value;
      wasm_register_import_env(env);


      owned_host_funcs[owned_host_func_count] = wasm_func_new_with_env(
        module_handle->store,
        func_type,
        wasm_import_func_callback,
        env,
        wasm_import_func_env_finalizer
      );

      if (!owned_host_funcs[owned_host_func_count]) {
        free(env);
        free(imports);
        free(owned_host_funcs);
        wasm_delete_owned_globals(owned_host_globals, owned_host_global_count);
        free(owned_host_globals);
        wasm_importtype_vec_delete(&import_types);
        return wasm_make_link_error(js, "Failed to create function import");
      }

      imports[i] = wasm_func_as_extern(owned_host_funcs[owned_host_func_count]);
      owned_host_func_count++;
      continue;
    }

    if (kind == WASM_EXTERN_MEMORY) {
      wasm_extern_handle_t *handle = wasm_extern_handle(value, WASM_EXTERN_WRAP_MEMORY);
      if (!handle || !handle->as.memory) {
        free(imports);
        free(owned_host_funcs);
        wasm_delete_owned_globals(owned_host_globals, owned_host_global_count);
        free(owned_host_globals);
        wasm_importtype_vec_delete(&import_types);
        return wasm_make_link_error(js, "Missing memory import");
      }
      imports[i] = wasm_memory_as_extern(handle->as.memory);
      continue;
    }

    if (kind == WASM_EXTERN_TABLE) {
      wasm_extern_handle_t *handle = wasm_extern_handle(value, WASM_EXTERN_WRAP_TABLE);
      if (!handle || !handle->as.table) {
        free(imports);
        free(owned_host_funcs);
        wasm_delete_owned_globals(owned_host_globals, owned_host_global_count);
        free(owned_host_globals);
        wasm_importtype_vec_delete(&import_types);
        return wasm_make_link_error(js, "Missing table import");
      }
      imports[i] = wasm_table_as_extern(handle->as.table);
      continue;
    }

    if (kind == WASM_EXTERN_GLOBAL) {
      wasm_extern_handle_t *handle = wasm_extern_handle(value, WASM_EXTERN_WRAP_GLOBAL);
      if (!handle || !handle->as.global) {
        const wasm_globaltype_t *global_type = wasm_externtype_as_globaltype_const(extern_type);
        const wasm_valtype_t *content = global_type ? wasm_globaltype_content(global_type) : NULL;
        wasm_val_t initial = WASM_INIT_VAL;
        wasm_global_t *owned_global = NULL;
        if (global_type && content
            && wasm_globaltype_mutability(global_type) == WASM_CONST
            && js_value_to_wasm(js, value, wasm_valtype_kind(content), &initial)) {
          owned_global = wasm_global_new(module_handle->store, global_type, &initial);
        }
        if (owned_global) {
          owned_host_globals[owned_host_global_count++] = owned_global;
          imports[i] = wasm_global_as_extern(owned_global);
          continue;
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "Missing global import %.*s.%.*s",
                 (int)wasm_name_len(module_name),
                 module_name && module_name->data ? module_name->data : "",
                 (int)wasm_name_len(field_name),
                 field_name && field_name->data ? field_name->data : "");
        free(imports);
        free(owned_host_funcs);
        wasm_delete_owned_globals(owned_host_globals, owned_host_global_count);
        free(owned_host_globals);
        wasm_importtype_vec_delete(&import_types);
        return wasm_make_link_error(js, msg);
      }
      imports[i] = wasm_global_as_extern(handle->as.global);
      continue;
    }
  }

  {
    wasm_extern_vec_t import_vec = WASM_EMPTY_VEC;
    if (imports && import_types.size > 0)
      import_vec = (wasm_extern_vec_t){ import_types.size, imports, import_types.size, sizeof(*imports), NULL };

    wasm_clear_pending_import_throw();
    instance = wasm_instance_new_with_args(module_handle->store, module_handle->module, &import_vec, &trap, KILOBYTE(1024), 0);
  }

  free(imports);
  wasm_importtype_vec_delete(&import_types);

  if (!instance) {
    for (size_t i = 0; i < owned_host_func_count; i++) {
      if (owned_host_funcs[i]) wasm_func_delete(owned_host_funcs[i]);
    }
    wasm_delete_owned_globals(owned_host_globals, owned_host_global_count);
    free(owned_host_funcs);
    free(owned_host_globals);
    
    if (trap) {
      if (g_wasm_pending_import_throw_exists) {
        ant_value_t thrown = wasm_consume_pending_import_throw();
        js_mark_errorlike_no_stack(js, thrown);
        wasm_trap_delete(trap);
        return js_throw(js, thrown);
      }
      return wasm_trap_to_error(js, trap);
    }
    
    return wasm_make_link_error(js, "Failed to instantiate WebAssembly module");
  }
  
  wasm_clear_pending_import_throw();
  wasm_instance_exports(instance, &exports);
  wasm_module_exports(module_handle->module, &export_types);

  {
    wasm_instance_handle_t *inst_handle = calloc(1, sizeof(*inst_handle));
    if (!inst_handle) {
      wasm_extern_vec_delete(&exports);
      wasm_exporttype_vec_delete(&export_types);
      wasm_delete_owned_globals(owned_host_globals, owned_host_global_count);
      free(owned_host_globals);
      return js_mkerr(js, "out of memory");
    }
    inst_handle->instance = instance;
    inst_handle->exports = exports;
    inst_handle->host_funcs = owned_host_funcs;
    inst_handle->host_func_count = owned_host_func_count;
    inst_handle->host_globals = owned_host_globals;
    inst_handle->host_global_count = owned_host_global_count;

    instance_obj = wasm_wrap_instance(js, inst_handle, module_obj);
  }
  if (vtype(instance_obj) != T_OBJ) {
    wasm_exporttype_vec_delete(&export_types);
    return instance_obj;
  }
  js_set_finalizer(instance_obj, wasm_instance_finalize);
  if (!wasm_register_instance_owner(instance, instance_obj)) {
    wasm_exporttype_vec_delete(&export_types);
    return js_mkerr(js, "out of memory");
  }

  for (size_t i = 0; i < export_types.size && i < exports.size; i++) {
    const wasm_exporttype_t *export_type = export_types.data[i];
    const wasm_name_t *name = wasm_exporttype_name(export_type);
    ant_value_t export_value = wasm_wrap_export_value(js, instance_obj, export_type, exports.data[i]);
    js_setprop(js, exports_obj, js_mkstr(js, name->data, wasm_name_len(name)), export_value);
  }
  
  wasm_exporttype_vec_delete(&export_types);
  js_set_slot_wb(js, instance_obj, SLOT_ENTRIES, exports_obj);
  if (is_object_type(import_obj)) js_set_slot_wb(js, instance_obj, SLOT_MAP, import_obj);

  *out_instance = instance_obj;
  return js_mkundef();
}

static ant_value_t js_wasm_instance_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t instance = js_mkundef();
  ant_value_t import_obj = (nargs >= 2 && is_object_type(args[1])) ? args[1] : js_mkundef();
  ant_value_t err;

  if (vtype(js->new_target) == T_UNDEF) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Instance constructor requires 'new'");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Instance requires a module");

  err = wasm_instantiate_module(js, args[0], import_obj, &instance);
  if (is_err(err)) return err;
  if (vtype(instance) != T_OBJ) return js_throw(js, wasm_error_value(js, err));
  
  return instance;
}

static ant_value_t js_wasm_global_value_getter(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_GLOBAL);
  wasm_val_t value = WASM_INIT_VAL;

  if (!handle || !handle->as.global) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Global");

  wasm_global_get(handle->as.global, &value);
  if (handle->use_cached_value) handle->cached_value = value;
  return wasm_value_to_js(js, &value);
}

static ant_value_t js_wasm_global_value_setter(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_GLOBAL);
  wasm_globaltype_t *type = NULL;
  
  const wasm_valtype_t *content = NULL;
  wasm_val_t value = WASM_INIT_VAL;

  if (!handle || !handle->as.global)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Global");
  if (nargs < 1) return js_mkundef();

  type = wasm_global_type(handle->as.global);
  if (!type) return js_mkerr(js, "Failed to inspect WebAssembly.Global");
  if (wasm_globaltype_mutability(type) != WASM_VAR) {
    wasm_globaltype_delete(type);
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Global is immutable");
  }

  content = wasm_globaltype_content(type);
  if (!js_value_to_wasm(js, args[0], wasm_valtype_kind(content), &value)) {
    wasm_globaltype_delete(type);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported global value");
  }

  wasm_global_set(handle->as.global, &value);
  if (handle->use_cached_value) handle->cached_value = value;
  wasm_globaltype_delete(type);
  
  return js_mkundef();
}

static ant_value_t js_wasm_global_value_of(ant_t *js, ant_value_t *args, int nargs) {
  return js_wasm_global_value_getter(js, NULL, 0);
}

static ant_value_t js_wasm_global_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t descriptor;
  ant_value_t mutable_val;
  
  const char *value_type;
  ant_offset_t value_type_len = 0;
  bool ok = false;
  
  wasm_valkind_t kind;
  wasm_store_t *store = NULL;
  wasm_valtype_t *valtype = NULL;
  wasm_globaltype_t *globaltype = NULL;
  wasm_global_t *global = NULL;
  wasm_val_t initial = WASM_INIT_VAL;
  ant_value_t result;

  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Global constructor requires 'new'");
  if (nargs < 1 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Global requires a descriptor object");
  if (!ensure_wasm_engine())
    return js_mkerr(js, "Failed to initialize WebAssembly engine");

  descriptor = args[0];
  value_type = get_str_prop(js, descriptor, "value", 5, &value_type_len);
  if (!value_type)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Global descriptor requires a value type");

  kind = wasm_valkind_from_string(value_type, value_type_len, &ok);
  if (!ok)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported WebAssembly.Global value type");

  if (!js_value_to_wasm(js, nargs >= 2 ? args[1] : js_mknum(0), kind, &initial))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported WebAssembly.Global initial value");

  mutable_val = js_get(js, descriptor, "mutable");
  store = wasm_store_new(g_wasm_engine);
  if (!store) return js_mkerr(js, "Failed to create WebAssembly store");

  valtype = wasm_valtype_new(kind);
  globaltype = wasm_globaltype_new(valtype, js_truthy(js, mutable_val) ? WASM_VAR : WASM_CONST);
  global = globaltype ? wasm_global_new(store, globaltype, &initial) : NULL;

  wasm_globaltype_delete(globaltype);

  if (!global) {
    wasm_store_delete(store);
    return js_throw(js, wasm_make_runtime_error(js, "Failed to create WebAssembly.Global"));
  }

  result = wasm_wrap_extern_object(
    js, WASM_EXTERN_WRAP_GLOBAL, g_wasm_global_proto, BRAND_WASM_GLOBAL,
    store, true, global, js_mkundef()
  );
  if (vtype(result) == T_OBJ) {
    wasm_extern_handle_t *handle = wasm_extern_handle(result, WASM_EXTERN_WRAP_GLOBAL);
    if (handle) {
      handle->use_cached_value = true;
      handle->cached_value = initial;
    }
    js_set_finalizer(result, wasm_extern_finalize);
  }
  return result;
}

static bool wasm_to_page_count(ant_t *js, ant_value_t value, uint32_t *out) {
  double number = js_to_number(js, value);
  if (!isfinite(number) || number < 0 || floor(number) != number || number > UINT32_MAX)
    return false;
  *out = (uint32_t)number;
  return true;
}

static bool wasm_parse_memory_descriptor(ant_t *js, ant_value_t descriptor, wasm_limits_t *limits, ant_value_t *err) {
  ant_value_t initial_val, maximum_val, shared_val;
  uint32_t initial_pages, maximum_pages = wasm_limits_max_default;

  if (!is_object_type(descriptor)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Memory requires a descriptor object");
    return false;
  }

  initial_val = js_get(js, descriptor, "initial");
  if (vtype(initial_val) == T_UNDEF || !wasm_to_page_count(js, initial_val, &initial_pages)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Memory descriptor requires a valid initial page count");
    return false;
  }

  maximum_val = js_get(js, descriptor, "maximum");
  if (vtype(maximum_val) != T_UNDEF) {
    if (!wasm_to_page_count(js, maximum_val, &maximum_pages)) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Memory descriptor maximum must be a valid page count");
      return false;
    }
    if (maximum_pages < initial_pages) {
      *err = js_mkerr_typed(js, JS_ERR_RANGE, "WebAssembly.Memory maximum must be greater than or equal to initial");
      return false;
    }
  }

  shared_val = js_get(js, descriptor, "shared");
  if (js_truthy(js, shared_val)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "Shared WebAssembly.Memory is not supported");
    return false;
  }

  limits->min = initial_pages;
  limits->max = maximum_pages;
  *err = js_mkundef();
  return true;
}

static void wasm_detach_external_arraybuffer(ant_t *js, ant_value_t buffer) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(buffer);
  if (!data || data->is_detached) return;
  data->is_detached = 1;
  data->data = NULL;
  data->length = 0;
  data->capacity = 0;
  if (is_object_type(buffer)) js_set(js, buffer, "byteLength", js_mknum(0));
}

static ant_value_t wasm_make_external_arraybuffer(ant_t *js, uint8_t *data, size_t len, ant_value_t owner) {
  ArrayBufferData *buffer = calloc(1, sizeof(ArrayBufferData));
  if (!buffer) return js_mkerr(js, "out of memory");
  buffer->data = data;
  buffer->length = len;
  buffer->capacity = len;
  buffer->ref_count = 0;
  ant_value_t result = create_arraybuffer_obj(js, buffer);
  if (is_object_type(owner) && is_object_type(result)) js_set_slot_wb(js, result, SLOT_ENTRIES, owner);
  return result;
}

static bool wasm_parse_table_descriptor(ant_t *js, ant_value_t descriptor, wasm_limits_t *limits, wasm_valkind_t *element, ant_value_t *err) {
  ant_value_t initial_val, maximum_val;
  const char *element_name;
  ant_offset_t element_len = 0;
  uint32_t initial, maximum = wasm_limits_max_default;

  if (!is_object_type(descriptor)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table requires a descriptor object");
    return false;
  }

  element_name = get_str_prop(js, descriptor, "element", 7, &element_len);
  if (!element_name) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table descriptor requires an element type");
    return false;
  }
  if (element_len == 7 && memcmp(element_name, "anyfunc", 7) == 0) {
    *element = WASM_FUNCREF;
  } else {
    bool ok = false;
    *element = wasm_valkind_from_string(element_name, element_len, &ok);
    if (!ok || (*element != WASM_FUNCREF && *element != WASM_EXTERNREF)) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported WebAssembly.Table element type");
      return false;
    }
  }

  initial_val = js_get(js, descriptor, "initial");
  if (vtype(initial_val) == T_UNDEF || !wasm_to_page_count(js, initial_val, &initial)) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table descriptor requires a valid initial length");
    return false;
  }

  maximum_val = js_get(js, descriptor, "maximum");
  if (vtype(maximum_val) != T_UNDEF) {
    if (!wasm_to_page_count(js, maximum_val, &maximum)) {
      *err = js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table descriptor maximum must be a valid length");
      return false;
    }
    if (maximum < initial) {
      *err = js_mkerr_typed(js, JS_ERR_RANGE, "WebAssembly.Table maximum must be greater than or equal to initial");
      return false;
    }
  }

  limits->min = initial;
  limits->max = maximum;
  *err = js_mkundef();
  return true;
}

static bool wasm_table_value_ok(ant_t *js, wasm_valkind_t element, ant_value_t value, ant_value_t *err) {
  if (element == WASM_EXTERNREF) return true;
  if (vtype(value) == T_NULL || vtype(value) == T_UNDEF) return true;
  if (is_callable(value)) {
    ant_value_t state = js_get_slot(value, SLOT_DATA);
    wasm_func_handle_t *func_handle = is_object_type(state)
      ? (wasm_func_handle_t *)js_get_native(state, WASM_FUNC_STATE_TAG)
      : NULL;
    if (func_handle && func_handle->func) return true;
  }
  *err = js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table expects a WebAssembly function or null");
  return false;
}

static ant_value_t wasm_table_entry_get(ant_t *js, ant_value_t entries, uint32_t index) {
  char key[16];
  snprintf(key, sizeof(key), "%u", index);
  return js_get(js, entries, key);
}

static void wasm_table_entry_set(ant_t *js, ant_value_t entries, uint32_t index, ant_value_t value) {
  char key[16];
  snprintf(key, sizeof(key), "%u", index);
  js_set(js, entries, key, value);
}

static ant_value_t js_wasm_memory_buffer_getter(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_MEMORY);
  byte_t *data; size_t len;

  if (!handle || !handle->as.memory)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Memory");

  data = wasm_memory_data(handle->as.memory);
  len = wasm_memory_data_size(handle->as.memory);
  ArrayBufferData *cached = buffer_get_arraybuffer_data(handle->memory_buffer);
  if (cached && !cached->is_detached && cached->data == (uint8_t *)data && cached->length == len)
    return handle->memory_buffer;
  if (cached) wasm_detach_external_arraybuffer(js, handle->memory_buffer);

  ant_value_t buffer = wasm_make_external_arraybuffer(js, (uint8_t *)data, len, js->this_val);
  if (is_err(buffer)) return buffer;

  handle->memory_buffer = buffer;
  js_set_slot_wb(js, js->this_val, SLOT_AUX, buffer);
  return buffer;
}

static ant_value_t js_wasm_memory_grow(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_MEMORY);
  wasm_memory_pages_t old_size;
  uint32_t delta;

  if (!handle || !handle->as.memory)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Memory");

  if (!wasm_to_page_count(js, nargs > 0 ? args[0] : js_mknum(0), &delta))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Memory.grow requires a valid page count");

  old_size = wasm_memory_size(handle->as.memory);

  if (delta == 0) return js_mknum((double)old_size);

  if (!wasm_memory_grow(handle->as.memory, delta))
    return js_mkerr_typed(js, JS_ERR_RANGE, "Failed to grow memory by %u pages", delta);

  wasm_detach_external_arraybuffer(js, handle->memory_buffer);
  handle->memory_buffer = js_mkundef();
  js_set_slot_wb(js, js->this_val, SLOT_AUX, js_mkundef());

  return js_mknum((double)old_size);
}

static ant_value_t js_wasm_memory_ctor(ant_t *js, ant_value_t *args, int nargs) {
  wasm_limits_t limits;
  wasm_store_t *store = NULL;
  wasm_memorytype_t *memorytype = NULL;
  wasm_memory_t *memory = NULL;
  ant_value_t err, result;

  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Memory constructor requires 'new'");
  if (!ensure_wasm_engine())
    return js_mkerr(js, "Failed to initialize WebAssembly engine");
  if (!wasm_parse_memory_descriptor(js, nargs > 0 ? args[0] : js_mkundef(), &limits, &err))
    return err;

  store = wasm_store_new(g_wasm_engine);
  if (!store) return js_mkerr(js, "Failed to create WebAssembly store");

  memorytype = wasm_memorytype_new(&limits);
  memory = memorytype ? wasm_memory_new(store, memorytype) : NULL;
  wasm_memorytype_delete(memorytype);

  if (!memory) {
    wasm_store_delete(store);
    return js_throw(js, wasm_make_runtime_error(js, "Failed to create WebAssembly.Memory"));
  }

  result = wasm_wrap_extern_object(
    js, WASM_EXTERN_WRAP_MEMORY, g_wasm_memory_proto, BRAND_WASM_MEMORY,
    store, true, memory, js_mkundef()
  );
  if (vtype(result) != T_OBJ) {
    wasm_memory_delete(memory);
    wasm_store_delete(store);
    return result;
  }

  wasm_extern_handle_t *handle = wasm_extern_handle(result, WASM_EXTERN_WRAP_MEMORY);
  if (!handle) {
    wasm_memory_delete(memory);
    wasm_store_delete(store);
    return js_mkerr(js, "out of memory");
  }

  js_set_finalizer(result, wasm_extern_finalize);
  return result;
}

static ant_value_t js_wasm_table_length_getter(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_TABLE);
  if (!handle || !handle->as.table)  return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Table");
  if (handle->standalone_table && handle->standalone_table_element != WASM_FUNCREF)
    return js_mknum((double)handle->standalone_table_size);
  return js_mknum((double)wasm_table_size(handle->as.table));
}

static ant_value_t js_wasm_table_get(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_TABLE);
  wasm_ref_t *ref;
  wasm_func_t *func;
  uint32_t index;

  if (!handle || !handle->as.table)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Table");

  if (!wasm_to_page_count(js, nargs > 0 ? args[0] : js_mknum(0), &index))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table.get requires a valid index");

  if (handle->standalone_table && handle->standalone_table_element != WASM_FUNCREF) {
    ant_value_t entries;
    if (index >= handle->standalone_table_size)
      return js_mkerr_typed(js, JS_ERR_RANGE, "WebAssembly.Table index is out of bounds");
    entries = js_get(js, js->this_val, "__wasmTableEntries");
    return wasm_table_entry_get(js, entries, index);
  }

  {
    ant_value_t entries = js_get(js, js->this_val, "__wasmTableEntries");
    if (is_object_type(entries)) {
      ant_value_t retained = wasm_table_entry_get(js, entries, index);
      if (vtype(retained) != T_UNDEF && vtype(retained) != T_NULL)
        return retained;
    }
  }

  ref = wasm_table_get(handle->as.table, index);

  if (!ref) return js_mknull();
  func = wasm_ref_as_func(ref);
  
  if (func) {
    ant_value_t owner = wasm_find_instance_owner(func->inst_comm_rt);
    if (!is_object_type(owner))
      owner = js_get_slot(js->this_val, SLOT_ENTRIES);
    ant_value_t wrapped = wasm_wrap_func(js, func, owner, true);
    wasm_ref_delete(ref);
    return wrapped;
  }
  
  wasm_ref_delete(ref);
  return js_mknull();
}

static ant_value_t js_wasm_table_set(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_TABLE);
  wasm_ref_t *ref = NULL;
  uint32_t index;
  ant_value_t err;

  if (!handle || !handle->as.table) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Table");
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table.set requires 2 arguments");

  if (!wasm_to_page_count(js, args[0], &index))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table.set requires a valid index");

  if (handle->standalone_table && handle->standalone_table_element != WASM_FUNCREF) {
    ant_value_t entries;
    if (index >= handle->standalone_table_size)
      return js_mkerr_typed(js, JS_ERR_RANGE, "WebAssembly.Table index is out of bounds");
    if (!wasm_table_value_ok(js, handle->standalone_table_element, args[1], &err))
      return err;
    entries = js_get(js, js->this_val, "__wasmTableEntries");
    wasm_table_entry_set(js, entries, index, args[1]);
    return js_mkundef();
  }

  if (!(vtype(args[1]) == T_NULL || vtype(args[1]) == T_UNDEF)) {
    ant_value_t state;
    wasm_func_handle_t *func_handle;
    
    if (!is_callable(args[1]))
      return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table.set expects a WebAssembly function or null");
    
    state = js_get_slot(args[1], SLOT_DATA);
    func_handle = is_object_type(state)
      ? (wasm_func_handle_t *)js_get_native(state, WASM_FUNC_STATE_TAG)
      : NULL;
    if (!func_handle || !func_handle->func)
      return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table.set expects a WebAssembly function or null");
    
    ref = wasm_func_as_ref(func_handle->func);
    if (!ref) return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to materialize WebAssembly function reference");
  }

  if (!wasm_table_set(handle->as.table, index, ref)) {
    if (ref) wasm_ref_delete(ref);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to update WebAssembly.Table");
  }

  {
    ant_value_t entries = js_get(js, js->this_val, "__wasmTableEntries");
    if (is_object_type(entries))
      wasm_table_entry_set(js, entries, index,
                           vtype(args[1]) == T_UNDEF ? js_mknull()
                                                      : args[1]);
  }
  
  return js_mkundef();
}

static ant_value_t js_wasm_table_grow(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_TABLE);
  uint32_t delta, old_size, needed;
  ant_value_t init, err, entries;
  wasm_ref_t *ref = NULL;

  if (!handle || !handle->as.table)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Table");
  if (!wasm_to_page_count(js, nargs > 0 ? args[0] : js_mknum(0), &delta))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table.grow requires a valid length");

  if (!handle->standalone_table)
    return js_mkerr_typed(js, JS_ERR_TYPE, "The current WAMR backend does not support host-side table.grow");

  init = nargs > 1 ? args[1] : js_mknull();
  if (!wasm_table_value_ok(js, handle->standalone_table_element, init, &err))
    return err;

  if (handle->standalone_table_element == WASM_FUNCREF) {
    if (!(vtype(init) == T_NULL || vtype(init) == T_UNDEF)) {
      ant_value_t state;
      wasm_func_handle_t *func_handle;

      state = js_get_slot(init, SLOT_DATA);
      func_handle = is_object_type(state)
        ? (wasm_func_handle_t *)js_get_native(state, WASM_FUNC_STATE_TAG)
        : NULL;
      if (!func_handle || !func_handle->func)
        return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table.grow expects a WebAssembly function or null");

      ref = wasm_func_as_ref(func_handle->func);
      if (!ref) return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to materialize WebAssembly function reference");
    }

    old_size = wasm_table_size(handle->as.table);
    if (!wasm_table_grow(handle->as.table, delta, ref)) {
      if (ref) wasm_ref_delete(ref);
      return js_mkerr_typed(js, JS_ERR_RANGE, "Failed to grow WebAssembly.Table");
    }
    entries = js_get(js, js->this_val, "__wasmTableEntries");
    if (is_object_type(entries)) {
      for (uint32_t i = old_size; i < old_size + delta; i++)
        wasm_table_entry_set(js, entries, i,
                             vtype(init) == T_UNDEF ? js_mknull() : init);
    }
    handle->standalone_table_size = wasm_table_size(handle->as.table);
    return js_mknum((double)old_size);
  }

  old_size = handle->standalone_table_size;
  if (delta == 0) return js_mknum((double)old_size);
  if (delta > UINT32_MAX - old_size)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Failed to grow WebAssembly.Table");
  needed = old_size + delta;
  if (needed > handle->standalone_table_max_size)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Failed to grow WebAssembly.Table");

  entries = js_get(js, js->this_val, "__wasmTableEntries");
  for (uint32_t i = old_size; i < needed; i++) wasm_table_entry_set(js, entries, i, init);
  handle->standalone_table_size = needed;
  return js_mknum((double)old_size);
}

static ant_value_t js_wasm_table_ctor(ant_t *js, ant_value_t *args, int nargs) {
  wasm_limits_t limits;
  wasm_valkind_t element;
  wasm_store_t *store = NULL;
  wasm_valtype_t *valtype = NULL;
  wasm_tabletype_t *tabletype = NULL;
  wasm_table_t *table = NULL;
  ant_value_t err, result;

  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table constructor requires 'new'");
  if (!ensure_wasm_engine())
    return js_mkerr(js, "Failed to initialize WebAssembly engine");
  if (!wasm_parse_table_descriptor(js, nargs > 0 ? args[0] : js_mkundef(), &limits, &element, &err))
    return err;

  store = wasm_store_new(g_wasm_engine);
  if (!store) return js_mkerr(js, "Failed to create WebAssembly store");

  valtype = wasm_valtype_new(element);
  tabletype = valtype ? wasm_tabletype_new(valtype, &limits) : NULL;
  table = tabletype ? wasm_table_new(store, tabletype, NULL) : NULL;
  wasm_tabletype_delete(tabletype);

  if (!table) {
    wasm_store_delete(store);
    return js_throw(js, wasm_make_runtime_error(js, "Failed to create WebAssembly.Table"));
  }

  result = wasm_wrap_extern_object(
    js, WASM_EXTERN_WRAP_TABLE, g_wasm_table_proto, BRAND_WASM_TABLE,
    store, true, table, js_mkundef()
  );
  if (vtype(result) == T_OBJ) {
    ant_value_t entries = js_mkobj(js);
    wasm_extern_handle_t *handle = wasm_extern_handle(result, WASM_EXTERN_WRAP_TABLE);
    if (!handle || is_err(entries)) {
      js_clear_native(result, WASM_EXTERN_NATIVE_TAG);
      wasm_table_delete(table);
      wasm_store_delete(store);
      free(handle);
      return is_err(entries) ? entries : js_mkerr(js, "out of memory");
    }

    handle->standalone_table = true;
    handle->standalone_table_element = element;
    handle->standalone_table_size = limits.min;
    handle->standalone_table_max_size = limits.max;
    for (uint32_t i = 0; i < limits.min; i++) wasm_table_entry_set(js, entries, i, js_mknull());
    js_set(js, result, "__wasmTableEntries", entries);
    js_set_descriptor(js, result, "__wasmTableEntries", 18, 0);
    js_set_finalizer(result, wasm_extern_finalize);
  }

  return result;
}

static ant_value_t js_wasm_tag_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "The current WAMR backend does not expose WebAssembly.Tag");
}

static ant_value_t js_wasm_exception_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "The current WAMR backend does not expose WebAssembly.Exception");
}

static ant_value_t js_wasm_validate(ant_t *js, ant_value_t *args, int nargs) {
  wasm_byte_vec_t binary = WASM_EMPTY_VEC;
  wasm_store_t *store;
  
  bool ok;
  char error_buf[128] = {0};
  bool suppress_wasi_warning = false;

  if (nargs < 1) return js_false;
  if (!ensure_wasm_engine()) return js_false;
  if (!(store = wasm_store_new(g_wasm_engine))) return js_false;
  
  if (!wasm_buffer_source_to_vec(js, args[0], &binary, error_buf, sizeof(error_buf))) {
    wasm_store_delete(store);
    return js_false;
  }

  suppress_wasi_warning = wasi_bytes_need_wasi_command_warning_suppression(
    (const uint8_t *)binary.data, binary.size
  );
  
  if (suppress_wasi_warning) wasm_runtime_set_log_level(WASM_LOG_LEVEL_ERROR);
  ok = wasm_module_validate(store, &binary);
  
  if (suppress_wasi_warning) wasm_runtime_set_log_level(WASM_LOG_LEVEL_WARNING);
  wasm_byte_vec_delete(&binary);
  wasm_store_delete(store);
  
  return js_bool(ok);
}

static ant_value_t js_wasm_compile(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t module = js_mkundef();
  ant_value_t err;

  if (nargs < 1) {
    wasm_reject_with_error(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.compile requires 1 argument"));
    return promise;
  }

  err = wasm_module_from_bytes(js, args[0], &module);
  if (is_err(err) || vtype(module) != T_OBJ) {
    wasm_reject_with_error(js, promise, wasm_error_value(js, err));
    return promise;
  }

  js_resolve_promise(js, promise, module);
  return promise;
}

static ant_value_t js_wasm_instantiate(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t module = js_mkundef();
  ant_value_t instance = js_mkundef();
  ant_value_t import_obj = (nargs >= 2 && is_object_type(args[1])) ? args[1] : js_mkundef();
  ant_value_t err;

  if (nargs < 1) {
    wasm_reject_with_error(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.instantiate requires 1 argument"));
    return promise;
  }

  if (wasm_module_handle(args[0])) {
    err = wasm_instantiate_module(js, args[0], import_obj, &instance);
    if (is_err(err) || vtype(instance) != T_OBJ) {
      wasm_reject_with_error(js, promise, wasm_error_value(js, err));
      return promise;
    }
    js_resolve_promise(js, promise, instance);
    return promise;
  }

  err = wasm_module_from_bytes(js, args[0], &module);
  if (is_err(err) || vtype(module) != T_OBJ) {
    wasm_reject_with_error(js, promise, wasm_error_value(js, err));
    return promise;
  }

  err = wasm_instantiate_module(js, module, import_obj, &instance);
  if (is_err(err) || vtype(instance) != T_OBJ) {
    wasm_reject_with_error(js, promise, wasm_error_value(js, err));
    return promise;
  }

  ant_value_t result = js_mkobj(js);
  js_set(js, result, "module", module);
  js_set(js, result, "instance", instance);
  js_resolve_promise(js, promise, result);
  
  return promise;
}

static ant_value_t js_wasm_compile_error_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t msg = (nargs > 0) ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(msg)) return msg;
  return wasm_make_error(js, g_wasm_compileerror_proto, "CompileError", js_str(js, msg));
}

static ant_value_t js_wasm_link_error_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t msg = (nargs > 0) ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(msg)) return msg;
  return wasm_make_error(js, g_wasm_linkerror_proto, "LinkError", js_str(js, msg));
}

static ant_value_t js_wasm_runtime_error_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t msg = (nargs > 0) ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(msg)) return msg;
  return wasm_make_error(js, g_wasm_runtimeerror_proto, "RuntimeError", js_str(js, msg));
}

void gc_mark_wasm(ant_t *js, gc_mark_fn mark) {
  for (size_t i = 0; i < g_wasm_import_env_count; i++) {
    wasm_import_func_env_t *env = g_wasm_import_envs[i];
    mark(js, env->fn);
    mark(js, env->owner);
  }
  
  if (g_wasm_pending_import_throw_exists)
    mark(js, g_wasm_pending_import_throw);
}

void init_wasm_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);
  
  ant_value_t error_proto = js_get_ctor_proto(js, "Error", 5);
  ant_value_t ns = js_mkobj(js);

  if (!ensure_wasm_engine()) return;

  g_wasm_module_proto = js_mkobj(js);
  g_wasm_instance_proto = js_mkobj(js);
  g_wasm_global_proto = js_mkobj(js);
  g_wasm_memory_proto = js_mkobj(js);
  g_wasm_table_proto = js_mkobj(js);
  g_wasm_tag_proto = js_mkobj(js);
  g_wasm_exception_proto = js_mkobj(js);

  g_wasm_compileerror_proto = js_mkobj(js);
  g_wasm_linkerror_proto = js_mkobj(js);
  g_wasm_runtimeerror_proto = js_mkobj(js);

  js_set_proto_init(g_wasm_module_proto, js->sym.object_proto);
  js_set_proto_init(g_wasm_instance_proto, js->sym.object_proto);
  js_set_proto_init(g_wasm_global_proto, js->sym.object_proto);
  js_set_proto_init(g_wasm_memory_proto, js->sym.object_proto);
  js_set_proto_init(g_wasm_table_proto, js->sym.object_proto);
  js_set_proto_init(g_wasm_tag_proto, js->sym.object_proto);
  js_set_proto_init(g_wasm_exception_proto, js->sym.object_proto);

  js_set_proto_init(g_wasm_compileerror_proto, error_proto);
  js_set_proto_init(g_wasm_linkerror_proto, error_proto);
  js_set_proto_init(g_wasm_runtimeerror_proto, error_proto);

  js_set(js, g_wasm_global_proto, "valueOf", js_mkfun(js_wasm_global_value_of));
  js_set_getter_desc(js, g_wasm_global_proto, "value", 5, js_mkfun(js_wasm_global_value_getter), JS_DESC_C);
  js_set_setter_desc(js, g_wasm_global_proto, "value", 5, js_mkfun(js_wasm_global_value_setter), JS_DESC_C);

  js_set_getter_desc(js, g_wasm_instance_proto, "exports", 7, js_mkfun(js_wasm_instance_exports_getter), JS_DESC_C);
  js_set_getter_desc(js, g_wasm_memory_proto, "buffer", 6, js_mkfun(js_wasm_memory_buffer_getter), JS_DESC_C);
  js_set(js, g_wasm_memory_proto, "grow", js_mkfun(js_wasm_memory_grow));

  js_set_getter_desc(js, g_wasm_table_proto, "length", 6, js_mkfun(js_wasm_table_length_getter), JS_DESC_C);
  js_set(js, g_wasm_table_proto, "get", js_mkfun(js_wasm_table_get));
  js_set(js, g_wasm_table_proto, "set", js_mkfun(js_wasm_table_set));
  js_set(js, g_wasm_table_proto, "grow", js_mkfun(js_wasm_table_grow));

  ant_value_t module_ctor = js_make_ctor(js, js_wasm_module_ctor, g_wasm_module_proto, "Module", 6);
  ant_value_t instance_ctor = js_make_ctor(js, js_wasm_instance_ctor, g_wasm_instance_proto, "Instance", 8);
  ant_value_t global_ctor = js_make_ctor(js, js_wasm_global_ctor, g_wasm_global_proto, "Global", 6);
  ant_value_t memory_ctor = js_make_ctor(js, js_wasm_memory_ctor, g_wasm_memory_proto, "Memory", 6);
  ant_value_t table_ctor = js_make_ctor(js, js_wasm_table_ctor, g_wasm_table_proto, "Table", 5);
  ant_value_t tag_ctor = js_make_ctor(js, js_wasm_tag_ctor, g_wasm_tag_proto, "Tag", 3);
  ant_value_t exception_ctor = js_make_ctor(js, js_wasm_exception_ctor, g_wasm_exception_proto, "Exception", 9);

  ant_value_t compile_error_ctor = js_make_ctor(js, js_wasm_compile_error_ctor, g_wasm_compileerror_proto, "CompileError", 12);
  ant_value_t link_error_ctor = js_make_ctor(js, js_wasm_link_error_ctor, g_wasm_linkerror_proto, "LinkError", 9);
  ant_value_t runtime_error_ctor = js_make_ctor(js, js_wasm_runtime_error_ctor, g_wasm_runtimeerror_proto, "RuntimeError", 12);

  js_set(js, module_ctor, "imports", js_mkfun(js_wasm_module_imports));
  js_set(js, module_ctor, "exports", js_mkfun(js_wasm_module_exports));
  js_set(js, module_ctor, "customSections", js_mkfun(js_wasm_module_custom_sections));

  js_set(js, ns, "validate", js_mkfun(js_wasm_validate));
  js_set(js, ns, "compile", js_mkfun(js_wasm_compile));
  js_set(js, ns, "instantiate", js_mkfun(js_wasm_instantiate));

  js_set(js, ns, "Module", module_ctor);
  js_set(js, ns, "Instance", instance_ctor);
  js_set(js, ns, "Global", global_ctor);
  js_set(js, ns, "Memory", memory_ctor);
  js_set(js, ns, "Table", table_ctor);
  js_set(js, ns, "Tag", tag_ctor);
  js_set(js, ns, "Exception", exception_ctor);
  js_set(js, ns, "CompileError", compile_error_ctor);
  js_set(js, ns, "LinkError", link_error_ctor);
  js_set(js, ns, "RuntimeError", runtime_error_ctor);

  js_set(js, global, "WebAssembly", ns);
  js_set_descriptor(js, global, "WebAssembly", 11, JS_DESC_W | JS_DESC_C);
}
