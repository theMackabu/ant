#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "ant.h"
#include "errors.h"
#include "base64.h"
#include "internal.h"
#include "silver/engine.h"
#include "runtime.h"
#include "descriptors.h"

#include "modules/buffer.h"
#include "modules/symbol.h"

#define TA_ARENA_SIZE (16 * 1024 * 1024)
#define BUFFER_REGISTRY_INITIAL_CAP 64

static uint8_t *ta_arena = NULL;
static size_t ta_arena_offset = 0;

static ArrayBufferData **buffer_registry   = NULL;
static ant_value_t g_typedarray_iter_proto = 0;

static size_t buffer_registry_count = 0;
static size_t buffer_registry_cap   = 0;

static bool advance_typedarray(ant_t *js, js_iter_t *it, ant_value_t *out) {
  ant_value_t iter = it->iterator;
  ant_value_t ta_obj = js_get_slot(iter, SLOT_DATA);
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  
  uint32_t kind = ITER_STATE_KIND(state);
  uint32_t idx  = ITER_STATE_INDEX(state);

  ant_value_t ta_val = js_get_slot(ta_obj, SLOT_BUFFER);
  TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(ta_val);
  if (!ta || !ta->buffer || ta->buffer->is_detached || idx >= (uint32_t)ta->length)
    return false;

  uint8_t *data = ta->buffer->data + ta->byte_offset;
  double value;
  switch (ta->type) {
    case TYPED_ARRAY_INT8:          value = (double)((int8_t *)data)[idx];   break;
    case TYPED_ARRAY_UINT8:
    case TYPED_ARRAY_UINT8_CLAMPED: value = (double)data[idx];              break;
    case TYPED_ARRAY_INT16:         value = (double)((int16_t *)data)[idx];  break;
    case TYPED_ARRAY_UINT16:        value = (double)((uint16_t *)data)[idx]; break;
    case TYPED_ARRAY_INT32:         value = (double)((int32_t *)data)[idx];  break;
    case TYPED_ARRAY_UINT32:        value = (double)((uint32_t *)data)[idx]; break;
    case TYPED_ARRAY_FLOAT32:       value = (double)((float *)data)[idx];    break;
    case TYPED_ARRAY_FLOAT64:       value = ((double *)data)[idx];           break;
    default: return false;
  }

  switch (kind) {
  case ARR_ITER_KEYS:
    *out = js_mknum((double)idx);
    break;
  case ARR_ITER_ENTRIES: {
    ant_value_t pair = js_mkarr(js);
    js_arr_push(js, pair, js_mknum((double)idx));
    js_arr_push(js, pair, js_mknum(value));
    *out = pair;
    break;
  }
  default:
    *out = js_mknum(value);
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
    }
  }
}

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

static ant_value_t js_arraybuffer_slice(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_slice(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_subarray(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_fill(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_at(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_set(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_copyWithin(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_toReversed(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_toSorted(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_with(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_getUint8(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_setUint8(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_getInt16(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_setInt16(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_getInt32(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_setInt32(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_getFloat32(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_setFloat32(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_getFloat64(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_dataview_setFloat64(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_typedarray_toString(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_buffer_slice(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_buffer_toString(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_buffer_toBase64(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t js_buffer_write(ant_t *js, ant_value_t *args, int nargs);

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
    &&L_4, &&L_4, &&L_4, &&L_8, &&L_8, &&L_8
  };
  
  if (type > TYPED_ARRAY_BIGUINT64) goto L_1;
  goto *dispatch[type];
  
  L_1: return 1;
  L_2: return 2;
  L_4: return 4;
  L_8: return 8;
}

static const char *typedarray_type_name(TypedArrayType type) {
switch (type) {
  case TYPED_ARRAY_INT8: return "Int8Array";
  case TYPED_ARRAY_UINT8: return "Uint8Array";
  case TYPED_ARRAY_UINT8_CLAMPED: return "Uint8ClampedArray";
  case TYPED_ARRAY_INT16: return "Int16Array";
  case TYPED_ARRAY_UINT16: return "Uint16Array";
  case TYPED_ARRAY_INT32: return "Int32Array";
  case TYPED_ARRAY_UINT32: return "Uint32Array";
  case TYPED_ARRAY_FLOAT32: return "Float32Array";
  case TYPED_ARRAY_FLOAT64: return "Float64Array";
  case TYPED_ARRAY_BIGINT64: return "BigInt64Array";
  case TYPED_ARRAY_BIGUINT64: return "BigUint64Array";
  default: return "Uint8Array";
}}

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
    length, typedarray_type_name(type), ab_obj
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
  js_set_slot(obj, SLOT_BUFFER, ANT_PTR(data));
  js_set(js, obj, "byteLength", js_mknum((double)length));

  return obj;
}

// ArrayBuffer.prototype.slice(begin, end)
static ant_value_t js_arraybuffer_slice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  if (vtype(data_val) != T_NUM) {
    return js_mkerr(js, "Not an ArrayBuffer");
  }
  
  ArrayBufferData *data = (ArrayBufferData *)(uintptr_t)js_getnum(data_val);
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
  js_set_slot(new_obj, SLOT_BUFFER, ANT_PTR(new_data));
  js_set(js, new_obj, "byteLength", js_mknum((double)new_length));
  
  return new_obj;
}

// ArrayBuffer.prototype.transfer(newLength)
static ant_value_t js_arraybuffer_transfer(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  if (vtype(data_val) != T_NUM) {
    return js_mkerr(js, "Not an ArrayBuffer");
  }
  
  ArrayBufferData *data = (ArrayBufferData *)(uintptr_t)js_getnum(data_val);
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
  js_set_slot(new_obj, SLOT_BUFFER, ANT_PTR(new_data));
  js_set(js, new_obj, "byteLength", js_mknum((double)new_length));
  
  return new_obj;
}

// ArrayBuffer.prototype.transferToFixedLength(newLength)
static ant_value_t js_arraybuffer_transferToFixedLength(ant_t *js, ant_value_t *args, int nargs) {
  return js_arraybuffer_transfer(js, args, nargs);
}

// ArrayBuffer.prototype.detached getter
static ant_value_t js_arraybuffer_detached_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t data_val = js_get_slot(this_val, SLOT_BUFFER);
  if (vtype(data_val) != T_NUM) return js_false;
  
  ArrayBufferData *data = (ArrayBufferData *)(uintptr_t)js_getnum(data_val);
  if (!data) return js_true;
  
  return js_bool(data->is_detached);
}

static ant_value_t typedarray_index_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  if (key_len == 0 || key_len > 10) return js_mkundef();
  
  size_t index = 0;
  for (size_t i = 0; i < key_len; i++) {
    char c = key[i];
    if (c < '0' || c > '9') return js_mkundef();
    index = index * 10 + (c - '0');
  }
  
  ant_value_t ta_val = js_get_slot(obj, SLOT_BUFFER);
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_val);
  if (!ta_data || index >= ta_data->length) return js_mkundef();
  if (!ta_data->buffer || ta_data->buffer->is_detached) return js_mkundef();
  
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

static bool typedarray_index_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value) {
  if (key_len == 0 || key_len > 10) return false;
  
  size_t index = 0;
  for (size_t i = 0; i < key_len; i++) {
    char c = key[i];
    if (c < '0' || c > '9') return false;
    index = index * 10 + (c - '0');
  }
  
  ant_value_t ta_val = js_get_slot(obj, SLOT_BUFFER);
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_val);
  if (!ta_data || index >= ta_data->length) return true;
  if (!ta_data->buffer || ta_data->buffer->is_detached) return true;
  
  double num_val = vtype(value) == T_NUM ? js_getnum(value) : 0;
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

static bool typedarray_read_number(const TypedArrayData *ta_data, size_t index, double *out) {
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached || index >= ta_data->length) return false;
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;

  static const void *dispatch[] = {
    &&R_INT8, &&R_UINT8, &&R_UINT8, &&R_INT16, &&R_UINT16,
    &&R_INT32, &&R_UINT32, &&R_FLOAT32, &&R_FLOAT64, &&R_FAIL, &&R_FAIL
  };

  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto R_FAIL;
  goto *dispatch[ta_data->type];

  R_INT8:    *out = (double)((int8_t *)data)[index];   return true;
  R_UINT8:   *out = (double)data[index];               return true;
  R_INT16:   *out = (double)((int16_t *)data)[index];  return true;
  R_UINT16:  *out = (double)((uint16_t *)data)[index]; return true;
  R_INT32:   *out = (double)((int32_t *)data)[index];  return true;
  R_UINT32:  *out = (double)((uint32_t *)data)[index]; return true;
  R_FLOAT32: *out = (double)((float *)data)[index];    return true;
  R_FLOAT64: *out = ((double *)data)[index];           return true;
  R_FAIL:    return false;
}

static bool typedarray_write_number(TypedArrayData *ta_data, size_t index, double value) {
  if (!ta_data || !ta_data->buffer || ta_data->buffer->is_detached || index >= ta_data->length) return false;
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;

  static const void *dispatch[] = {
    &&W_INT8, &&W_UINT8, &&W_UINT8, &&W_INT16, &&W_UINT16,
    &&W_INT32, &&W_UINT32, &&W_FLOAT32, &&W_FLOAT64, &&W_FAIL, &&W_FAIL
  };

  if (ta_data->type > TYPED_ARRAY_BIGUINT64) goto W_FAIL;
  goto *dispatch[ta_data->type];

  W_INT8:    ((int8_t *)data)[index] = (int8_t)value;     return true;
  W_UINT8:   data[index] = (uint8_t)value;                return true;
  W_INT16:   ((int16_t *)data)[index] = (int16_t)value;   return true;
  W_UINT16:  ((uint16_t *)data)[index] = (uint16_t)value; return true;
  W_INT32:   ((int32_t *)data)[index] = (int32_t)value;   return true;
  W_UINT32:  ((uint32_t *)data)[index] = (uint32_t)value; return true;
  W_FLOAT32: ((float *)data)[index] = (float)value;       return true;
  W_FLOAT64: ((double *)data)[index] = value;             return true;
  W_FAIL:    return false;
}

ant_value_t create_arraybuffer_obj(ant_t *js, ArrayBufferData *buffer) {
  ant_value_t ab_obj = js_mkobj(js);
  ant_value_t ab_proto = js_get_ctor_proto(js, "ArrayBuffer", 11);
  if (is_special_object(ab_proto)) js_set_proto_init(ab_obj, ab_proto);
  
  js_set_slot(ab_obj, SLOT_BUFFER, js_mknum((double)(uintptr_t)buffer));
  js_set(js, ab_obj, "byteLength", js_mknum((double)buffer->length));
  buffer->ref_count++;
  
  return ab_obj;
}

ant_value_t create_typed_array_with_buffer(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, const char *type_name, ant_value_t arraybuffer_obj
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
  
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, type_name, strlen(type_name));
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  
  js_set_slot(obj, SLOT_BUFFER, js_mktypedarray(ta_data));
  js_set(js, obj, "length", js_mknum((double)length));
  js_set(js, obj, "byteLength", js_mknum((double)(length * element_size)));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset));
  js_set(js, obj, "BYTES_PER_ELEMENT", js_mknum((double)element_size));
  js_set(js, obj, "buffer", arraybuffer_obj);
  
  js_set_getter(obj, typedarray_index_getter);
  js_set_setter(obj, typedarray_index_setter);
  
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

typedef struct {
  ant_value_t *values;
  size_t length;
  size_t capacity;
} iter_collect_ctx_t;

static bool iter_collect_callback(ant_t *js, ant_value_t value, void *udata) {
  (void)js;
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
  
  ant_value_t buffer_data_val = js_get_slot(args[0], SLOT_BUFFER);
  if (vtype(buffer_data_val) == T_NUM) {
    ArrayBufferData *buffer = (ArrayBufferData *)(uintptr_t)js_getnum(buffer_data_val);
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
      uint8_t *data = buffer->data;
      
      static const void *write_dispatch[] = {
        &&W_INT8, &&W_UINT8, &&W_UINT8, &&W_INT16, &&W_UINT16,
        &&W_INT32, &&W_UINT32, &&W_FLOAT32, &&W_FLOAT64, &&W_DONE, &&W_DONE
      };
      
      for (size_t i = 0; i < length; i++) {
        ant_value_t elem;
        if (values) elem = values[i]; else {
          char idx_str[16];
          snprintf(idx_str, sizeof(idx_str), "%zu", i);
          elem = js_get(js, args[0], idx_str);
        }
        
        double val = vtype(elem) == T_NUM 
          ? js_getnum(elem) 
          : js_to_number(js, elem);
        
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
// TypedArray.prototype.at(index)
static ant_value_t js_typedarray_at(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  if (nargs == 0 || vtype(args[0]) != T_NUM) return js_mkundef();
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t idx = (ssize_t)js_getnum(args[0]);
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) return js_mkundef();
  if (!ta_data->buffer || ta_data->buffer->is_detached) return js_mkundef();
  
  size_t index = (size_t)idx;
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  double value;
  
  switch (ta_data->type) {
    case TYPED_ARRAY_INT8:         value = (double)((int8_t*)data)[index];   break;
    case TYPED_ARRAY_UINT8:
    case TYPED_ARRAY_UINT8_CLAMPED: value = (double)data[index];            break;
    case TYPED_ARRAY_INT16:        value = (double)((int16_t*)data)[index];  break;
    case TYPED_ARRAY_UINT16:       value = (double)((uint16_t*)data)[index]; break;
    case TYPED_ARRAY_INT32:        value = (double)((int32_t*)data)[index];  break;
    case TYPED_ARRAY_UINT32:       value = (double)((uint32_t*)data)[index]; break;
    case TYPED_ARRAY_FLOAT32:      value = (double)((float*)data)[index];    break;
    case TYPED_ARRAY_FLOAT64:      value = ((double*)data)[index];           break;
    default: return js_mkundef();
  }
  return js_mknum(value);
}

static ant_value_t js_typedarray_slice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
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
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
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
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  double value = 0;
  if (nargs > 0 && vtype(args[0]) == T_NUM) value = js_getnum(args[0]);
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t start = 0, end = len;
  
  if (nargs > 1 && vtype(args[1]) == T_NUM) start = (ssize_t)js_getnum(args[1]);
  if (nargs > 2 && vtype(args[2]) == T_NUM) end = (ssize_t)js_getnum(args[2]);
  
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

// TypedArray.prototype.set(source, offset = 0)
static ant_value_t js_typedarray_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "set requires source argument");

  ant_value_t this_val = js_getthis(js);
  ant_value_t dst_ta_val = js_get_slot(this_val, SLOT_BUFFER);
  TypedArrayData *dst = (TypedArrayData *)js_gettypedarray(dst_ta_val);
  if (!dst) return js_mkerr(js, "Invalid TypedArray");
  if (!dst->buffer || dst->buffer->is_detached) return js_mkerr(js, "Cannot operate on a detached TypedArray");

  ssize_t offset_i = 0;
  if (nargs > 1 && vtype(args[1]) == T_NUM) offset_i = (ssize_t)js_getnum(args[1]);
  if (offset_i < 0) return js_mkerr(js, "Offset out of bounds");
  size_t offset = (size_t)offset_i;
  if (offset > dst->length) return js_mkerr(js, "Offset out of bounds");

  ant_value_t src_val = args[0];
  ant_value_t src_ta_val = js_get_slot(src_val, SLOT_BUFFER);
  TypedArrayData *src_ta = (TypedArrayData *)js_gettypedarray(src_ta_val);

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
      double value = 0;
      if (!typedarray_read_number(src_ta, i, &value)) value = 0;
      (void)typedarray_write_number(dst, offset + i, value);
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
    double value = 0;
    if (vtype(src_val) == T_STR) {
      ant_offset_t slen = 0;
      ant_offset_t soff = vstr(js, src_val, &slen);
      const unsigned char *sptr = (const unsigned char *)(uintptr_t)soff;
      if (i < (size_t)slen) value = sptr[i];
    } else {
      char idx[24];
      size_t idx_len = uint_to_str(idx, sizeof(idx), (uint64_t)i);
      idx[idx_len] = '\0';
      ant_value_t elem = js_get(js, src_val, idx);
      value = vtype(elem) == T_NUM ? js_getnum(elem) : 0;
    }
    (void)typedarray_write_number(dst, offset + i, value);
  }

  return js_mkundef();
}

// TypedArray.prototype.copyWithin(target, start, end)
static ant_value_t js_typedarray_copyWithin(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(ta_data_val);
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
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
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
  
  ant_value_t out = create_typed_array_like(
    js, this_val, ta_data->type,
    new_buffer, 0, length
  ); free_array_buffer_data(new_buffer);
  
  return out;
}

// TypedArray.prototype.toSorted(comparefn)
static ant_value_t js_typedarray_toSorted(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid TypedArray");
  
  size_t length = ta_data->length;
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(new_buffer->data, ta_data->buffer->data + ta_data->byte_offset, length * element_size);
  ant_value_t result = create_typed_array_like(js, this_val, ta_data->type, new_buffer, 0, length);
  
  free_array_buffer_data(new_buffer);
  if (is_err(result)) return result;
  
  ant_value_t result_ta_val = js_get_slot(result, SLOT_BUFFER);
  TypedArrayData *result_ta = (TypedArrayData *)js_gettypedarray(result_ta_val);
  uint8_t *data = result_ta->buffer->data;
  
  ant_value_t comparefn = (nargs > 0 && vtype(args[0]) == T_FUNC) ? args[0] : js_mkundef();
  bool has_comparefn = vtype(comparefn) == T_FUNC;
  
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
        ant_value_t cmp_args[2] = { js_mknum(a_val), js_mknum(b_val) };
        ant_value_t cmp_result = sv_vm_call(js->vm, js, comparefn, js_mkundef(), cmp_args, 2, NULL, false);
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
static ant_value_t js_typedarray_with(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "with requires index and value");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
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
  
  ant_value_t out = create_typed_array_like(
    js, this_val, ta_data->type,
    new_buffer, 0, length
  ); free_array_buffer_data(new_buffer);
  
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
DEFINE_TYPEDARRAY_CONSTRUCTOR(Float32Array, TYPED_ARRAY_FLOAT32)
DEFINE_TYPEDARRAY_CONSTRUCTOR(Float64Array, TYPED_ARRAY_FLOAT64)
DEFINE_TYPEDARRAY_CONSTRUCTOR(BigInt64Array, TYPED_ARRAY_BIGINT64)
DEFINE_TYPEDARRAY_CONSTRUCTOR(BigUint64Array, TYPED_ARRAY_BIGUINT64)

static ant_value_t js_dataview_constructor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "DataView constructor requires 'new'");
  }
  if (nargs < 1) {
    return js_mkerr(js, "DataView requires an ArrayBuffer");
  }
  
  ant_value_t buffer_data_val = js_get_slot(args[0], SLOT_BUFFER);
  if (vtype(buffer_data_val) != T_NUM) {
    return js_mkerr(js, "First argument must be an ArrayBuffer");
  }
  
  ArrayBufferData *buffer = (ArrayBufferData *)(uintptr_t)js_getnum(buffer_data_val);
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
  
  DataViewData *dv_data = ta_arena_alloc(sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");
  
  dv_data->buffer = buffer;
  dv_data->byte_offset = byte_offset;
  dv_data->byte_length = byte_length;
  buffer->ref_count++;
  
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get_ctor_proto(js, "DataView", 8);
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  
  js_set_slot(obj, SLOT_DATA, ANT_PTR(dv_data));
  js_mkprop_fast(js, obj, "buffer", 6, args[0]);
  js_set_descriptor(js, obj, "buffer", 6, 0);

  js_set(js, obj, "byteLength", js_mknum((double)byte_length));
  js_set(js, obj, "byteOffset", js_mknum((double)byte_offset)); 
  
  return obj;
}

// DataView.prototype.getUint8(byteOffset)
static ant_value_t js_dataview_getUint8(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getUint8 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_setUint8(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setUint8 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_getInt16(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getInt16 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_getInt32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getInt32 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_getFloat32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getFloat32 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_setInt16(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setInt16 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_setInt32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setInt32 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_setFloat32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setFloat32 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_getFloat64(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "getFloat64 requires byteOffset");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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
static ant_value_t js_dataview_setFloat64(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "setFloat64 requires byteOffset and value");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t dv_data_val = js_get_slot(this_val, SLOT_DATA);
  
  if (vtype(dv_data_val) != T_NUM) {
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

static ant_value_t typedarray_join_with(ant_t *js, ant_value_t this_val, const char *sep, size_t sep_len) {
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);

  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
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

// Buffer.prototype.toString(encoding)
static ant_value_t js_buffer_slice(ant_t *js, ant_value_t *args, int nargs) {
  return js_typedarray_subarray(js, args, nargs);
}

// Buffer.prototype.toString(encoding)
static ant_value_t js_buffer_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Invalid Buffer");
  
  BufferEncoding encoding = ENC_UTF8;
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    size_t enc_len;
    char *enc_str = js_getstr(js, args[0], &enc_len);
    encoding = parse_encoding(enc_str, enc_len);
    if (encoding == ENC_UNKNOWN) encoding = ENC_UTF8;
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
    char *str = malloc(char_count + 1);
    if (!str) return js_mkerr(js, "Failed to allocate string");
    
    for (size_t i = 0; i < char_count; i++) str[i] = (char)data[i * 2];
    str[char_count] = '\0';
    
    ant_value_t result = js_mkstr(js, str, char_count);
    free(str);
    return result;
  } else return js_mkstr(js, (char *)data, len);
}

// Buffer.prototype.toBase64()
static ant_value_t js_buffer_toBase64(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t encoding_arg = js_mkstr(js, "base64", 6);
  ant_value_t new_args[1] = {encoding_arg};
  return js_buffer_toString(js, new_args, 1);
}

// Buffer.prototype.write(string, offset, length, encoding)
static ant_value_t js_buffer_write(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "write requires a string");
  
  ant_value_t this_val = js_getthis(js);
  ant_value_t ta_data_val = js_get_slot(this_val, SLOT_BUFFER);
  
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
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
    
    ant_value_t ta_data_val = js_get_slot(buf, SLOT_BUFFER);
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
static ant_value_t js_buffer_compare(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Buffer.compare requires two arguments");
  
  ant_value_t ta1_val = js_get_slot(args[0], SLOT_BUFFER);
  ant_value_t ta2_val = js_get_slot(args[1], SLOT_BUFFER);
  
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
  js_set_slot(obj, SLOT_BUFFER, ANT_PTR(data));
  js_set(js, obj, "byteLength", js_mknum((double)length));
  
  return obj;
}

ant_value_t buffer_library(ant_t *js) {
  return js_get(js, js_glob(js), "Buffer");
}

void init_buffer_module() {
  ant_t *js = rt->js;
  
  ant_value_t glob = js->global;
  ant_value_t object_proto = js->object;

  ant_value_t arraybuffer_ctor_obj = js_mkobj(js);
  ant_value_t arraybuffer_proto = js_mkobj(js);
  js_set_proto_init(arraybuffer_proto, object_proto);
  
  js_set(js, arraybuffer_proto, "slice", js_mkfun(js_arraybuffer_slice));
  js_set(js, arraybuffer_proto, "transfer", js_mkfun(js_arraybuffer_transfer));
  js_set(js, arraybuffer_proto, "transferToFixedLength", js_mkfun(js_arraybuffer_transferToFixedLength));
  js_set_getter_desc(js, arraybuffer_proto, "detached", 8, js_mkfun(js_arraybuffer_detached_getter), JS_DESC_E);
  js_set_sym(js, arraybuffer_proto, get_toStringTag_sym(), js_mkstr(js, "ArrayBuffer", 11));
  
  js_set_slot(arraybuffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_arraybuffer_constructor));
  js_mkprop_fast(js, arraybuffer_ctor_obj, "prototype", 9, arraybuffer_proto);
  js_mkprop_fast(js, arraybuffer_ctor_obj, "name", 4, ANT_STRING("ArrayBuffer"));
  js_set_descriptor(js, arraybuffer_ctor_obj, "name", 4, 0);
  js_define_species_getter(js, arraybuffer_ctor_obj);
  js_set(js, glob, "ArrayBuffer", js_obj_to_func(arraybuffer_ctor_obj));
  
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
  js_set_sym(js, typedarray_proto, get_toStringTag_sym(), js_mkstr(js, "TypedArray", 10));

  g_typedarray_iter_proto = js_mkobj(js);
  js_set_proto_init(g_typedarray_iter_proto, js->sym.iterator_proto);
  js_set(js, g_typedarray_iter_proto, "next", js_mkfun(ta_iter_next));
  js_iter_register_advance(g_typedarray_iter_proto, advance_typedarray);

  ant_value_t ta_values_fn = js_mkfun(ta_values);
  js_set(js, typedarray_proto, "values", ta_values_fn);
  js_set(js, typedarray_proto, "keys", js_mkfun(ta_keys));
  js_set(js, typedarray_proto, "entries", js_mkfun(ta_entries));
  js_set_sym(js, typedarray_proto, get_iterator_sym(), ta_values_fn);
  
  #define SETUP_TYPEDARRAY(name) \
    do { \
      ant_value_t name##_ctor_obj = js_mkobj(js); \
      ant_value_t name##_proto = js_mkobj(js); \
      js_set_proto_init(name##_proto, typedarray_proto); \
      js_setprop(js, name##_proto, ANT_STRING("constructor"), js_obj_to_func(name##_ctor_obj)); \
      js_set_descriptor(js, name##_proto, "constructor", 11, JS_DESC_W | JS_DESC_C); \
      js_set_sym(js, name##_proto, get_toStringTag_sym(), js_mkstr(js, #name, sizeof(#name) - 1)); \
      js_set_slot(name##_ctor_obj, SLOT_CFUNC, js_mkfun(js_##name##_constructor)); \
      js_setprop(js, name##_ctor_obj, js_mkstr(js, "prototype", 9), name##_proto); \
      js_mkprop_fast(js, name##_ctor_obj, "name", 4, ANT_STRING(#name)); \
      js_set_descriptor(js, name##_ctor_obj, "name", 4, 0); \
      js_define_species_getter(js, name##_ctor_obj); \
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
  
  ant_value_t dataview_ctor_obj = js_mkobj(js);
  ant_value_t dataview_proto = js_mkobj(js);
  js_set_proto_init(dataview_proto, object_proto);
  
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
  js_set_sym(js, dataview_proto, get_toStringTag_sym(), js_mkstr(js, "DataView", 8));

  js_set_slot(dataview_ctor_obj, SLOT_CFUNC, js_mkfun(js_dataview_constructor));
  js_mkprop_fast(js, dataview_ctor_obj, "prototype", 9, dataview_proto);
  js_mkprop_fast(js, dataview_ctor_obj, "name", 4, ANT_STRING("DataView"));
  js_set_descriptor(js, dataview_ctor_obj, "name", 4, 0);
  js_set(js, glob, "DataView", js_obj_to_func(dataview_ctor_obj));
  
  ant_value_t sharedarraybuffer_ctor_obj = js_mkobj(js);
  ant_value_t sharedarraybuffer_proto = js_mkobj(js);
  js_set_proto_init(sharedarraybuffer_proto, object_proto);
  
  js_set(js, sharedarraybuffer_proto, "slice", js_mkfun(js_arraybuffer_slice));
  js_set_sym(js, sharedarraybuffer_proto, get_toStringTag_sym(), js_mkstr(js, "SharedArrayBuffer", 17));
  
  js_set_slot(sharedarraybuffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_sharedarraybuffer_constructor));
  js_mkprop_fast(js, sharedarraybuffer_ctor_obj, "prototype", 9, sharedarraybuffer_proto);
  js_mkprop_fast(js, sharedarraybuffer_ctor_obj, "name", 4, ANT_STRING("SharedArrayBuffer"));
  js_set_descriptor(js, sharedarraybuffer_ctor_obj, "name", 4, 0);
  js_set(js, glob, "SharedArrayBuffer", js_obj_to_func(sharedarraybuffer_ctor_obj));
  
  ant_value_t buffer_ctor_obj = js_mkobj(js);
  ant_value_t buffer_proto = js_mkobj(js);
  
  ant_value_t uint8array_ctor = js_get(js, glob, "Uint8Array");
  ant_value_t uint8array_proto = js_get(js, uint8array_ctor, "prototype");
  
  if (is_special_object(uint8array_proto)) js_set_proto_init(buffer_proto, uint8array_proto);
  else js_set_proto_init(buffer_proto, typedarray_proto);
  
  js_set(js, buffer_proto, "slice", js_mkfun(js_buffer_slice));
  js_set(js, buffer_proto, "toString", js_mkfun(js_buffer_toString));
  js_set(js, buffer_proto, "toBase64", js_mkfun(js_buffer_toBase64));
  js_set(js, buffer_proto, "write", js_mkfun(js_buffer_write));
  
  js_set_sym(js, buffer_proto, get_toStringTag_sym(), js_mkstr(js, "Buffer", 6));
  js_set_sym(js, buffer_proto, get_iterator_sym(), ta_values_fn);
  js_set(js, buffer_proto, "values", ta_values_fn);
  
  js_set(js, buffer_ctor_obj, "from", js_mkfun(js_buffer_from));
  js_set(js, buffer_ctor_obj, "alloc", js_mkfun(js_buffer_alloc));
  js_set(js, buffer_ctor_obj, "allocUnsafe", js_mkfun(js_buffer_allocUnsafe));
  js_set(js, buffer_ctor_obj, "isBuffer", js_mkfun(js_buffer_isBuffer));
  js_set(js, buffer_ctor_obj, "isEncoding", js_mkfun(js_buffer_isEncoding));
  js_set(js, buffer_ctor_obj, "byteLength", js_mkfun(js_buffer_byteLength));
  js_set(js, buffer_ctor_obj, "concat", js_mkfun(js_buffer_concat));
  js_set(js, buffer_ctor_obj, "compare", js_mkfun(js_buffer_compare));

  js_set_slot(buffer_ctor_obj, SLOT_CFUNC, js_mkfun(js_buffer_from));
  js_mkprop_fast(js, buffer_ctor_obj, "prototype", 9, buffer_proto);
  js_mkprop_fast(js, buffer_ctor_obj, "name", 4, ANT_STRING("Buffer"));
  js_set_descriptor(js, buffer_ctor_obj, "name", 4, 0);
  js_set(js, glob, "Buffer", js_obj_to_func(buffer_ctor_obj));
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
  
  ta_arena_offset = 0;
}

size_t buffer_get_external_memory(void) {
  size_t total = ta_arena ? ta_arena_offset : 0;
  
  for (size_t i = 0; i < buffer_registry_count; i++) {
    if (buffer_registry[i])
      total += sizeof(ArrayBufferData) + buffer_registry[i]->capacity;
  }
  total += buffer_registry_cap * sizeof(ArrayBufferData *);
  
  return total;
}
