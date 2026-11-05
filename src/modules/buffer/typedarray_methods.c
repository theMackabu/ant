#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "gc/roots.h"
#include "silver/call.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/iterator.h"
#include "buffer_internal.h"

static bool advance_typedarray(ant_t *js, iterator_t *it, ant_value_t *out) {
  ant_value_t iter = it->iterator;
  ant_value_t ta_obj = js_get_slot(iter, SLOT_DATA);
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == kTypeNumber) ? (uint32_t)Ant_Value_AsNumberUnchecked(state_v) : 0;
  
  uint32_t kind = iter_state_kind(state);
  uint32_t idx  = iter_state_index(state);

  TypedArrayData *ta = buffer_get_typedarray_data(ta_obj);
  if (!Ant_Buffer_ViewIsLive(ta) || idx >= (uint32_t)ta->length) return false;
  
  ant_value_t value;
  if (!buffer_typedarray_data_read_index(js, ta, idx, &value)) return false;

  switch (kind) {
  case ARR_ITER_KEYS:
    *out = Ant_Value_FromNumber((double)idx);
    break;
  case ARR_ITER_ENTRIES: {
    ant_value_t pair = Ant_Array_Create(js);
    js_arr_push(js, pair, Ant_Value_FromNumber((double)idx));
    js_arr_push(js, pair, value);
    *out = pair;
    break;
  }
  default:
    *out = value;
    break;
  }

  js_set_slot(iter, SLOT_ITER_STATE, Ant_Value_FromNumber((double)iter_state_pack(kind, idx + 1)));
  return true;
}

static ant_value_t ta_iter_next(ant_params_t) {
  return js_iter_next_result(js, call_this, advance_typedarray);
}

static ant_value_t ta_values(ant_params_t) {
  if (!typedarray_live(call_this)) return typedarray_this_error(js, call_this);
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_slot_wb(js, iter, SLOT_DATA, call_this);
  js_set_slot(iter, SLOT_ITER_STATE, Ant_Value_FromNumber((double)iter_state_pack(ARR_ITER_VALUES, 0)));
  js_set_proto_init(iter, js->builtins.typedarray_iter_proto);
  return iter;
}

static ant_value_t ta_keys(ant_params_t) {
  if (!typedarray_live(call_this)) return typedarray_this_error(js, call_this);
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_slot_wb(js, iter, SLOT_DATA, call_this);
  js_set_slot(iter, SLOT_ITER_STATE, Ant_Value_FromNumber((double)iter_state_pack(ARR_ITER_KEYS, 0)));
  js_set_proto_init(iter, js->builtins.typedarray_iter_proto);
  return iter;
}

static ant_value_t ta_entries(ant_params_t) {
  if (!typedarray_live(call_this)) return typedarray_this_error(js, call_this);
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_slot_wb(js, iter, SLOT_DATA, call_this);
  js_set_slot(iter, SLOT_ITER_STATE, Ant_Value_FromNumber((double)iter_state_pack(ARR_ITER_ENTRIES, 0)));
  js_set_proto_init(iter, js->builtins.typedarray_iter_proto);
  return iter;
}

static ant_value_t js_typedarray_every(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.every requires a callable");

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  ant_value_t callback = args[0];
  ant_value_t this_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;

  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return Ant_Value_False;

    ant_value_t call_args[3] = { value, Ant_Value_FromNumber((double)i), call_this };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    if (!js_truthy(js, result)) return Ant_Value_False;
  }

  return Ant_Value_True;
}

static ant_value_t js_typedarray_forEach(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.forEach requires a callable");

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  ant_value_t this_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return Ant_Value_Undefined;
    ant_value_t call_args[3] = { value, Ant_Value_FromNumber((double)i), call_this };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
  }

  return Ant_Value_Undefined;
}

static ant_value_t js_typedarray_some(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.some requires a callable");

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  ant_value_t this_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return Ant_Value_False;
    ant_value_t call_args[3] = { value, Ant_Value_FromNumber((double)i), call_this };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return Ant_Value_True;
  }

  return Ant_Value_False;
}

static ant_value_t js_typedarray_find(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.find requires a callable");

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  ant_value_t this_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return Ant_Value_Undefined;
    ant_value_t call_args[3] = { value, Ant_Value_FromNumber((double)i), call_this };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return value;
  }

  return Ant_Value_Undefined;
}

static ant_value_t js_typedarray_findIndex(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.findIndex requires a callable");

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  ant_value_t this_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return Ant_Value_FromNumber(-1);
    ant_value_t call_args[3] = { value, Ant_Value_FromNumber((double)i), call_this };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return Ant_Value_FromNumber((double)i);
  }

  return Ant_Value_FromNumber(-1);
}

static ant_value_t js_typedarray_map(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.map requires a callable");

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  ArrayBufferData *buffer = create_array_buffer_data(js, ta_data->byte_length);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  
  ant_value_t out = create_typed_array_like(js, call_this, ta_data->type, buffer, 0, ta_data->length);
  free_array_buffer_data(js, buffer);
  
  if (is_err(out)) return out;
  TypedArrayData *out_ta = buffer_get_typedarray_data(out);

  ant_value_t this_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return out;
    ant_value_t call_args[3] = { value, Ant_Value_FromNumber((double)i), call_this };
    ant_value_t mapped = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, Ant_Value_Undefined);
    if (is_err(mapped)) return mapped;
    ant_value_t write_result = typedarray_write_value(js, out_ta, i, mapped);
    if (is_err(write_result)) return write_result;
  }

  return out;
}

static ant_value_t js_typedarray_filter(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.filter requires a callable");

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  gc_temp_root_scope_t kept;
  gc_temp_root_scope_begin(js, &kept);

  ant_value_t this_arg = nargs > 1 ? args[1] : Ant_Value_Undefined;
  ant_value_t out = Ant_Value_Undefined;
  for (size_t i = 0; i < ta_data->length; i++) {
    ant_value_t value = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) continue;
    ant_value_t call_args[3] = { value, Ant_Value_FromNumber((double)i), call_this };
    ant_value_t result = sv_vm_call(js->vm, js, args[0], this_arg, call_args, 3, NULL, Ant_Value_Undefined);
    if (is_err(result)) { out = result; goto done; }
    if (js_truthy(js, result) && !gc_temp_root_push(&kept, value)) { out = js_mkerr(js, "oom"); goto done; }
  }

  size_t count = kept.len;
  ArrayBufferData *buffer = create_array_buffer_data(js, count * get_element_size(ta_data->type));
  if (!buffer) { out = js_mkerr(js, "Failed to allocate buffer"); goto done; }
  
  out = create_typed_array_like(js, call_this, ta_data->type, buffer, 0, count);
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

static ant_value_t js_typedarray_reduce_common(ant_native_params_t, ant_value_t call_this, bool right) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "TypedArray.prototype.reduce requires a callable");

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);
  if (ta_data->length == 0 && nargs < 2)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Reduce of empty TypedArray with no initial value");

  size_t i = right ? ta_data->length : 0;
  ant_value_t acc = nargs > 1 ? args[1] : Ant_Value_Undefined;
  if (nargs < 2) {
    if (right) i--;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &acc)) return Ant_Value_Undefined;
    if (!right) i++;
  }

  while (right ? (i > 0) : (i < ta_data->length)) {
    if (right) i--;
    ant_value_t value = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, i, &value)) return acc;
    ant_value_t call_args[4] = { acc, value, Ant_Value_FromNumber((double)i), call_this };
    acc = sv_vm_call(js->vm, js, args[0], Ant_Value_Undefined, call_args, 4, NULL, Ant_Value_Undefined);
    if (is_err(acc)) return acc;
    if (!right) i++;
  }

  return acc;
}

static ant_value_t js_typedarray_reduce(ant_params_t) {
  return js_typedarray_reduce_common(js, args, nargs, call_this, false);
}

static ant_value_t js_typedarray_reduceRight(ant_params_t) {
  return js_typedarray_reduce_common(js, args, nargs, call_this, true);
}

// TypedArray.prototype.at(index)
static ant_value_t js_typedarray_at(ant_params_t) {
  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);
  
  if (nargs == 0 || vtype(args[0]) != kTypeNumber) return Ant_Value_Undefined;
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t idx = (ssize_t)Ant_Value_AsNumberUnchecked(args[0]);
  if (idx < 0) idx += len;
  if (idx < 0 || idx >= len) return Ant_Value_Undefined;
  if (!Ant_Buffer_ViewIsLive(ta_data)) return Ant_Value_Undefined;
  
  ant_value_t value;
  if (!buffer_typedarray_data_read_index(js, ta_data, (size_t)idx, &value)) return Ant_Value_Undefined;
  return value;
}

static ant_value_t js_typedarray_slice(ant_params_t) {
  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t begin = 0, end = len;
  
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) begin = (ssize_t)Ant_Value_AsNumberUnchecked(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) end = (ssize_t)Ant_Value_AsNumberUnchecked(args[1]);
  
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
    js, call_this, ta_data->type, 
    new_buffer, 0, new_length
  ); free_array_buffer_data(js, new_buffer);
  
  return out;
}

// TypedArray.prototype.subarray(begin, end)
ant_value_t js_typedarray_subarray(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  if (!ta_data || !Ant_Buffer_IsLive(ta_data->backing)) return typedarray_this_error(js, call_this);
  
  ssize_t len = typedarray_view_live(ta_data) ? (ssize_t)ta_data->length : 0;
  ssize_t begin = 0, end = len;
  
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) begin = (ssize_t)Ant_Value_AsNumberUnchecked(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) end = (ssize_t)Ant_Value_AsNumberUnchecked(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  size_t element_size = get_element_size(ta_data->type);
  size_t new_offset = ta_data->byte_offset + (size_t)begin * element_size;

  ant_value_t out = create_typed_array_like(
    js, call_this, ta_data->type,
    ta_data->backing, new_offset, new_length
  );

  if (__builtin_expect(ta_data->length_tracking, 0) && (nargs < 2 || vtype(args[1]) == kTypeUndefined) && !is_err(out)) {
    TypedArrayData *out_data = buffer_get_typedarray_data(out);
    out_data->length_tracking = true;
    typedarray_view_refresh(out_data);
  }

  return out;
}

// TypedArray.prototype.fill(value, start, end)
static ant_value_t js_typedarray_fill(ant_params_t) {
  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);
  
  ant_value_t value = typedarray_coerce_value(js, ta_data, nargs > 0 ? args[0] : Ant_Value_Undefined);
  if (is_err(value)) return value;

  bool is_bigint = typedarray_type_is_bigint(ta_data->type);
  double num_value = is_bigint ? 0 : Ant_Value_AsNumberUnchecked(value);

  ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);
  
  ssize_t len = (ssize_t)ta_data->length;
  ssize_t start = 0, end = len;
  
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) start = (ssize_t)Ant_Value_AsNumberUnchecked(args[1]);
  if (nargs > 2 && vtype(args[2]) == kTypeNumber) end = (ssize_t)Ant_Value_AsNumberUnchecked(args[2]);
  
  start = normalize_index(start, len);
  end = normalize_index(end, len);
  if (end < start) end = start;

  if (is_bigint) {
    for (ssize_t i = start; i < end; i++) {
      ant_value_t write_result = typedarray_write_value(js, ta_data, (size_t)i, value);
      if (is_err(write_result)) return write_result;
    }
    return call_this;
  }
  
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;

  #define BUFFER_TA_FILL_INT(ctype, store) {                         \
    ctype fill = store(num_value);                                   \
    for (ssize_t i = start; i < end; i++) ((ctype *)data)[i] = fill; \
    break;                                                           \
  }
  
  #define BUFFER_TA_FILL_FLOAT BUFFER_TA_FILL_INT
  #define BUFFER_TA_FILL_BIGINT(ctype, store) break;
  #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) case TYPED_ARRAY_##name: BUFFER_TA_FILL_##kind(ctype, store)

  switch (ta_data->type) {
    #include "buffer/typed_arrays.h"
    default: break;
  }

  #undef BUFFER_TA_FILL_INT
  #undef BUFFER_TA_FILL_FLOAT
  #undef BUFFER_TA_FILL_BIGINT

  return call_this;
}

// TypedArray.prototype.set(source, offset = 0)
static ant_value_t js_typedarray_set(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "set requires source argument");

  TypedArrayData *dst = typedarray_live(call_this);
  if (!dst) return typedarray_this_error(js, call_this);

  ssize_t offset_i = 0;
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) offset_i = (ssize_t)Ant_Value_AsNumberUnchecked(args[1]);
  if (offset_i < 0) return js_mkerr(js, "Offset out of bounds");
  size_t offset = (size_t)offset_i;
  if (offset > dst->length) return js_mkerr(js, "Offset out of bounds");

  ant_value_t src_val = args[0];
  TypedArrayData *src_ta = buffer_get_typedarray_data(src_val);
  if (src_ta && !typedarray_live(src_val)) return typedarray_this_error(js, src_val);

  if (src_ta) {
    if (typedarray_type_is_bigint(src_ta->type) != typedarray_type_is_bigint(dst->type))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot mix BigInt and other types, use explicit conversions");
    size_t src_len = src_ta->length;
    if (offset + src_len > dst->length) return js_mkerr_typed(js, JS_ERR_RANGE, "offset is out of bounds");

    if (src_ta->type == dst->type) {
      size_t el = get_element_size(dst->type);
      uint8_t *dst_data = dst->buffer->data + dst->byte_offset + offset * el;
      uint8_t *src_data = src_ta->buffer->data + src_ta->byte_offset;
      memmove(dst_data, src_data, src_len * el);
      return Ant_Value_Undefined;
    }

    for (size_t i = 0; i < src_len; i++) {
      ant_value_t value = Ant_Value_Undefined;
      if (!buffer_typedarray_data_read_index(js, src_ta, i, &value))
        return js_mkerr(js, "Failed to read from source TypedArray");
      ant_value_t write_result = typedarray_write_value(js, dst, offset + i, value);
      if (is_err(write_result)) return write_result;
    }
    return Ant_Value_Undefined;
  }

  if (!is_special_object(src_val) && vtype(src_val) != kTypeString) {
    return js_mkerr(js, "set source must be array-like or TypedArray");
  }

  size_t src_len = 0;
  if (vtype(src_val) == kTypeString) {
    src_len = (size_t)vstrlen(js, src_val);
  } else {
    ant_value_t len_val = js_get(js, src_val, "length");
    src_len = vtype(len_val) == kTypeNumber ? (size_t)Ant_Value_AsNumberUnchecked(len_val) : 0;
  }

  if (offset + src_len > dst->length) return js_mkerr_typed(js, JS_ERR_RANGE, "offset is out of bounds");

  for (size_t i = 0; i < src_len; i++) {
    if (vtype(src_val) == kTypeString) {
      ant_offset_t slen = 0;
      ant_offset_t soff = vstr(js, src_val, &slen);
      const unsigned char *sptr = (const unsigned char *)(uintptr_t)soff;
      double value = 0;
      if (i < (size_t)slen) value = sptr[i];
      ant_value_t write_result = typedarray_write_value(js, dst, offset + i, Ant_Value_FromNumber(value));
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

  return Ant_Value_Undefined;
}

// TypedArray.prototype.copyWithin(target, start, end)
static ant_value_t js_typedarray_copyWithin(ant_params_t) {
  TypedArrayData *ta = typedarray_live(call_this);
  if (!ta) return typedarray_this_error(js, call_this);

  ssize_t len = (ssize_t)ta->length;
  if (len <= 0) return call_this;

  ssize_t target = 0, start = 0, end = len;
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) target = (ssize_t)Ant_Value_AsNumberUnchecked(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) start = (ssize_t)Ant_Value_AsNumberUnchecked(args[1]);
  if (nargs > 2 && vtype(args[2]) == kTypeNumber) end = (ssize_t)Ant_Value_AsNumberUnchecked(args[2]);

  target = normalize_index(target, len);
  start = normalize_index(start, len);
  end = normalize_index(end, len);
  if (end < start) end = start;
  if (target >= len || start >= len || end <= start) return call_this;

  size_t count = (size_t)(end - start);
  size_t max_to_end = (size_t)(len - target);
  if (count > max_to_end) count = max_to_end;
  if (count == 0) return call_this;

  size_t el = get_element_size(ta->type);
  uint8_t *base = ta->buffer->data + ta->byte_offset;
  memmove(base + (size_t)target * el, base + (size_t)start * el, count * el);
  return call_this;
}

// TypedArray.prototype.toReversed()
static ant_value_t js_typedarray_toReversed(ant_params_t) {
  (void)args; (void)nargs;
  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);
  
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
    js, call_this, ta_data->type,
    new_buffer, 0, length
  ); free_array_buffer_data(js, new_buffer);
  
  return out;
}

// TypedArray.prototype.toSorted(comparefn)
static inline int typedarray_sort_compare(ant_t *js, ant_value_t comparefn, double a, double b) {
  if (vtype(comparefn) != kTypeFunction) return (a > b) - (a < b);
  ant_value_t cmp_args[2] = { Ant_Value_FromNumber(a), Ant_Value_FromNumber(b) };
  return (int)Ant_Value_AsNumberUnchecked(sv_vm_call(js->vm, js, comparefn, Ant_Value_Undefined, cmp_args, 2, NULL, Ant_Value_Undefined));
}

static ant_value_t js_typedarray_toSorted(ant_params_t) {
  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);
  
  size_t length = ta_data->length;
  size_t element_size = get_element_size(ta_data->type);
  ArrayBufferData *new_buffer = create_array_buffer_data(js, length * element_size);
  if (!new_buffer) return js_mkerr(js, "Failed to allocate new buffer");
  
  memcpy(new_buffer->data, ta_data->buffer->data + ta_data->byte_offset, length * element_size);
  ant_value_t result = create_typed_array_like(js, call_this, ta_data->type, new_buffer, 0, length);
  
  free_array_buffer_data(js, new_buffer);
  if (is_err(result)) return result;
  
  TypedArrayData *result_ta = buffer_get_typedarray_data(result);
  uint8_t *data = result_ta->buffer->data;
  
  ant_value_t comparefn = (nargs > 0 && vtype(args[0]) == kTypeFunction) ? args[0] : Ant_Value_Undefined;
  
  #define BUFFER_TA_SORT_INT(ctype, load) {                                                       \
    ctype *items = (ctype *)data;                                                                 \
    for (size_t i = 1; i < length; i++) for (size_t j = i; j > 0; j--) {                          \
      if (typedarray_sort_compare(js, comparefn, load(items[j - 1]), load(items[j])) <= 0) break; \
      ctype tmp = items[j - 1]; items[j - 1] = items[j]; items[j] = tmp;                          \
    }                                                                                             \
    break;                                                                                        \
  }
  
  #define BUFFER_TA_SORT_FLOAT BUFFER_TA_SORT_INT
  #define BUFFER_TA_SORT_BIGINT(ctype, load) break;
  #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) case TYPED_ARRAY_##name: BUFFER_TA_SORT_##kind(ctype, load)

  switch (ta_data->type) {
    #include "buffer/typed_arrays.h"
    default: break;
  }

  #undef BUFFER_TA_SORT_INT
  #undef BUFFER_TA_SORT_FLOAT
  #undef BUFFER_TA_SORT_BIGINT

  return result;
}

// TypedArray.prototype.with(index, value)
static ant_value_t js_typedarray_with(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "with requires index and value");
  
  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);
  
  ssize_t index = (ssize_t)Ant_Value_AsNumberUnchecked(args[0]);
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
    js, call_this, ta_data->type,
    new_buffer, 0, length
  ); free_array_buffer_data(js, new_buffer);
  if (is_err(out)) return out;

  TypedArrayData *out_ta = buffer_get_typedarray_data(out);
  ant_value_t write_result = typedarray_write_value(js, out_ta, (size_t)index, args[1]);
  if (is_err(write_result)) return write_result;
  
  return out;
}

#define BUFFER_TA_JOIN_INT(ctype, load) written = (ctype)-1 < 0   \
  ? snprintf(buf + pos, cap - pos, "%d", (int)((ctype *)data)[i]) \
  : snprintf(buf + pos, cap - pos, "%u", (unsigned)((ctype *)data)[i]); break;

#define BUFFER_TA_JOIN_FLOAT(ctype, load) written = snprintf(buf + pos, cap - pos, "%g", load(((ctype *)data)[i])); break;

#define BUFFER_TA_JOIN_BIGINT(ctype, load)                                            \
  written = (ctype)-1 < 0                                                             \
    ? snprintf(buf + pos, cap - pos, "%lld", (long long)((ctype *)data)[i])           \
    : snprintf(buf + pos, cap - pos, "%llu", (unsigned long long)((ctype *)data)[i]); \
  break;

static bool typedarray_join_reserve(char **buf, size_t *cap, size_t needed) {
  if (needed <= *cap) return true;
  size_t next = *cap;
  
  while (next < needed) next = next > SIZE_MAX / 2 ? needed : next * 2;
  char *grown = realloc(*buf, next);
  
  if (!grown) return false;
  *buf = grown;
  *cap = next;
  
  return true;
}

static ant_value_t typedarray_join_with(ant_t *js, ant_value_t this_val, const char *sep, size_t sep_len) {
  TypedArrayData *ta_data = typedarray_live(this_val);
  if (!ta_data) return typedarray_this_error(js, this_val);
  
  if (ta_data->length == 0) return js_mkstr(js, "", 0);
  uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
  
  size_t len = ta_data->length;
  size_t cap = len * 12;
  char *buf = malloc(cap);
  
  if (!buf) return Ant_Error_CreateOutOfMemory(js);
  size_t pos = 0;

  for (size_t i = 0; i < len; i++) {
    size_t sep_here = i > 0 ? sep_len : 0;
    if (!typedarray_join_reserve(&buf, &cap, pos + sep_here + 32)) {
      free(buf);
      return Ant_Error_CreateOutOfMemory(js);
    }
    
    memcpy(buf + pos, sep, sep_here);
    pos += sep_here;

    int written = 0;
    #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) case TYPED_ARRAY_##name: BUFFER_TA_JOIN_##kind(ctype, load)

    switch (ta_data->type) {
      #include "buffer/typed_arrays.h"
      default: break;
    }
    
    if (written > 0) pos += (size_t)written;
  }

  ant_value_t ret = js_mkstr(js, buf, pos);
  free(buf);
  return ret;
}

#undef BUFFER_TA_JOIN_INT
#undef BUFFER_TA_JOIN_FLOAT
#undef BUFFER_TA_JOIN_BIGINT

// TypedArray.prototype.toString()
static ant_value_t js_typedarray_toString(ant_params_t) {
  return typedarray_join_with(js, call_this, ",", 1);
}

// TypedArray.prototype.join(separator)
static ant_value_t js_typedarray_join(ant_params_t) {
  const char *sep = ",";
  size_t sep_len = 1;
  if (nargs > 0 && vtype(args[0]) == kTypeString)
    sep = js_getstr(js, args[0], &sep_len);
  return typedarray_join_with(js, call_this, sep, sep_len);
}

typedef enum {
  TA_SEARCH_INDEX_OF,
  TA_SEARCH_LAST_INDEX_OF,
  TA_SEARCH_INCLUDES,
} typedarray_search_kind_t;

static ant_value_t typedarray_search_from_index(ant_t *js, ant_value_t value, double *out) {
  if (is_object_type(value) || vtype(value) == kTypeBuiltin) {
    value = js_to_primitive(js, value, 2);
    if (is_err(value)) return value;
  }

  if (vtype(value) == kTypeSymbol || vtype(value) == kTypeBigInt)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert value to a number");

  double number = js_to_number(js, value);
  *out = isnan(number) ? 0 : trunc(number);
  return Ant_Value_Undefined;
}

static inline bool typedarray_search_int(double needle, double lo, double hi, int64_t *out) {
  if (!(needle >= lo && needle <= hi) || needle != trunc(needle)) return false;
  *out = (int64_t)needle;
  return true;
}

static inline bool typedarray_search_bigint(ant_t *js, ant_value_t search, bool is_signed, uint64_t *out) {
  if (!is_signed) return bigint_to_uint64_checked(js, search, out);
  int64_t value = 0;
  if (!bigint_to_int64_checked(js, search, &value)) return false;
  *out = (uint64_t)value;
  return true;
}

#define TA_INT_MAX(T) ((T)-1 < 0 ? (double)((1ULL << (8 * sizeof(T) - 1)) - 1) : (double)(T)-1)
#define TA_INT_MIN(T) ((T)-1 < 0 ? -TA_INT_MAX(T) - 1 : 0.0)

#define TA_SEARCH_LOOP(T, match) do {                                               \
  const T *p = (const T *)data;                                                     \
  if (reverse) { for (size_t i = end; i-- > start;) if (match) return (int64_t)i; } \
  else for (size_t i = start; i < end; i++) if (match) return (int64_t)i;           \
  return -1;                                                                        \
} while (0)

#define TA_SEARCH_INT(T, load) do {                                         \
  if (vtype(search) != kTypeNumber) return -1;                              \
  if (!typedarray_search_int(Ant_Value_AsNumberUnchecked(search),           \
  TA_INT_MIN(T), TA_INT_MAX(T), &bits))                                     \
    return -1;                                                              \
  T value = (T)bits;                                                        \
  if (sizeof(T) == 1 && !reverse) {                                         \
    const uint8_t *hit = memchr(data + start, (uint8_t)value, end - start); \
    return hit ? (int64_t)(hit - data) : -1;                                \
  }                                                                         \
  TA_SEARCH_LOOP(T, p[i] == value);                                         \
} while (0)

#define TA_SEARCH_FLOAT(T, load) do {                  \
  if (vtype(search) != kTypeNumber) return -1;         \
  double needle = Ant_Value_AsNumberUnchecked(search); \
  if (isnan(needle)) {                                 \
    if (!same_value_zero) return -1;                   \
    TA_SEARCH_LOOP(T, isnan(load(p[i])));              \
  }                                                    \
  TA_SEARCH_LOOP(T, load(p[i]) == needle);             \
} while (0)

#define TA_SEARCH_BIGINT(T, load) do {                                     \
  if (!typedarray_search_bigint(js, search, (T)-1 < 0, &ubits)) return -1; \
  TA_SEARCH_LOOP(T, (uint64_t)p[i] == ubits);                              \
} while (0)

static int64_t typedarray_search_data(
  ant_t *js, TypedArrayType type, const uint8_t *data,
  size_t start, size_t end, bool reverse,
  ant_value_t search, bool same_value_zero
) {
  int64_t bits = 0;
  uint64_t ubits = 0;

  #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, load, store) case TYPED_ARRAY_##name: TA_SEARCH_##kind(ctype, load);
  switch (type) {
    #include "buffer/typed_arrays.h"
    default: return -1;
  }
}

#undef TA_SEARCH_BIGINT
#undef TA_SEARCH_FLOAT
#undef TA_SEARCH_INT
#undef TA_SEARCH_LOOP
#undef TA_INT_MIN
#undef TA_INT_MAX

static ant_value_t typedarray_search(ant_native_params_t, ant_value_t call_this, typedarray_search_kind_t kind) {
  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  bool includes = kind == TA_SEARCH_INCLUDES;
  bool reverse = kind == TA_SEARCH_LAST_INDEX_OF;
  ant_value_t miss = includes ? Ant_Value_False : Ant_Value_FromNumber(-1);

  size_t len = ta_data->length;
  if (len == 0) return miss;

  double from = reverse ? (double)len - 1 : 0;
  if (nargs > 1) {
    ant_value_t status = typedarray_search_from_index(js, args[1], &from);
    if (is_err(status)) return status;
  }

  size_t start = 0, end = len;
  if (from < 0) from += (double)len;
  if (reverse) {
    if (from < 0) return miss;
    if (from < (double)len) end = (size_t)from + 1;
  } else {
    if (from >= (double)len) return miss;
    if (from > 0) start = (size_t)from;
  }

  ant_value_t search = nargs > 0 ? args[0] : Ant_Value_Undefined;
  uint8_t *data = NULL;
  size_t byte_length = 0, live_len = 0;

  ta_data = typedarray_live(call_this);
  if (Ant_Buffer_GetViewBytes(ta_data, &data, &byte_length)) live_len = ta_data->length;
  if (live_len < end) {
    if (includes && vtype(search) == kTypeUndefined) return Ant_Value_True;
    end = live_len;
  }

  int64_t found = start < end ? typedarray_search_data(
    js, ta_data->type, data, start, end, reverse, search, includes
  ) : -1;

  if (includes) return Ant_Value_FromBool(found >= 0);
  return Ant_Value_FromNumber((double)found);
}

static ant_value_t js_typedarray_indexOf(ant_params_t) {
  return typedarray_search(js, args, nargs, call_this, TA_SEARCH_INDEX_OF);
}

static ant_value_t js_typedarray_lastIndexOf(ant_params_t) {
  return typedarray_search(js, args, nargs, call_this, TA_SEARCH_LAST_INDEX_OF);
}

static ant_value_t js_typedarray_includes(ant_params_t) {
  return typedarray_search(js, args, nargs, call_this, TA_SEARCH_INCLUDES);
}

static ant_value_t js_typedarray_reverse(ant_params_t) {
  (void)args;
  (void)nargs;

  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

  for (size_t left = 0, right = ta_data->length; left < right && left < --right; left++) {
    ant_value_t a = Ant_Value_Undefined;
    ant_value_t b = Ant_Value_Undefined;
    if (!buffer_typedarray_data_read_index(js, ta_data, left, &a))
      return js_mkerr(js, "Failed to read from TypedArray");
    if (!buffer_typedarray_data_read_index(js, ta_data, right, &b))
      return js_mkerr(js, "Failed to read from TypedArray");
    ant_value_t write_b = typedarray_write_value(js, ta_data, left, b);
    if (is_err(write_b)) return write_b;
    ant_value_t write_a = typedarray_write_value(js, ta_data, right, a);
    if (is_err(write_a)) return write_a;
  }

  return call_this;
}

static ant_value_t js_typedarray_sort(ant_params_t) {
  TypedArrayData *ta_data = typedarray_live(call_this);
  if (!ta_data) return typedarray_this_error(js, call_this);

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
        ant_value_t cmp_val = sv_vm_call(js->vm, js, args[0], Ant_Value_Undefined, cmp_args, 2, NULL, Ant_Value_Undefined);
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
  return call_this;
}

void typedarray_define_methods(ant_t *js, ant_value_t typedarray_proto) {
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
}

void typedarray_define_iterators(ant_t *js, ant_value_t typedarray_proto) {
  js->builtins.typedarray_iter_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.typedarray_iter_proto, js->sym.iterator_proto);
  defmethod(js, js->builtins.typedarray_iter_proto, "next", 4, Ant_Function_Create(ta_iter_next));
  js_iter_register_advance(js, js->builtins.typedarray_iter_proto, advance_typedarray);

  defmethod(js, typedarray_proto, "values", 6, Ant_Function_Create(ta_values));
  defmethod(js, typedarray_proto, "keys", 4, Ant_Function_Create(ta_keys));
  defmethod(js, typedarray_proto, "entries", 7, Ant_Function_Create(ta_entries));
  defalias_sym_of(js, typedarray_proto, js->sym.iterator_sym, "values");
}
