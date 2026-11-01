#include "wasm_internal.h"

wasm_engine_t *g_wasm_engine = NULL;
static uv_once_t g_wasm_engine_once = UV_ONCE_INIT;

void wasm_clear_pending_import_throw(ant_t *js) {
  js->wasm_pending_import_throw = false;
  js->mutable_roots.wasm_pending_import_throw = Ant_Value_Undefined;
}

void wasm_set_pending_import_throw(ant_t *js, ant_value_t value) {
  js->wasm_pending_import_throw = true;
  js->mutable_roots.wasm_pending_import_throw = value;
}

ant_value_t wasm_consume_pending_import_throw(ant_t *js) {
  ant_value_t value = js->wasm_pending_import_throw
    ? js->mutable_roots.wasm_pending_import_throw
    : Ant_Value_Undefined;
  wasm_clear_pending_import_throw(js);
  return value;
}

bool wasm_register_instance_owner(ant_t *js, wasm_instance_t *instance, ant_value_t owner) {
  if (!instance || !instance->inst_comm_rt || !is_object_type(owner)) return true;
  return ant_ptr_pair_put(&js->wasm_instance_owners, (uintptr_t)instance->inst_comm_rt, 0, owner);
}

void wasm_unregister_instance_owner(ant_t *js, wasm_instance_t *instance) {
  if (instance && instance->inst_comm_rt)
    ant_ptr_pair_remove(&js->wasm_instance_owners, (uintptr_t)instance->inst_comm_rt, 0);
}

ant_value_t wasm_find_instance_owner(ant_t *js, WASMModuleInstanceCommon *runtime_instance) {
  ant_value_t owner;
  if (runtime_instance && ant_ptr_pair_get(&js->wasm_instance_owners, (uintptr_t)runtime_instance, 0, &owner)) return owner;
  return Ant_Value_Undefined;
}

static void create_wasm_engine(void) {
  g_wasm_engine = wasm_engine_new();
  if (g_wasm_engine) wasm_runtime_set_log_level(WASM_LOG_LEVEL_FATAL);
}

bool ensure_wasm_engine(void) {
  uv_once(&g_wasm_engine_once, create_wasm_engine);
  return g_wasm_engine != NULL;
}

size_t wasm_name_len(const wasm_name_t *name) {
  if (!name || !name->data) return 0;
  if (name->size > 0 && name->data[name->size - 1] == '\0') return name->size - 1;
  return name->size;
}

const char *wasm_extern_kind_name(wasm_externkind_t kind) {
switch (kind) {
  case WASM_EXTERN_FUNC: return "function";
  case WASM_EXTERN_GLOBAL: return "global";
  case WASM_EXTERN_TABLE: return "table";
  case WASM_EXTERN_MEMORY: return "memory";
  default: return "unknown";
}}

wasm_valkind_t wasm_valkind_from_string(const char *name, size_t len, bool *ok) {
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

wasm_module_handle_t *wasm_module_handle(ant_value_t value) {
  if (!js_check_brand(value, BRAND_WASM_MODULE)) return NULL;
  return (wasm_module_handle_t *)js_get_native(value, WASM_MODULE_NATIVE_TAG);
}

wasm_extern_handle_t *wasm_extern_handle(ant_value_t value, wasm_extern_wrap_kind_t kind) {
  if ((kind == WASM_EXTERN_WRAP_GLOBAL && !js_check_brand(value, BRAND_WASM_GLOBAL))
      || (kind == WASM_EXTERN_WRAP_MEMORY && !js_check_brand(value, BRAND_WASM_MEMORY))
      || (kind == WASM_EXTERN_WRAP_TABLE && !js_check_brand(value, BRAND_WASM_TABLE))) {
    return NULL;
  }

  wasm_extern_handle_t *handle = (wasm_extern_handle_t *)js_get_native(value, WASM_EXTERN_NATIVE_TAG);
  return handle && handle->kind == kind ? handle : NULL;
}

static ant_value_t wasm_make_error(ant_t *js, ant_value_t proto, const char *message) {
  ant_value_t err = Ant_Object_CreateBare(js);
  if (is_err(err)) return err;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, err);
  js_set_proto_init(err, proto);
  defhidden(js, err, "message", 7, js_mkstr(js, message ? message : "", message ? strlen(message) : 0));
  js_set_slot(err, SLOT_ERROR_BRAND, Ant_Value_True);
  js_capture_stack(js, err);
  GC_ROOT_RESTORE(js, root_mark);

  return err;
}

ant_value_t wasm_make_compile_error(ant_t *js, const char *message) {
  return wasm_make_error(js, js->builtins.wasm_compileerror_proto, message);
}

ant_value_t wasm_make_link_error(ant_t *js, const char *message) {
  return wasm_make_error(js, js->builtins.wasm_linkerror_proto, message);
}

ant_value_t wasm_make_runtime_error(ant_t *js, const char *message) {
  return wasm_make_error(js, js->builtins.wasm_runtimeerror_proto, message);
}

ant_value_t wasm_error_value(ant_t *js, ant_value_t value) {
  if (is_err(value)) return Ant_Exception_Value(js, value);
  return value;
}

void wasm_reject_with_error(ant_t *js, ant_value_t promise, ant_value_t error) {
  js_reject_promise(js, promise, error);
}

bool wasm_buffer_source_to_vec(ant_t *js, ant_value_t value, wasm_byte_vec_t *out, char *error_buf, size_t error_buf_len) {
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

ant_value_t wasm_value_from_i64(ant_t *js, int64_t value) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%" PRId64, value);
  if (n < 0) return js_mkerr(js, "Failed to convert i64");
  return js_mkbigint(js, buf, (size_t)n, value < 0);
}

ant_value_t wasm_value_to_js(ant_t *js, const wasm_val_t *value) {
  switch (value->kind) {
    case WASM_I32: return Ant_Value_FromNumber((double)value->of.i32);
    case WASM_I64: return wasm_value_from_i64(js, value->of.i64);
    case WASM_F32: return Ant_Value_FromNumber((double)value->of.f32);
    case WASM_F64: return Ant_Value_FromNumber(value->of.f64);
    case WASM_EXTERNREF:
      return value->of.ref ? Ant_Value_Undefined : Ant_Value_Null;
    case WASM_FUNCREF: {
      wasm_func_t *func;
      if (!value->of.ref) return Ant_Value_Null;
      func = wasm_ref_as_func(value->of.ref);
      if (!func) return Ant_Value_Undefined;
      return wasm_wrap_func(js, func, Ant_Value_Undefined, true);
    }
    default: return Ant_Value_Undefined;
  }
}

static bool js_value_to_i64(ant_t *js, ant_value_t value, int64_t *out) {
  if (vtype(value) == kTypeBigInt) {
    ant_value_t str_val = js_tostring_val(js, value);
    if (is_err(str_val) || vtype(str_val) != kTypeString) return false;
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

bool js_value_to_wasm(ant_t *js, ant_value_t value, wasm_valkind_t kind, wasm_val_t *out) {
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
      return vtype(value) == kTypeNull || vtype(value) == kTypeUndefined;
    case WASM_FUNCREF: {
      ant_value_t state;
      wasm_func_handle_t *handle;
      if (vtype(value) == kTypeNull || vtype(value) == kTypeUndefined) {
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

ant_value_t wasm_js_from_result_vec(ant_t *js, wasm_val_vec_t *results) {
  if (!results || results->size == 0) return Ant_Value_Undefined;
  if (results->size == 1) return wasm_value_to_js(js, &results->data[0]);

  ant_value_t arr = Ant_Array_Create(js);
  for (size_t i = 0; i < results->size; i++) {
    js_arr_push(js, arr, wasm_value_to_js(js, &results->data[i]));
  }
  return arr;
}

ant_value_t wasm_trap_to_error(ant_t *js, wasm_trap_t *trap) {
  wasm_message_t message = WASM_EMPTY_VEC;
  wasm_trap_message(trap, &message);

  ant_value_t err = wasm_make_runtime_error(js, message.data ? message.data : "WebAssembly trap");

  wasm_byte_vec_delete(&message);
  wasm_trap_delete(trap);
  return err;
}

ant_value_t wasm_wrap_extern_object(ant_t *js, wasm_extern_wrap_kind_t kind, ant_value_t proto, int brand, wasm_store_t *store, bool own_handle, void *ptr, ant_value_t owner) {
  wasm_extern_handle_t *handle = calloc(1, sizeof(*handle));
  if (!handle) return Ant_Error_CreateOutOfMemory(js);

  handle->kind = kind;
  handle->store = store;
  handle->own_handle = own_handle;
  handle->memory_buffer = Ant_Value_Undefined;
  handle->cached_value = (wasm_val_t)WASM_INIT_VAL;

  switch (kind) {
    case WASM_EXTERN_WRAP_GLOBAL: handle->as.global = (wasm_global_t *)ptr; break;
    case WASM_EXTERN_WRAP_MEMORY: handle->as.memory = (wasm_memory_t *)ptr; break;
    case WASM_EXTERN_WRAP_TABLE: handle->as.table = (wasm_table_t *)ptr; break;
  }

  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_BRAND, Ant_Value_FromNumber(brand));
  js_set_native(obj, handle, WASM_EXTERN_NATIVE_TAG);
  if (is_object_type(owner)) js_set_slot_wb(js, obj, SLOT_ENTRIES, owner);
  return obj;
}

void wasm_extern_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  wasm_extern_handle_t *handle = (wasm_extern_handle_t *)js_get_native(value, WASM_EXTERN_NATIVE_TAG);
  if (!handle) return;
  Ant_List_Remove(handle);

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

bool wasm_to_page_count(ant_t *js, ant_value_t value, uint32_t *out) {
  double number = js_to_number(js, value);
  if (!isfinite(number) || number < 0 || floor(number) != number || number > UINT32_MAX)
    return false;
  *out = (uint32_t)number;
  return true;
}

ant_value_t wasm_sequence_to_list(ant_t *js, ant_value_t value, const char *not_iterable) {
  if (!is_object_type(value)) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", not_iterable);
  return Ant_Iterator_ToList(js, value, not_iterable);
}
