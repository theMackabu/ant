#include "wasm_internal.h"

static ant_value_t wasm_wrap_module(ant_t *js, wasm_store_t *store, wasm_module_t *module) {
  wasm_module_handle_t *handle = calloc(1, sizeof(*handle));
  if (!handle) {
    if (module) wasm_module_delete(module);
    if (store) wasm_store_delete(store);
    return Ant_Error_CreateOutOfMemory(js);
  }

  handle->store = store;
  handle->module = module;

  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, js->builtins.wasm_module_proto);
  js_set_slot(obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_WASM_MODULE));
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

static void wasm_load_failure_reason(const wasm_byte_vec_t *binary, char *reason, size_t reason_len) {
  static const char prefix[] = "WASM module load failed: ";
  snprintf(reason, reason_len, "invalid module");

  uint8_t *copy = binary->size ? malloc(binary->size) : NULL;
  if (!copy) return;
  memcpy(copy, binary->data, binary->size);

  char error_buf[128] = {0};
  wasm_module_t loaded = wasm_runtime_load(copy, (uint32_t)binary->size, error_buf, sizeof(error_buf));
  if (loaded) wasm_runtime_unload(loaded);
  else if (error_buf[0]) {
    const char *text = strncmp(error_buf, prefix, sizeof(prefix) - 1) == 0 ? error_buf + sizeof(prefix) - 1 : error_buf;
    snprintf(reason, reason_len, "%s", text);
  }
  free(copy);
}

ant_value_t wasm_module_from_bytes(ant_t *js, ant_value_t value, ant_value_t *out_module, const char *api) {
  wasm_byte_vec_t binary = WASM_EMPTY_VEC;
  wasm_store_t *store = NULL;
  wasm_module_t *module = NULL;

  char error_buf[128] = {0};

  *out_module = Ant_Value_Undefined;
  if (js->codegen_blocked & ANT_CODEGEN_NO_WASM)
    return wasm_make_compile_error(js, "Wasm code generation disallowed by embedder");

  if (!ensure_wasm_engine()) return js_mkerr(js, "Failed to initialize WebAssembly engine");
  if (!(store = wasm_store_new(g_wasm_engine))) return js_mkerr(js, "Failed to create WebAssembly store");

  if (!wasm_buffer_source_to_vec(js, value, &binary, error_buf, sizeof(error_buf))) {
    wasm_store_delete(store);
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s", error_buf);
  }

  module = wasm_module_new(store, &binary);

  if (!module) {
    char reason[160], message[200];
    wasm_load_failure_reason(&binary, reason, sizeof(reason));
    snprintf(message, sizeof(message), "%s: %s", api, reason);
    wasm_byte_vec_delete(&binary);
    wasm_store_delete(store);
    return wasm_make_compile_error(js, message);
  }

  *out_module = wasm_wrap_module(js, store, module);
  if (vtype(*out_module) == kTypeObject) {
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
  return Ant_Value_Undefined;
}

ant_value_t js_wasm_module_ctor(ant_params_t) {
  ant_value_t module = Ant_Value_Undefined;
  ant_value_t err;

  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Module constructor requires 'new'");
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Module requires a BufferSource");

  err = wasm_module_from_bytes(js, args[0], &module, "WebAssembly.Module()");
  if (is_err(err)) return err;
  if (vtype(module) != kTypeObject) return js_throw(js, wasm_error_value(js, err));
  return module;
}

static ant_value_t wasm_module_type_descriptors(ant_t *js, ant_value_t module_obj, bool imports) {
  wasm_module_handle_t *handle = wasm_module_handle(module_obj);
  if (!handle) return js_mkerr_typed(js, JS_ERR_TYPE, "Expected a WebAssembly.Module");

  ant_value_t arr = Ant_Array_Create(js);
  if (imports) {
    wasm_importtype_vec_t vec = WASM_EMPTY_VEC;
    wasm_module_imports(handle->module, &vec);
    for (size_t i = 0; i < vec.size; i++) {
      const wasm_importtype_t *entry = vec.data[i];
      const wasm_name_t *module_name = wasm_importtype_module(entry);
      const wasm_name_t *name = wasm_importtype_name(entry);
      const wasm_externtype_t *type = wasm_importtype_type(entry);

      ant_value_t item = Ant_Object_Create(js);
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
      
      ant_value_t item = Ant_Object_Create(js);
      js_set(js, item, "name", js_mkstr(js, name->data, wasm_name_len(name)));
      js_set(js, item, "kind", js_mkstr(js, wasm_extern_kind_name(wasm_externtype_kind(type)), strlen(wasm_extern_kind_name(wasm_externtype_kind(type)))));
      js_arr_push(js, arr, item);
    }
    wasm_exporttype_vec_delete(&vec);
  }

  return arr;
}

ant_value_t js_wasm_module_imports(ant_params_t) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.Module.imports requires 1 argument");
  return wasm_module_type_descriptors(js, args[0], true);
}

ant_value_t js_wasm_module_exports(ant_params_t) {
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

ant_value_t js_wasm_module_custom_sections(ant_params_t) {
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
  result = Ant_Array_Create(js);

  if (!handle->bytes || handle->bytes_len < 8) return result;
  while (offset < handle->bytes_len) {
    uint8_t section_id = handle->bytes[offset++];
    uint32_t section_size, name_len;
    size_t section_start, section_end, name_start, data_start;

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
        ant_value_t section = create_arraybuffer_copy(js, handle->bytes + data_start, section_end - data_start);
        if (is_err(section)) return section;
        js_arr_push(js, result, section);
      }
    }

    offset = section_end;
  }

  return result;
}

ant_value_t js_wasm_validate(ant_params_t) {
  wasm_byte_vec_t binary = WASM_EMPTY_VEC;
  wasm_store_t *store;
  
  bool ok;
  char error_buf[128] = {0};

  if (nargs < 1) return Ant_Value_False;
  if (!ensure_wasm_engine()) return Ant_Value_False;
  if (!(store = wasm_store_new(g_wasm_engine))) return Ant_Value_False;
  
  if (!wasm_buffer_source_to_vec(js, args[0], &binary, error_buf, sizeof(error_buf))) {
    wasm_store_delete(store);
    return Ant_Value_False;
  }

  ok = wasm_module_validate(store, &binary);
  wasm_byte_vec_delete(&binary);
  wasm_store_delete(store);
  
  return Ant_Value_FromBool(ok);
}

ant_value_t js_wasm_compile(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t module = Ant_Value_Undefined;
  ant_value_t err;

  if (nargs < 1) {
    wasm_reject_with_error(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "WebAssembly.compile requires 1 argument"));
    return promise;
  }

  err = wasm_module_from_bytes(js, args[0], &module, "WebAssembly.compile()");
  if (is_err(err) || vtype(module) != kTypeObject) {
    wasm_reject_with_error(js, promise, err);
    return promise;
  }

  js_resolve_promise(js, promise, module);
  return promise;
}
