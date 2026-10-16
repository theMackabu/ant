// TODO: split into smaller modules per type (src/buffer/*.c) AND optimize

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "ant.h"
#include "cfunc.h"
#include "ascii.h"
#include "ptr.h"
#include "utf8.h"
#include "utils.h"
#include "index.h"
#include "errors.h"
#include "base64.h"
#include "internal.h"
#include "numbers.h"
#include "gc/roots.h"
#include "descriptors.h"

#include "silver/call.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "native_getter.h"
#include "modules/symbol.h"
#include "modules/iterator.h"

#define BUFFER_REGISTRY_INITIAL_CAP 64

// Node compatibility exports only
// Ant does not enforce these as allocation limits
#define BUFFER_COMPAT_MAX_LENGTH 4294967296.0
#define BUFFER_COMPAT_MAX_STRING_LENGTH 536870888.0

static void *ta_meta_alloc(ant_t *js, size_t size) {
  void *ptr = calloc(1, size);
  if (!ptr) return NULL;
  js->buffers.meta_bytes += size;
  return ptr;
}

static void ta_meta_free(ant_t *js, void *ptr, size_t size) {
  if (!ptr) return;
  js->buffers.meta_bytes = js->buffers.meta_bytes >= size ? js->buffers.meta_bytes - size : 0;
  free(ptr);
}

ArrayBufferData *buffer_get_arraybuffer_data(ant_value_t value) {
  if (!is_object_type(value) || buffer_is_dataview(value)) return NULL;
  return (ArrayBufferData *)js_get_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG);
}

TypedArrayData *buffer_get_typedarray_data(ant_value_t value) {
  if (vtype(value) == kTypeTypedArray)
    return (TypedArrayData *)js_gettypedarray(value);
  if (!is_object_type(value)) return NULL;
  return (TypedArrayData *)js_get_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);
}

static inline TypedArrayData *typedarray_live(ant_value_t value) {
  TypedArrayData *ta = buffer_get_typedarray_data(value);
  return Ant_Buffer_ViewIsLive(ta) ? ta : NULL;
}

static __attribute__((noinline, cold)) ant_value_t typedarray_this_error(ant_t *js, ant_value_t value) {
  return buffer_get_typedarray_data(value)
    ? js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform this operation on a detached ArrayBuffer")
    : js_mkerr_typed(js, JS_ERR_TYPE, "this is not a typed array");
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
  free_array_buffer_data(js, data);
}

static void typedarray_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  TypedArrayData *ta_data = (TypedArrayData *)js_get_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);
  if (!ta_data) return;
  js_clear_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);

  if (ta_data->buffer) free_array_buffer_data(js, ta_data->buffer);
  ta_meta_free(js, ta_data, sizeof(*ta_data));
}

static void dataview_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  DataViewData *dv_data = (DataViewData *)js_get_native(value, BUFFER_DATAVIEW_NATIVE_TAG);
  if (!dv_data) return;
  js_clear_native(value, BUFFER_DATAVIEW_NATIVE_TAG);

  if (dv_data->buffer) free_array_buffer_data(js, dv_data->buffer);
  ta_meta_free(js, dv_data, sizeof(*dv_data));
}

bool buffer_is_dataview(ant_value_t obj) {
  return js_check_brand(obj, BRAND_DATAVIEW);
}

bool buffer_is_binary_source(ant_value_t value) {
  if (vtype(value) == kTypeTypedArray) return true;
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
    Ant_Buffer_GetViewBytes(ta, out, len);
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
    Ant_Buffer_GetViewBytes(dv, out, len);
    return true;
  }

  return false;
}

bool buffer_typedarray_data_read_index(ant_t *js, const TypedArrayData *ta_data, size_t index, ant_value_t *out) {
  uint8_t *data; size_t byte_length;
  if (!out || !Ant_Buffer_GetViewBytes(ta_data, &data, &byte_length) || index >= ta_data->length) return false;

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

bool buffer_typedarray_read_index(ant_t *js, ant_value_t value, size_t index, ant_value_t *out) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(value);
  return buffer_typedarray_data_read_index(js, ta_data, index, out);
}

static bool advance_typedarray(ant_t *js, iterator_t *it, ant_value_t *out) {
  ant_value_t iter = it->iterator;
  ant_value_t ta_obj = js_get_slot(iter, SLOT_DATA);
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == kTypeNumber) ? (uint32_t)js_getnum(state_v) : 0;
  
  uint32_t kind = iter_state_kind(state);
  uint32_t idx  = iter_state_index(state);

  TypedArrayData *ta = buffer_get_typedarray_data(ta_obj);
  if (!Ant_Buffer_ViewIsLive(ta) || idx >= (uint32_t)ta->length) return false;
  
  ant_value_t value;
  if (!buffer_typedarray_data_read_index(js, ta, idx, &value)) return false;

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

  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, idx + 1)));
  return true;
}

static ant_value_t ta_iter_next(ant_params_t) {
  return js_iter_next_result(js, advance_typedarray);
}

static ant_value_t ta_values(ant_params_t) {
  if (!typedarray_live(js->this_val)) return typedarray_this_error(js, js->this_val);
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_slot_wb(js, iter, SLOT_DATA, js->this_val);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(ARR_ITER_VALUES, 0)));
  js_set_proto_init(iter, js->builtins.typedarray_iter_proto);
  return iter;
}

static ant_value_t ta_keys(ant_params_t) {
  if (!typedarray_live(js->this_val)) return typedarray_this_error(js, js->this_val);
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_slot_wb(js, iter, SLOT_DATA, js->this_val);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(ARR_ITER_KEYS, 0)));
  js_set_proto_init(iter, js->builtins.typedarray_iter_proto);
  return iter;
}

static ant_value_t ta_entries(ant_params_t) {
  if (!typedarray_live(js->this_val)) return typedarray_this_error(js, js->this_val);
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_slot_wb(js, iter, SLOT_DATA, js->this_val);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(ARR_ITER_ENTRIES, 0)));
  js_set_proto_init(iter, js->builtins.typedarray_iter_proto);
  return iter;
}

static inline size_t array_buffer_owned_bytes(const ArrayBufferData *data) {
  return data->storage == ARRAY_BUFFER_STORAGE_INLINE ? data->capacity : 0;
}

static void register_buffer(ant_t *js, ArrayBufferData *data) {
  if (js->buffers.count >= UINT32_MAX) return;
  
  if (js->buffers.count == js->buffers.cap) {
    size_t cap = js->buffers.cap ? (size_t)js->buffers.cap * 2 : BUFFER_REGISTRY_INITIAL_CAP;
    if (cap > UINT32_MAX) cap = UINT32_MAX;
    
    ArrayBufferData **entries = realloc(js->buffers.entries, cap * sizeof(*entries));
    if (!entries) return;
    
    js->buffers.entries = entries;
    js->buffers.cap = (uint32_t)cap;
  }
  
  js->buffers.entries[js->buffers.count++] = data;
  js->buffers.bytes += array_buffer_owned_bytes(data);
  data->registry_slot = js->buffers.count;
  
  gc_note_offheap_alloc(js, array_buffer_owned_bytes(data));
}

static void unregister_buffer(ant_t *js, ArrayBufferData *data) {
  if (data->registry_slot == 0) return;
  
  size_t idx = (size_t)data->registry_slot - 1;
  if (idx >= js->buffers.count || js->buffers.entries[idx] != data) return;
  
  ArrayBufferData *last = js->buffers.entries[--js->buffers.count];
  js->buffers.entries[idx] = last;
  last->registry_slot = (uint32_t)idx + 1;
  data->registry_slot = 0;
  js->buffers.bytes -= array_buffer_owned_bytes(data);
}

static inline ssize_t normalize_index(ssize_t idx, ssize_t len) {
  if (idx < 0) idx += len;
  if (idx < 0) return 0;
  if (idx > len) return len;
  return idx;
}

static inline size_t buffer_string_offset(ant_value_t value, size_t limit, size_t fallback) {
  if (vtype(value) != kTypeNumber) return fallback;
  double raw = js_getnum(value);
  if (!(raw > 0) || isnan(raw)) return 0;
  if (raw > (double)limit) return limit;
  return (size_t)raw;
}

static inline void buffer_string_bounds(
  ant_value_t start_arg, ant_value_t end_arg,
  size_t limit, size_t *start_out, size_t *len_out
) {
  size_t start = buffer_string_offset(start_arg, limit, 0);
  size_t end = buffer_string_offset(end_arg, limit, limit);
  if (end < start) end = start;
  *start_out = start;
  *len_out = end - start;
}

static ArrayBufferData *alloc_array_buffer_data(size_t length) {
  if (length > SIZE_MAX - sizeof(ArrayBufferData)) return NULL;
  ArrayBufferData *data = calloc(1, sizeof(ArrayBufferData) + length);
  if (!data) return NULL;
  
  data->data = (uint8_t *)(data + 1);
  data->length = length;
  data->capacity = length;
  data->ref_count = 1;
  data->storage = ARRAY_BUFFER_STORAGE_INLINE;
  return data;
}

ArrayBufferData *create_external_array_buffer_data(uint8_t *bytes, size_t length) {
  ArrayBufferData *data = calloc(1, sizeof(ArrayBufferData));
  if (!data) return NULL;
  
  data->data = bytes;
  data->length = length;
  data->capacity = length;
  return data;
}

ArrayBufferData *create_array_buffer_data(ant_t *js, size_t length) {
  ArrayBufferData *data = alloc_array_buffer_data(length);
  if (data) register_buffer(js, data);
  return data;
}

static ArrayBufferData *create_shared_array_buffer_data(size_t length) {
  ArrayBufferData *data = alloc_array_buffer_data(length);
  if (data) data->is_shared = 1;
  return data;
}

void free_array_buffer_data(ant_t *js, ArrayBufferData *data) {
  if (!data || --data->ref_count > 0) return;
  ArrayBufferData *owner = data->owner;
  if (!data->is_shared) unregister_buffer(js, data);
  free(data);
  free_array_buffer_data(js, owner);
}

void buffer_detach(ant_t *js, ArrayBufferData *data) {
  ArrayBufferData *owner = data->owner;
  if (data->storage != ARRAY_BUFFER_STORAGE_INLINE) data->capacity = 0;
  if (owner) data->storage = ARRAY_BUFFER_STORAGE_INLINE;
  
  data->owner = NULL;
  data->data = NULL;
  data->length = 0;
  data->is_detached = 1;
  free_array_buffer_data(js, owner);
}

bool buffer_can_transfer(const ArrayBufferData *data) {
  return data && !data->is_shared && !data->is_detached && data->storage != ARRAY_BUFFER_STORAGE_EXTERNAL;
}

void buffer_transfer_storage(ant_t *js, ArrayBufferData *from, ArrayBufferData *to) {
  ArrayBufferData *owner = from->owner ? from->owner : from;
  owner->ref_count++;
  
  to->owner = owner;
  to->data = from->data;
  to->length = from->length;
  to->capacity = from->capacity;
  to->storage = ARRAY_BUFFER_STORAGE_BORROWED;
  
  buffer_detach(js, from);
}

static size_t get_element_size(TypedArrayType type) {
  static const void *dispatch[] = {
    &&L_1, &&L_1, &&L_1, &&L_2, &&L_2,
    &&L_4, &&L_4, &&L_2, &&L_4, &&L_8, &&L_8, &&L_8
  };
  
  if (type >= TYPED_ARRAY_COUNT) goto L_1;
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

static_assert(
  TYPED_ARRAY_COUNT == sizeof(((ant_t *)0)->mutable_roots.typedarray_names) / sizeof(ant_value_t),
  "resize typedarray_names in isolate_values.h"
);

static_assert(
  TYPED_ARRAY_COUNT == sizeof(((ant_t *)0)->builtins.typedarray_protos) / sizeof(ant_value_t),
  "resize typedarray_protos in isolate_values.h"
);

static ant_value_t js_typedarray_toStringTag_getter(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta = buffer_get_typedarray_data(this_val);
  if (!ta) return js_mkundef();

  ant_value_t *cached = &js->mutable_roots.typedarray_names[ta->type];
  if (*cached) return *cached;
  
  const char *name = buffer_typedarray_type_name(ta->type);
  ant_value_t str = js_mkstr(js, name, strlen(name));
  if (!is_err(str)) *cached = str;
  
  return str;
}

static ant_value_t create_typed_array_like(
  ant_t *js,
  ant_value_t this_val,
  TypedArrayType type,
  ArrayBufferData *buffer,
  size_t byte_offset,
  size_t length
) {
  TypedArrayData *source = buffer_get_typedarray_data(this_val);
  ant_value_t shared = source && source->buffer == buffer ? js_get_slot(this_val, SLOT_DATA) : js_mkundef();
  
  ant_value_t ab_obj = is_object_type(shared) ? shared : create_arraybuffer_obj(js, buffer);
  ant_value_t out = create_typed_array_with_buffer(
    js,type, buffer, byte_offset,
    length, js->builtins.typedarray_protos[type], ab_obj
  );
  
  if (is_err(out)) return out;
  ant_value_t proto = js_get_proto(js, this_val);
  if (is_special_object(proto)) js_set_proto_init(out, proto);
  
  return out;
}

static ant_value_t js_arraybuffer_constructor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "ArrayBuffer constructor requires 'new'");

  size_t length = 0;
  if (nargs > 0) {
    ant_value_t result = js_to_index_fast(js, args[0], &length);
    if (is_err(result)) return result;
  }
  
  ArrayBufferData *data = create_array_buffer_data(js, length);
  if (!data) return js_mkerr(js, "Failed to allocate ArrayBuffer");
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.arraybuffer_proto, call_new_target);

  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(obj, arraybuffer_finalize);

  return obj;
}

// ArrayBuffer.prototype.slice(begin, end)
static ant_value_t js_arraybuffer_slice(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  ArrayBufferData *data = buffer_get_arraybuffer_data(this_val);
  if (!data) return js_mkerr(js, "Invalid ArrayBuffer");
  if (data->is_detached) return js_mkerr(js, "Cannot slice a detached ArrayBuffer");
  
  ssize_t len = (ssize_t)data->length;
  ssize_t begin = 0, end = len;
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) end = (ssize_t)js_getnum(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  ArrayBufferData *new_data = data->is_shared
    ? create_shared_array_buffer_data(new_length)
    : create_array_buffer_data(js, new_length);
  
  if (!new_data) return js_mkerr(js, "Failed to allocate new ArrayBuffer");
  memcpy(new_data->data, data->data + begin, new_length);

  ant_value_t new_obj = Ant_Object_CreateBare(js);
  ant_value_t proto = data->is_shared ? js->builtins.sharedarraybuffer_proto : js->builtins.arraybuffer_proto;

  if (is_special_object(proto)) js_set_proto_init(new_obj, proto);
  js_set_native(new_obj, new_data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(new_obj, arraybuffer_finalize);
  
  return new_obj;
}

// ArrayBuffer.prototype.transfer(newLength)
static ant_value_t js_arraybuffer_transfer(ant_params_t) {
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
  if (nargs > 0 && vtype(args[0]) != kTypeUndefined) {
    ant_value_t result = js_to_index_fast(js, args[0], &new_length);
    if (is_err(result)) return result;
  }
  
  if (data->is_detached)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot transfer a detached ArrayBuffer");
  if (!buffer_can_transfer(data))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot transfer this ArrayBuffer");

  bool same_length = new_length == data->length;
  ArrayBufferData *new_data = create_array_buffer_data(js, same_length ? 0 : new_length);
  if (!new_data) return js_mkerr(js, "Failed to allocate new ArrayBuffer");
  
  if (same_length) buffer_transfer_storage(js, data, new_data);
  else {
    memcpy(new_data->data, data->data, data->length < new_length ? data->length : new_length);
    buffer_detach(js, data);
  }

  ant_value_t new_obj = create_arraybuffer_obj(js, new_data);
  free_array_buffer_data(js, new_data);
  return new_obj;
}

// ArrayBuffer.prototype.transferToFixedLength(newLength)
static ant_value_t js_arraybuffer_transferToFixedLength(ant_params_t) {
  return js_arraybuffer_transfer(js, args, nargs, js_mkundef());
}

// ArrayBuffer.prototype.detached getter
static ant_value_t js_arraybuffer_detached_getter(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  ArrayBufferData *data = buffer_get_arraybuffer_data(this_val);
  if (!data) return js_false;
  
  return js_bool(data->is_detached);
}

static ant_value_t js_arraybuffer_byteLength_getter(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  ArrayBufferData *data = buffer_get_arraybuffer_data(this_val);
  if (!data || data->is_detached) return js_mknum(0);
  return js_mknum((double)data->length);
}

static __attribute__((noinline, cold)) ant_value_t typedarray_receiver_error(ant_t *js, const char *getter) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Method get TypedArray.prototype.%s called on incompatible receiver", getter);
}

static ant_value_t js_typedarray_length_getter(ant_params_t) {
  TypedArrayData *data = buffer_get_typedarray_data(js_getthis(js));
  if (!data) return typedarray_receiver_error(js, "length");
  if (!Ant_Buffer_ViewIsLive(data)) return js_mknum(0);
  return js_mknum((double)data->length);
}

static ant_value_t js_typedarray_byteLength_getter(ant_params_t) {
  TypedArrayData *data = buffer_get_typedarray_data(js_getthis(js));
  if (!data) return typedarray_receiver_error(js, "byteLength");
  if (!Ant_Buffer_ViewIsLive(data)) return js_mknum(0);
  return js_mknum((double)data->byte_length);
}

static ant_value_t js_typedarray_byteOffset_getter(ant_params_t) {
  TypedArrayData *data = buffer_get_typedarray_data(js_getthis(js));
  if (!data) return typedarray_receiver_error(js, "byteOffset");
  if (!Ant_Buffer_ViewIsLive(data)) return js_mknum(0);
  return js_mknum((double)data->byte_offset);
}

static ant_value_t js_dataview_buffer_getter(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  DataViewData *dv = buffer_get_dataview_data(this_val);
  if (!dv) return js_mkerr_typed(js, JS_ERR_TYPE, "Not a DataView");
  return js_get_slot(this_val, SLOT_DATA);
}

static ant_value_t js_typedarray_buffer_getter(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  if (!buffer_get_typedarray_data(this_val)) return typedarray_receiver_error(js, "buffer");
  return js_get_slot(this_val, SLOT_DATA);
}

static ant_value_t dataview_live_view(ant_t *js, const char *getter, DataViewData **out) {
  *out = buffer_get_dataview_data(js_getthis(js));
  if (!*out) return js_mkerr_typed(js, JS_ERR_TYPE, "Not a DataView");
  if (Ant_Buffer_ViewIsLive(*out)) return js_mkundef();
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform get DataView.prototype.%s on a detached or out-of-bounds ArrayBuffer", getter);
}

static ant_value_t js_dataview_byteLength_getter(ant_params_t) {
  DataViewData *dv;
  ant_value_t error = dataview_live_view(js, "byteLength", &dv);
  return is_err(error) ? error : js_mknum((double)dv->byte_length);
}

static ant_value_t js_dataview_byteOffset_getter(ant_params_t) {
  DataViewData *dv;
  ant_value_t error = dataview_live_view(js, "byteOffset", &dv);
  return is_err(error) ? error : js_mknum((double)dv->byte_offset);
}

#define BUFFER_SIZE_GETTER(name, fn, tag, detach_mode, ref, flag, field) \
  static const ant_native_getter_t name = { \
    .meta = { fn, NULL, 0, CFUNC_NATIVE_GETTER }, .native_tag = tag, .kind = ANT_NATIVE_GETTER_SIZE, \
    .detach = detach_mode, .ref_offset = ref, .flag_offset = flag, .field_offset = field }

#define BUFFER_SLOT_GETTER(name, fn, tag) \
  static const ant_native_getter_t name = { \
    .meta = { fn, NULL, 0, CFUNC_NATIVE_GETTER }, .native_tag = tag, .kind = ANT_NATIVE_GETTER_SLOT_DATA }

BUFFER_SIZE_GETTER(typedarray_length_getter, js_typedarray_length_getter, BUFFER_TYPEDARRAY_NATIVE_TAG,
  ANT_NATIVE_DETACH_REF, offsetof(TypedArrayData, buffer), offsetof(ArrayBufferData, is_detached), offsetof(TypedArrayData, length));
BUFFER_SIZE_GETTER(typedarray_byte_length_getter, js_typedarray_byteLength_getter, BUFFER_TYPEDARRAY_NATIVE_TAG,
  ANT_NATIVE_DETACH_REF, offsetof(TypedArrayData, buffer), offsetof(ArrayBufferData, is_detached), offsetof(TypedArrayData, byte_length));
BUFFER_SIZE_GETTER(typedarray_byte_offset_getter, js_typedarray_byteOffset_getter, BUFFER_TYPEDARRAY_NATIVE_TAG,
  ANT_NATIVE_DETACH_REF, offsetof(TypedArrayData, buffer), offsetof(ArrayBufferData, is_detached), offsetof(TypedArrayData, byte_offset));
BUFFER_SIZE_GETTER(dataview_byte_length_getter, js_dataview_byteLength_getter, BUFFER_DATAVIEW_NATIVE_TAG,
  ANT_NATIVE_DETACH_REF_SLOW, offsetof(DataViewData, buffer), offsetof(ArrayBufferData, is_detached), offsetof(DataViewData, byte_length));
BUFFER_SIZE_GETTER(dataview_byte_offset_getter, js_dataview_byteOffset_getter, BUFFER_DATAVIEW_NATIVE_TAG,
  ANT_NATIVE_DETACH_REF_SLOW, offsetof(DataViewData, buffer), offsetof(ArrayBufferData, is_detached), offsetof(DataViewData, byte_offset));
BUFFER_SIZE_GETTER(arraybuffer_byte_length_getter, js_arraybuffer_byteLength_getter, BUFFER_ARRAYBUFFER_NATIVE_TAG,
  ANT_NATIVE_DETACH_SELF, 0, offsetof(ArrayBufferData, is_detached), offsetof(ArrayBufferData, length));
BUFFER_SIZE_GETTER(sharedarraybuffer_byte_length_getter, js_arraybuffer_byteLength_getter, BUFFER_ARRAYBUFFER_NATIVE_TAG,
  ANT_NATIVE_DETACH_SELF, 0, offsetof(ArrayBufferData, is_detached), offsetof(ArrayBufferData, length));
BUFFER_SLOT_GETTER(typedarray_buffer_getter, js_typedarray_buffer_getter, BUFFER_TYPEDARRAY_NATIVE_TAG);
BUFFER_SLOT_GETTER(dataview_buffer_getter, js_dataview_buffer_getter, BUFFER_DATAVIEW_NATIVE_TAG);

// ArrayBuffer.isView(value)
static ant_value_t js_arraybuffer_isView(ant_params_t) {
  if (nargs < 1) return js_false;
  return js_bool(buffer_is_dataview(args[0]) || buffer_get_typedarray_data(args[0]) != NULL);
}

static ant_value_t buffer_unwrap_bigint(ant_value_t value) {
  if (!is_object_type(value)) return value;
  ant_value_t primitive = js_get_slot(value, SLOT_PRIMITIVE);
  return vtype(primitive) == kTypeBigInt ? primitive : value;
}

static ant_value_t buffer_require_bigint_value(ant_t *js, ant_value_t value) {
  value = buffer_unwrap_bigint(value);
  return vtype(value) == kTypeBigInt ? value : js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
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
  uint8_t *data; size_t byte_length;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &byte_length) || index >= ta_data->length) {
    return js_mkundef();
  }

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

static constexpr size_t TYPEDARRAY_EXACT_DIGITS_MAX = 15;

typedef enum {
  TYPEDARRAY_KEY_ORDINARY,
  TYPEDARRAY_KEY_INDEX,
  TYPEDARRAY_KEY_INVALID,
} typedarray_key_kind_t;

static typedarray_key_kind_t typedarray_key_kind(const TypedArrayData *ta, const char *key, size_t len, size_t *index) {
  bool live = Ant_Buffer_ViewIsLive(ta);
  bool digits = len <= TYPEDARRAY_EXACT_DIGITS_MAX && (key[0] != '0' || len == 1);
  uint64_t whole = 0;
  
  for (size_t i = 0; i < len && digits; i++) {
    digits = key[i] >= '0' && key[i] <= '9';
    whole = whole * 10 + (uint64_t)(key[i] - '0');
  }
  
  if (digits) {
    if (!live || whole >= ta->length) return TYPEDARRAY_KEY_INVALID;
    *index = (size_t)whole;
    return TYPEDARRAY_KEY_INDEX;
  }
  
  if (len == 2 && key[0] == '-' && key[1] == '0') return TYPEDARRAY_KEY_INVALID;
  
  double number = 0;
  size_t processed = 0;
  if (!ant_number_parse(key, len, ANT_NUMBER_PARSE_JS_NUMBER, &number, &processed) || processed != len)
    return TYPEDARRAY_KEY_ORDINARY;
  
  char canonical[ANT_CANONICAL_NUMERIC_KEY_MAX + 8];
  size_t canonical_len = js_number_to_chars(tov(number), canonical, sizeof(canonical));
  return canonical_len == len && memcmp(canonical, key, len) == 0 ? TYPEDARRAY_KEY_INVALID : TYPEDARRAY_KEY_ORDINARY;
}

static ant_value_t typedarray_coerce_value(ant_t *js, const TypedArrayData *ta, ant_value_t value) {
  if (ta->type == TYPED_ARRAY_BIGINT64 || ta->type == TYPED_ARRAY_BIGUINT64) return buffer_require_bigint_value(js, value);
  double number = js_to_number(js, value);
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : tov(number);
}

static ant_value_t typedarray_index_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  if (!ant_key_may_be_numeric(key, key_len)) return js_mkundef();
  TypedArrayData *ta_data = buffer_get_typedarray_data(obj);
  size_t index = 0;
  typedarray_key_kind_t kind = ta_data ? typedarray_key_kind(ta_data, key, key_len, &index) : TYPEDARRAY_KEY_ORDINARY;
  if (kind == TYPEDARRAY_KEY_ORDINARY) return js_mkundef();

  ant_value_t value;
  if (kind == TYPEDARRAY_KEY_INVALID || !buffer_typedarray_data_read_index(js, ta_data, index, &value)) return T_EMPTY;
  return value;
}

static bool typedarray_index_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value) {
  if (!ant_key_may_be_numeric(key, key_len)) return false;
  TypedArrayData *ta_data = buffer_get_typedarray_data(obj);
  size_t index = 0;
  typedarray_key_kind_t kind = ta_data ? typedarray_key_kind(ta_data, key, key_len, &index) : TYPEDARRAY_KEY_ORDINARY;
  if (kind == TYPEDARRAY_KEY_ORDINARY) return false;
  
  ant_value_t coerced = typedarray_coerce_value(js, ta_data, value);
  if (!is_err(coerced) && kind == TYPEDARRAY_KEY_INDEX) (void)typedarray_write_value(js, ta_data, index, coerced);
  return true;
}

static bool typedarray_index_deleter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  if (!ant_key_may_be_numeric(key, key_len)) return true;
  TypedArrayData *ta_data = buffer_get_typedarray_data(obj);
  size_t index = 0;
  return !ta_data || typedarray_key_kind(ta_data, key, key_len, &index) != TYPEDARRAY_KEY_INDEX;
}

static ant_value_t typedarray_index_definer(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t value, bool has_value, bool plain_data
) {
  if (!ant_key_may_be_numeric(key, key_len)) return js_mkundef();
  TypedArrayData *ta_data = buffer_get_typedarray_data(obj);
  size_t index = 0;
  typedarray_key_kind_t kind = ta_data ? typedarray_key_kind(ta_data, key, key_len, &index) : TYPEDARRAY_KEY_ORDINARY;
  if (kind == TYPEDARRAY_KEY_ORDINARY) return js_mkundef();
  if (kind == TYPEDARRAY_KEY_INVALID || !plain_data) return T_EMPTY;
  if (!has_value) return js_true;
  
  ant_value_t coerced = typedarray_coerce_value(js, ta_data, value);
  if (is_err(coerced)) return coerced;
  (void)typedarray_write_value(js, ta_data, index, coerced);
  return js_true;
}

static bool typedarray_read_number(const TypedArrayData *ta_data, size_t index, double *out) {
  uint8_t *data; size_t byte_length;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &byte_length) || index >= ta_data->length) return false;

  static const void *dispatch[] = {
    &&R_INT8, &&R_UINT8, &&R_UINT8, &&R_INT16, &&R_UINT16,
    &&R_INT32, &&R_UINT32, &&R_FLOAT16, &&R_FLOAT32, &&R_FLOAT64, &&R_FAIL, &&R_FAIL
  };

  if (ta_data->type >= TYPED_ARRAY_COUNT) goto R_FAIL;
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

static ant_value_t js_typedarray_every(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.every requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  ant_value_t callback = args[0];
  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();

  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return js_false;

    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (!js_truthy(js, result)) return js_false;
  }

  return js_true;
}

static ant_value_t js_typedarray_forEach(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.forEach requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return js_mkundef();
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, js_mkundef());
    if (is_err(result)) return result;
  }

  return js_mkundef();
}

static ant_value_t js_typedarray_some(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.some requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return js_false;
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return js_true;
  }

  return js_false;
}

static ant_value_t js_typedarray_find(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.find requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return js_mkundef();
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return value;
  }

  return js_mkundef();
}

static ant_value_t js_typedarray_findIndex(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.findIndex requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return js_mknum(-1);
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return js_mknum((double)i);
  }

  return js_mknum(-1);
}

static ant_value_t js_typedarray_map(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.map requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  ArrayBufferData *buffer = create_array_buffer_data(js, ta_data->byte_length);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  ant_value_t out = create_typed_array_like(js, this_val, ta_data->type, buffer, 0, ta_data->length);
  free_array_buffer_data(js, buffer);
  
  if (is_err(out)) return out;
  TypedArrayData *out_ta = buffer_get_typedarray_data(out);

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return out;
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t mapped = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, js_mkundef());
    if (is_err(mapped)) return mapped;
    ant_value_t write_result = typedarray_write_value(js, out_ta, i, mapped);
    if (is_err(write_result)) return write_result;
  }

  return out;
}

static ant_value_t js_typedarray_filter(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.filter requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  gc_temp_root_scope_t kept;
  gc_temp_root_scope_begin(js, &kept);

  ant_value_t this_arg = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t out = js_mkundef();
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) continue;
    ant_value_t call_args[3] = { value, js_mknum((double)i), this_val };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, js_mkundef());
    if (is_err(result)) { out = result; goto done; }
    if (js_truthy(js, result) && !gc_temp_root_push(&kept, value)) { out = js_mkerr(js, "oom"); goto done; }
  }

  size_t count = kept.len;
  ArrayBufferData *buffer = create_array_buffer_data(js, count * get_element_size(ta_data->type));
  if (!buffer) { out = js_mkerr(js, "Failed to allocate buffer"); goto done; }
  
  out = create_typed_array_like(js, this_val, ta_data->type, buffer, 0, count);
  free_array_buffer_data(js, buffer);
  
  if (is_err(out)) goto done;
  TypedArrayData *out_ta = buffer_get_typedarray_data(out);

  for (size_t i = 0; i < count; i++) {
    ant_value_t write_result = typedarray_write_value(js, out_ta, i, kept.items[i]);
    if (is_err(write_result)) { out = write_result; goto done; }
  }

done:
  gc_temp_root_scope_end(&kept);
  return out;
}

static ant_value_t js_typedarray_reduce_common(ant_native_params_t, bool right) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.reduce requires a callable");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  if (ta_data->length == 0 && nargs < 2)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Reduce of empty TypedArray with no initial value");

  size_t i = right ? ta_data->length : 0;
  ant_value_t acc = nargs > 1 ? args[1] : js_mkundef();
  if (nargs < 2) {
    if (right) i--;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &acc)) return js_mkundef();
    if (!right) i++;
  }

  while (right ? (i > 0) : (i < ta_data->length)) {
    if (right) i--;
    ant_value_t value = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return acc;
    ant_value_t call_args[4] = { acc, value, js_mknum((double)i), this_val };
    acc = sv_vm_call(js->vm, js, args[0], js_mkundef(), call_args, 4, NULL, js_mkundef());
    if (is_err(acc)) return acc;
    if (!right) i++;
  }

  return acc;
}

static ant_value_t js_typedarray_reduce(ant_params_t) {
  return js_typedarray_reduce_common(js, args, nargs, false);
}

static ant_value_t js_typedarray_reduceRight(ant_params_t) {
  return js_typedarray_reduce_common(js, args, nargs, true);
}

ant_value_t create_arraybuffer_obj(ant_t *js, ArrayBufferData *buffer) {
  ant_value_t ab_obj = Ant_Object_CreateBare(js);
  ant_value_t ab_proto = js->builtins.arraybuffer_proto;
  if (is_special_object(ab_proto)) js_set_proto_init(ab_obj, ab_proto);
  
  js_set_native(ab_obj, buffer, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(ab_obj, arraybuffer_finalize);
  buffer->ref_count++;
  
  return ab_obj;
}

ant_value_t create_arraybuffer_copy(ant_t *js, const void *bytes, size_t length) {
  ArrayBufferData *buffer = create_array_buffer_data(js, length);
  if (!buffer) return js_mkerr(js, "out of memory");
  if (bytes && length) memcpy(buffer->data, bytes, length);
  
  ant_value_t ab_obj = create_arraybuffer_obj(js, buffer);
  free_array_buffer_data(js, buffer);
  
  return ab_obj;
}

static const ant_exotic_ops_t typedarray_exotic_ops = {
  .getter = typedarray_index_getter,
  .setter = typedarray_index_setter,
  .deleter = typedarray_index_deleter,
  .definer = typedarray_index_definer,
  .keys = ANT_EXOTIC_KEYS_NUMERIC,
};

ant_value_t create_typed_array_with_buffer(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, ant_value_t proto, ant_value_t arraybuffer_obj
) {
  TypedArrayData *ta_data = ta_meta_alloc(js, sizeof(TypedArrayData));
  if (!ta_data) return js_mkerr(js, "Failed to allocate TypedArray");
  
  size_t element_size = get_element_size(type);
  ta_data->buffer = buffer;
  ta_data->type = type;
  ta_data->byte_offset = byte_offset;
  ta_data->byte_length = length * element_size;
  ta_data->length = length;
  buffer->ref_count++;
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  
  js_set_native(obj, ta_data, BUFFER_TYPEDARRAY_NATIVE_TAG);
  js_set_slot_wb(js, obj, SLOT_DATA, arraybuffer_obj);
  
  js_set_exotic_ops(obj, &typedarray_exotic_ops);
  js_set_finalizer(obj, typedarray_finalize);
  
  return obj;
}

ant_value_t create_typed_array(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, ant_value_t proto
) {
  ant_value_t ab_obj = create_arraybuffer_obj(js, buffer);
  ant_value_t result = create_typed_array_with_buffer(js, type, buffer, byte_offset, length, proto, ab_obj);
  free_array_buffer_data(js, buffer); return result;
}

ant_value_t create_dataview_with_buffer(
  ant_t *js, ArrayBufferData *buffer,
  size_t byte_offset, size_t byte_length,
  ant_value_t arraybuffer_obj
) {
  DataViewData *dv_data = ta_meta_alloc(js, sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");

  dv_data->buffer = buffer;
  dv_data->byte_offset = byte_offset;
  dv_data->byte_length = byte_length;
  buffer->ref_count++;

  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js->builtins.dataview_proto;
  if (is_special_object(proto)) js_set_proto_init(obj, proto);

  js_set_native(obj, dv_data, BUFFER_DATAVIEW_NATIVE_TAG);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_DATAVIEW));
  js_set_slot_wb(js, obj, SLOT_DATA, arraybuffer_obj);
  js_set_finalizer(obj, dataview_finalize);

  return obj;
}

static ant_value_t create_typed_array_for_length(
  ant_t *js, TypedArrayType type, size_t length, ant_value_t proto
) {
  size_t element_size = get_element_size(type);
  if (length > (SIZE_MAX - sizeof(ArrayBufferData)) / element_size)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid TypedArray length");

  ArrayBufferData *buffer = create_array_buffer_data(js, length * element_size);
  if (!buffer) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid TypedArray length");

  return create_typed_array(js, type, buffer, 0, length, proto);
}

static bool typedarray_dense_number_source(ant_value_t source) {
  if (!js_is_array(source)) return false;
  ant_object_t *obj = js_obj_ptr(js_as_obj(source));
  if (!obj->flags.fast_array || obj->flags.is_exotic ||
      obj->u.array.len > obj->u.array.cap || obj->u.array.len >= 0x10000000) return false;
  for (uint32_t i = 0; i < obj->u.array.len; i++)
    if (vtype(obj->u.array.data[i]) != kTypeNumber) return false;
  return true;
}

static ant_value_t create_typed_array_from_iterable(
  ant_t *js, TypedArrayType type, ant_value_t source,
  ant_value_t iter_fn, ant_value_t proto
) {
  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);
  
  ant_value_t result = sv_vm_call(js->vm, js, iter_fn, source, NULL, 0, NULL, js_mkundef());
  if (is_err(result)) goto done;
  
  if (!is_object_type(result)) {
    result = js_mkerr_typed(js, JS_ERR_TYPE, "Iterator must be an object");
    goto done;
  }
  
  ant_value_t iterator = result;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&roots, iterator))) goto oom;
  
  ant_value_t next = js_getprop_fallback(js, iterator, "next");
  if (is_err(next)) { result = next; goto done; }

  if (js_iter_is_array_values(iterator, next, source) && typedarray_dense_number_source(source)) {
    ant_object_t *array = js_obj_ptr(js_as_obj(source));
    size_t length = array->u.array.len;

    js_set_slot(iterator, SLOT_ITER_STATE, js_mknum(iter_state_pack(ARR_ITER_VALUES, (uint32_t)length)));
    result = create_typed_array_for_length(js, type, length, proto);
    if (is_err(result)) goto done;
    
    TypedArrayData *target = buffer_get_typedarray_data(result);
    for (size_t i = 0; i < length; i++) {
      ant_value_t written = typedarray_write_value(js, target, i, array->u.array.data[i]);
      if (is_err(written)) { result = written; goto done; }
    }
    
    goto done;
  }
  
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&roots, next))) goto oom;
  size_t values_start = roots.len;

  while (true) {
    result = sv_vm_call(js->vm, js, next, iterator, NULL, 0, NULL, js_mkundef());
    if (is_err(result)) goto done;
    
    if (!is_object_type(result)) {
      result = js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result must be an object");
      goto done;
    }
    
    gc_temp_root_handle_t step = gc_temp_root_add(&roots, result);
    if (!gc_temp_root_handle_valid(step)) goto oom;
    
    ant_value_t done_value = js_getprop_fallback(js, result, "done");
    if (is_err(done_value)) { result = done_value; goto done; }
    
    if (js_truthy(js, done_value)) {
      gc_temp_root_truncate(&roots, step.index);
      break;
    }
    
    result = js_getprop_fallback(js, result, "value");
    if (is_err(result)) goto done;
    gc_temp_root_set(step, result);
  }

  size_t length = roots.len - values_start;
  result = create_typed_array_for_length(js, type, length, proto);
  
  if (is_err(result)) goto done;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&roots, result))) goto oom;
  
  TypedArrayData *target = buffer_get_typedarray_data(result);
  for (size_t i = 0; i < length; i++) {
    ant_value_t written = typedarray_write_value(js, target, i, roots.items[values_start + i]);
    if (is_err(written)) { result = written; goto done; }
  }

done:
  gc_temp_root_scope_end(&roots);
  return result;
oom:
  result = js_mkerr(js, "Failed to allocate TypedArray iterator values");
  goto done;
}

static ant_value_t js_typedarray_constructor(ant_native_params_t, TypedArrayType type, ant_value_t proto) {
  if (nargs == 0) return create_typed_array_for_length(js, type, 0, proto);

  ArrayBufferData *arraybuffer = buffer_get_arraybuffer_data(args[0]);
  if (arraybuffer) {
    ArrayBufferData *buffer = arraybuffer;
    size_t byte_offset = 0;
    size_t length = buffer->length;
    size_t element_size = get_element_size(type);

    if (nargs > 1) {
      ant_value_t result = js_to_index_fast(js, args[1], &byte_offset);
      if (is_err(result)) return result;
    }

    if (byte_offset % element_size != 0)
      return js_mkerr_typed(js, JS_ERR_RANGE, "Start offset is not aligned");

    bool has_length = nargs > 2 && vtype(args[2]) != kTypeUndefined;
    if (has_length) {
      ant_value_t result = js_to_index_fast(js, args[2], &length);
      if (is_err(result)) return result;
    }

    if (buffer->is_detached)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot create a view of a detached ArrayBuffer");
    if (byte_offset > buffer->length)
      return js_mkerr_typed(js, JS_ERR_RANGE, "Start offset is outside the bounds of the buffer");

    size_t available = buffer->length - byte_offset;
    if (has_length) {
      if (length > available / element_size) 
        return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid TypedArray length");
    } else {
      if (available % element_size != 0) 
        return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid TypedArray length");
      length = available / element_size;
    }

    return create_typed_array_with_buffer(js, type, buffer, byte_offset, length, proto, args[0]);
  }

  uint8_t arg_type = vtype(args[0]);
  if (is_boxable_primitive_type(arg_type) ||
      arg_type == kTypeNull || arg_type == kTypeUndefined) {
    size_t length = 0;
    ant_value_t result = js_to_index_fast(js, args[0], &length);
    if (is_err(result)) return result;
    return create_typed_array_for_length(js, type, length, proto);
  }

  TypedArrayData *source = buffer_get_typedarray_data(args[0]);
  if (source) {
    if (!Ant_Buffer_ViewIsLive(source))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot copy a detached TypedArray");
    
    bool source_bigint = source->type == TYPED_ARRAY_BIGINT64 || source->type == TYPED_ARRAY_BIGUINT64;
    bool target_bigint = type == TYPED_ARRAY_BIGINT64 || type == TYPED_ARRAY_BIGUINT64;
    
    if (source_bigint != target_bigint)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot mix BigInt and Number TypedArrays");
    
    ant_value_t result = create_typed_array_for_length(js, type, source->length, proto);
    if (is_err(result)) return result;
    
    TypedArrayData *target = buffer_get_typedarray_data(result);
    if (source->type == type) memcpy(
      target->buffer->data, 
      source->buffer->data + source->byte_offset, 
      source->byte_length
    ); 
    else for (size_t i = 0; i < source->length; i++) {
      ant_value_t value = js_mkundef();
      buffer_typedarray_data_read_index(js, source, i, &value);
      ant_value_t written = typedarray_write_value(js, target, i, value);
      if (is_err(written)) return written;
    }
    
    return result;
  }

  if (is_special_object(args[0])) {
    ant_value_t iter_fn = js_get_sym(js, args[0], js->sym.iterator_sym);
    if (is_err(iter_fn)) return iter_fn;
    
    if (vtype(iter_fn) != kTypeUndefined && vtype(iter_fn) != kTypeNull) {
      if (!is_callable(iter_fn)) return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray source iterator is not callable");
      return create_typed_array_from_iterable(js, type, args[0], iter_fn, proto);
    }

    size_t length = 0;
    ant_value_t len_val = js_get(js, args[0], "length");
    if (is_err(len_val)) return len_val;
    
    ant_value_t result = js_to_length_fast(js, len_val, &length);
    if (is_err(result)) return result;
    
    result = create_typed_array_for_length(js, type, length, proto);
    if (is_err(result)) return result;
    
    TypedArrayData *result_ta = buffer_get_typedarray_data(result);
    for (size_t i = 0; i < length; i++) {
      char idx_str[24];
      snprintf(idx_str, sizeof(idx_str), "%zu", i);
      ant_value_t elem = js_get(js, args[0], idx_str);
      if (is_err(elem)) return elem;
      ant_value_t written = typedarray_write_value(js, result_ta, i, elem);
      if (is_err(written)) return written;
    }
    
    return result;
  }

  return js_mkerr(js, "Invalid TypedArray constructor arguments");
}

static ant_value_t js_typedarray_base_constructor(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray constructor is abstract");
}

// TypedArray.prototype.slice(begin, end)
// TypedArray.prototype.at(index)
static ant_value_t js_typedarray_at(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  
  if (nargs == 0 || vtype(args[0]) != kTypeNumber) return js_mkundef();
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t idx = (ssize_t)js_getnum(args[0]);
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) return js_mkundef();
  if (!Ant_Buffer_ViewIsLive(ta_data)) return js_mkundef();
  
  ant_value_t value;
  if (!buffer_typedarray_data_read_index(js, ta_data, (size_t)idx, &value)) return js_mkundef();
  return value;
}

static ant_value_t js_typedarray_slice(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t begin = 0, end = len;
  
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) end = (ssize_t)js_getnum(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(js, new_length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(
    new_buffer->data, 
    ta_data->buffer->data + ta_data->byte_offset + (size_t)begin * element_size,
    new_length * element_size
  );
  
  ant_value_t out = create_typed_array_like(
    js, this_val, ta_data->type, 
    new_buffer, 0, new_length
  ); free_array_buffer_data(js, new_buffer);
  
  return out;
}

// TypedArray.prototype.subarray(begin, end)
static ant_value_t js_typedarray_subarray(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t begin = 0, end = len;
  
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) end = (ssize_t)js_getnum(args[1]);
  
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
static ant_value_t js_typedarray_fill(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  
  ant_value_t value = nargs > 0 ? args[0] : js_mknum(0);
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t start = 0, end = len;
  
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) start = (ssize_t)js_getnum(args[1]);
  if (nargs > 2 && vtype(args[2]) == kTypeNumber) end = (ssize_t)js_getnum(args[2]);
  
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
  
  if (ta_data->type >= TYPED_ARRAY_COUNT) goto L_DONE;
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
static ant_value_t js_typedarray_set(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "set requires source argument");

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *dst = typedarray_live(this_val);
  if (!dst) return typedarray_this_error(js, this_val);

  ssize_t offset_i = 0;
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) offset_i = (ssize_t)js_getnum(args[1]);
  if (offset_i < 0) return js_mkerr(js, "Offset out of bounds");
  size_t offset = (size_t)offset_i;
  if (offset > dst->length) return js_mkerr(js, "Offset out of bounds");

  ant_value_t src_val = args[0];
  TypedArrayData *src_ta = buffer_get_typedarray_data(src_val);
  if (src_ta && !typedarray_live(src_val)) return typedarray_this_error(js, src_val);

  if (src_ta) {
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
      if (!buffer_typedarray_data_read_index(js, src_ta, i, &value))
        return js_mkerr(js, "Failed to read from source TypedArray");
      ant_value_t write_result = typedarray_write_value(js, dst, offset + i, value);
      if (is_err(write_result)) return write_result;
    }
    return js_mkundef();
  }

  if (!is_special_object(src_val) && vtype(src_val) != kTypeString) {
    return js_mkerr(js, "set source must be array-like or TypedArray");
  }

  size_t src_len = 0;
  if (vtype(src_val) == kTypeString) {
    src_len = (size_t)vstrlen(js, src_val);
  } else {
    ant_value_t len_val = js_get(js, src_val, "length");
    src_len = vtype(len_val) == kTypeNumber ? (size_t)js_getnum(len_val) : 0;
  }

  if (offset + src_len > dst->length) return js_mkerr(js, "Source is too large");

  for (size_t i = 0; i < src_len; i++) {
    if (vtype(src_val) == kTypeString) {
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
static ant_value_t js_typedarray_copyWithin(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta = typedarray_live(this_val);
  if (!ta) return typedarray_this_error(js, this_val);

  ssize_t len = (ssize_t)ta->length;
  if (len <= 0) return this_val;

  ssize_t target = 0, start = 0, end = len;
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) target = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) start = (ssize_t)js_getnum(args[1]);
  if (nargs > 2 && vtype(args[2]) == kTypeNumber) end = (ssize_t)js_getnum(args[2]);

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
static ant_value_t js_typedarray_toReversed(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  
  size_t length = ta_data->length;
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(js, length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  uint8_t *src = ta_data->buffer->data + ta_data->byte_offset;
  uint8_t *dst = new_buffer->data;
  
  for (size_t i = 0; i < length; i++) {
    memcpy(dst + i * element_size, src + (length - 1 - i) * element_size, element_size);
  }
  
  ant_value_t out = create_typed_array_like(
    js, this_val, ta_data->type,
    new_buffer, 0, length
  ); free_array_buffer_data(js, new_buffer);
  
  return out;
}

// TypedArray.prototype.toSorted(comparefn)
static ant_value_t js_typedarray_toSorted(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  
  size_t length = ta_data->length;
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(js, length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(new_buffer->data, ta_data->buffer->data + ta_data->byte_offset, length * element_size);
  ant_value_t result = create_typed_array_like(js, this_val, ta_data->type, new_buffer, 0, length);
  
  free_array_buffer_data(js, new_buffer);
  if (is_err(result)) return result;
  
  TypedArrayData *result_ta = buffer_get_typedarray_data(result);
  uint8_t *data = result_ta->buffer->data;
  
  ant_value_t comparefn = (nargs > 0 && vtype(args[0]) == kTypeFunction) ? args[0] : js_mkundef();
  bool has_comparefn = vtype(comparefn) == kTypeFunction;
  
  for (size_t i = 1; i < length; i++) {
    for (size_t j = i; j > 0; j--) {
      double a_val, b_val;
      int cmp;
      
      static const void *read_dispatch[] = {
        &&R_INT8, &&R_UINT8, &&R_UINT8, &&R_INT16, &&R_UINT16,
        &&R_INT32, &&R_UINT32, &&R_FLOAT16, &&R_FLOAT32, &&R_FLOAT64, &&R_DONE, &&R_DONE
      };
      
      if (ta_data->type >= TYPED_ARRAY_COUNT) goto R_DONE;
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
        ant_value_t cmp_result = sv_vm_call(js->vm, js, comparefn, js_mkundef(), cmp_args, 2, NULL, js_mkundef());
        cmp = (int)js_getnum(cmp_result);
      } else {
        cmp = (a_val > b_val) ? 1 : ((a_val < b_val) ? -1 : 0);
      }
      
      if (cmp <= 0) break;
      
      static const void *swap_dispatch[] = {
        &&S_INT8, &&S_UINT8, &&S_UINT8, &&S_INT16, &&S_UINT16,
        &&S_INT32, &&S_UINT32, &&S_FLOAT16, &&S_FLOAT32, &&S_FLOAT64, &&S_DONE, &&S_DONE
      };
      
      if (ta_data->type >= TYPED_ARRAY_COUNT) goto S_DONE;
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
static ant_value_t js_typedarray_with(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "with requires index and value");
  
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  
  ssize_t index = (ssize_t)js_getnum(args[0]);
  size_t length = ta_data->length;
  
  if (index < 0) index = (ssize_t)length + index;
  if (index < 0 || (size_t)index >= length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(js, length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(new_buffer->data, ta_data->buffer->data + ta_data->byte_offset, length * element_size);
  
  ant_value_t out = create_typed_array_like(
    js, this_val, ta_data->type,
    new_buffer, 0, length
  ); free_array_buffer_data(js, new_buffer);
  if (is_err(out)) return out;

  TypedArrayData *out_ta = buffer_get_typedarray_data(out);
  ant_value_t write_result = typedarray_write_value(js, out_ta, (size_t)index, args[1]);
  if (is_err(write_result)) return write_result;
  
  return out;
}

static ant_value_t js_typedarray_from(ant_native_params_t, TypedArrayType type, const char *type_name) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "%s.from requires at least 1 argument", type_name);

  ant_value_t source = args[0];
  bool has_map = nargs >= 2 && vtype(args[1]) != kTypeUndefined;
  
  ant_value_t map_fn = js_mkundef();
  ant_value_t this_arg = nargs >= 3 ? args[2] : js_mkundef();
  ant_value_t result = js_mkundef();
  ant_value_t *collected = NULL;
  
  gc_temp_root_scope_t temp_roots = {0};

  if (has_map) {
    if (!is_callable(args[1])) return js_mkerr_typed(
      js, JS_ERR_TYPE, "%s.from: mapFn is not a function", type_name
    );
    map_fn = args[1];
  }

  gc_temp_root_scope_begin(js, &temp_roots);
  
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, source))) goto oom;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, map_fn))) goto oom;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, this_arg))) goto oom;

  size_t count = 0, cap = 16;
  collected = malloc(cap * sizeof(ant_value_t));
  if (!collected) goto oom;

  iterator_t it;
  if (js_iter_open(js, source, &it)) {
    ant_value_t item;
    while (js_iter_next(js, &it, &item)) {
      if (count >= cap) {
        cap *= 2;
        ant_value_t *tmp = realloc(collected, cap * sizeof(ant_value_t));
        if (!tmp) {
          result = js_mkerr(js, "oom");
          break;
        }
        collected = tmp;
      }
      if (has_map) {
        ant_value_t map_args[2] = { item, js_mknum((double)count) };
        item = sv_vm_call(js->vm, js, map_fn, this_arg, map_args, 2, NULL, js_mkundef());
        if (is_err(item)) {
          result = item;
          break;
        }
      }
      collected[count++] = item;
      if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, item))) {
        result = js_mkerr(js, "oom");
        break;
      }
    }

    if (is_err(result)) Ant_Exception_Set(js, result);
    js_iter_close(js, &it);
  } else if (!Ant_Exception_Pending(js)) {
    ant_value_t len_val = js_get(js, source, "length");
    if (is_err(len_val)) {
      result = len_val;
      goto done;
    }

    size_t len = vtype(len_val) == kTypeNumber
      ? (size_t)js_getnum(len_val) : 0;

    for (size_t i = 0; i < len; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%zu", i);
      ant_value_t item = js_get(js, source, idx);
      if (is_err(item)) { result = item; goto done; }
      if (count >= cap) {
        cap *= 2;
        ant_value_t *tmp = realloc(collected, cap * sizeof(ant_value_t));
        if (!tmp) goto oom;
        collected = tmp;
      }
      if (has_map) {
        ant_value_t map_args[2] = { item, js_mknum((double)i) };
        item = sv_vm_call(js->vm, js, map_fn, this_arg, map_args, 2, NULL, js_mkundef());
        if (is_err(item)) {
          result = item;
          goto done;
        }
      }
      collected[count++] = item;
      if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, item))) goto oom;
    }
  }

  if (Ant_Exception_Pending(js)) {
    result = Ant_Exception_Current(js);
    goto done;
  }

  size_t elem_size = get_element_size(type);
  ArrayBufferData *buffer = create_array_buffer_data(js, count * elem_size);
  if (!buffer) goto oom;

  result = create_typed_array(js, type, buffer, 0, count, js->builtins.typedarray_protos[type]);
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
  gc_temp_root_scope_end(&temp_roots);
  free(collected);
  return result;

oom:
  result = js_mkerr(js, "oom");
  goto done;
}

static ant_value_t js_typedarray_of(ant_native_params_t, TypedArrayType type, ant_value_t proto) {
  size_t count = (size_t)nargs;
  size_t elem_size = get_element_size(type);
  ArrayBufferData *buffer = create_array_buffer_data(js, count * elem_size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");

  ant_value_t result = create_typed_array(js, type, buffer, 0, count, proto);
  if (is_err(result)) return result;

  TypedArrayData *result_ta = buffer_get_typedarray_data(result);
  for (int i = 0; i < nargs; i++) {
    ant_value_t write_result = typedarray_write_value(js, result_ta, (size_t)i, args[i]);
    if (is_err(write_result)) return write_result;
  }

  return result;
}

static ant_value_t js_typedarray_kind_constructor(ant_params_t) {
  TypedArrayType type = (TypedArrayType)js_getnum(js_get_slot(js->current_func, SLOT_AUX));
  const char *name = buffer_typedarray_type_name(type);
  
  if (vtype(call_new_target) == kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "%s constructor requires 'new'", name);
  ant_value_t result = js_typedarray_constructor(js, args, nargs, type, js->builtins.typedarray_protos[type]);
  if (is_err(result)) return result;
  
  ant_value_t proto = js_instance_proto_from_new_target(js, js_get_proto(js, result), call_new_target);
  if (is_special_object(proto)) js_set_proto_wb(js, result, proto);
  return result;
}

static bool typedarray_ctor_kind(ant_t *js, ant_value_t ctor, TypedArrayType *out) {
  ant_value_t cur = ctor;
  
  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    ant_value_t cfunc = js_get_slot(cur, SLOT_CFUNC);
    if (vtype(cfunc) == kTypeBuiltin && js_as_cfunc_meta(cfunc)->fn == js_typedarray_kind_constructor) {
      *out = (TypedArrayType)js_getnum(js_get_slot(cur, SLOT_AUX));
      return true;
    }
    
    cur = js_get_proto(js, cur);
  }
  
  return false;
}

static ant_value_t typedarray_adopt_this(ant_t *js, ant_value_t result) {
  if (is_err(result)) return result;
  ant_value_t proto = js_instance_proto_from_new_target(js, js_get_proto(js, result), js->this_val);
  if (is_special_object(proto)) js_set_proto_wb(js, result, proto);
  return result;
}

static ant_value_t js_typedarray_static_from(ant_params_t) {
  TypedArrayType type;
  if (!typedarray_ctor_kind(js, js->this_val, &type)) return js_mkerr_typed(js, JS_ERR_TYPE, "this is not a typed array constructor");
  return typedarray_adopt_this(js, js_typedarray_from(js, args, nargs, type, buffer_typedarray_type_name(type)));
}

static ant_value_t js_typedarray_static_of(ant_params_t) {
  TypedArrayType type;
  if (!typedarray_ctor_kind(js, js->this_val, &type)) return js_mkerr_typed(js, JS_ERR_TYPE, "this is not a typed array constructor");
  return typedarray_adopt_this(js, js_typedarray_of(js, args, nargs, type, js->builtins.typedarray_protos[type]));
}

static void setup_typedarray_kind(ant_t *js, TypedArrayType type, ant_value_t base_ctor, ant_value_t base_proto) {
  const char *name = buffer_typedarray_type_name(type);
  ant_value_t ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t proto = Ant_Object_CreateBare(js);
  ant_value_t elem_size = js_mknum((double)get_element_size(type));
  
  js_set_proto_init(ctor_obj, base_ctor);
  js_set_proto_init(proto, base_proto);
  js->builtins.typedarray_protos[type] = proto;
  js_set_slot(ctor_obj, SLOT_CFUNC, Ant_Function_Create(js_typedarray_kind_constructor));
  js_set_slot(ctor_obj, SLOT_AUX, js_mknum((double)type));
  
  mkprop(js, ctor_obj, js->length_str, js_mknum(3), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop(js, ctor_obj, ANT_STRING("name"), js_mkstr(js, name, strlen(name)), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop_exact_attrs(js, ctor_obj, ANT_STRING("prototype"), proto, 0);
  mkprop_exact_attrs(js, ctor_obj, ANT_STRING("BYTES_PER_ELEMENT"), elem_size, 0);
  
  ant_value_t ctor = js_obj_to_func(js, ctor_obj);
  mkprop(js, proto, ANT_STRING("constructor"), ctor, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);
  mkprop_exact_attrs(js, proto, ANT_STRING("BYTES_PER_ELEMENT"), elem_size, 0);
  js_set_global_builtin(js, name, ctor);
}

static ant_value_t js_dataview_constructor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "DataView constructor requires 'new'");
  if (nargs < 1) return js_mkerr(js, "DataView requires an ArrayBuffer");
  
  ArrayBufferData *buffer = buffer_get_arraybuffer_data(args[0]);
  if (!buffer) return js_mkerr(js, "First argument must be an ArrayBuffer");
  
  size_t byte_offset = 0;
  size_t byte_length = buffer->length;
  
  if (nargs > 1) {
    ant_value_t result = js_to_index_fast(js, args[1], &byte_offset);
    if (is_err(result)) return result;
  }
  
  if (buffer->is_detached)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot create a view of a detached ArrayBuffer");
  
  size_t buffer_length = buffer->length;
  if (byte_offset > buffer_length)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Start offset is outside the bounds of the buffer");
  
  if (nargs > 2 && vtype(args[2]) != kTypeUndefined) {
    ant_value_t result = js_to_index_fast(js, args[2], &byte_length);
    if (is_err(result)) return result;
    if (byte_length > buffer_length - byte_offset)
      return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid DataView length");
  } else byte_length = buffer_length - byte_offset;
  
  if (buffer->is_detached)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot create a view of a detached ArrayBuffer");

  DataViewData *dv_data = ta_meta_alloc(js, sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");
  
  dv_data->buffer = buffer;
  dv_data->byte_offset = byte_offset;
  dv_data->byte_length = byte_length;
  buffer->ref_count++;
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.dataview_proto, call_new_target);
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  
  js_set_native(obj, dv_data, BUFFER_DATAVIEW_NATIVE_TAG);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_DATAVIEW));
  js_set_slot_wb(js, obj, SLOT_DATA, args[0]);
  js_set_finalizer(obj, dataview_finalize);
  
  return obj;
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

static uint8_t *hex_decode_node_prefix(const char *data, size_t len, size_t *out_len) {
  size_t max_len = len / 2;
  uint8_t *decoded = malloc(max_len == 0 ? 1 : max_len);
  if (!decoded) return NULL;

  size_t count = 0;
  for (size_t i = 0; i + 1 < len; i += 2) {
    int hi = ant_ascii_hex_value((unsigned char)data[i]);
    int lo = ant_ascii_hex_value((unsigned char)data[i + 1]);
    if (hi < 0 || lo < 0) break;
    decoded[count++] = (uint8_t)((hi << 4) | lo);
  }

  *out_len = count;
  return decoded;
}

static ant_value_t uint8array_from_bytes(ant_t *js, const uint8_t *bytes, size_t len) {
  ArrayBufferData *buffer = create_array_buffer_data(js, len);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  if (len > 0) memcpy(buffer->data, bytes, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, js->builtins.typedarray_protos[TYPED_ARRAY_UINT8]);
}

static ant_value_t js_uint8array_fromHex(ant_params_t) {
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

static bool base64_option_is(ant_t *js, ant_value_t value, const char *name) {
  size_t len = 0;
  const char *str = vtype(value) == kTypeString ? js_getstr(js, value, &len) : NULL;
  return str && len == strlen(name) && memcmp(str, name, len) == 0;
}

static ant_value_t base64_options(
  ant_t *js, ant_value_t options, bool *url, 
  ant_base64_last_chunk_t *last_chunk, bool *omit_padding
) {
  *url = false;
  
  if (last_chunk) *last_chunk = ANT_BASE64_LOOSE;
  if (omit_padding) *omit_padding = false;
  
  if (vtype(options) == kTypeUndefined) return js_mkundef();
  if (!is_object_type(options)) return js_mkerr_typed(js, JS_ERR_TYPE, "options must be an object");

  ant_value_t alphabet = js_get(js, options, "alphabet");
  if (base64_option_is(js, alphabet, "base64url")) *url = true;
  else if (vtype(alphabet) != kTypeUndefined && !base64_option_is(js, alphabet, "base64"))
    return js_mkerr_typed(js, JS_ERR_TYPE, "alphabet must be \"base64\" or \"base64url\"");

  if (last_chunk) {
    ant_value_t handling = js_get(js, options, "lastChunkHandling");
    if (base64_option_is(js, handling, "strict")) *last_chunk = ANT_BASE64_STRICT;
    else if (base64_option_is(js, handling, "stop-before-partial")) *last_chunk = ANT_BASE64_STOP_BEFORE_PARTIAL;
    else if (vtype(handling) != kTypeUndefined && !base64_option_is(js, handling, "loose"))
      return js_mkerr_typed(js, JS_ERR_TYPE, "lastChunkHandling must be \"loose\", \"strict\", or \"stop-before-partial\"");
  }

  if (omit_padding) *omit_padding = js_truthy(js, js_get(js, options, "omitPadding"));
  return js_mkundef();
}

static ant_value_t base64_source_arg(ant_t *js, ant_value_t *args, int nargs, const char **str, size_t *len) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return js_mkerr_typed(js, JS_ERR_TYPE, "input argument must be a string");
  *str = js_getstr(js, args[0], len);
  return js_mkundef();
}

static ant_value_t js_uint8array_fromBase64(ant_params_t) {
  const char *str; size_t len;
  ant_value_t checked = base64_source_arg(js, args, nargs, &str, &len);
  if (is_err(checked)) return checked;

  bool url; ant_base64_last_chunk_t last_chunk;
  checked = base64_options(js, nargs > 1 ? args[1] : js_mkundef(), &url, &last_chunk, NULL);
  if (is_err(checked)) return checked;

  size_t cap = (len / 4) * 3 + 3, read = 0, written = 0;
  uint8_t *decoded = malloc(cap);
  if (!decoded) return js_mkerr(js, "Failed to allocate buffer");

  const char *error = NULL;
  if (!ant_base64_decode_into(str, len, url, last_chunk, decoded, cap, &read, &written, &error)) {
    free(decoded);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "%s", error);
  }

  ant_value_t result = uint8array_from_bytes(js, decoded, written);
  free(decoded);
  
  return result;
}

static ant_value_t js_uint8array_toHex(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array.prototype.toHex called on incompatible receiver");
  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &len))
    return js_mkerr(js, "Cannot read from detached Uint8Array");

  char *hex = malloc(len * 2 + 1);
  
  if (!hex) return js_mkerr(js, "Failed to allocate hex string");
  hex_encode(data, len, hex);

  ant_value_t result = js_mkstr(js, hex, len * 2);
  free(hex);
  
  return result;
}

static ant_value_t js_uint8array_toBase64(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array.prototype.toBase64 called on incompatible receiver");
  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &len))
    return js_mkerr(js, "Cannot read from detached Uint8Array");

  bool url, omit_padding;
  ant_value_t checked = base64_options(js, nargs > 0 ? args[0] : js_mkundef(), &url, NULL, &omit_padding);
  if (is_err(checked)) return checked;

  size_t out_len = 0;
  char *encoded = ant_base64_encode(data, len, &out_len);
  if (!encoded) return js_mkerr(js, "Failed to encode base64");

  if (url) for (size_t i = 0; i < out_len; i++) {
    if (encoded[i] == '+') encoded[i] = '-';
    else if (encoded[i] == '/') encoded[i] = '_';
  }
  if (omit_padding) while (out_len > 0 && encoded[out_len - 1] == '=') out_len--;

  ant_value_t result = js_mkstr(js, encoded, out_len);
  free(encoded);
  return result;
}

static ant_value_t uint8array_set_result(ant_t *js, size_t read, size_t written) {
  ant_value_t result = Ant_Object_Create(js);
  js_set(js, result, "read", js_mknum((double)read));
  js_set(js, result, "written", js_mknum((double)written));
  return result;
}

static ant_value_t uint8array_set_bytes(ant_t *js, const uint8_t *bytes, size_t byte_len, size_t read) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array setFrom called on incompatible receiver");
  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &len))
    return js_mkerr(js, "Cannot write to detached Uint8Array");
  if (byte_len > len)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Decoded data does not fit in Uint8Array");

  if (byte_len > 0) memcpy(data, bytes, byte_len);
  return uint8array_set_result(js, read, byte_len);
}

static ant_value_t js_uint8array_setFromHex(ant_params_t) {
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

static ant_value_t js_uint8array_setFromBase64(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(js_getthis(js));
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array setFrom called on incompatible receiver");

  const char *str; size_t len;
  ant_value_t checked = base64_source_arg(js, args, nargs, &str, &len);
  if (is_err(checked)) return checked;

  bool url; ant_base64_last_chunk_t last_chunk;
  checked = base64_options(js, nargs > 1 ? args[1] : js_mkundef(), &url, &last_chunk, NULL);
  if (is_err(checked)) return checked;

  uint8_t *data; size_t cap;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &cap)) return js_mkerr(js, "Cannot write to detached Uint8Array");

  size_t read = 0, written = 0;
  const char *error = NULL;
  if (!ant_base64_decode_into(str, len, url, last_chunk, data, cap, &read, &written, &error))
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "%s", error);
  return uint8array_set_result(js, read, written);
}

typedef enum {
  ENC_UTF8,
  ENC_HEX,
  ENC_BASE64,
  ENC_BASE64URL,
  ENC_ASCII,
  ENC_LATIN1,
  ENC_UCS2,
  ENC_UNKNOWN
} BufferEncoding;

static BufferEncoding parse_encoding(const char *enc, size_t len) {
  if (len == 3 && strncasecmp(enc, "hex", 3) == 0) return ENC_HEX;
  if (len == 5 && strncasecmp(enc, "ascii", 5) == 0) return ENC_ASCII;
  if (len == 6 && strncasecmp(enc, "base64", 6) == 0) return ENC_BASE64;
  if (len == 9 && strncasecmp(enc, "base64url", 9) == 0) return ENC_BASE64URL;
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

static ant_value_t buffer_unknown_encoding_error(ant_t *js, ant_value_t encoding) {
  ant_value_t str = js_tostring_val(js, encoding);
  if (is_err(str)) return str;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  
  size_t len = 0;
  const char *enc = js_getstr(js, str, &len);
  ant_value_t props = Ant_Object_CreateBare(js);
  
  js_set(js, props, "code", js_mkstr(js, "ERR_UNKNOWN_ENCODING", 20));
  ant_value_t err = js_mkerr_props(js, JS_ERR_TYPE, props, "Unknown encoding: %.*s", (int)len, enc ? enc : "");
  
  GC_ROOT_RESTORE(js, root_mark);
  return err;
}

static ant_value_t buffer_require_encoding(ant_t *js, ant_value_t value, BufferEncoding *out) {
  ant_value_t str = vtype(value) == kTypeString ? value : js_tostring_val(js, value);
  if (is_err(str)) return str;
  
  size_t len = 0;
  const char *enc = js_getstr(js, str, &len);
  *out = enc ? parse_encoding(enc, len) : ENC_UNKNOWN;
  
  if (*out == ENC_UNKNOWN) return buffer_unknown_encoding_error(js, str);
  return js_mkundef();
}

static size_t buffer_encode_units_into(const char *str, size_t len, uint8_t *dst, size_t cap, bool wide) {
  if (!wide && str_is_ascii(str)) {
    size_t written = len < cap ? len : cap;
    memcpy(dst, str, written);
    return written;
  }

  utf16_iterator_t iterator = utf16_iterator(str, len);
  size_t written = 0, step = wide ? 2 : 1;
  uint16_t unit;
  while (cap - written >= step && utf16_next_code_unit(&iterator, &unit)) {
    dst[written++] = (uint8_t)unit;
    if (wide) dst[written++] = (uint8_t)(unit >> 8);
  }
  return written;
}

static ant_value_t buffer_decode_binary(
  ant_t *js, const char *str, size_t len, BufferEncoding encoding, uint8_t **out, size_t *out_len
) {
  if (encoding == ENC_HEX) {
    *out = hex_decode_node_prefix(str, len, out_len);
    return *out ? js_mkundef() : js_mkerr(js, "Failed to allocate buffer");
  }
  *out = ant_base64_decode_loose(str, len, out_len);
  return *out ? js_mkundef() : js_mkerr_typed(js, JS_ERR_TYPE, "Invalid base64 string");
}

static inline bool buffer_encoding_is_binary(BufferEncoding encoding) {
  return encoding == ENC_HEX || encoding == ENC_BASE64 || encoding == ENC_BASE64URL;
}

static ant_value_t buffer_encode_into(
  ant_t *js, const char *str, size_t len, BufferEncoding encoding,
  uint8_t *dst, size_t cap, size_t *written
) {
  if (buffer_encoding_is_binary(encoding)) {
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    ant_value_t result = buffer_decode_binary(js, str, len, encoding, &decoded, &decoded_len);
    if (is_err(result)) return result;
    *written = decoded_len < cap ? decoded_len : cap;
    memcpy(dst, decoded, *written);
    free(decoded);
    return js_mkundef();
  }

  if (encoding == ENC_UTF8) *written = utf8_export_into(str, len, dst, cap, NULL);
  else *written = buffer_encode_units_into(str, len, dst, cap, encoding == ENC_UCS2);
  return js_mkundef();
}

static size_t buffer_encode_capacity(ant_t *js, ant_value_t str_value, size_t len, BufferEncoding encoding) {
  if (encoding == ENC_UTF8) return len;
  size_t units = (size_t)str_utf16_len(js, str_value);
  return encoding == ENC_UCS2 ? units * 2 : units;
}

static ant_value_t buffer_encode_string(ant_t *js, ant_value_t value, BufferEncoding encoding, const uint8_t **out, size_t *out_len, uint8_t **owned) {
  ant_value_t str_value = js_tostring_val(js, value);
  if (is_err(str_value)) return str_value;

  size_t len = 0;
  const char *str = js_getstr(js, str_value, &len);
  *owned = NULL;

  if (encoding == ENC_UTF8 && str_is_valid_utf8(str)) {
    *out = (const uint8_t *)str;
    *out_len = len;
    return js_mkundef();
  }

  if (buffer_encoding_is_binary(encoding)) {
    ant_value_t result = buffer_decode_binary(js, str, len, encoding, owned, out_len);
    *out = *owned;
    return result;
  }

  size_t cap = buffer_encode_capacity(js, str_value, len, encoding);
  *owned = malloc(cap ? cap : 1);
  if (!*owned) return js_mkerr(js, "Failed to allocate buffer");
  *out = *owned;
  return buffer_encode_into(js, str, len, encoding, *owned, cap, out_len);
}

static size_t buffer_encoded_length(ant_t *js, ant_value_t str_value, const char *str, size_t len, BufferEncoding encoding) {
  if (encoding == ENC_UTF8) return utf8_export_length(str, len);
  size_t units = (size_t)str_utf16_len(js, str_value);
  
  switch (encoding) {
    case ENC_HEX: return units >> 1;
    case ENC_UCS2: return units * 2;
    case ENC_BASE64:
    case ENC_BASE64URL:
      if (units > 0 && str[len - 1] == '=') {
        units--;
        if (units > 0 && str[len - 2] == '=') units--;
      }
      return (units * 3) >> 2;
    default: return units;
  }
}

__attribute__((noinline, cold)) static ant_value_t buffer_from_invalid(ant_t *js, ant_value_t value) {
  char got[160], message[320];
  js_describe_received(js, value, got, sizeof(got));
  snprintf(
    message, sizeof(message), "The first argument must be of type string or an instance of Buffer, "
    "ArrayBuffer, or Array or an Array-like Object. %s", got
  );
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", message);
}

static ant_value_t buffer_from_array_like(ant_t *js, ant_value_t source, ant_value_t length_val) {
  double length = vtype(length_val) == kTypeNumber ? js_getnum(length_val) : 0;
  size_t len = length > 0 && isfinite(length) ? (size_t)length : 0;
  
  ArrayBufferData *buffer = create_array_buffer_data(js, len);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");

  for (size_t i = 0; i < len; i++) {
    char idx_str[32];
    snprintf(idx_str, sizeof(idx_str), "%zu", i);
    ant_value_t elem = js_get(js, source, idx_str);
    if (is_err(elem)) return elem;
    buffer->data[i] = (uint8_t)js_to_uint32(js_to_number(js, elem));
  }

  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, js->builtins.buffer_proto);
}

static ant_value_t js_buffer_from(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Buffer.from requires at least one argument");
  
  if (vtype(args[0]) == kTypeString) {
    size_t len;
    char *str = js_getstr(js, args[0], &len);
    
    BufferEncoding encoding = ENC_UTF8;
    size_t enc_len = 0;
    if (nargs >= 2 && vtype(args[1]) == kTypeString && js_getstr(js, args[1], &enc_len) && enc_len > 0) {
      ant_value_t checked = buffer_require_encoding(js, args[1], &encoding);
      if (is_err(checked)) return checked;
    }
    
    if (buffer_encoding_is_binary(encoding)) {
      uint8_t *decoded = NULL;
      size_t decoded_len = 0;
      ant_value_t result = buffer_decode_binary(js, str, len, encoding, &decoded, &decoded_len);
      if (is_err(result)) return result;

      ArrayBufferData *buffer = create_array_buffer_data(js, decoded_len);
      if (!buffer) { free(decoded); return js_mkerr(js, "Failed to allocate buffer"); }
      memcpy(buffer->data, decoded, decoded_len);
      free(decoded);
      return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, decoded_len, js->builtins.buffer_proto);
    }

    size_t cap = buffer_encode_capacity(js, args[0], len, encoding);
    ArrayBufferData *buffer = create_array_buffer_data(js, cap);
    if (!buffer) return js_mkerr(js, "Failed to allocate buffer");

    size_t written = 0;
    ant_value_t result = buffer_encode_into(js, str, len, encoding, buffer->data, cap, &written);
    if (is_err(result)) return result;
    return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, written, js->builtins.buffer_proto);
  }

  ArrayBufferData *arraybuffer = buffer_get_arraybuffer_data(args[0]);
  if (arraybuffer && arraybuffer->is_detached)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform Construct on a detached ArrayBuffer");
  if (arraybuffer) {
    size_t byte_offset = 0;
    size_t byte_length = arraybuffer->length;

    if (nargs > 1 && vtype(args[1]) == kTypeNumber) byte_offset = (size_t)js_getnum(args[1]);
    if (byte_offset > arraybuffer->length)
      return js_mkerr(js, "Start offset is outside the bounds of the buffer");

    if (nargs > 2 && vtype(args[2]) == kTypeNumber) {
      byte_length = (size_t)js_getnum(args[2]);
      if (byte_length > arraybuffer->length - byte_offset)
        return js_mkerr(js, "Invalid Buffer length");
    } else byte_length = arraybuffer->length - byte_offset;

    return create_typed_array_with_buffer(
      js, TYPED_ARRAY_UINT8, arraybuffer, byte_offset, byte_length, js->builtins.buffer_proto, args[0]
    );
  }
  
  if (!is_object_type(args[0])) return buffer_from_invalid(js, args[0]);
  ant_value_t value_of = js_getprop_fallback(js, args[0], "valueOf");
  if (is_err(value_of)) return value_of;
  
  if (is_callable(value_of)) {
    ant_value_t primitive = sv_vm_call(js->vm, js, value_of, args[0], NULL, 0, NULL, js_mkundef());
    if (is_err(primitive)) return primitive;
    if (primitive != args[0] && (vtype(primitive) == kTypeString || is_object_type(primitive))) {
      ant_value_t from_args[3] = { primitive, nargs > 1 ? args[1] : js_mkundef(), nargs > 2 ? args[2] : js_mkundef() };
      return js_buffer_from(js, from_args, nargs > 1 ? nargs : 1, js_mkundef());
    }
  }

  ant_value_t length_val = js_get(js, args[0], "length");
  if (is_err(length_val)) return length_val;
  if (vtype(length_val) != kTypeUndefined) return buffer_from_array_like(js, args[0], length_val);

  ant_value_t type = js_get(js, args[0], "type");
  ant_value_t data = js_get(js, args[0], "data");
  if (is_err(type)) return type;
  if (is_err(data)) return data;
  
  size_t type_len = 0;
  const char *type_str = vtype(type) == kTypeString ? js_getstr(js, type, &type_len) : NULL;
  if (type_str && type_len == 6 && memcmp(type_str, "Buffer", 6) == 0 && vtype(data) == kTypeArray)
    return buffer_from_array_like(js, data, js_get(js, data, "length"));

  ant_value_t to_primitive = js_get_sym(js, args[0], js->sym.toPrimitive_sym);
  if (is_err(to_primitive)) return to_primitive;
  
  if (is_callable(to_primitive)) {
    ant_value_t hint = js_mkstr(js, "string", 6);
    ant_value_t primitive = sv_vm_call(js->vm, js, to_primitive, args[0], &hint, 1, NULL, js_mkundef());
    if (is_err(primitive)) return primitive;
    if (vtype(primitive) == kTypeString) {
      ant_value_t from_args[2] = { primitive, nargs > 1 ? args[1] : js_mkundef() };
      return js_buffer_from(js, from_args, nargs > 1 ? 2 : 1, js_mkundef());
    }
  }

  return buffer_from_invalid(js, args[0]);
}

static ant_value_t buffer_fill_range(
  ant_t *js, uint8_t *data, size_t start, size_t end,
  ant_value_t value, ant_value_t encoding_val
) {
  if (start >= end) return js_mkundef();
  size_t span = end - start;

  if (vtype(value) == kTypeString) {
    BufferEncoding encoding = ENC_UTF8;
    size_t enc_len = 0;
    bool empty_encoding = vtype(encoding_val) == kTypeString && js_getstr(js, encoding_val, &enc_len) && enc_len == 0;
    if (vtype(encoding_val) != kTypeUndefined && vtype(encoding_val) != kTypeNull && !empty_encoding) {
      ant_value_t checked = buffer_require_encoding(js, encoding_val, &encoding);
      if (is_err(checked)) return checked;
    }

    size_t str_len = 0;
    char *str = js_getstr(js, value, &str_len);
    if (str_len == 0) {
      memset(data + start, 0, span);
      return js_mkundef();
    }

    const uint8_t *pattern = NULL;
    size_t pattern_len = 0;
    uint8_t *owned = NULL;

    ant_value_t encoded = buffer_encode_string(js, value, encoding, &pattern, &pattern_len, &owned);
    if (is_err(encoded)) return encoded;

    if (pattern_len == 0) {
      free(owned);
      ant_value_t props = Ant_Object_CreateBare(js);
      js_set(js, props, "code", js_mkstr(js, "ERR_INVALID_ARG_VALUE", 21));
      return js_mkerr_props(js, JS_ERR_TYPE, props, "The argument 'value' is invalid. Received '%.*s'", (int)str_len, str);
    }

    for (size_t i = 0; i < span; i += pattern_len) {
      size_t chunk = span - i < pattern_len ? span - i : pattern_len;
      memcpy(data + start + i, pattern, chunk);
    }
    
    free(owned);
    return js_mkundef();
  }

  if (buffer_get_typedarray_data(value) || buffer_is_dataview(value)) {
    const uint8_t *source = NULL;
    size_t source_len = 0;
    buffer_source_get_bytes(js, value, &source, &source_len);
    if (source_len == 0) return js_mkerr_code(
      js, JS_ERR_TYPE, "ERR_INVALID_ARG_VALUE",
      "The argument 'value' is invalid. Received an empty buffer"
    );

    uint8_t *pattern = malloc(source_len);
    if (!pattern) return js_mkerr(js, "Failed to allocate buffer");
    memcpy(pattern, source, source_len);
    
    for (size_t i = 0; i < span; i += source_len) {
      size_t chunk = span - i < source_len ? span - i : source_len;
      memcpy(data + start + i, pattern, chunk);
    }
    
    free(pattern);
    return js_mkundef();
  }

  double num = js_to_number(js, value);
  memset(data + start, (int)(js_to_uint32(num) & 0xff), span);
  return js_mkundef();
}

static void buffer_group_digits(const char *digits, char *out, size_t len) {
  size_t n = strlen(digits), start = digits[0] == '-' ? 1 : 0, head = n;
  while (head >= start + 4) head -= 3;
  
  size_t w = 0;
  for (size_t i = 0; i < n && w + 1 < len; i++) {
    if (i >= head && i > start && (i - head) % 3 == 0) out[w++] = '_';
    if (w + 1 < len) out[w++] = digits[i];
  }
  out[w] = '\0';
}

static void buffer_describe_value(ant_t *js, ant_value_t value, char *out, size_t len) {
  char raw[96];
  
  if (vtype(value) == kTypeBigInt) {
    size_t n = strbigint(js, value, raw, sizeof(raw) - 1);
    raw[n < sizeof(raw) - 1 ? n : sizeof(raw) - 2] = '\0';
    const char *digits = raw[0] == '-' ? raw + 1 : raw;
    bool large = strlen(digits) > 10 || (strlen(digits) == 10 && strcmp(digits, "4294967296") > 0);
    if (large) buffer_group_digits(raw, out, len - 1);
    else snprintf(out, len - 1, "%s", raw);
    strncat(out, "n", len - strlen(out) - 1);
    return;
  }
  
  double d = js_getnum(value);
  if (d == 0 && signbit(d)) { snprintf(out, len, "-0"); return; }
  
  size_t n = js_number_to_chars(value, raw, sizeof(raw) - 1);
  raw[n < sizeof(raw) - 1 ? n : sizeof(raw) - 2] = '\0';
  if (isfinite(d) && d == trunc(d) && fabs(d) > 4294967296.0) buffer_group_digits(raw, out, len);
  else snprintf(out, len, "%s", raw);
}

__attribute__((noinline, cold)) static ant_value_t buffer_out_of_range(ant_t *js, const char *name, const char *range, ant_value_t received) {
  char got[128], message[384];
  buffer_describe_value(js, received, got, sizeof(got));
  snprintf(message, sizeof(message), "The value of \"%s\" is out of range. It must be %s. Received %s", name, range, got);
  return js_mkerr_code(js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE", message);
}

static ant_value_t buffer_size_arg(ant_t *js, ant_value_t *args, int nargs, size_t *out) {
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(value) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, "size", "of type number", value);

  double size = js_getnum(value);
  if (!(size >= 0 && size <= 9007199254740991.0))
    return buffer_out_of_range(js, "size", ">= 0 && <= 9007199254740991", value);

  *out = (size_t)size;
  return js_mkundef();
}

// Buffer.alloc(size[, fill[, encoding]])
static ant_value_t js_buffer_alloc(ant_params_t) {
  size_t size = 0;
  ant_value_t valid = buffer_size_arg(js, args, nargs, &size);
  if (is_err(valid)) return valid;
  
  ArrayBufferData *buffer = create_array_buffer_data(js, size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  memset(buffer->data, 0, size);
  ant_value_t result = create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, js->builtins.buffer_proto);
  if (is_err(result)) return result;

  ant_value_t fill = nargs > 1 ? args[1] : js_mkundef();
  if (size == 0 || vtype(fill) == kTypeUndefined) return result;
  if (vtype(fill) == kTypeNumber && js_getnum(fill) == 0) return result;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  
  ant_value_t filled = buffer_fill_range(
    js, buffer->data, 0, size, fill,
    vtype(fill) == kTypeString && nargs > 2 ? args[2] : js_mkundef()
  );
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(filled) ? filled : result;
}

// Buffer.allocUnsafe(size)
static ant_value_t js_buffer_allocUnsafe(ant_params_t) {
  size_t size = 0;
  ant_value_t valid = buffer_size_arg(js, args, nargs, &size);
  if (is_err(valid)) return valid;

  ArrayBufferData *buffer = create_array_buffer_data(js, size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, js->builtins.buffer_proto);
}

static ant_value_t js_buffer_allocUnsafeSlow(ant_params_t) {
  return js_buffer_allocUnsafe(js, args, nargs, js_mkundef());
}

static ant_value_t js_buffer_of(ant_params_t) {
  return js_typedarray_of(js, args, nargs, TYPED_ARRAY_UINT8, js->builtins.buffer_proto);
}

static ant_value_t buffer_index_arg(ant_t *js, ant_value_t v, const char *name, size_t *out) {
  if (vtype(v) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, name, "of type number", v);
  
  double d = js_getnum(v);
  if (!isfinite(d) || d != trunc(d)) return buffer_out_of_range(js, name, "an integer", v);
  if (d < 0 || d > 9007199254740991.0) return buffer_out_of_range(js, name, ">= 0 && <= 9007199254740991", v);
  
  *out = (size_t)d;
  return js_mkundef();
}

static ant_value_t js_buffer_copyBytesFrom(ant_params_t) {
  TypedArrayData *view = nargs > 0 ? buffer_get_typedarray_data(args[0]) : NULL;
  if (!view) return Ant_Error_CreateInvalidArgType(js, "view", "an instance of TypedArray", nargs > 0 ? args[0] : js_mkundef());
  
  size_t len = Ant_Buffer_ViewIsLive(view) ? view->length : 0;
  size_t start = 0, count = len;
  
  if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
    ant_value_t r = buffer_index_arg(js, args[1], "offset", &start);
    if (is_err(r)) return r;
  }
  
  if (nargs > 2 && vtype(args[2]) != kTypeUndefined) {
    ant_value_t r = buffer_index_arg(js, args[2], "length", &count);
    if (is_err(r)) return r;
  }
  
  start = start < len ? start : len;
  count = count < len - start ? count : len - start;
  
  size_t elem_size = get_element_size(view->type);
  size_t bytes = count * elem_size;
  ArrayBufferData *buffer = create_array_buffer_data(js, bytes);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  if (bytes) memcpy(buffer->data, view->buffer->data + view->byte_offset + start * elem_size, bytes);
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, bytes, js->builtins.buffer_proto);
}

static ant_value_t typedarray_join_with(ant_t *js, ant_value_t this_val, const char *sep, size_t sep_len) {
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  if (ta_data->length == 0) return js_mkstr(js, "", 0);

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
static ant_value_t js_typedarray_toString(ant_params_t) {
  return typedarray_join_with(js, js_getthis(js), ",", 1);
}

// TypedArray.prototype.join(separator)
static ant_value_t js_typedarray_join(ant_params_t) {
  const char *sep = ",";
  size_t sep_len = 1;
  if (nargs > 0 && vtype(args[0]) == kTypeString)
    sep = js_getstr(js, args[0], &sep_len);
  return typedarray_join_with(js, js_getthis(js), sep, sep_len);
}

static ant_value_t js_typedarray_indexOf(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  size_t len = ta_data->length;
  if (len == 0 || nargs < 1) return js_mknum(-1);

  int64_t from_index = 0;
  if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
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

static ant_value_t js_typedarray_lastIndexOf(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  size_t len = ta_data->length;
  if (len == 0 || nargs < 1) return js_mknum(-1);

  int64_t from_index = (int64_t)len - 1;
  if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
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
    if (!buffer_typedarray_data_read_index(js, ta_data, (size_t)i, &value)) return js_mknum(-1);
    if (strict_eq_values(js, value, search)) return js_mknum((double)i);
  }

  return js_mknum(-1);
}

static ant_value_t js_typedarray_includes(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  size_t len = ta_data->length;
  ant_value_t search = (nargs > 0) ? args[0] : js_mkundef();
  
  if (len == 0) return js_false;
  int64_t from_index = 0;
  
  if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
  double from_index_num = js_to_number(js, args[1]);
  if (!isnan(from_index_num)) from_index = (int64_t)from_index_num;
  if (from_index < 0) {
    from_index += (int64_t)len;
    if (from_index < 0) from_index = 0;
  }}

  if ((size_t)from_index >= len) return js_false;

  if (ta_data->type == TYPED_ARRAY_BIGINT64) {
    int64_t needle = 0;
    if (vtype(search) == kTypeBigInt) {
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
    if (vtype(search) == kTypeBigInt) {
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

static ant_value_t js_typedarray_reverse(ant_params_t) {
  (void)args;
  (void)nargs;

  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  for (size_t left = 0, right = ta_data->length; left < right && left < --right; left++) {
    ant_value_t a = js_mkundef();
    ant_value_t b = js_mkundef();
    if (!buffer_typedarray_data_read_index(js, ta_data, left, &a))
      return js_mkerr(js, "Failed to read from TypedArray");
    if (!buffer_typedarray_data_read_index(js, ta_data, right, &b))
      return js_mkerr(js, "Failed to read from TypedArray");
    ant_value_t write_b = typedarray_write_value(js, ta_data, left, b);
    if (is_err(write_b)) return write_b;
    ant_value_t write_a = typedarray_write_value(js, ta_data, right, a);
    if (is_err(write_a)) return write_a;
  }

  return this_val;
}

static ant_value_t js_typedarray_sort(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);

  bool has_compare = nargs > 0 && vtype(args[0]) != kTypeUndefined;
  if (has_compare && !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.sort comparefn must be callable");

  size_t len = ta_data->length;
  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);
  
  ant_value_t *values = gc_temp_root_slots(&roots, len);
  if (!values) {
    gc_temp_root_scope_end(&roots);
    return js_mkerr(js, "oom");
  }

  for (size_t i = 0; i < len; i++) if (!buffer_typedarray_data_read_index(js, ta_data, i, &values[i])) {
    gc_temp_root_scope_end(&roots);
    return js_mkerr(js, "Failed to read from TypedArray");
  }

  for (size_t i = 1; i < len; i++) {
    ant_value_t key = values[i];
    size_t j = i;
    while (j > 0) {
      double cmp = 0.0;
      if (has_compare) {
        ant_value_t cmp_args[2] = { values[j - 1], key };
        ant_value_t cmp_val = sv_vm_call(js->vm, js, args[0], js_mkundef(), cmp_args, 2, NULL, js_mkundef());
        if (is_err(cmp_val)) { gc_temp_root_scope_end(&roots); return cmp_val; }
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
    if (is_err(write_result)) { gc_temp_root_scope_end(&roots); return write_result; }
  }

  gc_temp_root_scope_end(&roots);
  return this_val;
}

// Buffer.prototype.toString(encoding)
static ant_value_t js_buffer_slice(ant_params_t) {
  return js_typedarray_subarray(js, args, nargs, js_mkundef());
}

static ant_value_t buffer_bytes_to_string_enc(ant_t *js, const uint8_t *data, size_t len, BufferEncoding encoding) {
  if (encoding == ENC_BASE64 || encoding == ENC_BASE64URL) {
    size_t out_len;
    
    char *encoded = encoding == ENC_BASE64URL
      ? ant_base64url_encode(data, len, &out_len)
      : ant_base64_encode(data, len, &out_len);
    
    if (!encoded) return js_mkerr(js, "Failed to encode base64");
    
    ant_value_t result = js_mkstr(js, encoded, out_len);
    free(encoded);
    return result;
  } else if (encoding == ENC_HEX) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return js_mkerr(js, "Failed to allocate hex string");
    
    hex_encode(data, len, hex);
    
    ant_value_t result = js_mkstr(js, hex, len * 2);
    free(hex);
    return result;
  } else if (encoding == ENC_LATIN1) {
    size_t out_len;
    char *out = latin1_to_utf8(data, len, &out_len);
    if (!out) return js_mkerr(js, "Failed to decode buffer as Latin-1");

    ant_value_t result = js_mkstr(js, out, out_len);
    free(out);
    return result;
  } else if (encoding == ENC_ASCII) {
    char *out = malloc(len == 0 ? 1 : len);
    if (!out) return js_mkerr(js, "Failed to allocate string");
    for (size_t i = 0; i < len; i++) out[i] = (char)(data[i] & 0x7f);
    ant_value_t result = js_mkstr(js, out, len);
    free(out);
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

bool buffer_bytes_to_string(
  ant_t *js, const uint8_t *data, size_t len,
  const char *encoding, size_t encoding_len, ant_value_t *out
) {
  BufferEncoding parsed = parse_encoding(encoding, encoding_len);
  if (parsed == ENC_UNKNOWN) return false;
  *out = buffer_bytes_to_string_enc(js, data, len, parsed);
  return true;
}

// Buffer.prototype.toString(encoding)
static ant_value_t js_buffer_toJSON(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(js_getthis(js));
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  size_t len = Ant_Buffer_ViewIsLive(ta_data) ? ta_data->byte_length : 0;
  const uint8_t *bytes = len ? ta_data->buffer->data + ta_data->byte_offset : NULL;

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t data = js_mkarr(js);
  GC_ROOT_PIN(js, data);
  for (size_t i = 0; i < len; i++) js_arr_push(js, data, js_mknum(bytes[i]));

  ant_value_t result = Ant_Object_Create(js);
  GC_ROOT_PIN(js, result);
  js_set(js, result, "type", js_mkstr(js, "Buffer", 6));
  js_set(js, result, "data", data);
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t js_buffer_toString(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  if (!Ant_Buffer_ViewIsLive(ta_data)) return js_mkstr(js, "", 0);
  
  size_t start = 0;
  size_t len = 0;
  
  buffer_string_bounds(
    nargs > 1 ? args[1] : js_mkundef(),
    nargs > 2 ? args[2] : js_mkundef(),
    ta_data->byte_length,
    &start, &len
  );
  
  if (len == 0) return js_mkstr(js, "", 0);
  
  BufferEncoding encoding = ENC_UTF8;
  if (nargs > 0 && vtype(args[0]) != kTypeUndefined) {
    ant_value_t checked = buffer_require_encoding(js, args[0], &encoding);
    if (is_err(checked)) return checked;
  }
  
  uint8_t *data = 
    ta_data->buffer->data + 
    ta_data->byte_offset + start;
  
  return buffer_bytes_to_string_enc(js, data, len, encoding);
}

static ant_value_t js_buffer_utf8Slice(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  size_t start = 0;
  size_t end = 0;

  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  uint8_t *bytes; size_t byte_length;
  if (!Ant_Buffer_GetViewBytes(ta_data, &bytes, &byte_length))
    return js_mkerr(js, "Cannot read from detached buffer");

  end = byte_length;
  if (nargs > 0 && vtype(args[0]) == kTypeNumber && js_getnum(args[0]) > 0)
    start = (size_t)js_getnum(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber && js_getnum(args[1]) >= 0)
    end = (size_t)js_getnum(args[1]);

  if (start > byte_length) start = byte_length;
  if (end > byte_length) end = byte_length;
  if (end < start) end = start;

  uint8_t *data = bytes + start;
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
static ant_value_t js_buffer_toBase64(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t encoding_arg = js_mkstr(js, "base64", 6);
  ant_value_t new_args[1] = {encoding_arg};
  return js_buffer_toString(js, new_args, 1, js_mkundef());
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


static int64_t buffer_last_index_offset(int64_t len, double offset, int64_t needle_len) {
  if (isnan(offset)) offset = (double)len;
  if (offset > 9e15) offset = 9e15;
  if (offset < -9e15) offset = -9e15;

  int64_t off = (int64_t)(offset < 0 ? ceil(offset) : floor(offset));
  if (off < 0) {
    if (off + len >= 0) return len + off;
    return needle_len == 0 ? 0 : -1;
  }
  
  if (off + needle_len <= len) return off;
  if (needle_len == 0) return len;
  
  return len - 1;
}

static ant_value_t buffer_index_of(ant_t *js, ant_value_t *args, int nargs, bool forward) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  if (nargs < 1) return js_mknum(-1);

  uint8_t *haystack; size_t haystack_len;
  Ant_Buffer_GetViewBytes(ta_data, &haystack, &haystack_len);
  double offset_num = forward ? 0.0 : (double)NAN;
  BufferEncoding encoding = ENC_UTF8;
  ant_value_t encoding_arg = js_mkundef();

  if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
    if (vtype(args[1]) == kTypeString) {
      size_t enc_len = 0;
      char *enc_str = js_getstr(js, args[1], &enc_len);
      encoding = parse_encoding(enc_str, enc_len);
      encoding_arg = args[1];
    } else {
      offset_num = js_to_number(js, args[1]);
    }
  }

  if (nargs > 2 && vtype(args[2]) != kTypeUndefined) {
    ant_value_t enc_value = js_tostring_val(js, args[2]);
    if (is_err(enc_value)) return enc_value;
    size_t enc_len = 0;
    char *enc_str = js_getstr(js, enc_value, &enc_len);
    encoding = parse_encoding(enc_str, enc_len);
    encoding_arg = enc_value;
  }

  size_t start = forward ? buffer_normalize_indexof_offset(haystack_len, offset_num) : 0;
  ant_value_t search = args[0];

  if (vtype(search) == kTypeNumber) {
    uint8_t needle = (uint8_t)js_to_uint32(js_getnum(search));
    if (!forward) {
      int64_t last = buffer_last_index_offset((int64_t)haystack_len, offset_num, 1);
      if (last < 0 || haystack_len == 0) return js_mknum(-1);
      for (int64_t i = last; i >= 0; i--) {
        if (haystack[i] == needle) return js_mknum((double)i);
      }
      return js_mknum(-1);
    }
    
    if (start >= haystack_len) return js_mknum(-1);
    for (size_t i = start; i < haystack_len; i++) {
      if (haystack[i] == needle) return js_mknum((double)i);
    }
    return js_mknum(-1);
  }

  const uint8_t *needle = NULL;
  size_t needle_len = 0;
  uint8_t *owned_needle = NULL;

  if (vtype(search) == kTypeString && encoding == ENC_UTF8) {
    needle = (const uint8_t *)js_getstr(js, search, &needle_len);
  } else if (vtype(search) == kTypeString) {
    if (encoding == ENC_UNKNOWN) return buffer_unknown_encoding_error(js, encoding_arg);
    ant_value_t encoded = buffer_encode_string(js, search, encoding, &needle, &needle_len, &owned_needle);
    if (is_err(encoded)) return encoded;
  } else if (!buffer_source_get_bytes(js, search, &needle, &needle_len)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "The value argument must be one of type number or string or an instance of Buffer or Uint8Array");
  }

  double found = -1;
  if (!forward) {
    int64_t last = buffer_last_index_offset((int64_t)haystack_len, offset_num, (int64_t)needle_len);
    if (needle_len == 0) found = (double)last;
    else if (last >= 0 && needle_len <= haystack_len) {
      int64_t pos = last < (int64_t)(haystack_len - needle_len) ? last : (int64_t)(haystack_len - needle_len);
      for (; pos >= 0; pos--) if (haystack[pos] == needle[0] && memcmp(haystack + pos, needle, needle_len) == 0) {
        found = (double)pos;
        break;
      }
    }
  } else if (needle_len == 0) {
    found = (double)start;
  } else if (start < haystack_len && needle_len <= haystack_len - start) {
    size_t limit = haystack_len - needle_len;
    uint8_t first = needle[0];
    for (size_t i = start; i <= limit; i++) if (haystack[i] == first && (needle_len == 1 || memcmp(haystack + i, needle, needle_len) == 0)) {
      found = (double)i;
      break;
    }
  }

  if (owned_needle) free(owned_needle);
  return js_mknum(found);
}

static ant_value_t js_buffer_indexOf(ant_params_t) {
  return buffer_index_of(js, args, nargs, true);
}

static ant_value_t js_buffer_lastIndexOf(ant_params_t) {
  return buffer_index_of(js, args, nargs, false);
}

static ant_value_t js_buffer_includes(ant_params_t) {
  ant_value_t result = buffer_index_of(js, args, nargs, true);
  if (is_err(result)) return result;
  return js_bool(js_getnum(result) != -1);
}

static ant_value_t buffer_validate_offset(ant_t *js, ant_value_t value, const char *name, size_t max, size_t *out) {
  if (vtype(value) != kTypeNumber) {
    ant_value_t props = Ant_Object_CreateBare(js);
    js_set(js, props, "code", js_mkstr(js, "ERR_INVALID_ARG_TYPE", 20));
    return js_mkerr_props(js, JS_ERR_TYPE, props, "The \"%s\" argument must be of type number", name);
  }
  
  double num = js_getnum(value);
  if (num != floor(num) || num < 0 || num > (double)max) {
    ant_value_t props = Ant_Object_CreateBare(js);
    js_set(js, props, "code", js_mkstr(js, "ERR_OUT_OF_RANGE", 16));
    return js_mkerr_props(
      js, JS_ERR_RANGE, props,
      "The value of \"%s\" is out of range. It must be an integer >= 0 && <= %zu. Received %g",
      name, max, num
    );
  }
  
  *out = (size_t)num;
  return js_mkundef();
}

// Buffer.prototype.fill(value[, offset[, end]][, encoding])
static ant_value_t js_buffer_fill(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &len))
    return js_mkerr(js, "Cannot write to detached buffer");

  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t offset_val = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t end_val = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t encoding_val = nargs > 3 ? args[3] : js_mkundef();

  if (vtype(value) == kTypeString) {
    if (vtype(offset_val) == kTypeUndefined || vtype(offset_val) == kTypeString) {
      encoding_val = offset_val;
      offset_val = end_val = js_mkundef();
    } else if (vtype(end_val) == kTypeString) {
      encoding_val = end_val;
      end_val = js_mkundef();
    }
  } else encoding_val = js_mkundef();

  size_t start = 0, end = len;

  if (vtype(offset_val) != kTypeUndefined) {
    ant_value_t checked = buffer_validate_offset(js, offset_val, "offset", SIZE_MAX >> 1, &start);
    if (is_err(checked)) return checked;
    if (vtype(end_val) != kTypeUndefined) {
      checked = buffer_validate_offset(js, end_val, "end", len, &end);
      if (is_err(checked)) return checked;
    }
    if (start >= end) return this_val;
  }

  ant_value_t result = buffer_fill_range(js, data, start, end, value, encoding_val);
  
  return is_err(result) ? result : this_val;
}

// Buffer.prototype.write(string, offset, length, encoding)
static ant_value_t js_buffer_write(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "write requires a string");
  
  ant_value_t this_val = js_getthis(js);
  TypedArrayData *ta_data = buffer_get_typedarray_data(this_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  size_t str_len;
  char *str = js_getstr(js, args[0], &str_len);
  
  uint8_t *bytes; size_t buf_len;
  Ant_Buffer_GetViewBytes(ta_data, &bytes, &buf_len);
  size_t offset = 0;
  size_t length = buf_len;
  BufferEncoding encoding = ENC_UTF8;
  
  ant_value_t offset_val = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t length_val = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t encoding_val = nargs > 3 ? args[3] : js_mkundef();

  if (vtype(offset_val) == kTypeUndefined) encoding_val = js_mkundef();
  else if (vtype(length_val) == kTypeUndefined && vtype(offset_val) == kTypeString) encoding_val = offset_val;
  else {
    ant_value_t checked = buffer_validate_offset(js, offset_val, "offset", buf_len, &offset);
    if (is_err(checked)) return checked;
    
    size_t remaining = buf_len - offset;
    if (vtype(length_val) == kTypeUndefined) length = remaining;
    else if (vtype(length_val) == kTypeString) {
      encoding_val = length_val;
      length = remaining;
    } else {
      checked = buffer_validate_offset(js, length_val, "length", buf_len, &length);
      if (is_err(checked)) return checked;
      if (length > remaining) length = remaining;
    }
  }

  if (js_truthy(js, encoding_val)) {
    ant_value_t checked = buffer_require_encoding(js, encoding_val, &encoding);
    if (is_err(checked)) return checked;
  }
  
  if (offset >= buf_len) return js_mknum(0);
  size_t available = buf_len - offset;
  if (length < available) available = length;

  size_t to_write = 0;
  ant_value_t result = buffer_encode_into(js, str, str_len, encoding, bytes + offset, available, &to_write);
  if (is_err(result)) return result;
  return js_mknum((double)to_write);
}

static ant_value_t js_buffer_copy(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "copy requires a target buffer");

  TypedArrayData *src = buffer_get_typedarray_data(js_getthis(js));
  TypedArrayData *dst = buffer_get_typedarray_data(args[0]);
  if (!src || !dst) return js_mkerr(js, "copy requires Buffer arguments");

  uint8_t *src_bytes, *dst_bytes; size_t src_size, dst_size;
  Ant_Buffer_GetViewBytes(src, &src_bytes, &src_size);
  Ant_Buffer_GetViewBytes(dst, &dst_bytes, &dst_size);

  size_t target_start = (nargs > 1 && vtype(args[1]) == kTypeNumber) ? (size_t)js_getnum(args[1]) : 0;
  size_t source_start = (nargs > 2 && vtype(args[2]) == kTypeNumber) ? (size_t)js_getnum(args[2]) : 0;
  size_t source_end = (nargs > 3 && vtype(args[3]) == kTypeNumber) ? (size_t)js_getnum(args[3]) : src_size;

  if (target_start > dst_size) target_start = dst_size;
  if (source_start > src_size) source_start = src_size;
  if (source_end > src_size) source_end = src_size;
  if (source_end < source_start) source_end = source_start;

  size_t src_len = source_end - source_start;
  size_t dst_len = dst_size - target_start;
  size_t copy_len = src_len < dst_len ? src_len : dst_len;
  if (copy_len == 0) return js_mknum(0);

  memmove(dst_bytes + target_start, src_bytes + source_start, copy_len);
  
  return js_mknum((double)copy_len);
}

static inline uint64_t buffer_load_bytes(const uint8_t *ptr, size_t width, bool little_endian) {
  uint64_t bits = 0;
  memcpy(&bits, ptr, width);
  return little_endian ? bits : __builtin_bswap64(bits) >> (64 - 8 * width);
}

static inline void buffer_store_bytes(uint8_t *ptr, size_t width, bool little_endian, uint64_t bits) {
  if (!little_endian) bits = __builtin_bswap64(bits << (64 - 8 * width));
  memcpy(ptr, &bits, width);
}

static __attribute__((noinline, cold)) ant_value_t buffer_bounds_error(ant_t *js, ant_value_t offset, double max) {
  double n = js_getnum(offset);
  if (floor(n) != n) return buffer_out_of_range(js, "offset", "an integer", offset);
  if (max < 0) return js_mkerr_code(js, JS_ERR_RANGE, "ERR_BUFFER_OUT_OF_BOUNDS", "Attempt to access memory outside buffer bounds");
  
  char range[64];
  if (isnan(max)) snprintf(range, sizeof(range), ">= 0 and <= NaN");
  else snprintf(range, sizeof(range), ">= 0 and <= %.0f", max);
  return buffer_out_of_range(js, "offset", range, offset);
}

static ant_value_t buffer_access_ptr(ant_t *js, ant_value_t offset_value, size_t width, uint8_t **ptr, size_t *offset) {
  if (vtype(offset_value) == kTypeUndefined) offset_value = js_mknum(0);
  if (vtype(offset_value) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, "offset", "of type number", offset_value);
  
  ant_value_t receiver = js_getthis(js);
  TypedArrayData *ta = buffer_get_typedarray_data(receiver);
  uint8_t *bytes; size_t live_len;
  Ant_Buffer_GetViewBytes(ta, &bytes, &live_len);

  double n = js_getnum(offset_value);
  double length = ta ? (double)live_len : js_to_number(js, js_getprop_fallback(js, receiver, "length"));
  double max = length - (double)width;
  if (floor(n) != n || n < 0 || !(n <= max)) return buffer_bounds_error(js, offset_value, max);
  if (!ta) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Buffer");

  *offset = (size_t)n;
  *ptr = bytes + *offset;
  return js_mkundef();
}

static bool buffer_bigint64_in_range(ant_t *js, ant_value_t value, bool is_signed, uint64_t *bits) {
  if (vtype(value) == kTypeBigInt && !is_signed) return bigint_to_uint64_checked(js, value, bits);
  
  if (vtype(value) == kTypeBigInt) {
    int64_t signed_bits;
    if (!bigint_to_int64_checked(js, value, &signed_bits)) return false;
    *bits = (uint64_t)signed_bits;
    return true;
  }
  
  if (vtype(value) != kTypeNumber) return true;
  double n = js_getnum(value);
  
  if (isnan(n)) return true;
  return is_signed ? n >= -0x1p63 && n < 0x1p63 : n >= 0 && n < 0x1p64;
}

static ant_value_t buffer_write_bigint64(ant_native_params_t, bool little_endian, bool is_signed) {
  ant_value_t value = buffer_unwrap_bigint(nargs > 0 ? args[0] : js_mkundef());
  const char *range = is_signed ? ">= -(2n ** 63n) and < 2n ** 63n" : ">= 0n and < 2n ** 64n";
  
  uint64_t bits = 0;
  if (!buffer_bigint64_in_range(js, value, is_signed, &bits)) return buffer_out_of_range(js, "value", range, value);
  
  size_t offset; uint8_t *ptr;
  ant_value_t error = buffer_access_ptr(js, nargs > 1 ? args[1] : js_mkundef(), 8, &ptr, &offset);
  if (is_err(error)) return error;
  
  if (vtype(value) != kTypeBigInt) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot mix BigInt and other types, use explicit conversions");
  buffer_store_bytes(ptr, 8, little_endian, bits);
  
  return js_mknum((double)(offset + 8));
}

typedef enum {
  BUFFER_NUM_UINT,
  BUFFER_NUM_INT,
  BUFFER_NUM_FLOAT,
  BUFFER_NUM_BIGINT,
  BUFFER_NUM_BIGUINT,
} buffer_num_kind_t;

typedef enum {
  BUFFER_NUM_BE,
  BUFFER_NUM_LE,
} buffer_num_endian_t;

static ant_value_t buffer_num_width(ant_t *js, ant_value_t value, size_t *width) {
  if (vtype(value) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, "byteLength", "of type number", value);
  double n = js_getnum(value);
  if (floor(n) != n) return buffer_out_of_range(js, "byteLength", "an integer", value);
  if (n < 1 || n > 6) return buffer_out_of_range(js, "byteLength", ">= 1 and <= 6", value);
  *width = (size_t)n;
  return js_mkundef();
}

static inline __attribute__((always_inline)) ant_value_t buffer_num_decode(
  ant_t *js, uint64_t bits, buffer_num_kind_t kind, size_t width
) {
  unsigned shift = 64 - 8 * (unsigned)width;
  switch (kind) {
    case BUFFER_NUM_UINT:    return js_mknum((double)bits);
    case BUFFER_NUM_INT:     return js_mknum((double)((int64_t)(bits << shift) >> shift));
    case BUFFER_NUM_BIGINT:  return bigint_from_int64(js, (int64_t)bits);
    case BUFFER_NUM_BIGUINT: return bigint_from_uint64(js, bits);
    case BUFFER_NUM_FLOAT: break;
  }
  
  if (width == 2) return js_mknum(half_to_double((uint16_t)bits));
  if (width == 4) {
    float f; uint32_t narrow = (uint32_t)bits;
    memcpy(&f, &narrow, sizeof(f));
    return js_mknum((double)f);
  }
  
  double d;
  memcpy(&d, &bits, sizeof(d));
  return js_mknum(d);
}

static inline __attribute__((always_inline)) uint64_t buffer_float_bits(double num, size_t width) {
  uint64_t bits;
  if (width == 2) return double_to_half(num);
  if (width == 4) { float f = (float)num; uint32_t narrow; memcpy(&narrow, &f, sizeof(narrow)); return narrow; }
  memcpy(&bits, &num, sizeof(bits));
  return bits;
}

static __attribute__((cold, noinline)) ant_value_t buffer_num_range_error(
  ant_t *js, buffer_num_kind_t kind, size_t width, double lo, double hi, double value
) {
  char range[96];
  unsigned bits_width = 8 * (unsigned)width;
  if (width <= 4) snprintf(range, sizeof(range), ">= %.0f and <= %.0f", lo, hi);
  else if (kind == BUFFER_NUM_UINT) snprintf(range, sizeof(range), ">= 0 and < 2 ** %u", bits_width);
  else snprintf(range, sizeof(range), ">= -(2 ** %u) and < 2 ** %u", bits_width - 1, bits_width - 1);
  return buffer_out_of_range(js, "value", range, js_mknum(value));
}

static inline __attribute__((always_inline)) ant_value_t buffer_read_num(
  ant_native_params_t, buffer_num_kind_t kind, size_t width, buffer_num_endian_t endian
) {
  if (!width) {
    ant_value_t r = buffer_num_width(js, nargs > 1 ? args[1] : js_mkundef(), &width);
    if (is_err(r)) return r;
  }
  
  size_t offset; uint8_t *ptr;
  ant_value_t error = buffer_access_ptr(js, nargs > 0 ? args[0] : js_mkundef(), width, &ptr, &offset);
  if (is_err(error)) return error;
  
  return buffer_num_decode(js, buffer_load_bytes(ptr, width, endian == BUFFER_NUM_LE), kind, width);
}

static inline __attribute__((always_inline)) ant_value_t buffer_write_num(ant_native_params_t, buffer_num_kind_t kind, size_t width, buffer_num_endian_t endian) {
  if (kind == BUFFER_NUM_BIGINT || kind == BUFFER_NUM_BIGUINT)
    return buffer_write_bigint64(js, args, nargs, endian == BUFFER_NUM_LE, kind == BUFFER_NUM_BIGINT);
  
  ant_value_t arg = nargs > 0 ? args[0] : js_mkundef();
  double value = vtype(arg) == kTypeNumber ? js_getnum(arg) : js_to_number(js, arg);
  
  if (!width) {
    ant_value_t r = buffer_num_width(js, nargs > 2 ? args[2] : js_mkundef(), &width);
    if (is_err(r)) return r;
  }
  
  uint64_t bits;
  if (kind == BUFFER_NUM_FLOAT) bits = buffer_float_bits(value, width);
  else {
    double span = (double)(1ULL << (8 * width - (kind == BUFFER_NUM_INT)));
    double lo = kind == BUFFER_NUM_INT ? -span : 0;
    if (value < lo || value > span - 1) return buffer_num_range_error(js, kind, width, lo, span - 1, value);
    bits = isnan(value) ? 0 : (uint64_t)(int64_t)trunc(value);
  }
  
  size_t offset; uint8_t *ptr;
  ant_value_t error = buffer_access_ptr(js, nargs > 1 ? args[1] : js_mkundef(), width, &ptr, &offset);
  if (is_err(error)) return error;
  
  buffer_store_bytes(ptr, width, endian == BUFFER_NUM_LE, bits);
  return js_mknum((double)(offset + width));
}

#define BUFFER_NUM_METHOD(type, suffix, kind, width, endian) \
  static ant_value_t js_buffer_read##type##suffix(ant_params_t) { \
    return buffer_read_num(js, args, nargs, BUFFER_NUM_##kind, width, BUFFER_NUM_##endian); \
  } \
  static ant_value_t js_buffer_write##type##suffix(ant_params_t) { \
    return buffer_write_num(js, args, nargs, BUFFER_NUM_##kind, width, BUFFER_NUM_##endian); \
  }
#include "buffer/num_methods.h"

typedef struct {
  ant_cfunc_meta_t meta;
  const char *alias;
} buffer_num_method_t;

#define BUFFER_NUM_ALIAS_UINT(op, suffix) op "Uint" #suffix
#define BUFFER_NUM_ALIAS_INT(op, suffix) NULL
#define BUFFER_NUM_ALIAS_FLOAT(op, suffix) NULL
#define BUFFER_NUM_ALIAS_BIGINT(op, suffix) NULL
#define BUFFER_NUM_ALIAS_BIGUINT(op, suffix) op "BigUint" #suffix

#define BUFFER_NUM_LENGTH_read(kind, width) (BUFFER_NUM_##kind >= BUFFER_NUM_FLOAT ? 0 : (width) ? 1 : 2)
#define BUFFER_NUM_LENGTH_write(kind, width) ((width) ? 1 : 3)

#define BUFFER_NUM_ENTRY(op, type, suffix, kind, width) { \
  { js_buffer_##op##type##suffix, #op #type #suffix, BUFFER_NUM_LENGTH_##op(kind, width), 0 }, \
  BUFFER_NUM_ALIAS_##kind(#op, suffix) },

static const buffer_num_method_t buffer_num_methods[] = {
  #define BUFFER_NUM_METHOD(type, suffix, kind, width, endian) BUFFER_NUM_ENTRY(read, type, suffix, kind, width)
  #include "buffer/num_methods.h"
  #define BUFFER_NUM_METHOD(type, suffix, kind, width, endian) BUFFER_NUM_ENTRY(write, type, suffix, kind, width)
  #include "buffer/num_methods.h"
};

static __attribute__((noinline)) ant_value_t dataview_receiver_error(ant_t *js, const char *method) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Method DataView.prototype.%s called on incompatible receiver", method);
}

static __attribute__((noinline)) ant_value_t dataview_range_error(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_RANGE, "Offset is outside the bounds of the DataView");
}

static __attribute__((noinline)) ant_value_t dataview_access_error(ant_t *js, DataViewData *dv, const char *method) {
  if (!Ant_Buffer_ViewIsLive(dv)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot perform DataView.prototype.%s on a detached or out-of-bounds ArrayBuffer", method
  );
  return dataview_range_error(js);
}

static inline __attribute__((always_inline)) uint8_t *dataview_access(DataViewData *dv, size_t index, size_t width) {
  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(dv, &data, &len)) return NULL;
  if (index > len || width > len - index) return NULL;
  return data + index;
}

static inline __attribute__((always_inline)) bool dataview_fast_ptr(DataViewData *dv, ant_value_t offset, size_t width, uint8_t **out) {
  double index = tod(offset);
  if (!Ant_Buffer_IsLive(dv->buffer) || !(index >= 0.0 && index <= (double)dv->byte_length - (double)width)) return false;
  *out = dv->buffer->data + dv->byte_offset + (size_t)index;
  return true;
}

static inline __attribute__((always_inline)) ant_value_t dataview_read_value(
  ant_t *js, const uint8_t *ptr, buffer_num_kind_t kind, size_t width, bool little_endian
) {
  return buffer_num_decode(js, buffer_load_bytes(ptr, width, little_endian), kind, width);
}

static __attribute__((noinline)) ant_value_t dataview_get_slow(
  ant_native_params_t, DataViewData *dv, const char *method, buffer_num_kind_t kind, size_t width
) {
  size_t index;
  ant_value_t index_result = js_to_index_fast(js, nargs > 0 ? args[0] : js_mkundef(), &index);
  if (is_err(index_result)) return index_result;
  bool little_endian = nargs > 1 && js_truthy(js, args[1]);
  
  uint8_t *ptr = dataview_access(dv, index, width);
  if (!ptr) return dataview_access_error(js, dv, method);
  return dataview_read_value(js, ptr, kind, width, little_endian);
}

static inline __attribute__((always_inline)) ant_value_t dataview_get(
  ant_native_params_t, const char *method, buffer_num_kind_t kind, size_t width
) {
  DataViewData *dv = buffer_get_dataview_data(js_getthis(js));
  if (!dv) return dataview_receiver_error(js, method);
  
  uint8_t *ptr;
  if (__builtin_expect(!dataview_fast_ptr(dv, nargs > 0 ? args[0] : js_mkundef(), width, &ptr), 0))
    return dataview_get_slow(js, args, nargs, dv, method, kind, width);
  return dataview_read_value(js, ptr, kind, width, nargs > 1 && js_truthy(js, args[1]));
}

static inline __attribute__((always_inline)) uint64_t dataview_number_bits(double num, buffer_num_kind_t kind, size_t width) {
  return kind == BUFFER_NUM_FLOAT ? buffer_float_bits(num, width) : (uint64_t)js_to_uint32(num);
}

static __attribute__((noinline)) ant_value_t dataview_set_slow(
  ant_native_params_t, DataViewData *dv, const char *method, buffer_num_kind_t kind, size_t width
) {
  size_t index;
  ant_value_t index_result = js_to_index_fast(js, nargs > 0 ? args[0] : js_mkundef(), &index);
  if (is_err(index_result)) return index_result;
  
  ant_value_t value = nargs > 1 ? args[1] : js_mkundef();
  uint64_t bits = 0;
  
  if (kind == BUFFER_NUM_BIGINT || kind == BUFFER_NUM_BIGUINT) {
    ant_value_t bigint = buffer_require_bigint_value(js, value);
    if (is_err(bigint)) return bigint;
    bool ok = kind == BUFFER_NUM_BIGINT
      ? bigint_to_int64_wrapping(js, bigint, (int64_t *)&bits)
      : bigint_to_uint64_wrapping(js, bigint, &bits);
    if (!ok) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  } else bits = dataview_number_bits(vtype(value) == kTypeNumber ? js_getnum(value) : js_to_number(js, value), kind, width);
  
  bool little_endian = nargs > 2 && js_truthy(js, args[2]);
  uint8_t *ptr = dataview_access(dv, index, width);
  if (!ptr) return dataview_access_error(js, dv, method);
  
  buffer_store_bytes(ptr, width, little_endian, bits);
  return js_mkundef();
}

static inline __attribute__((always_inline)) ant_value_t dataview_set(
  ant_native_params_t, const char *method, buffer_num_kind_t kind, size_t width
) {
  DataViewData *dv = buffer_get_dataview_data(js_getthis(js));
  if (!dv) return dataview_receiver_error(js, method);
  
  bool numeric = kind != BUFFER_NUM_BIGINT && kind != BUFFER_NUM_BIGUINT;
  ant_value_t value = nargs > 1 ? args[1] : js_mkundef();
  
  uint8_t *ptr;
  if (__builtin_expect(!numeric || vtype(value) != kTypeNumber || !dataview_fast_ptr(dv, nargs > 0 ? args[0] : js_mkundef(), width, &ptr), 0))
    return dataview_set_slow(js, args, nargs, dv, method, kind, width);
  
  buffer_store_bytes(ptr, width, nargs > 2 && js_truthy(js, args[2]), dataview_number_bits(js_getnum(value), kind, width));
  return js_mkundef();
}

#define DATAVIEW_METHOD(type, kind, width) \
  static ant_value_t js_dataview_get##type(ant_params_t) { \
    return dataview_get(js, args, nargs, "get" #type, BUFFER_NUM_##kind, width); \
  } \
  static ant_value_t js_dataview_set##type(ant_params_t) { \
    return dataview_set(js, args, nargs, "set" #type, BUFFER_NUM_##kind, width); \
  }
#include "buffer/dataview_methods.h"

static const ant_cfunc_meta_t dataview_methods[] = {
  #define DATAVIEW_METHOD(type, kind, width) \
    { js_dataview_get##type, "get" #type, 1, 0 }, \
    { js_dataview_set##type, "set" #type, 2, 0 },
  #include "buffer/dataview_methods.h"
};

// Buffer.isBuffer(obj)
static ant_value_t js_buffer_isBuffer(ant_params_t) {
  if (nargs < 1) return js_false;
  if (!is_special_object(args[0])) return js_false;
  
  ant_value_t proto = js_get_proto(js, args[0]);
  return js_bool(proto == js->builtins.buffer_proto);
}

// Buffer.isEncoding(encoding)
static ant_value_t js_buffer_isEncoding(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return js_false;
  
  size_t len;
  char *enc = js_getstr(js, args[0], &len);
  
  if ((len == 4 && strncasecmp(enc, "utf8", 4) == 0) ||
      (len == 5 && strncasecmp(enc, "utf-8", 5) == 0) ||
      (len == 3 && strncasecmp(enc, "hex", 3) == 0) ||
      (len == 6 && strncasecmp(enc, "base64", 6) == 0) ||
      (len == 9 && strncasecmp(enc, "base64url", 9) == 0) ||
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
static ant_value_t js_buffer_byteLength(ant_params_t) {
  if (nargs < 1) return js_mknum(0);
  
  ant_value_t arg = args[0];
  
  if (is_special_object(arg)) {
    ant_value_t bytelen = js_get(js, arg, "byteLength");
    if (vtype(bytelen) == kTypeNumber) return bytelen;
    
    ant_value_t len = js_get(js, arg, "length");
    if (vtype(len) == kTypeNumber) return len;
  }
  
  if (vtype(arg) == kTypeString) {
    size_t len;
    char *str = js_getstr(js, arg, &len);
    BufferEncoding encoding = ENC_UTF8;
    
    if (nargs > 1 && vtype(args[1]) == kTypeString) {
      size_t enc_len;
      char *enc_str = js_getstr(js, args[1], &enc_len);
      BufferEncoding parsed = parse_encoding(enc_str, enc_len);
      if (parsed != ENC_UNKNOWN) encoding = parsed;
    }
    
    return js_mknum((double)buffer_encoded_length(js, arg, str, len, encoding));
  }
  
  return js_mknum(0);
}

// Buffer.concat(list, totalLength)
static ant_value_t js_buffer_concat(ant_params_t) {
  ant_value_t list = nargs > 0 ? args[0] : js_mkundef();
  if (!js_is_array(list)) return Ant_Error_CreateInvalidArgType(js, "list", "an instance of Array", list);

  ant_offset_t count = js_arr_len(js, list);
  size_t total_length = 0;
  
  for (ant_offset_t i = 0; i < count; i++) {
    ant_value_t item = js_arr_get(js, list, i);
    TypedArrayData *ta = buffer_get_typedarray_data(item);
    
    if (!ta || ta->type != TYPED_ARRAY_UINT8) {
      char name[32];
      snprintf(name, sizeof(name), "list[%u]", (unsigned)i);
      return Ant_Error_CreateInvalidArgType(js, name, "an instance of Buffer or Uint8Array", item);
    }
    
    if (Ant_Buffer_ViewIsLive(ta)) total_length += ta->byte_length;
  }

  if (nargs > 1 && vtype(args[1]) == kTypeNumber) total_length = (size_t)js_getnum(args[1]);
  ArrayBufferData *buffer = create_array_buffer_data(js, total_length);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");

  size_t offset = 0;
  for (ant_offset_t i = 0; i < count; i++) {
    uint8_t *bytes;
    size_t len;
    
    if (!Ant_Buffer_GetViewBytes(buffer_get_typedarray_data(js_arr_get(js, list, i)), &bytes, &len)) {
      free_array_buffer_data(js, buffer);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform %%TypedArray%%.prototype.set on a detached or out-of-bounds ArrayBuffer");
    }
    
    if (len > total_length - offset) len = total_length - offset;
    memcpy(buffer->data + offset, bytes, len);
    offset += len;
  }

  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, total_length, js->builtins.buffer_proto);
}

static ant_value_t buffer_compare_arrays(ant_t *js, TypedArrayData *ta1, TypedArrayData *ta2) {
  if (!ta1 || !ta2) return js_mkerr(js, "Invalid buffer");
  
  uint8_t *a, *b; size_t a_len, b_len;
  Ant_Buffer_GetViewBytes(ta1, &a, &a_len);
  Ant_Buffer_GetViewBytes(ta2, &b, &b_len);
  
  size_t len = a_len < b_len ? a_len : b_len;
  int cmp = len ? memcmp(a, b, len) : 0;
  
  if (cmp == 0) {
    if (a_len < b_len) cmp = -1;
    else if (a_len > b_len) cmp = 1;
  } else cmp = cmp < 0 ? -1 : 1;
  
  return js_mknum((double)cmp);
}

// Buffer.prototype.compare(target)
static ant_value_t js_buffer_proto_compare(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Buffer.compare requires a target buffer");
  
  TypedArrayData *ta1 = buffer_get_typedarray_data(js_getthis(js));
  TypedArrayData *ta2 = buffer_get_typedarray_data(args[0]);
  if (!ta1 || !ta2) return js_mkerr(js, "Arguments must be Buffers");
  
  return buffer_compare_arrays(js, ta1, ta2);
}

// Buffer.prototype.equals(target)
static ant_value_t js_buffer_proto_equals(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Buffer.equals requires a target buffer");

  TypedArrayData *ta1 = buffer_get_typedarray_data(js_getthis(js));
  TypedArrayData *ta2 = buffer_get_typedarray_data(args[0]);
  if (!ta1 || !ta2) return js_mkerr(js, "Arguments must be Buffers");
  if (ta1->byte_length != ta2->byte_length) return js_bool(false);

  ant_value_t cmp = buffer_compare_arrays(js, ta1, ta2);
  if (is_err(cmp)) return cmp;
  return js_bool(js_getnum(cmp) == 0);
}

// Buffer.compare(buf1, buf2)
static ant_value_t js_buffer_compare(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "Buffer.compare requires two arguments");
  
  TypedArrayData *ta1 = buffer_get_typedarray_data(args[0]);
  TypedArrayData *ta2 = buffer_get_typedarray_data(args[1]);
  if (!ta1 || !ta2) return js_mkerr(js, "Arguments must be Buffers");
  
  return buffer_compare_arrays(js, ta1, ta2);
}

static ant_value_t js_sharedarraybuffer_constructor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "SharedArrayBuffer constructor requires 'new'");
    
  size_t length = 0;
  if (nargs > 0) {
    ant_value_t result = js_to_index_fast(js, args[0], &length);
    if (is_err(result)) return result;
  }
  
  ArrayBufferData *data = create_shared_array_buffer_data(length);
  if (!data) return js_mkerr(js, "Failed to allocate SharedArrayBuffer");
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js->builtins.sharedarraybuffer_proto;

  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(obj, arraybuffer_finalize);
  
  return obj;
}

static ant_value_t buffer_make_constants(ant_t *js) {
  ant_value_t constants = Ant_Object_Create(js);
  js_set(js, constants, "MAX_LENGTH", js_mknum(BUFFER_COMPAT_MAX_LENGTH));
  js_set(js, constants, "MAX_STRING_LENGTH", js_mknum(BUFFER_COMPAT_MAX_STRING_LENGTH));
  return constants;
}

ant_value_t buffer_library(ant_t *js) {
  ant_value_t glob = js_glob(js);
  ant_value_t lib = Ant_Object_Create(js);

  js_set(js, lib, "Buffer", js_get(js, glob, "Buffer"));
  js_set(js, lib, "Blob", js_get(js, glob, "Blob"));
  js_set(js, lib, "File", js_get(js, glob, "File"));
  js_set(js, lib, "atob", js_get(js, glob, "atob"));
  js_set(js, lib, "btoa", js_get(js, glob, "btoa"));
  js_set(js, lib, "constants", buffer_make_constants(js));
  js_set(js, lib, "kMaxLength", js_mknum(BUFFER_COMPAT_MAX_LENGTH));
  js_set(js, lib, "kStringMaxLength", js_mknum(BUFFER_COMPAT_MAX_STRING_LENGTH));
  js_set(js, lib, "INSPECT_MAX_BYTES", js_mknum(BUFFER_INSPECT_MAX_BYTES));

  return lib;
}

void init_buffer_module(ant_t *js) {
  ant_value_t glob = js->global;
  ant_value_t object_proto = js->sym.object_proto;
  ant_value_t function_proto = js->sym.function_proto;

  ant_value_t arraybuffer_ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t arraybuffer_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(arraybuffer_proto, object_proto);
  js->builtins.arraybuffer_proto = arraybuffer_proto;
  
  defmethod(js, arraybuffer_proto, "slice", 5, Ant_Function_CreateWithArity(js_arraybuffer_slice, 2));
  defmethod(js, arraybuffer_proto, "transfer", 8, Ant_Function_Create(js_arraybuffer_transfer));
  defmethod(js, arraybuffer_proto, "transferToFixedLength", 21, Ant_Function_Create(js_arraybuffer_transferToFixedLength));
  
  js_set_getter_desc(js, arraybuffer_proto, "detached", 8, Ant_Function_Create(js_arraybuffer_detached_getter), JS_DESC_C);
  js_set_getter_desc(js, arraybuffer_proto, "byteLength", 10, js_mkfun_meta(&arraybuffer_byte_length_getter.meta), JS_DESC_C);
  js_set_tostringtag(js, arraybuffer_proto, js_mkstr(js, "ArrayBuffer", 11));
  
  js_set_slot(arraybuffer_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(js_arraybuffer_constructor, 1));
  mkprop_exact_attrs(js, arraybuffer_ctor_obj, ANT_STRING("prototype"), arraybuffer_proto, 0);
  mkprop_exact_attrs(js, arraybuffer_ctor_obj, ANT_STRING("name"), ANT_STRING("ArrayBuffer"), ANT_PROP_ATTR_CONFIGURABLE);
  defmethod(js, arraybuffer_ctor_obj, "isView", 6, Ant_Function_CreateWithArity(js_arraybuffer_isView, 1));
  js_define_species_getter(js, arraybuffer_ctor_obj);
  
  ant_value_t arraybuffer_ctor = js_obj_to_func(js, arraybuffer_ctor_obj);
  defmethod(js, arraybuffer_proto, "constructor", 11, arraybuffer_ctor);
  js_set_global_builtin(js, "ArrayBuffer", arraybuffer_ctor);
  
  ant_value_t typedarray_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(typedarray_proto, object_proto);

  js_set_getter_desc(js, typedarray_proto, "buffer", 6, js_mkfun_meta(&typedarray_buffer_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, typedarray_proto, "length", 6, js_mkfun_meta(&typedarray_length_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, typedarray_proto, "byteLength", 10, js_mkfun_meta(&typedarray_byte_length_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, typedarray_proto, "byteOffset", 10, js_mkfun_meta(&typedarray_byte_offset_getter.meta), JS_DESC_C);
  
  defmethod(js, typedarray_proto, "at", 2, Ant_Function_CreateWithArity(js_typedarray_at, 1));
  defmethod(js, typedarray_proto, "set", 3, Ant_Function_CreateWithArity(js_typedarray_set, 1));
  defmethod(js, typedarray_proto, "copyWithin", 10, Ant_Function_CreateWithArity(js_typedarray_copyWithin, 2));
  defmethod(js, typedarray_proto, "slice", 5, Ant_Function_CreateWithArity(js_typedarray_slice, 2));
  defmethod(js, typedarray_proto, "subarray", 8, Ant_Function_CreateWithArity(js_typedarray_subarray, 2));
  defmethod(js, typedarray_proto, "fill", 4, Ant_Function_CreateWithArity(js_typedarray_fill, 1));
  defmethod(js, typedarray_proto, "toReversed", 10, Ant_Function_Create(js_typedarray_toReversed));
  defmethod(js, typedarray_proto, "toSorted", 8, Ant_Function_CreateWithArity(js_typedarray_toSorted, 1));
  defmethod(js, typedarray_proto, "with", 4, Ant_Function_CreateWithArity(js_typedarray_with, 2));
  defmethod(js, typedarray_proto, "toString", 8, Ant_Function_Create(js_typedarray_toString));
  defmethod(js, typedarray_proto, "join", 4, Ant_Function_CreateWithArity(js_typedarray_join, 1));
  defmethod(js, typedarray_proto, "indexOf", 7, Ant_Function_CreateWithArity(js_typedarray_indexOf, 1));
  defmethod(js, typedarray_proto, "lastIndexOf", 11, Ant_Function_CreateWithArity(js_typedarray_lastIndexOf, 1));
  defmethod(js, typedarray_proto, "includes", 8, Ant_Function_CreateWithArity(js_typedarray_includes, 1));
  defmethod(js, typedarray_proto, "every", 5, Ant_Function_CreateWithArity(js_typedarray_every, 1));
  defmethod(js, typedarray_proto, "filter", 6, Ant_Function_CreateWithArity(js_typedarray_filter, 1));
  defmethod(js, typedarray_proto, "find", 4, Ant_Function_CreateWithArity(js_typedarray_find, 1));
  defmethod(js, typedarray_proto, "findIndex", 9, Ant_Function_CreateWithArity(js_typedarray_findIndex, 1));
  defmethod(js, typedarray_proto, "forEach", 7, Ant_Function_CreateWithArity(js_typedarray_forEach, 1));
  defmethod(js, typedarray_proto, "map", 3, Ant_Function_CreateWithArity(js_typedarray_map, 1));
  defmethod(js, typedarray_proto, "reduce", 6, Ant_Function_CreateWithArity(js_typedarray_reduce, 1));
  defmethod(js, typedarray_proto, "reduceRight", 11, Ant_Function_CreateWithArity(js_typedarray_reduceRight, 1));
  defmethod(js, typedarray_proto, "reverse", 7, Ant_Function_Create(js_typedarray_reverse));
  defmethod(js, typedarray_proto, "some", 4, Ant_Function_CreateWithArity(js_typedarray_some, 1));
  defmethod(js, typedarray_proto, "sort", 4, Ant_Function_CreateWithArity(js_typedarray_sort, 1));
  js_set_sym_getter_desc(js, typedarray_proto, js->sym.toStringTag_sym, Ant_Function_Create(js_typedarray_toStringTag_getter), JS_DESC_C);

  ant_value_t typedarray_ctor_obj = Ant_Object_CreateBare(js);
  js_set_proto_init(typedarray_ctor_obj, function_proto);
  js_set_slot(typedarray_ctor_obj, SLOT_CFUNC, Ant_Function_Create(js_typedarray_base_constructor));
  mkprop(js, typedarray_ctor_obj, js->length_str, js_mknum(0), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop(js, typedarray_ctor_obj, ANT_STRING("name"), ANT_STRING("TypedArray"), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop_exact_attrs(js, typedarray_ctor_obj, ANT_STRING("prototype"), typedarray_proto, 0);
  
  defmethod(js, typedarray_ctor_obj, "of", 2, Ant_Function_CreateWithArity(js_typedarray_static_of, 0));
  defmethod(js, typedarray_ctor_obj, "from", 4, Ant_Function_CreateWithArity(js_typedarray_static_from, 1));
  js_define_species_getter(js, typedarray_ctor_obj);
  
  ant_value_t typedarray_ctor = js_obj_to_func(js, typedarray_ctor_obj);
  defmethod(js, typedarray_proto, "constructor", 11, typedarray_ctor);

  js->builtins.typedarray_iter_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.typedarray_iter_proto, js->sym.iterator_proto);
  defmethod(js, js->builtins.typedarray_iter_proto, "next", 4, Ant_Function_Create(ta_iter_next));
  js_iter_register_advance(js, js->builtins.typedarray_iter_proto, advance_typedarray);

  defmethod(js, typedarray_proto, "values", 6, Ant_Function_Create(ta_values));
  defmethod(js, typedarray_proto, "keys", 4, Ant_Function_Create(ta_keys));
  defmethod(js, typedarray_proto, "entries", 7, Ant_Function_Create(ta_entries));
  defalias_sym_of(js, typedarray_proto, js->sym.iterator_sym, "values");
  
  for (TypedArrayType type = TYPED_ARRAY_INT8; type <= TYPED_ARRAY_BIGUINT64; type++)
    setup_typedarray_kind(js, type, typedarray_ctor, typedarray_proto);

  ant_value_t uint8array_codec_ctor = js_get(js, glob, "Uint8Array");
  ant_value_t uint8array_codec_proto = js_get(js, uint8array_codec_ctor, "prototype");
  
  defmethod(js, uint8array_codec_ctor, "fromHex", 7, Ant_Function_CreateWithArity(js_uint8array_fromHex, 1));
  defmethod(js, uint8array_codec_ctor, "fromBase64", 10, Ant_Function_CreateWithArity(js_uint8array_fromBase64, 1));
  defmethod(js, uint8array_codec_proto, "toHex", 5, Ant_Function_Create(js_uint8array_toHex));
  defmethod(js, uint8array_codec_proto, "toBase64", 8, Ant_Function_Create(js_uint8array_toBase64));
  defmethod(js, uint8array_codec_proto, "setFromHex", 10, Ant_Function_CreateWithArity(js_uint8array_setFromHex, 1));
  defmethod(js, uint8array_codec_proto, "setFromBase64", 13, Ant_Function_CreateWithArity(js_uint8array_setFromBase64, 1));
  
  ant_value_t dataview_ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t dataview_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(dataview_proto, object_proto);
  js->builtins.dataview_proto = dataview_proto;
  
  js_define_methods(js, dataview_proto, dataview_methods, countof(dataview_methods), ANT_PROP_ATTR_METHOD);
  
  js_set_getter_desc(js, dataview_proto, "buffer", 6, js_mkfun_meta(&dataview_buffer_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, dataview_proto, "byteLength", 10, js_mkfun_meta(&dataview_byte_length_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, dataview_proto, "byteOffset", 10, js_mkfun_meta(&dataview_byte_offset_getter.meta), JS_DESC_C);
  js_set_tostringtag(js, dataview_proto, js_mkstr(js, "DataView", 8));

  js_set_slot(dataview_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(js_dataview_constructor, 1));
  mkprop_exact_attrs(js, dataview_ctor_obj, ANT_STRING("prototype"), dataview_proto, 0);
  mkprop_exact_attrs(js, dataview_ctor_obj, ANT_STRING("name"), ANT_STRING("DataView"), ANT_PROP_ATTR_CONFIGURABLE);
  
  ant_value_t dataview_ctor = js_obj_to_func(js, dataview_ctor_obj);
  defmethod(js, dataview_proto, "constructor", 11, dataview_ctor);
  js_set_global_builtin(js, "DataView", dataview_ctor);
  
  ant_value_t sharedarraybuffer_ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t sharedarraybuffer_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(sharedarraybuffer_proto, object_proto);
  js->builtins.sharedarraybuffer_proto = sharedarraybuffer_proto;
  
  defmethod(js, sharedarraybuffer_proto, "slice", 5, Ant_Function_CreateWithArity(js_arraybuffer_slice, 2));
  js_set_getter_desc(js, sharedarraybuffer_proto, "byteLength", 10, js_mkfun_meta(&sharedarraybuffer_byte_length_getter.meta), JS_DESC_C);
  js_set_tostringtag(js, sharedarraybuffer_proto, js_mkstr(js, "SharedArrayBuffer", 17));
  
  js_set_slot(sharedarraybuffer_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(js_sharedarraybuffer_constructor, 1));
  mkprop_exact_attrs(js, sharedarraybuffer_ctor_obj, ANT_STRING("prototype"), sharedarraybuffer_proto, 0);
  mkprop_exact_attrs(js, sharedarraybuffer_ctor_obj, ANT_STRING("name"), ANT_STRING("SharedArrayBuffer"), ANT_PROP_ATTR_CONFIGURABLE);
  js_define_species_getter(js, sharedarraybuffer_ctor_obj);
  
  ant_value_t sharedarraybuffer_ctor = js_obj_to_func(js, sharedarraybuffer_ctor_obj);
  defmethod(js, sharedarraybuffer_proto, "constructor", 11, sharedarraybuffer_ctor);
  js_set_global_builtin(js, "SharedArrayBuffer", sharedarraybuffer_ctor);
  
  ant_value_t buffer_ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t buffer_proto = Ant_Object_CreateBare(js);
  if (!is_object_type(js->builtins.buffer_proto)) js->builtins.buffer_proto = buffer_proto;
  
  ant_value_t uint8array_ctor = js_get(js, glob, "Uint8Array");
  ant_value_t uint8array_proto = js_get(js, uint8array_ctor, "prototype");
  
  if (is_special_object(uint8array_proto)) js_set_proto_init(buffer_proto, uint8array_proto);
  else js_set_proto_init(buffer_proto, typedarray_proto);
  
  static const ant_cfunc_meta_t buffer_proto_methods[] = {
    { js_buffer_slice,         "slice",       2, 0 },
    { js_buffer_toString,      "toString",    3, 0 },
    { js_buffer_toJSON,        "toJSON",      0, 0 },
    { js_buffer_utf8Slice,     "utf8Slice",   2, 0 },
    { js_buffer_toBase64,      "toBase64",    0, 0 },
    { js_buffer_indexOf,       "indexOf",     4, 0 },
    { js_buffer_lastIndexOf,   "lastIndexOf", 4, 0 },
    { js_buffer_includes,      "includes",    4, 0 },
    { js_buffer_proto_compare, "compare",     5, 0 },
    { js_buffer_proto_equals,  "equals",      1, 0 },
    { js_buffer_write,         "write",       4, 0 },
    { js_buffer_fill,          "fill",        4, 0 },
    { js_buffer_copy,          "copy",        4, 0 },
  };
  
  js_define_methods(js, buffer_proto, buffer_proto_methods, countof(buffer_proto_methods), ANT_PROP_ATTR_DEFAULT);
  mkprop_interned_exact(js, buffer_proto, intern_string("toLocaleString", 14), js_get(js, buffer_proto, "toString"), ANT_PROP_ATTR_DEFAULT);
  
  for (size_t i = 0; i < sizeof(buffer_num_methods) / sizeof(buffer_num_methods[0]); i++) {
    const buffer_num_method_t *m = &buffer_num_methods[i];
    ant_value_t fn = js_mkfun_meta(&m->meta);
    if (m->alias) fn = js_cfunc_promote(js, fn);
    js_set(js, buffer_proto, m->meta.name, fn);
    if (m->alias) js_set(js, buffer_proto, m->alias, fn);
  }
  
  js_set(js, buffer_proto, "values", js_get(js, typedarray_proto, "values"));
  js_set_sym(js, buffer_proto, js->sym.iterator_sym, js_get(js, buffer_proto, "values"));
  
  if (vtype(uint8array_ctor) == kTypeFunction) js_set_proto_init(buffer_ctor_obj, uint8array_ctor);
  js_set(js, buffer_ctor_obj, "poolSize", js_mknum(8192));
  js_set(js, buffer_ctor_obj, "from", Ant_Function_CreateWithArity(js_buffer_from, 3));
  js_set(js, buffer_ctor_obj, "copyBytesFrom", Ant_Function_CreateWithArity(js_buffer_copyBytesFrom, 3));
  js_set(js, buffer_ctor_obj, "of", Ant_Function_Create(js_buffer_of));
  js_set(js, buffer_ctor_obj, "alloc", Ant_Function_CreateWithArity(js_buffer_alloc, 3));
  js_set(js, buffer_ctor_obj, "allocUnsafe", Ant_Function_CreateWithArity(js_buffer_allocUnsafe, 1));
  js_set(js, buffer_ctor_obj, "allocUnsafeSlow", Ant_Function_CreateWithArity(js_buffer_allocUnsafeSlow, 1));
  js_set(js, buffer_ctor_obj, "isBuffer", Ant_Function_CreateWithArity(js_buffer_isBuffer, 1));
  js_set(js, buffer_ctor_obj, "compare", Ant_Function_CreateWithArity(js_buffer_compare, 2));
  js_set(js, buffer_ctor_obj, "isEncoding", Ant_Function_CreateWithArity(js_buffer_isEncoding, 1));
  js_set(js, buffer_ctor_obj, "concat", Ant_Function_CreateWithArity(js_buffer_concat, 2));
  js_set(js, buffer_ctor_obj, "byteLength", Ant_Function_CreateWithArity(js_buffer_byteLength, 2));
  js_define_species_getter(js, buffer_ctor_obj);

  js_set_slot(buffer_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(js_buffer_from, 3));
  mkprop_exact_attrs(js, buffer_ctor_obj, ANT_STRING("prototype"), buffer_proto, ANT_PROP_ATTR_WRITABLE);
  mkprop_exact_attrs(js, buffer_ctor_obj, ANT_STRING("name"), ANT_STRING("Buffer"), ANT_PROP_ATTR_CONFIGURABLE);
  
  ant_value_t buffer_ctor = js_obj_to_func(js, buffer_ctor_obj);
  defmethod(js, buffer_proto, "constructor", 11, buffer_ctor);
  js_set_global_builtin(js, "Buffer", buffer_ctor);
}

void cleanup_buffer_module(ant_t *js) {
  for (size_t i = 0; i < js->buffers.count; i++) free(js->buffers.entries[i]);
  free(js->buffers.entries);
  js->buffers.entries = NULL;
  js->buffers.count = 0;
  js->buffers.cap = 0;
  js->buffers.bytes = 0;
  js->buffers.meta_bytes = 0;
}

size_t buffer_get_external_memory(ant_t *js) {
  return
    js->buffers.meta_bytes + js->buffers.bytes +
    js->buffers.cap * sizeof(ArrayBufferData *) +
    js->buffers.count * sizeof(ArrayBufferData);
}
