#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "wire.h"
#include "errors.h"
#include "gc/roots.h"
#include "silver/engine.h"

ant_value_t host_function_call(ant_params_t) {
  ant_wasm_runtime_t *runtime = active_runtime;
  
  if (!runtime || runtime->js != js)
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "Host bridge is unavailable");

  ant_value_t current = call_func;
  ant_value_t data = js_get_slot(js_as_obj(current), SLOT_DATA);
  
  if (vtype(data) != kTypeNumber)
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "Invalid host function");

  GC_ROOT_SAVE(root_mark, js);
  ant_wire_writer_t writer = {0};
  ant_wire_active_t active = {0};
  
  bool arguments_ok = nargs >= 0 &&
    (uint32_t)nargs <= ANT_WASM_WIRE_MAX_CONTAINER_ENTRIES &&
    wire_writer_u8(&writer, ANT_WASM_WIRE_VERSION) &&
    wire_writer_u8(&writer, ANT_WASM_WIRE_ARRAY) &&
    wire_writer_u32(&writer, (uint32_t)nargs);
  
  for (int index = 0; arguments_ok && index < nargs; index++)
    arguments_ok = wire_encode_value(runtime, args[index], &writer, &active, 1);
    
  free(active.items);
  if (!arguments_ok || writer.length > UINT32_MAX) {
    free(writer.data);
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Host function arguments cannot be transferred");
  }

  uint32_t response_length = 0;
  uint32_t response_pointer = ant_wasm_host_call(
    (int32_t)Ant_Value_AsNumberUnchecked(data), (const char *)writer.data,
    (uint32_t)writer.length, &response_length
  );
  
  free(writer.data);
  if (!response_pointer || response_length < 2) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "Host function returned an invalid response");
  }

  uint8_t *response = (uint8_t *)(uintptr_t)response_pointer;
  uint8_t status = response[0];
  
  ant_value_t decoded = Ant_Value_Undefined;
  GC_ROOT_PIN(js, decoded);
  
  bool decoded_ok = (status == ANT_WASM_RESPONSE_OK ||
    status == ANT_WASM_RESPONSE_ERROR) &&
    wire_decode(js, response + 1, response_length - 1u, &decoded);
  
  free(response);
  if (!decoded_ok) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "Host function returned an invalid response");
  }

  if (status == ANT_WASM_RESPONSE_ERROR) {
    ant_value_t message = Ant_Value_Undefined;
    if (is_object_type(decoded)) js_try_get_own_data_prop(js, decoded, "message", 7, &message);
    
    size_t message_length = 0;
    const char *message_bytes = js_getstr(js, message, &message_length);
    
    char *message_copy = message_bytes ? malloc(message_length + 1) : NULL;
    if (message_copy) {
      memcpy(message_copy, message_bytes, message_length);
      message_copy[message_length] = '\0';
    }
    
    ant_value_t error = message_copy
      ? js_mkerr_typed(js, JS_ERR_TYPE, "%s", message_copy)
      : js_mkerr_typed(js, JS_ERR_TYPE, "Host function failed");
    
    free(message_copy);
    GC_ROOT_RESTORE(js, root_mark);
    
    return error;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return decoded;
}
