#include <limits.h>
#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "ptr.h"
#include "arrays.h"
#include "keys.h"
#include "props.h"
#include "stringify.h"
#include "protectors.h"
#include "silver/call.h"
#include "modules/proxy.h"

bool is_array_index(const char *key, ant_offset_t klen) {
  if (klen == 0 || (klen > 1 && key[0] == '0')) return false;
  for (ant_offset_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
  }
  return true;
}

bool parse_array_index(const char *key, size_t klen, ant_offset_t max_len, unsigned long *out_idx) {
  if (
    klen == 0 || key[0] < '0' || key[0] > '9' ||
    (klen > 1 && key[0] == '0')
  ) return false;
  
  const size_t max_ulong_digits = sizeof(unsigned long) == 8 ? 20u : 10u;
  if (klen > max_ulong_digits) return false;

  unsigned long parsed_idx = 0;
  if (klen == max_ulong_digits) for (size_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    unsigned long digit = (unsigned long)(key[i] - '0');
    if (
      parsed_idx > ULONG_MAX / 10 ||
      (parsed_idx == ULONG_MAX / 10 && digit > ULONG_MAX % 10)
    ) return false;
    parsed_idx = parsed_idx * 10 + digit;
  }
  
  else for (size_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    parsed_idx = parsed_idx * 10 + (unsigned long)(key[i] - '0');
  }
  
  if (parsed_idx >= max_len) return false;
  *out_idx = parsed_idx;
  
  return true;
}

ant_object_t *array_obj_ptr(ant_value_t obj) {
  if (!is_object_type(obj)) return NULL;
  ant_object_t *ptr = js_obj_ptr(obj);
  return (ptr && ptr->type_tag == kTypeArray) ? ptr : NULL;
}

ant_object_t *real_array_obj_ptr(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  return ant_object_is_array(ptr) ? ptr : NULL;
}

bool array_dense_storage(const ant_object_t *ptr) {
  return ptr && ptr->flags.fast_array && !ptr->flags.is_exotic && ptr->u.array.data;
}

bool array_dense_covers(const ant_object_t *ptr, ant_offset_t n) {
  return array_dense_storage(ptr) && n <= ptr->u.array.len && n <= ptr->u.array.cap;
}

bool array_dense_read(const ant_object_t *ptr, ant_offset_t idx, ant_value_t *out) {
  if (!array_dense_storage(ptr) || idx >= ptr->u.array.len || idx >= ptr->u.array.cap) return false;
  ant_value_t value = ptr->u.array.data[idx];
  if (is_empty_slot(value)) return false;
  *out = value;
  return true;
}

bool array_may_have_holes(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  return ptr ? ptr->flags.may_have_holes : true;
}

bool array_may_have_dense_elements(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  return ptr ? ptr->flags.may_have_dense_elements : true;
}

void array_mark_may_have_holes(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  if (ptr) ptr->flags.may_have_holes = 1;
}

void array_mark_packed(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  if (
    ptr && ptr->flags.fast_array && ptr->u.array.data && 
    ptr->u.array.len <= ptr->u.array.cap
  ) ptr->flags.may_have_holes = 0;
}

void array_define_or_set_index(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (!key) return;
  if (!array_obj_ptr(obj)) return;

  unsigned long idx = 0;
  if (!parse_array_index(key, klen, ((ant_offset_t)UINT32_MAX), &idx)) return;

  ant_offset_t cur_len = get_array_length(js, obj);
  ant_offset_t next_len = (ant_offset_t)idx + 1;
  
  if (next_len > cur_len) {
    if ((ant_offset_t)idx > cur_len) array_mark_may_have_holes(obj);
    array_len_set(js, obj, next_len);
  }
}

ant_offset_t get_array_length(ant_t *js, ant_value_t arr) {
  if (!is_object_type(arr)) return 0;

  ant_object_t *arr_ptr = array_obj_ptr(arr);
  if (arr_ptr) return (ant_offset_t)arr_ptr->u.array.len;

  ant_value_t val = lkp_interned_val(js, arr, js->intern.length);
  if (vtype(val) == kTypeNumber) return (ant_offset_t) Ant_Value_AsNumberUnchecked(val);
  
  return 0;
}

static ant_offset_t to_length_offset(double d) {
  if (!(d > 0)) return 0;
  return d > 9007199254740991.0 ? (ant_offset_t)9007199254740991.0 : (ant_offset_t)d;
}

ant_value_t array_like_length_checked(ant_t *js, ant_value_t obj, ant_offset_t *out) {
  ant_object_t *ptr = real_array_obj_ptr(obj);
  *out = ptr ? (ant_offset_t)ptr->u.array.len : 0;
  if (ptr) return Ant_Value_Undefined;

  ant_value_t val = js_getprop_fallback(js, obj, "length");
  if (is_err(val)) return val;
  val = js_to_numeric(js, val);
  
  if (is_err(val)) return val;
  if (vtype(val) == kTypeBigInt)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
  
  *out = to_length_offset(Ant_Value_AsNumberUnchecked(val));
  return Ant_Value_Undefined;
}

static ant_offset_t array_like_length(ant_t *js, ant_value_t obj) {
  ant_object_t *ptr = real_array_obj_ptr(obj);
  
  if (ptr) return (ant_offset_t)ptr->u.array.len;
  if (!is_object_type(obj)) return 0;

  ant_value_t val = lkp_interned_val(js, obj, js->intern.length);
  uint8_t t = vtype(val);
  
  if (t == kTypeNumber) return to_length_offset(Ant_Value_AsNumberUnchecked(val));
  if (t == kTypeString || t == kTypeBool) return to_length_offset(js_to_number(js, val));
  
  return 0;
}

ant_offset_t get_dense_buf(ant_value_t arr) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(arr));
  if (!ptr || !ptr->flags.fast_array || !ptr->u.array.data || ptr->u.array.cap == 0) return 0;
  return (ant_offset_t)(uintptr_t)ptr;
}

ant_object_t *dense_obj(ant_offset_t doff) {
  if (!doff) return NULL;
  ant_object_t *ptr = (ant_object_t *)(uintptr_t)doff;
  if (!ptr || !ptr->flags.fast_array || !ptr->u.array.data || ptr->u.array.cap == 0) return NULL;
  return ptr;
}

ant_value_t *dense_data(ant_offset_t doff) {
  ant_object_t *ptr = dense_obj(doff);
  return ptr ? ptr->u.array.data : NULL;
}

ant_offset_t dense_capacity(ant_offset_t doff) {
  ant_object_t *ptr = dense_obj(doff);
  return ptr ? (ant_offset_t)ptr->u.array.cap : 0;
}

ant_value_t dense_get(ant_offset_t doff, ant_offset_t idx) {
  ant_object_t *ptr = dense_obj(doff);
  if (!ptr || idx >= ptr->u.array.cap) return Ant_Value_Undefined;
  return ptr->u.array.data[idx];
}

void dense_set(ant_t *js, ant_offset_t doff, ant_offset_t idx, ant_value_t val) {
  ant_object_t *ptr = dense_obj(doff);
  if (!ptr || idx >= ptr->u.array.cap) return;
  ptr->u.array.data[idx] = val;
  if (!is_empty_slot(val)) ptr->flags.may_have_dense_elements = 1;
  gc_write_barrier_elem(js, ptr, (uint32_t)idx, val);
}

ant_offset_t dense_grow(ant_t *js, ant_value_t arr, ant_offset_t needed) {
  ant_object_t *obj = js_obj_ptr(js_as_obj(arr));
  
  if (!obj) return 0;
  if (needed > UINT32_MAX) return 0;

  ant_offset_t old_cap = obj->u.array.cap;
  ant_offset_t new_cap = old_cap ? old_cap : MAX_DENSE_INITIAL_CAP;

  while (new_cap < needed) new_cap *= 2;
  ant_offset_t len = obj->u.array.len;
  if (len > new_cap && len / 8 <= new_cap) new_cap = len;
  if (new_cap > UINT32_MAX) new_cap = UINT32_MAX;
  if ((size_t)new_cap > SIZE_MAX / sizeof(ant_value_t)) return 0;

  ant_value_t *next = gc_array_storage_grow(js, obj->u.array.data, (uint32_t)old_cap, (uint32_t)new_cap);
  if (!next) return 0;
  
  gc_array_grew(js);
  for (ant_offset_t i = old_cap; i < new_cap; i++) next[i] = T_EMPTY;

  obj->u.array.data = next;
  obj->u.array.cap = (uint32_t)new_cap;
  obj->flags.dense_length_fits = obj->u.array.len <= obj->u.array.cap;
  obj->flags.fast_array = 1;
  
  return (ant_offset_t)(uintptr_t)obj;
}

// TODO: make get and set dry
ant_value_t arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_object_t *fast = array_obj_ptr(arr);
  
  if (
    fast && fast->flags.fast_array && fast->u.array.data &&
    idx < (ant_offset_t)fast->u.array.len &&
    idx < (ant_offset_t)fast->u.array.cap
  ) {
    ant_value_t v = fast->u.array.data[idx];
    if (!is_empty_slot(v)) return v;
  }

  ant_offset_t semantic_len = get_array_length(js, arr);
  ant_value_t cur = arr;
  
  if (idx < semantic_len) {
    ant_offset_t doff = get_dense_buf(arr);
    ant_value_t v = doff && idx < dense_iterable_length(js, arr) ? dense_get(doff, idx) : T_EMPTY;
    if (!is_empty_slot(v)) return v;
  } else if (!fast || !fast->flags.arguments_object) {
    if (fast && array_proto_chain_plain(js, fast)) return Ant_Value_Undefined;
    cur = get_proto(js, arr);
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);

  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    ant_value_t cur_obj = js_as_obj(cur);
    if (!array_obj_ptr(cur_obj)) goto shape_lookup;

    ant_offset_t cur_doff = get_dense_buf(cur_obj);
    if (!cur_doff) goto shape_lookup;

    ant_offset_t cur_len = dense_iterable_length(js, cur_obj);
    if (idx >= cur_len) goto shape_lookup;

    ant_value_t v = dense_get(cur_doff, idx);
    if (!is_empty_slot(v)) return v;

shape_lookup:;
    ant_prop_loc_t off = lkp(js, cur_obj, idxstr, idxlen);
    if (off.obj) {
      const ant_shape_prop_t *prop_meta = prop_shape_meta(off);
      if (prop_meta && prop_meta->has_getter) {
        ant_value_t accessor_result;
        if (try_accessor_getter(js, arr, idxstr, idxlen, &accessor_result))
          return accessor_result;
      }
      return js_prop_load(off);
    }

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    cur = proto;
  }

  return Ant_Value_Undefined;
}

void arr_set(ant_t *js, ant_value_t arr, ant_offset_t idx, ant_value_t val) {
  ant_offset_t cur_len = get_array_length(js, arr);
  ant_offset_t doff = get_dense_buf(arr);
  
  if (doff) {
    ant_offset_t len = dense_iterable_length(js, arr);
    
    if (idx < len) {
      dense_set(js, doff, idx, val);
      return;
    }
    
    ant_offset_t density_limit = len * 4 > ARRAY_DENSE_GAP ? len * 4 : ARRAY_DENSE_GAP;
    if (idx >= density_limit) goto sparse;
    
    ant_offset_t cap = dense_capacity(doff);
    ant_offset_t stale_end = idx < cap ? idx : cap;
    
    if (idx >= cap) {
      doff = dense_grow(js, arr, idx + 1);
      if (doff == 0) goto sparse;
    }
    
    if (idx > len) {
      array_mark_may_have_holes(arr);
      ant_value_t *data = dense_data(doff);
      for (ant_offset_t i = len; i < stale_end; i++)
        if (vtype(data[i]) == kTypeUndefined) data[i] = T_EMPTY;
    }
    
    dense_set(js, doff, idx, val);
    if (idx >= cur_len) array_len_set(js, arr, idx + 1);
    
    return;
  }
  
  sparse:;
  char idxstr[24];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
  ant_value_t key = js_mkstr(js, idxstr, idxlen);
  
  js_setprop(js, arr, key, val);
}

bool arr_own_dense_element(
  ant_t *js, ant_value_t arr, const char *key, size_t klen, ant_value_t *out
) {
  unsigned long idx;
  ant_offset_t doff = get_dense_buf(arr);
  if (!doff || !parse_array_index(key, klen, dense_iterable_length(js, arr), &idx)) return false;
  ant_value_t v = dense_get(doff, (ant_offset_t)idx);
  if (is_empty_slot(v)) return false;
  if (out) *out = v;
  return true;
}

bool arr_has(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_offset_t semantic_len = get_array_length(js, arr);
  if (idx >= semantic_len) return false;
  ant_offset_t doff = get_dense_buf(arr);
  
  if (doff) {
    ant_offset_t len = dense_iterable_length(js, arr);
    if (idx < len) {
      ant_value_t v = dense_get(doff, idx);
      if (!is_empty_slot(v)) return true;
    }
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);

  ant_value_t cur = arr;
  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    ant_value_t cur_obj = js_as_obj(cur);
    if (!array_obj_ptr(cur_obj)) goto shape_has;

    ant_offset_t cur_doff = get_dense_buf(cur_obj);
    if (!cur_doff) goto shape_has;

    ant_offset_t cur_len = dense_iterable_length(js, cur_obj);
    if (idx >= cur_len) goto shape_has;
    if (!is_empty_slot(dense_get(cur_doff, idx))) return true;

shape_has:;
    if (lkp(js, cur_obj, idxstr, idxlen).obj) return true;

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    cur = proto;
  }

  return false;
}

bool array_result_extensible(ant_value_t result) {
  ant_object_t *ptr = is_object_type(result) ? js_obj_ptr(js_as_obj(result)) : NULL;
  return !ptr || ptr->flags.extensible;
}

static __attribute__((noinline)) ant_value_t array_result_set_checked(ant_t *js, ant_value_t result, ant_offset_t idx, ant_value_t val) {
  if (!array_result_extensible(result)) {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(result));
    bool has = arr_has(js, result, idx);
    if (ptr->flags.sealed && has)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine property: %u", (unsigned)idx);
    if (!has)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot add property %u, object is not extensible", (unsigned)idx);
  }
  
  arr_set(js, result, idx, val);
  return Ant_Value_Undefined;
}

ant_value_t array_result_set(ant_t *js, ant_value_t result, bool fresh, ant_offset_t idx, ant_value_t val) {
  if (__builtin_expect(!fresh, 0)) return array_result_set_checked(js, result, idx, val);
  arr_set(js, result, idx, val);
  return Ant_Value_Undefined;
}

ant_value_t alloc_array_with_proto_capacity(
  ant_t *js, ant_value_t proto, uint32_t minimum_capacity,
  uint32_t overwritten_prefix, bool exact_capacity
) {
  ant_object_t *obj = obj_alloc_with_proto(js, kTypeArray, proto);
  if (!obj) return js_mkerr(js, "oom");
  
  ant_value_t arr = mkref(kTypeArray, obj);
  uint32_t capacity = exact_capacity
    ? (minimum_capacity ? minimum_capacity : 1u) 
    : MAX_DENSE_INITIAL_CAP;
  
  while (capacity < minimum_capacity && capacity <= UINT32_MAX / 2) capacity *= 2;
  if (capacity < minimum_capacity) capacity = minimum_capacity;
  if ((size_t)capacity > SIZE_MAX / sizeof(*obj->u.array.data)) return js_mkerr(js, "oom");

  obj->u.array.cap = capacity;
  obj->u.array.len = 0;
  obj->u.array.data = gc_array_storage_alloc(js, obj->u.array.cap);
  
  if (obj->u.array.data) {
    gc_array_grew(js);
    ant_value_t *data = obj->u.array.data;
    size_t fill_start = overwritten_prefix < capacity ? overwritten_prefix : capacity;
    for (size_t i = fill_start; i < capacity; i++) data[i] = T_EMPTY;
    obj->flags.fast_array = 1;
    obj->flags.dense_length_fits = 1;
    obj->flags.may_have_holes = 0;
    obj->flags.may_have_dense_elements = 0;
  } else {
    obj->u.array.cap = 0;
    obj->u.array.len = 0;
    obj->flags.fast_array = 0;
    obj->flags.may_have_holes = 1;
    obj->flags.may_have_dense_elements = 1;
  }

  return arr;
}

ant_value_t Ant_Array_Create(ant_t *js) {
  return alloc_array_with_proto_capacity(js, js->sym.array_proto, MAX_DENSE_INITIAL_CAP, 0, false);
}

ant_value_t mkarr_dense_literal_capacity(ant_t *js, const ant_value_t *elements, uint32_t count, bool exact_capacity) {
  uint32_t capacity = exact_capacity ? gc_array_storage_fit(count) : count;
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, capacity, count, exact_capacity);
  if (is_err(arr)) return arr;

  ant_object_t *obj = array_obj_ptr(arr);
  if (!obj || !obj->flags.fast_array || !obj->u.array.data || obj->u.array.cap < count) {
    if (obj && obj->u.array.data) {
      uint32_t reset_count = count < obj->u.array.cap ? count : obj->u.array.cap;
      for (uint32_t i = 0; i < reset_count; i++) obj->u.array.data[i] = T_EMPTY;
    }
    
    for (uint32_t i = 0; i < count; i++) js_arr_push(js, arr, elements[i]);
    return arr;
  }

  bool has_holes = false;
  bool has_elements = false;
  
  for (uint32_t i = 0; i < count; i++) {
    ant_value_t value = elements[i];
    obj->u.array.data[i] = value;
    if (is_empty_slot(value)) has_holes = true;
    else has_elements = true;
  }
  
  obj->u.array.len = count;
  obj->flags.may_have_holes = has_holes;
  obj->flags.may_have_dense_elements = has_elements;
  
  return arr;
}

ant_value_t js_mkarr_dense_literal(ant_t *js, const ant_value_t *elements, uint32_t count) {
  return mkarr_dense_literal_capacity(js, elements, count, false);
}

ant_value_t js_mkarr_dense_uninit(ant_t *js, uint32_t count, ant_value_t **out_data) {
  *out_data = NULL;
  if (count == 0) return Ant_Array_Create(js);
  
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, gc_array_storage_fit(count), count, true);
  if (is_err(arr)) return arr;

  ant_object_t *obj = array_obj_ptr(arr);
  if (!obj || !obj->flags.fast_array || !obj->u.array.data || obj->u.array.cap < count)
    return js_mkerr(js, "oom");

  obj->u.array.len = count;
  obj->flags.may_have_dense_elements = 1;
  *out_data = obj->u.array.data;
  
  return arr;
}

void arr_del(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_offset_t semantic_len = get_array_length(js, arr);
  if (idx >= semantic_len) return;
  
  array_mark_may_have_holes(arr);
  ant_offset_t doff = get_dense_buf(arr);
  
  if (doff) {
    ant_offset_t len = dense_iterable_length(js, arr);
    if (idx < len) dense_set(js, doff, idx, T_EMPTY);
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
  
  js_delete_prop(js, arr, idxstr, idxlen);
}

bool js_array_iteration_default(ant_t *js, ant_value_t arr) {
  ant_object_t *ptr = array_obj_ptr(arr);
  if (!ptr || vtype(js->sym.iterator_sym) != kTypeSymbol) return false;
  
  if (
    ant_protector_intact(js, ANT_PROTECTOR_ARRAY_ITERATION) && 
    (ptr->flags.arguments_object || ant_object_proto(ptr) == js->sym.array_proto)
  ) return true;
  
  ant_prop_loc_t loc = lkp_sym_proto(js, arr, (ant_offset_t)vdata(js->sym.iterator_sym));
  if (!loc.obj) return false;
  
  const ant_shape_prop_t *meta = prop_shape_meta(loc);
  if (meta && (meta->has_getter || meta->has_setter)) return false;
  if (js_prop_load(loc) != js->sym.array_values_fn) return false;
  if (vtype(js->sym.array_iterator_proto) != kTypeObject) return true;
  
  ant_value_t next;
  return
    js_try_get_own_data_prop(js, js->sym.array_iterator_proto, "next", 4, &next) &&
    next == js->sym.array_iterator_next;
}

ant_value_t js_arr_spread_into(ant_t *js, ant_value_t dst, ant_value_t src) {
  ant_offset_t len = 0;
  ant_value_t len_result = array_like_length_checked(js, src, &len);
  if (is_err(len_result)) return len_result;
  
  ant_offset_t base = get_array_length(js, dst);
  ant_offset_t doff = get_dense_buf(dst);
  if (doff && real_array_obj_ptr(dst) && dense_capacity(doff) < base + len)
    doff = dense_grow(js, dst, base + len);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t value;
    if (!array_method_dense_element(src, i, &value)) {
      value = array_method_get_index(js, src, i);
      if (is_err(value)) return value;
      len_result = array_like_length_checked(js, src, &len);
      if (is_err(len_result)) return len_result;
    }
    
    if (doff && base + i < dense_capacity(doff) && base + i == get_array_length(js, dst)) {
      dense_set(js, doff, base + i, value);
      array_len_set(js, dst, base + i + 1);
    } else js_arr_push(js, dst, value);
  }
  
  return Ant_Value_Undefined;
}

ant_offset_t js_arr_len(ant_t *js, ant_value_t arr) {
  if (!array_obj_ptr(arr)) {
    if (is_proxy(arr)) {
      ant_value_t length = js_get(js, arr, "length");
      if (vtype(length) == kTypeNumber) return (ant_offset_t)Ant_Value_AsNumberUnchecked(length);
    }
    return 0;
  }
  return array_like_length(js, arr);
}

ant_value_t js_arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  if (is_proxy(arr)) {
    char idxstr[32];
    uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
    return js_get(js, arr, idxstr);
  }
  if (vtype(arr) != kTypeArray) return Ant_Value_Undefined;
  return arr_get(js, arr, idx);
}

static ant_value_t array_constructor_from_receiver(ant_t *js, ant_value_t receiver) {
  if (!is_object_type(receiver)) return Ant_Value_Undefined;
  ant_object_t *plain = real_array_obj_ptr(receiver);
  
  if (
    plain && ant_object_proto(plain) == js->sym.array_proto && 
    ant_protector_intact(js, ANT_PROTECTOR_ARRAY_SPECIES)
  ) return js->sym.array_ctor;
  
  ant_value_t species_source = receiver;
  if (is_proxy(species_source)) species_source = proxy_read_target(js, species_source);
  
  bool receiver_is_array = real_array_obj_ptr(species_source) != NULL;
  if (!receiver_is_array) {
    ant_value_t array_proto = js->sym.array_proto;
    if (is_object_type(array_proto) && is_object_type(species_source))
      receiver_is_array = proto_chain_contains(js, species_source, array_proto);
  }
  if (!receiver_is_array) return Ant_Value_Undefined;

  ant_value_t ctor = js_getprop_fallback(js, receiver, "constructor");
  if (is_err(ctor)) return ctor;
  if (ant_realm_foreign_array_ctor(js, ctor)) return Ant_Value_Undefined;

  if (is_object_type(ctor) || vtype(ctor) == kTypeBuiltin) {
    ctor = get_ctor_species_value(js, ctor);
    if (is_err(ctor)) return ctor;
    if (vtype(ctor) == kTypeNull) return Ant_Value_Undefined;
  }
  
  if (vtype(ctor) == kTypeUndefined) return ctor;
  if (!js_is_constructor(ctor))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object.constructor[Symbol.species] is not a constructor");
  
  return ctor;
}

static constexpr ant_offset_t ARRAY_RESULT_PREALLOC_MAX = 1024;

ant_value_t array_alloc_intrinsic(ant_t *js, ant_value_t source, ant_offset_t length_hint) {
  if (length_hint == 0) return Ant_Array_Create(js);
  
  ant_object_t *src = array_obj_ptr(source);
  bool exact = length_hint <= ARRAY_RESULT_PREALLOC_MAX ||
    (src && src->flags.fast_array && length_hint <= src->u.array.cap);
  
  ant_value_t arr = exact && length_hint <= UINT32_MAX
    ? alloc_array_with_proto_capacity(js, js->sym.array_proto, (uint32_t)length_hint, 0, false)
    : Ant_Array_Create(js);
  
  if (is_err(arr)) return arr;
  array_len_set(js, arr, length_hint);
  array_mark_may_have_holes(arr);
  
  return arr;
}

static ant_value_t array_construct_from_ctor(ant_t *js, ant_value_t ctor, ant_offset_t length_hint) {
  ant_value_t seed = Ant_Object_CreateBare(js);
  if (is_err(seed)) return seed;

  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t ctor_args[1] = { Ant_Value_FromNumber((double)length_hint) };
  ant_value_t constructed = sv_vm_call(js->vm, js, ctor, seed, ctor_args, 1, NULL, ctor);
  if (is_err(constructed)) return constructed;

  ant_value_t result = is_object_type(constructed) ? constructed : seed;
  js_set_slot_wb(js, js_as_obj(result), SLOT_CTOR, ctor);
  
  return result;
}

ant_value_t array_alloc_from_ctor_with_length(ant_t *js, ant_value_t ctor, ant_offset_t length_hint) {
  if (vtype(ctor) != kTypeFunction && vtype(ctor) != kTypeBuiltin) return Ant_Array_Create(js);
  if (same_ctor_identity(js, ctor, js->sym.array_ctor)) return array_alloc_intrinsic(js, Ant_Value_Undefined, length_hint);
  return array_construct_from_ctor(js, ctor, length_hint);
}

ant_value_t array_species_create(ant_t *js, ant_value_t receiver, ant_offset_t length, bool *intrinsic) {
  ant_value_t ctor = array_constructor_from_receiver(js, receiver);
  if (is_err(ctor)) return ctor;
  
  *intrinsic = vtype(ctor) == kTypeUndefined || same_ctor_identity(js, ctor, js->sym.array_ctor);
  if (*intrinsic) return array_alloc_intrinsic(js, receiver, length);
  return array_alloc_from_ctor_with_length(js, ctor, length);
}

__attribute__((noinline)) bool array_length_readonly_slow(ant_t *js, ant_value_t arr) {
  descriptor_entry_t *desc = lookup_descriptor(js, js_as_obj(arr), "length", 6);
  return desc && !desc->writable;
}

bool array_length_readonly(ant_t *js, ant_value_t arr) {
  ant_object_t *ptr = real_array_obj_ptr(arr);
  if (__builtin_expect(!ptr || !(ptr->flags.raw & (ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_EXOTIC)), 1)) return false;
  return array_length_readonly_slow(js, arr);
}

ant_value_t array_length_readonly_error(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property 'length' of object '[object Array]'");
}

ant_value_t set_declined(bool *declined, ant_value_t value) {
  if (declined) *declined = true;
  return value;
}

__attribute__((noinline, cold)) ant_value_t array_length_write_ignored(ant_t *js, ant_value_t value, bool *declined) {
  return sv_is_strict_context(js) ? array_length_readonly_error(js) : set_declined(declined, value);
}

ant_value_t array_grow_readonly_result(ant_t *js, uint64_t idx, ant_value_t value, bool *declined) {
  if (!sv_is_strict_context(js)) return set_declined(declined, value);
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot add property %llu, object is not extensible", (unsigned long long)idx);
}

ant_value_t validate_array_length(ant_t *js, ant_value_t v) {
  if (vtype(v) != kTypeNumber)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  double d = Ant_Value_AsNumberUnchecked(v);
  if (d < 0 || d != (uint32_t)d || d >= 4294967296.0)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  return Ant_Value_Undefined;
}

static inline bool object_refuses_new_props(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  return ptr && (ptr->flags.frozen || ptr->flags.sealed || !ptr->flags.extensible);
}

static __attribute__((noinline, cold)) ant_value_t not_extensible_error(ant_t *js, const char *key, size_t len) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot add property %.*s, object is not extensible", (int)len, key);
}

ant_value_t check_object_extensibility(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  if (!object_refuses_new_props(obj)) return Ant_Value_Undefined;
  return sv_is_strict_context(js) ? not_extensible_error(js, key, len) : Ant_Value_False;
}

ant_value_t check_object_extensibility_index(ant_t *js, ant_value_t obj, uint64_t idx) {
  if (!object_refuses_new_props(obj)) return Ant_Value_Undefined;
  if (!sv_is_strict_context(js)) return Ant_Value_False;
  
  char buf[24];
  size_t len = uint_to_str(buf, sizeof(buf), idx);
  
  return not_extensible_error(js, buf, len);
}

ant_value_t check_object_extensibility_symbol(ant_t *js, ant_value_t obj, ant_value_t sym) {
  if (!object_refuses_new_props(obj)) return Ant_Value_Undefined;
  if (!sv_is_strict_context(js)) return Ant_Value_False;
  
  ant_value_t text = js_symbol_to_string(js, sym);
  if (is_err(text)) return text;
  
  size_t len = 0;
  const char *bytes = js_getstr(js, text, &len);
  
  return not_extensible_error(js, bytes, len);
}

void array_len_set(ant_t *js, ant_value_t obj, ant_offset_t new_len) {
  ant_object_t *arr_ptr = array_obj_ptr(obj);
  if (arr_ptr) {
    if (new_len > (ant_offset_t)UINT32_MAX) new_len = (ant_offset_t)UINT32_MAX;
    arr_ptr->u.array.len = (uint32_t)new_len;
    arr_ptr->flags.dense_length_fits = new_len <= arr_ptr->u.array.cap;
    return;
  }

  ant_value_t new_len_val = Ant_Value_FromNumber((double)new_len);
  ant_prop_loc_t len_off = lkp_interned(obj, js->intern.length);
  
  if (len_off.obj) js_prop_store(js, len_off, new_len_val);
  else js_mkprop_fast(js, obj, "length", 6, new_len_val);
}

bool array_length_has_guarded_elements(ant_object_t *ptr) {
  return 
    ptr->flags.sealed || 
    ptr->flags.frozen || 
    !ptr->shape || ant_shape_may_have_index_keys(ptr->shape);
}

void array_length_set_unguarded(ant_t *js, ant_value_t obj, ant_offset_t new_len) {
  ant_offset_t doff = get_dense_buf(obj);
  ant_offset_t cur_len = get_array_length(js, obj);
  
  if (doff) {
    ant_offset_t cap = dense_capacity(doff);
    ant_offset_t clear_to = (cur_len < cap) ? cur_len : cap;
    if (new_len < clear_to) 
      for (ant_offset_t i = new_len; i < clear_to; i++) dense_set(js, doff, i, T_EMPTY);
  }
  
  if (new_len > cur_len) array_mark_may_have_holes(obj);
  array_len_set(js, obj, new_len);
}

typedef enum {
  PROTO_INDEX_ABSENT,
  PROTO_INDEX_WRITABLE,
  PROTO_INDEX_INTERCEPTS,
} proto_index_property_t;

static proto_index_property_t proto_index_property(ant_t *js, ant_value_t proto, uint64_t idx) {
  if (is_proxy(proto)) return PROTO_INDEX_INTERCEPTS;
  ant_value_t proto_obj = js_as_obj(proto);
  ant_object_t *ptr = js_obj_ptr(proto_obj);

  char key[24];
  size_t klen = uint_to_str(key, sizeof(key), idx);
  const char *interned = intern_find(js, key, klen);
  int32_t slot = interned && ptr->shape ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
  if (slot >= 0) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    return !prop || prop->has_getter || prop->has_setter || !(prop->attrs & ANT_PROP_ATTR_WRITABLE)
      ? PROTO_INDEX_INTERCEPTS : PROTO_INDEX_WRITABLE;
  }

  descriptor_entry_t *desc = ptr->flags.is_exotic ? lookup_descriptor(js, proto_obj, key, klen) : NULL;
  if (!desc) return PROTO_INDEX_ABSENT;
  return desc->has_getter || desc->has_setter || !desc->writable
    ? PROTO_INDEX_INTERCEPTS : PROTO_INDEX_WRITABLE;
}

__attribute__((noinline)) bool array_store_reaches_proto_slow(ant_t *js, ant_value_t arr, uint64_t idx) {
  ant_value_t cur = get_proto(js, arr);

  for (int depth = 0; is_object_type(cur); depth++) {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
    if (!ptr || depth >= MAX_PROTO_CHAIN_DEPTH) return true;
    if (ptr->flags.is_exotic || ant_shape_may_have_index_keys(ptr->shape)) {
      proto_index_property_t found = proto_index_property(js, cur, idx);
      if (found != PROTO_INDEX_ABSENT) return found == PROTO_INDEX_INTERCEPTS;
    }
    ant_value_t next = ant_object_proto(ptr);
    cur = is_object_type(next) || vtype(next) == kTypeNull ? next : get_proto(js, cur);
  }

  return false;
}

static __attribute__((noinline)) bool array_chain_revalidate(ant_t *js) {
  ant_object_t *array_proto = (ant_object_t *)vptr(js->sym.array_proto);
  ant_object_t *object_proto = (ant_object_t *)vptr(js->sym.object_proto);
  bool plain =
    ant_object_proto(array_proto) == js->sym.object_proto && ant_object_proto(object_proto) == mkval(kTypeNull, 0) &&
    !((array_proto->flags.raw | object_proto->flags.raw) & ANT_OBJECT_FLAG_EXOTIC) &&
    !ant_shape_may_have_index_keys(array_proto->shape) &&
    !ant_shape_may_have_index_keys(object_proto->shape);
  
  if (plain) {
    ant_object_guard_absence(array_proto);
    ant_object_guard_absence(object_proto);
    js->array_chain_plain_epoch = js->ic.epoch;
  }
  
  return plain;
}

bool array_proto_chain_plain(ant_t *js, ant_object_t *arr) {
  if (ant_object_proto(arr) != js->sym.array_proto) return false;
  if (((ant_object_t *)vptr(js->sym.array_proto))->u.array.len != 0) return false;
  return js->array_chain_plain_epoch == js->ic.epoch || array_chain_revalidate(js);
}

bool js_array_proto_chain_plain(ant_t *js, ant_value_t arr) {
  ant_object_t *ptr = vtype(arr) == kTypeArray ? js_obj_ptr(arr) : NULL;
  return ptr && array_proto_chain_plain(js, ptr);
}

static inline bool arguments_proto_chain_plain(ant_t *js, ant_object_t *args) {
  if (ant_object_proto(args) != js->sym.object_proto) return false;
  ant_object_t *object_proto = (ant_object_t *)vptr(js->sym.object_proto);
  return
    ant_object_proto(object_proto) == mkval(kTypeNull, 0) && !object_proto->flags.is_exotic &&
    !ant_shape_may_have_index_keys(object_proto->shape);
}

bool array_elements_proto_plain(ant_t *js, ant_object_t *ptr) {
  return ptr->flags.arguments_object ? arguments_proto_chain_plain(js, ptr) : array_proto_chain_plain(js, ptr);
}

ant_object_t *array_dense_source(ant_t *js, ant_value_t arr) {
  if (is_proxy(arr)) return NULL;
  ant_object_t *ptr = array_obj_ptr(arr);
  return array_dense_storage(ptr) && array_elements_proto_plain(js, ptr) ? ptr : NULL;
}

bool array_search_eq(ant_t *js, ant_value_t elem, ant_value_t search) {
  if (vtype(search) == kTypeNumber) return vtype(elem) == kTypeNumber && Ant_Value_AsNumberUnchecked(elem) == Ant_Value_AsNumberUnchecked(search);
  if (elem == search) return true;
  return vtype(elem) == vtype(search) && strict_eq_values(js, elem, search);
}

bool array_store_reaches_proto(ant_t *js, ant_value_t arr, ant_object_t *ptr, uint64_t idx) {
  return !array_proto_chain_plain(js, ptr) && array_store_reaches_proto_slow(js, arr, idx);
}

const ant_value_t *Ant_Array_DenseView(ant_value_t arr, int *len_out) {
  static const ant_value_t empty[1];
  ant_object_t *ptr = real_array_obj_ptr(arr);
  if (!ptr || !ptr->flags.fast_array || ptr->flags.is_exotic) return NULL;

  ant_offset_t len = ptr->u.array.len;
  if (len == 0) { *len_out = 0; return empty; }
  if (!ptr->u.array.data) return NULL;
  if (len > ptr->u.array.cap || len > (ant_offset_t)INT_MAX) return NULL;

  const ant_value_t *src = ptr->u.array.data;
  for (ant_offset_t i = 0; i < len; i++)
    if (is_empty_slot(src[i])) return NULL;

  *len_out = (int)len;
  return src;
}

ant_value_t Ant_ArgList_FromArrayLike(ant_t *js, ant_value_t array_like, ant_arg_list_t *list) {
  int dense_len = 0;
  const ant_value_t *dense = Ant_Array_DenseView(array_like, &dense_len);
  list->rooted = false;
  list->args = list->inline_args;
  list->argc = 0;

  if (dense && dense_len <= ANT_ARG_LIST_INLINE) {
    memcpy(list->inline_args, dense, (size_t)dense_len * sizeof(ant_value_t));
    list->argc = dense_len;
    return Ant_Value_Undefined;
  }

  gc_temp_root_scope_begin(js, &list->roots);
  list->rooted = true;

  if (dense) {
    ant_value_t *slots = gc_temp_root_slots(&list->roots, (size_t)dense_len);
    if (!slots) { Ant_ArgList_Release(list); return Ant_Error_CreateOutOfMemory(js); }
    memcpy(slots, dense, (size_t)dense_len * sizeof(ant_value_t));
    list->args = slots;
    list->argc = dense_len;
    return Ant_Value_Undefined;
  }

  ant_offset_t len = 0;
  ant_value_t result = Ant_Value_Undefined;
  
  if (real_array_obj_ptr(array_like)) len = get_array_length(js, array_like);
  else result = proxy_aware_length(js, array_like, &len);
  if (is_err(result)) goto fail;

  if (len > (ant_offset_t)INT_MAX) {
    result = js_mkerr_typed(js, JS_ERR_RANGE, "Argument list length exceeds engine limit");
    goto fail;
  }

  ant_value_t *args = gc_temp_root_slots(&list->roots, (size_t)len);
  if (!args) { result = Ant_Error_CreateOutOfMemory(js); goto fail; }

  for (ant_offset_t i = 0; i < len; i++) {
    if (array_method_dense_element(array_like, i, &args[i])) continue;
    args[i] = array_method_get_index(js, array_like, i);
    if (is_err(args[i])) { result = args[i]; goto fail; }
  }

  list->args = args;
  list->argc = (int)len;
  return Ant_Value_Undefined;

fail:
  Ant_ArgList_Release(list);
  return result;
}

void Ant_ArgList_Release(ant_arg_list_t *list) {
  if (list->rooted) gc_temp_root_scope_end(&list->roots);
  list->rooted = false;
  list->args = NULL;
  list->argc = 0;
}

bool own_key_is_array_index(const char *key, size_t key_len, unsigned long *out_idx) {
  if (key_len == 0) return false;
  if (key[0] < '0' || key[0] > '9') return false;
  if (key_len > 1 && key[0] == '0') return false;
  if (key_len == 1) {
    if (out_idx) *out_idx = (unsigned long)(key[0] - '0');
    return true;
  }
  if (key_len > 10) return false;

  unsigned long parsed = (unsigned long)(key[0] - '0');
  for (size_t i = 1; i < key_len; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    unsigned digit = (unsigned)(key[i] - '0');
    if (parsed > ((unsigned long)UINT32_MAX - digit) / 10UL) return false;
    parsed = parsed * 10UL + digit;
  }
  if (parsed >= (unsigned long)UINT32_MAX) return false;
  if (out_idx) *out_idx = parsed;
  return true;
}

static ant_value_t array_delete_shape_indices(ant_t *js, ant_value_t obj, ant_object_t *ptr, ant_offset_t new_len, ant_offset_t *final_len) {
  own_index_list_t indices;
  own_index_list_init(&indices);
  
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    unsigned long index = 0;
    
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (!own_key_is_array_index(prop->key.interned, intern_length(prop->key.interned), &index)) continue;
    if ((ant_offset_t)index < new_len) continue;
    
    if (!own_index_list_push(&indices, i, index)) {
      own_index_list_free(&indices);
      return Ant_Error_CreateOutOfMemory(js);
    }
  }
  
  if (!indices.count) {
    own_index_list_free(&indices);
    return Ant_Value_Undefined;
  }
  
  own_index_keys_sort(indices.items, indices.count);
  const char **keys = malloc(indices.count * sizeof(*keys));
  
  if (!keys) {
    own_index_list_free(&indices);
    return Ant_Error_CreateOutOfMemory(js);
  }
  
  for (uint32_t k = 0; k < indices.count; k++) 
    keys[k] = ant_shape_prop_at(shape, indices.items[k].slot)->key.interned;
  
  ant_value_t result = Ant_Value_Undefined;
  for (uint32_t k = indices.count; k-- > 0;) {
    ant_offset_t index = (ant_offset_t)indices.items[k].index;
    if (index < *final_len) break;
    ant_value_t deleted = js_delete_prop_ordinary(js, obj, keys[k], intern_length(keys[k]));
    if (is_err(deleted)) { result = deleted; break; }
    if (deleted == Ant_Value_False) { *final_len = index + 1; break; }
  }
  
  free(keys);
  own_index_list_free(&indices);
  
  return result;
}

static ant_offset_t dense_filled_end(ant_offset_t doff, ant_offset_t end, ant_offset_t floor) {
  while (end > floor && is_empty_slot(dense_get(doff, end - 1))) end--;
  return end;
}

ant_value_t array_truncate_length(ant_t *js, ant_value_t obj, ant_offset_t new_len, ant_offset_t *out_len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  ant_offset_t cur_len = get_array_length(js, obj);
  ant_offset_t doff = get_dense_buf(obj);
  
  ant_offset_t dense_end = 0;
  ant_offset_t final_len = new_len;
  
  if (doff) {
    ant_offset_t cap = dense_capacity(doff);
    dense_end = cur_len < cap ? cur_len : cap;
  }
  
  bool shrinking = new_len < cur_len && ptr && ptr->shape;
  if (shrinking && doff && (ptr->flags.sealed || ptr->flags.frozen))
    final_len = dense_filled_end(doff, dense_end, new_len);

  if (shrinking && ant_shape_may_have_index_keys(ptr->shape)) {
    ant_value_t deleted = array_delete_shape_indices(js, obj, ptr, new_len, &final_len);
    if (is_err(deleted)) return deleted;
  }
  
  doff = get_dense_buf(obj);
  if (doff) {
    ant_offset_t cap = dense_capacity(doff);
    if (dense_end > cap) dense_end = cap;
    for (ant_offset_t i = final_len; i < dense_end; i++) dense_set(js, doff, i, T_EMPTY);
  }
  
  if (final_len > cur_len) array_mark_may_have_holes(obj);
  array_len_set(js, obj, final_len);
  *out_len = final_len;
  
  return Ant_Value_Undefined;
}

ant_value_t array_truncate_stopped_error(ant_t *js, ant_offset_t final_len) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot delete property '%llu' of [object Array]", (unsigned long long)(final_len - 1));
}

bool arr_fill_numeric_typed(ant_t *js, ant_value_t arr, const TypedArrayData *ta) {
  if (ta->type == TYPED_ARRAY_BIGINT64 || ta->type == TYPED_ARRAY_BIGUINT64) return false;
  
  ant_object_t *dst = array_obj_ptr(arr);
  
  if (
    !dst || !dst->flags.fast_array || !dst->u.array.data || 
    dst->u.array.len || dst->u.array.cap < ta->length
  ) return false;
  
  for (size_t i = 0; i < ta->length; i++) 
    buffer_typedarray_data_read_index(js, ta, i, &dst->u.array.data[i]);
  
  dst->u.array.len = (uint32_t)ta->length;
  dst->flags.may_have_dense_elements = ta->length > 0;
  
  return true;
}

bool arr_fill_dense_values(ant_value_t arr, ant_offset_t doff, ant_offset_t len) {
  ant_object_t *dst = array_obj_ptr(arr);
  ant_object_t *from = dense_obj(doff);
  
  if (
    !dst || !from || !dst->flags.fast_array || !dst->u.array.data || 
    dst->u.array.len || dst->u.array.cap < len
  ) return false;
  
  uint32_t count = 0;
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t v = from->u.array.data[i];
    if (!is_empty_slot(v)) dst->u.array.data[count++] = v;
  }
  
  dst->u.array.len = count;
  dst->flags.may_have_dense_elements = count > 0;
  
  return true;
}

object_fill_t arr_fill_object_values(ant_t *js, ant_object_t *ptr, ant_value_t *out, uint32_t *resume) {
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  if (count > ptr->prop_count) count = ptr->prop_count;
  
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, count, 0, false);
  ant_object_t *dst = is_err(arr) ? NULL : array_obj_ptr(arr);
  if (!dst || !dst->flags.fast_array || !dst->u.array.data) return OBJECT_FILL_NONE;
  
  own_index_list_t indices;
  own_index_list_init(&indices);
  
  bool index_keys = ant_shape_may_have_index_keys(shape);
  ant_value_t *data = dst->u.array.data;
  uint32_t n = 0;
  
  object_fill_t result = OBJECT_FILL_DONE;
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || !(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) continue;
    
    if (prop->has_getter || prop->has_setter) {
      if (index_keys) goto fail;
      result = OBJECT_FILL_RESUME;
      *resume = i;
      break;
    }
    
    unsigned long index = 0;
    const char *key = prop->key.interned;
    
    if (!index_keys || !own_key_is_array_index(key, intern_length(key), &index)) data[n++] = ant_object_prop_get_unchecked(ptr, i);
    else if (!own_index_list_push(&indices, i, index)) goto fail;
  }
  
  if (indices.count) {
    own_index_keys_sort(indices.items, indices.count);
    memmove(data + indices.count, data, n * sizeof(*data));
    for (uint32_t k = 0; k < indices.count; k++) data[k] = ant_object_prop_get_unchecked(ptr, indices.items[k].slot);
    n += indices.count;
  }
  
  own_index_list_free(&indices);
  dst->u.array.len = n;
  dst->flags.may_have_dense_elements = n > 0;
  *out = arr;
  return result;
  
fail:
  own_index_list_free(&indices);
  return OBJECT_FILL_NONE;
}

void js_arr_reserve(ant_t *js, ant_value_t arr, uint32_t n) {
  if (vtype(arr) != kTypeArray && vtype(arr) != kTypeObject) return;
  ant_offset_t doff = get_dense_buf(arr);
  if (!doff) return;
  if (n > dense_capacity(doff)) dense_grow(js, arr, n);
}

void js_arr_push(ant_t *js, ant_value_t arr, ant_value_t val) {
  if (vtype(arr) != kTypeArray && vtype(arr) != kTypeObject) return;
  
  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    ant_offset_t len = get_array_length(js, arr);
    ant_offset_t cap = dense_capacity(doff);
    if (len >= cap) {
      doff = dense_grow(js, arr, len + 1);
      if (doff == 0) return;
    }
    if (is_empty_slot(val)) array_mark_may_have_holes(arr);
    dense_set(js, doff, len, val);
    array_len_set(js, arr, len + 1);
    return;
  }
  
  ant_offset_t len = 0;
  ant_value_t len_result = proxy_aware_length(js, arr, &len);
  if (is_err(len_result)) return;
  
  if (!is_empty_slot(val)) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
    js_mkprop_fast(js, arr, idxstr, idxlen, val);
  }
  
  array_len_set(js, arr, len + 1);
}

void js_arr_set_index(ant_t *js, ant_value_t arr, ant_offset_t idx, ant_value_t val) {
  arr_set(js, arr, idx, val);
}

void js_arr_set_dense_length(ant_t *js, ant_value_t arr, ant_offset_t len) {
  ant_offset_t doff = get_dense_buf(arr);
  if (!doff) return;
  if (len > dense_capacity(doff)) doff = dense_grow(js, arr, len);
  if (doff) array_len_set(js, arr, len);
}

ant_object_t *js_dense_array_obj(ant_value_t value) {
  ant_object_t *arr = real_array_obj_ptr(value);
  if (!array_dense_storage(arr) || arr->u.array.len > arr->u.array.cap) return NULL;
  return arr;
}

bool is_array_value(ant_value_t value) {
  while (is_proxy(value)) {
    ant_proxy_state_t *data = get_proxy_data(value);
    if (!data || data->revoked) return false;
    value = data->target;
  }
  return real_array_obj_ptr(value) != NULL;
}

ant_value_t js_is_array_value_checked(ant_t *js, ant_value_t value, bool *out) {
  while (is_proxy(value)) {
    ant_proxy_state_t *data = get_proxy_data(value);
    if (!data || data->revoked)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform IsArray on a revoked proxy");
    value = data->target;
  }
  *out = real_array_obj_ptr(value) != NULL;
  return Ant_Value_Undefined;
}

ant_value_t proxy_read_target(ant_t *js, ant_value_t obj) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  return data ? data->target : obj;
}

static inline TypedArrayData *array_method_typedarray_data(ant_value_t value, uint8_t value_type) {
  if (value_type == kTypeTypedArray) return (TypedArrayData *)js_gettypedarray(value);
  if (value_type == kTypeObject) return (TypedArrayData *)js_get_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);
  return NULL;
}

ant_value_t proxy_aware_length(ant_t *js, ant_value_t obj, ant_offset_t *out_len) {
  *out_len = 0;
  uint8_t obj_type = vtype(obj);
  if (is_proxy(obj)) {
    ant_value_t len_val = proxy_get(js, obj, "length", 6);
    if (is_err(len_val)) return len_val;
    if (vtype(len_val) == kTypeNumber) *out_len = (ant_offset_t)Ant_Value_AsNumberUnchecked(len_val);
    return Ant_Value_Undefined;
  }
  TypedArrayData *ta = obj_type == kTypeArray ? NULL : array_method_typedarray_data(obj, obj_type);
  if (ta) {
    *out_len = (ant_offset_t)ta->length;
    return Ant_Value_Undefined;
  }
  return array_like_length_checked(js, obj, out_len);
}

ant_value_t proxy_aware_get_elem(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  if (is_proxy(obj)) return proxy_get(js, obj, key, key_len);
  ant_prop_loc_t off = lkp(js, obj, key, key_len);
  return off.obj ? js_prop_load(off) : Ant_Value_Undefined;
}

ant_value_t proxy_delete_index(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
  return proxy_delete(js, obj, idxstr, idxlen);
}

bool array_method_dense_element(ant_value_t arr, ant_offset_t idx, ant_value_t *out) {
  return array_dense_read(array_obj_ptr(arr), idx, out);
}

ant_object_t *array_method_elements(ant_value_t arr) {
  return is_proxy(arr) ? NULL : array_obj_ptr(arr);
}

ant_value_t array_method_element(
  ant_t *js, ant_value_t arr, ant_object_t *elems, ant_offset_t idx, ant_value_t *out
) {
  if (array_dense_read(elems, idx, out)) return Ant_Value_True;
  
  ant_value_t has = array_method_has_index(js, arr, idx);
  if (has != Ant_Value_True) return has;
  
  *out = array_method_get_index(js, arr, idx);
  return is_err(*out) ? *out : Ant_Value_True;
}

ant_value_t array_method_has_index(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_value_t dense;
  if (array_method_dense_element(arr, idx, &dense)) return Ant_Value_True;
  if (is_proxy(arr)) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
    return proxy_has(js, arr, idxstr, idxlen);
  }
  
  uint8_t arr_type = vtype(arr);
  if (arr_type != kTypeArray) {
    TypedArrayData *ta = array_method_typedarray_data(arr, arr_type);
    if (ta) return Ant_Value_FromBool((size_t)idx < ta->length);
    return do_in(js, Ant_Value_FromNumber((double)idx), arr);
  }
  
  return Ant_Value_FromBool(arr_has(js, arr, idx));
}

static bool array_like_index_keys_absent(ant_value_t obj) {
  ant_value_t cur = obj;
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (vtype(cur) != kTypeObject || is_proxy(cur) || array_obj_ptr(cur)) return false;
    ant_object_t *ptr = js_obj_ptr(cur);
    if (!ptr || !ptr->shape || ptr->flags.is_exotic || ant_object_exotic_ops(ptr)) return false;
    if (ant_shape_may_have_index_keys(ptr->shape)) return false;
    if (vtype(js_string_wrapper_value(cur)) == kTypeString) return false;
    cur = ant_object_proto(ptr);
    if (vtype(cur) == kTypeNull) return true;
  }

  return false;
}

static bool array_like_index_absent(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  if (intern_find(js, key, key_len)) return false;

  ant_value_t cur = obj;
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (vtype(cur) != kTypeObject || is_proxy(cur) || array_obj_ptr(cur)) return false;
    ant_object_t *ptr = js_obj_ptr(cur);
    if (!ptr || ptr->flags.is_exotic || ant_object_exotic_ops(ptr)) return false;
    if (vtype(js_string_wrapper_value(cur)) == kTypeString) return false;
    cur = ant_object_proto(ptr);
    if (vtype(cur) == kTypeNull) return true;
  }

  return false;
}

ant_value_t array_method_get_index(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_value_t dense;
  if (array_method_dense_element(arr, idx, &dense)) return dense;
  if (is_proxy(arr)) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
    return proxy_get(js, arr, idxstr, idxlen);
  }
  
  uint8_t arr_type = vtype(arr);
  if (arr_type != kTypeArray) {
    TypedArrayData *ta = array_method_typedarray_data(arr, arr_type);
    ant_value_t item = Ant_Value_Undefined;
    if (ta && buffer_typedarray_data_read_index(js, ta, (size_t)idx, &item)) return item;
    if (!ta) {
      if (array_like_index_keys_absent(arr)) return Ant_Value_Undefined;
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
      if (array_like_index_absent(js, arr, idxstr, idxlen)) return Ant_Value_Undefined;
      return js_getprop_fallback_len(js, arr, idxstr, idxlen);
    }
  }
  
  return arr_get(js, arr, idx);
}
