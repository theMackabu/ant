#include <stdlib.h>
#include <string.h>

#include "response.h"
#include "wire.h"
#include "errors.h"
#include "gc/roots.h"

void clear_last_result(ant_wasm_runtime_t *runtime) {
  if (!runtime) return;
  free(runtime->last_result);
  runtime->last_result = NULL;
  runtime->last_result_length = 0;
}

static const uint8_t *take_last_wire(ant_wasm_runtime_t *runtime, ant_wire_writer_t *writer) {
  if (!runtime || writer->length > UINT32_MAX) return NULL;
  clear_last_result(runtime);
  
  runtime->last_result = writer->data;
  runtime->last_result_length = (uint32_t)writer->length;
  writer->data = NULL;
  writer->length = writer->capacity = 0;
  
  return runtime->last_result;
}

const uint8_t *serialize_value(
  ant_wasm_runtime_t *runtime, uint8_t status, ant_value_t value
) {
  ant_wire_writer_t writer = {0};
  
  bool ok = wire_writer_u8(&writer, status) &&
    wire_encode(runtime, value, &writer)    &&
    !ant_wasm_deadline_expired(runtime);
  
  const uint8_t *result = ok ? take_last_wire(runtime, &writer) : NULL;
  free(writer.data);
  
  return result;
}

static bool wire_writer_string_value(ant_wire_writer_t *writer, const char *value, size_t length) {
  return wire_writer_u8(writer, ANT_WASM_WIRE_STRING) && wire_writer_string(writer, value, length);
}

static const uint8_t *serialize_error_literal(
  ant_wasm_runtime_t *runtime, const char *name, const char *message
) {
  ant_wire_writer_t writer = {0};
  bool ok = wire_writer_u8(&writer, ANT_WASM_RESPONSE_ERROR) &&
    wire_writer_u8(&writer, ANT_WASM_WIRE_VERSION) &&
    wire_writer_u8(&writer, ANT_WASM_WIRE_OBJECT) &&
    wire_writer_u32(&writer, 2) &&
    wire_writer_string(&writer, "name", 4) &&
    wire_writer_string_value(&writer, name, strlen(name)) &&
    wire_writer_string(&writer, "message", 7) &&
    wire_writer_string_value(&writer, message, strlen(message));
  const uint8_t *result = ok ? take_last_wire(runtime, &writer) : NULL;
  free(writer.data);
  return result;
}

const uint8_t *serialize_timeout(ant_wasm_runtime_t *runtime) {
  ant_wire_writer_t writer = {0};
  if (!wire_writer_u8(&writer, ANT_WASM_RESPONSE_TIMEOUT)) return NULL;
  const uint8_t *result = take_last_wire(runtime, &writer);
  free(writer.data);
  return result;
}

const uint8_t *serialize_transfer_error(ant_wasm_runtime_t *runtime) {
  js_take_thrown(runtime->js, Ant_Value_Undefined);
  return serialize_error_literal(runtime, "TypeError", "The value cannot be transferred from Ant");
}

static bool define_payload_property(
  ant_t *js, ant_value_t payload, const char *name, size_t name_length,
  ant_value_t value
) {
  const char *interned_name = intern_string(js, name, name_length);
  return interned_name && !is_err(
    mkprop_interned_exact(js, payload, interned_name, value, 0)
  );
}

static bool error_data_prop(
  ant_t *js, ant_value_t obj, const char *key, size_t key_length, ant_value_t *out
) {
  for (int depth = 0; is_object_type(obj) && depth < 64; depth++) {
    if (js_try_get_own_data_prop(js, obj, key, key_length, out)) return true;
    prop_meta_t meta;
    if (is_proxy(obj) || lookup_string_prop_meta(js, obj, key, key_length, &meta)) return false;
    obj = js_get_proto(js, obj);
  }
  return false;
}

ant_value_t error_payload(
  ant_wasm_runtime_t *runtime, ant_value_t fallback
) {
  ant_t *js = runtime->js;
  GC_ROOT_SAVE(root_mark, js);

  ant_value_t thrown = js_take_thrown(js, fallback);
  ant_value_t payload = Ant_Object_CreateBare(js);
  ant_value_t name = Ant_Value_Undefined;
  ant_value_t message = Ant_Value_Undefined;
  ant_value_t stack = Ant_Value_Undefined;
  
  GC_ROOT_PIN(js, thrown);
  GC_ROOT_PIN(js, payload);
  GC_ROOT_PIN(js, name);
  GC_ROOT_PIN(js, message);
  GC_ROOT_PIN(js, stack);

  if (is_object_type(thrown)) {
    bool plain_header =
      error_data_prop(js, thrown, "name", 4, &name) &&
      error_data_prop(js, thrown, "message", 7, &message);
    js_try_get_own_data_prop(js, thrown, "stack", 5, &stack);
    if (plain_header && vtype(stack) != kTypeString)
      stack = Ant_Error_ConsumeMarker(js, js_error_stack_value(js, thrown));
  }
  
  if (vtype(name) != kTypeString) name = js_mkstr(js, "Error", 5);
  if (vtype(message) != kTypeString) {
    message = vtype(thrown) == kTypeString
      ? thrown : js_mkstr(js, "Ant evaluation failed", 21);
  }

  if (!define_payload_property(js, payload, "name", 4, name) ||
      !define_payload_property(js, payload, "message", 7, message)) {
    js_take_thrown(js, Ant_Value_Undefined);
    payload = Ant_Object_CreateBare(js);
    if (!is_err(payload)) {
      define_payload_property(js, payload, "name", 4, js_mkstr(js, "Error", 5));
      define_payload_property(
        js, payload, "message", 7,
        js_mkstr(js, "Ant evaluation failed", 21)
      );
    }
  } else if (
    vtype(stack) == kTypeString && 
    !define_payload_property(js, payload, "stack", 5, stack)
  ) js_take_thrown(js, Ant_Value_Undefined);

  GC_ROOT_RESTORE(js, root_mark);
  return payload;
}
