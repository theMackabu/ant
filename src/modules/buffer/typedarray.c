#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "index.h"
#include "errors.h"
#include "internal.h"
#include "protectors.h"
#include "numbers.h"
#include "gc/roots.h"
#include "descriptors.h"
#include "silver/call.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "native_getter.h"
#include "modules/symbol.h"
#include "modules/iterator.h"
#include "buffer_internal.h"

static __attribute__((noinline)) TypedArrayData *buffer_get_typedarray_data_slow(ant_value_t value) {
  if (vtype(value) == kTypeTypedArray)
    return (TypedArrayData *)js_gettypedarray(value);
  if (!is_object_type(value)) return NULL;
  return (TypedArrayData *)js_get_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);
}

TypedArrayData *buffer_get_typedarray_data(ant_value_t value) {
  if (vtype(value) == kTypeObject) {
    ant_object_t *obj = (ant_object_t *)vptr(value);
    if (obj->native_tag == BUFFER_TYPEDARRAY_NATIVE_TAG) return obj->native_ptr;
  }
  return buffer_get_typedarray_data_slow(value);
}

ArrayBufferData buffer_out_of_bounds = { 
  .is_detached = 1, 
  .views_slow = 1
};

__attribute__((noinline, cold)) TypedArrayData *typedarray_live_slow(TypedArrayData *ta) {
  if (buffer_is_growable_shared(ta->backing)) typedarray_view_refresh(ta);
  return Ant_Buffer_ViewIsLive(ta) ? ta : NULL;
}

__attribute__((noinline, cold)) ant_value_t typedarray_this_error(ant_t *js, ant_value_t value) {
  return buffer_get_typedarray_data(value)
    ? js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform this operation on a detached ArrayBuffer")
    : js_mkerr_typed(js, JS_ERR_TYPE, "this is not a typed array");
}

static void typedarray_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  TypedArrayData *ta_data = (TypedArrayData *)js_get_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);
  if (!ta_data) return;
  js_clear_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);

  buffer_view_release(js, ta_data->backing, ta_data);
  ta_meta_free(js, ta_data, sizeof(*ta_data));
}

bool buffer_typedarray_data_read_index(ant_t *js, const TypedArrayData *ta_data, size_t index, ant_value_t *out) {
  uint8_t *data; size_t byte_length;
  if (!out || !Ant_Buffer_GetViewBytes(ta_data, &data, &byte_length) || index >= ta_data->length) return false;

  #define BUFFER_TA_READ_INT(ctype, load) *out = Ant_Value_FromNumber(load(((ctype *)data)[index])); return true;
  #define BUFFER_TA_READ_FLOAT BUFFER_TA_READ_INT
  #define BUFFER_TA_READ_BIGINT(ctype, load) *out = load(js, ((ctype *)data)[index]); return !is_err(*out);
  #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) case TYPED_ARRAY_##name: BUFFER_TA_READ_##kind(ctype, load)
  switch (ta_data->type) {
    #include "buffer/typed_arrays.h"
    default: return false;
  }
  #undef BUFFER_TA_READ_INT
  #undef BUFFER_TA_READ_FLOAT
  #undef BUFFER_TA_READ_BIGINT
}

bool buffer_typedarray_read_index(ant_t *js, ant_value_t value, size_t index, ant_value_t *out) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(value);
  return buffer_typedarray_data_read_index(js, ta_data, index, out);
}

void typedarray_view_refresh(TypedArrayData *ta) {
  ArrayBufferData *backing = ta->backing;
  size_t size = get_element_size(ta->type);
  size_t total = buffer_byte_length(backing);
  size_t room = ta->byte_offset <= total ? (total - ta->byte_offset) / size : 0;
  bool fits = ta->byte_offset <= total && (ta->length_tracking || ta->base_length <= room);
  size_t length = ta->length_tracking ? room : ta->base_length;

  ta->buffer = fits ? backing : &buffer_out_of_bounds;
  ta->length = fits ? length : 0;
  ta->byte_length = fits ? length * size : 0;
}

static __attribute__((noinline, cold)) void typedarray_view_register(ant_t *js, TypedArrayData *ta) {
  buffer_views_add(js, ta->backing, ta, false);
  typedarray_view_refresh(ta);
}

static inline __attribute__((always_inline)) void typedarray_view_attach(
  ant_t *js, TypedArrayData *ta, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, bool tracking
) {
  ta->backing = buffer;
  ta->buffer = buffer;
  ta->byte_offset = byte_offset;
  ta->base_length = length;
  ta->length_tracking = tracking && buffer->resizable;
  ta->length = length;
  ta->byte_length = length * get_element_size(ta->type);
  buffer->ref_count++;

  if (__builtin_expect(buffer->resizable, 0)) typedarray_view_register(js, ta);
}

const char *buffer_typedarray_type_name(TypedArrayType type) {
  static const char *const names[] = {
    #define BUFFER_TYPED_ARRAY(name, ctor, ...) #ctor "Array",
    #include "buffer/typed_arrays.h"
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
  TypedArrayData *ta = buffer_get_typedarray_data(call_this);
  if (!ta) return Ant_Value_Undefined;

  ant_value_t *cached = &js->mutable_roots.typedarray_names[ta->type];
  if (*cached) return *cached;
  
  const char *name = buffer_typedarray_type_name(ta->type);
  ant_value_t str = js_mkstr(js, name, strlen(name));
  if (!is_err(str)) *cached = str;
  
  return str;
}

ant_value_t create_typed_array_like(
  ant_t *js,
  ant_value_t this_val,
  TypedArrayType type,
  ArrayBufferData *buffer,
  size_t byte_offset,
  size_t length
) {
  TypedArrayData *source = buffer_get_typedarray_data(this_val);
  ant_value_t shared = source && source->buffer == buffer ? js_get_slot(this_val, SLOT_DATA) : Ant_Value_Undefined;
  
  ant_value_t ab_obj = is_object_type(shared) ? shared : create_arraybuffer_obj(js, buffer);
  ant_value_t out = create_typed_array_with_buffer(
    js,type, buffer, byte_offset,
    length, js->builtins.typedarray_protos[type], ab_obj, false
  );
  
  if (is_err(out)) return out;
  ant_value_t proto = js_get_proto(js, this_val);
  if (is_special_object(proto)) js_set_proto_init(out, proto);
  
  return out;
}

ant_value_t buffer_unwrap_bigint(ant_value_t value) {
  if (!is_object_type(value)) return value;
  ant_value_t primitive = js_get_slot(value, SLOT_PRIMITIVE);
  return vtype(primitive) == kTypeBigInt ? primitive : value;
}

ant_value_t buffer_to_number_value(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeNumber) return value;
  
  if (is_object_type(value) || vtype(value) == kTypeBuiltin) {
    value = js_to_primitive(js, value, 2);
    if (is_err(value)) return value;
  }
  
  if (vtype(value) == kTypeBigInt) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
  double number = js_to_number(js, value);
  
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : Ant_Value_FromNumber(number);
}

ant_value_t typedarray_coerce_value(ant_t *js, const TypedArrayData *ta, ant_value_t value) {
  if (typedarray_type_is_bigint(ta->type)) return bigint_to_bigint(js, value);
  return buffer_to_number_value(js, value);
}

ant_value_t typedarray_write_value(ant_t *js, TypedArrayData *ta_data, size_t index, ant_value_t value) {
  value = typedarray_coerce_value(js, ta_data, value);
  if (is_err(value)) return value;

  double num = vtype(value) == kTypeNumber ? Ant_Value_AsNumberUnchecked(value) : 0;
  uint64_t bits = 0;

  if (typedarray_type_is_bigint(ta_data->type)) {
    bool ok = ta_data->type == TYPED_ARRAY_BIGINT64
      ? bigint_to_int64_wrapping(js, value, (int64_t *)&bits)
      : bigint_to_uint64_wrapping(js, value, &bits);
    
    if (!ok) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  }

  uint8_t *data; size_t byte_length;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &byte_length) || index >= ta_data->length) return Ant_Value_Undefined;

  #define BUFFER_TA_WRITE_INT(ctype, store) ((ctype *)data)[index] = store(num); break;
  #define BUFFER_TA_WRITE_FLOAT BUFFER_TA_WRITE_INT
  #define BUFFER_TA_WRITE_BIGINT(ctype, store) ((ctype *)data)[index] = store(bits); break;
  #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) case TYPED_ARRAY_##name: BUFFER_TA_WRITE_##kind(ctype, store)

  switch (ta_data->type) {
    #include "buffer/typed_arrays.h"
    default: break;
  }

  #undef BUFFER_TA_WRITE_INT
  #undef BUFFER_TA_WRITE_FLOAT
  #undef BUFFER_TA_WRITE_BIGINT

  return Ant_Value_Undefined;
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
  size_t canonical_len = js_number_to_chars(Ant_Value_FromNumber(number), canonical, sizeof(canonical));
  return canonical_len == len && memcmp(canonical, key, len) == 0 ? TYPEDARRAY_KEY_INVALID : TYPEDARRAY_KEY_ORDINARY;
}

static ant_value_t typedarray_index_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  if (!ant_key_may_be_numeric(key, key_len)) return Ant_Value_Undefined;
  TypedArrayData *ta_data = buffer_get_typedarray_data(obj);
  size_t index = 0;
  typedarray_key_kind_t kind = ta_data ? typedarray_key_kind(ta_data, key, key_len, &index) : TYPEDARRAY_KEY_ORDINARY;
  if (kind == TYPEDARRAY_KEY_ORDINARY) return Ant_Value_Undefined;

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
  if (!ant_key_may_be_numeric(key, key_len)) return Ant_Value_Undefined;
  TypedArrayData *ta_data = buffer_get_typedarray_data(obj);
  size_t index = 0;
  typedarray_key_kind_t kind = ta_data ? typedarray_key_kind(ta_data, key, key_len, &index) : TYPEDARRAY_KEY_ORDINARY;
  if (kind == TYPEDARRAY_KEY_ORDINARY) return Ant_Value_Undefined;
  if (kind == TYPEDARRAY_KEY_INVALID || !plain_data) return T_EMPTY;
  if (!has_value) return Ant_Value_True;
  
  ant_value_t coerced = typedarray_coerce_value(js, ta_data, value);
  if (is_err(coerced)) return coerced;
  (void)typedarray_write_value(js, ta_data, index, coerced);
  return Ant_Value_True;
}

static const ant_exotic_ops_t typedarray_exotic_ops = {
  .getter = typedarray_index_getter,
  .setter = typedarray_index_setter,
  .deleter = typedarray_index_deleter,
  .definer = typedarray_index_definer,
  .keys = ANT_EXOTIC_KEYS_NUMERIC,
};

const void *buffer_typedarray_exotic_ops(void) {
  return &typedarray_exotic_ops;
}

static inline TypedArrayData *typedarray_fast_data(ant_value_t obj) {
  ant_object_t *ptr = vtype(obj) == kTypeObject ? js_obj_ptr(obj) : NULL;
  if (!ptr || ant_object_exotic_ops(ptr) != &typedarray_exotic_ops || ptr->native_tag != BUFFER_TYPEDARRAY_NATIVE_TAG) return NULL;
  return ptr->native_ptr;
}

__attribute__((noinline)) bool buffer_typedarray_get_index(ant_t *js, ant_value_t obj, uint32_t index, ant_value_t *out) {
  TypedArrayData *ta_data = typedarray_fast_data(obj);
  if (!ta_data) return false;
  if (ta_data->buffer && __builtin_expect(ta_data->buffer->views_slow, 0)) typedarray_live_slow(ta_data);
  
  if (!Ant_Buffer_ViewIsLive(ta_data) || index >= ta_data->length) {
    *out = Ant_Value_Undefined;
    return true;
  }
  
  if (!buffer_typedarray_data_read_index(js, ta_data, index, out) && !is_err(*out)) *out = Ant_Value_Undefined;
  
  return true;
}

static buffer_ta_access_t typedarray_inline_access(
  ant_value_t obj, ant_value_t key, TypedArrayData **out, TypedArrayType *type
) {
  TypedArrayData *ta_data = typedarray_fast_data(obj);
  if (!ta_data) return BUFFER_TA_ACCESS_NONE;
  
  *type = ta_data->type;
  if (!Ant_Buffer_ViewIsLive(ta_data) || vtype(key) != kTypeNumber) return BUFFER_TA_ACCESS_SLOW;
  
  double index = Ant_Value_AsNumberUnchecked(key);
  if (!(index >= 0 && index < (double)UINT32_MAX) || trunc(index) != index || (size_t)index >= ta_data->length)
    return BUFFER_TA_ACCESS_SLOW;
  
  *out = ta_data;
  switch (ta_data->type) {
    case TYPED_ARRAY_FLOAT16: case TYPED_ARRAY_BIGINT64: case TYPED_ARRAY_BIGUINT64: return BUFFER_TA_ACCESS_SLOW;
    default: return BUFFER_TA_ACCESS_INLINE;
  }
}

__attribute__((noinline)) buffer_ta_access_t buffer_typedarray_inline_get(
  ant_value_t obj, ant_value_t key, TypedArrayType *type
) {
  TypedArrayData *ta_data;
  return typedarray_inline_access(obj, key, &ta_data, type);
}

__attribute__((noinline)) buffer_ta_access_t buffer_typedarray_inline_put(
  ant_value_t obj, ant_value_t key, ant_value_t number, TypedArrayType *type
) {
  TypedArrayData *ta_data;
  buffer_ta_access_t access = typedarray_inline_access(obj, key, &ta_data, type);
  
  if (access != BUFFER_TA_ACCESS_INLINE) return access;
  if (vtype(number) != kTypeNumber) return BUFFER_TA_ACCESS_SLOW;
  if (ta_data->type == TYPED_ARRAY_FLOAT32 || ta_data->type == TYPED_ARRAY_FLOAT64) return BUFFER_TA_ACCESS_INLINE;
  
  double value = Ant_Value_AsNumberUnchecked(number);
  if (!(fabs(value) < 0x1p63) || trunc(value) != value) return BUFFER_TA_ACCESS_SLOW;
  
  return ta_data->type != TYPED_ARRAY_UINT8_CLAMPED || (value >= 0 && value <= UINT8_MAX)
    ? BUFFER_TA_ACCESS_INLINE : BUFFER_TA_ACCESS_SLOW;
}

__attribute__((noinline)) bool buffer_typedarray_set_index(ant_t *js, ant_value_t obj, uint32_t index, ant_value_t value, ant_value_t *result) {
  TypedArrayData *ta_data = typedarray_fast_data(obj);
  if (!ta_data) return false;
  
  ant_value_t coerced = typedarray_coerce_value(js, ta_data, value);
  if (is_err(coerced)) {
    *result = coerced;
    return true;
  }
  
  (void)typedarray_write_value(js, ta_data, index, coerced);
  *result = value;
  
  return true;
}

static inline __attribute__((always_inline)) ant_value_t create_typed_array_view(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, ant_value_t proto, ant_value_t arraybuffer_obj, bool tracking
) {
  TypedArrayData *ta_data = ta_meta_alloc(js, sizeof(TypedArrayData));
  if (!ta_data) return js_mkerr(js, "Failed to allocate TypedArray");

  ta_data->type = type;
  typedarray_view_attach(js, ta_data, buffer, byte_offset, length, tracking);
  
  ant_value_t obj = is_special_object(proto) 
    ? js_mkobj_with_proto(js, proto) 
    : Ant_Object_CreateBare(js);
  
  js_set_native(obj, ta_data, BUFFER_TYPEDARRAY_NATIVE_TAG);
  js_set_slot_wb(js, obj, SLOT_DATA, arraybuffer_obj);
  ant_typed_array_protector_holder_mark(obj);
  
  js_set_exotic_ops(obj, &typedarray_exotic_ops);
  js_set_finalizer(obj, typedarray_finalize);

  return obj;
}

ant_value_t create_typed_array_with_buffer(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, ant_value_t proto, ant_value_t arraybuffer_obj, bool tracking
) {
  return create_typed_array_view(js, type, buffer, byte_offset, length, proto, arraybuffer_obj, tracking);
}

ant_value_t create_typed_array(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, ant_value_t proto
) {
  ant_value_t ab_obj = create_arraybuffer_obj(js, buffer);
  ant_value_t result = create_typed_array_with_buffer(js, type, buffer, byte_offset, length, proto, ab_obj, false);
  free_array_buffer_data(js, buffer); return result;
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

static void typedarray_store_numbers(TypedArrayData *ta, const ant_value_t *src, size_t count) {
  uint8_t *data; size_t byte_length;
  if (!Ant_Buffer_GetViewBytes(ta, &data, &byte_length) || count > ta->length) return;

  #define BUFFER_TA_STORE_INT(ctype, store)                                                             \
    for (size_t i = 0; i < count; i++) ((ctype *)data)[i] = store(Ant_Value_AsNumberUnchecked(src[i])); \
    return;
  #define BUFFER_TA_STORE_FLOAT BUFFER_TA_STORE_INT
  #define BUFFER_TA_STORE_BIGINT(ctype, store) return;
  #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) case TYPED_ARRAY_##name: BUFFER_TA_STORE_##kind(ctype, store)

  switch (ta->type) {
    #include "buffer/typed_arrays.h"
    default: return;
  }

  #undef BUFFER_TA_STORE_INT
  #undef BUFFER_TA_STORE_FLOAT
  #undef BUFFER_TA_STORE_BIGINT
}

static bool typedarray_from_dense_numbers(
  ant_t *js, TypedArrayType type, ant_value_t source,
  ant_value_t proto, ant_value_t *out
) {
  if (typedarray_type_is_bigint(type)) return false;
  if (!js_is_array(source) || !js_array_iteration_default(js, source)) return false;
  if (!typedarray_dense_number_source(source)) return false;
  
  ant_object_t *array = js_obj_ptr(js_as_obj(source));
  *out = create_typed_array_for_length(js, type, array->u.array.len, proto);
  if (!is_err(*out)) typedarray_store_numbers(buffer_get_typedarray_data(*out), array->u.array.data, array->u.array.len);
  
  return true;
}

static ant_value_t create_typed_array_from_iterable(
  ant_t *js, TypedArrayType type, ant_value_t source,
  ant_value_t iter_fn, ant_value_t proto
) {
  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);
  
  ant_value_t result = sv_vm_call(js->vm, js, iter_fn, source, NULL, 0, NULL, Ant_Value_Undefined);
  if (is_err(result)) goto done;
  
  if (!is_object_type(result)) {
    result = js_mkerr_typed(js, JS_ERR_TYPE, "Iterator must be an object");
    goto done;
  }
  
  ant_value_t iterator = result;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&roots, iterator))) goto oom;
  
  ant_value_t next = js_getprop_fallback(js, iterator, "next");
  if (is_err(next)) { result = next; goto done; }

  if (!gc_temp_root_handle_valid(gc_temp_root_add(&roots, next))) goto oom;
  size_t values_start = roots.len;

  while (true) {
    result = sv_vm_call(js->vm, js, next, iterator, NULL, 0, NULL, Ant_Value_Undefined);
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
    bool tracking = !has_length && buffer->resizable;
    if (has_length) {
      if (length > available / element_size)
        return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid TypedArray length");
    } else {
      if (!tracking && available % element_size != 0)
        return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid TypedArray length");
      length = available / element_size;
    }

    return create_typed_array_view(js, type, buffer, byte_offset, length, proto, args[0], tracking);
  }

  uint8_t arg_type = vtype(args[0]);
  if (is_boxable_primitive_type(arg_type) || arg_type == kTypeNull || arg_type == kTypeUndefined) {
    size_t length = 0;
    ant_value_t result = js_to_index_fast(js, args[0], &length);
    if (is_err(result)) return result;
    return create_typed_array_for_length(js, type, length, proto);
  }

  TypedArrayData *source = buffer_get_typedarray_data(args[0]);
  if (source) {
    if (!Ant_Buffer_ViewIsLive(source))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot copy a detached TypedArray");
    
    bool source_bigint = typedarray_type_is_bigint(source->type);
    bool target_bigint = typedarray_type_is_bigint(type);
    
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
      ant_value_t value = Ant_Value_Undefined;
      buffer_typedarray_data_read_index(js, source, i, &value);
      ant_value_t written = typedarray_write_value(js, target, i, value);
      if (is_err(written)) return written;
    }
    
    return result;
  }

  if (is_special_object(args[0])) {
    ant_value_t dense = Ant_Value_Undefined;
    if (typedarray_from_dense_numbers(js, type, args[0], proto, &dense)) return dense;
    
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

static ant_value_t js_typedarray_from(ant_native_params_t, TypedArrayType type, const char *type_name) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "%s.from requires at least 1 argument", type_name);

  ant_value_t source = args[0];
  bool has_map = nargs >= 2 && vtype(args[1]) != kTypeUndefined;
  
  ant_value_t map_fn = Ant_Value_Undefined;
  ant_value_t this_arg = nargs >= 3 ? args[2] : Ant_Value_Undefined;
  ant_value_t result = Ant_Value_Undefined;
  ant_value_t *collected = NULL;
  
  gc_temp_root_scope_t temp_roots = {0};

  if (has_map) {
    if (!is_callable(args[1])) return js_mkerr_typed(js, JS_ERR_TYPE, "%s.from: mapFn is not a function", type_name);
    map_fn = args[1];
  }

  if (!has_map && typedarray_from_dense_numbers(js, type, source, js->builtins.typedarray_protos[type], &result)) return result;
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
        ant_value_t map_args[2] = { item, Ant_Value_FromNumber((double)count) };
        item = sv_vm_call(js->vm, js, map_fn, this_arg, map_args, 2, NULL, Ant_Value_Undefined);
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

    if (is_err(result)) {
      Ant_Exception_Set(js, result);
      js_iter_close(js, &it);
    }
  } else if (!Ant_Exception_Pending(js)) {
    ant_value_t len_val = js_get(js, source, "length");
    if (is_err(len_val)) {
      result = len_val;
      goto done;
    }

    size_t len = vtype(len_val) == kTypeNumber
      ? (size_t)Ant_Value_AsNumberUnchecked(len_val) : 0;

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
        ant_value_t map_args[2] = { item, Ant_Value_FromNumber((double)i) };
        item = sv_vm_call(js->vm, js, map_fn, this_arg, map_args, 2, NULL, Ant_Value_Undefined);
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

ant_value_t js_typedarray_of(ant_native_params_t, TypedArrayType type, ant_value_t proto) {
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
  TypedArrayType type = (TypedArrayType)Ant_Value_AsNumberUnchecked(js_get_slot(call_func, SLOT_AUX));
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
      *out = (TypedArrayType)Ant_Value_AsNumberUnchecked(js_get_slot(cur, SLOT_AUX));
      return true;
    }
    
    cur = js_get_proto(js, cur);
  }
  
  return false;
}

static ant_value_t typedarray_adopt_this(ant_t *js, ant_value_t call_this, ant_value_t result) {
  if (is_err(result)) return result;
  ant_value_t proto = js_instance_proto_from_new_target(js, js_get_proto(js, result), call_this);
  if (is_special_object(proto)) js_set_proto_wb(js, result, proto);
  return result;
}

static ant_value_t js_typedarray_static_from(ant_params_t) {
  TypedArrayType type;
  if (!typedarray_ctor_kind(js, call_this, &type)) return js_mkerr_typed(js, JS_ERR_TYPE, "this is not a typed array constructor");
  return typedarray_adopt_this(js, call_this, js_typedarray_from(js, args, nargs, type, buffer_typedarray_type_name(type)));
}

static ant_value_t js_typedarray_static_of(ant_params_t) {
  TypedArrayType type;
  if (!typedarray_ctor_kind(js, call_this, &type)) return js_mkerr_typed(js, JS_ERR_TYPE, "this is not a typed array constructor");
  return typedarray_adopt_this(js, call_this, js_typedarray_of(js, args, nargs, type, js->builtins.typedarray_protos[type]));
}

static void setup_typedarray_kind(ant_t *js, TypedArrayType type, ant_value_t base_ctor, ant_value_t base_proto) {
  const char *name = buffer_typedarray_type_name(type);
  ant_value_t ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t proto = Ant_Object_CreateBare(js);
  ant_value_t elem_size = Ant_Value_FromNumber((double)get_element_size(type));
  
  js_set_proto_init(ctor_obj, base_ctor);
  js_set_proto_init(proto, base_proto);
  js->builtins.typedarray_protos[type] = proto;
  js_set_slot(ctor_obj, SLOT_CFUNC, Ant_Function_Create(js_typedarray_kind_constructor));
  js_set_slot(ctor_obj, SLOT_AUX, Ant_Value_FromNumber((double)type));
  
  mkprop(js, ctor_obj, js->length_str, Ant_Value_FromNumber(3), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop(js, ctor_obj, Ant_String_FromLiteral(js, "name"), js_mkstr(js, name, strlen(name)), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop_exact_attrs(js, ctor_obj, Ant_String_FromLiteral(js, "prototype"), proto, 0);
  mkprop_exact_attrs(js, ctor_obj, Ant_String_FromLiteral(js, "BYTES_PER_ELEMENT"), elem_size, 0);
  
  ant_value_t ctor = js_obj_to_func(js, ctor_obj);
  mkprop(js, proto, Ant_String_FromLiteral(js, "constructor"), ctor, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);
  mkprop_exact_attrs(js, proto, Ant_String_FromLiteral(js, "BYTES_PER_ELEMENT"), elem_size, 0);
  js_set_global_builtin(js, name, ctor);
}

ant_value_t typedarray_initialize(ant_t *js) {
  ant_value_t object_proto = js->sym.object_proto;
  ant_value_t function_proto = js->sym.function_proto;

  ant_value_t typedarray_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(typedarray_proto, object_proto);

  js_set_getter_desc(js, typedarray_proto, "buffer", 6, js_mkfun_meta(&typedarray_buffer_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, typedarray_proto, "length", 6, js_mkfun_meta(&typedarray_length_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, typedarray_proto, "byteLength", 10, js_mkfun_meta(&typedarray_byte_length_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, typedarray_proto, "byteOffset", 10, js_mkfun_meta(&typedarray_byte_offset_getter.meta), JS_DESC_C);
  
  typedarray_define_methods(js, typedarray_proto);
  js_set_sym_getter_desc(js, typedarray_proto, js->sym.toStringTag_sym, Ant_Function_Create(js_typedarray_toStringTag_getter), JS_DESC_C);

  ant_value_t typedarray_ctor_obj = Ant_Object_CreateBare(js);
  js_set_proto_init(typedarray_ctor_obj, function_proto);
  js_set_slot(typedarray_ctor_obj, SLOT_CFUNC, Ant_Function_Create(js_typedarray_base_constructor));
  mkprop(js, typedarray_ctor_obj, js->length_str, Ant_Value_FromNumber(0), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop(js, typedarray_ctor_obj, Ant_String_FromLiteral(js, "name"), Ant_String_FromLiteral(js, "TypedArray"), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop_exact_attrs(js, typedarray_ctor_obj, Ant_String_FromLiteral(js, "prototype"), typedarray_proto, 0);
  
  defmethod(js, typedarray_ctor_obj, "of", 2, Ant_Function_CreateWithArity(js_typedarray_static_of, 0));
  defmethod(js, typedarray_ctor_obj, "from", 4, Ant_Function_CreateWithArity(js_typedarray_static_from, 1));
  js_define_species_getter(js, typedarray_ctor_obj);
  
  ant_value_t typedarray_ctor = js_obj_to_func(js, typedarray_ctor_obj);
  defmethod(js, typedarray_proto, "constructor", 11, typedarray_ctor);
  typedarray_define_iterators(js, typedarray_proto);

  for (TypedArrayType type = 0; type < TYPED_ARRAY_COUNT; type++)
    setup_typedarray_kind(js, type, typedarray_ctor, typedarray_proto);

  return typedarray_proto;
}
