#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "ant.h"
#include "ptr.h"
#include "utf8.h"
#include "utils.h"
#include "errors.h"
#include "base64.h"
#include "internal.h"
#include "runtime.h"
#include "gc/roots.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/symbol.h"

#define BUFFER_REGISTRY_INITIAL_CAP 64

// Node compatibility exports only
// Ant does not enforce these as allocation limits
#define BUFFER_COMPAT_MAX_LENGTH 4294967296.0
#define BUFFER_COMPAT_MAX_STRING_LENGTH 536870888.0
#define BUFFER_COMPAT_INSPECT_MAX_BYTES 50.0

static size_t ta_metadata_bytes     = 0;
static size_t buffer_registry_count = 0;
static size_t buffer_registry_cap   = 0;

static ArrayBufferData **buffer_registry   = NULL;
static ant_value_t g_typedarray_iter_proto = 0;

enum {
  BUFFER_ARRAYBUFFER_NATIVE_TAG = 0x41425546u, // ABUF
  BUFFER_TYPEDARRAY_NATIVE_TAG  = 0x54594152u, // TYAR
  BUFFER_DATAVIEW_NATIVE_TAG    = 0x44564957u, // DVIW
};

static void *ta_meta_alloc(size_t size) {
  void *ptr = ant_calloc(size);
  if (!ptr) return NULL;
  ta_metadata_bytes += size;
  return ptr;
}

static void ta_meta_free(void *ptr, size_t size) {
  if (!ptr) return;
  if (ta_metadata_bytes >= size) ta_metadata_bytes -= size;
  else ta_metadata_bytes = 0;
  free(ptr);
}

ArrayBufferData *buffer_get_arraybuffer_data(ant_value_t value) {
  if (!is_object_type(value) || buffer_is_dataview(value)) return NULL;
  return (ArrayBufferData *)js_get_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG);
}

TypedArrayData *buffer_get_typedarray_data(ant_value_t value) {
  if (vtype(value) == T_TYPEDARRAY)
    return (TypedArrayData *)js_gettypedarray(value);
  if (!is_object_type(value)) return NULL;
  return (TypedArrayData *)js_get_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);
}

DataViewData *buffer_get_dataview_data(ant_value_t value) {
  if (!is_object_type(value)) return NULL;
  return (DataViewData *)js_get_native(value, BUFFER_DATAVIEW_NATIVE_TAG);
}

static void arraybuffer_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ArrayBufferData *data = (ArrayBufferData *)js_get_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  if (!data) return;
  js_clear_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  free_array_buffer_data(data);
}

static void typedarray_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  TypedArrayData *ta_data = (TypedArrayData *)js_get_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);
  if (!ta_data) return;
  js_clear_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);

  if (ta_data->buffer) free_array_buffer_data(ta_data->buffer);
  ta_meta_free(ta_data, sizeof(*ta_data));
}

static void dataview_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  DataViewData *dv_data = (DataViewData *)js_get_native(value, BUFFER_DATAVIEW_NATIVE_TAG);
  if (!dv_data) return;
  js_clear_native(value, BUFFER_DATAVIEW_NATIVE_TAG);

  if (dv_data->buffer) free_array_buffer_data(dv_data->buffer);
  ta_meta_free(dv_data, sizeof(*dv_data));
}

bool buffer_is_dataview(ant_value_t obj) {
  return js_check_brand(obj, BRAND_DATAVIEW);
}

bool buffer_is_binary_source(ant_value_t value) {
  if (vtype(value) == T_TYPEDARRAY) return true;
  if (!is_object_type(value)) return false;
  if (buffer_is_dataview(value)) return true;
  return buffer_get_typedarray_data(value) != NULL || buffer_get_arraybuffer_data(value) != NULL;
}

bool buffer_source_get_bytes(ant_t *js, ant_value_t value, const uint8_t **out, size_t *len) {
  if (out) *out = NULL;
  if (len) *len = 0;
  if (!buffer_is_binary_source(value)) return false;

  TypedArrayData *ta = buffer_get_typedarray_data(value);
  
  if (ta) {
    if (!ta->buffer || ta->buffer->is_detached) { *out = NULL; *len = 0; return true; }
    *out = ta->buffer->data + ta->byte_offset;
    *len = ta->byte_length;
    return true;
  }

  ArrayBufferData *ab = buffer_get_arraybuffer_data(value);
  if (ab) {
    if (ab->is_detached) { *out = NULL; *len = 0; return true; }
    *out = ab->data;
    *len = ab->length;
    return true;
  }

  if (buffer_is_dataview(value)) {
    DataViewData *dv = buffer_get_dataview_data(value);
    if (!dv || !dv->buffer || dv->buffer->is_detached) { *out = NULL; *len = 0; return true; }
    *out = dv->buffer->data + dv->byte_offset;
    *len = dv->byte_length;
    return true;
  }

  return false;
}

static bool typedarray_read_value(ant_t *js, const TypedArrayData *ta_data, size_t index, ant_value_t *out) {
  if (!out || !ta_data || !ta_data->buffer || ta_data->buffer->is_detached || index >= ta_data->length) {
    return false;
  }

  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8:          *out = js_mknum((double)((int8_t *)data)[index]); return true;
    case TYPED_ARRAY_UINT8:
    case TYPED_ARRAY_UINT8_CLAMPED: *out = js_mknum((double)data[index]); return true;
    case TYPED_ARRAY_INT16:         *out = js_mknum((double)((int16_t *)data)[index]); return true;
    case TYPED_ARRAY_UINT16:        *out = js_mknum((double)((uint16_t *)data)[index]); return true;
    case TYPED_ARRAY_INT32:         *out = js_mknum((double)((int32_t *)data)[index]); return true;
    case TYPED_ARRAY_UINT32:        *out = js_mknum((double)((uint32_t *)data)[index]); return true;
    case TYPED_ARRAY_FLOAT16:       *out = js_mknum(half_to_double(((uint16_t *)data)[index])); return true;
    case TYPED_ARRAY_FLOAT32:       *out = js_mknum((double)((float *)data)[index]); return true;
    case TYPED_ARRAY_FLOAT64:       *out = js_mknum(((double *)data)[index]); return true;
    case TYPED_ARRAY_BIGINT64:      *out = bigint_from_int64(js, ((int64_t *)data)[index]); return !is_err(*out);
    case TYPED_ARRAY_BIGUINT64:     *out = bigint_from_uint64(js, ((uint64_t *)data)[index]); return !is_err(*out);
    default: return false;
  }
}

static bool advance_typedarray(ant_t *js, js_iter_t *it, ant_value_t *out) {
  ant_value_t iter = it->iterator;
  ant_value_t ta_obj = js_get_slot(iter, SLOT_DATA);
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  
  uint32_t kind = ITER_STATE_KIND(state);
  uint32_t idx  = ITER_STATE_INDEX(state);

  TypedArrayData *ta = buffer_get_typedarray_data(ta_obj);
  if (!ta || !ta->buffer || ta->buffer->is_detached || idx >= (uint32_t)ta->length) return false;
  
  ant_value_t value;
  if (!typedarray_read_value(js, ta, idx, &value)) return false;

  switch (kind) {
  case ARR_ITER_KEYS:
    *out = js_mknum((double)idx);
    break;
  case ARR_ITER_ENTRIES: {
    ant_value_t pair = js_mkarr(js);
    js_arr_push(js, pair, js_mknum((double)idx));
    js_arr_push(js, pair, value);
    *out = pair;
    break;
  }
  default:
    *out = value;
    break;
  }

  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, idx + 1)));
  return true;
}

static ant_value_t ta_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  js_iter_t it = { .iterator = js->this_val };
  ant_value_t value;
  return js_iter_result(js, advance_typedarray(js, &it, &value), value);
}

static ant_value_t ta_values(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t iter = js_mkobj(js);
  js_set_slot_wb(js, iter, SLOT_DATA, js->this_val);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(ARR_ITER_VALUES, 0)));
  js_set_proto_init(iter, g_typedarray_iter_proto);
  return iter;
}

static ant_value_t ta_keys(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t iter = js_mkobj(js);
  js_set_slot_wb(js, iter, SLOT_DATA, js->this_val);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(ARR_ITER_KEYS, 0)));
  js_set_proto_init(iter, g_typedarray_iter_proto);
  return iter;
}

static ant_value_t ta_entries(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t iter = js_mkobj(js);
  js_set_slot_wb(js, iter, SLOT_DATA, js->this_val);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(ARR_ITER_ENTRIES, 0)));
  js_set_proto_init(iter, g_typedarray_iter_proto);
  return iter;
}

static void register_buffer(ArrayBufferData *data) {
  if (!data) return;
  
  if (!buffer_registry) {
    buffer_registry = calloc(BUFFER_REGISTRY_INITIAL_CAP, sizeof(ArrayBufferData *));
    if (!buffer_registry) return;
    buffer_registry_cap = BUFFER_REGISTRY_INITIAL_CAP;
  }
  
  if (buffer_registry_count >= buffer_registry_cap) {
    size_t new_cap = buffer_registry_cap * 2;
    ArrayBufferData **new_reg = realloc(buffer_registry, new_cap * sizeof(ArrayBufferData *));
    if (!new_reg) return;
    buffer_registry = new_reg;
    buffer_registry_cap = new_cap;
  }
  
  buffer_registry[buffer_registry_count++] = data;
}

static void unregister_buffer(ArrayBufferData *data) {
  if (!data || !buffer_registry) return;
  
  for (size_t i = 0; i < buffer_registry_count; i++) {
  if (buffer_registry[i] == data) { 
    buffer_registry[i] = buffer_registry[--buffer_registry_count]; 
    return; 
  }}
}

static inline ssize_t normalize_index(ssize_t idx, ssize_t len) {
  if (idx < 0) idx += len;
  if (idx < 0) return 0;
  if (idx > len) return len;
  return idx;
}

ArrayBufferData *create_array_buffer_data(size_t length) {
  ArrayBufferData *data = ant_calloc(sizeof(ArrayBufferData) + length);
  if (!data) return NULL;
  
  data->data = (uint8_t *)(data + 1);
  memset(data->data, 0, length);
  
  data->length = length;
  data->capacity = length;
  data->ref_count = 1;
  data->is_shared = 0;
  data->is_detached = 0;
  
  register_buffer(data);
  return data;
}

static ArrayBufferData *create_shared_array_buffer_data(size_t length) {
  ArrayBufferData *data = create_array_buffer_data(length);
  if (data) data->is_shared = 1;
  return data;
}

void free_array_buffer_data(ArrayBufferData *data) {
  if (!data) return;
  data->ref_count--;
  if (data->ref_count <= 0) {
    unregister_buffer(data);
    free(data);
  }
}

static size_t get_element_size(TypedArrayType type) {
  static const void *dispatch[] = {
    &&L_1, &&L_1, &&L_1, &&L_2, &&L_2,
    &&L_4, &&L_4, &&L_2, &&L_4, &&L_8, &&L_8, &&L_8
  };
  
  if (type > TYPED_ARRAY_BIGUINT64) goto L_1;
  goto *dispatch[type];
  
  L_1: return 1;
  L_2: return 2;
  L_4: return 4;
  L_8: return 8;
}

const char *buffer_typedarray_type_name(TypedArrayType type) {
  static const char *const names[] = {
    "Int8Array",
    "Uint8Array",
    "Uint8ClampedArray",
    "Int16Array",
    "Uint16Array",
    "Int32Array",
    "Uint32Array",
    "Float16Array",
    "Float32Array",
    "Float64Array",
    "BigInt64Array",
    "BigUint64Array",
  };

  int i = (int)type;
  if (i < 0 || i >= (int)(sizeof(names) / sizeof(names[0]))) return "Uint8Array";
  return names[i];
}

static ant_value_t js_typedarray_toStringTag_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta = buffer_get_typedarray_data(this_val);
  if (!ta) return js_mkundef();

  const char *name = buffer_typedarray_type_name(ta->type);
  return js_mkstr(js, name, strlen(name));
}

static ant_value_t create_typed_array_like(
  ant_t *js,
  ant_value_t this_val,
  TypedArrayType type,
  ArrayBufferData *buffer,
  size_t byte_offset,
  size_t length
) {
  ant_value_t ab_obj = create_arraybuffer_obj(js, buffer);
  ant_value_t out = create_typed_array_with_buffer(
    js,type, buffer, byte_offset,
    length, buffer_typedarray_type_name(type), ab_obj
  );
  
  if (is_err(out)) return out;
  ant_value_t proto = js_get_proto(js, this_val);
  if (is_special_object(proto)) js_set_proto_init(out, proto);
  
  return out;
}

static ant_value_t js_arraybuffer_constructor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "ArrayBuffer constructor requires 'new'");
  }
  size_t length = 0;
  if (nargs > 0 && vtype(args[0]) == T_NUM) {
    length = (size_t)js_getnum(args[0]);
  }
  
  ArrayBufferData *data = create_array_buffer_data(length);
  if (!data) {
    return js_mkerr(js, "Failed to allocate ArrayBuffer");
  }
  
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, "ArrayBuffer", 11);

  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set(js, obj, "byteLength", js_mknum((double)length));
  js_set_finalizer(obj, arraybuffer_finalize);

  return obj;
}

// ArrayBuffer.prototype.slice(begin, end)
static ant_value_t js_arraybuffer_slice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ArrayBufferData *data = buffer_get_arraybuffer_data(this_val);
  if (!data) return js_mkerr(js, "Invalid ArrayBuffer");
  if (data->is_detached) return js_mkerr(js, "Cannot slice a detached ArrayBuffer");
  
  ssize_t len = (ssize_t)data->length;
  ssize_t begin = 0, end = len;
  if (nargs > 0 && vtype(args[0]) == T_NUM) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == T_NUM) end = (ssize_t)js_getnum(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  ArrayBufferData *new_data = create_array_buffer_data(new_length);
  if (!new_data) return js_mkerr(js, "Failed to allocate new ArrayBuffer");
  
  memcpy(new_data->data, data->data + begin, new_length);

  ant_value_t new_obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, "ArrayBuffer", 11);

  if (is_special_object(proto)) js_set_proto_init(new_obj, proto);
  js_set_native(new_obj, new_data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set(js, new_obj, "byteLength", js_mknum((double)new_length));
  js_set_finalizer(new_obj, arraybuffer_finalize);
  
  return new_obj;
}

// ArrayBuffer.prototype.transfer(newLength)
static ant_value_t js_arraybuffer_transfer(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ArrayBufferData *data = buffer_get_arraybuffer_data(this_val);
  if (!data) return js_mkerr(js, "Invalid ArrayBuffer");
  
  if (data->is_detached) {
    return js_mkerr(js, "Cannot transfer a detached ArrayBuffer");
  }
  
  if (data->is_shared) {
    return js_mkerr(js, "Cannot transfer a SharedArrayBuffer");
  }
  
  size_t new_length = data->length;
  if (nargs > 0 && vtype(args[0]) == T_NUM) {
    new_length = (size_t)js_getnum(args[0]);
  }
  
  ArrayBufferData *new_data = create_array_buffer_data(new_length);
  if (!new_data) return js_mkerr(js, "Failed to allocate new ArrayBuffer");
  
  size_t copy_length = data->length < new_length ? data->length : new_length;
  memcpy(new_data->data, data->data, copy_length);
  
  data->is_detached = 1;
  data->length = 0;
  js_set(js, this_val, "byteLength", js_mknum(0));

  ant_value_t new_obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, "ArrayBuffer", 11);

  if (is_special_object(proto)) js_set_proto_init(new_obj, proto);
  js_set_native(new_obj, new_data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set(js, new_obj, "byteLength", js_mknum((double)new_length));
  js_set_finalizer(new_obj, arraybuffer_finalize);
  
  return new_obj;
}

// ArrayBuffer.prototype.transferToFixedLength(newLength)
static ant_value_t js_arraybuffer_transferToFixedLength(ant_t *js, ant_value_t *args, int nargs) {
  return js_arraybuffer_transfer(js, args, nargs);
}

// ArrayBuffer.prototype.detached getter
static ant_value_t js_arraybuffer_detached_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ArrayBufferData *data = buffer_get_arraybuffer_data(this_val);
  if (!data) return js_false;
  
  return js_bool(data->is_detached);
}

static ant_value_t js_arraybuffer_byteLength_getter(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_val = js_getthis(js);
  ArrayBufferData *data = buffer_get_arraybuffer_data(this_val);
  if (!data || data->is_detached) return js_mknum(0);
  return js_mknum((double)data->length);
}

// ArrayBuffer.isView(value)
static ant_value_t js_arraybuffer_isView(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  return js_bool(buffer_is_dataview(args[0]) || buffer_get_typedarray_data(args[0]) != NULL);
}

static ant_value_t buffer_require_bigint_value(ant_t *js, ant_value_t value) {
  if (vtype(value) == T_BIGINT) return value;
  if (is_object_type(value)) {
    ant_value_t primitive = js_get_slot(value, SLOT_PRIMITIVE);
    if (vtype(primitive) == T_BIGINT) return primitive;
  }
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
}

static uint8_t typedarray_to_uint8_clamped(double value) {
  if (isnan(value) || value <= 0.0) return 0;
  if (value >= 255.0) return 255;

  double floor_value = floor(value);
  double fraction = value - floor_value;
  if (fraction > 0.5) return (uint8_t)(floor_value + 1.0);
  if (fraction < 0.5) return (uint8_t)floor_value;
  return ((uint32_t)floor_value & 1U) ? (uint8_t)(floor_value + 1.0) : (uint8_t)floor_value;
}

static ant_value_t typedarray_write_value(ant_t *js, TypedArrayData *ta_data, size_t index, ant_value_t value) {
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached || index >= ta_data->length) {
    return js_mkundef();
  }

  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8:          ((int8_t *)data)[index] = (int8_t)js_to_int32(js_to_number(js, value)); return js_mkundef();
    case TYPED_ARRAY_UINT8:         data[index] = (uint8_t)js_to_uint32(js_to_number(js, value)); return js_mkundef();
    case TYPED_ARRAY_UINT8_CLAMPED: data[index] = typedarray_to_uint8_clamped(js_to_number(js, value)); return js_mkundef();
    case TYPED_ARRAY_INT16:         ((int16_t *)data)[index] = (int16_t)js_to_int32(js_to_number(js, value)); return js_mkundef();
    case TYPED_ARRAY_UINT16:        ((uint16_t *)data)[index] = (uint16_t)js_to_uint32(js_to_number(js, value)); return js_mkundef();
    case TYPED_ARRAY_INT32:         ((int32_t *)data)[index] = js_to_int32(js_to_number(js, value)); return js_mkundef();
    case TYPED_ARRAY_UINT32:        ((uint32_t *)data)[index] = js_to_uint32(js_to_number(js, value)); return js_mkundef();
    case TYPED_ARRAY_FLOAT16:       ((uint16_t *)data)[index] = double_to_half(js_to_number(js, value)); return js_mkundef();
    case TYPED_ARRAY_FLOAT32:       ((float *)data)[index] = (float)js_to_number(js, value); return js_mkundef();
    case TYPED_ARRAY_FLOAT64:       ((double *)data)[index] = js_to_number(js, value); return js_mkundef();
    case TYPED_ARRAY_BIGINT64: {
      ant_value_t bigint = buffer_require_bigint_value(js, value);
      int64_t wrapped = 0;
      if (is_err(bigint)) return bigint;
      if (!bigint_to_int64_wrapping(js, bigint, &wrapped)) {
        return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
      }
      ((int64_t *)data)[index] = wrapped;
      return js_mkundef();
    }
    case TYPED_ARRAY_BIGUINT64: {
      ant_value_t bigint = buffer_require_bigint_value(js, value);
      uint64_t wrapped = 0;
      if (is_err(bigint)) return bigint;
      if (!bigint_to_uint64_wrapping(js, bigint, &wrapped)) {
        return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
      }
      ((uint64_t *)data)[index] = wrapped;
      return js_mkundef();
    }
    default:
      return js_mkundef();
  }
}

static ant_value_t typedarray_index_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  if (key_len == 0 || key_len > 10) return js_mkundef();
  
  size_t index = 0;
  for (size_t i = 0; i < key_len; i++) {
    char c = key[i];
    if (c < '0' || c > '9') return js_mkundef();
    index = index * 10 + (c - '0');
  }
  
  TypedArrayData *ta_data = buffer_get_typedarray_data(obj);
  if (!ta_data || index >= ta_data->length) return js_mkundef();
  if (!ta_data->buffer || ta_data->buffer->is_detached) return js_mkundef();

  ant_value_t value;
  if (!typedarray_read_value(js, ta_data, index, &value)) return js_mkundef();
  return value;
}

static bool typedarray_index_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value) {
  if (key_len == 0 || key_len > 10) return false;
  
  size_t index = 0;
  for (size_t i = 0; i < key_len; i++) {
    char c = key[i];
    if (c < '0' || c > '9') return false;
    index = index * 10 + (c - '0');
  }
  
  TypedArrayData *ta_data = buffer_get_typedarray_data(obj);
  if (!ta_data || index >= ta_data->length) return true;
  if (!ta_data->buffer || ta_data->buffer->is_detached) return true;

  return !is_err(typedarray_write_value(js, ta_data, index, value));
}

static bool typedarray_read_number(const TypedArrayData *ta_data, size_t index, double *out) {
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached || index >= ta_data->length) return false;
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;

  static const void *dispatch[] = {
    &&R_INT8, &&R_UINT8, &&R_UINT8, &&R_INT16, &&R_UINT16,
    &&R_INT32, &&R_UINT32, &&R_FLOAT16, &&R_FLOAT32, &&R_FLOAT64, &&R_FAIL, &&R_FAIL
  };

  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto R_FAIL;
  goto *dispatch[ta_data->type];

  R_INT8:    *out = (double)((int8_t *)data)[index];   return true;
  R_UINT8:   *out = (double)data[index];               return true;
  R_INT16:   *out = (double)((int16_t *)data)[index];  return true;
  R_UINT16:  *out = (double)((uint16_t *)data)[index]; return true;
  R_INT32:   *out = (double)((int32_t *)data)[index];  return true;
  R_UINT32:  *out = (double)((uint32_t *)data)[index]; return true;
  R_FLOAT16: *out = half_to_double(((uint16_t *)data)[index]); return true;
  R_FLOAT32: *out = (double)((float *)data)[index];    return true;
  R_FLOAT64: *out = ((double *)data)[index];           return true;
  R_FAIL:    return false;
}

static bool typedarray_write_number(TypedArrayData *ta_data, size_t index, double value) {
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached || index >= ta_data->length) return false;
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;

  static const void *dispatch[] = {
    &&W_INT8, &&W_UINT8, &&W_UINT8_CLAMPED, &&W_INT16, &&W_UINT16,
    &&W_INT32, &&W_UINT32, &&W_FLOAT16, &&W_FLOAT32, &&W_FLOAT64, &&W_FAIL, &&W_FAIL
  };

  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto W_FAIL;
  goto *dispatch[ta_data->type];

  W_INT8:    ((int8_t *)data)[index] = (int8_t)js_to_int32(value);     return true;
  W_UINT8:   data[index] = (uint8_t)js_to_uint32(value);               return true;
  W_UINT8_CLAMPED: data[index] = typedarray_to_uint8_clamped(value);   return true;
  W_INT16:   ((int16_t *)data)[index] = (int16_t)js_to_int32(value);   return true;
  W_UINT16:  ((uint16_t *)data)[index] = (uint16_t)js_to_uint32(value); return true;
  W_INT32:   ((int32_t *)data)[index] = js_to_int32(value);            return true;
  W_UINT32:  ((uint32_t *)data)[index] = js_to_uint32(value);          return true;
  W_FLOAT16: ((uint16_t *)data)[index] = double_to_half(value); return true;
  W_FLOAT32: ((float *)data)[index] = (float)value;       return true;
  W_FLOAT64: ((double *)data)[index] = value;             return true;
  W_FAIL:    return false;
}

static ant_value_t js_typedarray_every(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.every requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ant_value_t callback = args[0];
  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();

  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, i, &value)) return js_false;

    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
    if (!js_truthy(js, result)) return js_false;
  }

  return js_true;
}

static ant_value_t js_typedarray_forEach(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.forEach requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, i, &value)) return js_mkundef();
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
  }

  return js_mkundef();
}

static ant_value_t js_typedarray_some(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.some requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, i, &value)) return js_false;
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return js_true;
  }

  return js_false;
}

static ant_value_t js_typedarray_find(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.find requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, i, &value)) return js_mkundef();
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return value;
  }

  return js_mkundef();
}

static ant_value_t js_typedarray_findIndex(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.findIndex requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, i, &value)) return js_mknum(-1);
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return js_mknum((double)i);
  }

  return js_mknum(-1);
}

static ant_value_t js_typedarray_map(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.map requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ArrayBufferData *buffer = create_array_buffer_data(ta_data->byte_length);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  ant_value_t out = create_typed_array_like(js, this_val, ta_data->type, buffer, 0, ta_data->length);
  if (is_err(out)) return out;
  TypedArrayData *out_ta = buffer_get_typedarray_data(out);

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, i, &value)) return out;
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t mapped = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, false);
    if (is_err(mapped)) return mapped;
    ant_value_t write_result = typedarray_write_value(js, out_ta, i, mapped);
    if (is_err(write_result)) return write_result;
  }

  return out;
}

static ant_value_t js_typedarray_filter(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.filter requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ant_value_t *kept = NULL;
  if (ta_data->length > 0) {
    kept = malloc(sizeof(ant_value_t) * ta_data->length);
    if (!kept) return js_mkerr(js, "oom");
  }

  size_t count = 0;
  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, i, &value)) continue;
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, false);
    if (is_err(result)) { free(kept); return result; }
    if (js_truthy(js, result)) kept[count++] = value;
  }

  ArrayBufferData *buffer = create_array_buffer_data(count * get_element_size(ta_data->type));
  if (!buffer) { free(kept); return js_mkerr(js, "Failed to allocate buffer"); }
  ant_value_t out = create_typed_array_like(js, this_val, ta_data->type, buffer, 0, count);
  if (is_err(out)) { free(kept); return out; }
  TypedArrayData *out_ta = buffer_get_typedarray_data(out);

  for (size_t i = 0; i < count; i++) {
    ant_value_t write_result = typedarray_write_value(js, out_ta, i, kept[i]);
    if (is_err(write_result)) { free(kept); return write_result; }
  }

  free(kept);
  return out;
}

static ant_value_t js_typedarray_reduce_common(ant_t *js, ant_value_t *args, int nargs, bool right) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.reduce requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");
  if (ta_data->length == 0 && nargs < 2)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Reduce of empty TypedArray with no initial value");

  size_t i = right ? ta_data->length : 0;
  ant_value_t acc = nargs > 1 ? args[1] : js_mkundef();
  if (nargs < 2) {
    if (right) i--;
    if (!typedarray_read_value(js, ta_data, i, &acc)) return js_mkundef();
    if (!right) i++;
  }

  while (right ? (i > 0) : (i < ta_data->length)) {
    if (right) i--;
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, i, &value)) return acc;
    ant_value_t call_args[4] = { acc, value, js_mknum((double)i), this_val };
    acc = sv_vm_call(js->vm, js, args[0], js_mkundef(), call_args, 4, NULL, false);
    if (is_err(acc)) return acc;
    if (!right) i++;
  }

  return acc;
}

static ant_value_t js_typedarray_reduce(ant_t *js, ant_value_t *args, int nargs) {
  return js_typedarray_reduce_common(js, args, nargs, false);
}

static ant_value_t js_typedarray_reduceRight(ant_t *js, ant_value_t *args, int nargs) {
  return js_typedarray_reduce_common(js, args, nargs, true);
}

ant_value_t create_arraybuffer_obj(ant_t *js, ArrayBufferData *buffer) {
  ant_value_t ab_obj = js_mkobj(js);
  ant_value_t ab_proto = js_get_ctor_proto(js, "ArrayBuffer", 11);
  if (is_special_object(ab_proto)) js_set_proto_init(ab_obj, ab_proto);
  
  js_set_native(ab_obj, buffer, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set(js, ab_obj, "byteLength", js_mknum((double)buffer->length));
  js_set_finalizer(ab_obj, arraybuffer_finalize);
  buffer->ref_count++;
  
  return ab_obj;
}

ant_value_t create_typed_array_with_buffer(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, const char *type_name, ant_value_t arraybuffer_obj
) {
  TypedArrayData *ta_data = ta_meta_alloc(sizeof(TypedArrayData));
  if (!ta_data) return js_mkerr(js, "Failed to allocate TypedArray");
  
  size_t element_size = get_element_size(type);
  ta_data->buffer = buffer;
  ta_data->type = type;
  ta_data->byte_offset = byte_offset;
  ta_data->byte_length = length * element_size;
  ta_data->length = length;
  buffer->ref_count++;
  
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, type_name, strlen(type_name));
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  
  js_set_native(obj, ta_data, BUFFER_TYPEDARRAY_NATIVE_TAG);
  js_set(js, obj, "length", js_mknum((double)length));
  js_set(js, obj, "byteLength", js_mknum((double)(length * element_size)));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset));
  js_set(js, obj, "BYTES_PER_ELEMENT", js_mknum((double)element_size));
  js_set(js, obj, "buffer", arraybuffer_obj);
  
  js_set_getter(obj, typedarray_index_getter);
  js_set_setter(obj, typedarray_index_setter);
  js_set_finalizer(obj, typedarray_finalize);
  
  return obj;
}

ant_value_t create_typed_array(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, const char *type_name
) {
  ant_value_t ab_obj = create_arraybuffer_obj(js, buffer);
  ant_value_t result = create_typed_array_with_buffer(js, type, buffer, byte_offset, length, type_name, ab_obj);
  free_array_buffer_data(buffer); return result;
}

ant_value_t create_dataview_with_buffer(
  ant_t *js, ArrayBufferData *buffer,
  size_t byte_offset, size_t byte_length,
  ant_value_t arraybuffer_obj
) {
  DataViewData *dv_data = ta_meta_alloc(sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");

  dv_data->buffer = buffer;
  dv_data->byte_offset = byte_offset;
  dv_data->byte_length = byte_length;
  buffer->ref_count++;

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, "DataView", 8);
  if (is_special_object(proto)) js_set_proto_init(obj, proto);

  js_set_native(obj, dv_data, BUFFER_DATAVIEW_NATIVE_TAG);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_DATAVIEW));
  js_mkprop_fast(js, obj, "buffer", 6, arraybuffer_obj);
  js_set_descriptor(js, obj, "buffer", 6, 0);
  js_set(js, obj, "byteLength", js_mknum((double)byte_length));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset));
  js_set_finalizer(obj, dataview_finalize);

  return obj;
}

typedef struct {
  ant_value_t *values;
  size_t length;
  size_t capacity;
} iter_collect_ctx_t;

static bool iter_collect_callback(ant_t *js, ant_value_t value, void *udata) {
  iter_collect_ctx_t *ctx = (iter_collect_ctx_t *)udata;
  if (ctx->length >= ctx->capacity) {
    ctx->capacity *= 2;
    ant_value_t *new_values = realloc(ctx->values, ctx->capacity * sizeof(ant_value_t));
    if (!new_values) return false;
    ctx->values = new_values;
  }
  ctx->values[ctx->length++] = value;
  return true;
}

static ant_value_t js_typedarray_constructor(ant_t *js, ant_value_t *args, int nargs, TypedArrayType type, const char *type_name) {
  if (nargs == 0) {
    ArrayBufferData *buffer = create_array_buffer_data(0);
    return create_typed_array(js, type, buffer, 0, 0, type_name);
  }
  
  if (vtype(args[0]) == T_NUM) {
    size_t length = (size_t)js_getnum(args[0]);
    size_t element_size = get_element_size(type);
    ArrayBufferData *buffer = create_array_buffer_data(length * element_size);
    if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
    return create_typed_array(js, type, buffer, 0, length, type_name);
  }
  
  ArrayBufferData *arraybuffer = buffer_get_arraybuffer_data(args[0]);
  if (arraybuffer) {
    ArrayBufferData *buffer = arraybuffer;
    size_t byte_offset = 0;
    size_t length = buffer->length;
    
    if (nargs > 1 && vtype(args[1]) == T_NUM) {
      byte_offset = (size_t)js_getnum(args[1]);
    }
    
    size_t element_size = get_element_size(type);
    
    if (byte_offset > buffer->length) {
      return js_mkerr(js, "Start offset is outside the bounds of the buffer");
    }
    
    if (nargs > 2 && vtype(args[2]) == T_NUM) {
      length = (size_t)js_getnum(args[2]);
      size_t available = buffer->length - byte_offset;
      if (length > available / element_size) {
        return js_mkerr(js, "Invalid TypedArray length");
      }
    } else length = (buffer->length - byte_offset) / element_size;
    
    return create_typed_array_with_buffer(js, type, buffer, byte_offset, length, type_name, args[0]);
  }
  
  if (is_special_object(args[0])) {
    ant_value_t len_val = js_get(js, args[0], "length");
    size_t length = 0; ant_value_t *values = NULL;
    bool is_iterable = false;
    
    if (vtype(len_val) == T_NUM) length = (size_t)js_getnum(len_val); else {
      iter_collect_ctx_t ctx = { .values = NULL, .length = 0, .capacity = 16 };
      ctx.values = malloc(ctx.capacity * sizeof(ant_value_t));
      if (!ctx.values) return js_mkerr(js, "Failed to allocate memory");
      is_iterable = js_iter(js, args[0], iter_collect_callback, &ctx);
      
      if (is_iterable) {
        values = ctx.values;
        length = ctx.length;
      } else free(ctx.values);
    }
    
    if (length > 0 || is_iterable || vtype(len_val) == T_NUM) {
      size_t element_size = get_element_size(type);
      ArrayBufferData *buffer = create_array_buffer_data(length * element_size);
      if (!buffer) { if (values) free(values); return js_mkerr(js, "Failed to allocate buffer"); }
      
      ant_value_t result = create_typed_array(js, type, buffer, 0, length, type_name);
      if (is_err(result)) { if (values) free(values); return result; }
      TypedArrayData *result_ta = buffer_get_typedarray_data(result);
      
      for (size_t i = 0; i < length; i++) {
        ant_value_t elem;
        if (values) elem = values[i]; else {
          char idx_str[16];
          snprintf(idx_str, sizeof(idx_str), "%zu", i);
          elem = js_get(js, args[0], idx_str);
        }
        ant_value_t write_result = typedarray_write_value(js, result_ta, i, elem);
        if (is_err(write_result)) {
          if (values) free(values);
          return write_result;
        }
      }
      if (values) free(values);
      return result;
    }
  }
  
  return js_mkerr(js, "Invalid TypedArray constructor arguments");
}

static ant_value_t js_typedarray_base_constructor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray constructor is abstract");
}

// TypedArray.prototype.slice(begin, end)
// TypedArray.prototype.at(index)
static ant_value_t js_typedarray_at(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  if (nargs == 0 || vtype(args[0]) != T_NUM) return js_mkundef();
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t idx = (ssize_t)js_getnum(args[0]);
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) return js_mkundef();
  if (!ta_data->buffer || ta_data->buffer->is_detached) return js_mkundef();
  
  ant_value_t value;
  if (!typedarray_read_value(js, ta_data, (size_t)idx, &value)) return js_mkundef();
  return value;
}

static ant_value_t js_typedarray_slice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t begin = 0, end = len;
  
  if (nargs > 0 && vtype(args[0]) == T_NUM) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == T_NUM) end = (ssize_t)js_getnum(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(new_length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(
    new_buffer->data, 
    ta_data->buffer->data + ta_data->byte_offset + (size_t)begin * element_size,
    new_length * element_size
  );
  
  ant_value_t out = create_typed_array_like(
    js, this_val, ta_data->type, 
    new_buffer, 0, new_length
  ); free_array_buffer_data(new_buffer);
  
  return out;
}

// TypedArray.prototype.subarray(begin, end)
static ant_value_t js_typedarray_subarray(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t begin = 0, end = len;
  
  if (nargs > 0 && vtype(args[0]) == T_NUM) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == T_NUM) end = (ssize_t)js_getnum(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  size_t element_size = get_element_size(ta_data->type);
  size_t new_offset = ta_data->byte_offset + (size_t)begin * element_size;
  
  return create_typed_array_like(
    js, this_val, ta_data->type, 
    ta_data->buffer, new_offset, new_length
  );
}

// TypedArray.prototype.fill(value, start, end)
static ant_value_t js_typedarray_fill(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  ant_value_t value = nargs > 0 ? args[0] : js_mknum(0);
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t start = 0, end = len;
  
  if (nargs > 1 && vtype(args[1]) == T_NUM) start = (ssize_t)js_getnum(args[1]);
  if (nargs > 2 && vtype(args[2]) == T_NUM) end = (ssize_t)js_getnum(args[2]);
  
  start = normalize_index(start, len);
  end = normalize_index(end, len);
  if (end < start) end = start;

  if (ta_data->type == TYPED_ARRAY_BIGINT64 || ta_data->type == TYPED_ARRAY_BIGUINT64) {
    for (ssize_t i = start; i < end; i++) {
      ant_value_t write_result = typedarray_write_value(js, ta_data, (size_t)i, value);
      if (is_err(write_result)) return write_result;
    }
    return this_val;
  }
  
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  double num_value = js_to_number(js, value);
  
  static const void *dispatch[] = {
    &&L_INT8, &&L_UINT8, &&L_UINT8_CLAMPED, &&L_INT16, &&L_UINT16,
    &&L_INT32, &&L_UINT32, &&L_FLOAT16, &&L_FLOAT32, &&L_FLOAT64, &&L_DONE, &&L_DONE
  };
  
  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto L_DONE;
  goto *dispatch[ta_data->type];
  
  L_INT8:
    for (ssize_t i = start; i < end; i++) ((int8_t*)data)[i] = (int8_t)js_to_int32(num_value);
    goto L_DONE;
  L_UINT8:
    for (ssize_t i = start; i < end; i++) data[i] = (uint8_t)js_to_uint32(num_value);
    goto L_DONE;
  L_UINT8_CLAMPED:
    for (ssize_t i = start; i < end; i++) data[i] = typedarray_to_uint8_clamped(num_value);
    goto L_DONE;
  L_INT16:
    for (ssize_t i = start; i < end; i++) ((int16_t*)data)[i] = (int16_t)js_to_int32(num_value);
    goto L_DONE;
  L_UINT16:
    for (ssize_t i = start; i < end; i++) ((uint16_t*)data)[i] = (uint16_t)js_to_uint32(num_value);
    goto L_DONE;
  L_INT32:
    for (ssize_t i = start; i < end; i++) ((int32_t*)data)[i] = js_to_int32(num_value);
    goto L_DONE;
  L_UINT32:
    for (ssize_t i = start; i < end; i++) ((uint32_t*)data)[i] = js_to_uint32(num_value);
    goto L_DONE;
  L_FLOAT16:
    for (ssize_t i = start; i < end; i++) ((uint16_t*)data)[i] = double_to_half(num_value);
    goto L_DONE;
  L_FLOAT32:
    for (ssize_t i = start; i < end; i++) ((float*)data)[i] = (float)num_value;
    goto L_DONE;
  L_FLOAT64:
    for (ssize_t i = start; i < end; i++) ((double*)data)[i] = num_value;
    goto L_DONE;
  L_DONE:
    return this_val;
}

// TypedArray.prototype.set(source, offset = 0)
static ant_value_t js_typedarray_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "set requires source argument");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *dst = buffer_get_typedarray_data(this_val);
  if (!dst) return js_mkerr(js, "Invalid TypedArray");
  if (!dst->buffer || dst->buffer->is_detached) return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ssize_t offset_i = 0;
  if (nargs > 1 && vtype(args[1]) == T_NUM) offset_i = (ssize_t)js_getnum(args[1]);
  if (offset_i < 0) return js_mkerr(js, "Offset out of bounds");
  size_t offset = (size_t)offset_i;
  if (offset > dst->length) return js_mkerr(js, "Offset out of bounds");

  ant_value_t src_val = args[0];
  TypedArrayData *src_ta = buffer_get_typedarray_data(src_val);

  if (src_ta && src_ta->buffer && !src_ta->buffer->is_detached) {
    size_t src_len = src_ta->length;
    if (offset + src_len > dst->length) return js_mkerr(js, "Source is too large");

    if (src_ta->type == dst->type) {
      size_t el = get_element_size(dst->type);
      uint8_t *dst_data = dst->buffer->data + dst->byte_offset + offset * el;
      uint8_t *src_data = src_ta->buffer->data + src_ta->byte_offset;
      memmove(dst_data, src_data, src_len * el);
      return js_mkundef();
    }

    for (size_t i = 0; i < src_len; i++) {
      ant_value_t value = js_mkundef();
      if (!typedarray_read_value(js, src_ta, i, &value)) value = js_mknum(0);
      ant_value_t write_result = typedarray_write_value(js, dst, offset + i, value);
      if (is_err(write_result)) return write_result;
    }
    return js_mkundef();
  }

  if (!is_special_object(src_val) && vtype(src_val) != T_STR) {
    return js_mkerr(js, "set source must be array-like or TypedArray");
  }

  size_t src_len = 0;
  if (vtype(src_val) == T_STR) {
    src_len = (size_t)vstrlen(js, src_val);
  } else {
    ant_value_t len_val = js_get(js, src_val, "length");
    src_len = vtype(len_val) == T_NUM ? (size_t)js_getnum(len_val) : 0;
  }

  if (offset + src_len > dst->length) return js_mkerr(js, "Source is too large");

  for (size_t i = 0; i < src_len; i++) {
    if (vtype(src_val) == T_STR) {
      ant_offset_t slen = 0;
      ant_offset_t soff = vstr(js, src_val, &slen);
      const unsigned char *sptr = (const unsigned char *)(uintptr_t)soff;
      double value = 0;
      if (i < (size_t)slen) value = sptr[i];
      ant_value_t write_result = typedarray_write_value(js, dst, offset + i, js_mknum(value));
      if (is_err(write_result)) return write_result;
    } else {
      char idx[24];
      size_t idx_len = uint_to_str(idx, sizeof(idx), (uint64_t)i);
      idx[idx_len] = '\0';
      ant_value_t elem = js_get(js, src_val, idx);
      ant_value_t write_result = typedarray_write_value(js, dst, offset + i, elem);
      if (is_err(write_result)) return write_result;
    }
  }

  return js_mkundef();
}

// TypedArray.prototype.copyWithin(target, start, end)
static ant_value_t js_typedarray_copyWithin(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta = buffer_get_typedarray_data(this_val);
  if (!ta) return js_mkerr(js, "Invalid TypedArray");
  if (!ta->buffer || ta->buffer->is_detached) return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ssize_t len = (ssize_t)ta->length;
  if (len <= 0) return this_val;

  ssize_t target = 0, start = 0, end = len;
  if (nargs > 0 && vtype(args[0]) == T_NUM) target = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == T_NUM) start = (ssize_t)js_getnum(args[1]);
  if (nargs > 2 && vtype(args[2]) == T_NUM) end = (ssize_t)js_getnum(args[2]);

  target = normalize_index(target, len);
  start = normalize_index(start, len);
  end = normalize_index(end, len);
  if (end < start) end = start;
  if (target >= len || start >= len || end <= start) return this_val;

  size_t count = (size_t)(end - start);
  size_t max_to_end = (size_t)(len - target);
  if (count > max_to_end) count = max_to_end;
  if (count == 0) return this_val;

  size_t el = get_element_size(ta->type);
  uint8_t *base = ta->buffer->data + ta->byte_offset;
  memmove(base + (size_t)target * el, base + (size_t)start * el, count * el);
  return this_val;
}

// TypedArray.prototype.toReversed()
static ant_value_t js_typedarray_toReversed(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  size_t length = ta_data->length;
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  uint8_t *src = ta_data->buffer->data + ta_data->byte_offset;
  uint8_t *dst = new_buffer->data;
  
  for (size_t i = 0; i < length; i++) {
    memcpy(dst + i * element_size, src + (length - 1 - i) * element_size, element_size);
  }
  
  ant_value_t out = create_typed_array_like(
    js, this_val, ta_data->type,
    new_buffer, 0, length
  ); free_array_buffer_data(new_buffer);
  
  return out;
}

// TypedArray.prototype.toSorted(comparefn)
static ant_value_t js_typedarray_toSorted(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  size_t length = ta_data->length;
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(new_buffer->data, ta_data->buffer->data + ta_data->byte_offset, length * element_size);
  ant_value_t result = create_typed_array_like(js, this_val, ta_data->type, new_buffer, 0, length);
  
  free_array_buffer_data(new_buffer);
  if (is_err(result)) return result;
  
  TypedArrayData *result_ta = buffer_get_typedarray_data(result);
  uint8_t *data = result_ta->buffer->data;
  
  ant_value_t comparefn = (nargs > 0 && vtype(args[0]) == T_FUNC) ? args[0] : js_mkundef();
  bool has_comparefn = vtype(comparefn) == T_FUNC;
  
  for (size_t i = 1; i < length; i++) {
    for (size_t j = i; j > 0; j--) {
      double a_val, b_val;
      int cmp;
      
      static const void *read_dispatch[] = {
        &&R_INT8, &&R_UINT8, &&R_UINT8, &&R_INT16, &&R_UINT16,
        &&R_INT32, &&R_UINT32, &&R_FLOAT16, &&R_FLOAT32, &&R_FLOAT64, &&R_DONE, &&R_DONE
      };
      
      if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto R_DONE;
      goto *read_dispatch[ta_data->type];
      
      R_INT8:    a_val = (double)((int8_t*)data)[j-1]; b_val = (double)((int8_t*)data)[j]; goto R_COMPARE;
      R_UINT8:   a_val = (double)data[j-1]; b_val = (double)data[j]; goto R_COMPARE;
      R_INT16:   a_val = (double)((int16_t*)data)[j-1]; b_val = (double)((int16_t*)data)[j]; goto R_COMPARE;
      R_UINT16:  a_val = (double)((uint16_t*)data)[j-1]; b_val = (double)((uint16_t*)data)[j]; goto R_COMPARE;
      R_INT32:   a_val = (double)((int32_t*)data)[j-1]; b_val = (double)((int32_t*)data)[j]; goto R_COMPARE;
      R_UINT32:  a_val = (double)((uint32_t*)data)[j-1]; b_val = (double)((uint32_t*)data)[j]; goto R_COMPARE;
      R_FLOAT16: a_val = half_to_double(((uint16_t*)data)[j-1]); b_val = half_to_double(((uint16_t*)data)[j]); goto R_COMPARE;
      R_FLOAT32: a_val = (double)((float*)data)[j-1]; b_val = (double)((float*)data)[j]; goto R_COMPARE;
      R_FLOAT64: a_val = ((double*)data)[j-1]; b_val = ((double*)data)[j]; goto R_COMPARE;
      R_DONE:    goto SORT_DONE;
      
      R_COMPARE:
      if (has_comparefn) {
        ant_value_t cmp_args[2] = { js_mknum(a_val), js_mknum(b_val) };
        ant_value_t cmp_result = sv_vm_call(js->vm, js, comparefn, js_mkundef(), cmp_args, 2, NULL, false);
        cmp = (int)js_getnum(cmp_result);
      } else {
        cmp = (a_val > b_val) ? 1 : ((a_val < b_val) ? -1 : 0);
      }
      
      if (cmp <= 0) break;
      
      static const void *swap_dispatch[] = {
        &&S_INT8, &&S_UINT8, &&S_UINT8, &&S_INT16, &&S_UINT16,
        &&S_INT32, &&S_UINT32, &&S_FLOAT16, &&S_FLOAT32, &&S_FLOAT64, &&S_DONE, &&S_DONE
      };
      
      if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto S_DONE;
      goto *swap_dispatch[ta_data->type];
      
      S_INT8:    { int8_t tmp = ((int8_t*)data)[j-1]; ((int8_t*)data)[j-1] = ((int8_t*)data)[j]; ((int8_t*)data)[j] = tmp; goto S_DONE; }
      S_UINT8:   { uint8_t tmp = data[j-1]; data[j-1] = data[j]; data[j] = tmp; goto S_DONE; }
      S_INT16:   { int16_t tmp = ((int16_t*)data)[j-1]; ((int16_t*)data)[j-1] = ((int16_t*)data)[j]; ((int16_t*)data)[j] = tmp; goto S_DONE; }
      S_UINT16:  { uint16_t tmp = ((uint16_t*)data)[j-1]; ((uint16_t*)data)[j-1] = ((uint16_t*)data)[j]; ((uint16_t*)data)[j] = tmp; goto S_DONE; }
      S_INT32:   { int32_t tmp = ((int32_t*)data)[j-1]; ((int32_t*)data)[j-1] = ((int32_t*)data)[j]; ((int32_t*)data)[j] = tmp; goto S_DONE; }
      S_UINT32:  { uint32_t tmp = ((uint32_t*)data)[j-1]; ((uint32_t*)data)[j-1] = ((uint32_t*)data)[j]; ((uint32_t*)data)[j] = tmp; goto S_DONE; }
      S_FLOAT16: { uint16_t tmp = ((uint16_t*)data)[j-1]; ((uint16_t*)data)[j-1] = ((uint16_t*)data)[j]; ((uint16_t*)data)[j] = tmp; goto S_DONE; }
      S_FLOAT32: { float tmp = ((float*)data)[j-1]; ((float*)data)[j-1] = ((float*)data)[j]; ((float*)data)[j] = tmp; goto S_DONE; }
      S_FLOAT64: { double tmp = ((double*)data)[j-1]; ((double*)data)[j-1] = ((double*)data)[j]; ((double*)data)[j] = tmp; goto S_DONE; }
      S_DONE:;
    }
  }
  
  SORT_DONE:
  return result;
}

// TypedArray.prototype.with(index, value)
static ant_value_t js_typedarray_with(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "with requires index and value");
  
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  ssize_t index = (ssize_t)js_getnum(args[0]);
  size_t length = ta_data->length;
  
  if (index < 0) index = (ssize_t)length + index;
  if (index < 0 || (size_t)index >= length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(new_buffer->data, ta_data->buffer->data + ta_data->byte_offset, length * element_size);
  
  ant_value_t out = create_typed_array_like(
    js, this_val, ta_data->type,
    new_buffer, 0, length
  ); free_array_buffer_data(new_buffer);
  if (is_err(out)) return out;

  TypedArrayData *out_ta = buffer_get_typedarray_data(out);
  ant_value_t write_result = typedarray_write_value(js, out_ta, (size_t)index, args[1]);
  if (is_err(write_result)) return write_result;
  
  return out;
}

#define DEFINE_TYPEDARRAY_CONSTRUCTOR(name, type) \
  static ant_value_t js_##name##_constructor(ant_t *js, ant_value_t *args, int nargs) { \
    if (vtype(js->new_target) == T_UNDEF) return js_mkerr_typed(js, JS_ERR_TYPE, #name " constructor requires 'new'"); \
    return js_typedarray_constructor(js, args, nargs, type, #name); \
  }

DEFINE_TYPEDARRAY_CONSTRUCTOR(Int8Array, TYPED_ARRAY_INT8)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Uint8Array, TYPED_ARRAY_UINT8)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Uint8ClampedArray, TYPED_ARRAY_UINT8_CLAMPED)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Int16Array, TYPED_ARRAY_INT16)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Uint16Array, TYPED_ARRAY_UINT16)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Int32Array, TYPED_ARRAY_INT32)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Uint32Array, TYPED_ARRAY_UINT32)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Float16Array, TYPED_ARRAY_FLOAT16)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Float32Array, TYPED_ARRAY_FLOAT32)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Float64Array, TYPED_ARRAY_FLOAT64)
DEFINE_TYPEDARRAY_CONSTRUCTOR(BigInt64Array, TYPED_ARRAY_BIGINT64)
DEFINE_TYPEDARRAY_CONSTRUCTOR(BigUint64Array, TYPED_ARRAY_BIGUINT64)

static ant_value_t js_typedarray_from(ant_t *js, ant_value_t *args, int nargs, TypedArrayType type, const char *type_name) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "%s.from requires at least 1 argument", type_name);

  ant_value_t source = args[0];
  bool has_map = nargs >= 2 && vtype(args[1]) != T_UNDEF;
  
  ant_value_t map_fn = js_mkundef();
  ant_value_t this_arg = nargs >= 3 ? args[2] : js_mkundef();
  ant_value_t result = js_mkundef();
  ant_value_t *collected = NULL;
  
  gc_temp_root_scope_t temp_roots = {0};
  bool temp_roots_active = false;

  if (has_map) {
    if (!is_callable(args[1])) return js_mkerr_typed(
      js, JS_ERR_TYPE, "%s.from: mapFn is not a function", type_name
    );
    map_fn = args[1];
  }

  gc_temp_root_scope_begin(js, &temp_roots);
  temp_roots_active = true;
  
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, source))) goto oom;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, map_fn))) goto oom;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, this_arg))) goto oom;

  size_t count = 0, cap = 16;
  collected = malloc(cap * sizeof(ant_value_t));
  if (!collected) goto oom;

  js_iter_t it;
  if (js_iter_open(js, source, &it)) {
    ant_value_t item;
    while (js_iter_next(js, &it, &item)) {
      if (count >= cap) {
        cap *= 2;
        ant_value_t *tmp = realloc(collected, cap * sizeof(ant_value_t));
        if (!tmp) goto oom;
        collected = tmp;
      }
      if (has_map) {
        ant_value_t map_args[2] = { item, js_mknum((double)count) };
        item = sv_vm_call(js->vm, js, map_fn, this_arg, map_args, 2, NULL, false);
        if (is_err(item)) {
          result = item;
          goto done;
        }
      }
      collected[count++] = item;
      if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, item))) goto oom;
    }
    js_iter_close(js, &it);
  } else {
    ant_value_t len_val = js_get(js, source, "length");
    size_t len = vtype(len_val) == T_NUM ? (size_t)js_getnum(len_val) : 0;
    for (size_t i = 0; i < len; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%zu", i);
      ant_value_t item = js_get(js, source, idx);
      if (count >= cap) {
        cap *= 2;
        ant_value_t *tmp = realloc(collected, cap * sizeof(ant_value_t));
        if (!tmp) goto oom;
        collected = tmp;
      }
      if (has_map) {
        ant_value_t map_args[2] = { item, js_mknum((double)i) };
        item = sv_vm_call(js->vm, js, map_fn, this_arg, map_args, 2, NULL, false);
        if (is_err(item)) {
          result = item;
          goto done;
        }
      }
      collected[count++] = item;
      if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, item))) goto oom;
    }
  }

  size_t elem_size = get_element_size(type);
  ArrayBufferData *buffer = create_array_buffer_data(count * elem_size);
  if (!buffer) goto oom;

  result = create_typed_array(js, type, buffer, 0, count, type_name);
  if (is_err(result)) goto done;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, result))) goto oom;
  TypedArrayData *result_ta = buffer_get_typedarray_data(result);

  for (size_t i = 0; i < count; i++) {
    ant_value_t write_result = typedarray_write_value(js, result_ta, i, collected[i]);
    if (is_err(write_result)) {
      result = write_result;
      goto done;
    }
  }

done:
  if (temp_roots_active) gc_temp_root_scope_end(&temp_roots);
  free(collected);
  return result;

oom:
  result = js_mkerr(js, "oom");
  goto done;
}

#define DEFINE_TYPEDARRAY_FROM(name, type) \
  static ant_value_t js_##name##_from(ant_t *js, ant_value_t *args, int nargs) { \
    return js_typedarray_from(js, args, nargs, type, #name); \
  }

DEFINE_TYPEDARRAY_FROM(Int8Array, TYPED_ARRAY_INT8)
DEFINE_TYPEDARRAY_FROM(Uint8Array, TYPED_ARRAY_UINT8)
DEFINE_TYPEDARRAY_FROM(Uint8ClampedArray, TYPED_ARRAY_UINT8_CLAMPED)
DEFINE_TYPEDARRAY_FROM(Int16Array, TYPED_ARRAY_INT16)
DEFINE_TYPEDARRAY_FROM(Uint16Array, TYPED_ARRAY_UINT16)
DEFINE_TYPEDARRAY_FROM(Int32Array, TYPED_ARRAY_INT32)
DEFINE_TYPEDARRAY_FROM(Uint32Array, TYPED_ARRAY_UINT32)
DEFINE_TYPEDARRAY_FROM(Float16Array, TYPED_ARRAY_FLOAT16)
DEFINE_TYPEDARRAY_FROM(Float32Array, TYPED_ARRAY_FLOAT32)
DEFINE_TYPEDARRAY_FROM(Float64Array, TYPED_ARRAY_FLOAT64)
DEFINE_TYPEDARRAY_FROM(BigInt64Array, TYPED_ARRAY_BIGINT64)
DEFINE_TYPEDARRAY_FROM(BigUint64Array, TYPED_ARRAY_BIGUINT64)

static ant_value_t js_typedarray_of(ant_t *js, ant_value_t *args, int nargs, TypedArrayType type, const char *type_name) {
  size_t count = (size_t)nargs;
  size_t elem_size = get_element_size(type);
  ArrayBufferData *buffer = create_array_buffer_data(count * elem_size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");

  ant_value_t result = create_typed_array(js, type, buffer, 0, count, type_name);
  if (is_err(result)) return result;

  TypedArrayData *result_ta = buffer_get_typedarray_data(result);
  for (int i = 0; i < nargs; i++) {
    ant_value_t write_result = typedarray_write_value(js, result_ta, (size_t)i, args[i]);
    if (is_err(write_result)) return write_result;
  }

  return result;
}

#define DEFINE_TYPEDARRAY_OF(name, type) \
  static ant_value_t js_##name##_of(ant_t *js, ant_value_t *args, int nargs) { \
    return js_typedarray_of(js, args, nargs, type, #name); \
  }

DEFINE_TYPEDARRAY_OF(Int8Array, TYPED_ARRAY_INT8)
DEFINE_TYPEDARRAY_OF(Uint8Array, TYPED_ARRAY_UINT8)
DEFINE_TYPEDARRAY_OF(Uint8ClampedArray, TYPED_ARRAY_UINT8_CLAMPED)
DEFINE_TYPEDARRAY_OF(Int16Array, TYPED_ARRAY_INT16)
DEFINE_TYPEDARRAY_OF(Uint16Array, TYPED_ARRAY_UINT16)
DEFINE_TYPEDARRAY_OF(Int32Array, TYPED_ARRAY_INT32)
DEFINE_TYPEDARRAY_OF(Uint32Array, TYPED_ARRAY_UINT32)
DEFINE_TYPEDARRAY_OF(Float16Array, TYPED_ARRAY_FLOAT16)
DEFINE_TYPEDARRAY_OF(Float32Array, TYPED_ARRAY_FLOAT32)
DEFINE_TYPEDARRAY_OF(Float64Array, TYPED_ARRAY_FLOAT64)
DEFINE_TYPEDARRAY_OF(BigInt64Array, TYPED_ARRAY_BIGINT64)
DEFINE_TYPEDARRAY_OF(BigUint64Array, TYPED_ARRAY_BIGUINT64)

static ant_value_t js_dataview_constructor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "DataView constructor requires 'new'");
  }
  if (nargs < 1) {
    return js_mkerr(js, "DataView requires an ArrayBuffer");
  }
  
  ArrayBufferData *buffer = buffer_get_arraybuffer_data(args[0]);
  if (!buffer) {
    return js_mkerr(js, "First argument must be an ArrayBuffer");
  }
  size_t byte_offset = 0;
  size_t byte_length = buffer->length;
  
  if (nargs > 1 && vtype(args[1]) == T_NUM) {
    byte_offset = (size_t)js_getnum(args[1]);
  }
  
  if (byte_offset > buffer->length) {
    return js_mkerr(js, "Start offset is outside the bounds of the buffer");
  }
  
  if (nargs > 2 && vtype(args[2]) == T_NUM) {
    byte_length = (size_t)js_getnum(args[2]);
    if (byte_length > buffer->length - byte_offset) {
      return js_mkerr(js, "Invalid DataView length");
    }
  } else byte_length = buffer->length - byte_offset;
  
  DataViewData *dv_data = ta_meta_alloc(sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");
  
  dv_data->buffer = buffer;
  dv_data->byte_offset = byte_offset;
  dv_data->byte_length = byte_length;
  buffer->ref_count++;
  
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, "DataView", 8);
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  
  js_set_native(obj, dv_data, BUFFER_DATAVIEW_NATIVE_TAG);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_DATAVIEW));
  js_mkprop_fast(js, obj, "buffer", 6, args[0]);
  js_set_descriptor(js, obj, "buffer", 6, 0);

  js_set(js, obj, "byteLength", js_mknum((double)byte_length));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset)); 
  js_set_finalizer(obj, dataview_finalize);
  
  return obj;
}

// DataView.prototype.getUint8(byteOffset)
static ant_value_t js_dataview_getInt8(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getInt8 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  
  if (offset >= dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  int8_t value = (int8_t)dv->buffer->data[dv->byte_offset + offset];
  return js_mknum((double)value);
}

// DataView.prototype.setInt8(byteOffset, value)
static ant_value_t js_dataview_setInt8(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setInt8 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  int8_t value = (int8_t)js_to_int32(js_getnum(args[1]));
  
  if (offset >= dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  dv->buffer->data[dv->byte_offset + offset] = (uint8_t)value;
  return js_mkundef();
}

// DataView.prototype.getUint8(byteOffset)
static ant_value_t js_dataview_getUint8(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getUint8 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  
  if (offset >= dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t value = dv->buffer->data[dv->byte_offset + offset];
  return js_mknum((double)value);
}

// DataView.prototype.setUint8(byteOffset, value)
static ant_value_t js_dataview_setUint8(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setUint8 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  uint8_t value = (uint8_t)js_to_uint32(js_getnum(args[1]));
  
  if (offset >= dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  dv->buffer->data[dv->byte_offset + offset] = value;
  return js_mkundef();
}

// DataView.prototype.getInt16(byteOffset, littleEndian)
static ant_value_t js_dataview_getInt16(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getInt16 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  bool little_endian = (nargs > 1 && js_truthy(js, args[1]));
  
  if (offset + 2 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  int16_t value;
  
  if (little_endian) {
    value = (int16_t)(ptr[0] | (ptr[1] << 8));
  } else {
    value = (int16_t)((ptr[0] << 8) | ptr[1]);
  }
  
  return js_mknum((double)value);
}

// DataView.prototype.getUint16(byteOffset, littleEndian)
static ant_value_t js_dataview_getUint16(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getUint16 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  bool little_endian = (nargs > 1 && js_truthy(js, args[1]));
  
  if (offset + 2 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint16_t value;
  
  if (little_endian) value = (uint16_t)(ptr[0] | (ptr[1] << 8));
  else value = (uint16_t)((ptr[0] << 8) | ptr[1]);
  
  return js_mknum((double)value);
}

// DataView.prototype.setUint16(byteOffset, value, littleEndian)
static ant_value_t js_dataview_setUint16(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setUint16 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  uint16_t value = (uint16_t)js_to_uint32(js_getnum(args[1]));
  bool little_endian = (nargs > 2 && js_truthy(js, args[2]));
  
  if (offset + 2 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  
  if (little_endian) {
    ptr[0] = (uint8_t)(value & 0xFF);
    ptr[1] = (uint8_t)((value >> 8) & 0xFF);
  } else {
    ptr[0] = (uint8_t)((value >> 8) & 0xFF);
    ptr[1] = (uint8_t)(value & 0xFF);
  }
  
  return js_mkundef();
}

// DataView.prototype.getInt32(byteOffset, littleEndian)
static ant_value_t js_dataview_getInt32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getInt32 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  bool little_endian = (nargs > 1 && js_truthy(js, args[1]));
  
  if (offset + 4 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  int32_t value;
  
  if (little_endian) {
    value = (int32_t)(ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
  } else {
    value = (int32_t)((ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3]);
  }
  
  return js_mknum((double)value);
}

// DataView.prototype.getFloat32(byteOffset, littleEndian)
static ant_value_t js_dataview_getFloat32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getFloat32 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  bool little_endian = (nargs > 1 && js_truthy(js, args[1]));
  
  if (offset + 4 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint32_t bits;
  
  if (little_endian) {
    bits = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
  } else {
    bits = (ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3];
  }
  
  float value;
  memcpy(&value, &bits, 4);
  return js_mknum((double)value);
}

// DataView.prototype.setInt16(byteOffset, value, littleEndian)
static ant_value_t js_dataview_setInt16(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setInt16 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  int16_t value = (int16_t)js_to_int32(js_getnum(args[1]));
  bool little_endian = (nargs > 2 && js_truthy(js, args[2]));
  
  if (offset + 2 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  
  if (little_endian) {
    ptr[0] = (uint8_t)(value & 0xFF);
    ptr[1] = (uint8_t)((value >> 8) & 0xFF);
  } else {
    ptr[0] = (uint8_t)((value >> 8) & 0xFF);
    ptr[1] = (uint8_t)(value & 0xFF);
  }
  
  return js_mkundef();
}

// DataView.prototype.setInt32(byteOffset, value, littleEndian)
static ant_value_t js_dataview_setInt32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setInt32 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  int32_t value = js_to_int32(js_getnum(args[1]));
  bool little_endian = (nargs > 2 && js_truthy(js, args[2]));
  
  if (offset + 4 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  
  if (little_endian) {
    ptr[0] = (uint8_t)(value & 0xFF);
    ptr[1] = (uint8_t)((value >> 8) & 0xFF);
    ptr[2] = (uint8_t)((value >> 16) & 0xFF);
    ptr[3] = (uint8_t)((value >> 24) & 0xFF);
  } else {
    ptr[0] = (uint8_t)((value >> 24) & 0xFF);
    ptr[1] = (uint8_t)((value >> 16) & 0xFF);
    ptr[2] = (uint8_t)((value >> 8) & 0xFF);
    ptr[3] = (uint8_t)(value & 0xFF);
  }
  
  return js_mkundef();
}

// DataView.prototype.getUint32(byteOffset, littleEndian)
static ant_value_t js_dataview_getUint32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getUint32 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  bool little_endian = (nargs > 1 && js_truthy(js, args[1]));
  
  if (offset + 4 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint32_t value;
  
  if (little_endian) value = (uint32_t)(ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
  else value = (uint32_t)((ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3]);
  
  return js_mknum((double)value);
}

// DataView.prototype.setUint32(byteOffset, value, littleEndian)
static ant_value_t js_dataview_setUint32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setUint32 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  
  uint32_t value = js_to_uint32(js_getnum(args[1]));
  bool little_endian = (nargs > 2 && js_truthy(js, args[2]));
  
  if (offset + 4 > dv->byte_length) return js_mkerr(js, "Offset out of bounds");  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  
  if (little_endian) {
    ptr[0] = (uint8_t)(value & 0xFF);
    ptr[1] = (uint8_t)((value >> 8) & 0xFF);
    ptr[2] = (uint8_t)((value >> 16) & 0xFF);
    ptr[3] = (uint8_t)((value >> 24) & 0xFF);
  } else {
    ptr[0] = (uint8_t)((value >> 24) & 0xFF);
    ptr[1] = (uint8_t)((value >> 16) & 0xFF);
    ptr[2] = (uint8_t)((value >> 8) & 0xFF);
    ptr[3] = (uint8_t)(value & 0xFF);
  }
  
  return js_mkundef();
}

// DataView.prototype.setFloat32(byteOffset, value, littleEndian)
static ant_value_t js_dataview_setFloat32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setFloat32 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  float value = (float)js_getnum(args[1]);
  bool little_endian = (nargs > 2 && js_truthy(js, args[2]));
  
  if (offset + 4 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint32_t bits;
  memcpy(&bits, &value, 4);
  
  if (little_endian) {
    ptr[0] = (uint8_t)(bits & 0xFF);
    ptr[1] = (uint8_t)((bits >> 8) & 0xFF);
    ptr[2] = (uint8_t)((bits >> 16) & 0xFF);
    ptr[3] = (uint8_t)((bits >> 24) & 0xFF);
  } else {
    ptr[0] = (uint8_t)((bits >> 24) & 0xFF);
    ptr[1] = (uint8_t)((bits >> 16) & 0xFF);
    ptr[2] = (uint8_t)((bits >> 8) & 0xFF);
    ptr[3] = (uint8_t)(bits & 0xFF);
  }
  
  return js_mkundef();
}

// DataView.prototype.getFloat64(byteOffset, littleEndian)
static ant_value_t js_dataview_getFloat64(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getFloat64 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  bool little_endian = (nargs > 1 && js_truthy(js, args[1]));
  
  if (offset + 8 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint64_t bits;
  
  if (little_endian) {
    bits = (uint64_t)ptr[0] | ((uint64_t)ptr[1] << 8) | ((uint64_t)ptr[2] << 16) | ((uint64_t)ptr[3] << 24) |
           ((uint64_t)ptr[4] << 32) | ((uint64_t)ptr[5] << 40) | ((uint64_t)ptr[6] << 48) | ((uint64_t)ptr[7] << 56);
  } else {
    bits = ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
           ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | (uint64_t)ptr[7];
  }
  
  double value;
  memcpy(&value, &bits, 8);
  return js_mknum(value);
}

// DataView.prototype.setFloat64(byteOffset, value, littleEndian)
static ant_value_t js_dataview_setFloat64(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setFloat64 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  double value = js_getnum(args[1]);
  bool little_endian = (nargs > 2 && js_truthy(js, args[2]));
  
  if (offset + 8 > dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint64_t bits;
  memcpy(&bits, &value, 8);
  
  if (little_endian) {
    ptr[0] = (uint8_t)(bits & 0xFF);
    ptr[1] = (uint8_t)((bits >> 8) & 0xFF);
    ptr[2] = (uint8_t)((bits >> 16) & 0xFF);
    ptr[3] = (uint8_t)((bits >> 24) & 0xFF);
    ptr[4] = (uint8_t)((bits >> 32) & 0xFF);
    ptr[5] = (uint8_t)((bits >> 40) & 0xFF);
    ptr[6] = (uint8_t)((bits >> 48) & 0xFF);
    ptr[7] = (uint8_t)((bits >> 56) & 0xFF);
  } else {
    ptr[0] = (uint8_t)((bits >> 56) & 0xFF);
    ptr[1] = (uint8_t)((bits >> 48) & 0xFF);
    ptr[2] = (uint8_t)((bits >> 40) & 0xFF);
    ptr[3] = (uint8_t)((bits >> 32) & 0xFF);
    ptr[4] = (uint8_t)((bits >> 24) & 0xFF);
    ptr[5] = (uint8_t)((bits >> 16) & 0xFF);
    ptr[6] = (uint8_t)((bits >> 8) & 0xFF);
    ptr[7] = (uint8_t)(bits & 0xFF);
  }
  
  return js_mkundef();
}

static ant_value_t js_dataview_getBigInt64(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getBigInt64 requires byteOffset");

  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  bool little_endian = (nargs > 1 && js_truthy(js, args[1]));

  if (offset + 8 > dv->byte_length) return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint64_t bits;
  if (little_endian) {
    bits = (uint64_t)ptr[0] | ((uint64_t)ptr[1] << 8) | ((uint64_t)ptr[2] << 16) | ((uint64_t)ptr[3] << 24) |
           ((uint64_t)ptr[4] << 32) | ((uint64_t)ptr[5] << 40) | ((uint64_t)ptr[6] << 48) | ((uint64_t)ptr[7] << 56);
  } else {
    bits = ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
           ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | (uint64_t)ptr[7];
  }

  return bigint_from_int64(js, (int64_t)bits);
}

static ant_value_t js_dataview_setBigInt64(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setBigInt64 requires byteOffset and value");

  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  ant_value_t bigint = buffer_require_bigint_value(js, args[1]);
  bool little_endian = (nargs > 2 && js_truthy(js, args[2]));
  int64_t wrapped = 0;

  if (is_err(bigint)) return bigint;
  if (offset + 8 > dv->byte_length) return js_mkerr(js, "Offset out of bounds");
  if (!bigint_to_int64_wrapping(js, bigint, &wrapped)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  }

  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint64_t bits = (uint64_t)wrapped;
  if (little_endian) {
    ptr[0] = (uint8_t)(bits & 0xFF);
    ptr[1] = (uint8_t)((bits >> 8) & 0xFF);
    ptr[2] = (uint8_t)((bits >> 16) & 0xFF);
    ptr[3] = (uint8_t)((bits >> 24) & 0xFF);
    ptr[4] = (uint8_t)((bits >> 32) & 0xFF);
    ptr[5] = (uint8_t)((bits >> 40) & 0xFF);
    ptr[6] = (uint8_t)((bits >> 48) & 0xFF);
    ptr[7] = (uint8_t)((bits >> 56) & 0xFF);
  } else {
    ptr[0] = (uint8_t)((bits >> 56) & 0xFF);
    ptr[1] = (uint8_t)((bits >> 48) & 0xFF);
    ptr[2] = (uint8_t)((bits >> 40) & 0xFF);
    ptr[3] = (uint8_t)((bits >> 32) & 0xFF);
    ptr[4] = (uint8_t)((bits >> 24) & 0xFF);
    ptr[5] = (uint8_t)((bits >> 16) & 0xFF);
    ptr[6] = (uint8_t)((bits >> 8) & 0xFF);
    ptr[7] = (uint8_t)(bits & 0xFF);
  }

  return js_mkundef();
}

static ant_value_t js_dataview_getBigUint64(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getBigUint64 requires byteOffset");

  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  bool little_endian = (nargs > 1 && js_truthy(js, args[1]));

  if (offset + 8 > dv->byte_length) return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  uint64_t bits;
  if (little_endian) {
    bits = (uint64_t)ptr[0] | ((uint64_t)ptr[1] << 8) | ((uint64_t)ptr[2] << 16) | ((uint64_t)ptr[3] << 24) |
           ((uint64_t)ptr[4] << 32) | ((uint64_t)ptr[5] << 40) | ((uint64_t)ptr[6] << 48) | ((uint64_t)ptr[7] << 56);
  } else {
    bits = ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
           ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | (uint64_t)ptr[7];
  }

  return bigint_from_uint64(js, bits);
}

static ant_value_t js_dataview_setBigUint64(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setBigUint64 requires byteOffset and value");

  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr(js, "Not a DataView");
  size_t offset = (size_t)js_getnum(args[0]);
  ant_value_t bigint = buffer_require_bigint_value(js, args[1]);
  bool little_endian = (nargs > 2 && js_truthy(js, args[2]));
  uint64_t wrapped = 0;

  if (is_err(bigint)) return bigint;
  if (offset + 8 > dv->byte_length) return js_mkerr(js, "Offset out of bounds");
  if (!bigint_to_uint64_wrapping(js, bigint, &wrapped)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  }

  uint8_t *ptr = dv->buffer->data + dv->byte_offset + offset;
  if (little_endian) {
    ptr[0] = (uint8_t)(wrapped & 0xFF);
    ptr[1] = (uint8_t)((wrapped >> 8) & 0xFF);
    ptr[2] = (uint8_t)((wrapped >> 16) & 0xFF);
    ptr[3] = (uint8_t)((wrapped >> 24) & 0xFF);
    ptr[4] = (uint8_t)((wrapped >> 32) & 0xFF);
    ptr[5] = (uint8_t)((wrapped >> 40) & 0xFF);
    ptr[6] = (uint8_t)((wrapped >> 48) & 0xFF);
    ptr[7] = (uint8_t)((wrapped >> 56) & 0xFF);
  } else {
    ptr[0] = (uint8_t)((wrapped >> 56) & 0xFF);
    ptr[1] = (uint8_t)((wrapped >> 48) & 0xFF);
    ptr[2] = (uint8_t)((wrapped >> 40) & 0xFF);
    ptr[3] = (uint8_t)((wrapped >> 32) & 0xFF);
    ptr[4] = (uint8_t)((wrapped >> 24) & 0xFF);
    ptr[5] = (uint8_t)((wrapped >> 16) & 0xFF);
    ptr[6] = (uint8_t)((wrapped >> 8) & 0xFF);
    ptr[7] = (uint8_t)(wrapped & 0xFF);
  }

  return js_mkundef();
}

static uint8_t *hex_decode(const char *data, size_t len, size_t *out_len) {
  if (len % 2 != 0) return NULL;
  
  size_t decoded_len = len / 2;
  size_t alloc_len = decoded_len;
  
  if (alloc_len == 0) alloc_len = 1;
  uint8_t *decoded = malloc(alloc_len);
  if (!decoded) return NULL;
  
  for (size_t i = 0; i < decoded_len; i++) {
    unsigned char hi_ch = (unsigned char)data[i * 2];
    unsigned char lo_ch = (unsigned char)data[i * 2 + 1];
    int hi; int lo;
    
    if (hi_ch >= '0' && hi_ch <= '9') { hi = hi_ch - '0'; goto have_hi; }
    if (hi_ch >= 'a' && hi_ch <= 'f') { hi = hi_ch - 'a' + 10; goto have_hi; }
    if (hi_ch >= 'A' && hi_ch <= 'F') { hi = hi_ch - 'A' + 10; goto have_hi; }
    goto fail;
    
  have_hi:
    if (lo_ch >= '0' && lo_ch <= '9') { lo = lo_ch - '0'; goto have_lo; }
    if (lo_ch >= 'a' && lo_ch <= 'f') { lo = lo_ch - 'a' + 10; goto have_lo; }
    if (lo_ch >= 'A' && lo_ch <= 'F') { lo = lo_ch - 'A' + 10; goto have_lo; }
    goto fail;
    
  have_lo:
    decoded[i] = (uint8_t)((hi << 4) | lo);
  }
  
  *out_len = decoded_len;
  return decoded;

fail:
  free(decoded);
  return NULL;
}

static int hex_nibble(unsigned char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static uint8_t *hex_decode_node_prefix(const char *data, size_t len, size_t *out_len) {
  size_t max_len = len / 2;
  uint8_t *decoded = malloc(max_len == 0 ? 1 : max_len);
  if (!decoded) return NULL;

  size_t count = 0;
  for (size_t i = 0; i + 1 < len; i += 2) {
    int hi = hex_nibble((unsigned char)data[i]);
    int lo = hex_nibble((unsigned char)data[i + 1]);
    if (hi < 0 || lo < 0) break;
    decoded[count++] = (uint8_t)((hi << 4) | lo);
  }

  *out_len = count;
  return decoded;
}

static ant_value_t uint8array_from_bytes(ant_t *js, const uint8_t *bytes, size_t len) {
  ArrayBufferData *buffer = create_array_buffer_data(len);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  if (len > 0) memcpy(buffer->data, bytes, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Uint8Array");
}

static ant_value_t js_uint8array_fromHex(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(source)) return source;

  size_t len = 0;
  char *str = js_getstr(js, source, &len);
  
  size_t decoded_len = 0;
  uint8_t *decoded = hex_decode(str, len, &decoded_len);
  if (!decoded) return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid hex string");

  ant_value_t result = uint8array_from_bytes(js, decoded, decoded_len);
  free(decoded);
  
  return result;
}

static ant_value_t js_uint8array_fromBase64(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(source)) return source;

  size_t len = 0;
  char *str = js_getstr(js, source, &len);
  
  size_t decoded_len = 0;
  uint8_t *decoded = ant_base64_decode(str, len, &decoded_len);
  if (!decoded) return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid base64 string");

  ant_value_t result = uint8array_from_bytes(js, decoded, decoded_len);
  free(decoded);
  
  return result;
}

static ant_value_t js_uint8array_toHex(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array.prototype.toHex called on incompatible receiver");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot read from detached Uint8Array");

  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  size_t len = ta_data->byte_length;
  char *hex = malloc(len * 2 + 1);
  
  if (!hex) return js_mkerr(js, "Failed to allocate hex string");
  for (size_t i = 0; i < len; i++) snprintf(hex + i * 2, 3, "%02x", data[i]);

  ant_value_t result = js_mkstr(js, hex, len * 2);
  free(hex);
  
  return result;
}

static ant_value_t js_uint8array_toBase64(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array.prototype.toBase64 called on incompatible receiver");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot read from detached Uint8Array");

  size_t out_len = 0;
  char *encoded = ant_base64_encode(
    ta_data->buffer->data + ta_data->byte_offset,
    ta_data->byte_length, &out_len
  );
  
  if (!encoded) return js_mkerr(js, "Failed to encode base64");
  ant_value_t result = js_mkstr(js, encoded, out_len);
  free(encoded);
  
  return result;
}

static ant_value_t uint8array_set_result(ant_t *js, size_t read, size_t written) {
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "read", js_mknum((double)read));
  js_set(js, result, "written", js_mknum((double)written));
  return result;
}

static ant_value_t uint8array_set_bytes(ant_t *js, const uint8_t *bytes, size_t byte_len, size_t read) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array setFrom called on incompatible receiver");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot write to detached Uint8Array");
  if (byte_len > ta_data->byte_length)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Decoded data does not fit in Uint8Array");

  if (byte_len > 0) memcpy(ta_data->buffer->data + ta_data->byte_offset, bytes, byte_len);
  return uint8array_set_result(js, read, byte_len);
}

static ant_value_t js_uint8array_setFromHex(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(source)) return source;

  size_t len = 0;
  char *str = js_getstr(js, source, &len);
  size_t decoded_len = 0;
  
  uint8_t *decoded = hex_decode(str, len, &decoded_len);
  if (!decoded) return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid hex string");

  ant_value_t result = uint8array_set_bytes(js, decoded, decoded_len, len);
  free(decoded);
  
  return result;
}

static ant_value_t js_uint8array_setFromBase64(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "", 0);
  if (is_err(source)) return source;

  size_t len = 0;
  char *str = js_getstr(js, source, &len);
  size_t decoded_len = 0;
  
  uint8_t *decoded = ant_base64_decode(str, len, &decoded_len);
  if (!decoded) return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid base64 string");

  ant_value_t result = uint8array_set_bytes(js, decoded, decoded_len, len);
  free(decoded);
  
  return result;
}

typedef enum {
  ENC_UTF8,
  ENC_HEX,
  ENC_BASE64,
  ENC_ASCII,
  ENC_LATIN1,
  ENC_UCS2,
  ENC_UNKNOWN
} BufferEncoding;

static BufferEncoding parse_encoding(const char *enc, size_t len) {
  if (len == 3 && strncasecmp(enc, "hex", 3) == 0) return ENC_HEX;
  if (len == 5 && strncasecmp(enc, "ascii", 5) == 0) return ENC_ASCII;
  if (len == 6 && strncasecmp(enc, "base64", 6) == 0) return ENC_BASE64;
  if ((len == 4 && strncasecmp(enc, "utf8", 4) == 0) || (len == 5 && strncasecmp(enc, "utf-8", 5) == 0)) return ENC_UTF8;
  if ((len == 6 && strncasecmp(enc, "latin1", 6) == 0) || (len == 6 && strncasecmp(enc, "binary", 6) == 0)) return ENC_LATIN1;
  
  if (
    (len == 4 && strncasecmp(enc, "ucs2", 4) == 0) ||
    (len == 5 && strncasecmp(enc, "ucs-2", 5) == 0) ||
    (len == 7 && strncasecmp(enc, "utf16le", 7) == 0) ||
    (len == 8 && strncasecmp(enc, "utf-16le", 8) == 0)
  ) return ENC_UCS2;
  
  return ENC_UNKNOWN;
}

// Buffer.from(array/string/buffer, encoding)
static ant_value_t js_buffer_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Buffer.from requires at least one argument");
  
  if (vtype(args[0]) == T_STR) {
    size_t len;
    char *str = js_getstr(js, args[0], &len);
    
    BufferEncoding encoding = ENC_UTF8;
    if (nargs >= 2 && vtype(args[1]) == T_STR) {
      size_t enc_len;
      char *enc_str = js_getstr(js, args[1], &enc_len);
      encoding = parse_encoding(enc_str, enc_len);
      if (encoding == ENC_UNKNOWN) encoding = ENC_UTF8;
    }
    
    if (encoding == ENC_BASE64) {
      size_t decoded_len;
      uint8_t *decoded = ant_base64_decode(str, len, &decoded_len);
      if (!decoded) return js_mkerr(js, "Failed to decode base64");
      
      ArrayBufferData *buffer = create_array_buffer_data(decoded_len);
      if (!buffer) { free(decoded); return js_mkerr(js, "Failed to allocate buffer"); }
      
      memcpy(buffer->data, decoded, decoded_len);
      free(decoded);
      return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, decoded_len, "Buffer");
    } else if (encoding == ENC_HEX) {
      size_t decoded_len;
      uint8_t *decoded = hex_decode(str, len, &decoded_len);
      if (!decoded) return js_mkerr(js, "Failed to decode hex");
      
      ArrayBufferData *buffer = create_array_buffer_data(decoded_len);
      if (!buffer) { free(decoded); return js_mkerr(js, "Failed to allocate buffer"); }
      
      memcpy(buffer->data, decoded, decoded_len);
      free(decoded);
      return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, decoded_len, "Buffer");
    } else if (encoding == ENC_UCS2) {
      size_t unit_count = utf16_strlen(str, len);
      size_t decoded_len = unit_count * 2;
      ArrayBufferData *buffer = create_array_buffer_data(decoded_len);
      if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
      
      for (size_t i = 0; i < unit_count; i++) {
        uint32_t unit = utf16_code_unit_at(str, len, i);
        buffer->data[i * 2] = (uint8_t)(unit & 0xff);
        buffer->data[i * 2 + 1] = (uint8_t)((unit >> 8) & 0xff);
      }
      return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, decoded_len, "Buffer");
    } else {
      ArrayBufferData *buffer = create_array_buffer_data(len);
      if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
      
      memcpy(buffer->data, str, len);
      return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Buffer");
    }
  }

  ArrayBufferData *arraybuffer = buffer_get_arraybuffer_data(args[0]);
  if (arraybuffer) {
    size_t byte_offset = 0;
    size_t byte_length = arraybuffer->length;

    if (nargs > 1 && vtype(args[1]) == T_NUM) byte_offset = (size_t)js_getnum(args[1]);
    if (byte_offset > arraybuffer->length)
      return js_mkerr(js, "Start offset is outside the bounds of the buffer");

    if (nargs > 2 && vtype(args[2]) == T_NUM) {
      byte_length = (size_t)js_getnum(args[2]);
      if (byte_length > arraybuffer->length - byte_offset)
        return js_mkerr(js, "Invalid Buffer length");
    } else byte_length = arraybuffer->length - byte_offset;

    return create_typed_array_with_buffer(
      js, TYPED_ARRAY_UINT8, arraybuffer, byte_offset, byte_length, "Buffer", args[0]
    );
  }
  
  ant_value_t length_val = js_get(js, args[0], "length");
  if (vtype(length_val) == T_NUM) {
    size_t len = (size_t)js_getnum(length_val);
    ArrayBufferData *buffer = create_array_buffer_data(len);
    if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
    
    for (size_t i = 0; i < len; i++) {
      char idx_str[32];
      snprintf(idx_str, sizeof(idx_str), "%zu", i);
      ant_value_t elem = js_get(js, args[0], idx_str);
      if (vtype(elem) == T_NUM) {
        buffer->data[i] = (uint8_t)js_getnum(elem);
      }
    }
    
    return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Buffer");
  }
  
  return js_mkerr(js, "Invalid argument to Buffer.from");
}

// Buffer.alloc(size)
static ant_value_t js_buffer_alloc(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Buffer.alloc requires a size argument");
  }
  
  size_t size = (size_t)js_getnum(args[0]);
  ArrayBufferData *buffer = create_array_buffer_data(size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  memset(buffer->data, 0, size);
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, "Buffer");
}

// Buffer.allocUnsafe(size)
static ant_value_t js_buffer_allocUnsafe(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Buffer.allocUnsafe requires a size argument");
  }
  
  size_t size = (size_t)js_getnum(args[0]);
  ArrayBufferData *buffer = create_array_buffer_data(size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, "Buffer");
}

static ant_value_t js_buffer_allocUnsafeSlow(ant_t *js, ant_value_t *args, int nargs) {
  return js_buffer_allocUnsafe(js, args, nargs);
}

static ant_value_t typedarray_join_with(ant_t *js, ant_value_t this_val, const char *sep, size_t sep_len) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkstr(js, "", 0);
  if (!ta_data->buffer || ta_data->buffer->is_detached || ta_data->length == 0)
    return js_mkstr(js, "", 0);

  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  size_t len = ta_data->length;

  size_t cap = len * 12;
  char *buf = malloc(cap);
  if (!buf) return js_mkerr(js, "Out of memory");
  size_t pos = 0;

  for (size_t i = 0; i < len; i++) {
    if (i > 0) {
      if (pos + sep_len + 32 > cap) {
        cap *= 2;
        char *tmp = realloc(buf, cap);
        if (!tmp) { free(buf); return js_mkerr(js, "Out of memory"); }
        buf = tmp;
      }
      memcpy(buf + pos, sep, sep_len);
      pos += sep_len;
    }

    if (pos + 32 > cap) {
      cap *= 2;
      char *tmp = realloc(buf, cap);
      if (!tmp) { free(buf); return js_mkerr(js, "Out of memory"); }
      buf = tmp;
    }

    int written = 0;
    switch (ta_data->type) {
      case TYPED_ARRAY_INT8:          written = snprintf(buf + pos, cap - pos, "%d", ((int8_t*)data)[i]); break;
      case TYPED_ARRAY_UINT8:
      case TYPED_ARRAY_UINT8_CLAMPED: written = snprintf(buf + pos, cap - pos, "%u", data[i]); break;
      case TYPED_ARRAY_INT16:         written = snprintf(buf + pos, cap - pos, "%d", ((int16_t*)data)[i]); break;
      case TYPED_ARRAY_UINT16:        written = snprintf(buf + pos, cap - pos, "%u", ((uint16_t*)data)[i]); break;
      case TYPED_ARRAY_INT32:         written = snprintf(buf + pos, cap - pos, "%d", ((int32_t*)data)[i]); break;
      case TYPED_ARRAY_UINT32:        written = snprintf(buf + pos, cap - pos, "%u", ((uint32_t*)data)[i]); break;
      case TYPED_ARRAY_FLOAT16:       written = snprintf(buf + pos, cap - pos, "%g", half_to_double(((uint16_t*)data)[i])); break;
      case TYPED_ARRAY_FLOAT32:       written = snprintf(buf + pos, cap - pos, "%g", (double)((float*)data)[i]); break;
      case TYPED_ARRAY_FLOAT64:       written = snprintf(buf + pos, cap - pos, "%g", ((double*)data)[i]); break;
      case TYPED_ARRAY_BIGINT64:      written = snprintf(buf + pos, cap - pos, "%lld", ((long long*)data)[i]); break;
      case TYPED_ARRAY_BIGUINT64:     written = snprintf(buf + pos, cap - pos, "%llu", ((unsigned long long*)data)[i]); break;
      default: break;
    }
    if (written > 0) pos += (size_t)written;
  }

  ant_value_t ret = js_mkstr(js, buf, pos);
  free(buf);
  return ret;
}

// TypedArray.prototype.toString()
static ant_value_t js_typedarray_toString(ant_t *js, ant_value_t *args, int nargs) {
  return typedarray_join_with(js, js_getthis(js), ",", 1);
}

// TypedArray.prototype.join(separator)
static ant_value_t js_typedarray_join(ant_t *js, ant_value_t *args, int nargs) {
  const char *sep = ",";
  size_t sep_len = 1;
  if (nargs > 0 && vtype(args[0]) == T_STR)
    sep = js_getstr(js, args[0], &sep_len);
  return typedarray_join_with(js, js_getthis(js), sep, sep_len);
}

static ant_value_t js_typedarray_indexOf(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached) return js_mknum(-1);

  size_t len = ta_data->length;
  if (len == 0 || nargs < 1) return js_mknum(-1);

  int64_t from_index = 0;
  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
  from_index = (int64_t)js_to_number(js, args[1]);
  if (from_index < 0) {
    from_index += (int64_t)len;
    if (from_index < 0) from_index = 0;
  }}
  
  if ((size_t)from_index >= len) return js_mknum(-1);
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  double needle_num = js_to_number(js, args[0]);

  for (size_t i = (size_t)from_index; i < len; i++) {
    bool match = false;
    switch (ta_data->type) {
      case TYPED_ARRAY_INT8:          match = ((int8_t *)data)[i] == (int8_t)needle_num; break;
      case TYPED_ARRAY_UINT8:
      case TYPED_ARRAY_UINT8_CLAMPED: match = data[i] == (uint8_t)needle_num; break;
      case TYPED_ARRAY_INT16:         match = ((int16_t *)data)[i] == (int16_t)needle_num; break;
      case TYPED_ARRAY_UINT16:        match = ((uint16_t *)data)[i] == (uint16_t)needle_num; break;
      case TYPED_ARRAY_INT32:         match = ((int32_t *)data)[i] == (int32_t)needle_num; break;
      case TYPED_ARRAY_UINT32:        match = ((uint32_t *)data)[i] == (uint32_t)needle_num; break;
      case TYPED_ARRAY_FLOAT16:       match = half_to_double(((uint16_t *)data)[i]) == needle_num; break;
      case TYPED_ARRAY_FLOAT32:       match = ((float *)data)[i] == (float)needle_num; break;
      case TYPED_ARRAY_FLOAT64:       match = ((double *)data)[i] == needle_num; break;
      case TYPED_ARRAY_BIGINT64:      match = ((int64_t *)data)[i] == (int64_t)needle_num; break;
      case TYPED_ARRAY_BIGUINT64:     match = ((uint64_t *)data)[i] == (uint64_t)needle_num; break;
      default: break;
    }
    if (match) return js_mknum((double)i);
  }

  return js_mknum(-1);
}

static ant_value_t js_typedarray_lastIndexOf(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached) return js_mknum(-1);

  size_t len = ta_data->length;
  if (len == 0 || nargs < 1) return js_mknum(-1);

  int64_t from_index = (int64_t)len - 1;
  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
    double from_num = js_to_number(js, args[1]);
    if (isnan(from_num)) return js_mknum(-1);
    from_index = (int64_t)from_num;
    if (from_index < 0) from_index += (int64_t)len;
    if (from_index >= (int64_t)len) from_index = (int64_t)len - 1;
  }
  if (from_index < 0) return js_mknum(-1);

  ant_value_t search = nargs > 0 ? args[0] : js_mkundef();
  for (int64_t i = from_index; i >= 0; i--) {
    ant_value_t value = js_mkundef();
    if (!typedarray_read_value(js, ta_data, (size_t)i, &value)) return js_mknum(-1);
    if (strict_eq_values(js, value, search)) return js_mknum((double)i);
  }

  return js_mknum(-1);
}

static ant_value_t js_typedarray_includes(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached) {
    return js_mkerr(js, "Invalid TypedArray");
  }

  size_t len = ta_data->length;
  ant_value_t search = (nargs > 0) ? args[0] : js_mkundef();
  
  if (len == 0) return js_false;
  int64_t from_index = 0;
  
  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
  double from_index_num = js_to_number(js, args[1]);
  if (!isnan(from_index_num)) from_index = (int64_t)from_index_num;
  if (from_index < 0) {
    from_index += (int64_t)len;
    if (from_index < 0) from_index = 0;
  }}

  if ((size_t)from_index >= len) return js_false;

  if (ta_data->type == TYPED_ARRAY_BIGINT64) {
    int64_t needle = 0;
    if (vtype(search) == T_BIGINT) {
      if (!bigint_to_int64_wrapping(js, search, &needle)) return js_false;
    } else needle = (int64_t)js_to_number(js, search);
    
    int64_t *data = (int64_t *)(ta_data->buffer->data + ta_data->byte_offset);
    for (size_t i = (size_t)from_index; i < len; i++) {
      if (data[i] == needle) return js_true;
    }
    
    return js_false;
  }

  if (ta_data->type == TYPED_ARRAY_BIGUINT64) {
    uint64_t needle = 0;
    if (vtype(search) == T_BIGINT) {
      if (!bigint_to_uint64_wrapping(js, search, &needle)) return js_false;
    } else needle = (uint64_t)js_to_number(js, search);
    
    uint64_t *data = (uint64_t *)(ta_data->buffer->data + ta_data->byte_offset);
    for (size_t i = (size_t)from_index; i < len; i++) {
      if (data[i] == needle) return js_true;
    }
    
    return js_false;
  }

  double needle = js_to_number(js, search);
  for (size_t i = (size_t)from_index; i < len; i++) {
    double value = 0;
    if (!typedarray_read_number(ta_data, i, &value)) return js_false;
    if (isnan(value) && isnan(needle)) return js_true;
    if (value == needle) return js_true;
  }

  return js_false;
}

static ant_value_t js_typedarray_reverse(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  for (size_t left = 0, right = ta_data->length; left < right && left < --right; left++) {
    ant_value_t a = js_mkundef();
    ant_value_t b = js_mkundef();
    if (!typedarray_read_value(js, ta_data, left, &a)) break;
    if (!typedarray_read_value(js, ta_data, right, &b)) break;
    ant_value_t write_b = typedarray_write_value(js, ta_data, left, b);
    if (is_err(write_b)) return write_b;
    ant_value_t write_a = typedarray_write_value(js, ta_data, right, a);
    if (is_err(write_a)) return write_a;
  }

  return this_val;
}

static ant_value_t js_typedarray_sort(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot operate on a detached TypedArray");

  bool has_compare = nargs > 0 && vtype(args[0]) != T_UNDEF;
  if (has_compare && !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.sort comparefn must be callable");

  size_t len = ta_data->length;
  ant_value_t *values = len ? malloc(sizeof(ant_value_t) * len) : NULL;
  if (len && !values) return js_mkerr(js, "oom");

  for (size_t i = 0; i < len; i++) {
    if (!typedarray_read_value(js, ta_data, i, &values[i])) {
      free(values);
      return this_val;
    }
  }

  for (size_t i = 1; i < len; i++) {
    ant_value_t key = values[i];
    size_t j = i;
    while (j > 0) {
      double cmp = 0.0;
      if (has_compare) {
        ant_value_t cmp_args[2] = { values[j - 1], key };
        ant_value_t cmp_val = sv_vm_call(js->vm, js, args[0], js_mkundef(), cmp_args, 2, NULL, false);
        if (is_err(cmp_val)) { free(values); return cmp_val; }
        cmp = js_to_number(js, cmp_val);
      } else {
        double left = js_to_number(js, values[j - 1]);
        double right = js_to_number(js, key);
        cmp = (left > right) - (left < right);
      }
      if (!(cmp > 0)) break;
      values[j] = values[j - 1];
      j--;
    }
    values[j] = key;
  }

  for (size_t i = 0; i < len; i++) {
    ant_value_t write_result = typedarray_write_value(js, ta_data, i, values[i]);
    if (is_err(write_result)) { free(values); return write_result; }
  }

  free(values);
  return this_val;
}

// Buffer.prototype.toString(encoding)
static ant_value_t js_buffer_slice(ant_t *js, ant_value_t *args, int nargs) {
  return js_typedarray_subarray(js, args, nargs);
}

// Buffer.prototype.toString(encoding)
static ant_value_t js_buffer_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  BufferEncoding encoding = ENC_UTF8;
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    size_t enc_len;
    char *enc_str = js_getstr(js, args[0], &enc_len);
    encoding = parse_encoding(enc_str, enc_len);
    if (encoding == ENC_UNKNOWN) encoding = ENC_UTF8;
  }
  
  if (!ta_data->buffer || ta_data->buffer->is_detached) {
    return js_mkerr(js, "Cannot read from detached buffer");
  }
  
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  size_t len = ta_data->byte_length;
  
  if (encoding == ENC_BASE64) {
    size_t out_len;
    char *encoded = ant_base64_encode(data, len, &out_len);
    if (!encoded) return js_mkerr(js, "Failed to encode base64");
    
    ant_value_t result = js_mkstr(js, encoded, out_len);
    free(encoded);
    return result;
  } else if (encoding == ENC_HEX) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return js_mkerr(js, "Failed to allocate hex string");
    
    for (size_t i = 0; i < len; i++) {
      snprintf(hex + i * 2, 3, "%02x", data[i]);
    }
    
    ant_value_t result = js_mkstr(js, hex, len * 2);
    free(hex);
    return result;
  } else if (encoding == ENC_UCS2) {
    size_t char_count = len / 2;
    char *str = malloc(char_count * 3 + 1);
    if (!str) return js_mkerr(js, "Failed to allocate string");
    
    size_t out_len = 0;
    for (size_t i = 0; i < char_count; i++) {
      uint32_t unit = (uint32_t)data[i * 2] | ((uint32_t)data[i * 2 + 1] << 8);
      out_len += (size_t)utf8_encode(unit, str + out_len);
    }
    
    str[out_len] = '\0';
    ant_value_t result = js_mkstr(js, str, out_len);
    free(str);
    
    return result;
  } else {
    size_t out_cap = len * 3 + 1;
    char *out = malloc(out_cap);
    if (!out) return js_mkerr(js, "Failed to allocate string");
    
    utf8_dec_t dec = { .bom_seen = true, .ignore_bom = true };
    utf8proc_ssize_t out_len = utf8_whatwg_decode(&dec, data, len, out, false, false);
    
    if (out_len < 0) {
      free(out);
      return js_mkerr(js, "Failed to decode buffer as UTF-8");
    }
    
    ant_value_t result = js_mkstr(js, out, (size_t)out_len);
    free(out);
    
    return result;
  }
}

static ant_value_t js_buffer_utf8Slice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  size_t start = 0;
  size_t end = 0;

  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  if (!ta_data->buffer || ta_data->buffer->is_detached)
    return js_mkerr(js, "Cannot read from detached buffer");

  end = ta_data->byte_length;
  if (nargs > 0 && vtype(args[0]) == T_NUM && js_getnum(args[0]) > 0)
    start = (size_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == T_NUM && js_getnum(args[1]) >= 0)
    end = (size_t)js_getnum(args[1]);

  if (start > ta_data->byte_length) start = ta_data->byte_length;
  if (end > ta_data->byte_length) end = ta_data->byte_length;
  if (end < start) end = start;

  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset + start;
  size_t len = end - start; size_t out_cap = len * 3 + 1;
  
  char *out = malloc(out_cap);
  if (!out) return js_mkerr(js, "Failed to allocate string");

  utf8_dec_t dec = { .bom_seen = true, .ignore_bom = true };
  utf8proc_ssize_t out_len = utf8_whatwg_decode(&dec, data, len, out, false, false);
  if (out_len < 0) {
    free(out);
    return js_mkerr(js, "Failed to decode buffer as UTF-8");
  }

  ant_value_t result = js_mkstr(js, out, (size_t)out_len);
  free(out);
  
  return result;
}

// Buffer.prototype.toBase64()
static ant_value_t js_buffer_toBase64(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t encoding_arg = js_mkstr(js, "base64", 6);
  ant_value_t new_args[1] = {encoding_arg};
  return js_buffer_toString(js, new_args, 1);
}

static size_t buffer_normalize_indexof_offset(size_t len, double offset) {
  if (isnan(offset)) return 0;
  if (isinf(offset)) return offset < 0 ? 0 : len;

  double integer = offset < 0 ? ceil(offset) : floor(offset);
  if (integer < 0) {
    double from_end = (double)len + integer;
    return from_end <= 0 ? 0 : (size_t)from_end;
  }
  if (integer >= (double)len) return len;
  return (size_t)integer;
}

static ant_value_t buffer_encode_search_string(ant_t *js, ant_value_t value, BufferEncoding encoding, const uint8_t **out, size_t *out_len, uint8_t **owned) {
  ant_value_t str_value = js_tostring_val(js, value);
  if (is_err(str_value)) return str_value;

  size_t len = 0;
  char *str = js_getstr(js, str_value, &len);

  *out = (const uint8_t *)str;
  *out_len = len;
  *owned = NULL;

  if (encoding == ENC_BASE64) {
    size_t decoded_len = 0;
    uint8_t *decoded = ant_base64_decode(str, len, &decoded_len);
    if (!decoded) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid base64 string");
    *out = decoded;
    *out_len = decoded_len;
    *owned = decoded;
  } else if (encoding == ENC_HEX) {
    size_t decoded_len = 0;
    uint8_t *decoded = hex_decode_node_prefix(str, len, &decoded_len);
    if (!decoded) return js_mkerr(js, "Failed to allocate buffer");
    *out = decoded;
    *out_len = decoded_len;
    *owned = decoded;
  } else if (encoding == ENC_UCS2) {
    size_t unit_count = utf16_strlen(str, len);
    size_t decoded_len = unit_count * 2;
    uint8_t *decoded = malloc(decoded_len == 0 ? 1 : decoded_len);
    if (!decoded) return js_mkerr(js, "Failed to allocate string");
    for (size_t i = 0; i < unit_count; i++) {
      uint32_t unit = utf16_code_unit_at(str, len, i);
      decoded[i * 2] = (uint8_t)(unit & 0xff);
      decoded[i * 2 + 1] = (uint8_t)((unit >> 8) & 0xff);
    }
    *out = decoded;
    *out_len = decoded_len;
    *owned = decoded;
  }

  return js_mkundef();
}

static ant_value_t js_buffer_indexOf(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached) return js_mknum(-1);
  if (nargs < 1) return js_mknum(-1);

  uint8_t *haystack = ta_data->buffer->data + ta_data->byte_offset;
  size_t haystack_len = ta_data->byte_length;
  double offset_num = 0.0;
  BufferEncoding encoding = ENC_UTF8;

  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
    if (vtype(args[1]) == T_STR) {
      size_t enc_len = 0;
      char *enc_str = js_getstr(js, args[1], &enc_len);
      encoding = parse_encoding(enc_str, enc_len);
      if (encoding == ENC_UNKNOWN) return js_mkerr_typed(js, JS_ERR_TYPE, "Unknown encoding");
    } else {
      offset_num = js_to_number(js, args[1]);
    }
  }

  if (nargs > 2 && vtype(args[2]) != T_UNDEF) {
    ant_value_t enc_value = js_tostring_val(js, args[2]);
    if (is_err(enc_value)) return enc_value;
    size_t enc_len = 0;
    char *enc_str = js_getstr(js, enc_value, &enc_len);
    encoding = parse_encoding(enc_str, enc_len);
    if (encoding == ENC_UNKNOWN) return js_mkerr_typed(js, JS_ERR_TYPE, "Unknown encoding");
  }

  size_t start = buffer_normalize_indexof_offset(haystack_len, offset_num);
  ant_value_t search = args[0];

  if (vtype(search) == T_NUM) {
    if (start >= haystack_len) return js_mknum(-1);
    uint8_t needle = (uint8_t)js_to_uint32(js_getnum(search));
    for (size_t i = start; i < haystack_len; i++) {
      if (haystack[i] == needle) return js_mknum((double)i);
    }
    return js_mknum(-1);
  }

  const uint8_t *needle = NULL;
  size_t needle_len = 0;
  uint8_t *owned_needle = NULL;

  if (vtype(search) == T_STR) {
    ant_value_t encoded = buffer_encode_search_string(js, search, encoding, &needle, &needle_len, &owned_needle);
    if (is_err(encoded)) return encoded;
  } else if (!buffer_source_get_bytes(js, search, &needle, &needle_len)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "The value argument must be one of type number or string or an instance of Buffer or Uint8Array");
  }

  if (needle_len == 0) {
    if (owned_needle) free(owned_needle);
    return js_mknum((double)start);
  }

  if (start >= haystack_len || needle_len > haystack_len - start) {
    if (owned_needle) free(owned_needle);
    return js_mknum(-1);
  }

  size_t limit = haystack_len - needle_len;
  uint8_t first = needle[0];
  for (size_t i = start; i <= limit; i++) {
    if (haystack[i] == first && (needle_len == 1 || memcmp(haystack + i, needle, needle_len) == 0)) {
      if (owned_needle) free(owned_needle);
      return js_mknum((double)i);
    }
  }

  if (owned_needle) free(owned_needle);
  return js_mknum(-1);
}

// Buffer.prototype.write(string, offset, length, encoding)
static ant_value_t js_buffer_write(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "write requires a string");
  
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  size_t str_len;
  char *str = js_getstr(js, args[0], &str_len);
  size_t offset = 0;
  size_t length = ta_data->byte_length;
  
  if (nargs > 1 && vtype(args[1]) == T_NUM) {
    offset = (size_t)js_getnum(args[1]);
  }
  
  if (nargs > 2 && vtype(args[2]) == T_NUM) {
    length = (size_t)js_getnum(args[2]);
  }
  
  if (offset >= ta_data->byte_length) {
    return js_mknum(0);
  }
  
  size_t available = ta_data->byte_length - offset;
  size_t to_write = (str_len < length) ? str_len : length;
  to_write = (to_write < available) ? to_write : available;
  
  memcpy(ta_data->buffer->data + ta_data->byte_offset + offset, str, to_write);
  return js_mknum((double)to_write);
}

static ant_value_t js_buffer_copy(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "copy requires a target buffer");

  TypedArrayData *src = buffer_get_typedarray_data(js_getthis(js));
  TypedArrayData *dst = buffer_get_typedarray_data(args[0]);
  if (!src || !dst) return js_mkerr(js, "copy requires Buffer arguments");

  size_t target_start = (nargs > 1 && vtype(args[1]) == T_NUM) ? (size_t)js_getnum(args[1]) : 0;
  size_t source_start = (nargs > 2 && vtype(args[2]) == T_NUM) ? (size_t)js_getnum(args[2]) : 0;
  size_t source_end = (nargs > 3 && vtype(args[3]) == T_NUM) ? (size_t)js_getnum(args[3]) : src->byte_length;

  if (target_start > dst->byte_length) target_start = dst->byte_length;
  if (source_start > src->byte_length) source_start = src->byte_length;
  if (source_end > src->byte_length) source_end = src->byte_length;
  if (source_end < source_start) source_end = source_start;

  size_t src_len = source_end - source_start;
  size_t dst_len = dst->byte_length - target_start;
  size_t copy_len = src_len < dst_len ? src_len : dst_len;
  if (copy_len == 0) return js_mknum(0);

  uint8_t *src_ptr = src->buffer->data + src->byte_offset + source_start;
  uint8_t *dst_ptr = dst->buffer->data + dst->byte_offset + target_start;
  memmove(dst_ptr, src_ptr, copy_len);
  
  return js_mknum((double)copy_len);
}

static bool buffer_checked_byte_offset(
  ant_value_t value,
  size_t byte_length,
  size_t width,
  size_t *out
) {
  double offset_num = vtype(value) == T_NUM ? js_getnum(value) : 0.0;
  if (!isfinite(offset_num) || offset_num < 0) return false;

  offset_num = floor(offset_num);
  if (offset_num > (double)byte_length) return false;

  size_t offset = (size_t)offset_num;
  if (width > byte_length || offset > byte_length - width) return false;

  *out = offset;
  return true;
}

static ant_value_t js_buffer_writeInt16BE(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "writeInt16BE requires a value");

  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  int16_t value = (int16_t)js_to_int32(js_getnum(args[0]));
  size_t offset = 0;
  if (!buffer_checked_byte_offset(nargs > 1 ? args[1] : js_mkundef(), ta->byte_length, 2, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  ptr[0] = (uint8_t)((value >> 8) & 0xff);
  ptr[1] = (uint8_t)(value & 0xff);
  
  return js_mknum((double)(offset + 2));
}

static ant_value_t js_buffer_writeInt32BE(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "writeInt32BE requires a value");

  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  int32_t value = js_to_int32(js_getnum(args[0]));
  size_t offset = 0;
  if (!buffer_checked_byte_offset(nargs > 1 ? args[1] : js_mkundef(), ta->byte_length, 4, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  ptr[0] = (uint8_t)((value >> 24) & 0xff);
  ptr[1] = (uint8_t)((value >> 16) & 0xff);
  ptr[2] = (uint8_t)((value >> 8) & 0xff);
  ptr[3] = (uint8_t)(value & 0xff);
  
  return js_mknum((double)(offset + 4));
}

static ant_value_t js_buffer_writeUInt32BE(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "writeUInt32BE requires a value");

  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  uint32_t value = js_to_uint32(js_getnum(args[0]));
  size_t offset = 0;
  if (!buffer_checked_byte_offset(nargs > 1 ? args[1] : js_mkundef(), ta->byte_length, 4, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  ptr[0] = (uint8_t)((value >> 24) & 0xff);
  ptr[1] = (uint8_t)((value >> 16) & 0xff);
  ptr[2] = (uint8_t)((value >> 8) & 0xff);
  ptr[3] = (uint8_t)(value & 0xff);
  
  return js_mknum((double)(offset + 4));
}

static ant_value_t js_buffer_writeUInt16BE(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "writeUInt16BE requires a value");

  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  double number = js_to_number(js, args[0]);
  if (!isfinite(number) || number < 0 || floor(number) != number || number > 0xffff)
    return js_mkerr_typed(js, JS_ERR_RANGE, "value out of range");

  uint16_t value = (uint16_t)number;
  size_t offset = 0;
  if (!buffer_checked_byte_offset(nargs > 1 ? args[1] : js_mkundef(), ta->byte_length, 2, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  ptr[0] = (uint8_t)((value >> 8) & 0xff);
  ptr[1] = (uint8_t)(value & 0xff);

  return js_mknum((double)(offset + 2));
}

static ant_value_t js_buffer_writeUIntBE(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_mkerr(js, "writeUIntBE requires value, offset, and byteLength");

  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  double number = js_to_number(js, args[0]);
  size_t byte_length = (size_t)js_getnum(args[2]);
  size_t offset = 0;

  if (byte_length == 0 || byte_length > 6) return js_mkerr(js, "byteLength out of range");
  uint64_t max = (1ULL << (byte_length * 8)) - 1;
  if (!isfinite(number) || number < 0 || floor(number) != number || number > (double)max)
    return js_mkerr_typed(js, JS_ERR_RANGE, "value out of range");

  uint64_t value = (uint64_t)number;
  if (!buffer_checked_byte_offset(args[1], ta->byte_length, byte_length, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  for (size_t i = byte_length; i > 0; i--) {
    ptr[i - 1] = (uint8_t)(value & 0xff);
    value >>= 8;
  }

  return js_mknum((double)(offset + byte_length));
}

static ant_value_t js_buffer_readInt16BE(ant_t *js, ant_value_t *args, int nargs) {
  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  size_t offset = 0;
  if (!buffer_checked_byte_offset(nargs > 0 ? args[0] : js_mkundef(), ta->byte_length, 2, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  int16_t value = (int16_t)((ptr[0] << 8) | ptr[1]);
  
  return js_mknum((double)value);
}

static ant_value_t js_buffer_readUInt16BE(ant_t *js, ant_value_t *args, int nargs) {
  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  size_t offset = 0;
  if (!buffer_checked_byte_offset(nargs > 0 ? args[0] : js_mkundef(), ta->byte_length, 2, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  uint16_t value = (uint16_t)(((uint16_t)ptr[0] << 8) | (uint16_t)ptr[1]);

  return js_mknum((double)value);
}

static ant_value_t js_buffer_readInt32BE(ant_t *js, ant_value_t *args, int nargs) {
  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  size_t offset = 0;
  if (!buffer_checked_byte_offset(nargs > 0 ? args[0] : js_mkundef(), ta->byte_length, 4, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  int32_t value = (int32_t)(((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) |
    ((uint32_t)ptr[2] << 8) | (uint32_t)ptr[3]);
    
  return js_mknum((double)value);
}

static ant_value_t js_buffer_readUInt32BE(ant_t *js, ant_value_t *args, int nargs) {
  TypedArrayData *ta = buffer_get_typedarray_data(js_getthis(js));
  if (!ta) return js_mkerr(js, "Invalid Buffer");

  size_t offset = 0;
  if (!buffer_checked_byte_offset(nargs > 0 ? args[0] : js_mkundef(), ta->byte_length, 4, &offset))
    return js_mkerr(js, "Offset out of bounds");

  uint8_t *ptr = ta->buffer->data + ta->byte_offset + offset;
  uint32_t value = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) |
    ((uint32_t)ptr[2] << 8) | (uint32_t)ptr[3];
    
  return js_mknum((double)value);
}

// Buffer.isBuffer(obj)
static ant_value_t js_buffer_isBuffer(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  if (!is_special_object(args[0])) return js_false;
  
  ant_value_t proto = js_get_proto(js, args[0]);
  ant_value_t buffer_proto = js_get_ctor_proto(js, "Buffer", 6);
  
  return js_bool(proto == buffer_proto);
}

// Buffer.isEncoding(encoding)
static ant_value_t js_buffer_isEncoding(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) return js_false;
  
  size_t len;
  char *enc = js_getstr(js, args[0], &len);
  
  if ((len == 4 && strncasecmp(enc, "utf8", 4) == 0) ||
      (len == 5 && strncasecmp(enc, "utf-8", 5) == 0) ||
      (len == 3 && strncasecmp(enc, "hex", 3) == 0) ||
      (len == 6 && strncasecmp(enc, "base64", 6) == 0) ||
      (len == 5 && strncasecmp(enc, "ascii", 5) == 0) ||
      (len == 6 && strncasecmp(enc, "latin1", 6) == 0) ||
      (len == 6 && strncasecmp(enc, "binary", 6) == 0) ||
      (len == 4 && strncasecmp(enc, "ucs2", 4) == 0) ||
      (len == 5 && strncasecmp(enc, "ucs-2", 5) == 0) ||
      (len == 7 && strncasecmp(enc, "utf16le", 7) == 0) ||
      (len == 8 && strncasecmp(enc, "utf-16le", 8) == 0)) {
    return js_true;
  }
  
  return js_false;
}

// Buffer.byteLength(string, encoding)
static ant_value_t js_buffer_byteLength(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mknum(0);
  
  ant_value_t arg = args[0];
  
  if (is_special_object(arg)) {
    ant_value_t bytelen = js_get(js, arg, "byteLength");
    if (vtype(bytelen) == T_NUM) return bytelen;
    
    ant_value_t len = js_get(js, arg, "length");
    if (vtype(len) == T_NUM) return len;
  }
  
  if (vtype(arg) == T_STR) {
    size_t len;
    js_getstr(js, arg, &len);
    return js_mknum((double)len);
  }
  
  return js_mknum(0);
}

// Buffer.concat(list, totalLength)
static ant_value_t js_buffer_concat(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_special_object(args[0])) {
    return js_mkerr(js, "First argument must be an array");
  }
  
  ant_value_t list = args[0];
  ant_value_t len_val = js_get(js, list, "length");
  if (vtype(len_val) != T_NUM) {
    return js_mkerr(js, "First argument must be an array");
  }
  
  size_t list_len = (size_t)js_getnum(len_val);
  size_t total_length = 0;
  
  if (nargs > 1 && vtype(args[1]) == T_NUM) {
    total_length = (size_t)js_getnum(args[1]);
  } else {
    for (size_t i = 0; i < list_len; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%zu", i);
      ant_value_t buf = js_get(js, list, idx);
      ant_value_t buf_len = js_get(js, buf, "length");
      if (vtype(buf_len) == T_NUM) total_length += (size_t)js_getnum(buf_len);
    }
  }
  
  ArrayBufferData *buffer = create_array_buffer_data(total_length);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  size_t offset = 0;
  for (size_t i = 0; i < list_len && offset < total_length; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%zu", i);
    ant_value_t buf = js_get(js, list, idx);
    
    TypedArrayData *ta = buffer_get_typedarray_data(buf);
    if (!ta || !ta->buffer) continue;
    
    size_t copy_len = ta->byte_length;
    if (offset + copy_len > total_length) {
      copy_len = total_length - offset;
    }
    
    memcpy(buffer->data + offset, ta->buffer->data + ta->byte_offset, copy_len);
    offset += copy_len;
  }
  
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, total_length, "Buffer");
}

// Buffer.compare(buf1, buf2)
static ant_value_t js_buffer_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Buffer.compare requires two arguments");
  
  TypedArrayData *ta1 = buffer_get_typedarray_data(args[0]);
  TypedArrayData *ta2 = buffer_get_typedarray_data(args[1]);
  
  if (!ta1 || !ta2) {
    return js_mkerr(js, "Arguments must be Buffers");
  }
  
  if (!ta1 || !ta1->buffer || !ta2 || !ta2->buffer) {
    return js_mkerr(js, "Invalid buffer");
  }
  
  size_t len = ta1->byte_length < ta2->byte_length ? ta1->byte_length : ta2->byte_length;
  int cmp = memcmp(ta1->buffer->data + ta1->byte_offset, ta2->buffer->data + ta2->byte_offset, len);
  
  if (cmp == 0) {
    if (ta1->byte_length < ta2->byte_length) cmp = -1;
    else if (ta1->byte_length > ta2->byte_length) cmp = 1;
  } else cmp = cmp < 0 ? -1 : 1;
  
  return js_mknum((double)cmp);
}

static ant_value_t js_sharedarraybuffer_constructor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SharedArrayBuffer constructor requires 'new'");
  }
  size_t length = 0;
  if (nargs > 0 && vtype(args[0]) == T_NUM) {
    length = (size_t)js_getnum(args[0]);
  }
  
  ArrayBufferData *data = create_shared_array_buffer_data(length);
  if (!data) {
    return js_mkerr(js, "Failed to allocate SharedArrayBuffer");
  }
  
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, "SharedArrayBuffer", 17);

  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set(js, obj, "byteLength", js_mknum((double)length));
  js_set_finalizer(obj, arraybuffer_finalize);
  
  return obj;
}

static ant_value_t buffer_make_constants(ant_t *js) {
  ant_value_t constants = js_newobj(js);
  js_set(js, constants, "MAX_LENGTH", js_mknum(BUFFER_COMPAT_MAX_LENGTH));
  js_set(js, constants, "MAX_STRING_LENGTH", js_mknum(BUFFER_COMPAT_MAX_STRING_LENGTH));
  return constants;
}

ant_value_t buffer_library(ant_t *js) {
  ant_value_t glob = js_glob(js);
  ant_value_t lib = js_newobj(js);

  js_set(js, lib, "Buffer", js_get(js, glob, "Buffer"));
  js_set(js, lib, "Blob", js_get(js, glob, "Blob"));
  js_set(js, lib, "File", js_get(js, glob, "File"));
  js_set(js, lib, "atob", js_get(js, glob, "atob"));
  js_set(js, lib, "btoa", js_get(js, glob, "btoa"));
  js_set(js, lib, "constants", buffer_make_constants(js));
  js_set(js, lib, "kMaxLength", js_mknum(BUFFER_COMPAT_MAX_LENGTH));
  js_set(js, lib, "kStringMaxLength", js_mknum(BUFFER_COMPAT_MAX_STRING_LENGTH));
  js_set(js, lib, "INSPECT_MAX_BYTES", js_mknum(BUFFER_COMPAT_INSPECT_MAX_BYTES));

  return lib;
}

void init_buffer_module() {
  ant_t *js = rt->js;
  
  ant_value_t glob = js->global;
  ant_value_t object_proto = js->sym.object_proto;
  ant_value_t function_proto = js_get_slot(glob, SLOT_FUNC_PROTO);
  if (vtype(function_proto) == T_UNDEF) function_proto = js_get_ctor_proto(js, "Function", 8);

  ant_value_t arraybuffer_ctor_obj = js_mkobj(js);
  ant_value_t arraybuffer_proto = js_mkobj(js);
  js_set_proto_init(arraybuffer_proto, object_proto);
  
  js_set(js, arraybuffer_proto, "slice", js_mkfun(js_arraybuffer_slice));
  js_set(js, arraybuffer_proto, "transfer", js_mkfun(js_arraybuffer_transfer));
  js_set(js, arraybuffer_proto, "transferToFixedLength", js_mkfun(js_arraybuffer_transferToFixedLength));
  js_set_getter_desc(js, arraybuffer_proto, "detached", 8, js_mkfun(js_arraybuffer_detached_getter), JS_DESC_E);
  js_set_getter_desc(js, arraybuffer_proto, "byteLength", 10, js_mkfun(js_arraybuffer_byteLength_getter), JS_DESC_C);
  js_set_sym(js, arraybuffer_proto, get_toStringTag_sym(), js_mkstr(js, "ArrayBuffer", 11));
  
  js_set_slot(arraybuffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_arraybuffer_constructor));
  js_mkprop_fast(js, arraybuffer_ctor_obj, "prototype", 9, arraybuffer_proto);
  js_mkprop_fast(js, arraybuffer_ctor_obj, "name", 4, ANT_STRING("ArrayBuffer"));
  js_set_descriptor(js, arraybuffer_ctor_obj, "name", 4, 0);
  js_set(js, arraybuffer_ctor_obj, "isView", js_mkfun(js_arraybuffer_isView));
  js_define_species_getter(js, arraybuffer_ctor_obj);
  ant_value_t arraybuffer_ctor = js_obj_to_func(arraybuffer_ctor_obj);
  js_set(js, arraybuffer_proto, "constructor", arraybuffer_ctor);
  js_set_descriptor(js, arraybuffer_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  js_set(js, glob, "ArrayBuffer", arraybuffer_ctor);
  
  ant_value_t typedarray_proto = js_mkobj(js);
  js_set_proto_init(typedarray_proto, object_proto);
  
  js_set(js, typedarray_proto, "at", js_mkfun(js_typedarray_at));
  js_set(js, typedarray_proto, "set", js_mkfun(js_typedarray_set));
  js_set(js, typedarray_proto, "copyWithin", js_mkfun(js_typedarray_copyWithin));
  js_set(js, typedarray_proto, "slice", js_mkfun(js_typedarray_slice));
  js_set(js, typedarray_proto, "subarray", js_mkfun(js_typedarray_subarray));
  js_set(js, typedarray_proto, "fill", js_mkfun(js_typedarray_fill));
  js_set(js, typedarray_proto, "toReversed", js_mkfun(js_typedarray_toReversed));
  js_set(js, typedarray_proto, "toSorted", js_mkfun(js_typedarray_toSorted));
  js_set(js, typedarray_proto, "with", js_mkfun(js_typedarray_with));
  js_set(js, typedarray_proto, "toString", js_mkfun(js_typedarray_toString));
  js_set(js, typedarray_proto, "join", js_mkfun(js_typedarray_join));
  js_set(js, typedarray_proto, "indexOf", js_mkfun(js_typedarray_indexOf));
  js_set(js, typedarray_proto, "lastIndexOf", js_mkfun(js_typedarray_lastIndexOf));
  js_set(js, typedarray_proto, "includes", js_mkfun(js_typedarray_includes));
  js_set(js, typedarray_proto, "every", js_mkfun(js_typedarray_every));
  js_set(js, typedarray_proto, "filter", js_mkfun(js_typedarray_filter));
  js_set(js, typedarray_proto, "find", js_mkfun(js_typedarray_find));
  js_set(js, typedarray_proto, "findIndex", js_mkfun(js_typedarray_findIndex));
  js_set(js, typedarray_proto, "forEach", js_mkfun(js_typedarray_forEach));
  js_set(js, typedarray_proto, "map", js_mkfun(js_typedarray_map));
  js_set(js, typedarray_proto, "reduce", js_mkfun(js_typedarray_reduce));
  js_set(js, typedarray_proto, "reduceRight", js_mkfun(js_typedarray_reduceRight));
  js_set(js, typedarray_proto, "reverse", js_mkfun(js_typedarray_reverse));
  js_set(js, typedarray_proto, "some", js_mkfun(js_typedarray_some));
  js_set(js, typedarray_proto, "sort", js_mkfun(js_typedarray_sort));
  js_set_sym_getter_desc(js, typedarray_proto, get_toStringTag_sym(), js_mkfun(js_typedarray_toStringTag_getter), JS_DESC_C);

  ant_value_t typedarray_ctor_obj = js_mkobj(js);
  js_set_proto_init(typedarray_ctor_obj, function_proto);
  js_set_slot(typedarray_ctor_obj, SLOT_CFUNC, js_mkfun(js_typedarray_base_constructor));
  js_mkprop_fast(js, typedarray_ctor_obj, "prototype", 9, typedarray_proto);
  js_mkprop_fast(js, typedarray_ctor_obj, "name", 4, ANT_STRING("TypedArray"));
  js_set_descriptor(js, typedarray_ctor_obj, "name", 4, 0);
  
  ant_value_t typedarray_ctor = js_obj_to_func(typedarray_ctor_obj);
  js_set(js, typedarray_proto, "constructor", typedarray_ctor);
  js_set_descriptor(js, typedarray_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  g_typedarray_iter_proto = js_mkobj(js);
  js_set_proto_init(g_typedarray_iter_proto, js->sym.iterator_proto);
  js_set(js, g_typedarray_iter_proto, "next", js_mkfun(ta_iter_next));
  js_iter_register_advance(g_typedarray_iter_proto, advance_typedarray);

  js_set(js, typedarray_proto, "values", js_mkfun(ta_values));
  js_set(js, typedarray_proto, "keys", js_mkfun(ta_keys));
  js_set(js, typedarray_proto, "entries", js_mkfun(ta_entries));
  js_set_sym(js, typedarray_proto, get_iterator_sym(), js_get(js, typedarray_proto, "values"));
  
  // TODO: find a better way of doing this, macro is code smell
  #define SETUP_TYPEDARRAY(name, type) \
    do { \
      ant_value_t name##_ctor_obj = js_mkobj(js); \
      ant_value_t name##_proto = js_mkobj(js); \
      ant_value_t elem_size = js_mknum((double)get_element_size(type)); \
      js_set_proto_init(name##_ctor_obj, typedarray_ctor); \
      js_set_proto_init(name##_proto, typedarray_proto); \
      js_set_sym(js, name##_proto, get_toStringTag_sym(), js_mkstr(js, #name, sizeof(#name) - 1)); \
      js_set_slot(name##_ctor_obj, SLOT_CFUNC, js_mkfun(js_##name##_constructor)); \
      js_setprop(js, name##_ctor_obj, js_mkstr(js, "prototype", 9), name##_proto); \
      js_set(js, name##_ctor_obj, "BYTES_PER_ELEMENT", elem_size); \
      js_set(js, name##_proto, "BYTES_PER_ELEMENT", elem_size); \
      js_mkprop_fast(js, name##_ctor_obj, "name", 4, ANT_STRING(#name)); \
      js_set_descriptor(js, name##_ctor_obj, "name", 4, 0); \
      js_define_species_getter(js, name##_ctor_obj); \
      js_set(js, name##_ctor_obj, "from", js_mkfun(js_##name##_from)); \
      js_set(js, name##_ctor_obj, "of", js_mkfun(js_##name##_of)); \
      ant_value_t name##_ctor = js_obj_to_func(name##_ctor_obj); \
      js_setprop(js, name##_proto, ANT_STRING("constructor"), name##_ctor); \
      js_set_descriptor(js, name##_proto, "constructor", 11, JS_DESC_W | JS_DESC_C); \
      js_set(js, glob, #name, name##_ctor); \
    } while(0)
  
  SETUP_TYPEDARRAY(Int8Array, TYPED_ARRAY_INT8);
  SETUP_TYPEDARRAY(Uint8Array, TYPED_ARRAY_UINT8);
  SETUP_TYPEDARRAY(Uint8ClampedArray, TYPED_ARRAY_UINT8_CLAMPED);
  SETUP_TYPEDARRAY(Int16Array, TYPED_ARRAY_INT16);
  SETUP_TYPEDARRAY(Uint16Array, TYPED_ARRAY_UINT16);
  SETUP_TYPEDARRAY(Int32Array, TYPED_ARRAY_INT32);
  SETUP_TYPEDARRAY(Uint32Array, TYPED_ARRAY_UINT32);
  SETUP_TYPEDARRAY(Float16Array, TYPED_ARRAY_FLOAT16);
  SETUP_TYPEDARRAY(Float32Array, TYPED_ARRAY_FLOAT32);
  SETUP_TYPEDARRAY(Float64Array, TYPED_ARRAY_FLOAT64);
  SETUP_TYPEDARRAY(BigInt64Array, TYPED_ARRAY_BIGINT64);
  SETUP_TYPEDARRAY(BigUint64Array, TYPED_ARRAY_BIGUINT64);

  ant_value_t uint8array_codec_ctor = js_get(js, glob, "Uint8Array");
  ant_value_t uint8array_codec_proto = js_get(js, uint8array_codec_ctor, "prototype");
  js_set(js, uint8array_codec_ctor, "fromHex", js_mkfun(js_uint8array_fromHex));
  js_set(js, uint8array_codec_ctor, "fromBase64", js_mkfun(js_uint8array_fromBase64));
  js_set(js, uint8array_codec_proto, "toHex", js_mkfun(js_uint8array_toHex));
  js_set(js, uint8array_codec_proto, "toBase64", js_mkfun(js_uint8array_toBase64));
  js_set(js, uint8array_codec_proto, "setFromHex", js_mkfun(js_uint8array_setFromHex));
  js_set(js, uint8array_codec_proto, "setFromBase64", js_mkfun(js_uint8array_setFromBase64));
  
  ant_value_t dataview_ctor_obj = js_mkobj(js);
  ant_value_t dataview_proto = js_mkobj(js);
  js_set_proto_init(dataview_proto, object_proto);
  
  js_set(js, dataview_proto, "getInt8", js_mkfun(js_dataview_getInt8));
  js_set(js, dataview_proto, "setInt8", js_mkfun(js_dataview_setInt8));
  js_set(js, dataview_proto, "getUint8", js_mkfun(js_dataview_getUint8));
  js_set(js, dataview_proto, "setUint8", js_mkfun(js_dataview_setUint8));
  js_set(js, dataview_proto, "getInt16", js_mkfun(js_dataview_getInt16));
  js_set(js, dataview_proto, "setInt16", js_mkfun(js_dataview_setInt16));
  js_set(js, dataview_proto, "getUint16", js_mkfun(js_dataview_getUint16));
  js_set(js, dataview_proto, "setUint16", js_mkfun(js_dataview_setUint16));
  js_set(js, dataview_proto, "getInt32", js_mkfun(js_dataview_getInt32));
  js_set(js, dataview_proto, "setInt32", js_mkfun(js_dataview_setInt32));
  js_set(js, dataview_proto, "getUint32", js_mkfun(js_dataview_getUint32));
  js_set(js, dataview_proto, "setUint32", js_mkfun(js_dataview_setUint32));
  js_set(js, dataview_proto, "getFloat32", js_mkfun(js_dataview_getFloat32));
  js_set(js, dataview_proto, "setFloat32", js_mkfun(js_dataview_setFloat32));
  js_set(js, dataview_proto, "getFloat64", js_mkfun(js_dataview_getFloat64));
  js_set(js, dataview_proto, "setFloat64", js_mkfun(js_dataview_setFloat64));
  js_set(js, dataview_proto, "getBigInt64", js_mkfun(js_dataview_getBigInt64));
  js_set(js, dataview_proto, "setBigInt64", js_mkfun(js_dataview_setBigInt64));
  js_set(js, dataview_proto, "getBigUint64", js_mkfun(js_dataview_getBigUint64));
  js_set(js, dataview_proto, "setBigUint64", js_mkfun(js_dataview_setBigUint64));
  js_set_sym(js, dataview_proto, get_toStringTag_sym(), js_mkstr(js, "DataView", 8));

  js_set_slot(dataview_ctor_obj, SLOT_CFUNC, js_mkfun(js_dataview_constructor));
  js_mkprop_fast(js, dataview_ctor_obj, "prototype", 9, dataview_proto);
  js_mkprop_fast(js, dataview_ctor_obj, "name", 4, ANT_STRING("DataView"));
  js_set_descriptor(js, dataview_ctor_obj, "name", 4, 0);
  
  ant_value_t dataview_ctor = js_obj_to_func(dataview_ctor_obj);
  js_set(js, dataview_proto, "constructor", dataview_ctor);
  js_set_descriptor(js, dataview_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  js_set(js, glob, "DataView", dataview_ctor);
  
  ant_value_t sharedarraybuffer_ctor_obj = js_mkobj(js);
  ant_value_t sharedarraybuffer_proto = js_mkobj(js);
  js_set_proto_init(sharedarraybuffer_proto, object_proto);
  
  js_set(js, sharedarraybuffer_proto, "slice", js_mkfun(js_arraybuffer_slice));
  js_set_getter_desc(js, sharedarraybuffer_proto, "byteLength", 10, js_mkfun(js_arraybuffer_byteLength_getter), JS_DESC_C);
  js_set_sym(js, sharedarraybuffer_proto, get_toStringTag_sym(), js_mkstr(js, "SharedArrayBuffer", 17));
  
  js_set_slot(sharedarraybuffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_sharedarraybuffer_constructor));
  js_mkprop_fast(js, sharedarraybuffer_ctor_obj, "prototype", 9, sharedarraybuffer_proto);
  js_mkprop_fast(js, sharedarraybuffer_ctor_obj, "name", 4, ANT_STRING("SharedArrayBuffer"));
  js_set_descriptor(js, sharedarraybuffer_ctor_obj, "name", 4, 0);
  js_define_species_getter(js, sharedarraybuffer_ctor_obj);
  
  ant_value_t sharedarraybuffer_ctor = js_obj_to_func(sharedarraybuffer_ctor_obj);
  js_set(js, sharedarraybuffer_proto, "constructor", sharedarraybuffer_ctor);
  js_set_descriptor(js, sharedarraybuffer_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  js_set(js, glob, "SharedArrayBuffer", sharedarraybuffer_ctor);
  
  ant_value_t buffer_ctor_obj = js_mkobj(js);
  ant_value_t buffer_proto = js_mkobj(js);
  
  ant_value_t uint8array_ctor = js_get(js, glob, "Uint8Array");
  ant_value_t uint8array_proto = js_get(js, uint8array_ctor, "prototype");
  
  if (is_special_object(uint8array_proto)) js_set_proto_init(buffer_proto, uint8array_proto);
  else js_set_proto_init(buffer_proto, typedarray_proto);
  
  js_set(js, buffer_proto, "slice", js_mkfun(js_buffer_slice));
  js_set(js, buffer_proto, "toString", js_mkfun(js_buffer_toString));
  js_set(js, buffer_proto, "utf8Slice", js_mkfun(js_buffer_utf8Slice));
  js_set(js, buffer_proto, "toBase64", js_mkfun(js_buffer_toBase64));
  js_set(js, buffer_proto, "indexOf", js_mkfun(js_buffer_indexOf));
  js_set(js, buffer_proto, "write", js_mkfun(js_buffer_write));
  js_set(js, buffer_proto, "copy", js_mkfun(js_buffer_copy));
  js_set(js, buffer_proto, "writeInt16BE", js_mkfun(js_buffer_writeInt16BE));
  js_set(js, buffer_proto, "writeInt32BE", js_mkfun(js_buffer_writeInt32BE));
  js_set(js, buffer_proto, "writeUInt16BE", js_mkfun(js_buffer_writeUInt16BE));
  js_set(js, buffer_proto, "writeUIntBE", js_mkfun(js_buffer_writeUIntBE));
  js_set(js, buffer_proto, "writeUInt32BE", js_mkfun(js_buffer_writeUInt32BE));
  js_set(js, buffer_proto, "readInt16BE", js_mkfun(js_buffer_readInt16BE));
  js_set(js, buffer_proto, "readUInt16BE", js_mkfun(js_buffer_readUInt16BE));
  js_set(js, buffer_proto, "readInt32BE", js_mkfun(js_buffer_readInt32BE));
  js_set(js, buffer_proto, "readUInt32BE", js_mkfun(js_buffer_readUInt32BE));
  
  js_set_sym(js, buffer_proto, get_toStringTag_sym(), js_mkstr(js, "Buffer", 6));
  js_set(js, buffer_proto, "values", js_get(js, typedarray_proto, "values"));
  js_set_sym(js, buffer_proto, get_iterator_sym(), js_get(js, buffer_proto, "values"));
  
  js_set(js, buffer_ctor_obj, "from", js_mkfun(js_buffer_from));
  js_set(js, buffer_ctor_obj, "alloc", js_mkfun(js_buffer_alloc));
  js_set(js, buffer_ctor_obj, "allocUnsafe", js_mkfun(js_buffer_allocUnsafe));
  js_set(js, buffer_ctor_obj, "allocUnsafeSlow", js_mkfun(js_buffer_allocUnsafeSlow));
  js_set(js, buffer_ctor_obj, "isBuffer", js_mkfun(js_buffer_isBuffer));
  js_set(js, buffer_ctor_obj, "isEncoding", js_mkfun(js_buffer_isEncoding));
  js_set(js, buffer_ctor_obj, "byteLength", js_mkfun(js_buffer_byteLength));
  js_set(js, buffer_ctor_obj, "concat", js_mkfun(js_buffer_concat));
  js_set(js, buffer_ctor_obj, "compare", js_mkfun(js_buffer_compare));
  js_define_species_getter(js, buffer_ctor_obj);

  js_set_slot(buffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_buffer_from));
  js_mkprop_fast(js, buffer_ctor_obj, "prototype", 9, buffer_proto);
  js_mkprop_fast(js, buffer_ctor_obj, "name", 4, ANT_STRING("Buffer"));
  js_set_descriptor(js, buffer_ctor_obj, "name", 4, 0);
  
  ant_value_t buffer_ctor = js_obj_to_func(buffer_ctor_obj);
  js_set(js, buffer_proto, "constructor", buffer_ctor);
  js_set_descriptor(js, buffer_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  js_set(js, glob, "Buffer", buffer_ctor);
}

void cleanup_buffer_module(void) {
  if (buffer_registry) {
    for (size_t i = 0; i < buffer_registry_count; i++) {
      if (buffer_registry[i]) free(buffer_registry[i]);
    }
    free(buffer_registry);
    buffer_registry = NULL;
    buffer_registry_count = 0;
    buffer_registry_cap = 0;
  }
  
  ta_metadata_bytes = 0;
}

size_t buffer_get_external_memory(void) {
  size_t total = ta_metadata_bytes;
  
  for (size_t i = 0; i < buffer_registry_count; i++) if (buffer_registry[i])
    total += sizeof(ArrayBufferData) + buffer_registry[i]->capacity;
  total += buffer_registry_cap * sizeof(ArrayBufferData *);
  
  return total;
}
