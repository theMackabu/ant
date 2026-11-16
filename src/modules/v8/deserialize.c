#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ptr.h"
#include "utf8.h"
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


static ant_value_t des_corrupt(des_t *d) {
  return js_mkerr_typed(d->js, JS_ERR_GENERIC, "Unable to deserialize cloned data.");
}

static bool get_bytes(des_t *d, const uint8_t **out, uint64_t len) {
  if (len > d->len - d->pos) return false;
  *out = d->data + d->pos;
  d->pos += (size_t)len;
  return true;
}

static bool get_u8(des_t *d, uint8_t *out) {
  const uint8_t *byte;
  if (!get_bytes(d, &byte, 1)) return false;
  *out = *byte;
  return true;
}

static bool get_varint(des_t *d, uint64_t *out) {
  uint8_t byte = 0x80;
  *out = 0;
  for (unsigned shift = 0; (byte & 0x80) && shift < 64; shift += 7) {
    if (!get_u8(d, &byte)) return false;
    *out |= (uint64_t)(byte & 0x7f) << shift;
  }
  return !(byte & 0x80);
}

static bool get_double(des_t *d, double *out) {
  const uint8_t *bytes;
  if (!get_bytes(d, &bytes, sizeof(*out))) return false;
  memcpy(out, bytes, sizeof(*out));
  return true;
}

static bool get_sized(des_t *d, const uint8_t **out, uint64_t *len) {
  return get_varint(d, len) && get_bytes(d, out, *len);
}

static bool peek_tag(des_t *d, uint8_t *out) {
  while (d->pos < d->len && d->data[d->pos] == V8_TAG_PADDING) d->pos++;
  if (d->pos == d->len) return false;
  *out = d->data[d->pos];
  return true;
}

static bool take_tag(des_t *d, uint8_t tag) {
  uint8_t next;
  if (!peek_tag(d, &next) || next != tag) return false;
  d->pos++;
  return true;
}

static ant_value_t des_track(des_t *d, ant_value_t value) {
  if (!is_err(value)) js_arr_push(d->js, d->refs, value);
  return value;
}

static ant_value_t des_box(des_t *d, ant_value_t primitive) {
  return is_err(primitive) ? primitive : des_track(d, js_normalize_sloppy_this(d->js, primitive));
}

static ant_value_t des_string(des_t *d, uint8_t tag) {
  ant_t *js = d->js;
  const uint8_t *bytes;
  uint64_t len;
  if (!get_sized(d, &bytes, &len)) return des_corrupt(d);
  if (tag == V8_TAG_UTF8_STRING) return js_mkstr(js, bytes, (size_t)len);

  size_t size = 0;
  char *text;
  if (tag == V8_TAG_ONE_BYTE_STRING) text = latin1_to_utf8(bytes, (size_t)len, &size);
  else if (len & 1) return des_corrupt(d);
  else if ((text = malloc((size_t)len * 2 + 1))) {
    for (uint64_t i = 0; i < len; i += 2) {
      uint32_t unit = bytes[i] | (uint32_t)bytes[i + 1] << 8;
      uint32_t low = i + 3 < len ? bytes[i + 2] | (uint32_t)bytes[i + 3] << 8 : 0;
      if (unit >> 10 == 0x36 && low >> 10 == 0x37) {
        unit = 0x10000 + ((unit & 0x3ff) << 10) + (low & 0x3ff);
        i += 2;
      }
      size += (size_t)utf8_encode(unit, text + size);
    }
  }

  if (!text) return Ant_Error_CreateOutOfMemory(js);
  ant_value_t str = js_mkstr(js, text, size);
  free(text);
  return str;
}

static ant_value_t des_bigint(des_t *d) {
  const uint8_t *bytes;
  uint64_t bitfield;
  if (!get_varint(d, &bitfield) || !get_bytes(d, &bytes, bitfield >> 1)) return des_corrupt(d);

  size_t count = (size_t)((bitfield >> 1) + 3) / 4;
  uint32_t *limbs = calloc(count + 1, sizeof(uint32_t));
  if (!limbs) return Ant_Error_CreateOutOfMemory(d->js);
  memcpy(limbs, bytes, (size_t)(bitfield >> 1));
  ant_value_t value = bigint_from_limbs(d->js, limbs, count, bitfield & 1);
  free(limbs);
  return value;
}

static ant_value_t des_define(des_t *d, ant_value_t obj, ant_value_t key, ant_value_t value) {
  ant_t *js = d->js;
  if (vtype(key) == kTypeString) {
    size_t len = 0;
    const char *str = js_getstr(js, key, &len);
    return js_mkprop_fast(js, obj, str, len, value);
  }
  double index = vtype(key) == kTypeNumber ? Ant_Value_AsNumberUnchecked(key) : -1;
  if (!(index >= 0 && index < UINT32_MAX && index == (double)(uint32_t)index)) return des_corrupt(d);
  return js_setprop_index(js, obj, (uint32_t)index, value);
}

static ant_value_t des_props(des_t *d, ant_value_t obj, uint8_t end_tag) {
  ant_t *js = d->js;
  uint64_t count = 0, expected;

  for (; !take_tag(d, end_tag); count++) {
    GC_ROOT_SAVE(mark, js);
    ant_value_t key = des_value(d);
    GC_ROOT_PIN(js, key);
    ant_value_t value = is_err(key) ? key : des_value(d);
    GC_ROOT_PIN(js, value);
    ant_value_t defined = is_err(value) ? value : des_define(d, obj, key, value);
    GC_ROOT_RESTORE(js, mark);
    if (is_err(defined)) return defined;
  }

  return get_varint(d, &expected) && expected == count ? obj : des_corrupt(d);
}

static ant_value_t des_array(des_t *d, bool dense) {
  ant_t *js = d->js;
  uint64_t len, trailing;
  if (!get_varint(d, &len) || len > UINT32_MAX || (dense && len > d->len - d->pos)) return des_corrupt(d);
  ant_value_t arr = des_track(d, Ant_Array_Create(js));
  if (is_err(arr)) return arr;

  bool holes = false;
  for (uint64_t i = 0; dense && i < len; i++) {
    if (take_tag(d, V8_TAG_HOLE)) {
      holes = true;
      continue;
    }
    ant_value_t item = des_value(d);
    if (is_err(item)) return item;
    if (holes) item = js_setprop_index(js, arr, (uint32_t)i, item);
    else js_arr_push(js, arr, item);
    if (is_err(item)) return item;
  }

  ant_value_t r = des_props(d, arr, dense ? V8_TAG_END_DENSE_ARRAY : V8_TAG_END_SPARSE_ARRAY);
  if (is_err(r)) return r;
  if (!get_varint(d, &trailing) || trailing != len) return des_corrupt(d);
  if ((uint64_t)js_arr_len(js, arr) == len) return arr;
  r = js_setprop(js, arr, Ant_String_FromLiteral(js, "length"), Ant_Value_FromNumber((double)len));
  return is_err(r) ? r : arr;
}

static ant_value_t des_error(des_t *d) {
  ant_t *js = d->js;
  uint8_t tag;
  int type = JS_ERR_GENERIC;
  if (!get_u8(d, &tag)) return des_corrupt(d);
  for (int candidate = 0; candidate < JS_ERR_TYPE_COUNT; candidate++) {
    if (v8_error_prototype[candidate] != (char)tag) continue;
    type = candidate;
    if (!get_u8(d, &tag)) return des_corrupt(d);
    break;
  }

  ant_value_t err = Ant_Object_CreateBare(js);
  if (is_err(err)) return err;
  js_set_proto_init(err, js->builtins.error_protos[type]);
  js_set_slot(err, SLOT_ERROR_BRAND, Ant_Value_True);
  js_set_slot(err, SLOT_ERR_TYPE, Ant_Value_FromNumber((double)type));
  des_track(d, err);

  for (; tag != V8_ERROR_END; ) {
    ant_value_t value = des_value(d);
    if (is_err(value)) return value;
    bool text = vtype(value) == kTypeString;
    if (tag == V8_ERROR_MESSAGE && text) defhidden(js, err, "message", 7, value);
    else if (tag == V8_ERROR_STACK && text) js_error_define_stack(js, err, Ant_Value_Undefined, value);
    else if (tag == V8_ERROR_CAUSE) defhidden(js, err, "cause", 5, value);
    else return des_corrupt(d);
    if (!get_u8(d, &tag)) return des_corrupt(d);
  }
  return err;
}

static ant_value_t des_view(des_t *d, ant_value_t buffer_obj) {
  ant_t *js = d->js;
  ArrayBufferData *buffer = buffer_get_arraybuffer_data(buffer_obj);
  uint8_t kind;
  uint64_t offset, length, flags = 0;
  if (!get_u8(d, &kind) || !get_varint(d, &offset) || !get_varint(d, &length)) return des_corrupt(d);
  if (d->version >= 14 && !get_varint(d, &flags)) return des_corrupt(d);
  if (offset > buffer->length || length > buffer->length - offset) return des_corrupt(d);

  if (kind == V8_VIEW_DATAVIEW)
    return des_track(d, create_dataview_with_buffer(js, buffer, (size_t)offset, (size_t)length, buffer_obj, flags & 1));

  for (int type = 0; type < TYPED_ARRAY_COUNT; type++) {
    size_t size = get_element_size((TypedArrayType)type);
    if (v8_view_tags[type] != (char)kind) continue;
    if (offset % size || length % size) break;
    return des_track(d, create_typed_array_with_buffer(
      js, (TypedArrayType)type, buffer, (size_t)offset, (size_t)length / size,
      js->builtins.typedarray_protos[type], buffer_obj, flags & 1
    ));
  }
  return des_corrupt(d);
}

static ant_value_t des_view_of(des_t *d, ant_value_t buffer) {
  if (is_err(buffer) || !buffer_get_arraybuffer_data(buffer) || !take_tag(d, V8_TAG_ARRAY_BUFFER_VIEW)) return buffer;
  return des_view(d, buffer);
}

static ant_value_t des_buffer(des_t *d, bool resizable) {
  ant_t *js = d->js;
  const uint8_t *bytes;
  uint64_t len, max = 0;
  
  if (!get_varint(d, &len) || (resizable && !get_varint(d, &max)) || !get_bytes(d, &bytes, len)) return des_corrupt(d);
  if (resizable && max < len) return des_corrupt(d);

  return des_track(d, resizable
    ? create_resizable_arraybuffer_copy(js, bytes, (size_t)len, (size_t)max)
    : create_arraybuffer_copy(js, bytes, (size_t)len));
}

static ant_value_t des_transfer(des_t *d) {
  uint64_t id;
  if (!get_varint(d, &id) || id >= (uint64_t)js_arr_len(d->js, d->transfers)) return des_corrupt(d);
  ant_value_t buffer = js_arr_get(d->js, d->transfers, (ant_offset_t)id);
  return buffer_get_arraybuffer_data(buffer) ? des_track(d, buffer) : des_corrupt(d);
}

static ant_value_t des_host_object(des_t *d) {
  ant_t *js = d->js;
  ant_value_t read = js_get(js, d->self, "_readHostObject");
  if (!is_callable(read)) return des_corrupt(d);

  uint32_t id = (uint32_t)js_arr_len(js, d->refs);
  js_arr_push(js, d->refs, Ant_Value_Undefined);
  ant_value_t object = sv_vm_call_explicit_this(js->vm, js, read, d->self, NULL, 0);
  
  if (is_err(object)) return object;
  if (!is_object_type(object)) return js_mkerr_typed(js, JS_ERR_TYPE, "_readHostObject must return an object");

  ant_value_t stored = js_setprop_index(js, d->refs, id, object);
  return is_err(stored) ? stored : object;
}

static ant_value_t des_collection(des_t *d, bool is_map) {
  ant_t *js = d->js;
  map_entry_t **map = NULL;
  set_entry_t **set = NULL;
  uint64_t count = 0, expected;
  ant_value_t collection = des_track(d, is_map ? collections_map_create(js, &map) : collections_set_create(js, &set));
  if (is_err(collection)) return collection;

  for (; !take_tag(d, is_map ? V8_TAG_END_MAP : V8_TAG_END_SET); count += is_map ? 2 : 1) {
    GC_ROOT_SAVE(mark, js);
    ant_value_t key = des_value(d);
    GC_ROOT_PIN(js, key);
    ant_value_t value = is_map && !is_err(key) ? des_value(d) : key;
    bool stored = !is_err(value) && (is_map
      ? collections_map_store_cloned_entry(js, map, key, value)
      : collections_set_store_cloned_entry(js, set, value));
    GC_ROOT_RESTORE(js, mark);
    if (is_err(value)) return value;
    if (!stored) return Ant_Error_CreateOutOfMemory(js);
  }

  return get_varint(d, &expected) && expected == count ? collection : des_corrupt(d);
}

static ant_value_t des_regexp(des_t *d) {
  ant_t *js = d->js;
  char flags[sizeof(v8_regexp_flags) / sizeof(v8_regexp_flags[0])];
  size_t count = 0;
  uint64_t bits;

  GC_ROOT_SAVE(mark, js);
  ant_value_t source = des_value(d);
  GC_ROOT_PIN(js, source);
  ant_value_t result = source;

  if (!is_err(source)) {
    if (vtype(source) != kTypeString || !get_varint(d, &bits)) result = des_corrupt(d);
    else {
      for (size_t i = 0; i < sizeof(flags); i++) {
        if (bits & v8_regexp_flags[i].bit) flags[count++] = v8_regexp_flags[i].flag;
      }
      ant_value_t flag_string = js_mkstr(js, flags, count);
      GC_ROOT_PIN(js, flag_string);
      result = des_track(d, regexp_create_literal(js, source, flag_string));
    }
  }

  GC_ROOT_RESTORE(js, mark);
  return result;
}

ant_value_t des_value(des_t *d) {
  ant_t *js = d->js;
  if (sv_check_c_stack_overflow(js)) return Ant_Error_CreateStackOverflow(js);

  uint8_t tag;
  uint64_t n;
  double number;
  if (!peek_tag(d, &tag)) return des_corrupt(d);
  d->pos++;

  switch (tag) {
    case V8_TAG_UNDEFINED: return Ant_Value_Undefined;
    case V8_TAG_NULL: return Ant_Value_Null;
    case V8_TAG_TRUE: return Ant_Value_True;
    case V8_TAG_FALSE: return Ant_Value_False;

    case V8_TAG_INT32:
      if (!get_varint(d, &n) || n > UINT32_MAX) return des_corrupt(d);
      return Ant_Value_FromNumber((double)(int32_t)((uint32_t)n >> 1 ^ -(uint32_t)(n & 1)));
    case V8_TAG_UINT32:
      return get_varint(d, &n) && n <= UINT32_MAX ? Ant_Value_FromNumber((double)n) : des_corrupt(d);
    case V8_TAG_DOUBLE:
      return get_double(d, &number) ? Ant_Value_FromNumber(number) : des_corrupt(d);
    case V8_TAG_BIGINT: return des_bigint(d);

    case V8_TAG_UTF8_STRING:
    case V8_TAG_ONE_BYTE_STRING:
    case V8_TAG_TWO_BYTE_STRING: return des_string(d, tag);

    case V8_TAG_REFERENCE:
      if (!get_varint(d, &n) || n >= (uint64_t)js_arr_len(js, d->refs)) return des_corrupt(d);
      return des_view_of(d, js_arr_get(js, d->refs, (ant_offset_t)n));

    case V8_TAG_BEGIN_OBJECT: {
      ant_value_t obj = des_track(d, Ant_Object_Create(js));
      return is_err(obj) ? obj : des_props(d, obj, V8_TAG_END_OBJECT);
    }
    case V8_TAG_BEGIN_DENSE_ARRAY: return des_array(d, true);
    case V8_TAG_BEGIN_SPARSE_ARRAY: return des_array(d, false);
    case V8_TAG_BEGIN_MAP: return des_collection(d, true);
    case V8_TAG_BEGIN_SET: return des_collection(d, false);

    case V8_TAG_DATE:
      return get_double(d, &number) ? des_track(d, js_mkdate(js, number)) : des_corrupt(d);
    case V8_TAG_REGEXP: return des_regexp(d);
    case V8_TAG_ERROR: return des_error(d);

    case V8_TAG_TRUE_OBJECT: return des_box(d, Ant_Value_True);
    case V8_TAG_FALSE_OBJECT: return des_box(d, Ant_Value_False);
    case V8_TAG_NUMBER_OBJECT:
      return get_double(d, &number) ? des_box(d, Ant_Value_FromNumber(number)) : des_corrupt(d);
    case V8_TAG_BIGINT_OBJECT: return des_box(d, des_bigint(d));
    case V8_TAG_STRING_OBJECT: {
      ant_value_t value = des_value(d);
      return is_err(value) || vtype(value) == kTypeString ? des_box(d, value) : des_corrupt(d);
    }

    case V8_TAG_ARRAY_BUFFER: return des_view_of(d, des_buffer(d, false));
    case V8_TAG_RESIZABLE_ARRAY_BUFFER: return des_view_of(d, des_buffer(d, true));
    case V8_TAG_ARRAY_BUFFER_TRANSFER: return des_view_of(d, des_transfer(d));
    case V8_TAG_HOST_OBJECT: return des_host_object(d);

    default: return des_corrupt(d);
  }
}

enum {
  DES_STATE_BUFFER,
  DES_STATE_REFS,
  DES_STATE_TRANSFERS,
};

static des_t *des_enter(ant_t *js, ant_value_t self) {
  des_t *d = js_get_native(self, DESERIALIZER_NATIVE_TAG);
  if (!d) return NULL;

  ant_value_t state = js_get_slot(self, SLOT_DATA);
  d->js = js;
  d->self = self;
  d->refs = js_arr_get(js, state, DES_STATE_REFS);
  d->transfers = js_arr_get(js, state, DES_STATE_TRANSFERS);
  if (!buffer_source_get_bytes(js, js_arr_get(js, state, DES_STATE_BUFFER), &d->data, &d->len)) d->len = 0;
  if (d->pos > d->len) d->pos = d->len;
  return d;
}

static void deserializer_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t self = js_obj_from_ptr(obj);
  des_t *d = js_get_native(self, DESERIALIZER_NATIVE_TAG);
  js_clear_native(self, DESERIALIZER_NATIVE_TAG);
  free(d);
}

#define DESERIALIZER(d)                \
  des_t *d = des_enter(js, call_this); \
  if (!d) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation")

static ant_value_t deserializer_construct(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor Deserializer cannot be invoked without 'new'");

  ant_value_t buffer = nargs > 0 ? args[0] : Ant_Value_Undefined;
  const uint8_t *bytes;
  size_t len;
  if (buffer_get_arraybuffer_data(buffer) || !buffer_source_get_bytes(js, buffer, &bytes, &len))
    return Ant_Error_CreateInvalidArgType(js, "buffer", "an instance of Buffer, TypedArray, or DataView", buffer);

  des_t *d = calloc(1, sizeof(*d));
  if (!d) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t state = Ant_Array_Create(js);
  js_set_slot_wb(js, call_this, SLOT_DATA, state);
  js_arr_push(js, state, buffer);
  js_arr_push(js, state, Ant_Array_Create(js));
  js_arr_push(js, state, Ant_Array_Create(js));
  js_set(js, call_this, "buffer", buffer);
  js_set_native(call_this, d, DESERIALIZER_NATIVE_TAG);
  js_set_finalizer(call_this, deserializer_finalize);
  return call_this;
}

static ant_value_t deserializer_read_header(ant_params_t) {
  DESERIALIZER(d);
  uint8_t tag;
  if (!get_u8(d, &tag) || tag != V8_TAG_VERSION || !get_varint(d, &d->version) ||
      d->version < V8_WIRE_MIN_VERSION || d->version > V8_WIRE_VERSION)
    return js_mkerr_typed(js, JS_ERR_GENERIC, "Unable to deserialize cloned data due to invalid or unsupported version.");
  return Ant_Value_True;
}

static ant_value_t deserializer_read_value(ant_params_t) {
  DESERIALIZER(d);
  return des_value(d);
}

static ant_value_t deserializer_transfer_array_buffer(ant_params_t) {
  DESERIALIZER(d);
  if (nargs < 2 || !buffer_get_arraybuffer_data(args[1]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "arrayBuffer must be an ArrayBuffer or SharedArrayBuffer");
  ant_value_t stored = js_setprop_index(js, d->transfers, js_to_uint32(js_to_number(js, args[0])), args[1]);
  return is_err(stored) ? stored : Ant_Value_Undefined;
}

static ant_value_t deserializer_get_wire_format_version(ant_params_t) {
  DESERIALIZER(d);
  return Ant_Value_FromNumber((double)d->version);
}

static ant_value_t deserializer_read_uint32(ant_params_t) {
  DESERIALIZER(d);
  uint64_t value;
  if (!get_varint(d, &value) || value > UINT32_MAX) return js_mkerr_typed(js, JS_ERR_GENERIC, "ReadUint32() failed");
  return Ant_Value_FromNumber((double)value);
}

static ant_value_t deserializer_read_uint64(ant_params_t) {
  DESERIALIZER(d);
  uint64_t value;
  if (!get_varint(d, &value)) return js_mkerr_typed(js, JS_ERR_GENERIC, "ReadUint64() failed");
  ant_value_t halves = Ant_Array_Create(js);
  js_arr_push(js, halves, Ant_Value_FromNumber((double)(uint32_t)(value >> 32)));
  js_arr_push(js, halves, Ant_Value_FromNumber((double)(uint32_t)value));
  return halves;
}

static ant_value_t deserializer_read_double(ant_params_t) {
  DESERIALIZER(d);
  double value;
  return get_double(d, &value) ? Ant_Value_FromNumber(value) : js_mkerr_typed(js, JS_ERR_GENERIC, "ReadDouble() failed");
}

static ant_value_t deserializer_read_raw_bytes(ant_params_t) {
  DESERIALIZER(d);
  const uint8_t *bytes;
  size_t offset = d->pos;
  double length = nargs > 0 ? js_to_number(js, args[0]) : -1;
  if (!(length >= 0) || !get_bytes(d, &bytes, (uint64_t)length)) return js_mkerr_typed(js, JS_ERR_GENERIC, "ReadRawBytes() failed");
  return Ant_Value_FromNumber((double)offset);
}

ant_value_t v8_deserializer_constructor(ant_t *js) {
  ant_value_t proto = Ant_Object_Create(js);
  js_set(js, proto, "readHeader", Ant_Function_Create(deserializer_read_header));
  js_set(js, proto, "readValue", Ant_Function_Create(deserializer_read_value));
  js_set(js, proto, "transferArrayBuffer", Ant_Function_CreateWithArity(deserializer_transfer_array_buffer, 2));
  js_set(js, proto, "getWireFormatVersion", Ant_Function_Create(deserializer_get_wire_format_version));
  js_set(js, proto, "readUint32", Ant_Function_Create(deserializer_read_uint32));
  js_set(js, proto, "readUint64", Ant_Function_Create(deserializer_read_uint64));
  js_set(js, proto, "readDouble", Ant_Function_Create(deserializer_read_double));
  js_set(js, proto, "_readRawBytes", Ant_Function_CreateWithArity(deserializer_read_raw_bytes, 1));
  return Ant_Function_CreateConstructor(js, deserializer_construct, proto, "Deserializer", 1);
}
