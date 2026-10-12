#include "stringify.h"

#include "ant.h"
#include "utils.h"
#include "internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static size_t stringify_hex_bytes(const uint8_t *data, size_t count, bool lead_space, char *buf, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < count; i++) {
    if (lead_space || i > 0) n += cpy(buf + n, REMAIN(n, len), " ", 1);
    n += (size_t)snprintf(buf + n, REMAIN(n, len), "%02x", data[i]);
  }
  return n;
}

static size_t stringify_typed_element(TypedArrayType type, const uint8_t *data, size_t i, char *buf, size_t len) {
  switch (type) {
    case TYPED_ARRAY_INT8:          return (size_t)snprintf(buf, len, "%d", (int)((const int8_t *)data)[i]);
    case TYPED_ARRAY_UINT8:
    case TYPED_ARRAY_UINT8_CLAMPED: return (size_t)snprintf(buf, len, "%u", (unsigned)data[i]);
    case TYPED_ARRAY_INT16:         return (size_t)snprintf(buf, len, "%d", (int)((const int16_t *)data)[i]);
    case TYPED_ARRAY_UINT16:        return (size_t)snprintf(buf, len, "%u", (unsigned)((const uint16_t *)data)[i]);
    case TYPED_ARRAY_INT32:         return (size_t)snprintf(buf, len, "%d", ((const int32_t *)data)[i]);
    case TYPED_ARRAY_UINT32:        return (size_t)snprintf(buf, len, "%u", ((const uint32_t *)data)[i]);
    case TYPED_ARRAY_FLOAT16:       return (size_t)snprintf(buf, len, "%g", half_to_double(((const uint16_t *)data)[i]));
    case TYPED_ARRAY_FLOAT32:       return (size_t)snprintf(buf, len, "%g", (double)((const float *)data)[i]);
    case TYPED_ARRAY_FLOAT64:       return (size_t)snprintf(buf, len, "%g", ((const double *)data)[i]);
    case TYPED_ARRAY_BIGINT64:      return (size_t)snprintf(buf, len, "%lldn", (long long)((const int64_t *)data)[i]);
    case TYPED_ARRAY_BIGUINT64:     return (size_t)snprintf(buf, len, "%llun", (unsigned long long)((const uint64_t *)data)[i]);
    default:                        return (size_t)snprintf(buf, len, "%u", (unsigned)data[i]);
  }
}

static bool stringify_is_buffer(ant_t *js, ant_value_t obj) {
  ant_value_t proto = js_get_proto(js, obj);
  ant_value_t buffer_proto = js_get_ctor_proto(js, "Buffer", 6);
  return vtype(proto) == kTypeObject && vtype(buffer_proto) == kTypeObject && vdata(proto) == vdata(buffer_proto);
}

static size_t stringify_node_buffer(const TypedArrayData *ta, const uint8_t *data, char *buf, size_t len) {
  size_t shown = ta->byte_length < BUFFER_INSPECT_MAX_BYTES ? ta->byte_length : BUFFER_INSPECT_MAX_BYTES;
  size_t n = cpy(buf, len, "<Buffer", 7);
  n += stringify_hex_bytes(data, shown, true, buf + n, REMAIN(n, len));
  
  if (shown < ta->byte_length) {
    size_t remaining = ta->byte_length - shown;
    n += (size_t)snprintf(buf + n, REMAIN(n, len), " ... %zu more byte%s", remaining, remaining == 1 ? "" : "s");
  }
  
  return n + cpy(buf + n, REMAIN(n, len), shown == 0 ? " >" : ">", shown == 0 ? 2 : 1);
}

size_t stringify_typedarray(ant_t *js, ant_value_t obj, const TypedArrayData *ta, char *buf, size_t len) {
  const uint8_t *data = ta->buffer->data + ta->byte_offset;
  if (stringify_is_buffer(js, obj)) return stringify_node_buffer(ta, data, buf, len);
  
  const char *name = ta->type <= TYPED_ARRAY_BIGUINT64 ? buffer_typedarray_type_name(ta->type) : "TypedArray";
  size_t n = cpy(buf, len, name, strlen(name));
  n += (size_t)snprintf(buf + n, REMAIN(n, len), "(%zu) [ ", ta->length);
  
  for (size_t i = 0; i < ta->length && i < 100; i++) {
    if (i > 0) n += cpy(buf + n, REMAIN(n, len), ", ", 2);
    n += stringify_typed_element(ta->type, data, i, buf + n, REMAIN(n, len));
  }
  
  if (ta->length > 100) n += cpy(buf + n, REMAIN(n, len), ", ...", 5);
  return n + cpy(buf + n, REMAIN(n, len), " ]", 2);
}

size_t stringify_arraybuffer(const ArrayBufferData *ab, char *buf, size_t len) {
  size_t n = (size_t)snprintf(buf, len, "%s {\n  [Uint8Contents]: <", ab->is_shared ? "SharedArrayBuffer" : "ArrayBuffer");
  if (ab->data) n += stringify_hex_bytes(ab->data, ab->length, false, buf + n, REMAIN(n, len));
  return n + (size_t)snprintf(buf + n, REMAIN(n, len), ">,\n  [byteLength]: %zu\n}", ab->length);
}

size_t stringify_dataview(const DataViewData *dv, char *buf, size_t len) {
  size_t n = (size_t)snprintf(
    buf, len, "DataView {\n  [byteLength]: %zu,\n  [byteOffset]: %zu,\n  [buffer]: ArrayBuffer {\n    [Uint8Contents]: <",
    dv->byte_length, dv->byte_offset
  );
  if (dv->buffer->data) n += stringify_hex_bytes(dv->buffer->data, dv->buffer->length, false, buf + n, REMAIN(n, len));
  return n + (size_t)snprintf(buf + n, REMAIN(n, len), ">,\n    [byteLength]: %zu\n  }\n}", dv->buffer->length);
}

size_t stringify_blob(const blob_data_t *bd, bool is_file, char *buf, size_t len) {
  size_t n = (size_t)snprintf(buf, len, "%s { size: %zu, type: '%s'", is_file ? "File" : "Blob", bd->size, bd->type ? bd->type : "");
  if (is_file) n += (size_t)snprintf(
    buf + n, REMAIN(n, len), ", name: '%s', lastModified: %" PRId64,
    bd->name ? bd->name : "", bd->last_modified
  );
  return n + cpy(buf + n, REMAIN(n, len), " }", 2);
}
