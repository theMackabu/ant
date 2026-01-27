#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "ant.h"
#include "errors.h"
#include "arena.h"
#include "runtime.h"

#include "modules/buffer.h"
#include "modules/symbol.h"

#define TA_ARENA_SIZE (16 * 1024 * 1024)
static uint8_t *ta_arena = NULL;
static size_t ta_arena_offset = 0;

static inline ssize_t normalize_index(ssize_t idx, ssize_t len) {
  if (idx < 0) idx += len;
  if (idx < 0) return 0;
  if (idx > len) return len;
  return idx;
}

static void *ta_arena_alloc(size_t size) {
  size = (size + 7) & ~7;
  
  if (!ta_arena) {
#ifdef _WIN32
    ta_arena = VirtualAlloc(NULL, TA_ARENA_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ta_arena) return NULL;
#else
    void *hint = (void *)0x100000;
    ta_arena = mmap(
      hint, TA_ARENA_SIZE, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (ta_arena == MAP_FAILED) {
      ta_arena = mmap(
        NULL, TA_ARENA_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (ta_arena == MAP_FAILED) return NULL;
#endif
  }
  
  if (ta_arena_offset + size > TA_ARENA_SIZE) return NULL;
  void *ptr = ta_arena + ta_arena_offset;
  ta_arena_offset += size;
  
  return ptr;
}

static jsval_t js_arraybuffer_slice(struct js *js, jsval_t *args, int nargs);
static jsval_t js_typedarray_slice(struct js *js, jsval_t *args, int nargs);
static jsval_t js_typedarray_subarray(struct js *js, jsval_t *args, int nargs);
static jsval_t js_typedarray_fill(struct js *js, jsval_t *args, int nargs);
static jsval_t js_typedarray_toReversed(struct js *js, jsval_t *args, int nargs);
static jsval_t js_typedarray_toSorted(struct js *js, jsval_t *args, int nargs);
static jsval_t js_typedarray_with(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getUint8(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_setUint8(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getInt16(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_setInt16(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getInt32(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_setInt32(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getFloat32(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_setFloat32(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_getFloat64(struct js *js, jsval_t *args, int nargs);
static jsval_t js_dataview_setFloat64(struct js *js, jsval_t *args, int nargs);
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
  ArrayBufferData *data = ant_calloc(sizeof(ArrayBufferData) + length);
  if (!data) return NULL;
  
  data->data = (uint8_t *)(data + 1);
  memset(data->data, 0, length);
  
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
  if (data->ref_count <= 0) free(data);
}

static size_t get_element_size(TypedArrayType type) {
  static const void *dispatch[] = {
    &&L_1, &&L_1, &&L_1, &&L_2, &&L_2,
    &&L_4, &&L_4, &&L_4, &&L_8, &&L_8, &&L_8
  };
  
  if (type > TYPED_ARRAY_BIGUINT64) goto L_1;
  goto *dispatch[type];
  
  L_1: return 1;
  L_2: return 2;
  L_4: return 4;
  L_8: return 8;
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
  jsval_t proto = js_get_ctor_proto(js, "ArrayBuffer", 11);
  
  if (js_type(proto) == JS_OBJ) js_set_proto(js, obj, proto);
  js_set_slot(js, obj, SLOT_BUFFER, ANT_PTR(data));
  js_set(js, obj, "byteLength", js_mknum((double)length));
  
  return obj;
}

// ArrayBuffer.prototype.slice(begin, end)
static jsval_t js_arraybuffer_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  if (js_type(data_val) != JS_NUM) {
    return js_mkerr(js, "Not an ArrayBuffer");
  }
  
  ArrayBufferData *data = (ArrayBufferData *)(uintptr_t)js_getnum(data_val);
  if (!data) return js_mkerr(js, "Invalid ArrayBuffer");
  
  ssize_t len = (ssize_t)data->length;
  ssize_t begin = 0, end = len;
  if (nargs > 0 && js_type(args[0]) == JS_NUM) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && js_type(args[1]) == JS_NUM) end = (ssize_t)js_getnum(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  ArrayBufferData *new_data = create_array_buffer_data(new_length);
  if (!new_data) return js_mkerr(js, "Failed to allocate new ArrayBuffer");
  
  memcpy(new_data->data, data->data + begin, new_length);
  
  jsval_t new_obj = js_mkobj(js);
  jsval_t proto = js_get_ctor_proto(js, "ArrayBuffer", 11);
  
  if (js_type(proto) == JS_OBJ) js_set_proto(js, new_obj, proto);
  js_set_slot(js, new_obj, SLOT_BUFFER, ANT_PTR(new_data));
  js_set(js, new_obj, "byteLength", js_mknum((double)new_length));
  
  return new_obj;
}

static jsval_t typedarray_index_getter(struct js *js, jsval_t obj, const char *key, size_t key_len) {
  if (key_len == 0 || key_len > 10) return js_mkundef();
  
  size_t index = 0;
  for (size_t i = 0; i < key_len; i++) {
    char c = key[i];
    if (c < '0' || c > '9') return js_mkundef();
    index = index * 10 + (c - '0');
  }
  
  jsval_t ta_val = js_get_slot(js, obj, SLOT_BUFFER);
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_val);
  if (!ta_data || index >= ta_data->length) return js_mkundef();
  
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

static bool typedarray_index_setter(struct js *js, jsval_t obj, const char *key, size_t key_len, jsval_t value) {
  if (key_len == 0 || key_len > 10) return false;
  
  size_t index = 0;
  for (size_t i = 0; i < key_len; i++) {
    char c = key[i];
    if (c < '0' || c > '9') return false;
    index = index * 10 + (c - '0');
  }
  
  jsval_t ta_val = js_get_slot(js, obj, SLOT_BUFFER);
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_val);
  if (!ta_data || index >= ta_data->length) return false;
  
  double num_val = js_type(value) == JS_NUM ? js_getnum(value) : 0;
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  
  static const void *dispatch[] = {
    &&S_INT8, &&S_UINT8, &&S_UINT8, &&S_INT16, &&S_UINT16,
    &&S_INT32, &&S_UINT32, &&S_FLOAT32, &&S_FLOAT64, &&S_FAIL, &&S_FAIL
  };
  
  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto S_FAIL;
  goto *dispatch[ta_data->type];
  
  S_INT8:    ((int8_t*)data)[index] = (int8_t)num_val;   goto S_DONE;
  S_UINT8:   data[index] = (uint8_t)num_val;             goto S_DONE;
  S_INT16:   ((int16_t*)data)[index] = (int16_t)num_val; goto S_DONE;
  S_UINT16:  ((uint16_t*)data)[index] = (uint16_t)num_val; goto S_DONE;
  S_INT32:   ((int32_t*)data)[index] = (int32_t)num_val; goto S_DONE;
  S_UINT32:  ((uint32_t*)data)[index] = (uint32_t)num_val; goto S_DONE;
  S_FLOAT32: ((float*)data)[index] = (float)num_val;     goto S_DONE;
  S_FLOAT64: ((double*)data)[index] = num_val;           goto S_DONE;
  S_FAIL:    return false;
  S_DONE:    return true;
}

static jsval_t create_arraybuffer_obj(struct js *js, ArrayBufferData *buffer) {
  jsval_t ab_obj = js_mkobj(js);
  jsval_t ab_proto = js_get_ctor_proto(js, "ArrayBuffer", 11);
  if (js_type(ab_proto) == JS_OBJ) js_set_proto(js, ab_obj, ab_proto);
  
  js_set_slot(js, ab_obj, SLOT_BUFFER, js_mknum((double)(uintptr_t)buffer));
  js_set(js, ab_obj, "byteLength", js_mknum((double)buffer->length));
  buffer->ref_count++;
  
  return ab_obj;
}

static jsval_t create_typed_array_with_buffer(
  struct js *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, const char *type_name, jsval_t arraybuffer_obj
) {
  TypedArrayData *ta_data = ta_arena_alloc(sizeof(TypedArrayData));
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
  
  js_set_slot(js, obj, SLOT_BUFFER, js_mktypedarray(ta_data));
  js_set(js, obj, "length", js_mknum((double)length));
  js_set(js, obj, "byteLength", js_mknum((double)(length * element_size)));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset));
  js_set(js, obj, "BYTES_PER_ELEMENT", js_mknum((double)element_size));
  js_set(js, obj, "buffer", arraybuffer_obj);
  
  js_set_getter(js, obj, typedarray_index_getter);
  js_set_setter(js, obj, typedarray_index_setter);
  
  return obj;
}

static jsval_t create_typed_array(
  struct js *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, const char *type_name
) {
  jsval_t ab_obj = create_arraybuffer_obj(js, buffer);
  return create_typed_array_with_buffer(js, type, buffer, byte_offset, length, type_name, ab_obj);
}

typedef struct {
  jsval_t *values;
  size_t length;
  size_t capacity;
} iter_collect_ctx_t;

static bool iter_collect_callback(struct js *js, jsval_t value, void *udata) {
  (void)js;
  iter_collect_ctx_t *ctx = (iter_collect_ctx_t *)udata;
  if (ctx->length >= ctx->capacity) {
    ctx->capacity *= 2;
    jsval_t *new_values = realloc(ctx->values, ctx->capacity * sizeof(jsval_t));
    if (!new_values) return false;
    ctx->values = new_values;
  }
  ctx->values[ctx->length++] = value;
  return true;
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
  
  jsval_t buffer_data_val = js_get_slot(js, args[0], SLOT_BUFFER);
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
    
    return create_typed_array_with_buffer(js, type, buffer, byte_offset, length, type_name, args[0]);
  }
  
  int arg_type = js_type(args[0]);
  if (arg_type == JS_OBJ) {
    jsval_t len_val = js_get(js, args[0], "length");
    size_t length = 0; jsval_t *values = NULL;
    bool is_iterable = false;
    
    if (js_type(len_val) == JS_NUM) length = (size_t)js_getnum(len_val); else {
      iter_collect_ctx_t ctx = { .values = NULL, .length = 0, .capacity = 16 };
      ctx.values = malloc(ctx.capacity * sizeof(jsval_t));
      if (!ctx.values) return js_mkerr(js, "Failed to allocate memory");
      is_iterable = js_iter(js, args[0], iter_collect_callback, &ctx);
      
      if (is_iterable) {
        values = ctx.values;
        length = ctx.length;
      } else free(ctx.values);
    }
    
    if (length > 0 || is_iterable || js_type(len_val) == JS_NUM) {
      size_t element_size = get_element_size(type);
      ArrayBufferData *buffer = create_array_buffer_data(length * element_size);
      if (!buffer) { if (values) free(values); return js_mkerr(js, "Failed to allocate buffer"); }
      
      jsval_t result = create_typed_array(js, type, buffer, 0, length, type_name);
      uint8_t *data = buffer->data;
      
      static const void *write_dispatch[] = {
        &&W_INT8, &&W_UINT8, &&W_UINT8, &&W_INT16, &&W_UINT16,
        &&W_INT32, &&W_UINT32, &&W_FLOAT32, &&W_FLOAT64, &&W_DONE, &&W_DONE
      };
      
      for (size_t i = 0; i < length; i++) {
        jsval_t elem;
        if (values) elem = values[i]; else {
          char idx_str[16];
          snprintf(idx_str, sizeof(idx_str), "%zu", i);
          elem = js_get(js, args[0], idx_str);
        }
        double val = js_type(elem) == JS_NUM ? js_getnum(elem) : 0;
        
        if (type > TYPED_ARRAY_BIGUINT64) goto W_DONE;
        goto *write_dispatch[type];
        
        W_INT8:    ((int8_t*)data)[i] = (int8_t)val;   goto W_NEXT;
        W_UINT8:   data[i] = (uint8_t)val;             goto W_NEXT;
        W_INT16:   ((int16_t*)data)[i] = (int16_t)val; goto W_NEXT;
        W_UINT16:  ((uint16_t*)data)[i] = (uint16_t)val; goto W_NEXT;
        W_INT32:   ((int32_t*)data)[i] = (int32_t)val; goto W_NEXT;
        W_UINT32:  ((uint32_t*)data)[i] = (uint32_t)val; goto W_NEXT;
        W_FLOAT32: ((float*)data)[i] = (float)val;    goto W_NEXT;
        W_FLOAT64: ((double*)data)[i] = val;          goto W_NEXT;
        W_NEXT:;
      }
      W_DONE:
      if (values) free(values);
      return result;
    }
  }
  
  return js_mkerr(js, "Invalid TypedArray constructor arguments");
}

// TypedArray.prototype.slice(begin, end)
static jsval_t js_typedarray_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t begin = 0, end = len;
  
  if (nargs > 0 && js_type(args[0]) == JS_NUM) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && js_type(args[1]) == JS_NUM) end = (ssize_t)js_getnum(args[1]);
  
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
  
  return create_typed_array(js, ta_data->type, new_buffer, 0, new_length, "TypedArray");
}

// TypedArray.prototype.subarray(begin, end)
static jsval_t js_typedarray_subarray(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t begin = 0, end = len;
  
  if (nargs > 0 && js_type(args[0]) == JS_NUM) begin = (ssize_t)js_getnum(args[0]);
  if (nargs > 1 && js_type(args[1]) == JS_NUM) end = (ssize_t)js_getnum(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  size_t element_size = get_element_size(ta_data->type);
  size_t new_offset = ta_data->byte_offset + (size_t)begin * element_size;
  
  return create_typed_array(js, ta_data->type, ta_data->buffer, new_offset, new_length, "TypedArray");
}

// TypedArray.prototype.fill(value, start, end)
static jsval_t js_typedarray_fill(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  double value = 0;
  if (nargs > 0 && js_type(args[0]) == JS_NUM) value = js_getnum(args[0]);
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t start = 0, end = len;
  
  if (nargs > 1 && js_type(args[1]) == JS_NUM) start = (ssize_t)js_getnum(args[1]);
  if (nargs > 2 && js_type(args[2]) == JS_NUM) end = (ssize_t)js_getnum(args[2]);
  
  start = normalize_index(start, len);
  end = normalize_index(end, len);
  if (end < start) end = start;
  
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  
  static const void *dispatch[] = {
    &&L_INT8, &&L_UINT8, &&L_UINT8, &&L_INT16, &&L_UINT16,
    &&L_INT32, &&L_UINT32, &&L_FLOAT32, &&L_FLOAT64, &&L_DONE, &&L_DONE
  };
  
  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto L_DONE;
  goto *dispatch[ta_data->type];
  
  L_INT8:
    for (ssize_t i = start; i < end; i++) ((int8_t*)data)[i] = (int8_t)value;
    goto L_DONE;
  L_UINT8:
    for (ssize_t i = start; i < end; i++) data[i] = (uint8_t)value;
    goto L_DONE;
  L_INT16:
    for (ssize_t i = start; i < end; i++) ((int16_t*)data)[i] = (int16_t)value;
    goto L_DONE;
  L_UINT16:
    for (ssize_t i = start; i < end; i++) ((uint16_t*)data)[i] = (uint16_t)value;
    goto L_DONE;
  L_INT32:
    for (ssize_t i = start; i < end; i++) ((int32_t*)data)[i] = (int32_t)value;
    goto L_DONE;
  L_UINT32:
    for (ssize_t i = start; i < end; i++) ((uint32_t*)data)[i] = (uint32_t)value;
    goto L_DONE;
  L_FLOAT32:
    for (ssize_t i = start; i < end; i++) ((float*)data)[i] = (float)value;
    goto L_DONE;
  L_FLOAT64:
    for (ssize_t i = start; i < end; i++) ((double*)data)[i] = value;
    goto L_DONE;
  L_DONE:
    return this_val;
}

// TypedArray.prototype.toReversed()
static jsval_t js_typedarray_toReversed(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
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
  
  return create_typed_array(js, ta_data->type, new_buffer, 0, length, "TypedArray");
}

// TypedArray.prototype.toSorted(comparefn)
static jsval_t js_typedarray_toSorted(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  size_t length = ta_data->length;
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(new_buffer->data, ta_data->buffer->data + ta_data->byte_offset, length * element_size);
  
  jsval_t result = create_typed_array(js, ta_data->type, new_buffer, 0, length, "TypedArray");
  jsval_t result_ta_val = js_get_slot(js, result, SLOT_BUFFER);
  TypedArrayData *result_ta = (TypedArrayData *)js_gettypedarray(result_ta_val);
  uint8_t *data = result_ta->buffer->data;
  
  jsval_t comparefn = (nargs > 0 && js_type(args[0]) == JS_FUNC) ? args[0] : js_mkundef();
  bool has_comparefn = js_type(comparefn) == JS_FUNC;
  
  for (size_t i = 1; i < length; i++) {
    for (size_t j = i; j > 0; j--) {
      double a_val, b_val;
      int cmp;
      
      static const void *read_dispatch[] = {
        &&R_INT8, &&R_UINT8, &&R_UINT8, &&R_INT16, &&R_UINT16,
        &&R_INT32, &&R_UINT32, &&R_FLOAT32, &&R_FLOAT64, &&R_DONE, &&R_DONE
      };
      
      if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto R_DONE;
      goto *read_dispatch[ta_data->type];
      
      R_INT8:    a_val = (double)((int8_t*)data)[j-1]; b_val = (double)((int8_t*)data)[j]; goto R_COMPARE;
      R_UINT8:   a_val = (double)data[j-1]; b_val = (double)data[j]; goto R_COMPARE;
      R_INT16:   a_val = (double)((int16_t*)data)[j-1]; b_val = (double)((int16_t*)data)[j]; goto R_COMPARE;
      R_UINT16:  a_val = (double)((uint16_t*)data)[j-1]; b_val = (double)((uint16_t*)data)[j]; goto R_COMPARE;
      R_INT32:   a_val = (double)((int32_t*)data)[j-1]; b_val = (double)((int32_t*)data)[j]; goto R_COMPARE;
      R_UINT32:  a_val = (double)((uint32_t*)data)[j-1]; b_val = (double)((uint32_t*)data)[j]; goto R_COMPARE;
      R_FLOAT32: a_val = (double)((float*)data)[j-1]; b_val = (double)((float*)data)[j]; goto R_COMPARE;
      R_FLOAT64: a_val = ((double*)data)[j-1]; b_val = ((double*)data)[j]; goto R_COMPARE;
      R_DONE:    goto SORT_DONE;
      
      R_COMPARE:
      if (has_comparefn) {
        jsval_t cmp_args[2] = { js_mknum(a_val), js_mknum(b_val) };
        jsval_t cmp_result = js_call(js, comparefn, cmp_args, 2);
        cmp = (int)js_getnum(cmp_result);
      } else {
        cmp = (a_val > b_val) ? 1 : ((a_val < b_val) ? -1 : 0);
      }
      
      if (cmp <= 0) break;
      
      static const void *swap_dispatch[] = {
        &&S_INT8, &&S_UINT8, &&S_UINT8, &&S_INT16, &&S_UINT16,
        &&S_INT32, &&S_UINT32, &&S_FLOAT32, &&S_FLOAT64, &&S_DONE, &&S_DONE
      };
      
      if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto S_DONE;
      goto *swap_dispatch[ta_data->type];
      
      S_INT8:    { int8_t tmp = ((int8_t*)data)[j-1]; ((int8_t*)data)[j-1] = ((int8_t*)data)[j]; ((int8_t*)data)[j] = tmp; goto S_DONE; }
      S_UINT8:   { uint8_t tmp = data[j-1]; data[j-1] = data[j]; data[j] = tmp; goto S_DONE; }
      S_INT16:   { int16_t tmp = ((int16_t*)data)[j-1]; ((int16_t*)data)[j-1] = ((int16_t*)data)[j]; ((int16_t*)data)[j] = tmp; goto S_DONE; }
      S_UINT16:  { uint16_t tmp = ((uint16_t*)data)[j-1]; ((uint16_t*)data)[j-1] = ((uint16_t*)data)[j]; ((uint16_t*)data)[j] = tmp; goto S_DONE; }
      S_INT32:   { int32_t tmp = ((int32_t*)data)[j-1]; ((int32_t*)data)[j-1] = ((int32_t*)data)[j]; ((int32_t*)data)[j] = tmp; goto S_DONE; }
      S_UINT32:  { uint32_t tmp = ((uint32_t*)data)[j-1]; ((uint32_t*)data)[j-1] = ((uint32_t*)data)[j]; ((uint32_t*)data)[j] = tmp; goto S_DONE; }
      S_FLOAT32: { float tmp = ((float*)data)[j-1]; ((float*)data)[j-1] = ((float*)data)[j]; ((float*)data)[j] = tmp; goto S_DONE; }
      S_FLOAT64: { double tmp = ((double*)data)[j-1]; ((double*)data)[j-1] = ((double*)data)[j]; ((double*)data)[j] = tmp; goto S_DONE; }
      S_DONE:;
    }
  }
  
  SORT_DONE:
  return result;
}

// TypedArray.prototype.with(index, value)
static jsval_t js_typedarray_with(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "with requires index and value");
  
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  ssize_t index = (ssize_t)js_getnum(args[0]);
  double value = js_getnum(args[1]);
  size_t length = ta_data->length;
  
  if (index < 0) index = (ssize_t)length + index;
  if (index < 0 || (size_t)index >= length) {
    return js_mkerr(js, "Index out of bounds");
  }
  
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(new_buffer->data, ta_data->buffer->data + ta_data->byte_offset, length * element_size);
  
  uint8_t *data = new_buffer->data;
  
  static const void *dispatch[] = {
    &&W_INT8, &&W_UINT8, &&W_UINT8, &&W_INT16, &&W_UINT16,
    &&W_INT32, &&W_UINT32, &&W_FLOAT32, &&W_FLOAT64, &&W_DONE, &&W_DONE
  };
  
  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto W_DONE;
  goto *dispatch[ta_data->type];
  
  W_INT8:    ((int8_t*)data)[index] = (int8_t)value; goto W_DONE;
  W_UINT8:   data[index] = (uint8_t)value; goto W_DONE;
  W_INT16:   ((int16_t*)data)[index] = (int16_t)value; goto W_DONE;
  W_UINT16:  ((uint16_t*)data)[index] = (uint16_t)value; goto W_DONE;
  W_INT32:   ((int32_t*)data)[index] = (int32_t)value; goto W_DONE;
  W_UINT32:  ((uint32_t*)data)[index] = (uint32_t)value; goto W_DONE;
  W_FLOAT32: ((float*)data)[index] = (float)value; goto W_DONE;
  W_FLOAT64: ((double*)data)[index] = value; goto W_DONE;
  W_DONE:
  
  return create_typed_array(js, ta_data->type, new_buffer, 0, length, "TypedArray");
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
  
  jsval_t buffer_data_val = js_get_slot(js, args[0], SLOT_BUFFER);
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
  
  DataViewData *dv_data = ant_calloc(sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");
  
  dv_data->buffer = buffer;
  dv_data->byte_offset = byte_offset;
  dv_data->byte_length = byte_length;
  buffer->ref_count++;
  
  jsval_t obj = js_mkobj(js);
  jsval_t proto = js_get_ctor_proto(js, "DataView", 8);
  if (js_type(proto) == JS_OBJ) js_set_proto(js, obj, proto);
  
  js_set_slot(js, obj, SLOT_DATA, ANT_PTR(dv_data));
  js_mkprop_fast(js, obj, "buffer", 6, args[0]);
  js_set_descriptor(js, obj, "buffer", 6, 0);

  js_set(js, obj, "byteLength", js_mknum((double)byte_length));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset)); 
  
  return obj;
}

// DataView.prototype.getUint8(byteOffset)
static jsval_t js_dataview_getUint8(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getUint8 requires byteOffset");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
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
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
  size_t offset = (size_t)js_getnum(args[0]);
  uint8_t value = (uint8_t)js_to_uint32(js_getnum(args[1]));
  
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
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
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
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
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
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
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

// DataView.prototype.setInt16(byteOffset, value, littleEndian)
static jsval_t js_dataview_setInt16(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setInt16 requires byteOffset and value");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
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
static jsval_t js_dataview_setInt32(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setInt32 requires byteOffset and value");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
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

// DataView.prototype.setFloat32(byteOffset, value, littleEndian)
static jsval_t js_dataview_setFloat32(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setFloat32 requires byteOffset and value");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
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
static jsval_t js_dataview_getFloat64(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getFloat64 requires byteOffset");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
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
static jsval_t js_dataview_setFloat64(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setFloat64 requires byteOffset and value");
  
  jsval_t this_val = js_getthis(js);
  jsval_t dv_data_val = js_get_slot(js, this_val, SLOT_DATA);
  
  if (js_type(dv_data_val) != JS_NUM) {
    return js_mkerr(js, "Not a DataView");
  }
  
  DataViewData *dv = (DataViewData *)(uintptr_t)js_getnum(dv_data_val);
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

static uint8_t *hex_decode(const char *data, size_t len, size_t *out_len) {
  if (len % 2 != 0) return NULL;
  
  size_t decoded_len = len / 2;
  uint8_t *decoded = malloc(decoded_len);
  if (!decoded) return NULL;
  
  for (size_t i = 0; i < decoded_len; i++) {
    unsigned int byte;
    if (sscanf(data + i * 2, "%2x", &byte) != 1) {
      free(decoded); return NULL;
    }
    decoded[i] = (uint8_t)byte;
  }
  
  *out_len = decoded_len;
  return decoded;
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
static jsval_t js_buffer_from(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Buffer.from requires at least one argument");
  
  if (js_type(args[0]) == JS_STR) {
    size_t len;
    char *str = js_getstr(js, args[0], &len);
    
    BufferEncoding encoding = ENC_UTF8;
    if (nargs >= 2 && js_type(args[1]) == JS_STR) {
      size_t enc_len;
      char *enc_str = js_getstr(js, args[1], &enc_len);
      encoding = parse_encoding(enc_str, enc_len);
      if (encoding == ENC_UNKNOWN) encoding = ENC_UTF8;
    }
    
    if (encoding == ENC_BASE64) {
      size_t decoded_len;
      uint8_t *decoded = base64_decode(str, len, &decoded_len);
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
      size_t decoded_len = len * 2;
      ArrayBufferData *buffer = create_array_buffer_data(decoded_len);
      if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
      
      for (size_t i = 0; i < len; i++) {
        buffer->data[i * 2] = (uint8_t)str[i];
        buffer->data[i * 2 + 1] = 0;
      }
      return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, decoded_len, "Buffer");
    } else {
      ArrayBufferData *buffer = create_array_buffer_data(len);
      if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
      
      memcpy(buffer->data, str, len);
      return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Buffer");
    }
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
    
    return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Buffer");
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
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, "Buffer");
}

// Buffer.allocUnsafe(size)
static jsval_t js_buffer_allocUnsafe(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Buffer.allocUnsafe requires a size argument");
  }
  
  size_t size = (size_t)js_getnum(args[0]);
  ArrayBufferData *buffer = create_array_buffer_data(size);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, "Buffer");
}

// Buffer.prototype.toString(encoding)
static jsval_t js_buffer_toString(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  BufferEncoding encoding = ENC_UTF8;
  if (nargs > 0 && js_type(args[0]) == JS_STR) {
    size_t enc_len;
    char *enc_str = js_getstr(js, args[0], &enc_len);
    encoding = parse_encoding(enc_str, enc_len);
    if (encoding == ENC_UNKNOWN) encoding = ENC_UTF8;
  }
  
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  size_t len = ta_data->byte_length;
  
  if (encoding == ENC_BASE64) {
    size_t out_len;
    char *encoded = base64_encode(data, len, &out_len);
    if (!encoded) return js_mkerr(js, "Failed to encode base64");
    
    jsval_t result = js_mkstr(js, encoded, out_len);
    free(encoded);
    return result;
  } else if (encoding == ENC_HEX) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return js_mkerr(js, "Failed to allocate hex string");
    
    for (size_t i = 0; i < len; i++) {
      snprintf(hex + i * 2, 3, "%02x", data[i]);
    }
    
    jsval_t result = js_mkstr(js, hex, len * 2);
    free(hex);
    return result;
  } else if (encoding == ENC_UCS2) {
    size_t char_count = len / 2;
    char *str = malloc(char_count + 1);
    if (!str) return js_mkerr(js, "Failed to allocate string");
    
    for (size_t i = 0; i < char_count; i++) str[i] = (char)data[i * 2];
    str[char_count] = '\0';
    
    jsval_t result = js_mkstr(js, str, char_count);
    free(str);
    return result;
  } else return js_mkstr(js, (char *)data, len);
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
  jsval_t ta_data_val = js_get_slot(js, this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
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

// Buffer.isBuffer(obj)
static jsval_t js_buffer_isBuffer(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkfalse();
  if (js_type(args[0]) != JS_OBJ) return js_mkfalse();
  
  jsval_t proto = js_get_proto(js, args[0]);
  jsval_t buffer_proto = js_get_ctor_proto(js, "Buffer", 6);
  
  return (proto == buffer_proto) ? js_mktrue() : js_mkfalse();
}

// Buffer.isEncoding(encoding)
static jsval_t js_buffer_isEncoding(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || js_type(args[0]) != JS_STR) return js_mkfalse();
  
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
    return js_mktrue();
  }
  
  return js_mkfalse();
}

// Buffer.byteLength(string, encoding)
static jsval_t js_buffer_byteLength(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mknum(0);
  
  jsval_t arg = args[0];
  
  if (js_type(arg) == JS_OBJ) {
    jsval_t bytelen = js_get(js, arg, "byteLength");
    if (js_type(bytelen) == JS_NUM) return bytelen;
    
    jsval_t len = js_get(js, arg, "length");
    if (js_type(len) == JS_NUM) return len;
  }
  
  if (js_type(arg) == JS_STR) {
    size_t len;
    js_getstr(js, arg, &len);
    return js_mknum((double)len);
  }
  
  return js_mknum(0);
}

// Buffer.concat(list, totalLength)
static jsval_t js_buffer_concat(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || js_type(args[0]) != JS_OBJ) {
    return js_mkerr(js, "First argument must be an array");
  }
  
  jsval_t list = args[0];
  jsval_t len_val = js_get(js, list, "length");
  if (js_type(len_val) != JS_NUM) {
    return js_mkerr(js, "First argument must be an array");
  }
  
  size_t list_len = (size_t)js_getnum(len_val);
  size_t total_length = 0;
  
  if (nargs > 1 && js_type(args[1]) == JS_NUM) {
    total_length = (size_t)js_getnum(args[1]);
  } else {
    for (size_t i = 0; i < list_len; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%zu", i);
      jsval_t buf = js_get(js, list, idx);
      jsval_t buf_len = js_get(js, buf, "length");
      if (js_type(buf_len) == JS_NUM) total_length += (size_t)js_getnum(buf_len);
    }
  }
  
  ArrayBufferData *buffer = create_array_buffer_data(total_length);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  size_t offset = 0;
  for (size_t i = 0; i < list_len && offset < total_length; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%zu", i);
    jsval_t buf = js_get(js, list, idx);
    
    jsval_t ta_data_val = js_get_slot(js, buf, SLOT_BUFFER);
    TypedArrayData *ta = js_gettypedarray(ta_data_val);
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
static jsval_t js_buffer_compare(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Buffer.compare requires two arguments");
  
  jsval_t ta1_val = js_get_slot(js, args[0], SLOT_BUFFER);
  jsval_t ta2_val = js_get_slot(js, args[1], SLOT_BUFFER);
  
  TypedArrayData *ta1 = js_gettypedarray(ta1_val);
  TypedArrayData *ta2 = js_gettypedarray(ta2_val);
  
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
  jsval_t proto = js_get_ctor_proto(js, "SharedArrayBuffer", 17);
  
  if (js_type(proto) == JS_OBJ) js_set_proto(js, obj, proto);
  js_set_slot(js, obj, SLOT_BUFFER, ANT_PTR(data));
  js_set(js, obj, "byteLength", js_mknum((double)length));
  
  return obj;
}

void init_buffer_module() {
  struct js *js = rt->js;
  jsval_t glob = js_glob(js);
  
  jsval_t arraybuffer_ctor_obj = js_mkobj(js);
  jsval_t arraybuffer_proto = js_mkobj(js);
  
  js_set(js, arraybuffer_proto, "slice", js_mkfun(js_arraybuffer_slice));
  js_set(js, arraybuffer_proto, get_toStringTag_sym_key(), js_mkstr(js, "ArrayBuffer", 11));
  
  js_set_slot(js, arraybuffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_arraybuffer_constructor));
  js_mkprop_fast(js, arraybuffer_ctor_obj, "prototype", 9, arraybuffer_proto);
  js_mkprop_fast(js, arraybuffer_ctor_obj, "name", 4, ANT_STRING("ArrayBuffer"));
  js_set_descriptor(js, arraybuffer_ctor_obj, "name", 4, 0);
  js_set(js, glob, "ArrayBuffer", js_obj_to_func(arraybuffer_ctor_obj));
  
  jsval_t typedarray_proto = js_mkobj(js);
  js_set(js, typedarray_proto, "slice", js_mkfun(js_typedarray_slice));
  js_set(js, typedarray_proto, "subarray", js_mkfun(js_typedarray_subarray));
  js_set(js, typedarray_proto, "fill", js_mkfun(js_typedarray_fill));
  js_set(js, typedarray_proto, "toReversed", js_mkfun(js_typedarray_toReversed));
  js_set(js, typedarray_proto, "toSorted", js_mkfun(js_typedarray_toSorted));
  js_set(js, typedarray_proto, "with", js_mkfun(js_typedarray_with));
  js_set(js, typedarray_proto, get_toStringTag_sym_key(), js_mkstr(js, "TypedArray", 10));
  
  #define SETUP_TYPEDARRAY(name) \
    do { \
      jsval_t name##_ctor_obj = js_mkobj(js); \
      jsval_t name##_proto = js_mkobj(js); \
      js_set_proto(js, name##_proto, typedarray_proto); \
      js_set_slot(js, name##_ctor_obj, SLOT_CFUNC, js_mkfun(js_##name##_constructor)); \
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
  
  jsval_t dataview_ctor_obj = js_mkobj(js);
  jsval_t dataview_proto = js_mkobj(js);
  
  js_set(js, dataview_proto, "getUint8", js_mkfun(js_dataview_getUint8));
  js_set(js, dataview_proto, "setUint8", js_mkfun(js_dataview_setUint8));
  js_set(js, dataview_proto, "getInt16", js_mkfun(js_dataview_getInt16));
  js_set(js, dataview_proto, "setInt16", js_mkfun(js_dataview_setInt16));
  js_set(js, dataview_proto, "getInt32", js_mkfun(js_dataview_getInt32));
  js_set(js, dataview_proto, "setInt32", js_mkfun(js_dataview_setInt32));
  js_set(js, dataview_proto, "getFloat32", js_mkfun(js_dataview_getFloat32));
  js_set(js, dataview_proto, "setFloat32", js_mkfun(js_dataview_setFloat32));
  js_set(js, dataview_proto, "getFloat64", js_mkfun(js_dataview_getFloat64));
  js_set(js, dataview_proto, "setFloat64", js_mkfun(js_dataview_setFloat64));
  js_set(js, dataview_proto, get_toStringTag_sym_key(), js_mkstr(js, "DataView", 8));

  js_set_slot(js, dataview_ctor_obj, SLOT_CFUNC, js_mkfun(js_dataview_constructor));
  js_mkprop_fast(js, dataview_ctor_obj, "prototype", 9, dataview_proto);
  js_mkprop_fast(js, dataview_ctor_obj, "name", 4, ANT_STRING("DataView"));
  js_set_descriptor(js, dataview_ctor_obj, "name", 4, 0);
  js_set(js, glob, "DataView", js_obj_to_func(dataview_ctor_obj));
  
  jsval_t sharedarraybuffer_ctor_obj = js_mkobj(js);
  jsval_t sharedarraybuffer_proto = js_mkobj(js);
  
  js_set(js, sharedarraybuffer_proto, "slice", js_mkfun(js_arraybuffer_slice));
  js_set(js, sharedarraybuffer_proto, get_toStringTag_sym_key(), js_mkstr(js, "SharedArrayBuffer", 17));
  
  js_set_slot(js, sharedarraybuffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_sharedarraybuffer_constructor));
  js_mkprop_fast(js, sharedarraybuffer_ctor_obj, "prototype", 9, sharedarraybuffer_proto);
  js_mkprop_fast(js, sharedarraybuffer_ctor_obj, "name", 4, ANT_STRING("SharedArrayBuffer"));
  js_set_descriptor(js, sharedarraybuffer_ctor_obj, "name", 4, 0);
  js_set(js, glob, "SharedArrayBuffer", js_obj_to_func(sharedarraybuffer_ctor_obj));
  
  jsval_t buffer_ctor_obj = js_mkobj(js);
  jsval_t buffer_proto = js_mkobj(js);
  
  js_set(js, buffer_proto, "toString", js_mkfun(js_buffer_toString));
  js_set(js, buffer_proto, "toBase64", js_mkfun(js_buffer_toBase64));
  js_set(js, buffer_proto, "write", js_mkfun(js_buffer_write));
  js_set(js, buffer_proto, get_toStringTag_sym_key(), js_mkstr(js, "Buffer", 6));
  
  js_set(js, buffer_ctor_obj, "from", js_mkfun(js_buffer_from));
  js_set(js, buffer_ctor_obj, "alloc", js_mkfun(js_buffer_alloc));
  js_set(js, buffer_ctor_obj, "allocUnsafe", js_mkfun(js_buffer_allocUnsafe));
  js_set(js, buffer_ctor_obj, "isBuffer", js_mkfun(js_buffer_isBuffer));
  js_set(js, buffer_ctor_obj, "isEncoding", js_mkfun(js_buffer_isEncoding));
  js_set(js, buffer_ctor_obj, "byteLength", js_mkfun(js_buffer_byteLength));
  js_set(js, buffer_ctor_obj, "concat", js_mkfun(js_buffer_concat));
  js_set(js, buffer_ctor_obj, "compare", js_mkfun(js_buffer_compare));

  js_set_slot(js, buffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_buffer_from));
  js_mkprop_fast(js, buffer_ctor_obj, "prototype", 9, buffer_proto);
  js_mkprop_fast(js, buffer_ctor_obj, "name", 4, ANT_STRING("Buffer"));
  js_set_descriptor(js, buffer_ctor_obj, "name", 4, 0);
  js_set(js, glob, "Buffer", js_obj_to_func(buffer_ctor_obj));
}
