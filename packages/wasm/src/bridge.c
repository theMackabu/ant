#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "modules/bigint.h"
#include "modules/collections.h"
#include "modules/generator.h"
#include "modules/json.h"
#include "modules/math.h"
#include "modules/regex.h"
#include "modules/symbol.h"
#include "runtime.h"
#include "silver/engine.h"
#include "wasm_embed.h"
#include "wasm_abi.h"

typedef struct {
  uint8_t *data;
  size_t length;
  size_t capacity;
} ant_wire_writer_t;

typedef struct {
  const uint8_t *data;
  size_t length;
  size_t offset;
} ant_wire_reader_t;

typedef struct {
  ant_object_t **items;
  size_t length;
  size_t capacity;
} ant_wire_active_t;

typedef struct {
  ant_t *js;
  uint8_t *last_result;
  uint32_t last_result_length;
  double deadline_ms;
  bool timed_out;
} ant_wasm_runtime_t;

static ant_wasm_runtime_t *active_runtime;

static bool ant_wasm_deadline_expired(ant_wasm_runtime_t *runtime) {
  if (!runtime || !runtime->js->wasm_interrupt_enabled) return false;
  if (runtime->timed_out) return true;
  if (ant_wasm_now_ms() < runtime->deadline_ms) return false;
  runtime->timed_out = true;
  return true;
}

static bool wire_writer_reserve(ant_wire_writer_t *writer, size_t extra) {
  if (extra > SIZE_MAX - writer->length) return false;
  size_t needed = writer->length + extra;
  if (needed > ANT_WASM_WIRE_MAX_BYTES) return false;
  if (needed <= writer->capacity) return true;

  size_t capacity = writer->capacity ? writer->capacity : 256;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }

  uint8_t *data = realloc(writer->data, capacity);
  if (!data) return false;
  writer->data = data;
  writer->capacity = capacity;
  return true;
}

static bool wire_writer_bytes(
  ant_wire_writer_t *writer, const void *bytes, size_t length
) {
  if (!wire_writer_reserve(writer, length)) return false;
  if (length) memcpy(writer->data + writer->length, bytes, length);
  writer->length += length;
  return true;
}

static bool wire_writer_u8(ant_wire_writer_t *writer, uint8_t value) {
  return wire_writer_bytes(writer, &value, sizeof(value));
}

static bool wire_writer_u32(ant_wire_writer_t *writer, uint32_t value) {
  uint8_t bytes[4] = {
    (uint8_t)value,
    (uint8_t)(value >> 8),
    (uint8_t)(value >> 16),
    (uint8_t)(value >> 24),
  };
  return wire_writer_bytes(writer, bytes, sizeof(bytes));
}

static bool wire_writer_sized_bytes(
  ant_wire_writer_t *writer, const void *bytes, size_t length
) {
  return length <= UINT32_MAX &&
    wire_writer_u32(writer, (uint32_t)length) &&
    wire_writer_bytes(writer, bytes, length);
}

static bool wire_reader_bytes(
  ant_wire_reader_t *reader, void *output, size_t length
) {
  if (reader->offset > reader->length ||
      length > reader->length - reader->offset)
    return false;
  if (output && length) memcpy(output, reader->data + reader->offset, length);
  reader->offset += length;
  return true;
}

static bool wire_reader_u8(ant_wire_reader_t *reader, uint8_t *output) {
  return wire_reader_bytes(reader, output, sizeof(*output));
}

static bool wire_reader_u32(ant_wire_reader_t *reader, uint32_t *output) {
  uint8_t bytes[4];
  if (!wire_reader_bytes(reader, bytes, sizeof(bytes))) return false;
  *output = (uint32_t)bytes[0] |
    ((uint32_t)bytes[1] << 8) |
    ((uint32_t)bytes[2] << 16) |
    ((uint32_t)bytes[3] << 24);
  return true;
}

static bool wire_reader_sized_bytes(
  ant_wire_reader_t *reader, const uint8_t **output, uint32_t *length
) {
  if (!wire_reader_u32(reader, length) ||
      reader->offset > reader->length ||
      *length > reader->length - reader->offset)
    return false;
  *output = reader->data + reader->offset;
  reader->offset += *length;
  return true;
}

static bool wire_active_push(ant_wire_active_t *active, ant_object_t *object) {
  for (size_t i = 0; i < active->length; i++)
    if (active->items[i] == object) return false;

  if (active->length == active->capacity) {
    size_t capacity = active->capacity ? active->capacity * 2 : 16;
    ant_object_t **items = realloc(active->items, capacity * sizeof(*items));
    if (!items) return false;
    active->items = items;
    active->capacity = capacity;
  }

  active->items[active->length++] = object;
  return true;
}

static bool wire_encode_value(
  ant_wasm_runtime_t *runtime, ant_value_t value, ant_wire_writer_t *writer,
  ant_wire_active_t *active, unsigned depth
) {
  ant_t *js = runtime->js;
  if (depth > ANT_WASM_WIRE_MAX_DEPTH ||
      ant_wasm_deadline_expired(runtime))
    return false;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);

  bool ok = false;
  switch (vtype(value)) {
    case kTypeUndefined:
      ok = wire_writer_u8(writer, ANT_WASM_WIRE_UNDEFINED);
      break;
    case kTypeNull:
      ok = wire_writer_u8(writer, ANT_WASM_WIRE_NULL);
      break;
    case kTypeBool:
      ok = wire_writer_u8(
        writer, vdata(value) ? ANT_WASM_WIRE_TRUE : ANT_WASM_WIRE_FALSE
      );
      break;
    case kTypeNumber: {
      double number = js_getnum(value);
      ok = wire_writer_u8(writer, ANT_WASM_WIRE_NUMBER) &&
        wire_writer_bytes(writer, &number, sizeof(number));
      break;
    }
    case kTypeString: {
      size_t length = 0;
      const char *bytes = js_getstr(js, value, &length);
      ok = bytes && wire_writer_u8(writer, ANT_WASM_WIRE_STRING) &&
        wire_writer_sized_bytes(writer, bytes, length);
      break;
    }
    case kTypeBigInt: {
      size_t length = strbigint(js, value, NULL, 0);
      char *bytes = malloc(length + 1);
      if (bytes && strbigint(js, value, bytes, length + 1) == length) {
        ok = wire_writer_u8(writer, ANT_WASM_WIRE_BIGINT) &&
          wire_writer_sized_bytes(writer, bytes, length);
      }
      free(bytes);
      break;
    }
    case kTypeArray: {
      ant_object_t *object = js_obj_ptr(js_as_obj(value));
      if (!object || !wire_active_push(active, object)) break;

      ant_offset_t length = js_arr_len(js, value);
      ok = length <= ANT_WASM_WIRE_MAX_CONTAINER_ENTRIES &&
        wire_writer_u8(writer, ANT_WASM_WIRE_ARRAY) &&
        wire_writer_u32(writer, (uint32_t)length);
      ant_value_t item = js_mkundef();
      GC_ROOT_PIN(js, item);
      for (ant_offset_t i = 0; ok && i < length; i++) {
        if (ant_wasm_deadline_expired(runtime)) {
          ok = false;
          break;
        }
        item = js_arr_get(js, value, i);
        ok = !is_err(item) &&
          wire_encode_value(runtime, item, writer, active, depth + 1);
      }
      active->length--;
      break;
    }
    case kTypeObject: {
      ant_object_t *object = js_obj_ptr(js_as_obj(value));
      if (!object || !wire_active_push(active, object)) break;

      ant_value_t keys = js_own_property_keys(js, value, false, true);
      GC_ROOT_PIN(js, keys);
      if (is_err(keys) || vtype(keys) != kTypeArray) {
        active->length--;
        break;
      }

      ant_offset_t length = js_arr_len(js, keys);
      ok = length <= ANT_WASM_WIRE_MAX_CONTAINER_ENTRIES &&
        wire_writer_u8(writer, ANT_WASM_WIRE_OBJECT) &&
        wire_writer_u32(writer, (uint32_t)length);
      ant_value_t key = js_mkundef();
      ant_value_t item = js_mkundef();
      GC_ROOT_PIN(js, key);
      GC_ROOT_PIN(js, item);
      for (ant_offset_t i = 0; ok && i < length; i++) {
        if (ant_wasm_deadline_expired(runtime)) {
          ok = false;
          break;
        }
        key = js_arr_get(js, keys, i);
        size_t key_length = 0;
        const char *key_bytes = js_getstr(js, key, &key_length);
        if (!key_bytes || !wire_writer_sized_bytes(writer, key_bytes, key_length)) {
          ok = false;
          break;
        }
        item = js_getprop_fallback_len(js, value, key_bytes, key_length);
        ok = !is_err(item) &&
          wire_encode_value(runtime, item, writer, active, depth + 1);
      }
      active->length--;
      break;
    }
    default:
      break;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return ok;
}

static bool wire_encode(
  ant_wasm_runtime_t *runtime, ant_value_t value, ant_wire_writer_t *writer
) {
  ant_wire_active_t active = {0};
  bool ok = wire_writer_u8(writer, ANT_WASM_WIRE_VERSION) &&
    wire_encode_value(runtime, value, writer, &active, 0);
  free(active.items);
  return ok;
}

static bool wire_decode_value(
  ant_t *js, ant_wire_reader_t *reader, ant_value_t *output, unsigned depth
) {
  if (depth > ANT_WASM_WIRE_MAX_DEPTH) return false;

  uint8_t tag = 0;
  if (!wire_reader_u8(reader, &tag)) return false;

  switch (tag) {
    case ANT_WASM_WIRE_UNDEFINED:
      *output = js_mkundef();
      return true;
    case ANT_WASM_WIRE_NULL:
      *output = js_mknull();
      return true;
    case ANT_WASM_WIRE_FALSE:
      *output = js_false;
      return true;
    case ANT_WASM_WIRE_TRUE:
      *output = js_true;
      return true;
    case ANT_WASM_WIRE_NUMBER: {
      double number = 0;
      if (!wire_reader_bytes(reader, &number, sizeof(number))) return false;
      *output = js_mknum(number);
      return true;
    }
    case ANT_WASM_WIRE_STRING: {
      const uint8_t *bytes = NULL;
      uint32_t length = 0;
      if (!wire_reader_sized_bytes(reader, &bytes, &length)) return false;
      *output = js_mkstr(js, bytes, length);
      return !is_err(*output);
    }
    case ANT_WASM_WIRE_BIGINT: {
      const uint8_t *bytes = NULL;
      uint32_t length = 0;
      if (!wire_reader_sized_bytes(reader, &bytes, &length) || length == 0)
        return false;
      bool negative = bytes[0] == '-';
      const char *digits = (const char *)bytes + (negative ? 1 : 0);
      size_t digits_length = length - (negative ? 1u : 0u);
      if (digits_length == 0) return false;
      *output = js_mkbigint(js, digits, digits_length, negative);
      return !is_err(*output);
    }
    case ANT_WASM_WIRE_ARRAY: {
      uint32_t length = 0;
      if (!wire_reader_u32(reader, &length) ||
          length > ANT_WASM_WIRE_MAX_CONTAINER_ENTRIES)
        return false;

      GC_ROOT_SAVE(root_mark, js);
      ant_value_t array = js_mkarr(js);
      ant_value_t item = js_mkundef();
      GC_ROOT_PIN(js, array);
      GC_ROOT_PIN(js, item);
      if (is_err(array)) {
        GC_ROOT_RESTORE(js, root_mark);
        return false;
      }
      js_arr_reserve(js, array, length);
      for (uint32_t i = 0; i < length; i++) {
        if (!wire_decode_value(js, reader, &item, depth + 1)) {
          GC_ROOT_RESTORE(js, root_mark);
          return false;
        }
        js_arr_push(js, array, item);
      }
      if (js_arr_len(js, array) != length) {
        GC_ROOT_RESTORE(js, root_mark);
        return false;
      }
      *output = array;
      GC_ROOT_RESTORE(js, root_mark);
      return true;
    }
    case ANT_WASM_WIRE_OBJECT: {
      uint32_t length = 0;
      if (!wire_reader_u32(reader, &length) ||
          length > ANT_WASM_WIRE_MAX_CONTAINER_ENTRIES)
        return false;

      GC_ROOT_SAVE(root_mark, js);
      ant_value_t object = js_mkobj(js);
      ant_value_t item = js_mkundef();
      GC_ROOT_PIN(js, object);
      GC_ROOT_PIN(js, item);
      if (is_err(object)) {
        GC_ROOT_RESTORE(js, root_mark);
        return false;
      }
      for (uint32_t i = 0; i < length; i++) {
        const uint8_t *key_bytes = NULL;
        uint32_t key_length = 0;
        if (!wire_reader_sized_bytes(reader, &key_bytes, &key_length)) {
          GC_ROOT_RESTORE(js, root_mark);
          return false;
        }
        if (!wire_decode_value(js, reader, &item, depth + 1)) {
          GC_ROOT_RESTORE(js, root_mark);
          return false;
        }
        const char *interned_key = intern_string(
          (const char *)key_bytes, key_length
        );
        if (!interned_key) {
          GC_ROOT_RESTORE(js, root_mark);
          return false;
        }
        ant_value_t result = mkprop_interned_exact(
          js, object, interned_key, item, 0
        );
        if (is_err(result)) {
          GC_ROOT_RESTORE(js, root_mark);
          return false;
        }
      }
      *output = object;
      GC_ROOT_RESTORE(js, root_mark);
      return true;
    }
    default:
      return false;
  }
}

static bool wire_decode(
  ant_t *js, const void *data, size_t length, ant_value_t *output
) {
  ant_wire_reader_t reader = {data, length, 0};
  uint8_t version = 0;
  return wire_reader_u8(&reader, &version) &&
    version == ANT_WASM_WIRE_VERSION &&
    wire_decode_value(js, &reader, output, 0) &&
    reader.offset == reader.length;
}

static void clear_last_result(ant_wasm_runtime_t *runtime) {
  if (!runtime) return;
  free(runtime->last_result);
  runtime->last_result = NULL;
  runtime->last_result_length = 0;
}

static const uint8_t *take_last_wire(
  ant_wasm_runtime_t *runtime, ant_wire_writer_t *writer
) {
  if (!runtime || writer->length > UINT32_MAX) return NULL;
  clear_last_result(runtime);
  runtime->last_result = writer->data;
  runtime->last_result_length = (uint32_t)writer->length;
  writer->data = NULL;
  writer->length = writer->capacity = 0;
  return runtime->last_result;
}

static const uint8_t *serialize_value(
  ant_wasm_runtime_t *runtime, uint8_t status, ant_value_t value
) {
  ant_wire_writer_t writer = {0};
  bool ok = wire_writer_u8(&writer, status) &&
    wire_encode(runtime, value, &writer) &&
    !ant_wasm_deadline_expired(runtime);
  const uint8_t *result = ok ? take_last_wire(runtime, &writer) : NULL;
  free(writer.data);
  return result;
}

static bool wire_writer_string_value(
  ant_wire_writer_t *writer, const char *value, size_t length
) {
  return wire_writer_u8(writer, ANT_WASM_WIRE_STRING) &&
    wire_writer_sized_bytes(writer, value, length);
}

static const uint8_t *serialize_error_literal(
  ant_wasm_runtime_t *runtime, const char *name, const char *message
) {
  ant_wire_writer_t writer = {0};
  bool ok = wire_writer_u8(&writer, ANT_WASM_RESPONSE_ERROR) &&
    wire_writer_u8(&writer, ANT_WASM_WIRE_VERSION) &&
    wire_writer_u8(&writer, ANT_WASM_WIRE_OBJECT) &&
    wire_writer_u32(&writer, 2) &&
    wire_writer_sized_bytes(&writer, "name", 4) &&
    wire_writer_string_value(&writer, name, strlen(name)) &&
    wire_writer_sized_bytes(&writer, "message", 7) &&
    wire_writer_string_value(&writer, message, strlen(message));
  const uint8_t *result = ok ? take_last_wire(runtime, &writer) : NULL;
  free(writer.data);
  return result;
}

static const uint8_t *serialize_timeout(ant_wasm_runtime_t *runtime) {
  ant_wire_writer_t writer = {0};
  if (!wire_writer_u8(&writer, ANT_WASM_RESPONSE_TIMEOUT)) return NULL;
  const uint8_t *result = take_last_wire(runtime, &writer);
  free(writer.data);
  return result;
}

static const uint8_t *serialize_transfer_error(ant_wasm_runtime_t *runtime) {
  js_take_thrown(runtime->js, js_mkundef());
  return serialize_error_literal(
    runtime, "TypeError", "The value cannot be transferred from Ant"
  );
}

static bool define_payload_property(
  ant_t *js, ant_value_t payload, const char *name, size_t name_length,
  ant_value_t value
) {
  const char *interned_name = intern_string(name, name_length);
  return interned_name && !is_err(
    mkprop_interned_exact(js, payload, interned_name, value, 0)
  );
}

static ant_value_t error_payload(
  ant_wasm_runtime_t *runtime, ant_value_t fallback
) {
  ant_t *js = runtime->js;
  GC_ROOT_SAVE(root_mark, js);

  ant_value_t thrown = js_take_thrown(js, fallback);
  ant_value_t payload = js_mkobj(js);
  ant_value_t name = js_mkundef();
  ant_value_t message = js_mkundef();
  ant_value_t stack = js_mkundef();
  GC_ROOT_PIN(js, thrown);
  GC_ROOT_PIN(js, payload);
  GC_ROOT_PIN(js, name);
  GC_ROOT_PIN(js, message);
  GC_ROOT_PIN(js, stack);

  if (is_object_type(thrown)) {
    js_try_get_own_data_prop(js, thrown, "name", 4, &name);
    js_try_get_own_data_prop(js, thrown, "message", 7, &message);
    js_try_get_own_data_prop(js, thrown, "stack", 5, &stack);
  }
  if (vtype(name) != kTypeString)
    name = js_mkstr(js, "Error", 5);
  if (vtype(message) != kTypeString) {
    message = vtype(thrown) == kTypeString
      ? thrown : js_mkstr(js, "Ant evaluation failed", 21);
  }

  if (!define_payload_property(js, payload, "name", 4, name) ||
      !define_payload_property(js, payload, "message", 7, message)) {
    js_take_thrown(js, js_mkundef());
    payload = js_mkobj(js);
    if (!is_err(payload)) {
      define_payload_property(
        js, payload, "name", 4, js_mkstr(js, "Error", 5)
      );
      define_payload_property(
        js, payload, "message", 7,
        js_mkstr(js, "Ant evaluation failed", 21)
      );
    }
  } else if (vtype(stack) == kTypeString &&
             !define_payload_property(js, payload, "stack", 5, stack)) {
    js_take_thrown(js, js_mkundef());
  }

  GC_ROOT_RESTORE(js, root_mark);
  return payload;
}

static ant_value_t host_function_call(ant_params_t) {
  ant_wasm_runtime_t *runtime = active_runtime;
  if (!runtime || runtime->js != js)
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "Host bridge is unavailable");

  ant_value_t current = js_getcurrentfunc(js);
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
    arguments_ok = wire_encode_value(
      runtime, args[index], &writer, &active, 1
    );
  free(active.items);
  if (!arguments_ok || writer.length > UINT32_MAX) {
    free(writer.data);
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(
      js, JS_ERR_TYPE, "Host function arguments cannot be transferred"
    );
  }

  uint32_t response_length = 0;
  uint32_t response_pointer = ant_wasm_host_call(
    (int32_t)js_getnum(data), (const char *)writer.data,
    (uint32_t)writer.length, &response_length
  );
  free(writer.data);
  if (!response_pointer || response_length < 2) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "Host function returned an invalid response");
  }

  uint8_t *response = (uint8_t *)(uintptr_t)response_pointer;
  uint8_t status = response[0];
  ant_value_t decoded = js_mkundef();
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
    ant_value_t message = js_mkundef();
    if (is_object_type(decoded))
      js_try_get_own_data_prop(js, decoded, "message", 7, &message);
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

  ant_t *js = ant_create();
  if (!js) {
    free(runtime);
    return NULL;
  }

  runtime->js = js;
  active_runtime = runtime;
  ant_wasm_microtasks_reset(js);
  ant_runtime_init(js, 0, NULL, NULL);
  init_symbol_module(js);
  init_generator_module(js);
  init_math_module(js);
  init_bigint_module(js);
  init_regex_module(js);
  init_collections_module(js);
  init_json_module(js);
  return runtime;
}

ANT_WASM_EXPORT("ant_destroy")
void ant_wasm_destroy(ant_wasm_runtime_t *runtime) {
  if (!runtime) return;
  ant_wasm_microtasks_reset(runtime->js);
  if (active_runtime == runtime) active_runtime = NULL;
  js_destroy(runtime->js);
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
  const char *name, uint32_t name_length,
  const uint8_t *wire_value, uint32_t wire_length,
  int32_t function_id
) {
  if (!runtime || !name) return NULL;

  ant_t *js = runtime->js;
  volatile char stack_base;
  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, 768 * 1024);

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t value = js_mkundef();
  GC_ROOT_PIN(js, value);
  if (function_id >= 0) {
    value = js_heavy_mkfun(js, host_function_call, js_mknum((double)function_id));
  } else if (!wire_value || !wire_decode(js, wire_value, wire_length, &value)) {
    js_take_thrown(js, js_mkundef());
    const uint8_t *response = serialize_transfer_error(runtime);
    GC_ROOT_RESTORE(js, root_mark);
    return response;
  }

  if (is_err(value)) {
    ant_value_t payload = error_payload(runtime, value);
    const uint8_t *response = serialize_value(
      runtime, ANT_WASM_RESPONSE_ERROR, payload
    );
    GC_ROOT_RESTORE(js, root_mark);
    return response;
  }

  const char *interned_name = intern_string(name, name_length);
  ant_value_t set_result = interned_name
    ? mkprop_interned_exact(js, js->global, interned_name, value, 0)
    : js_mkerr_typed(js, JS_ERR_INTERNAL, "Unable to intern global name");
  if (is_err(set_result)) {
    ant_value_t payload = error_payload(runtime, set_result);
    const uint8_t *response = serialize_value(
      runtime, ANT_WASM_RESPONSE_ERROR, payload
    );
    GC_ROOT_RESTORE(js, root_mark);
    return response;
  }

  const uint8_t *response = serialize_value(
    runtime, ANT_WASM_RESPONSE_OK, js_mkundef()
  );
  GC_ROOT_RESTORE(js, root_mark);
  return response;
}

ANT_WASM_EXPORT("ant_eval")
const uint8_t *ant_wasm_eval(
  ant_wasm_runtime_t *runtime,
  const char *source, uint32_t source_length,
  uint32_t timeout_ms
) {
  if (!runtime || !source) return NULL;

  ant_t *js = runtime->js;
  volatile char stack_base;
  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, 768 * 1024);
  js->wasm_interrupt_ticks = 0;
  js->wasm_interrupt_enabled = timeout_ms > 0;
  runtime->timed_out = false;
  runtime->deadline_ms = js->wasm_interrupt_enabled
    ? ant_wasm_now_ms() + (double)timeout_ms : 0.0;

  ant_value_t result = js_eval_bytecode_eval(js, source, source_length);
  if (!runtime->timed_out) js_maybe_drain_microtasks(js);

  const uint8_t *response = NULL;
  if (runtime->timed_out) {
    js_take_thrown(js, js_mkundef());
    ant_wasm_microtasks_reset(js);
    response = serialize_timeout(runtime);
  } else if (is_err(result)) {
    ant_value_t payload = error_payload(runtime, result);
    response = serialize_value(
      runtime, ANT_WASM_RESPONSE_ERROR, payload
    );
    if (!response && runtime->timed_out) {
      js_take_thrown(js, js_mkundef());
      ant_wasm_microtasks_reset(js);
      response = serialize_timeout(runtime);
    } else if (!response) {
      response = serialize_transfer_error(runtime);
    }
  } else {
    response = serialize_value(runtime, ANT_WASM_RESPONSE_OK, result);
    if (!response && runtime->timed_out) {
      js_take_thrown(js, js_mkundef());
      ant_wasm_microtasks_reset(js);
      response = serialize_timeout(runtime);
    } else if (!response) {
      response = serialize_transfer_error(runtime);
    }
  }

  js->wasm_interrupt_enabled = false;
  runtime->deadline_ms = 0.0;
  return response;
}
