#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "runtime.h"
#include "modules/buffer.h"
#include "modules/symbol.h"

static jsval_t js_arraybuffer_slice(struct js *js, jsval_t *args, int nargs);
static jsval_t js_typedarray_slice(struct js *js, jsval_t *args, int nargs);
static jsval_t js_typedarray_subarray(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getUint8(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_setUint8(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getInt16(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getInt32(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getFloat32(struct js *js, jsval_t *args, int nargs);
static jsval_t js_buffer_toString(struct js *js, jsval_t *args, int nargs);
static jsval_t js_buffer_toBase64(struct js *js, jsval_t *args, int nargs);
static jsval_t js_buffer_write(struct js *js, jsval_t *args, int nargs);

static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const unsigned char base64_decode_table[256] = {
  ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,  ['G'] = 6,  ['H'] = 7,
  ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15,
  ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
  ['Y'] = 24, ['Z'] = 25, ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
  ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
  ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47,
  ['w'] = 48, ['x'] = 49, ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55,
  ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63,
};

static char *base64_encode(const uint8_t *data, size_t len, size_t *out_len) {
  size_t encoded_len = 4 * ((len + 2) / 3);
  char *result = malloc(encoded_len + 1);
  if (!result) return NULL;
  
  size_t j = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t octet_a = i < len ? data[i] : 0;
    uint32_t octet_b = i + 1 < len ? data[i + 1] : 0;
    uint32_t octet_c = i + 2 < len ? data[i + 2] : 0;
    uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
    
    result[j++] = base64_chars[(triple >> 18) & 0x3F];
    result[j++] = base64_chars[(triple >> 12) & 0x3F];
    result[j++] = (i + 1 < len) ? base64_chars[(triple >> 6) & 0x3F] : '=';
    result[j++] = (i + 2 < len) ? base64_chars[triple & 0x3F] : '=';
  }
  
  result[j] = '\0';
  *out_len = j;
  return result;
}

static uint8_t *base64_decode(const char *data, size_t len, size_t *out_len) {
  if (len % 4 != 0) return NULL;
  
  size_t decoded_len = len / 4 * 3;
  if (len > 0 && data[len - 1] == '=') decoded_len--;
  if (len > 1 && data[len - 2] == '=') decoded_len--;
  
  uint8_t *result = malloc(decoded_len);
  if (!result) return NULL;
  
  size_t j = 0;
  for (size_t i = 0; i < len; i += 4) {
    uint32_t sextet_a = base64_decode_table[(unsigned char)data[i]];
    uint32_t sextet_b = base64_decode_table[(unsigned char)data[i + 1]];
    uint32_t sextet_c = data[i + 2] == '=' ? 0 : base64_decode_table[(unsigned char)data[i + 2]];
    uint32_t sextet_d = data[i + 3] == '=' ? 0 : base64_decode_table[(unsigned char)data[i + 3]];
    uint32_t triple = (sextet_a << 18) + (sextet_b << 12) + (sextet_c << 6) + sextet_d;
    
    if (j < decoded_len) result[j++] = (triple >> 16) & 0xFF;
    if (j < decoded_len) result[j++] = (triple >> 8) & 0xFF;
    if (j < decoded_len) result[j++] = triple & 0xFF;
  }
  
  *out_len = decoded_len;
  return result;
}

static ArrayBufferData *create_array_buffer_data(size_t length) {
  ArrayBufferData *data = malloc(sizeof(ArrayBufferData));
  if (!data) return NULL;
  
  data->data = calloc(length, 1);
  if (!data->data && length > 0) {
    free(data);
    return NULL;
  }
  
  data->length = length;
  data->capacity = length;
  data->ref_count = 1;
  data->is_shared = 0;
  return data;
}

static ArrayBufferData *create_shared_array_buffer_data(size_t length) {
  ArrayBufferData *data = create_array_buffer_data(length);
  if (data) data->is_shared = 1;
  return data;
}

static void free_array_buffer_data(ArrayBufferData *data) {
  if (!data) return;
  data->ref_count--;
  if (data->ref_count <= 0) {
    free(data->data);
    free(data);
  }
}

static size_t get_element_size(TypedArrayType type) {
  switch (type) {
    case TYPED_ARRAY_INT8:
    case TYPED_ARRAY_UINT8:
    case TYPED_ARRAY_UINT8_CLAMPED:
      return 1;
    case TYPED_ARRAY_INT16:
    case TYPED_ARRAY_UINT16:
      return 2;
    case TYPED_ARRAY_INT32:
    case TYPED_ARRAY_UINT32:
    case TYPED_ARRAY_FLOAT32:
      return 4;
    case TYPED_ARRAY_FLOAT64:
    case TYPED_ARRAY_BIGINT64:
    case TYPED_ARRAY_BIGUINT64:
      return 8;
    default:
      return 1;
  }
}

// ArrayBuffer constructor
static jsval_t js_arraybuffer_constructor(struct js *js, jsval_t *args, int nargs) {
  size_t length = 0;
  if (nargs > 0 && js_type(args[0]) == JS_NUM) {
    length = (size_t)js_getnum(args[0]);
  }
  
  ArrayBufferData *data = create_array_buffer_data(length);
  if (!data) {
    return js_mkerr(js, "Failed to allocate ArrayBuffer");
  }
  
  jsval_t obj = js_mkobj(js);
  js_set(js, obj, "_arraybuffer_data", js_mknum((double)(uintptr_t)data));
  js_set(js, obj, "byteLength", js_mknum((double)length));
  js_set(js, obj, "slice", js_mkfun(js_arraybuffer_slice));
  
  return obj;
}

// ArrayBuffer.prototype.slice(begin, end)
static jsval_t js_arraybuffer_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t data_val = js_get(js, this_val, "_arraybuffer_data");
  
  if (js_type(data_val) != JS_NUM) {
    return js_mkerr(js, "Not an ArrayBuffer");
  }
  
  ArrayBufferData *data = (ArrayBufferData *)(uintptr_t)js_getnum(data_val);
  if (!data) return js_mkerr(js, "Invalid ArrayBuffer");
  
  int begin = 0, end = data->length;
  if (nargs > 0 && js_type(args[0]) == JS_NUM) begin = (int)js_getnum(args[0]);
  if (nargs > 1 && js_type(args[1]) == JS_NUM) end = (int)js_getnum(args[1]);
  
  if (begin < 0) begin = data->length + begin;
  if (end < 0) end = data->length + end;
  if (begin < 0) begin = 0;
  if (end < 0) end = 0;
  if (begin > (int)data->length) begin = data->length;
  if (end > (int)data->length) end = data->length;
  if (end < begin) end = begin;
  
  size_t new_length = end - begin;
  ArrayBufferData *new_data = create_array_buffer_data(new_length);
  if (!new_data) return js_mkerr(js, "Failed to allocate new ArrayBuffer");
  
  memcpy(new_data->data, data->data + begin, new_length);
  
  jsval_t new_obj = js_mkobj(js);
  js_set(js, new_obj, "_arraybuffer_data", js_mknum((double)(uintptr_t)new_data));
  js_set(js, new_obj, "byteLength", js_mknum((double)new_length));
  js_set(js, new_obj, "slice", js_mkfun(js_arraybuffer_slice));
  
  return new_obj;
}

static jsval_t typedarray_index_getter(struct js *js, jsval_t obj, const char *key, size_t key_len) {
  if (key_len == 0 || key_len > 15) return js_mkundef();
  
  char *endptr;
  long index = strtol(key, &endptr, 10);
  if (endptr != key + key_len || index < 0) return js_mkundef();
  
  jsval_t ta_data_val = js_get(js, obj, "_typedarray_data");
  if (js_type(ta_data_val) != JS_NUM) return js_mkundef();
  
  TypedArrayData *ta_data = (TypedArrayData *)(uintptr_t)js_getnum(ta_data_val);
  if (!ta_data || (size_t)index >= ta_data->length) return js_mkundef();
  
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  double value;
  
  static const void *dispatch[] = {
    &&L_INT8, &&L_UINT8, &&L_UINT8, &&L_INT16, &&L_UINT16,
    &&L_INT32, &&L_UINT32, &&L_FLOAT32, &&L_FLOAT64, &&L_UNDEF, &&L_UNDEF
  };
  
  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto L_UNDEF;
  goto *dispatch[ta_data->type];
  
  L_INT8:    value = (double)((int8_t*)data)[index];   goto L_DONE;
  L_UINT8:   value = (double)data[index];              goto L_DONE;
  L_INT16:   value = (double)((int16_t*)data)[index];  goto L_DONE;
  L_UINT16:  value = (double)((uint16_t*)data)[index]; goto L_DONE;
  L_INT32:   value = (double)((int32_t*)data)[index];  goto L_DONE;
  L_UINT32:  value = (double)((uint32_t*)data)[index]; goto L_DONE;
  L_FLOAT32: value = (double)((float*)data)[index];    goto L_DONE;
  L_FLOAT64: value = ((double*)data)[index];           goto L_DONE;
  L_UNDEF:   return js_mkundef();
  L_DONE:    return js_mknum(value);
}

static jsval_t create_typed_array(struct js *js, TypedArrayType type, ArrayBufferData *buffer, size_t byte_offset, size_t length, const char *type_name) {
  TypedArrayData *ta_data = malloc(sizeof(TypedArrayData));
  if (!ta_data) return js_mkerr(js, "Failed to allocate TypedArray");
  
  size_t element_size = get_element_size(type);
  ta_data->buffer = buffer;
  ta_data->type = type;
  ta_data->byte_offset = byte_offset;
  ta_data->byte_length = length * element_size;
  ta_data->length = length;
  buffer->ref_count++;
  
  jsval_t obj = js_mkobj(js);
  jsval_t proto = js_get_ctor_proto(js, type_name, strlen(type_name));
  if (js_type(proto) == JS_OBJ) js_set_proto(js, obj, proto);
  
  js_set(js, obj, "_typedarray_data", js_mknum((double)(uintptr_t)ta_data));
  js_set(js, obj, "length", js_mknum((double)length));
  js_set(js, obj, "byteLength", js_mknum((double)(length * element_size)));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset));
  js_set(js, obj, "BYTES_PER_ELEMENT", js_mknum((double)element_size));
  js_set(js, obj, "slice", js_mkfun(js_typedarray_slice));
  js_set(js, obj, "subarray", js_mkfun(js_typedarray_subarray));
  
  js_set_getter(js, obj, typedarray_index_getter);
  
  return obj;
}

static jsval_t js_typedarray_constructor(struct js *js, jsval_t *args, int nargs, TypedArrayType type, const char *type_name) {
  if (nargs == 0) {
    ArrayBufferData *buffer = create_array_buffer_data(0);
    return create_typed_array(js, type, buffer, 0, 0, type_name);
  }
  
  if (js_type(args[0]) == JS_NUM) {
    size_t length = (size_t)js_getnum(args[0]);
    size_t element_size = get_element_size(type);
    ArrayBufferData *buffer = create_array_buffer_data(length * element_size);
    if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
    return create_typed_array(js, type, buffer, 0, length, type_name);
  }
  
  jsval_t buffer_data_val = js_get(js, args[0], "_arraybuffer_data");
  if (js_type(buffer_data_val) == JS_NUM) {
    ArrayBufferData *buffer = (ArrayBufferData *)(uintptr_t)js_getnum(buffer_data_val);
    size_t byte_offset = 0;
    size_t length = buffer->length;
    
    if (nargs > 1 && js_type(args[1]) == JS_NUM) {
      byte_offset = (size_t)js_getnum(args[1]);
    }
    
    size_t element_size = get_element_size(type);
    if (nargs > 2 && js_type(args[2]) == JS_NUM) {
      length = (size_t)js_getnum(args[2]);
    } else {
      length = (buffer->length - byte_offset) / element_size;
    }
    
    return create_typed_array(js, type, buffer, byte_offset, length, type_name);
  }
  
  return js_mkerr(js, "Invalid TypedArray constructor arguments");
}

// TypedArray.prototype.slice(begin, end)
static jsval_t js_typedarray_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get(js, this_val, "_typedarray_data");
  
  if (js_type(ta_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a TypedArray");
  }
  
  TypedArrayData *ta_data = (TypedArrayData *)(uintptr_t)js_getnum(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  int begin = 0, end = ta_data->length;
  if (nargs > 0 && js_type(args[0]) == JS_NUM) begin = (int)js_getnum(args[0]);
  if (nargs > 1 && js_type(args[1]) == JS_NUM) end = (int)js_getnum(args[1]);
  
  if (begin < 0) begin = ta_data->length + begin;
  if (end < 0) end = ta_data->length + end;
  if (begin < 0) begin = 0;
  if (end < 0) end = 0;
  if (begin > (int)ta_data->length) begin = ta_data->length;
  if (end > (int)ta_data->length) end = ta_data->length;
  if (end < begin) end = begin;
  
  size_t new_length = end - begin;
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(new_length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(
    new_buffer->data, 
    ta_data->buffer->data + ta_data->byte_offset + begin * element_size,
    new_length * element_size
  );
  
  return create_typed_array(js, ta_data->type, new_buffer, 0, new_length, "TypedArray");
}

// TypedArray.prototype.subarray(begin, end)
static jsval_t js_typedarray_subarray(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get(js, this_val, "_typedarray_data");
  
  if (js_type(ta_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a TypedArray");
  }
  
  TypedArrayData *ta_data = (TypedArrayData *)(uintptr_t)js_getnum(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  int begin = 0, end = ta_data->length;
  if (nargs > 0 && js_type(args[0]) == JS_NUM) begin = (int)js_getnum(args[0]);
  if (nargs > 1 && js_type(args[1]) == JS_NUM) end = (int)js_getnum(args[1]);
  
  if (begin < 0) begin = ta_data->length + begin;
  if (end < 0) end = ta_data->length + end;
  if (begin < 0) begin = 0;
  if (end < 0) end = 0;
  if (begin > (int)ta_data->length) begin = ta_data->length;
  if (end > (int)ta_data->length) end = ta_data->length;
  if (end < begin) end = begin;
  
  size_t new_length = end - begin;
  size_t element_size = get_element_size(ta_data->type);
  size_t new_offset = ta_data->byte_offset + begin * element_size;
  
  return create_typed_array(js, ta_data->type, ta_data->buffer, new_offset, new_length, "TypedArray");
}

#define DEFINE_TYPEDARRAY_CONSTRUCTOR(name, type) \
  static jsval_t js_##name##_constructor(struct js *js, jsval_t *args, int nargs) { \
    return js_typedarray_constructor(js, args, nargs, type, #name); \
  }

DEFINE_TYPEDARRAY_CONSTRUCTOR(Int8Array, TYPED_ARRAY_INT8)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Uint8Array, TYPED_ARRAY_UINT8)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Uint8ClampedArray, TYPED_ARRAY_UINT8_CLAMPED)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Int16Array, TYPED_ARRAY_INT16)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Uint16Array, TYPED_ARRAY_UINT16)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Int32Array, TYPED_ARRAY_INT32)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Uint32Array, TYPED_ARRAY_UINT32)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Float32Array, TYPED_ARRAY_FLOAT32)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Float64Array, TYPED_ARRAY_FLOAT64)
DEFINE_TYPEDARRAY_CONSTRUCTOR(BigInt64Array, TYPED_ARRAY_BIGINT64)
DEFINE_TYPEDARRAY_CONSTRUCTOR(BigUint64Array, TYPED_ARRAY_BIGUINT64)

static jsval_t js_dataview_constructor(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "DataView requires an ArrayBuffer");
  }
  
  jsval_t buffer_data_val = js_get(js, args[0], "_arraybuffer_data");
  if (js_type(buffer_data_val) != JS_NUM) {
    return js_mkerr(js, "First argument must be an ArrayBuffer");
  }
  
  ArrayBufferData *buffer = (ArrayBufferData *)(uintptr_t)js_getnum(buffer_data_val);
  size_t byte_offset = 0;
  size_t byte_length = buffer->length;
  
  if (nargs > 1 && js_type(args[1]) == JS_NUM) {
    byte_offset = (size_t)js_getnum(args[1]);
  }
  
  if (nargs > 2 && js_type(args[2]) == JS_NUM) {
    byte_length = (size_t)js_getnum(args[2]);
  } else {
    byte_length = buffer->length - byte_offset;
  }
  
  DataViewData *dv_data = malloc(sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");
  
  dv_data->buffer = buffer;
  dv_data->byte_offset = byte_offset;
  dv_data->byte_length = byte_length;
  buffer->ref_count++;
  
  jsval_t obj = js_mkobj(js);
  js_set(js, obj, "_dataview_data", js_mknum((double)(uintptr_t)dv_data));
  js_set(js, obj, "byteLength", js_mknum((double)byte_length));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset));
  js_set(js, obj, "getUint8", js_mkfun(js_dataview_getUint8));
  js_set(js, obj, "setUint8", js_mkfun(js_dataview_setUint8));
  js_set(js, obj, "getInt16", js_mkfun(js_dataview_getInt16));
  js_set(js, obj, "getInt32", js_mkfun(js_dataview_getInt32));
  js_set(js, obj, "getFloat32", js_mkfun(js_dataview_getFloat32));
  
  return obj;
}

// DataView.prototype.getUint8(byteOffset)
static jsval_t js_dataview_getUint8(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getUint8 requires byteOffset");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get(js, this_val, "_dataview_data");
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
  size_t offset = (size_t)js_getnum(args[0]);
  
  if (offset >= dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  uint8_t value = dv->buffer->data[dv->byte_offset + offset];
  return js_mknum((double)value);
}

// DataView.prototype.setUint8(byteOffset, value)
static jsval_t js_dataview_setUint8(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setUint8 requires byteOffset and value");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get(js, this_val, "_dataview_data");
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
  size_t offset = (size_t)js_getnum(args[0]);
  uint8_t value = (uint8_t)js_getnum(args[1]);
  
  if (offset >= dv->byte_length) {
    return js_mkerr(js, "Offset out of bounds");
  }
  
  dv->buffer->data[dv->byte_offset + offset] = value;
  return js_mkundef();
}

// DataView.prototype.getInt16(byteOffset, littleEndian)
static jsval_t js_dataview_getInt16(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getInt16 requires byteOffset");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get(js, this_val, "_dataview_data");
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
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

// DataView.prototype.getInt32(byteOffset, littleEndian)
static jsval_t js_dataview_getInt32(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getInt32 requires byteOffset");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get(js, this_val, "_dataview_data");
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
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
static jsval_t js_dataview_getFloat32(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getFloat32 requires byteOffset");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get(js, this_val, "_dataview_data");
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
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

// Buffer.from(array/string/buffer)
static jsval_t js_buffer_from(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Buffer.from requires at least one argument");
  }
  
  if (js_type(args[0]) == JS_STR) {
    size_t len;
    char *str = js_getstr(js, args[0], &len);
    
    ArrayBufferData *buffer = create_array_buffer_data(len);
    if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
    
    memcpy(buffer->data, str, len);
    jsval_t obj = create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Buffer");
    js_set(js, obj, "toString", js_mkfun(js_buffer_toString));
    js_set(js, obj, "toBase64", js_mkfun(js_buffer_toBase64));
    js_set(js, obj, "write", js_mkfun(js_buffer_write));
    return obj;
  }
  
  jsval_t length_val = js_get(js, args[0], "length");
  if (js_type(length_val) == JS_NUM) {
    size_t len = (size_t)js_getnum(length_val);
    ArrayBufferData *buffer = create_array_buffer_data(len);
    if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
    
    for (size_t i = 0; i < len; i++) {
      char idx_str[32];
      snprintf(idx_str, sizeof(idx_str), "%zu", i);
      jsval_t elem = js_get(js, args[0], idx_str);
      if (js_type(elem) == JS_NUM) {
        buffer->data[i] = (uint8_t)js_getnum(elem);
      }
    }
    
    jsval_t obj = create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Buffer");
    js_set(js, obj, "toString", js_mkfun(js_buffer_toString));
    js_set(js, obj, "toBase64", js_mkfun(js_buffer_toBase64));
    js_set(js, obj, "write", js_mkfun(js_buffer_write));
    return obj;
  }
  
  return js_mkerr(js, "Invalid argument to Buffer.from");
}

// Buffer.alloc(size)
static jsval_t js_buffer_alloc(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Buffer.alloc requires a size argument");
  }
  
  size_t size = (size_t)js_getnum(args[0]);
  ArrayBufferData *buffer = create_array_buffer_data(size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  memset(buffer->data, 0, size);
  
  jsval_t obj = create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, "Buffer");
  js_set(js, obj, "toString", js_mkfun(js_buffer_toString));
  js_set(js, obj, "toBase64", js_mkfun(js_buffer_toBase64));
  js_set(js, obj, "write", js_mkfun(js_buffer_write));
  
  return obj;
}

// Buffer.allocUnsafe(size)
static jsval_t js_buffer_allocUnsafe(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Buffer.allocUnsafe requires a size argument");
  }
  
  size_t size = (size_t)js_getnum(args[0]);
  ArrayBufferData *buffer = create_array_buffer_data(size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  jsval_t obj = create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, "Buffer");
  js_set(js, obj, "toString", js_mkfun(js_buffer_toString));
  js_set(js, obj, "toBase64", js_mkfun(js_buffer_toBase64));
  js_set(js, obj, "write", js_mkfun(js_buffer_write));
  
  return obj;
}

// Buffer.prototype.toString(encoding)
static jsval_t js_buffer_toString(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get(js, this_val, "_typedarray_data");
  
  if (js_type(ta_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a Buffer");
  }
  
  TypedArrayData *ta_data = (TypedArrayData *)(uintptr_t)js_getnum(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  char *encoding = "utf8";
  if (nargs > 0 && js_type(args[0]) == JS_STR) {
    encoding = js_getstr(js, args[0], NULL);
  }
  
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  size_t len = ta_data->byte_length;
  
  if (strcmp(encoding, "base64") == 0) {
    size_t out_len;
    char *encoded = base64_encode(data, len, &out_len);
    if (!encoded) return js_mkerr(js, "Failed to encode base64");
    
    jsval_t result = js_mkstr(js, encoded, out_len);
    free(encoded);
    return result;
  } else if (strcmp(encoding, "hex") == 0) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return js_mkerr(js, "Failed to allocate hex string");
    
    for (size_t i = 0; i < len; i++) {
      snprintf(hex + i * 2, 3, "%02x", data[i]);
    }
    
    jsval_t result = js_mkstr(js, hex, len * 2);
    free(hex);
    return result;
  } else return js_mkstr(js, data, len);
}

// Buffer.prototype.toBase64()
static jsval_t js_buffer_toBase64(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t encoding_arg = js_mkstr(js, "base64", 6);
  jsval_t new_args[1] = {encoding_arg};
  return js_buffer_toString(js, new_args, 1);
}

// Buffer.prototype.write(string, offset, length, encoding)
static jsval_t js_buffer_write(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "write requires a string");
  
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get(js, this_val, "_typedarray_data");
  
  if (js_type(ta_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a Buffer");
  }
  
  TypedArrayData *ta_data = (TypedArrayData *)(uintptr_t)js_getnum(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  size_t str_len;
  char *str = js_getstr(js, args[0], &str_len);
  size_t offset = 0;
  size_t length = ta_data->byte_length;
  
  if (nargs > 1 && js_type(args[1]) == JS_NUM) {
    offset = (size_t)js_getnum(args[1]);
  }
  
  if (nargs > 2 && js_type(args[2]) == JS_NUM) {
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

static jsval_t js_sharedarraybuffer_constructor(struct js *js, jsval_t *args, int nargs) {
  size_t length = 0;
  if (nargs > 0 && js_type(args[0]) == JS_NUM) {
    length = (size_t)js_getnum(args[0]);
  }
  
  ArrayBufferData *data = create_shared_array_buffer_data(length);
  if (!data) {
    return js_mkerr(js, "Failed to allocate SharedArrayBuffer");
  }
  
  jsval_t obj = js_mkobj(js);
  js_set(js, obj, "_arraybuffer_data", js_mknum((double)(uintptr_t)data));
  js_set(js, obj, "_shared", js_mktrue());
  js_set(js, obj, "byteLength", js_mknum((double)length));
  js_set(js, obj, "slice", js_mkfun(js_arraybuffer_slice));
  
  return obj;
}

void init_buffer_module() {
  struct js *js = rt->js;
  jsval_t glob = js_glob(js);
  
  jsval_t arraybuffer_ctor_obj = js_mkobj(js);
  jsval_t arraybuffer_proto = js_mkobj(js);
  js_set(js, arraybuffer_proto, "slice", js_mkfun(js_arraybuffer_slice));
  js_setprop(js, arraybuffer_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(js_arraybuffer_constructor));
  js_setprop(js, arraybuffer_ctor_obj, js_mkstr(js, "prototype", 9), arraybuffer_proto);
  js_set(js, glob, "ArrayBuffer", js_obj_to_func(arraybuffer_ctor_obj));
  
  #define SETUP_TYPEDARRAY(name) \
    do { \
      jsval_t name##_ctor_obj = js_mkobj(js); \
      jsval_t name##_proto = js_mkobj(js); \
      js_set(js, name##_proto, "slice", js_mkfun(js_typedarray_slice)); \
      js_set(js, name##_proto, "subarray", js_mkfun(js_typedarray_subarray)); \
      js_setprop(js, name##_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(js_##name##_constructor)); \
      js_setprop(js, name##_ctor_obj, js_mkstr(js, "prototype", 9), name##_proto); \
      js_set(js, glob, #name, js_obj_to_func(name##_ctor_obj)); \
    } while(0)
  
  SETUP_TYPEDARRAY(Int8Array);
  SETUP_TYPEDARRAY(Uint8Array);
  SETUP_TYPEDARRAY(Uint8ClampedArray);
  SETUP_TYPEDARRAY(Int16Array);
  SETUP_TYPEDARRAY(Uint16Array);
  SETUP_TYPEDARRAY(Int32Array);
  SETUP_TYPEDARRAY(Uint32Array);
  SETUP_TYPEDARRAY(Float32Array);
  SETUP_TYPEDARRAY(Float64Array);
  SETUP_TYPEDARRAY(BigInt64Array);
  SETUP_TYPEDARRAY(BigUint64Array);
  
  jsval_t dataview_constructor = js_mkfun(js_dataview_constructor);
  jsval_t dataview_proto = js_mkobj(js);
  js_set(js, dataview_proto, "getUint8", js_mkfun(js_dataview_getUint8));
  js_set(js, dataview_proto, "setUint8", js_mkfun(js_dataview_setUint8));
  js_set(js, dataview_proto, "getInt16", js_mkfun(js_dataview_getInt16));
  js_set(js, dataview_proto, "getInt32", js_mkfun(js_dataview_getInt32));
  js_set(js, dataview_proto, "getFloat32", js_mkfun(js_dataview_getFloat32));
  js_set(js, dataview_constructor, "prototype", dataview_proto);
  js_set(js, glob, "DataView", dataview_constructor);
  
  jsval_t sharedarraybuffer_constructor = js_mkfun(js_sharedarraybuffer_constructor);
  jsval_t sharedarraybuffer_proto = js_mkobj(js);
  js_set(js, sharedarraybuffer_proto, "slice", js_mkfun(js_arraybuffer_slice));
  js_set(js, sharedarraybuffer_constructor, "prototype", sharedarraybuffer_proto);
  js_set(js, glob, "SharedArrayBuffer", sharedarraybuffer_constructor);
  
  jsval_t buffer_obj = js_mkobj(js);
  js_set(js, buffer_obj, "from", js_mkfun(js_buffer_from));
  js_set(js, buffer_obj, "alloc", js_mkfun(js_buffer_alloc));
  js_set(js, buffer_obj, "allocUnsafe", js_mkfun(js_buffer_allocUnsafe));
  js_set(js, buffer_obj, get_toStringTag_sym_key(), js_mkstr(js, "Buffer", 6));
  js_set(js, glob, "Buffer", buffer_obj);
}
