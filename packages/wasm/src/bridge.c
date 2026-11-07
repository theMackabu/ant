#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "wire.h"
#include "bridge.h"
#include "response.h"
#include "runtime.h"
#include "errors.h"

#include "modules/abort.h"
#include "modules/atomics.h"
#include "modules/blob.h"
#include "modules/crypto.h"
#include "modules/domexception.h"
#include "modules/events.h"
#include "modules/formdata.h"
#include "modules/globals.h"
#include "modules/headers.h"
#include "modules/navigator.h"
#include "modules/observable.h"
#include "modules/performance.h"
#include "modules/request.h"
#include "modules/response.h"
#include "modules/structured-clone.h"
#include "modules/url.h"
#include "streams/codec.h"
#include "streams/compression.h"
#include "streams/queuing.h"
#include "streams/readable.h"
#include "streams/transform.h"
#include "streams/writable.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/date.h"
#include "modules/intl.h"
#include "modules/io.h"
#include "modules/reflect.h"
#include "modules/temporal.h"
#include "modules/textcodec.h"
#include "modules/uri.h"
#include "modules/collections.h"
#include "modules/generator.h"
#include "modules/json.h"
#include "modules/math.h"
#include "modules/regex.h"
#include "modules/symbol.h"
#include "modules/iterator.h"
#include "modules/microtasks.h"

ant_wasm_runtime_t *active_runtime;

bool ant_wasm_should_interrupt(ant_t *js) {
  return active_runtime && active_runtime->js == js &&
    ant_wasm_deadline_expired(active_runtime);
}

ANT_WASM_EXPORT("ant_alloc")
void *ant_wasm_alloc(uint32_t size) {
  return size ? malloc(size) : NULL;
}

ANT_WASM_EXPORT("ant_free")
void ant_wasm_free(void *pointer) {
  free(pointer);
}

ANT_WASM_EXPORT("ant_create")
ant_wasm_runtime_t *ant_wasm_create(void) {
  if (active_runtime) return NULL;

  ant_wasm_runtime_t *runtime = calloc(1, sizeof(*runtime));
  if (!runtime) return NULL;

  ant_t *js = Ant_Create(nullptr);
  if (!js) {
    free(runtime);
    return NULL;
  }

  runtime->js = js;
  js->uncaught_nonfatal = true;
  active_runtime = runtime;
  
  ant_microtasks_discard(js);
  ant_runtime_init(js, 0, NULL, NULL);
  init_symbol_module(js);
  js_init_function_has_instance(js);
  init_intrinsic_symbols(js);
  js_set_sym(js, js->Ant, js->sym.toStringTag_sym, Ant_String_FromLiteral(js, "Ant"));
  
  init_iterator_module(js);
  init_generator_module(js);
  init_domexception_module(js);
  init_globals_module(js);
  init_intl_module(js);
  init_buffer_module(js);
  init_structured_clone_module(js);
  init_headers_module(js);
  init_blob_module(js);
  init_formdata_module(js);
  init_math_module(js);
  init_bigint_module(js);
  init_date_module(js);
  init_temporal_module(js);
  init_regex_module(js);
  init_collections_module(js);
  init_queuing_strategies_module(js);
  init_readable_stream_module(js);
  init_writable_stream_module(js);
  init_transform_stream_module(js);
  init_codec_stream_module(js);
  init_compression_stream_module(js);
  init_atomics_module(js);
  init_crypto_module(js);
  init_request_module(js);
  init_response_module(js);
  init_console_module(js);
  init_json_module(js);
  init_events_module(js);
  init_abort_module(js);
  init_performance_module(js);
  init_uri_module(js);
  init_url_module(js);
  init_reflect_module(js);
  init_textcodec_module(js);
  init_navigator_module(js);
  init_observable_module(js);
  js_set(js, js_glob(js), "queueMicrotask", Ant_Function_CreateWithArity(js_queue_microtask, 1));
  
  return runtime;
}

ANT_WASM_EXPORT("ant_destroy")
void ant_wasm_destroy(ant_wasm_runtime_t *runtime) {
  if (!runtime) return;
  
  ant_microtasks_discard(runtime->js);
  if (active_runtime == runtime) active_runtime = NULL;
  
  Ant_Destroy(runtime->js);
  clear_last_result(runtime);
  free(runtime);
}

ANT_WASM_EXPORT("ant_result_length")
uint32_t ant_wasm_result_length(const ant_wasm_runtime_t *runtime) {
  return runtime ? runtime->last_result_length : 0;
}

ANT_WASM_EXPORT("ant_release_result")
void ant_wasm_release_result(ant_wasm_runtime_t *runtime) {
  clear_last_result(runtime);
}

ANT_WASM_EXPORT("ant_set_global")
const uint8_t *ant_wasm_set_global(
  ant_wasm_runtime_t *runtime,
  const uint8_t *name, uint32_t name_units,
  const uint8_t *wire_value, uint32_t wire_length,
  int32_t function_id
) {
  if (!runtime || !name) return NULL;

  ant_t *js = runtime->js;
  volatile char stack_base;
  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, 768 * 1024);

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t value = Ant_Value_Undefined;
  GC_ROOT_PIN(js, value);
  
  if (function_id >= 0)
    value = js_heavy_mkfun(js, host_function_call, Ant_Value_FromNumber((double)function_id));
  else if (!wire_value || !wire_decode(js, wire_value, wire_length, &value)) {
    js_take_thrown(js, Ant_Value_Undefined);
    const uint8_t *response = serialize_transfer_error(runtime);
    GC_ROOT_RESTORE(js, root_mark);
    return response;
  }

  if (is_err(value)) {
    ant_value_t payload = error_payload(runtime, value);
    const uint8_t *response = serialize_value(runtime, ANT_WASM_RESPONSE_ERROR, payload);
    GC_ROOT_RESTORE(js, root_mark);
    return response;
  }

  size_t name_length = 0;
  char *name_bytes = wire_utf16_to_wtf8(name, name_units, &name_length);
  
  const char *interned_name = name_bytes ? intern_string(js, name_bytes, name_length) : NULL;
  free(name_bytes);
  
  ant_value_t set_result = interned_name
    ? mkprop_interned_exact(js, js->global, interned_name, value, 0)
    : js_mkerr_typed(js, JS_ERR_INTERNAL, "Unable to intern global name");
  
  if (is_err(set_result)) {
    ant_value_t payload = error_payload(runtime, set_result);
    const uint8_t *response = serialize_value(runtime, ANT_WASM_RESPONSE_ERROR, payload);
    GC_ROOT_RESTORE(js, root_mark);
    return response;
  }

  const uint8_t *response = serialize_value(runtime, ANT_WASM_RESPONSE_OK, Ant_Value_Undefined);
  GC_ROOT_RESTORE(js, root_mark);
  
  return response;
}

ANT_WASM_EXPORT("ant_eval")
const uint8_t *ant_wasm_eval(
  ant_wasm_runtime_t *runtime, const uint8_t *source_units, 
  uint32_t source_unit_count, uint32_t timeout_ms
) {
  if (!runtime || !source_units) return NULL;

  size_t source_length = 0;
  char *source = wire_utf16_to_wtf8(source_units, source_unit_count, &source_length);
  if (!source) return NULL;

  ant_t *js = runtime->js;
  volatile char stack_base;
  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, 768 * 1024);
  
  js->wasm_interrupt_ticks = 0;
  js->wasm_interrupt_enabled = timeout_ms > 0;
  
  runtime->timed_out = false;
  runtime->deadline_ms = js->wasm_interrupt_enabled
    ? ant_wasm_now_ms() + (double)timeout_ms : 0.0;

  ant_value_t result = js_eval_bytecode_eval(js, source, source_length, false);
  if (!runtime->timed_out) process_microtasks(js);

  const uint8_t *response = NULL;
  if (runtime->timed_out) {
    js_take_thrown(js, Ant_Value_Undefined);
    ant_microtasks_discard(js);
    response = serialize_timeout(runtime);
  } else if (is_err(result)) {
    ant_value_t payload = error_payload(runtime, result);
    response = serialize_value(runtime, ANT_WASM_RESPONSE_ERROR, payload);
    
    if (!response && runtime->timed_out) {
      js_take_thrown(js, Ant_Value_Undefined);
      ant_microtasks_discard(js);
      response = serialize_timeout(runtime);
    } else if (!response) response = serialize_transfer_error(runtime);
  } else {
    response = serialize_value(runtime, ANT_WASM_RESPONSE_OK, result);
    if (!response && runtime->timed_out) {
      js_take_thrown(js, Ant_Value_Undefined);
      ant_microtasks_discard(js);
      response = serialize_timeout(runtime);
    } else if (!response) response = serialize_transfer_error(runtime);
  }

  js->wasm_interrupt_enabled = false;
  runtime->deadline_ms = 0.0;
  free(source);
  
  return response;
}
