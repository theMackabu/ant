#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "protectors.h"
#include "gc/roots.h"
#include "silver/call.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/symbol.h"
#include "buffer_internal.h"

static inline size_t buffer_string_offset(ant_value_t value, size_t limit, size_t fallback) {
  if (vtype(value) != kTypeNumber) return fallback;
  double raw = Ant_Value_AsNumberUnchecked(value);
  
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

ant_value_t buffer_byte_subarray(ant_t *js, ant_value_t value, size_t begin, size_t end) {
  TypedArrayData *ta_data = typedarray_view_live(buffer_get_typedarray_data(value));
  if (!ta_data || !Ant_Buffer_IsLive(ta_data->backing)) return typedarray_this_error(js, value);

  if (end > ta_data->byte_length) end = ta_data->byte_length;
  if (begin > end) begin = end;

  ant_value_t shared = ta_data->buffer == ta_data->backing ? js_get_slot(value, SLOT_DATA) : Ant_Value_Undefined;
  ant_value_t ab_obj = is_object_type(shared) ? shared : create_arraybuffer_obj(js, ta_data->backing);

  return create_typed_array_with_buffer(
    js, TYPED_ARRAY_UINT8, ta_data->backing, ta_data->byte_offset + begin,
    end - begin, js->builtins.buffer_proto, ab_obj, false
  );
}

ant_value_t Ant_Buffer_FromView(ant_t *js, ant_value_t value) {
  if (buffer_get_typedarray_data(value)) return buffer_byte_subarray(js, value, 0, SIZE_MAX);

  DataViewData *dv = buffer_get_dataview_data(value);
  if (!dv || !Ant_Buffer_IsLive(dv->backing))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform this operation on a detached ArrayBuffer");

  ant_value_t shared = dv->buffer == dv->backing ? js_get_slot(value, SLOT_DATA) : Ant_Value_Undefined;
  ant_value_t ab_obj = is_object_type(shared) ? shared : create_arraybuffer_obj(js, dv->backing);

  return create_typed_array_with_buffer(
    js, TYPED_ARRAY_UINT8, dv->backing, dv->byte_offset,
    dv->byte_length, js->builtins.buffer_proto, ab_obj, false
  );
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
  double length = vtype(length_val) == kTypeNumber ? Ant_Value_AsNumberUnchecked(length_val) : 0;
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

static ant_value_t buffer_from_typed_array(ant_t *js, const TypedArrayData *ta) {
  uint8_t *src = NULL;
  size_t byte_length = 0;
  size_t len = ta->length;

  if (!Ant_Buffer_GetViewBytes(ta, &src, &byte_length)) len = 0;
  ArrayBufferData *buffer = create_array_buffer_data(js, len);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");

  switch (ta->type) {
    case TYPED_ARRAY_UINT8:
    case TYPED_ARRAY_INT8:
    case TYPED_ARRAY_UINT8_CLAMPED:
      if (len > 0) memcpy(buffer->data, src, len);
      break;
    case TYPED_ARRAY_BIGINT64:
    case TYPED_ARRAY_BIGUINT64:
      if (len > 0) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot mix BigInt and other types, use explicit conversions");
      break;
    default:
      for (size_t i = 0; i < len; i++) {
        ant_value_t elem = Ant_Value_Undefined;
        buffer_typedarray_data_read_index(js, ta, i, &elem);
        if (is_err(elem)) return elem;
        buffer->data[i] = (uint8_t)js_to_uint32(js_to_number(js, elem));
      }
  }

  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, js->builtins.buffer_proto);
}

ant_value_t Ant_Buffer_FromString(ant_t *js, ant_value_t value, ant_value_t encoding_val) {
  size_t len;
  char *str = js_getstr(js, value, &len);
  
  BufferEncoding encoding = ENC_UTF8;
  size_t enc_len = 0;
  if (vtype(encoding_val) == kTypeString && js_getstr(js, encoding_val, &enc_len) && enc_len > 0) {
    ant_value_t checked = buffer_require_encoding(js, encoding_val, &encoding);
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

  size_t cap = buffer_encode_capacity(js, value, len, encoding);
  ArrayBufferData *buffer = create_array_buffer_data(js, cap);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");

  size_t written = 0;
  ant_value_t result = buffer_encode_into(js, str, len, encoding, buffer->data, cap, &written);
  if (is_err(result)) return result;
  
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, written, js->builtins.buffer_proto);
}

static ant_value_t js_buffer_from(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Buffer.from requires at least one argument");
  if (vtype(args[0]) == kTypeString) return Ant_Buffer_FromString(js, args[0], nargs >= 2 ? args[1] : Ant_Value_Undefined);

  ArrayBufferData *arraybuffer = buffer_get_arraybuffer_data(args[0]);
  if (arraybuffer && arraybuffer->is_detached)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform Construct on a detached ArrayBuffer");
  if (arraybuffer) {
    size_t byte_offset = 0;
    size_t byte_length = arraybuffer->length;

    if (nargs > 1 && vtype(args[1]) == kTypeNumber) byte_offset = (size_t)Ant_Value_AsNumberUnchecked(args[1]);
    if (byte_offset > arraybuffer->length)
      return js_mkerr(js, "Start offset is outside the bounds of the buffer");

    bool tracking = arraybuffer->resizable;
    if (nargs > 2 && vtype(args[2]) == kTypeNumber) {
      byte_length = (size_t)Ant_Value_AsNumberUnchecked(args[2]);
      if (byte_length > arraybuffer->length - byte_offset)
        return js_mkerr(js, "Invalid Buffer length");
      tracking = false;
    } else byte_length = arraybuffer->length - byte_offset;

    return create_typed_array_with_buffer(
      js, TYPED_ARRAY_UINT8, arraybuffer, byte_offset, byte_length, js->builtins.buffer_proto, args[0], tracking
    );
  }
  
  if (!is_object_type(args[0])) return buffer_from_invalid(js, args[0]);
  ant_value_t value_of = js_getprop_fallback(js, args[0], "valueOf");
  if (is_err(value_of)) return value_of;
  
  if (is_callable(value_of)) {
    ant_value_t primitive = sv_vm_call(js->vm, js, value_of, args[0], NULL, 0, NULL, Ant_Value_Undefined);
    if (is_err(primitive)) return primitive;
    if (primitive != args[0] && (vtype(primitive) == kTypeString || is_object_type(primitive))) {
      ant_value_t from_args[3] = { primitive, nargs > 1 ? args[1] : Ant_Value_Undefined, nargs > 2 ? args[2] : Ant_Value_Undefined };
      return js_buffer_from(js, from_args, nargs > 1 ? nargs : 1, Ant_Value_Undefined, call_this, call_func);
    }
  }

  TypedArrayData *source_ta = typedarray_live(args[0]);
  if (source_ta) return buffer_from_typed_array(js, source_ta);

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
    ant_value_t primitive = sv_vm_call(js->vm, js, to_primitive, args[0], &hint, 1, NULL, Ant_Value_Undefined);
    if (is_err(primitive)) return primitive;
    if (vtype(primitive) == kTypeString) {
      ant_value_t from_args[2] = { primitive, nargs > 1 ? args[1] : Ant_Value_Undefined };
      return js_buffer_from(js, from_args, nargs > 1 ? 2 : 1, Ant_Value_Undefined, call_this, call_func);
    }
  }

  return buffer_from_invalid(js, args[0]);
}

static ant_value_t buffer_fill_range(
  ant_t *js, uint8_t *data, size_t start, size_t end,
  ant_value_t value, ant_value_t encoding_val
) {
  if (start >= end) return Ant_Value_Undefined;
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
      return Ant_Value_Undefined;
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
    return Ant_Value_Undefined;
  }

  if (vtype(value) != kTypeNumber && (buffer_get_typedarray_data(value) || buffer_is_dataview(value))) {
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
    return Ant_Value_Undefined;
  }

  double num = js_to_number(js, value);
  memset(data + start, (int)(js_to_uint32(num) & 0xff), span);
  return Ant_Value_Undefined;
}

static ant_value_t buffer_size_arg(ant_native_params_t, size_t *out) {
  ant_value_t value = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(value) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, "size", "of type number", value);

  double size = Ant_Value_AsNumberUnchecked(value);
  if (!(size >= 0 && size <= 9007199254740991.0))
    return Ant_Error_CreateOutOfRange(js, "size", ">= 0 && <= 9007199254740991", value);

  *out = (size_t)size;
  return Ant_Value_Undefined;
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

  ant_value_t fill = nargs > 1 ? args[1] : Ant_Value_Undefined;
  if (size == 0 || vtype(fill) == kTypeUndefined) return result;
  if (vtype(fill) == kTypeNumber && Ant_Value_AsNumberUnchecked(fill) == 0) return result;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  
  ant_value_t filled = buffer_fill_range(
    js, buffer->data, 0, size, fill,
    vtype(fill) == kTypeString && nargs > 2 ? args[2] : Ant_Value_Undefined
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
  return js_buffer_allocUnsafe(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
}

static ant_value_t js_buffer_of(ant_params_t) {
  return js_typedarray_of(js, args, nargs, TYPED_ARRAY_UINT8, js->builtins.buffer_proto);
}

static ant_value_t buffer_index_arg(ant_t *js, ant_value_t v, const char *name, size_t *out) {
  if (vtype(v) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, name, "of type number", v);
  
  double d = Ant_Value_AsNumberUnchecked(v);
  if (!isfinite(d) || d != trunc(d)) return Ant_Error_CreateOutOfRange(js, name, "an integer", v);
  if (d < 0 || d > 9007199254740991.0) return Ant_Error_CreateOutOfRange(js, name, ">= 0 && <= 9007199254740991", v);
  
  *out = (size_t)d;
  return Ant_Value_Undefined;
}

static ant_value_t js_buffer_copyBytesFrom(ant_params_t) {
  TypedArrayData *view = nargs > 0 ? buffer_get_typedarray_data(args[0]) : NULL;
  if (!view) return Ant_Error_CreateInvalidArgType(js, "view", "an instance of TypedArray", nargs > 0 ? args[0] : Ant_Value_Undefined);
  
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

// Buffer.prototype.toString(encoding)
static ant_value_t js_buffer_slice(ant_params_t) {
  return js_typedarray_subarray(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
}

// Buffer.prototype.toString(encoding)
static ant_value_t js_buffer_toJSON(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  size_t len = Ant_Buffer_ViewIsLive(ta_data) ? ta_data->byte_length : 0;
  const uint8_t *bytes = len ? ta_data->buffer->data + ta_data->byte_offset : NULL;

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t data = Ant_Array_Create(js);
  GC_ROOT_PIN(js, data);
  for (size_t i = 0; i < len; i++) js_arr_push(js, data, Ant_Value_FromNumber(bytes[i]));

  ant_value_t result = Ant_Object_Create(js);
  GC_ROOT_PIN(js, result);
  js_set(js, result, "type", js_mkstr(js, "Buffer", 6));
  js_set(js, result, "data", data);
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t js_buffer_toString(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  if (!Ant_Buffer_ViewIsLive(ta_data)) return js_mkstr(js, "", 0);
  
  size_t start = 0;
  size_t len = 0;
  
  buffer_string_bounds(
    nargs > 1 ? args[1] : Ant_Value_Undefined,
    nargs > 2 ? args[2] : Ant_Value_Undefined,
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
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  size_t start = 0;
  size_t end = 0;

  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  uint8_t *bytes; size_t byte_length;
  if (!Ant_Buffer_GetViewBytes(ta_data, &bytes, &byte_length))
    return js_mkerr(js, "Cannot read from detached buffer");

  end = byte_length;
  if (nargs > 0 && vtype(args[0]) == kTypeNumber && Ant_Value_AsNumberUnchecked(args[0]) > 0)
    start = (size_t)Ant_Value_AsNumberUnchecked(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber && Ant_Value_AsNumberUnchecked(args[1]) >= 0)
    end = (size_t)Ant_Value_AsNumberUnchecked(args[1]);

  if (start > byte_length) start = byte_length;
  if (end > byte_length) end = byte_length;
  if (end < start) end = start;

  return js_mkstr_utf8(js, bytes + start, end - start);
}

// Buffer.prototype.toBase64()
static ant_value_t js_buffer_toBase64(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t encoding_arg = js_mkstr(js, "base64", 6);
  ant_value_t new_args[1] = {encoding_arg};
  return js_buffer_toString(js, new_args, 1, Ant_Value_Undefined, call_this, call_func);
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

static ant_value_t buffer_index_of(ant_native_params_t, ant_value_t call_this, bool forward) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  
  if (!ta_data) return typedarray_this_error(js, call_this);
  if (nargs < 1) return Ant_Value_FromNumber(-1);

  double offset_num = forward ? 0.0 : (double)NAN;
  BufferEncoding encoding = ENC_UTF8;
  ant_value_t encoding_arg = Ant_Value_Undefined;

  if (nargs > 1 && vtype(args[1]) != kTypeUndefined) {
    if (vtype(args[1]) == kTypeString) {
      size_t enc_len = 0;
      char *enc_str = js_getstr(js, args[1], &enc_len);
      encoding = parse_encoding(enc_str, enc_len);
      encoding_arg = args[1];
    } else offset_num = js_to_number(js, args[1]);
  }

  ant_value_t search = args[0];
  if (vtype(search) != kTypeNumber && nargs > 2 && vtype(args[2]) != kTypeUndefined) {
    ant_value_t enc_value = js_to_primitive(js, args[2], 0);
    if (!is_err(enc_value)) enc_value = js_tostring_val(js, enc_value);
    if (is_err(enc_value)) return enc_value;
    
    size_t enc_len = 0;
    char *enc_str = js_getstr(js, enc_value, &enc_len);
    
    encoding = parse_encoding(enc_str, enc_len);
    encoding_arg = enc_value;
  }

  uint8_t *haystack; size_t haystack_len;
  Ant_Buffer_GetViewBytes(ta_data, &haystack, &haystack_len);
  size_t start = forward ? buffer_normalize_indexof_offset(haystack_len, offset_num) : 0;

  if (vtype(search) == kTypeNumber) {
    uint8_t needle = (uint8_t)js_to_uint32(Ant_Value_AsNumberUnchecked(search));
    
    if (!forward) {
      int64_t last = buffer_last_index_offset((int64_t)haystack_len, offset_num, 1);
      if (last < 0 || haystack_len == 0) return Ant_Value_FromNumber(-1);
      for (int64_t i = last; i >= 0; i--) if (haystack[i] == needle) return Ant_Value_FromNumber((double)i);
      return Ant_Value_FromNumber(-1);
    }
    
    if (start >= haystack_len) return Ant_Value_FromNumber(-1);
    for (size_t i = start; i < haystack_len; i++) if (haystack[i] == needle) return Ant_Value_FromNumber((double)i);
    
    return Ant_Value_FromNumber(-1);
  }

  const uint8_t *needle = NULL;
  size_t needle_len = 0;
  uint8_t *owned_needle = NULL;

  if (vtype(search) == kTypeString && encoding == ENC_UTF8)
    needle = (const uint8_t *)js_getstr(js, search, &needle_len);
  else if (vtype(search) == kTypeString) {
    if (encoding == ENC_UNKNOWN) return buffer_unknown_encoding_error(js, encoding_arg);
    ant_value_t encoded = buffer_encode_string(js, search, encoding, &needle, &needle_len, &owned_needle);
    if (is_err(encoded)) return encoded;
  } else if (!buffer_source_get_bytes(js, search, &needle, &needle_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The value argument must be one of type number or string or an instance of Buffer or Uint8Array");

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
  return Ant_Value_FromNumber(found);
}

static ant_value_t js_buffer_indexOf(ant_params_t) {
  return buffer_index_of(js, args, nargs, call_this, true);
}

static ant_value_t js_buffer_lastIndexOf(ant_params_t) {
  return buffer_index_of(js, args, nargs, call_this, false);
}

static ant_value_t js_buffer_includes(ant_params_t) {
  ant_value_t result = buffer_index_of(js, args, nargs, call_this, true);
  if (is_err(result)) return result;
  return Ant_Value_FromBool(Ant_Value_AsNumberUnchecked(result) != -1);
}

static ant_value_t buffer_validate_offset(ant_t *js, ant_value_t value, const char *name, size_t max, size_t *out) {
  if (vtype(value) != kTypeNumber) {
    ant_value_t props = Ant_Object_CreateBare(js);
    js_set(js, props, "code", js_mkstr(js, "ERR_INVALID_ARG_TYPE", 20));
    return js_mkerr_props(js, JS_ERR_TYPE, props, "The \"%s\" argument must be of type number", name);
  }
  
  double num = Ant_Value_AsNumberUnchecked(value);
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
  return Ant_Value_Undefined;
}

// Buffer.prototype.fill(value[, offset[, end]][, encoding])
static ant_value_t js_buffer_fill(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");

  ant_value_t value = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (
    vtype(value) != kTypeNumber && vtype(value) != kTypeString &&
    !buffer_get_typedarray_data(value) && !buffer_is_dataview(value)
  ) value = Ant_Value_FromNumber(js_to_number(js, value));

  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &len))
    return js_mkerr(js, "Cannot write to detached buffer");

  ant_value_t offset_val = nargs > 1 ? args[1] : Ant_Value_Undefined;
  ant_value_t end_val = nargs > 2 ? args[2] : Ant_Value_Undefined;
  ant_value_t encoding_val = nargs > 3 ? args[3] : Ant_Value_Undefined;

  if (vtype(value) == kTypeString) {
    if (vtype(offset_val) == kTypeUndefined || vtype(offset_val) == kTypeString) {
      encoding_val = offset_val;
      offset_val = end_val = Ant_Value_Undefined;
    } else if (vtype(end_val) == kTypeString) {
      encoding_val = end_val;
      end_val = Ant_Value_Undefined;
    }
  } else encoding_val = Ant_Value_Undefined;

  size_t start = 0, end = len;

  if (vtype(offset_val) != kTypeUndefined) {
    ant_value_t checked = buffer_validate_offset(js, offset_val, "offset", SIZE_MAX >> 1, &start);
    if (is_err(checked)) return checked;
    if (vtype(end_val) != kTypeUndefined) {
      checked = buffer_validate_offset(js, end_val, "end", len, &end);
      if (is_err(checked)) return checked;
    }
    if (start >= end) return call_this;
  }

  ant_value_t result = buffer_fill_range(js, data, start, end, value, encoding_val);
  return is_err(result) ? result : call_this;
}

// Buffer.prototype.write(string, offset, length, encoding)
static ant_value_t js_buffer_write(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  if (nargs < 1 || vtype(args[0]) != kTypeString)
    return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", "argument must be a string");

  size_t str_len;
  char *str = js_getstr(js, args[0], &str_len);
  
  uint8_t *bytes; size_t buf_len;
  Ant_Buffer_GetViewBytes(ta_data, &bytes, &buf_len);
  size_t offset = 0;
  size_t length = buf_len;
  BufferEncoding encoding = ENC_UTF8;
  
  ant_value_t offset_val = nargs > 1 ? args[1] : Ant_Value_Undefined;
  ant_value_t length_val = nargs > 2 ? args[2] : Ant_Value_Undefined;
  ant_value_t encoding_val = nargs > 3 ? args[3] : Ant_Value_Undefined;

  if (vtype(offset_val) == kTypeUndefined) encoding_val = Ant_Value_Undefined;
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
    if (vtype(encoding_val) != kTypeString) Ant_Buffer_GetViewBytes(ta_data, &bytes, &buf_len);
  }
  
  if (offset >= buf_len) return Ant_Value_FromNumber(0);
  size_t available = buf_len - offset;
  if (length < available) available = length;

  size_t to_write = 0;
  ant_value_t result = buffer_encode_into(js, str, str_len, encoding, bytes + offset, available, &to_write);
  if (is_err(result)) return result;
  return Ant_Value_FromNumber((double)to_write);
}

static ant_value_t js_buffer_copy(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "copy requires a target buffer");

  TypedArrayData *src = buffer_get_typedarray_data(call_this);
  TypedArrayData *dst = buffer_get_typedarray_data(args[0]);
  if (!src || !dst) return js_mkerr(js, "copy requires Buffer arguments");

  uint8_t *src_bytes, *dst_bytes; size_t src_size, dst_size;
  Ant_Buffer_GetViewBytes(src, &src_bytes, &src_size);
  Ant_Buffer_GetViewBytes(dst, &dst_bytes, &dst_size);

  size_t target_start = (nargs > 1 && vtype(args[1]) == kTypeNumber) ? (size_t)Ant_Value_AsNumberUnchecked(args[1]) : 0;
  size_t source_start = (nargs > 2 && vtype(args[2]) == kTypeNumber) ? (size_t)Ant_Value_AsNumberUnchecked(args[2]) : 0;
  size_t source_end = (nargs > 3 && vtype(args[3]) == kTypeNumber) ? (size_t)Ant_Value_AsNumberUnchecked(args[3]) : src_size;

  if (target_start > dst_size) target_start = dst_size;
  if (source_start > src_size) source_start = src_size;
  if (source_end > src_size) source_end = src_size;
  if (source_end < source_start) source_end = source_start;

  size_t src_len = source_end - source_start;
  size_t dst_len = dst_size - target_start;
  size_t copy_len = src_len < dst_len ? src_len : dst_len;
  if (copy_len == 0) return Ant_Value_FromNumber(0);

  memmove(dst_bytes + target_start, src_bytes + source_start, copy_len);
  
  return Ant_Value_FromNumber((double)copy_len);
}

static __attribute__((noinline, cold)) ant_value_t buffer_bounds_error(ant_t *js, ant_value_t offset, double max) {
  double n = Ant_Value_AsNumberUnchecked(offset);
  if (floor(n) != n) return Ant_Error_CreateOutOfRange(js, "offset", "an integer", offset);
  if (max < 0) return js_mkerr_code(js, JS_ERR_RANGE, "ERR_BUFFER_OUT_OF_BOUNDS", "Attempt to access memory outside buffer bounds");
  
  char range[64];
  if (isnan(max)) snprintf(range, sizeof(range), ">= 0 and <= NaN");
  else snprintf(range, sizeof(range), ">= 0 and <= %.0f", max);
  return Ant_Error_CreateOutOfRange(js, "offset", range, offset);
}

static ant_value_t buffer_access_ptr(ant_t *js, ant_value_t call_this, ant_value_t offset_value, size_t width, uint8_t **ptr, size_t *offset) {
  if (vtype(offset_value) == kTypeUndefined) offset_value = Ant_Value_FromNumber(0);
  if (vtype(offset_value) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, "offset", "of type number", offset_value);
  
  TypedArrayData *ta = buffer_get_typedarray_data(call_this);
  uint8_t *bytes; size_t live_len;
  Ant_Buffer_GetViewBytes(ta, &bytes, &live_len);

  double n = Ant_Value_AsNumberUnchecked(offset_value);
  double length = ta ? (double)live_len : js_to_number(js, js_getprop_fallback(js, call_this, "length"));
  double max = length - (double)width;
  if (floor(n) != n || n < 0 || !(n <= max)) return buffer_bounds_error(js, offset_value, max);
  if (!ta) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid Buffer");

  *offset = (size_t)n;
  *ptr = bytes + *offset;
  return Ant_Value_Undefined;
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
  double n = Ant_Value_AsNumberUnchecked(value);
  
  if (isnan(n)) return true;
  return is_signed ? n >= -0x1p63 && n < 0x1p63 : n >= 0 && n < 0x1p64;
}

static ant_value_t buffer_write_bigint64(ant_native_params_t, ant_value_t call_this, bool little_endian, bool is_signed) {
  ant_value_t value = buffer_unwrap_bigint(nargs > 0 ? args[0] : Ant_Value_Undefined);
  const char *range = is_signed ? ">= -(2n ** 63n) and < 2n ** 63n" : ">= 0n and < 2n ** 64n";
  
  uint64_t bits = 0;
  if (!buffer_bigint64_in_range(js, value, is_signed, &bits)) return Ant_Error_CreateOutOfRange(js, "value", range, value);
  
  size_t offset; uint8_t *ptr;
  ant_value_t error = buffer_access_ptr(js, call_this, nargs > 1 ? args[1] : Ant_Value_Undefined, 8, &ptr, &offset);
  if (is_err(error)) return error;
  
  if (vtype(value) != kTypeBigInt) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot mix BigInt and other types, use explicit conversions");
  buffer_store_bytes(ptr, 8, little_endian, bits);
  
  return Ant_Value_FromNumber((double)(offset + 8));
}

static ant_value_t buffer_num_width(ant_t *js, ant_value_t value, size_t *width) {
  if (vtype(value) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, "byteLength", "of type number", value);
  double n = Ant_Value_AsNumberUnchecked(value);
  if (floor(n) != n) return Ant_Error_CreateOutOfRange(js, "byteLength", "an integer", value);
  if (n < 1 || n > 6) return Ant_Error_CreateOutOfRange(js, "byteLength", ">= 1 and <= 6", value);
  *width = (size_t)n;
  return Ant_Value_Undefined;
}

static __attribute__((cold, noinline)) ant_value_t buffer_num_range_error(
  ant_t *js, buffer_num_kind_t kind, size_t width, double lo, double hi, double value
) {
  char range[96];
  unsigned bits_width = 8 * (unsigned)width;
  if (width <= 4) snprintf(range, sizeof(range), ">= %.0f and <= %.0f", lo, hi);
  else if (kind == BUFFER_NUM_UINT) snprintf(range, sizeof(range), ">= 0 and < 2 ** %u", bits_width);
  else snprintf(range, sizeof(range), ">= -(2 ** %u) and < 2 ** %u", bits_width - 1, bits_width - 1);
  return Ant_Error_CreateOutOfRange(js, "value", range, Ant_Value_FromNumber(value));
}

static inline __attribute__((always_inline)) ant_value_t buffer_read_num(
  ant_native_params_t, ant_value_t call_this, buffer_num_kind_t kind, size_t width, buffer_num_endian_t endian
) {
  if (!width) {
    ant_value_t r = buffer_num_width(js, nargs > 1 ? args[1] : Ant_Value_Undefined, &width);
    if (is_err(r)) return r;
  }
  
  size_t offset; uint8_t *ptr;
  ant_value_t error = buffer_access_ptr(js, call_this, nargs > 0 ? args[0] : Ant_Value_Undefined, width, &ptr, &offset);
  if (is_err(error)) return error;
  
  return buffer_num_decode(js, buffer_load_bytes(ptr, width, endian == BUFFER_NUM_LE), kind, width);
}

static inline __attribute__((always_inline)) ant_value_t buffer_write_num(ant_native_params_t, ant_value_t call_this, buffer_num_kind_t kind, size_t width, buffer_num_endian_t endian) {
  if (kind == BUFFER_NUM_BIGINT || kind == BUFFER_NUM_BIGUINT)
    return buffer_write_bigint64(js, args, nargs, call_this, endian == BUFFER_NUM_LE, kind == BUFFER_NUM_BIGINT);
  
  ant_value_t arg = nargs > 0 ? args[0] : Ant_Value_Undefined;
  double value = vtype(arg) == kTypeNumber ? Ant_Value_AsNumberUnchecked(arg) : js_to_number(js, arg);
  
  if (!width) {
    ant_value_t r = buffer_num_width(js, nargs > 2 ? args[2] : Ant_Value_Undefined, &width);
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
  ant_value_t error = buffer_access_ptr(js, call_this, nargs > 1 ? args[1] : Ant_Value_Undefined, width, &ptr, &offset);
  if (is_err(error)) return error;
  
  buffer_store_bytes(ptr, width, endian == BUFFER_NUM_LE, bits);
  return Ant_Value_FromNumber((double)(offset + width));
}

#define BUFFER_NUM_METHOD(type, suffix, kind, width, endian)                                            \
  static ant_value_t js_buffer_read##type##suffix(ant_params_t) {                                       \
    return buffer_read_num(js, args, nargs, call_this, BUFFER_NUM_##kind, width, BUFFER_NUM_##endian);  \
  }                                                                                                     \
  static ant_value_t js_buffer_write##type##suffix(ant_params_t) {                                      \
    return buffer_write_num(js, args, nargs, call_this, BUFFER_NUM_##kind, width, BUFFER_NUM_##endian); \
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

#define BUFFER_NUM_ENTRY(op, type, suffix, kind, width) {                                      \
  { js_buffer_##op##type##suffix, #op #type #suffix, BUFFER_NUM_LENGTH_##op(kind, width), 0 }, \
  BUFFER_NUM_ALIAS_##kind(#op, suffix) },

static const buffer_num_method_t buffer_num_methods[] = {
  #define BUFFER_NUM_METHOD(type, suffix, kind, width, endian) BUFFER_NUM_ENTRY(read, type, suffix, kind, width)
  #include "buffer/num_methods.h"
  #define BUFFER_NUM_METHOD(type, suffix, kind, width, endian) BUFFER_NUM_ENTRY(write, type, suffix, kind, width)
  #include "buffer/num_methods.h"
};

// Buffer.isBuffer(obj)
static ant_value_t js_buffer_isBuffer(ant_params_t) {
  if (nargs < 1) return Ant_Value_False;
  if (!is_special_object(args[0])) return Ant_Value_False;
  
  ant_value_t proto = js_get_proto(js, args[0]);
  return Ant_Value_FromBool(proto == js->builtins.buffer_proto);
}

// Buffer.isEncoding(encoding)
static ant_value_t js_buffer_isEncoding(ant_params_t) {
  return Ant_Value_FromBool(nargs > 0 && Ant_Buffer_IsEncoding(js, args[0]));
}

// Buffer.byteLength(string, encoding)
static ant_value_t js_buffer_byteLength(ant_params_t) {
  if (nargs < 1) return Ant_Value_FromNumber(0);
  
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
    
    return Ant_Value_FromNumber((double)buffer_encoded_length(js, arg, str, len, encoding));
  }
  
  return Ant_Value_FromNumber(0);
}

// Buffer.concat(list, totalLength)
static ant_value_t js_buffer_concat(ant_params_t) {
  ant_value_t list = nargs > 0 ? args[0] : Ant_Value_Undefined;
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

  if (nargs > 1 && vtype(args[1]) == kTypeNumber) total_length = (size_t)Ant_Value_AsNumberUnchecked(args[1]);
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
  
  return Ant_Value_FromNumber((double)cmp);
}

// Buffer.prototype.compare(target)
static ant_value_t js_buffer_proto_compare(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Buffer.compare requires a target buffer");
  
  TypedArrayData *ta1 = buffer_get_typedarray_data(call_this);
  TypedArrayData *ta2 = buffer_get_typedarray_data(args[0]);
  if (!ta1 || !ta2) return js_mkerr(js, "Arguments must be Buffers");
  
  return buffer_compare_arrays(js, ta1, ta2);
}

// Buffer.prototype.equals(target)
static ant_value_t js_buffer_proto_equals(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Buffer.equals requires a target buffer");

  TypedArrayData *ta1 = buffer_get_typedarray_data(call_this);
  TypedArrayData *ta2 = buffer_get_typedarray_data(args[0]);
  if (!ta1 || !ta2) return js_mkerr(js, "Arguments must be Buffers");
  if (ta1->byte_length != ta2->byte_length) return Ant_Value_FromBool(false);

  ant_value_t cmp = buffer_compare_arrays(js, ta1, ta2);
  if (is_err(cmp)) return cmp;
  return Ant_Value_FromBool(Ant_Value_AsNumberUnchecked(cmp) == 0);
}

// Buffer.compare(buf1, buf2)
static ant_value_t js_buffer_compare(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "Buffer.compare requires two arguments");
  
  TypedArrayData *ta1 = buffer_get_typedarray_data(args[0]);
  TypedArrayData *ta2 = buffer_get_typedarray_data(args[1]);
  if (!ta1 || !ta2) return js_mkerr(js, "Arguments must be Buffers");
  
  return buffer_compare_arrays(js, ta1, ta2);
}

static ant_value_t buffer_make_constants(ant_t *js) {
  ant_value_t constants = Ant_Object_Create(js);
  js_set(js, constants, "MAX_LENGTH", Ant_Value_FromNumber(4294967296.0));
  js_set(js, constants, "MAX_STRING_LENGTH", Ant_Value_FromNumber(ANT_STRING_MAX_LENGTH));
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
  js_set(js, lib, "kMaxLength", Ant_Value_FromNumber(4294967296.0));
  js_set(js, lib, "kStringMaxLength", Ant_Value_FromNumber(ANT_STRING_MAX_LENGTH));
  js_set(js, lib, "INSPECT_MAX_BYTES", Ant_Value_FromNumber(BUFFER_INSPECT_MAX_BYTES));

  return lib;
}

void init_buffer_module(ant_t *js) {
  ant_value_t glob = js->global;

  arraybuffer_initialize(js);
  ant_value_t typedarray_proto = typedarray_initialize(js);
  uint8array_codec_initialize(js);
  dataview_initialize(js);
  sharedarraybuffer_initialize(js);

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
  mkprop_interned_exact(js, buffer_proto, intern_string(js, "toLocaleString", 14), js_get(js, buffer_proto, "toString"), ANT_PROP_ATTR_DEFAULT);
  
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
  js_set(js, buffer_ctor_obj, "poolSize", Ant_Value_FromNumber(8192));
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
  mkprop_exact_attrs(js, buffer_ctor_obj, Ant_String_FromLiteral(js, "prototype"), buffer_proto, ANT_PROP_ATTR_WRITABLE);
  mkprop_exact_attrs(js, buffer_ctor_obj, Ant_String_FromLiteral(js, "name"), Ant_String_FromLiteral(js, "Buffer"), ANT_PROP_ATTR_CONFIGURABLE);
  
  ant_value_t buffer_ctor = js_obj_to_func(js, buffer_ctor_obj);
  defmethod(js, buffer_proto, "constructor", 11, buffer_ctor);
  js_set_global_builtin(js, "Buffer", buffer_ctor);

  ant_typed_array_protector_holder_mark(typedarray_proto);
  ant_typed_array_protector_holder_mark(buffer_proto);
  
  for (int type = 0; type < TYPED_ARRAY_COUNT; type++)
    ant_typed_array_protector_holder_mark(js->builtins.typedarray_protos[type]);
}
