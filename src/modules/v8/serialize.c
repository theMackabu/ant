#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uthash.h>

#include "utf8.h"
#include "ptr.h"
#include "arrays.h"
#include "cfunc.h"
#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "silver/call.h"
#include "v8_internal.h"

#include "modules/v8.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/date.h"
#include "modules/regex.h"
#include "modules/collections.h"

static void put_bytes(ser_t *s, const void *bytes, size_t n) {
  if (s->oom || n == 0) return;
  if (s->len + n > s->cap) {
    size_t cap = s->cap ? s->cap : 64;
    while (cap < s->len + n) cap *= 2;
    uint8_t *data = realloc(s->data, cap);
    if (!data) {
      s->oom = true;
      return;
    }
    s->data = data;
    s->cap = cap;
  }
  memcpy(s->data + s->len, bytes, n);
  s->len += n;
}

static void put_u8(ser_t *s, uint8_t value) {
  put_bytes(s, &value, 1);
}

static size_t varint_encode(uint8_t out[10], uint64_t value) {
  size_t n = 0;
  for (; value > 0x7f; value >>= 7) out[n++] = (uint8_t)(value | 0x80);
  out[n++] = (uint8_t)value;
  return n;
}

static void put_varint(ser_t *s, uint64_t value) {
  uint8_t bytes[10];
  put_bytes(s, bytes, varint_encode(bytes, value));
}

static void put_double(ser_t *s, double value) {
  put_bytes(s, &value, sizeof(value));
}

static void put_number(ser_t *s, double value) {
  int32_t integer = value >= INT32_MIN && value <= INT32_MAX ? (int32_t)value : 0;
  if ((double)integer != value || (value == 0 && signbit(value))) {
    put_u8(s, V8_TAG_DOUBLE);
    put_double(s, value);
    return;
  }
  put_u8(s, V8_TAG_INT32);
  put_varint(s, ((uint32_t)integer << 1) ^ (uint32_t)(integer >> 31));
}

static void put_string(ser_t *s, const char *str, size_t len) {
  const uint8_t *bytes = (const uint8_t *)str;
  uint8_t *narrowed = NULL;
  size_t count = len;
  bool one_byte = true;

  for (size_t i = 0; i < len; i++) {
    if (bytes[i] < 0x80) continue;
    bytes = narrowed = utf8_to_latin1(str, len, &count, &one_byte);
    if (!narrowed && one_byte) s->oom = true;
    break;
  }

  if (one_byte) {
    put_u8(s, V8_TAG_ONE_BYTE_STRING);
    put_varint(s, count);
    put_bytes(s, bytes, count);
    free(narrowed);
    return;
  }

  uint8_t length[10];
  size_t length_size = varint_encode(length, utf16_strlen(str, len) * 2);
  if ((s->len + 1 + length_size) & 1) put_u8(s, V8_TAG_PADDING);
  put_u8(s, V8_TAG_TWO_BYTE_STRING);
  put_bytes(s, length, length_size);

  utf16_iterator_t units = utf16_iterator(str, len);
  uint16_t unit;
  while (utf16_next_code_unit(&units, &unit)) put_bytes(s, &unit, sizeof(unit));
}

static void put_string_value(ser_t *s, ant_value_t value) {
  size_t len = 0;
  const char *str = js_getstr(s->js, value, &len);
  put_string(s, str ? str : "", str ? len : 0);
}

static void put_bigint(ser_t *s, ant_value_t value) {
  size_t count = 0;
  const uint32_t *limbs = bigint_limbs(s->js, value, &count);
  while (count > 0 && limbs[count - 1] == 0) count--;

  size_t words = (count + 1) / 2;
  put_varint(s, words * 16 | (bigint_is_negative(s->js, value) ? 1 : 0));
  put_bytes(s, limbs, count * sizeof(uint32_t));
  if (count & 1) put_bytes(s, &(uint32_t){ 0 }, sizeof(uint32_t));
}

static ant_value_t ser_clone_error(ser_t *s, const char *text, size_t len) {
  ant_t *js = s->js;
  ant_value_t message = js_mkstr(js, text, len);
  ant_value_t create = js_get(js, s->self, "_getDataCloneError");
  if (!is_callable(create)) return js_mkerr_typed(js, JS_ERR_GENERIC, "%.*s", (int)len, text);

  ant_value_t error = sv_vm_call_explicit_this(js->vm, js, create, s->self, &message, 1);
  return is_err(error) ? error : js_throw(js, error);
}

static ant_value_t ser_detached(ser_t *s) {
  static const char message[] = "An ArrayBuffer is detached and could not be cloned.";
  return ser_clone_error(s, message, sizeof(message) - 1);
}

static ant_value_t ser_uncloneable(ser_t *s, ant_value_t value) {
  ant_t *js = s->js;
  uint8_t type = vtype(value);
  const char *text = "#<Object>";
  size_t len = strlen(text);

  if (type == kTypeSymbol || type == kTypeFunction || type == kTypeBuiltin) {
    ant_value_t str = js_tostring_val(js, value);
    if (is_err(str)) return str;
    text = js_getstr(js, str, &len);
  }

  char message[160];
  int size = snprintf(message, sizeof(message), "%.*s could not be cloned.", (int)(len > 128 ? 128 : len), text);
  return ser_clone_error(s, message, (size_t)size);
}

static ser_ref_t *ser_find(ser_ref_t *table, ant_value_t value) {
  ser_ref_t *ref = NULL;
  HASH_FIND(hh, table, &value, sizeof(value), ref);
  return ref;
}

static void ser_remember(ser_t *s, ser_ref_t **table, ant_value_t value, uint32_t id) {
  ser_ref_t *ref = calloc(1, sizeof(*ref));
  if (!ref) {
    s->oom = true;
    return;
  }
  ref->key = value;
  ref->id = id;
  HASH_ADD(hh, *table, key, sizeof(ref->key), ref);
  js_arr_push(s->js, s->seen, value);
}

static bool ser_reference(ser_t *s, ant_value_t value) {
  ser_ref_t *ref = ser_find(s->refs, value);
  if (!ref) {
    ser_remember(s, &s->refs, value, HASH_COUNT(s->refs));
    return false;
  }
  put_u8(s, V8_TAG_REFERENCE);
  put_varint(s, ref->id);
  return true;
}

static ant_value_t ser_props(ser_t *s, ant_value_t obj, bool array, uint32_t *count) {
  ant_t *js = s->js;
  ant_object_t *ptr = js_obj_ptr(obj);
  ant_iter_t iter = js_prop_iter_begin(js, obj);
  ant_iter_key_t key;
  ant_value_t prop;
  ant_value_t result = Ant_Value_Undefined;

  while (ptr && !is_err(result) && js_prop_iter_next_key(&iter, &key, &prop)) {
    if (key.is_symbol || !js_is_own_enumerable_prop(js, obj, ptr, &key)) continue;
    unsigned long index = 0;
    bool indexed = own_key_is_array_index(key.str, key.key_len, &index);
    if (array && (indexed || (key.key_len == 6 && memcmp(key.str, "length", 6) == 0))) continue;

    const ant_shape_prop_t *meta = ant_shape_prop_at(ptr->shape, key.slot);
    bool plain = !ptr->flags.is_exotic && meta && !meta->has_getter && !meta->has_setter;
    ant_value_t value = plain ? prop : js_get(js, obj, key.str);
    if (is_err(value)) {
      result = value;
      break;
    }

    if (indexed) put_number(s, (double)index);
    else put_string(s, key.str, key.key_len);
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, value);
    result = ser_value(s, value);
    GC_ROOT_RESTORE(js, mark);
    (*count)++;
  }

  js_prop_iter_end(&iter);
  return result;
}

static bool array_element(ant_t *js, ant_value_t arr, ant_object_t *ptr, ant_offset_t index, ant_value_t *out) {
  if (array_dense_read(ptr, index, out)) return true;
  if (array_dense_storage(ptr)) return false;
  if (Ant_Object_HasOwn(js, arr, Ant_Value_FromNumber((double)index)) != Ant_Value_True) return false;
  *out = js_arr_get(js, arr, index);
  return true;
}

static ant_value_t ser_array(ser_t *s, ant_value_t value) {
  ant_t *js = s->js;
  ant_object_t *ptr = array_obj_ptr(value);
  ant_offset_t len = js_arr_len(js, value);
  ant_value_t item, r;

  bool dense = true;
  if (array_may_have_holes(value)) {
    for (ant_offset_t i = 0; i < len && dense; i++) dense = array_element(js, value, ptr, i, &item);
  }

  uint32_t count = 0;
  put_u8(s, dense ? V8_TAG_BEGIN_DENSE_ARRAY : V8_TAG_BEGIN_SPARSE_ARRAY);
  put_varint(s, len);

  for (ant_offset_t i = 0; i < len; i++) {
    bool present = array_element(js, value, ptr, i, &item);
    if (!present && !dense) continue;
    if (!dense) {
      put_number(s, (double)i);
      count++;
    }
    if (is_err(r = ser_value(s, present ? item : Ant_Value_Undefined))) return r;
    if (js_arr_len(js, value) != len) return js_mkerr_typed(js, JS_ERR_GENERIC, "#<Array> could not be cloned.");
  }

  if (is_err(r = ser_props(s, value, true, &count))) return r;
  put_u8(s, dense ? V8_TAG_END_DENSE_ARRAY : V8_TAG_END_SPARSE_ARRAY);
  put_varint(s, count);
  put_varint(s, len);
  return Ant_Value_Undefined;
}

static char error_prototype_tag(ant_t *js, ant_value_t value) {
  for (ant_value_t proto = js_get_proto(js, value); is_object_type(proto); proto = js_get_proto(js, proto)) {
    for (int type = 0; type < JS_ERR_TYPE_COUNT; type++) 
    if (v8_error_prototype[type] && proto == js->builtins.error_protos[type]) return v8_error_prototype[type];
  }
  return 0;
}

static ant_value_t ser_error(ser_t *s, ant_value_t value) {
  ant_t *js = s->js;
  char prototype = error_prototype_tag(js, value);
  ant_value_t message, cause;
  ant_value_t stack = js_get(js, value, "stack");
  if (is_err(stack)) return stack;

  put_u8(s, V8_TAG_ERROR);
  if (prototype) put_u8(s, (uint8_t)prototype);
  if (js_try_get_own_data_prop(js, value, "message", 7, &message) && vtype(message) == kTypeString) {
    put_u8(s, V8_ERROR_MESSAGE);
    put_string_value(s, message);
  }
  if (vtype(stack) == kTypeString) {
    put_u8(s, V8_ERROR_STACK);
    put_string_value(s, stack);
  }
  if (js_try_get_own_data_prop(js, value, "cause", 5, &cause)) {
    put_u8(s, V8_ERROR_CAUSE);
    ant_value_t r = ser_value(s, cause);
    if (is_err(r)) return r;
  }
  put_u8(s, V8_ERROR_END);
  return Ant_Value_Undefined;
}

static ant_value_t ser_object(ser_t *s, ant_value_t value);

static ant_value_t ser_view(ser_t *s, ant_value_t value, TypedArrayData *ta, DataViewData *dv) {
  ant_t *js = s->js;
  if (!(ta ? Ant_Buffer_ViewIsLive(ta) : Ant_Buffer_ViewIsLive(dv))) return ser_detached(s);

  if (s->views_as_host_objects) {
    if (ser_reference(s, value)) return Ant_Value_Undefined;
    ant_value_t write = js_get(js, s->self, "_writeHostObject");
    if (!is_callable(write)) return ser_uncloneable(s, value);
    put_u8(s, V8_TAG_HOST_OBJECT);
    ant_value_t r = sv_vm_call_explicit_this(js->vm, js, write, s->self, &value, 1);
    return is_err(r) ? r : Ant_Value_Undefined;
  }

  if (!ser_find(s->refs, value)) {
    ant_value_t buffer = js_get(js, value, "buffer");
    ant_value_t r = is_err(buffer) ? buffer : ser_object(s, buffer);
    if (is_err(r)) return r;
  }
  if (ser_reference(s, value)) return Ant_Value_Undefined;

  ArrayBufferData *buffer = ta ? ta->buffer : dv->buffer;
  bool tracking = ta ? ta->length_tracking : dv->length_tracking;
  put_u8(s, V8_TAG_ARRAY_BUFFER_VIEW);
  put_u8(s, ta ? (uint8_t)v8_view_tags[ta->type] : (uint8_t)V8_VIEW_DATAVIEW);
  put_varint(s, ta ? ta->byte_offset : dv->byte_offset);
  put_varint(s, ta ? ta->byte_length : dv->byte_length);
  put_varint(s, (tracking ? 1 : 0) | (buffer->resizable ? 2 : 0));
  return Ant_Value_Undefined;
}

static ant_value_t ser_buffer(ser_t *s, ant_value_t value, ArrayBufferData *buffer) {
  ant_t *js = s->js;
  ser_ref_t *transfer = ser_find(s->transfers, value);
  if (transfer) {
    put_u8(s, V8_TAG_ARRAY_BUFFER_TRANSFER);
    put_varint(s, transfer->id);
    return Ant_Value_Undefined;
  }

  if (buffer->is_shared) {
    ant_value_t lookup = js_get(js, s->self, "_getSharedArrayBufferId");
    if (!is_callable(lookup)) return ser_uncloneable(s, value);
    ant_value_t id = sv_vm_call_explicit_this(js->vm, js, lookup, s->self, &value, 1);
    if (is_err(id)) return id;
    put_u8(s, V8_TAG_SHARED_ARRAY_BUFFER);
    put_varint(s, js_to_uint32(js_to_number(js, id)));
    return Ant_Value_Undefined;
  }

  if (buffer->is_detached) return ser_detached(s);
  put_u8(s, buffer->resizable ? V8_TAG_RESIZABLE_ARRAY_BUFFER : V8_TAG_ARRAY_BUFFER);
  put_varint(s, buffer->length);
  if (buffer->resizable) put_varint(s, buffer->capacity);
  put_bytes(s, buffer->data, buffer->length);
  return Ant_Value_Undefined;
}

static ant_value_t ser_boxed(ser_t *s, ant_value_t value, ant_value_t primitive) {
  switch (vtype(primitive)) {
    case kTypeBool:
      put_u8(s, primitive == Ant_Value_True ? V8_TAG_TRUE_OBJECT : V8_TAG_FALSE_OBJECT);
      break;
    case kTypeNumber:
      put_u8(s, V8_TAG_NUMBER_OBJECT);
      put_double(s, Ant_Value_AsNumberUnchecked(primitive));
      break;
    case kTypeBigInt:
      put_u8(s, V8_TAG_BIGINT_OBJECT);
      put_bigint(s, primitive);
      break;
    case kTypeString:
      put_u8(s, V8_TAG_STRING_OBJECT);
      put_string_value(s, primitive);
      break;
    default: return ser_uncloneable(s, value);
  }
  return Ant_Value_Undefined;
}

static void ser_regexp(ser_t *s, ant_value_t value) {
  ant_value_t source = js_get_slot(value, SLOT_DATA);
  ant_value_t flags = regexp_flags_value(s->js, value);
  size_t len = 0;
  const char *text = vtype(flags) == kTypeString ? js_getstr(s->js, flags, &len) : "";

  uint32_t bits = 0;
  for (size_t i = 0; i < sizeof(v8_regexp_flags) / sizeof(v8_regexp_flags[0]); i++) {
    if (memchr(text, v8_regexp_flags[i].flag, len)) bits |= v8_regexp_flags[i].bit;
  }

  put_u8(s, V8_TAG_REGEXP);
  put_string_value(s, source);
  put_varint(s, bits);
}

static ant_value_t ser_map(ser_t *s, ant_value_t value) {
  map_entry_t **head = get_map_from_obj(value);
  map_entry_t *entry, *next;
  uint32_t count = 0;

  put_u8(s, V8_TAG_BEGIN_MAP);
  if (head) HASH_ITER(hh, *head, entry, next) {
    ant_value_t r = ser_value(s, entry->key_val);
    if (is_err(r) || is_err(r = ser_value(s, entry->value))) return r;
    count += 2;
  }
  put_u8(s, V8_TAG_END_MAP);
  put_varint(s, count);
  return Ant_Value_Undefined;
}

static ant_value_t ser_set(ser_t *s, ant_value_t value) {
  set_entry_t **head = get_set_from_obj(value);
  set_entry_t *entry, *next;
  uint32_t count = 0;

  put_u8(s, V8_TAG_BEGIN_SET);
  if (head) HASH_ITER(hh, *head, entry, next) {
    ant_value_t r = ser_value(s, entry->value);
    if (is_err(r)) return r;
    count++;
  }
  put_u8(s, V8_TAG_END_SET);
  put_varint(s, count);
  return Ant_Value_Undefined;
}

static ant_value_t ser_object(ser_t *s, ant_value_t value) {
  TypedArrayData *ta = buffer_get_typedarray_data(value);
  if (!ta && vtype(value) == kTypeTypedArray) ta = (TypedArrayData *)js_gettypedarray(value);
  
  DataViewData *dv = !ta && buffer_is_dataview(value) ? buffer_get_dataview_data(value) : NULL;
  if (ta || dv) return ser_view(s, value, ta, dv);
  if (ser_reference(s, value)) return Ant_Value_Undefined;

  ArrayBufferData *buffer = buffer_get_arraybuffer_data(value);
  if (buffer) return ser_buffer(s, value, buffer);

  uint8_t type = vtype(value);
  ant_object_t *ptr = js_obj_ptr(value);
  if (!ptr || type == kTypeFunction || type == kTypeBuiltin || type == kTypePromise || type == kTypeGenerator)
    return ser_uncloneable(s, value);
  if (js_is_arguments(value) || ptr->type_tag == kTypeWeakMap || ptr->type_tag == kTypeWeakSet)
    return ser_uncloneable(s, value);
  if ((js_proxy_is_host(value) || ptr->flags.is_exotic) && type != kTypeArray) return ser_uncloneable(s, value);

  ant_extra_slot_t *boxed = type == kTypeObject ? ant_object_extra_slot(ptr, SLOT_PRIMITIVE) : NULL;
  if (boxed) return ser_boxed(s, value, boxed->value);

  if (is_date_instance(value)) {
    ant_value_t time = js_get_slot(value, SLOT_DATA);
    put_u8(s, V8_TAG_DATE);
    put_double(s, vtype(time) == kTypeNumber ? Ant_Value_AsNumberUnchecked(time) : 0);
    return Ant_Value_Undefined;
  }

  if (ptr->flags.regexp_brand) {
    ser_regexp(s, value);
    return Ant_Value_Undefined;
  }

  if (js_get_slot(value, SLOT_ERROR_BRAND) == Ant_Value_True) return ser_error(s, value);
  if (ptr->type_tag == kTypeMap) return ser_map(s, value);
  if (ptr->type_tag == kTypeSet) return ser_set(s, value);
  if (type == kTypeArray) return ser_array(s, value);

  uint32_t count = 0;
  put_u8(s, V8_TAG_BEGIN_OBJECT);
  ant_value_t r = ser_props(s, value, false, &count);
  if (is_err(r)) return r;
  
  put_u8(s, V8_TAG_END_OBJECT);
  put_varint(s, count);
  
  return Ant_Value_Undefined;
}

ant_value_t ser_value(ser_t *s, ant_value_t value) {
  switch (vtype(value)) {
    case kTypeUndefined: put_u8(s, V8_TAG_UNDEFINED); break;
    case kTypeNull: put_u8(s, V8_TAG_NULL); break;
    case kTypeBool: put_u8(s, value == Ant_Value_True ? V8_TAG_TRUE : V8_TAG_FALSE); break;
    case kTypeNumber: put_number(s, Ant_Value_AsNumberUnchecked(value)); break;
    case kTypeString: put_string_value(s, value); break;
    case kTypeBigInt:
      put_u8(s, V8_TAG_BIGINT);
      put_bigint(s, value);
      break;
    default:
      if (!is_object_type(value) && vtype(value) != kTypeTypedArray) return ser_uncloneable(s, value);
      if (sv_check_c_stack_overflow(s->js)) return js_mkerr_typed(s->js, JS_ERR_RANGE, "Maximum call stack size exceeded");
      return ser_object(s, value);
  }
  return Ant_Value_Undefined;
}

static ser_t *ser_enter(ant_t *js, ant_value_t self) {
  ser_t *s = js_get_native(self, SERIALIZER_NATIVE_TAG);
  if (!s) return NULL;
  s->js = js;
  s->self = self;
  s->seen = js_get_slot(self, SLOT_DATA);
  return s;
}

static ant_value_t ser_leave(ser_t *s, ant_value_t result) {
  if (!s->oom || is_err(result)) return result;
  s->oom = false;
  return Ant_Error_CreateOutOfMemory(s->js);
}

static void ser_table_free(ser_ref_t **table) {
  ser_ref_t *ref, *next;
  HASH_ITER(hh, *table, ref, next) {
    HASH_DEL(*table, ref);
    free(ref);
  }
}

static void ser_release(ser_t *s) {
  ser_table_free(&s->refs);
  ser_table_free(&s->transfers);
  free(s->data);
  s->data = NULL;
  s->len = s->cap = 0;
}

static void serializer_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t self = js_obj_from_ptr(obj);
  ser_t *s = js_get_native(self, SERIALIZER_NATIVE_TAG);
  if (!s) return;
  js_clear_native(self, SERIALIZER_NATIVE_TAG);
  ser_release(s);
  free(s);
}

#define SERIALIZER(s)                  \
  ser_t *s = ser_enter(js, call_this); \
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation")

static ant_value_t serializer_construct(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor Serializer cannot be invoked without 'new'");

  ser_t *s = calloc(1, sizeof(*s));
  if (!s) return Ant_Error_CreateOutOfMemory(js);
  js_set_slot_wb(js, call_this, SLOT_DATA, Ant_Array_Create(js));
  js_set_native(call_this, s, SERIALIZER_NATIVE_TAG);
  js_set_finalizer(call_this, serializer_finalize);
  return call_this;
}

static ant_value_t serializer_write_header(ant_params_t) {
  SERIALIZER(s);
  put_u8(s, V8_TAG_VERSION);
  put_u8(s, V8_WIRE_VERSION);
  return ser_leave(s, Ant_Value_Undefined);
}

static ant_value_t serializer_write_value(ant_params_t) {
  SERIALIZER(s);
  ant_value_t r = ser_value(s, nargs > 0 ? args[0] : Ant_Value_Undefined);
  return ser_leave(s, is_err(r) ? r : Ant_Value_True);
}

static ant_value_t serializer_release_buffer(ant_params_t) {
  SERIALIZER(s);
  ArrayBufferData *out = create_array_buffer_data(js, s->len);
  if (!out) return Ant_Error_CreateOutOfMemory(js);
  if (s->len) memcpy(out->data, s->data, s->len);
  ant_value_t buffer = create_typed_array(js, TYPED_ARRAY_UINT8, out, 0, s->len, js->builtins.buffer_proto);
  ser_release(s);
  return buffer;
}

static ant_value_t serializer_transfer_array_buffer(ant_params_t) {
  SERIALIZER(s);
  if (nargs < 2 || !buffer_get_arraybuffer_data(args[1]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "arrayBuffer must be an ArrayBuffer");
  ser_remember(s, &s->transfers, args[1], js_to_uint32(js_to_number(js, args[0])));
  return ser_leave(s, Ant_Value_Undefined);
}

static ant_value_t serializer_write_uint32(ant_params_t) {
  SERIALIZER(s);
  put_varint(s, nargs > 0 ? js_to_uint32(js_to_number(js, args[0])) : 0);
  return ser_leave(s, Ant_Value_Undefined);
}

static ant_value_t serializer_write_uint64(ant_params_t) {
  SERIALIZER(s);
  uint64_t high = nargs > 0 ? js_to_uint32(js_to_number(js, args[0])) : 0;
  uint64_t low = nargs > 1 ? js_to_uint32(js_to_number(js, args[1])) : 0;
  put_varint(s, high << 32 | low);
  return ser_leave(s, Ant_Value_Undefined);
}

static ant_value_t serializer_write_double(ant_params_t) {
  SERIALIZER(s);
  put_double(s, nargs > 0 ? js_to_number(js, args[0]) : (double)NAN);
  return ser_leave(s, Ant_Value_Undefined);
}

static ant_value_t serializer_write_raw_bytes(ant_params_t) {
  SERIALIZER(s);
  const uint8_t *bytes;
  size_t len;
  if (nargs < 1 || buffer_get_arraybuffer_data(args[0]) || !buffer_source_get_bytes(js, args[0], &bytes, &len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "source must be a TypedArray or a DataView");
  put_bytes(s, bytes, len);
  return ser_leave(s, Ant_Value_Undefined);
}

static ant_value_t serializer_treat_views_as_host_objects(ant_params_t) {
  SERIALIZER(s);
  s->views_as_host_objects = nargs > 0 && js_truthy(js, args[0]);
  return Ant_Value_Undefined;
}

ant_value_t v8_serializer_constructor(ant_t *js) {
  ant_value_t proto = Ant_Object_Create(js);
  js_set(js, proto, "writeHeader", Ant_Function_Create(serializer_write_header));
  js_set(js, proto, "writeValue", Ant_Function_CreateWithArity(serializer_write_value, 1));
  js_set(js, proto, "releaseBuffer", Ant_Function_Create(serializer_release_buffer));
  js_set(js, proto, "transferArrayBuffer", Ant_Function_CreateWithArity(serializer_transfer_array_buffer, 2));
  js_set(js, proto, "writeUint32", Ant_Function_CreateWithArity(serializer_write_uint32, 1));
  js_set(js, proto, "writeUint64", Ant_Function_CreateWithArity(serializer_write_uint64, 2));
  js_set(js, proto, "writeDouble", Ant_Function_CreateWithArity(serializer_write_double, 1));
  js_set(js, proto, "writeRawBytes", Ant_Function_CreateWithArity(serializer_write_raw_bytes, 1));
  js_set(js, proto, "_setTreatArrayBufferViewsAsHostObjects", Ant_Function_CreateWithArity(serializer_treat_views_as_host_objects, 1));
  return Ant_Function_CreateConstructor(js, serializer_construct, proto, "Serializer", 0);
}
