#include "wasm_internal.h"

static void wasm_streaming_fail(ant_t *js, ant_value_t state, ant_value_t error) {
  ant_value_t value = is_err(error) ? Ant_Exception_Value(js, error) : error;
  js_reject_promise(js, js_get_slot(state, SLOT_DATA), value);
}

static ant_value_t wasm_streaming_type_error(ant_t *js, ant_value_t state, const char *detail) {
  ant_value_t api = js_get_slot(state, SLOT_ENTRIES);
  size_t api_len = 0;
  const char *api_str = js_getstr(js, api, &api_len);
  return js_mkerr_typed(js, JS_ERR_TYPE, "%.*s: %s", (int)api_len, api_str ? api_str : "", detail);
}

static ant_value_t wasm_streaming_on_reject(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  js_reject_promise(js, js_get_slot(state, SLOT_DATA), nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t wasm_streaming_on_bytes(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t promise = js_get_slot(state, SLOT_DATA);
  ant_value_t imports = js_get_slot(state, SLOT_AUX);
  ant_value_t api = js_get_slot(state, SLOT_ENTRIES);
  ant_value_t module = js_mkundef();

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, state);
  size_t api_len = 0;
  ant_value_t err = wasm_module_from_bytes(js, nargs > 0 ? args[0] : js_mkundef(), &module, js_getstr(js, api, &api_len));
  GC_ROOT_PIN(js, module);

  if (is_err(err) || vtype(module) != kTypeObject) wasm_streaming_fail(js, state, err);
  else if (js_get_slot(state, SLOT_MAP) != js_true) js_resolve_promise(js, promise, module);
  else {
    ant_value_t instance = js_mkundef();
    err = wasm_instantiate_module(js, module, is_object_type(imports) ? imports : js_mkundef(), &instance);
    if (is_err(err) || vtype(instance) != kTypeObject) wasm_streaming_fail(js, state, err);
    else js_resolve_promise(js, promise, wasm_instantiate_result(js, module, instance));
  }

  GC_ROOT_RESTORE(js, mark);
  return js_mkundef();
}

static bool wasm_mime_is_wasm(ant_t *js, ant_value_t content_type) {
  size_t len = 0;
  const char *str = vtype(content_type) == kTypeString ? js_getstr(js, content_type, &len) : NULL;
  if (!str) return false;

  size_t start = 0, end = len;
  for (size_t i = 0; i < len; i++) if (str[i] == ';') { end = i; break; }
  while (start < end && (str[start] == ' ' || str[start] == '\t')) start++;
  while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t')) end--;
  return end - start == 16 && strncasecmp(str + start, "application/wasm", 16) == 0;
}

static ant_value_t wasm_streaming_on_response(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t response = nargs > 0 ? args[0] : js_mkundef();

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, state);
  ant_value_t error = js_mkundef();

  if (!is_object_type(response) || !js_check_brand(response, BRAND_RESPONSE)) {
    error = wasm_streaming_type_error(js, state, "Argument 0 must be provided and must be a Response");
    goto fail;
  }

  ant_value_t headers = js_get(js, response, "headers");
  GC_ROOT_PIN(js, headers);
  ant_value_t get_fn = is_err(headers) ? headers : js_get(js, headers, "get");
  if (is_err(get_fn)) { error = get_fn; goto fail; }

  ant_value_t header_name = js_mkstr(js, "content-type", 12);
  ant_value_t content_type = sv_vm_call(js->vm, js, get_fn, headers, &header_name, 1, NULL, js_mkundef());
  if (is_err(content_type)) { error = content_type; goto fail; }
  if (!wasm_mime_is_wasm(js, content_type)) {
    error = wasm_streaming_type_error(js, state, "Incorrect response MIME type. Expected 'application/wasm'.");
    goto fail;
  }

  ant_value_t ok = js_get(js, response, "ok");
  if (is_err(ok)) { error = ok; goto fail; }
  if (!js_truthy(js, ok)) {
    error = wasm_streaming_type_error(js, state, "HTTP status code is not ok");
    goto fail;
  }

  ant_value_t array_buffer = js_get(js, response, "arrayBuffer");
  ant_value_t bytes = is_err(array_buffer) ? array_buffer : sv_vm_call(js->vm, js, array_buffer, response, NULL, 0, NULL, js_mkundef());
  if (is_err(bytes)) { error = bytes; goto fail; }
  GC_ROOT_PIN(js, bytes);

  ant_value_t on_bytes = js_heavy_mkfun(js, wasm_streaming_on_bytes, state);
  GC_ROOT_PIN(js, on_bytes);
  ant_value_t on_reject = js_heavy_mkfun(js, wasm_streaming_on_reject, state);
  GC_ROOT_PIN(js, on_reject);
  ant_value_t next = js_promise_then(js, bytes, on_bytes, on_reject);
  if (is_err(next)) { error = next; goto fail; }
  promise_mark_handled(next);

  GC_ROOT_RESTORE(js, mark);
  return js_mkundef();

fail:
  wasm_streaming_fail(js, state, error);
  GC_ROOT_RESTORE(js, mark);
  return js_mkundef();
}

static ant_value_t wasm_streaming_start(ant_native_params_t, bool instantiate) {
  ant_value_t promise = js_mkpromise(js);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, promise);

  ant_value_t state = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, state);
  js_set_slot_wb(js, state, SLOT_DATA, promise);
  js_set_slot_wb(js, state, SLOT_AUX, instantiate && nargs > 1 ? args[1] : js_mkundef());
  js_set_slot_wb(js, state, SLOT_ENTRIES, instantiate
    ? ANT_STRING("WebAssembly.instantiateStreaming()")
    : ANT_STRING("WebAssembly.compileStreaming()"));
  js_set_slot(state, SLOT_MAP, js_bool(instantiate));

  ant_value_t source = js_mkpromise(js);
  GC_ROOT_PIN(js, source);
  js_resolve_promise(js, source, nargs > 0 ? args[0] : js_mkundef());

  ant_value_t on_response = js_heavy_mkfun(js, wasm_streaming_on_response, state);
  GC_ROOT_PIN(js, on_response);
  ant_value_t on_reject = js_heavy_mkfun(js, wasm_streaming_on_reject, state);
  GC_ROOT_PIN(js, on_reject);

  ant_value_t next = js_promise_then(js, source, on_response, on_reject);
  if (is_err(next)) js_reject_promise(js, promise, Ant_Exception_Value(js, next));
  else promise_mark_handled(next);

  GC_ROOT_RESTORE(js, mark);
  return promise;
}

ant_value_t js_wasm_compile_streaming(ant_params_t) {
  return wasm_streaming_start(js, args, nargs, false);
}

ant_value_t js_wasm_instantiate_streaming(ant_params_t) {
  return wasm_streaming_start(js, args, nargs, true);
}
