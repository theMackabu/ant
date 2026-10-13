#include "stringify.h"

#include "ant.h"
#include "utils.h"
#include "internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static constexpr size_t STRINGIFY_MAX_ARRAY_LENGTH = 100;

static size_t stringify_hex_bytes(const uint8_t *data, size_t count, bool lead_space, char *buf, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < count; i++) {
    if (lead_space || i > 0) n += cpy(buf + n, REMAIN(n, len), " ", 1);
    n += (size_t)snprintf(buf + n, REMAIN(n, len), "%02x", data[i]);
  }
  return n;
}

static size_t stringify_typed_element(ant_t *js, const TypedArrayData *ta, size_t i, char *buf, size_t len) {
  ant_value_t value;
  if (!buffer_typedarray_data_read_index(js, ta, i, &value)) return 0;
  if (vtype(value) == kTypeNumber && tod(value) == 0 && signbit(tod(value))) return cpy(buf, len, "-0", 2);
  
  size_t n = tostr(js, value, buf, len);
  return vtype(value) == kTypeBigInt ? n + cpy(buf + n, REMAIN(n, len), "n", 1) : n;
}

static bool stringify_is_buffer(ant_t *js, ant_value_t obj) {
  ant_value_t proto = js_get_proto(js, obj);
  ant_value_t buffer_proto = js_get_ctor_proto(js, "Buffer", 6);
  return vtype(proto) == kTypeObject && vtype(buffer_proto) == kTypeObject && vdata(proto) == vdata(buffer_proto);
}

static size_t stringify_more_bytes(size_t remaining, char *buf, size_t len) {
  return (size_t)snprintf(buf, len, " ... %zu more byte%s", remaining, remaining == 1 ? "" : "s");
}

static size_t stringify_node_buffer(const uint8_t *data, size_t byte_length, char *buf, size_t len) {
  size_t shown = byte_length < BUFFER_INSPECT_MAX_BYTES ? byte_length : BUFFER_INSPECT_MAX_BYTES;
  size_t n = cpy(buf, len, "<Buffer", 7);
  n += stringify_hex_bytes(data, shown, true, buf + n, REMAIN(n, len));
  if (shown < byte_length) n += stringify_more_bytes(byte_length - shown, buf + n, REMAIN(n, len));
  return n + cpy(buf + n, REMAIN(n, len), shown == 0 ? " >" : ">", shown == 0 ? 2 : 1);
}

size_t stringify_typedarray(ant_t *js, ant_value_t obj, const TypedArrayData *ta, char *buf, size_t len) {
  bool detached = ta->buffer->is_detached;
  size_t length = detached ? 0 : ta->length;
  const uint8_t *data = ta->buffer->data + ta->byte_offset;
  
  if (stringify_is_buffer(js, obj)) return stringify_node_buffer(data, detached ? 0 : ta->byte_length, buf, len);
  
  const char *name = ta->type <= TYPED_ARRAY_BIGUINT64 ? buffer_typedarray_type_name(ta->type) : "TypedArray";
  size_t n = cpy(buf, len, name, strlen(name));
  n += (size_t)snprintf(buf + n, REMAIN(n, len), "(%zu) [", length);
  if (length == 0) return n + cpy(buf + n, REMAIN(n, len), "]", 1);
  
  size_t shown = length < STRINGIFY_MAX_ARRAY_LENGTH ? length : STRINGIFY_MAX_ARRAY_LENGTH;
  for (size_t i = 0; i < shown; i++) {
    n += cpy(buf + n, REMAIN(n, len), i ? ", " : " ", i ? 2 : 1);
    n += stringify_typed_element(js, ta, i, buf + n, REMAIN(n, len));
  }
  
  if (shown < length) n += cpy(buf + n, REMAIN(n, len), ", ...", 5);
  return n + cpy(buf + n, REMAIN(n, len), " ]", 2);
}

static size_t stringify_arraybuffer_body(const ArrayBufferData *ab, const char *indent, char *buf, size_t len) {
  const char *name = ab->is_shared ? "SharedArrayBuffer" : "ArrayBuffer";
  if (ab->is_detached) return (size_t)snprintf(buf, len, "%s { (detached), [byteLength]: 0 }", name);
  
  size_t shown = ab->length < STRINGIFY_MAX_ARRAY_LENGTH ? ab->length : STRINGIFY_MAX_ARRAY_LENGTH;
  size_t n = (size_t)snprintf(buf, len, "%s {\n%s  [Uint8Contents]: <", name, indent);
  
  if (ab->data) n += stringify_hex_bytes(ab->data, shown, false, buf + n, REMAIN(n, len));
  if (shown < ab->length) n += stringify_more_bytes(ab->length - shown, buf + n, REMAIN(n, len));
  
  return n + (size_t)snprintf(buf + n, REMAIN(n, len), ">,\n%s  [byteLength]: %zu\n%s}", indent, ab->length, indent);
}

size_t stringify_arraybuffer(const ArrayBufferData *ab, char *buf, size_t len) {
  return stringify_arraybuffer_body(ab, "", buf, len);
}

size_t stringify_dataview(const DataViewData *dv, char *buf, size_t len) {
  size_t n = dv->buffer->is_detached
    ? (size_t)snprintf(buf, len, "DataView {\n  [byteLength]: 0,\n  [byteOffset]: undefined,\n  [buffer]: ")
    : (size_t)snprintf(
      buf, len, "DataView {\n  [byteLength]: %zu,\n  [byteOffset]: %zu,\n  [buffer]: ",
      dv->byte_length, dv->byte_offset
    );
  
  n += stringify_arraybuffer_body(dv->buffer, "  ", buf + n, REMAIN(n, len));
  return n + cpy(buf + n, REMAIN(n, len), "\n}", 2);
}

size_t stringify_blob(const blob_data_t *bd, bool is_file, char *buf, size_t len) {
  size_t n = (size_t)snprintf(buf, len, "%s { size: %zu, type: '%s'", is_file ? "File" : "Blob", bd->size, bd->type ? bd->type : "");
  if (is_file) n += (size_t)snprintf(
    buf + n, REMAIN(n, len), ", name: '%s', lastModified: %" PRId64,
    bd->name ? bd->name : "", bd->last_modified
  );
  
  return n + cpy(buf + n, REMAIN(n, len), " }", 2);
}
