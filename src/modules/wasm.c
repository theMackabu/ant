#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <wasm_c_api.h>
#include <wasm_export.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundef"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#include <wasm_c_api_internal.h>
#include <wasm_runtime.h>
#pragma clang diagnostic pop

#include "ant.h"
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
} wasm_module_handle_t;

typedef struct {
  wasm_instance_t *instance;
  wasm_extern_vec_t exports;
  wasm_func_t **host_funcs;
  size_t host_func_count;
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
  wasm_val_t cached_value;
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

static wasm_engine_t *g_wasm_engine = NULL;

static wasm_import_func_env_t **g_wasm_import_envs = NULL;
static size_t g_wasm_import_env_count = 0;
static size_t g_wasm_import_env_cap = 0;

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

static ant_value_t g_wasm_module_proto = 0;
static ant_value_t g_wasm_instance_proto = 0;
static ant_value_t g_wasm_global_proto = 0;
static ant_value_t g_wasm_memory_proto = 0;
static ant_value_t g_wasm_table_proto = 0;
static ant_value_t g_wasm_tag_proto = 0;
static ant_value_t g_wasm_exception_proto = 0;

static ant_value_t g_wasm_compileerror_proto = 0;
static ant_value_t g_wasm_linkerror_proto = 0;
static ant_value_t g_wasm_runtimeerror_proto = 0;

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
  }
}

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
  ant_value_t slot = js_get_slot(value, SLOT_DATA);
  if (vtype(slot) != T_NUM) return NULL;
  return (wasm_module_handle_t *)(uintptr_t)(size_t)js_getnum(slot);
}

static wasm_instance_handle_t *wasm_instance_handle(ant_value_t value) {
  if (!js_check_brand(value, BRAND_WASM_INSTANCE)) return NULL;
  ant_value_t slot = js_get_slot(value, SLOT_DATA);
  if (vtype(slot) != T_NUM) return NULL;
  return (wasm_instance_handle_t *)(uintptr_t)(size_t)js_getnum(slot);
}

static wasm_extern_handle_t *wasm_extern_handle(ant_value_t value, wasm_extern_wrap_kind_t kind) {
  if ((kind == WASM_EXTERN_WRAP_GLOBAL && !js_check_brand(value, BRAND_WASM_GLOBAL))
      || (kind == WASM_EXTERN_WRAP_MEMORY && !js_check_brand(value, BRAND_WASM_MEMORY))
      || (kind == WASM_EXTERN_WRAP_TABLE && !js_check_brand(value, BRAND_WASM_TABLE))) {
    return NULL;
  }

  ant_value_t slot = js_get_slot(value, SLOT_DATA);
  if (vtype(slot) != T_NUM) return NULL;
  wasm_extern_handle_t *handle = (wasm_extern_handle_t *)(uintptr_t)(size_t)js_getnum(slot);
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
    case WASM_FUNCREF:
      return value->of.ref ? js_mkundef() : js_mknull();
    default:
      return js_mkundef();
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
    case WASM_FUNCREF:
      out->of.ref = NULL;
      return vtype(value) == T_NULL || vtype(value) == T_UNDEF;
    default:
      return false;
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
  js_set_slot(obj, SLOT_DATA, ANT_PTR(handle));
  return obj;
}

static void wasm_module_finalize(ant_t *js, ant_object_t *obj) {
  (void)js;
  if (!obj->extra_slots) return;

  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
    if (entries[i].slot != SLOT_DATA || vtype(entries[i].value) != T_NUM) continue;
    wasm_module_handle_t *handle = (wasm_module_handle_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
    if (!handle) return;
    if (handle->module) wasm_module_delete(handle->module);
    if (handle->store) wasm_store_delete(handle->store);
    free(handle);
    return;
  }
}

static ant_value_t wasm_wrap_instance(ant_t *js, wasm_instance_handle_t *handle, ant_value_t module_ref) {
  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, g_wasm_instance_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_WASM_INSTANCE));
  js_set_slot(obj, SLOT_DATA, ANT_PTR(handle));
  js_set_slot_wb(js, obj, SLOT_CTOR, module_ref);
  return obj;
}

static void wasm_instance_finalize(ant_t *js, ant_object_t *obj) {
  (void)js;
  if (!obj->extra_slots) return;

  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
    if (entries[i].slot != SLOT_DATA || vtype(entries[i].value) != T_NUM) continue;
    wasm_instance_handle_t *handle = (wasm_instance_handle_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
    if (!handle) return;
    for (size_t j = 0; j < handle->host_func_count; j++) {
      if (handle->host_funcs[j]) wasm_func_delete(handle->host_funcs[j]);
    }
    free(handle->host_funcs);
    wasm_extern_vec_delete(&handle->exports);
    free(handle);
    return;
  }
}

static ant_value_t wasm_wrap_extern_object(ant_t *js, wasm_extern_wrap_kind_t kind, ant_value_t proto, int brand, wasm_store_t *store, bool own_handle, void *ptr, ant_value_t owner) {
  wasm_extern_handle_t *handle = calloc(1, sizeof(*handle));
  if (!handle) return js_mkerr(js, "out of memory");

  handle->kind = kind;
  handle->store = store;
  handle->own_handle = own_handle;
  handle->cached_value = (wasm_val_t)WASM_INIT_VAL;

  switch (kind) {
    case WASM_EXTERN_WRAP_GLOBAL: handle->as.global = (wasm_global_t *)ptr; break;
    case WASM_EXTERN_WRAP_MEMORY: handle->as.memory = (wasm_memory_t *)ptr; break;
    case WASM_EXTERN_WRAP_TABLE: handle->as.table = (wasm_table_t *)ptr; break;
  }

  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(brand));
  js_set_slot(obj, SLOT_DATA, ANT_PTR(handle));
  if (is_object_type(owner)) js_set_slot_wb(js, obj, SLOT_ENTRIES, owner);
  return obj;
}

static void wasm_extern_finalize(ant_t *js, ant_object_t *obj) {
  (void)js;
  if (!obj->extra_slots) return;

  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
    if (entries[i].slot != SLOT_DATA || vtype(entries[i].value) != T_NUM) continue;
    wasm_extern_handle_t *handle = (wasm_extern_handle_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
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
    return;
  }
}

static ant_value_t js_wasm_exported_func_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  if (vtype(state) != T_OBJ) return js_mkerr(js, "Invalid WebAssembly function");

  ant_value_t func_ptr = js_get_slot(state, SLOT_DATA);
  if (vtype(func_ptr) != T_NUM) return js_mkerr(js, "Invalid WebAssembly function");

  wasm_func_t *func = (wasm_func_t *)(uintptr_t)(size_t)js_getnum(func_ptr);
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

  trap = wasm_func_call(func, &wasm_args, &wasm_results);
  if (trap) {
    result = wasm_trap_to_error(js, trap);
    wasm_val_vec_delete(&wasm_args);
    wasm_val_vec_delete(&wasm_results);
    wasm_functype_delete(type);
    return js_throw(js, result);
  }

  result = wasm_js_from_result_vec(js, &wasm_results);

  wasm_val_vec_delete(&wasm_args);
  wasm_val_vec_delete(&wasm_results);
  wasm_functype_delete(type);
  return result;
}

static ant_value_t wasm_wrap_export_value(ant_t *js, ant_value_t instance_obj, const wasm_exporttype_t *export_type, wasm_extern_t *external) {
  switch (wasm_extern_kind(external)) {
    case WASM_EXTERN_FUNC: {
      ant_value_t state = js_mkobj(js);
      js_set_slot(state, SLOT_DATA, ANT_PTR(wasm_extern_as_func(external)));
      js_set_slot_wb(js, state, SLOT_ENTRIES, instance_obj);
      return js_heavy_mkfun(js, js_wasm_exported_func_call, state);
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

  *out_module = js_mkundef();

  if (!ensure_wasm_engine()) return js_mkerr(js, "Failed to initialize WebAssembly engine");
  if (!(store = wasm_store_new(g_wasm_engine))) return js_mkerr(js, "Failed to create WebAssembly store");

  if (!wasm_buffer_source_to_vec(js, value, &binary, error_buf, sizeof(error_buf))) {
    wasm_store_delete(store);
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s", error_buf);
  }

  if (!wasm_module_validate(store, &binary)) {
    wasm_byte_vec_delete(&binary);
    wasm_store_delete(store);
    return wasm_make_compile_error(js, "Invalid WebAssembly binary");
  }

  module = wasm_module_new(store, &binary);
  wasm_byte_vec_delete(&binary);
  if (!module) {
    wasm_store_delete(store);
    return wasm_make_compile_error(js, "Failed to compile WebAssembly module");
  }

  *out_module = wasm_wrap_module(js, store, module);
  if (vtype(*out_module) == T_OBJ) {
    js_set_finalizer(*out_module, wasm_module_finalize);
    js_set_slot_wb(js, *out_module, SLOT_MAP, value);
  }
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

  result = sv_vm_call(js->vm, js, env->fn, js_mkundef(), js_args, args ? (int)args->size : 0, NULL, false);
  free(js_args);

  if (is_err(result)) {
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
  
  size_t owned_host_func_count = 0;
  wasm_extern_vec_t exports = WASM_EMPTY_VEC;
  
  wasm_trap_t *trap = NULL;
  wasm_instance_t *instance = NULL;
  ant_value_t instance_obj = js_mkundef();
  ant_value_t exports_obj = js_mkobj(js);

  *out_instance = js_mkundef();

  if (!module_handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Module");

  if (wasi_module_has_wasi_imports(module_handle->module)) {
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
    if (!imports || !owned_host_funcs) {
      free(imports);
      free(owned_host_funcs);
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

    if (kind == WASM_EXTERN_MEMORY || kind == WASM_EXTERN_TABLE) {
      free(imports);
      free(owned_host_funcs);
      wasm_importtype_vec_delete(&import_types);
      return wasm_make_link_error(js, "The current WAMR backend does not support memory/table imports");
    }

    if (kind == WASM_EXTERN_FUNC) {
      const wasm_functype_t *func_type = wasm_externtype_as_functype_const(extern_type);
      wasm_import_func_env_t *env = NULL;

      if (!is_callable(value)) {
        free(imports);
        free(owned_host_funcs);
        wasm_importtype_vec_delete(&import_types);
        return wasm_make_link_error(js, "Missing function import");
      }

      env = calloc(1, sizeof(*env));
      if (!env) {
        free(imports);
        free(owned_host_funcs);
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
        wasm_importtype_vec_delete(&import_types);
        return wasm_make_link_error(js, "Failed to create function import");
      }

      imports[i] = wasm_func_as_extern(owned_host_funcs[owned_host_func_count]);
      owned_host_func_count++;
      continue;
    }

    if (kind == WASM_EXTERN_GLOBAL) {
      wasm_extern_handle_t *handle = wasm_extern_handle(value, WASM_EXTERN_WRAP_GLOBAL);
      if (!handle || !handle->as.global) {
        free(imports);
        free(owned_host_funcs);
        wasm_importtype_vec_delete(&import_types);
        return wasm_make_link_error(js, "Missing global import");
      }
      imports[i] = wasm_global_as_extern(handle->as.global);
      continue;
    }
  }

  {
    wasm_extern_vec_t import_vec = WASM_EMPTY_VEC;
    if (imports && import_types.size > 0)
      import_vec = (wasm_extern_vec_t){ import_types.size, imports, import_types.size, sizeof(*imports), NULL };

    instance = wasm_instance_new_with_args(module_handle->store, module_handle->module, &import_vec, &trap, KILOBYTE(32), 0);
  }

  free(imports);
  wasm_importtype_vec_delete(&import_types);

  if (!instance) {
    for (size_t i = 0; i < owned_host_func_count; i++) {
      if (owned_host_funcs[i]) wasm_func_delete(owned_host_funcs[i]);
    }
    free(owned_host_funcs);
    if (trap) return wasm_trap_to_error(js, trap);
    return wasm_make_link_error(js, "Failed to instantiate WebAssembly module");
  }

  wasm_instance_exports(instance, &exports);
  wasm_module_exports(module_handle->module, &export_types);

  {
    wasm_instance_handle_t *inst_handle = calloc(1, sizeof(*inst_handle));
    if (!inst_handle) {
      wasm_extern_vec_delete(&exports);
      wasm_exporttype_vec_delete(&export_types);
      return js_mkerr(js, "out of memory");
    }
    inst_handle->instance = instance;
    inst_handle->exports = exports;
    inst_handle->host_funcs = owned_host_funcs;
    inst_handle->host_func_count = owned_host_func_count;

    instance_obj = wasm_wrap_instance(js, inst_handle, module_obj);
  }
  if (vtype(instance_obj) != T_OBJ) {
    wasm_exporttype_vec_delete(&export_types);
    return instance_obj;
  }
  js_set_finalizer(instance_obj, wasm_instance_finalize);

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
  if (handle->use_cached_value) return wasm_value_to_js(js, &handle->cached_value);

  wasm_global_get(handle->as.global, &value);
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

  if (handle->use_cached_value) handle->cached_value = value;
  else wasm_global_set(handle->as.global, &value);
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

static ant_value_t js_wasm_memory_buffer_getter(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_MEMORY);
  byte_t *data; size_t len; ArrayBufferData *buffer;

  if (!handle || !handle->as.memory)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Memory");

  data = wasm_memory_data(handle->as.memory);
  len = wasm_memory_data_size(handle->as.memory);
  buffer = calloc(1, sizeof(ArrayBufferData));
  
  if (!buffer) return js_mkerr(js, "out of memory");
  buffer->data = (uint8_t *)data;
  buffer->length = len;
  buffer->capacity = len;
  buffer->ref_count = 1;
  
  return create_arraybuffer_obj(js, buffer);
}

static ant_value_t js_wasm_memory_grow(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_MEMORY);
  wasm_memory_pages_t old_size;
  uint32_t delta;

  if (!handle || !handle->as.memory)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Memory");

  delta = (uint32_t)(nargs > 0 ? js_to_number(js, args[0]) : 0);
  old_size = wasm_memory_size(handle->as.memory);

  if (delta == 0) return js_mknum((double)old_size);

  wasm_module_inst_t inst = (wasm_module_inst_t)handle->as.memory->inst_comm_rt;
  if (!inst)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Memory instance not available");

  if (inst->module_type == Wasm_Module_Bytecode) {
    WASMModuleInstance *wasm_inst = (WASMModuleInstance *)inst;
    WASMMemoryInstance *mem_inst = wasm_inst->memories[handle->as.memory->memory_idx_rt];
    uint32_t needed = mem_inst->cur_page_count + delta;
    if (needed > mem_inst->max_page_count) mem_inst->max_page_count = needed;
  }

  if (!wasm_runtime_enlarge_memory(inst, (uint64_t)delta))
    return js_mkerr_typed(js, JS_ERR_RANGE, "Failed to grow memory by %u pages", delta);

  return js_mknum((double)old_size);
}

static ant_value_t js_wasm_memory_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Memory constructor requires 'new'");
  return js_mkerr_typed(js, JS_ERR_TYPE, "The current WAMR backend does not support standalone WebAssembly.Memory");
}

static ant_value_t js_wasm_table_length_getter(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_TABLE);
  if (!handle || !handle->as.table)  return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Table");
  return js_mknum((double)wasm_table_size(handle->as.table));
}

static ant_value_t js_wasm_table_get(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_TABLE);
  wasm_ref_t *ref;
  uint32_t index;

  if (!handle || !handle->as.table)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Table");

  index = (uint32_t)(nargs > 0 ? js_to_number(js, args[0]) : 0);
  ref = wasm_table_get(handle->as.table, index);
  
  if (!ref) return js_mknull();
  wasm_ref_delete(ref);
  
  return js_mknull();
}

static ant_value_t js_wasm_table_set(ant_t *js, ant_value_t *args, int nargs) {
  wasm_extern_handle_t *handle = wasm_extern_handle(js->this_val, WASM_EXTERN_WRAP_TABLE);
  uint32_t index;

  if (!handle || !handle->as.table) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Table");
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table.set requires 2 arguments");

  index = (uint32_t)js_to_number(js, args[0]);
  if (!wasm_table_set(handle->as.table, index, NULL)) return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to update WebAssembly.Table");
  
  return js_mkundef();
}

static ant_value_t js_wasm_table_grow(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "The current WAMR backend does not support host-side table.grow");
}

static ant_value_t js_wasm_table_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Table constructor requires 'new'");
  return js_mkerr_typed(js, JS_ERR_TYPE, "The current WAMR backend does not support standalone WebAssembly.Table");
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
  
  char error_buf[128] = {0};
  bool ok;

  if (nargs < 1) return js_false;
  if (!ensure_wasm_engine()) return js_false;
  if (!(store = wasm_store_new(g_wasm_engine))) return js_false;
  
  if (!wasm_buffer_source_to_vec(js, args[0], &binary, error_buf, sizeof(error_buf))) {
    wasm_store_delete(store);
    return js_false;
  }

  ok = wasm_module_validate(store, &binary);
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
}}

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

  js_set_proto_init(g_wasm_module_proto, js->object);
  js_set_proto_init(g_wasm_instance_proto, js->object);
  js_set_proto_init(g_wasm_global_proto, js->object);
  js_set_proto_init(g_wasm_memory_proto, js->object);
  js_set_proto_init(g_wasm_table_proto, js->object);
  js_set_proto_init(g_wasm_tag_proto, js->object);
  js_set_proto_init(g_wasm_exception_proto, js->object);

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
