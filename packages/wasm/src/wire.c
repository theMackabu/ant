#include <stdlib.h>
#include <string.h>

#include "wire.h"
#include "gc/roots.h"
#include "gc/strings.h"
#include "modules/bigint.h"
#include "props.h"
#include "silver/engine.h"
#include "utf8.h"

#define WIRE_TRY(cond) do { if (!(cond)) return false; } while (0)

#define WIRE_ROOT_SCOPE(js)                                                  \
  wire_root_scope_t wire_root_scope_                                         \
    __attribute__((cleanup(wire_root_scope_leave))) = {(js), gc_root_scope(js)}

#define WIRE_AUTOFREE __attribute__((cleanup(wire_free_bytes)))

typedef struct {
  ant_t *js;
  size_t mark;
} wire_root_scope_t;

typedef struct {
  const uint8_t *data;
  size_t length;
  size_t offset;
} ant_wire_reader_t;

typedef bool (*wire_container_encoder_t)(
  ant_wasm_runtime_t *runtime, ant_value_t value, ant_wire_writer_t *writer,
  ant_wire_active_t *active, unsigned depth
);

static void wire_root_scope_leave(wire_root_scope_t *scope) {
  gc_pop_roots(scope->js, scope->mark);
}

static void wire_free_bytes(char **bytes) {
  free(*bytes);
}

static inline void wire_store_u16(uint8_t *output, uint16_t value) {
  output[0] = (uint8_t)value;
  output[1] = (uint8_t)(value >> 8);
}

static inline uint16_t wire_load_u16(const uint8_t *input) {
  return (uint16_t)(input[0] | input[1] << 8);
}

static inline uint32_t wire_load_u32(const uint8_t *input) {
  return wire_load_u16(input) | (uint32_t)wire_load_u16(input + 2) << 16;
}

static bool wire_writer_reserve(ant_wire_writer_t *writer, size_t extra) {
  WIRE_TRY(extra <= ANT_WASM_WIRE_MAX_BYTES - writer->length);
  size_t needed = writer->length + extra;
  if (needed <= writer->capacity) return true;

  size_t capacity = writer->capacity ? writer->capacity : 256;
  while (capacity < needed) capacity *= 2;

  uint8_t *data = realloc(writer->data, capacity);
  WIRE_TRY(data);
  writer->data = data;
  writer->capacity = capacity;
  return true;
}

static bool wire_writer_bytes(
  ant_wire_writer_t *writer, const void *bytes, size_t length
) {
  WIRE_TRY(wire_writer_reserve(writer, length));
  if (length) memcpy(writer->data + writer->length, bytes, length);
  writer->length += length;
  return true;
}

bool wire_writer_u8(ant_wire_writer_t *writer, uint8_t value) {
  return wire_writer_bytes(writer, &value, sizeof(value));
}

bool wire_writer_u32(ant_wire_writer_t *writer, uint32_t value) {
  uint8_t bytes[4];
  wire_store_u16(bytes, (uint16_t)value);
  wire_store_u16(bytes + 2, (uint16_t)(value >> 16));
  return wire_writer_bytes(writer, bytes, sizeof(bytes));
}

bool wire_writer_string(
  ant_wire_writer_t *writer, const char *bytes, size_t length
) {
  size_t unit_count = utf16_strlen(bytes, length);
  WIRE_TRY(unit_count <= UINT32_MAX / 2);
  WIRE_TRY(wire_writer_u32(writer, (uint32_t)unit_count));
  WIRE_TRY(wire_writer_reserve(writer, unit_count * 2));

  uint8_t *output = writer->data + writer->length;
  utf16_iterator_t iterator = utf16_iterator(bytes, length);
  uint16_t unit = 0;
  for (size_t i = 0; i < unit_count; i++) {
    WIRE_TRY(utf16_next_code_unit(&iterator, &unit));
    wire_store_u16(output + i * 2, unit);
  }
  WIRE_TRY(!utf16_next_code_unit(&iterator, &unit));

  writer->length += unit_count * 2;
  return true;
}

char *wire_utf16_to_wtf8(
  const uint8_t *units, size_t unit_count, size_t *length
) {
  if (unit_count > (SIZE_MAX - 1) / 3) return NULL;
  char *output = malloc(unit_count * 3 + 1);
  if (!output) return NULL;

  size_t written = 0;
  for (size_t i = 0; i < unit_count; i++)
    written += utf16_code_unit_to_wtf8(wire_load_u16(units + i * 2), output + written);

  written = wtf8_join_surrogate_pairs(output, written);
  output[written] = '\0';
  *length = written;
  return output;
}

static const uint8_t *wire_reader_take(
  ant_wire_reader_t *reader, size_t count, size_t size
) {
  if (count > (reader->length - reader->offset) / size) return NULL;
  const uint8_t *bytes = reader->data + reader->offset;
  reader->offset += count * size;
  return bytes;
}

static bool wire_reader_bytes(
  ant_wire_reader_t *reader, void *output, size_t length
) {
  const uint8_t *bytes = wire_reader_take(reader, length, 1);
  WIRE_TRY(bytes);
  memcpy(output, bytes, length);
  return true;
}

static bool wire_reader_u8(ant_wire_reader_t *reader, uint8_t *output) {
  return wire_reader_bytes(reader, output, sizeof(*output));
}

static bool wire_reader_u32(ant_wire_reader_t *reader, uint32_t *output) {
  const uint8_t *bytes = wire_reader_take(reader, 4, 1);
  WIRE_TRY(bytes);
  *output = wire_load_u32(bytes);
  return true;
}

static bool wire_reader_count(ant_wire_reader_t *reader, uint32_t *count) {
  return wire_reader_u32(reader, count) &&
    *count <= ANT_WASM_WIRE_MAX_CONTAINER_ENTRIES;
}

static char *wire_reader_string(ant_wire_reader_t *reader, size_t *length) {
  uint32_t unit_count = 0;
  if (!wire_reader_u32(reader, &unit_count)) return NULL;
  const uint8_t *units = wire_reader_take(reader, unit_count, 2);
  return units ? wire_utf16_to_wtf8(units, unit_count, length) : NULL;
}

static bool wire_active_push(ant_wire_active_t *active, ant_object_t *object) {
  for (size_t i = 0; i < active->length; i++)
    WIRE_TRY(active->items[i] != object);

  if (active->length == active->capacity) {
    size_t capacity = active->capacity ? active->capacity * 2 : 16;
    ant_object_t **items = realloc(active->items, capacity * sizeof(*items));
    WIRE_TRY(items);
    active->items = items;
    active->capacity = capacity;
  }

  active->items[active->length++] = object;
  return true;
}

static bool encode_number(ant_wire_writer_t *writer, double number) {
  return wire_writer_u8(writer, ANT_WASM_WIRE_NUMBER) &&
    wire_writer_bytes(writer, &number, sizeof(number));
}

static bool encode_tagged_string(
  ant_wire_writer_t *writer, uint8_t tag, const char *bytes, size_t length
) {
  return bytes && wire_writer_u8(writer, tag) &&
    wire_writer_string(writer, bytes, length);
}

static bool encode_string(
  ant_t *js, ant_value_t value, ant_wire_writer_t *writer
) {
  size_t length = 0;
  const char *bytes = js_getstr(js, value, &length);
  return encode_tagged_string(writer, ANT_WASM_WIRE_STRING, bytes, length);
}

static bool encode_bigint(
  ant_t *js, ant_value_t value, ant_wire_writer_t *writer
) {
  size_t length = strbigint(js, value, NULL, 0);
  WIRE_AUTOFREE char *bytes = malloc(length + 1);
  return bytes && strbigint(js, value, bytes, length + 1) == length &&
    encode_tagged_string(writer, ANT_WASM_WIRE_BIGINT, bytes, length);
}

static bool encode_container_header(
  ant_wire_writer_t *writer, uint8_t tag, ant_offset_t length
) {
  return length <= ANT_WASM_WIRE_MAX_CONTAINER_ENTRIES &&
    wire_writer_u8(writer, tag) &&
    wire_writer_u32(writer, (uint32_t)length);
}

static bool encode_entry(
  ant_wasm_runtime_t *runtime, ant_value_t item, ant_wire_writer_t *writer,
  ant_wire_active_t *active, unsigned depth
) {
  return !is_err(item) &&
    wire_encode_value(runtime, item, writer, active, depth + 1);
}

static bool encode_array(
  ant_wasm_runtime_t *runtime, ant_value_t value, ant_wire_writer_t *writer,
  ant_wire_active_t *active, unsigned depth
) {
  ant_t *js = runtime->js;
  ant_offset_t length = js_arr_len(js, value);
  WIRE_TRY(encode_container_header(writer, ANT_WASM_WIRE_ARRAY, length));

  WIRE_ROOT_SCOPE(js);
  ant_value_t item = Ant_Value_Undefined;
  GC_ROOT_PIN(js, item);

  for (ant_offset_t i = 0; i < length; i++) {
    WIRE_TRY(!ant_wasm_deadline_expired(runtime));
    item = js_arr_get(js, value, i);
    WIRE_TRY(encode_entry(runtime, item, writer, active, depth));
  }
  return true;
}

static bool encode_object(
  ant_wasm_runtime_t *runtime, ant_value_t value, ant_wire_writer_t *writer,
  ant_wire_active_t *active, unsigned depth
) {
  ant_t *js = runtime->js;
  WIRE_ROOT_SCOPE(js);
  ant_value_t keys = js_own_property_keys(js, value, false, true);
  ant_value_t key = Ant_Value_Undefined;
  ant_value_t item = Ant_Value_Undefined;
  GC_ROOT_PIN(js, keys);
  GC_ROOT_PIN(js, key);
  GC_ROOT_PIN(js, item);

  WIRE_TRY(!is_err(keys) && vtype(keys) == kTypeArray);
  ant_offset_t length = js_arr_len(js, keys);
  WIRE_TRY(encode_container_header(writer, ANT_WASM_WIRE_OBJECT, length));

  for (ant_offset_t i = 0; i < length; i++) {
    WIRE_TRY(!ant_wasm_deadline_expired(runtime));
    key = js_arr_get(js, keys, i);
    size_t key_length = 0;
    const char *key_bytes = js_getstr(js, key, &key_length);
    WIRE_TRY(key_bytes && wire_writer_string(writer, key_bytes, key_length));
    item = js_getprop_fallback_len(js, value, key_bytes, key_length);
    WIRE_TRY(encode_entry(runtime, item, writer, active, depth));
  }
  return true;
}

static bool encode_container(
  ant_wasm_runtime_t *runtime, ant_value_t value, ant_wire_writer_t *writer,
  ant_wire_active_t *active, unsigned depth, wire_container_encoder_t encode
) {
  ant_object_t *object = js_obj_ptr(js_as_obj(value));
  WIRE_TRY(object && wire_active_push(active, object));
  bool ok = encode(runtime, value, writer, active, depth);
  active->length--;
  return ok;
}

bool wire_encode_value(
  ant_wasm_runtime_t *runtime, ant_value_t value, ant_wire_writer_t *writer,
  ant_wire_active_t *active, unsigned depth
) {
  WIRE_TRY(depth <= ANT_WASM_WIRE_MAX_DEPTH);
  WIRE_TRY(!ant_wasm_deadline_expired(runtime));

  ant_t *js = runtime->js;
  WIRE_ROOT_SCOPE(js);
  GC_ROOT_PIN(js, value);

  switch (vtype(value)) {
    case kTypeUndefined: return wire_writer_u8(writer, ANT_WASM_WIRE_UNDEFINED);
    case kTypeNull:      return wire_writer_u8(writer, ANT_WASM_WIRE_NULL);
    case kTypeBool:      return wire_writer_u8(writer, vdata(value) ? ANT_WASM_WIRE_TRUE : ANT_WASM_WIRE_FALSE);
    case kTypeNumber:    return encode_number(writer, Ant_Value_AsNumberUnchecked(value));
    case kTypeString:    return encode_string(js, value, writer);
    case kTypeBigInt:    return encode_bigint(js, value, writer);
    case kTypeArray:     return encode_container(runtime, value, writer, active, depth, encode_array);
    case kTypeObject:    return encode_container(runtime, value, writer, active, depth, encode_object);
    default:             return false;
  }
}

bool wire_encode(
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
);

static bool decode_number(ant_wire_reader_t *reader, ant_value_t *output) {
  double number = 0;
  WIRE_TRY(wire_reader_bytes(reader, &number, sizeof(number)));
  *output = Ant_Value_FromNumber(number);
  return true;
}

static bool decode_string(
  ant_t *js, ant_wire_reader_t *reader, ant_value_t *output
) {
  size_t length = 0;
  WIRE_AUTOFREE char *bytes = wire_reader_string(reader, &length);
  WIRE_TRY(bytes);
  *output = js_mkstr(js, bytes, length);
  return !is_err(*output);
}

static bool decode_bigint(
  ant_t *js, ant_wire_reader_t *reader, ant_value_t *output
) {
  size_t length = 0;
  WIRE_AUTOFREE char *bytes = wire_reader_string(reader, &length);
  WIRE_TRY(bytes);

  bool negative = length > 0 && bytes[0] == '-';
  size_t digits_length = length - negative;
  WIRE_TRY(digits_length);
  *output = js_mkbigint(js, bytes + negative, digits_length, negative);
  return !is_err(*output);
}

static const char *decode_key(ant_t *js, ant_wire_reader_t *reader) {
  size_t length = 0;
  WIRE_AUTOFREE char *bytes = wire_reader_string(reader, &length);
  return bytes ? intern_string(js, bytes, length) : NULL;
}

static bool decode_array(
  ant_t *js, ant_wire_reader_t *reader, ant_value_t *output, unsigned depth
) {
  uint32_t length = 0;
  WIRE_TRY(wire_reader_count(reader, &length));

  WIRE_ROOT_SCOPE(js);
  ant_value_t array = Ant_Array_Create(js);
  ant_value_t item = Ant_Value_Undefined;
  GC_ROOT_PIN(js, array);
  GC_ROOT_PIN(js, item);
  WIRE_TRY(!is_err(array));

  js_arr_reserve(js, array, length);
  for (uint32_t i = 0; i < length; i++) {
    WIRE_TRY(wire_decode_value(js, reader, &item, depth + 1));
    js_arr_push(js, array, item);
  }
  WIRE_TRY(js_arr_len(js, array) == length);

  *output = array;
  return true;
}

static bool decode_object(
  ant_t *js, ant_wire_reader_t *reader, ant_value_t *output, unsigned depth
) {
  uint32_t length = 0;
  WIRE_TRY(wire_reader_count(reader, &length));

  WIRE_ROOT_SCOPE(js);
  ant_value_t object = Ant_Object_Create(js);
  ant_value_t item = Ant_Value_Undefined;
  GC_ROOT_PIN(js, object);
  GC_ROOT_PIN(js, item);
  WIRE_TRY(!is_err(object));

  for (uint32_t i = 0; i < length; i++) {
    const char *key = decode_key(js, reader);
    WIRE_TRY(key && wire_decode_value(js, reader, &item, depth + 1));
    WIRE_TRY(!is_err(mkprop_interned_exact(js, object, key, item, 0)));
  }

  *output = object;
  return true;
}

static bool wire_decode_value(
  ant_t *js, ant_wire_reader_t *reader, ant_value_t *output, unsigned depth
) {
  uint8_t tag = 0;
  WIRE_TRY(depth <= ANT_WASM_WIRE_MAX_DEPTH);
  WIRE_TRY(wire_reader_u8(reader, &tag));

  switch (tag) {
    case ANT_WASM_WIRE_UNDEFINED: *output = Ant_Value_Undefined; return true;
    case ANT_WASM_WIRE_NULL:      *output = Ant_Value_Null;      return true;
    case ANT_WASM_WIRE_FALSE:     *output = Ant_Value_False;     return true;
    case ANT_WASM_WIRE_TRUE:      *output = Ant_Value_True;      return true;
    case ANT_WASM_WIRE_NUMBER:    return decode_number(reader, output);
    case ANT_WASM_WIRE_STRING:    return decode_string(js, reader, output);
    case ANT_WASM_WIRE_BIGINT:    return decode_bigint(js, reader, output);
    case ANT_WASM_WIRE_ARRAY:     return decode_array(js, reader, output, depth);
    case ANT_WASM_WIRE_OBJECT:    return decode_object(js, reader, output, depth);
    default:                      return false;
  }
}

bool wire_decode(
  ant_t *js, const void *data, size_t length, ant_value_t *output
) {
  ant_wire_reader_t reader = {data, length, 0};
  uint8_t version = 0;
  return wire_reader_u8(&reader, &version) &&
    version == ANT_WASM_WIRE_VERSION &&
    wire_decode_value(js, &reader, output, 0) &&
    reader.offset == reader.length;
}
