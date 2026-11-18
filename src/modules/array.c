#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <limits.h>
#include <math.h>

#include "utf8.h"
#include "cfunc.h"
#include "props.h"
#include "arrays.h"
#include "convert.h"
#include "proto.h"
#include "builder.h"
#include "stringify.h"
#include "protectors.h"
#include "silver/call.h"
#include "modules/array.h"
#include "modules/number.h"
#include "modules/proxy.h"
#include "modules/string.h"
#include "modules/iterator.h"

ant_value_t builtin_Array(ant_params_t) {
  ant_value_t arr = Ant_Array_Create(js);
  if (is_err(arr)) return arr;
  
  if (nargs == 1 && vtype(args[0]) == kTypeNumber) {
    ant_value_t err = validate_array_length(js, args[0]);
    if (is_err(err)) return err;
    ant_offset_t new_len = (ant_offset_t)Ant_Value_AsNumberUnchecked(args[0]);
    ant_offset_t doff = get_dense_buf(arr);
    if (doff && new_len <= MAX_DENSE_PREALLOC_LEN) 
      if (new_len > dense_capacity(doff)) doff = dense_grow(js, arr, new_len);
    if (new_len > 0) array_mark_may_have_holes(arr);
    array_len_set(js, arr, new_len);
  } else if (nargs > 0) for (int i = 0; i < nargs; i++) 
    arr_set(js, arr, (ant_offset_t)i, args[i]);

  ant_value_t instance_proto = js_instance_proto_from_new_target(js, js->sym.array_proto, call_new_target);
  
  if (is_object_type(instance_proto)) js_set_proto_init(arr, instance_proto);
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin) {
    js_set_slot(arr, SLOT_CTOR, call_new_target);
  }
  
  return arr;
}

static inline ant_value_t require_callback(ant_native_params_t, const char *name) {
  if (nargs == 0 || !is_callable(args[0]))
    return js_mkerr(js, "%s requires a function argument", name);
  return args[0];
}

static inline void array_copy_write(ant_t *js, ant_value_t result, ant_offset_t doff, ant_offset_t idx, ant_value_t value) {
  if (doff) dense_set(js, doff, idx, value);
  else arr_set(js, result, idx, value);
}

static inline ant_value_t array_copy_element(
  ant_t *js, ant_value_t arr, ant_object_t *elems,
  ant_value_t result, ant_offset_t doff, ant_offset_t from, ant_offset_t to
) {
  ant_value_t value;
  if (!array_dense_read(elems, from, &value)) {
    value = array_method_get_index(js, arr, from);
    if (is_err(value)) return value;
  }
  array_copy_write(js, result, doff, to, value);
  return Ant_Value_Undefined;
}

static ant_value_t array_copy_spliced(
  ant_t *js, ant_value_t arr, ant_offset_t len, ant_offset_t start,
  ant_offset_t skip, const ant_value_t *items, ant_offset_t item_count
) {
  ant_offset_t out_len = len - skip + item_count;
  
  if (out_len > (ant_offset_t)9007199254740991) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid array length");
  if (out_len > UINT32_MAX) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  if (out_len == 0) return Ant_Array_Create(js);
  
  ant_value_t result = alloc_array_with_proto_capacity(
    js, js->sym.array_proto, 
    gc_array_storage_fit((uint32_t)out_len), 0, true
  );
  
  if (is_err(result)) return result;
  
  ant_offset_t doff = get_dense_buf(result);
  if (doff && dense_capacity(doff) >= out_len) {
    array_mark_may_have_holes(result);
    array_len_set(js, result, out_len);
  } else doff = 0;
  
  GC_ROOT_SAVE(roots, js);
  GC_ROOT_PIN(js, result);
  
  ant_object_t *elems = array_obj_ptr(arr);
  ant_value_t err = Ant_Value_Undefined;
  
  for (ant_offset_t i = 0; i < start && !is_err(err); i++)
    err = array_copy_element(js, arr, elems, result, doff, i, i);
  for (ant_offset_t k = 0; k < item_count && !is_err(err); k++)
    array_copy_write(js, result, doff, start + k, items[k]);
  for (ant_offset_t i = start + skip; i < len && !is_err(err); i++)
    err = array_copy_element(js, arr, elems, result, doff, i, i - skip + item_count);
  
  GC_ROOT_RESTORE(js, roots);
  if (is_err(err)) return err;
  if (doff) array_obj_ptr(result)->flags.may_have_holes = 0;
  
  return result;
}

static ant_value_t arguments_get_index(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  char key[24];
  uint_to_str(key, sizeof(key), (uint64_t)idx);
  return js_getprop_fallback(js, obj, key);
}

static ant_value_t arguments_delete_index(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  char key[24];
  size_t key_len = uint_to_str(key, sizeof(key), (uint64_t)idx);
  return js_delete_prop(js, obj, key, key_len);
}

static ant_value_t arguments_move_index(ant_t *js, ant_value_t obj, ant_offset_t from, ant_offset_t to) {
  if (!arr_has(js, obj, from)) return arguments_delete_index(js, obj, to);
  ant_value_t value = arguments_get_index(js, obj, from);
  if (is_err(value)) return value;
  return js_setprop_index(js, obj, (uint32_t)to, value);
}

static ant_value_t arguments_set_length(ant_t *js, ant_value_t obj, ant_offset_t len) {
  ant_value_t result = js_setprop(js, obj, js->length_str, Ant_Value_FromNumber((double)len));
  if (is_err(result)) return result;
  ant_value_t stored = lkp_interned_val(js, obj, js->intern.length);
  if (vtype(stored) != kTypeNumber || Ant_Value_AsNumberUnchecked(stored) != (double)len)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property 'length' of object");
  return Ant_Value_FromNumber((double)len);
}

static inline bool array_dense_writable(ant_t *js, ant_object_t *dense) {
  if (!dense->flags.extensible) return false;
  return (!dense->flags.sealed && !dense->flags.frozen) || Ant_Array_TryUnshare(js, dense);
}

static bool array_refuses_add(ant_value_t arr, int nargs) {
  if (nargs <= 0 || is_proxy(arr)) return false;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(arr));
  return ptr && (ptr->flags.frozen || ptr->flags.sealed || !ptr->flags.extensible);
}

static __attribute__((noinline, cold)) ant_value_t array_add_refused_error(ant_t *js, ant_offset_t idx) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot add property %llu, object is not extensible", (unsigned long long)idx);
}

static bool arguments_refuses_add(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  return ptr->flags.frozen || ptr->flags.sealed || !ptr->flags.extensible;
}

static bool arguments_refuses_delete(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  return ptr->flags.frozen || ptr->flags.sealed;
}

static __attribute__((noinline, cold)) ant_value_t array_delete_refused(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  ant_value_t described = Ant_Value_Describe(js, obj);
  if (is_err(described)) return described;
  size_t len = 0;
  const char *bytes = js_getstr(js, described, &len);
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot delete property '%llu' of %.*s", (unsigned long long)idx, (int)len, bytes);
}

static ant_value_t arguments_push(ant_t *js, ant_value_t obj, ant_args_t) {
  ant_offset_t len = 0;
  ant_value_t result = array_like_length_checked(js, obj, &len);
  if (is_err(result)) return result;
  
  if (nargs > 0 && arguments_refuses_add(obj)) return array_add_refused_error(js, len);
  for (int i = 0; i < nargs; i++, len++) {
    result = js_setprop_index(js, obj, (uint32_t)len, args[i]);
    if (is_err(result)) return result;
  }
  
  return arguments_set_length(js, obj, len);
}

static ant_value_t arguments_pop(ant_t *js, ant_value_t obj) {
  ant_offset_t len = 0;
  ant_value_t result = array_like_length_checked(js, obj, &len);
  if (is_err(result)) return result;
  
  if (len == 0) {
    result = arguments_set_length(js, obj, 0);
    return is_err(result) ? result : Ant_Value_Undefined;
  }
  
  if (arguments_refuses_delete(obj)) return array_delete_refused(js, obj, len - 1);
  ant_value_t value = arguments_get_index(js, obj, len - 1);
  if (is_err(value)) return value;
  
  if (is_err(result = arguments_delete_index(js, obj, len - 1))) return result;
  if (is_err(result = arguments_set_length(js, obj, len - 1))) return result;
  
  return value;
}

static ant_value_t arguments_shift(ant_t *js, ant_value_t obj) {
  ant_offset_t len = 0;
  ant_value_t result = array_like_length_checked(js, obj, &len);
  if (is_err(result)) return result;
  if (len == 0) {
    result = arguments_set_length(js, obj, 0);
    return is_err(result) ? result : Ant_Value_Undefined;
  }
  if (js_obj_ptr(obj)->flags.frozen && len > 1) return Ant_Error_CreateReadOnlyProperty(js, obj, Ant_Value_FromNumber(0));
  ant_value_t first = arguments_get_index(js, obj, 0);
  if (is_err(first)) return first;
  for (ant_offset_t k = 1; k < len; k++)
    if (is_err(result = arguments_move_index(js, obj, k, k - 1))) return result;
  if (arguments_refuses_delete(obj)) return array_delete_refused(js, obj, len - 1);
  if (is_err(result = arguments_delete_index(js, obj, len - 1))) return result;
  if (is_err(result = arguments_set_length(js, obj, len - 1))) return result;
  return first;
}

static ant_value_t arguments_unshift(ant_t *js, ant_value_t obj, ant_args_t) {
  ant_offset_t len = 0;
  ant_value_t result = array_like_length_checked(js, obj, &len);
  if (is_err(result)) return result;
  
  if (nargs > 0) {
    if (arguments_refuses_add(obj)) return array_add_refused_error(js, len ? len - 1 + (ant_offset_t)nargs : 0);
    for (ant_offset_t k = len; k > 0; k--)
      if (is_err(result = arguments_move_index(js, obj, k - 1, k - 1 + (ant_offset_t)nargs))) return result;
    for (int i = 0; i < nargs; i++)
      if (is_err(result = js_setprop_index(js, obj, (uint32_t)i, args[i]))) return result;
  }
  
  return arguments_set_length(js, obj, len + (ant_offset_t)nargs);
}

static inline bool array_string_wrapper_length(ant_t *js, ant_value_t arr, ant_offset_t *len) {
  ant_value_t prim = js_string_wrapper_value(arr);
  if (vtype(prim) != kTypeString) return false;
  *len = str_utf16_len(js, prim);
  return true;
}

static ant_value_t array_string_wrapper_write_error(ant_t *js, const char *key) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property '%s' of object '[object String]'", key);
}

static ant_value_t array_method_receiver(ant_t *js, ant_value_t value, const char *name) {
  uint8_t t = vtype(value);
  if (t == kTypeArray || t == kTypeObject) return value;
  if (t == kTypeUndefined || t == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Array.prototype.%s called on null or undefined", name);
  return is_boxable_primitive_type(t) ? js_normalize_sloppy_this(js, value) : value;
}

static ant_value_t array_set_index_throw(ant_t *js, ant_value_t obj, ant_offset_t idx, ant_value_t value);
static ant_value_t array_set_length_throw(ant_t *js, ant_value_t obj, ant_offset_t len);

static __attribute__((noinline, cold)) ant_value_t array_integer_arg_coerced(ant_t *js, ant_value_t arg, double *out) {
  double number = js_to_number(js, arg);
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  *out = number != number ? 0 : trunc(number);
  return Ant_Value_Undefined;
}

static inline __attribute__((always_inline)) ant_value_t array_integer_arg(
  ant_t *js, ant_value_t *args, int nargs, int at, double fallback, double *out
) {
  *out = fallback;
  if (at >= nargs || vtype(args[at]) == kTypeUndefined) return Ant_Value_Undefined;
  if (__builtin_expect(vtype(args[at]) != kTypeNumber, 0)) return array_integer_arg_coerced(js, args[at], out);
  
  double number = Ant_Value_AsNumberUnchecked(args[at]);
  *out = number != number ? 0 : trunc(number);
  return Ant_Value_Undefined;
}

static inline __attribute__((always_inline)) ant_value_t array_relative_arg(
  ant_t *js, ant_value_t *args, int nargs, int at,
  ant_offset_t len, ant_offset_t fallback, ant_offset_t *out
) {
  double relative;
  ant_value_t err = array_integer_arg(js, args, nargs, at, (double)fallback, &relative);
  if (is_err(err)) return err;
  
  if (relative < 0) relative = relative + (double)len < 0 ? 0 : relative + (double)len;
  else if (relative > (double)len) relative = (double)len;
  
  *out = (ant_offset_t)relative;
  return Ant_Value_Undefined;
}
static ant_value_t array_set_index_throw(ant_t *js, ant_value_t obj, ant_offset_t idx, ant_value_t value);

static ant_value_t builtin_array_push(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "push");
  
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");

  if (is_proxy(arr)) {
    PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
    if (len > (ant_offset_t)9007199254740991 - (ant_offset_t)nargs)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Pushing %d elements on an array-like of length %.0f is disallowed by its maximum length", nargs, (double)len);
    for (int i = 0; i < nargs; i++, len++) {
      ant_value_t set = array_set_index_throw(js, arr, len, args[i]);
      if (is_err(set)) return set;
    }
    
    ant_value_t set_len = array_set_length_throw(js, arr, len);
    if (is_err(set_len)) return set_len;
    return Ant_Value_FromNumber((double) len);
  }
  
  if (js_is_arguments(arr)) return arguments_push(js, arr, args, nargs);
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  if (__builtin_expect(len > (ant_offset_t)9007199254740991 - (ant_offset_t)nargs, 0))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Pushing %d elements on an array-like of length %.0f is disallowed by its maximum length", nargs, (double)len);

  ant_object_t *target = js_obj_ptr(js_as_obj(arr));
  if (array_refuses_add(arr, nargs)) {
    if (!Ant_Array_UnshareForAppend(js, arr, (uint32_t)nargs)) return js_mkerr(js, "oom");
    if (array_refuses_add(arr, nargs)) return array_add_refused_error(js, len);
  }
  if (target && (ant_object_is_frozen(target) || array_length_readonly(js, arr)))
    return array_length_readonly_error(js);
  
  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    ant_object_t *dense = dense_obj(doff);
    bool plain = dense && array_proto_chain_plain(js, dense);
    int i = 0;
    for (; i < nargs && (plain || !array_store_reaches_proto_slow(js, arr, len)); i++) {
      ant_offset_t cap = dense_capacity(doff);
      if (__builtin_expect(len >= cap && len >= ARRAY_DENSE_GAP && len >= dense_iterable_length(js, arr) * 4, 0)) break;
      if (len >= cap) {
        doff = dense_grow(js, arr, len + 1);
        if (doff == 0) return js_mkerr(js, "oom");
      }
      if (is_empty_slot(args[i])) array_mark_may_have_holes(arr);
      dense_set_owned(js, doff, len, args[i]);
      len++;
    }
    array_len_set(js, arr, len);
    if (i == nargs) return Ant_Value_FromNumber((double) len);

    for (; i < nargs; i++) {
      ant_value_t stored = array_set_index_throw(js, arr, len, args[i]);
      if (is_err(stored)) return stored;
      len++;
    }
    ant_value_t len_val = Ant_Value_FromNumber((double) len);
    ant_value_t set = js_setprop_throw(js, arr, js->length_str, false, 0, len_val);
    return is_err(set) ? set : len_val;
  }
  
  for (int i = 0; i < nargs; i++, len++) {
    ant_value_t stored = array_set_index_throw(js, arr, len, args[i]);
    if (is_err(stored)) return stored;
  }

  ant_value_t new_len = Ant_Value_FromNumber((double) len);
  ant_value_t set = js_setprop_throw(js, arr, js->length_str, false, 0, new_len);
  return is_err(set) ? set : new_len;
}

static ant_value_t array_set_index_throw(ant_t *js, ant_value_t obj, ant_offset_t idx, ant_value_t value) {
  if (idx <= UINT32_MAX) return js_setprop_index_throw(js, obj, (uint32_t)idx, value);
  char key[24];
  size_t klen = uint_to_str(key, sizeof(key), (uint64_t)idx);
  return js_setprop_throw(js, obj, js_mkstr(js, key, klen), false, 0, value);
}

static ant_value_t array_delete_index_throw(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  char key[24];
  size_t klen = uint_to_str(key, sizeof(key), (uint64_t)idx);
  ant_value_t deleted = js_delete_prop_ordinary(js, obj, key, klen);
  if (is_err(deleted)) return deleted;
  if (deleted == Ant_Value_False) return array_delete_refused(js, obj, idx);
  return Ant_Value_Undefined;
}

static ant_value_t array_set_length_throw(ant_t *js, ant_value_t obj, ant_offset_t len) {
  ant_value_t set = js_setprop_throw(js, obj, js->length_str, false, 0, Ant_Value_FromNumber((double)len));
  return is_err(set) ? set : Ant_Value_Undefined;
}

static ant_value_t array_set_result_length(ant_t *js, ant_value_t result, ant_offset_t len, bool fresh) {
  if (fresh) return Ant_Value_Undefined;
  ant_object_t *ptr = vtype(result) == kTypeArray ? (ant_object_t *)vptr(result) : NULL;
  bool plain = ptr && !ptr->flags.arguments_object && !(ptr->flags.raw & (ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_EXOTIC));
  if (plain && ptr->u.array.len == len) return Ant_Value_Undefined;
  return array_set_length_throw(js, result, len);
}

static ant_value_t array_move_range_throw(
  ant_t *js, ant_value_t arr, ant_offset_t from, ant_offset_t to, ant_offset_t count, bool backward
) {
  for (ant_offset_t n = 0; n < count; n++) {
    ant_offset_t i = backward ? count - 1 - n : n;
    ant_value_t has = array_method_has_index(js, arr, from + i);
    if (is_err(has)) return has;
    
    ant_value_t done;
    if (js_truthy(js, has)) {
      ant_value_t value = array_method_get_index(js, arr, from + i);
      if (is_err(value)) return value;
      done = array_set_index_throw(js, arr, to + i, value);
    } else done = array_delete_index_throw(js, arr, to + i);
    if (is_err(done)) return done;
  }
  return Ant_Value_Undefined;
}

static inline ant_object_t *array_mutable_dense(ant_t *js, ant_value_t arr) {
  if (vtype(arr) != kTypeArray) return NULL;
  ant_object_t *ptr = array_obj_ptr(arr);
  const uint16_t mask =
    ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS | ANT_OBJECT_FLAG_FROZEN |
    ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_EXOTIC | ANT_OBJECT_FLAG_ARGUMENTS;
  if (!ptr || (ptr->flags.raw & mask) != (ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS)) return NULL;
  if (!ptr->u.array.data) return NULL;
  return ptr;
}

static inline bool array_hole_reads_proto(ant_t *js, ant_object_t *ptr, ant_value_t slot) {
  return is_empty_slot(slot) && !array_proto_chain_plain(js, ptr);
}

static ant_value_t array_pop_generic(ant_t *js, ant_value_t arr) {
  ant_offset_t len = 0;
  ant_value_t len_result = array_like_length_checked(js, arr, &len);
  if (is_err(len_result)) return len_result;
  
  if (len == 0) {
    ant_value_t set = array_set_length_throw(js, arr, 0);
    return is_err(set) ? set : Ant_Value_Undefined;
  }
  
  ant_value_t last = array_method_get_index(js, arr, len - 1);
  if (is_err(last)) return last;
  ant_value_t done = array_delete_index_throw(js, arr, len - 1);
  if (is_err(done)) return done;
  done = array_set_length_throw(js, arr, len - 1);
  
  return is_err(done) ? done : last;
}

static ant_value_t array_shift_generic(ant_t *js, ant_value_t arr) {
  ant_offset_t len = 0;
  ant_value_t len_result = array_like_length_checked(js, arr, &len);
  if (is_err(len_result)) return len_result;
  
  if (len == 0) {
    ant_value_t set = array_set_length_throw(js, arr, 0);
    return is_err(set) ? set : Ant_Value_Undefined;
  }
  
  ant_value_t first = array_method_get_index(js, arr, 0);
  if (is_err(first)) return first;
  ant_value_t done = array_move_range_throw(js, arr, 1, 0, len - 1, false);
  if (is_err(done)) return done;
  done = array_delete_index_throw(js, arr, len - 1);
  if (is_err(done)) return done;
  done = array_set_length_throw(js, arr, len - 1);
  
  return is_err(done) ? done : first;
}

static ant_value_t array_splice_generic(
  ant_t *js, ant_value_t arr, ant_offset_t len, ant_offset_t start,
  ant_offset_t delete_count, ant_value_t *items, ant_offset_t insert_count,
  ant_value_t removed, bool intrinsic
) {
  for (ant_offset_t k = 0; k < delete_count; k++) {
    ant_value_t has = array_method_has_index(js, arr, start + k);
    if (is_err(has)) return has;
    if (!js_truthy(js, has)) continue;
    ant_value_t value = array_method_get_index(js, arr, start + k);
    if (is_err(value)) return value;
    ant_value_t set = array_result_set(js, removed, intrinsic, k, value);
    if (is_err(set)) return set;
  }
  
  ant_value_t done = intrinsic
    ? array_set_result_length(js, removed, delete_count, false)
    : array_set_length_throw(js, removed, delete_count);
  if (is_err(done)) return done;
  
  ant_offset_t tail = len - start - delete_count;
  if (insert_count != delete_count) {
    done = array_move_range_throw(js, arr, start + delete_count, start + insert_count, tail, insert_count > delete_count);
    if (is_err(done)) return done;
  }
  
  for (ant_offset_t k = len; k > len - delete_count + insert_count; k--) {
    done = array_delete_index_throw(js, arr, k - 1);
    if (is_err(done)) return done;
  }
  
  for (ant_offset_t k = 0; k < insert_count; k++) {
    done = array_set_index_throw(js, arr, start + k, items[k]);
    if (is_err(done)) return done;
  }
  
  done = array_set_length_throw(js, arr, len - delete_count + insert_count);
  return is_err(done) ? done : removed;
}

static ant_value_t builtin_array_pop(ant_params_t) {
  ant_object_t *fast = array_mutable_dense(js, call_this);
  if (__builtin_expect(fast != NULL, 1)) {
    uint32_t len = fast->u.array.len;
    if (len == 0) return Ant_Value_Undefined;
    ant_value_t last = fast->u.array.data[--len];
    if (__builtin_expect(array_hole_reads_proto(js, fast, last), 0)) return array_pop_generic(js, call_this);
    fast->u.array.data[len] = T_EMPTY;
    fast->u.array.len = len;
    return is_empty_slot(last) ? Ant_Value_Undefined : last;
  }
  
  if (!Ant_Array_UnshareValue(js, call_this)) return js_mkerr(js, "oom");
  ant_value_t arr = array_method_receiver(js, call_this, "pop");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");

  if (is_proxy(arr)) {
    PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
    if (len == 0) {
      js_setprop(js, arr, js->length_str, Ant_Value_FromNumber(0.0));
      return Ant_Value_Undefined;
    }
    
    len--;
    char idxstr[16];
    
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
    ant_value_t result = proxy_aware_get_elem(js, arr, idxstr, idxlen);
    
    if (is_err(result)) return result;
    ant_value_t del = proxy_delete(js, arr, idxstr, idxlen);
    
    if (is_err(del)) return del;
    js_setprop(js, arr, js->length_str, Ant_Value_FromNumber((double) len));
    
    return result;
  }
  
  if (js_is_arguments(arr)) return arguments_pop(js, arr);

  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    ant_object_t *ptr = (ant_object_t *)(uintptr_t)doff;
    bool is_array = ptr->type_tag == kTypeArray;
    if (ptr->flags.sealed || array_length_readonly(js, arr)) return array_pop_generic(js, arr);
    
    ant_offset_t len = is_array ? ptr->u.array.len : get_array_length(js, arr);
    if (len == 0) return Ant_Value_Undefined;
    if (len > ptr->u.array.cap) goto pop_slow;
    len--;
    
    ant_value_t result = ptr->u.array.data[len];
    if (array_hole_reads_proto(js, ptr, result)) return array_pop_generic(js, arr);
    if (is_empty_slot(result)) result = Ant_Value_Undefined;
    ptr->u.array.data[len] = T_EMPTY;
    
    if (is_array) {
      ptr->u.array.len = (uint32_t)len;
      ptr->flags.dense_length_fits = true;
    } else array_len_set(js, arr, len);
    
    return result;
  }

  pop_slow:
  return array_pop_generic(js, arr);
}

bool js_is_array_push_builtin(ant_value_t func) {
  return vtype(func) == kTypeBuiltin && js_as_cfunc(func) == builtin_array_push;
}

bool js_is_array_pop_builtin(ant_value_t func) {
  return vtype(func) == kTypeBuiltin && js_as_cfunc(func) == builtin_array_pop;
}

static ant_value_t builtin_array_slice(ant_params_t) {
  ant_value_t arr = call_this;
  ant_value_t string_val = unwrap_primitive(js, arr);
  bool string_like = (vtype(string_val) == kTypeString);

  if (!string_like) {
    arr = array_method_receiver(js, arr, "slice");
    if (is_err(arr)) return arr;
  }
  
  ant_offset_t len = 0;
  ant_offset_t string_byte_len = 0;
  ant_offset_t string_off = 0;
  
  if (string_like) {
    string_off = vstr(js, string_val, &string_byte_len);
    len = str_utf16_len(js, string_val);
  } else {
    ant_value_t len_result = array_like_length_checked(js, arr, &len);
    if (is_err(len_result)) return len_result;
  }
  
  ant_offset_t start = 0, end = len;
  double dlen = (double)(len);
  if (nargs >= 1) {
    double d;
    if (vtype(args[0]) == kTypeNumber) d = string_number_to_integer(Ant_Value_AsNumberUnchecked(args[0]));
    else {
      ant_value_t status = string_to_integer_or_infinity(js, args[0], &d);
      if (is_err(status)) return status;
    }
    if (d < 0) start = (ant_offset_t) (d + dlen < 0 ? 0 : d + dlen);
    else start = (ant_offset_t) (d > dlen ? dlen : d);
  }
  
  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    double d;
    if (vtype(args[1]) == kTypeNumber) d = string_number_to_integer(Ant_Value_AsNumberUnchecked(args[1]));
    else {
      ant_value_t status = string_to_integer_or_infinity(js, args[1], &d);
      if (is_err(status)) return status;
    }
    if (d < 0) end = (ant_offset_t) (d + dlen < 0 ? 0 : d + dlen);
    else end = (ant_offset_t) (d > dlen ? dlen : d);
  }
  
  if (start > end) start = end;
  ant_offset_t count = end - start;
  bool intrinsic = false;
  
  ant_value_t result = array_species_create(js, arr, count, &intrinsic);
  if (is_err(result)) return result;
  
  ant_object_t *src = !string_like && vtype(arr) == kTypeArray ? array_obj_ptr(arr) : NULL;
  ant_object_t *dst = intrinsic && vtype(result) == kTypeArray ? array_obj_ptr(result) : NULL;
  
  if (
    src && dst && array_dense_covers(src, end) &&
    dst->flags.fast_array && dst->u.array.data && (dst->u.array.len == 0 || dst->u.array.len == count) &&
    array_elements_proto_plain(js, src) &&
    (count <= dst->u.array.cap || dense_grow(js, result, count))
  ) {
    bool has_holes = false, has_elements = false;
    bool dst_old = dst->flags.generation == 1;
    for (ant_offset_t i = 0; i < count; i++) {
      ant_value_t value = src->u.array.data[start + i];
      dst->u.array.data[i] = value;
      if (dst_old) gc_write_barrier_elem(js, dst, (uint32_t)i, value);
      if (is_empty_slot(value)) has_holes = true;
      else has_elements = true;
    }
    dst->u.array.len = (uint32_t)count;
    dst->flags.may_have_holes = has_holes;
    dst->flags.may_have_dense_elements |= has_elements;
    return result;
  }
  
  ant_offset_t result_idx = 0;
  bool skipped = false;
  for (ant_offset_t i = start; i < end; i++, result_idx++) {
    ant_value_t elem;
    if (string_like) {
      uint32_t code_unit = utf16_code_unit_at((const char *)(uintptr_t)string_off, string_byte_len, i);
      if (code_unit == 0xFFFFFFFF) break;
      elem = js_string_from_utf16_code_unit(js, code_unit);
    } else {
      ant_value_t has = array_method_has_index(js, arr, i);
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) { skipped = true; continue; }
      elem = array_method_get_index(js, arr, i);
      if (is_err(elem)) return elem;
    }
    
    ant_value_t set = array_result_set(js, result, intrinsic, result_idx, elem);
    if (is_err(set)) return set;
  }
  
  if (!string_like && get_array_length(js, result) != result_idx)
    js_setprop(js, result, js->length_str, Ant_Value_FromNumber((double)result_idx));
  if (intrinsic && !skipped && result_idx == count) array_mark_packed(result);
  return result;
}

static __attribute__((noinline, cold)) ant_value_t array_join_too_long(ant_t *js, ant_offset_t len) {
  if (len > UINT32_MAX) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid array length");
  return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid string length");
}

static ant_value_t array_join_body(ant_params_t) {
  if (sv_check_c_stack_overflow(js)) return js_mkerr_typed(js, JS_ERR_RANGE, "Maximum call stack size exceeded");
  ant_value_t arr = array_method_receiver(js, call_this, "join");
  if (is_err(arr)) return arr;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, arr);

  const char *sep = ",";
  ant_offset_t sep_len = 1;
  ant_value_t sep_value = Ant_Value_Undefined;
  ant_value_t sep_arg = nargs >= 1 ? args[0] : Ant_Value_Undefined;
  
  GC_ROOT_PIN(js, sep_arg);
  GC_ROOT_PIN(js, sep_value);
  
  ant_offset_t len = 0;
  ant_value_t len_result = proxy_aware_length(js, arr, &len);
  if (is_err(len_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return len_result;
  }

  if (vtype(sep_arg) != kTypeUndefined) {
    sep_value = coerce_to_str_hint(js, sep_arg, 1);
    if (is_err(sep_value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return sep_value;
    }
    ant_offset_t sep_off = vstr(js, sep_value, &sep_len);
    sep = (const char *)(uintptr_t)sep_off;
  }
  
  if (len == 0) {
    ant_value_t empty = js_mkstr(js, "", 0);
    GC_ROOT_RESTORE(js, root_mark);
    return empty;
  }
  if (__builtin_expect(len > (ant_offset_t)ANT_STRING_MAX_LENGTH && (len > UINT32_MAX || sep_len > 0), 0)) {
    GC_ROOT_RESTORE(js, root_mark);
    return array_join_too_long(js, len);
  }

  ant_object_t *fast_arr = array_obj_ptr(arr);
  bool fast_primitives = fast_arr && fast_arr->flags.fast_array &&
    !fast_arr->flags.may_have_holes && fast_arr->u.array.data &&
    (ant_offset_t)fast_arr->u.array.len == len;
  
  size_t fast_total = 0;
  if (fast_primitives) {
    if (len > 1 && (size_t)sep_len > SIZE_MAX / ((size_t)len - 1))
      fast_primitives = false;
    else fast_total = len > 1 ? ((size_t)len - 1) * (size_t)sep_len : 0;
  }
  
  for (ant_offset_t i = 0; fast_primitives && i < len; i++) {
    ant_value_t elem = fast_arr->u.array.data[i];
    size_t elem_len = 0;
    
    switch (vtype(elem)) {
      case kTypeString:
        if (str_is_heap_rope(elem) || str_is_heap_builder(elem)) {
          fast_primitives = false;
          continue;
        }
        elem_len = (size_t)vstrlen(js, elem);
        break;
      case kTypeBool: elem_len = vdata(elem) ? 4 : 5; break;
      case kTypeNull:
      case kTypeUndefined: break;
      default: fast_primitives = false; continue;
    }
    
    if (elem_len > SIZE_MAX - fast_total) fast_primitives = false;
    else fast_total += elem_len;
  }

  if (fast_primitives) {
    ant_value_t result = js_mkstr(js, NULL, fast_total);
    GC_ROOT_PIN(js, result);
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }

    ant_flat_string_t *flat = (ant_flat_string_t *)vptr(result);
    fast_arr = array_obj_ptr(arr);
    if (vtype(sep_value) == kTypeString) {
      ant_offset_t sep_off = vstr(js, sep_value, &sep_len);
      sep = (const char *)(uintptr_t)sep_off;
    }
    
    size_t pos = 0;
    for (ant_offset_t i = 0; i < len; i++) {
      if (i > 0) {
        memcpy(flat->bytes + pos, sep, (size_t)sep_len);
        pos += (size_t)sep_len;
      }
      
      ant_value_t elem = fast_arr->u.array.data[i];
      const char *elem_ptr = NULL;
      size_t elem_len = 0;
      
      switch (vtype(elem)) {
        case kTypeString: {
          ant_offset_t str_len = 0;
          ant_offset_t str_off = vstr(js, elem, &str_len);
          elem_ptr = (const char *)(uintptr_t)str_off;
          elem_len = (size_t)str_len;
          break;
        }
        case kTypeBool:
          elem_ptr = vdata(elem) ? "true" : "false";
          elem_len = vdata(elem) ? 4 : 5;
          break;
        default: break;
      }
      
      if (elem_len > 0) {
        memcpy(flat->bytes + pos, elem_ptr, elem_len);
        pos += elem_len;
      }
    }
    
    flat->bytes[pos] = '\0';
    str_flat_finish(flat, str_detect_ascii_bytes(flat->bytes, pos));
    GC_ROOT_RESTORE(js, root_mark);
    
    return result;
  }
  
  char static_buf[1024];
  string_builder_t sb;
  string_builder_init(&sb, static_buf, sizeof(static_buf));
  
  ant_value_t elem = Ant_Value_Undefined;
  ant_value_t elem_str = Ant_Value_Undefined;
  
  GC_ROOT_PIN(js, elem);
  GC_ROOT_PIN(js, elem_str);

  for (ant_offset_t i = 0; i < len; i++) {
    if (i > 0 && !string_builder_append(&sb, sep, (size_t)sep_len)) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }

    if (!array_method_dense_element(arr, i, &elem)) elem = array_method_get_index(js, arr, i);
    elem_str = Ant_Value_Undefined;
    
    if (is_err(elem)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, root_mark);
      return elem;
    }
    
    if (vtype(elem) == kTypeNull || vtype(elem) == kTypeUndefined) continue;

    const char *elem_ptr = NULL;
    size_t elem_len = 0;
    
    char num_buf[32];
    
    switch (vtype(elem)) {
      case kTypeString: {
        ant_offset_t str_len = 0;
        ant_offset_t str_off = vstr(js, elem, &str_len);
        elem_ptr = (const char *)(uintptr_t)str_off;
        elem_len = (size_t)str_len;
        break;
      }
      case kTypeNumber:
        elem_len = js_number_to_chars(elem, num_buf, sizeof(num_buf));
        elem_ptr = num_buf;
        break;
      case kTypeBool:
        elem_ptr = vdata(elem) ? "true" : "false";
        elem_len = vdata(elem) ? 4 : 5;
        break;
      default: {
        elem_str = coerce_to_str_hint(js, elem, 1);
        if (is_err(elem_str)) {
          string_builder_dispose(&sb);
          GC_ROOT_RESTORE(js, root_mark);
          return elem_str;
        }
        ant_offset_t str_len = 0;
        ant_offset_t str_off = vstr(js, elem_str, &str_len);
        elem_ptr = (const char *)(uintptr_t)str_off;
        elem_len = (size_t)str_len;
        break;
      }
    }

    if (!string_builder_append(&sb, elem_ptr, elem_len)) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
  }
  
  ant_value_t ret = string_builder_finalize(js, &sb);
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

typedef struct {
  ant_value_t search;
  double search_num;
  uint8_t search_type;
  uint8_t mode;
} array_includes_query_t;

enum {
  ARRAY_INCLUDES_MATCH_GENERIC = 0,
  ARRAY_INCLUDES_MATCH_NUM     = 1,
  ARRAY_INCLUDES_MATCH_NAN     = 2,
  ARRAY_INCLUDES_MATCH_ID      = 3,
};

static inline array_includes_query_t array_includes_prepare_query(ant_value_t search) {
  array_includes_query_t q = {
    .search = search,
    .search_num = 0.0,
    .search_type = vtype(search),
    .mode = ARRAY_INCLUDES_MATCH_GENERIC,
  };

  if (q.search_type == kTypeNumber) {
    q.search_num = Ant_Value_AsNumberUnchecked(search);
    q.mode = isnan(q.search_num) 
      ? ARRAY_INCLUDES_MATCH_NAN 
      : ARRAY_INCLUDES_MATCH_NUM;
  } else if (
    q.search_type != kTypeString &&
    q.search_type != kTypeBigInt
  ) q.mode = ARRAY_INCLUDES_MATCH_ID;

  return q;
}

static inline bool array_includes_matches(ant_t *js, const array_includes_query_t *q, ant_value_t val) {
switch (q->mode) {
  case ARRAY_INCLUDES_MATCH_NUM: return vtype(val) == kTypeNumber && Ant_Value_AsNumberUnchecked(val) == q->search_num;
  case ARRAY_INCLUDES_MATCH_NAN: return vtype(val) == kTypeNumber && isnan(Ant_Value_AsNumberUnchecked(val));
  case ARRAY_INCLUDES_MATCH_ID:  return vtype(val) == q->search_type && vdata(val) == vdata(q->search);
  default: return strict_eq_values(js, val, q->search);
}}

static inline ant_offset_t array_includes_length_from_number(double len_num) {
  if (isnan(len_num) || len_num <= 0) return 0;
  if (len_num >= (double)UINT32_MAX) return UINT32_MAX;
  return (ant_offset_t)len_num;
}

static inline ant_offset_t array_includes_start_index(ant_native_params_t, ant_offset_t len) {
  int64_t start = 0;
  
  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
  double from_index_num = js_to_number(js, args[1]);
  if (!isnan(from_index_num)) start = (int64_t)from_index_num;
  if (start < 0) {
    start += (int64_t)len;
    if (start < 0) start = 0;
  }}

  if ((uint64_t)start >= (uint64_t)len) return len;
  return (ant_offset_t)start;
}

static ant_value_t array_includes_length_value(ant_t *js, ant_value_t arr) {
  if (real_array_obj_ptr(arr) && !is_proxy(arr)) return Ant_Value_FromNumber((double)get_array_length(js, arr));
  if (is_proxy(arr)) return proxy_get(js, arr, "length", 6);
  return js_getprop_fallback_len(js, arr, "length", 6);
}

static ant_value_t array_includes_get_index_value(
  ant_t *js, ant_value_t arr, ant_offset_t idx, char *idxstr, size_t idxlen
) {
  if (is_proxy(arr)) return proxy_get(js, arr, idxstr, idxlen);

  if (array_obj_ptr(arr)) {
    if (arr_has(js, arr, idx)) return arr_get(js, arr, idx);
    if (!lkp_proto(js, arr, idxstr, idxlen).obj) return Ant_Value_Undefined;
    idxstr[idxlen] = '\0';
    return js_getprop_super(js, get_proto(js, arr), arr, idxstr);
  }

  return js_getprop_fallback_len(js, arr, idxstr, idxlen);
}

static bool array_includes_get_dense_index_value(
  ant_value_t obj, ant_offset_t idx, ant_value_t *out
) {
  ant_value_t arr = (vtype(obj) == kTypeFunction) ? js_func_obj(obj) : obj;
  if (vtype(arr) != kTypeArray) return false;

  ant_offset_t doff = get_dense_buf(arr);
  if (!doff || idx >= dense_capacity(doff)) return false;

  ant_value_t val = dense_get(doff, idx);
  if (is_empty_slot(val)) return false;

  *out = val;
  return true;
}

static bool array_includes_get_proto_dense_index_value(
  ant_t *js, ant_value_t arr, ant_offset_t idx, ant_value_t *out
) {
  ant_value_t proto = get_proto(js, arr);
  for (int depth = 0; is_object_type(proto) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (array_includes_get_dense_index_value(proto, idx, out)) return true;
    proto = get_proto(js, proto);
  }

  return false;
}

static ant_value_t array_includes_get_array_index_value(
  ant_t *js, ant_value_t arr, ant_offset_t idx, char *idxstr, size_t idxlen
) {
  if (arr_has(js, arr, idx)) return arr_get(js, arr, idx);
  ant_value_t proto_dense_val = Ant_Value_Undefined;
  if (array_includes_get_proto_dense_index_value(js, arr, idx, &proto_dense_val))
    return proto_dense_val;
  if (!lkp_proto(js, arr, idxstr, idxlen).obj) return Ant_Value_Undefined;
  idxstr[idxlen] = '\0';
  return js_getprop_super(js, get_proto(js, arr), arr, idxstr);
}

static bool array_includes_object_may_have_indexed_props(
  ant_t *js, ant_value_t obj, bool include_dense
) {
  if (!is_object_type(obj) || is_proxy(obj)) return is_proxy(obj);

  ant_value_t as_obj = (vtype(obj) == kTypeFunction) ? js_func_obj(obj) : obj;
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr) return false;
  if (ptr->flags.is_exotic) return true;

  if (include_dense && vtype(as_obj) == kTypeArray) {
    ant_offset_t doff = get_dense_buf(as_obj);
    if (doff) {
      ant_offset_t dense_len = dense_capacity(doff);
      for (ant_offset_t i = 0; i < dense_len; i++) 
        if (!is_empty_slot(dense_get(doff, i))) return true;
    }
  }

  if (!ptr->shape) return false;
  uint32_t shape_count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < shape_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
    if (i >= ptr->prop_count) continue;
    
    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    if (is_array_index(key, (ant_offset_t)key_len)) return true;
  }

  return false;
}

static bool array_includes_can_skip_hole_lookups(ant_t *js, ant_value_t arr) {
  if (array_includes_object_may_have_indexed_props(js, arr, false)) return false;

  ant_value_t proto = get_proto(js, arr);
  for (int depth = 0; is_object_type(proto) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (array_includes_object_may_have_indexed_props(js, proto, true)) return false;
    proto = get_proto(js, proto);
  }

  return true;
}

static ant_value_t array_includes_dense_fast(
  ant_t *js, ant_value_t arr, const array_includes_query_t *query, ant_offset_t len, ant_offset_t start
) {
  if (!array_obj_ptr(arr) || is_proxy(arr)) return Ant_Value_Undefined;

  ant_offset_t doff = get_dense_buf(arr);
  if (!doff) return Ant_Value_Undefined;
  
  ant_value_t *dense = dense_data(doff);
  if (!dense) return Ant_Value_Undefined;

  ant_offset_t dense_len = dense_iterable_length(js, arr);
  if (dense_len != len) return Ant_Value_Undefined;

  if (!array_may_have_holes(arr)) {
    for (ant_offset_t i = start; i < len; i++) 
      if (array_includes_matches(js, query, dense[i])) return mkval(kTypeBool, 1);
    return mkval(kTypeBool, 0);
  }

  for (ant_offset_t i = start; i < len; i++) {
    ant_value_t val = dense[i];
    if (is_empty_slot(val)) return Ant_Value_Undefined;
    if (array_includes_matches(js, query, val)) return mkval(kTypeBool, 1);
  }

  return mkval(kTypeBool, 0);
}

static ant_value_t array_includes_array_slow(
  ant_t *js, ant_value_t arr, const array_includes_query_t *query, ant_offset_t len, ant_offset_t start
) {
  bool skip_hole_lookups =
    query->search_type != kTypeUndefined &&
    array_includes_can_skip_hole_lookups(js, arr);
  
  ant_value_t *dense = NULL;
  ant_offset_t dense_len = 0;

  if (skip_hole_lookups) {
    if (!array_may_have_dense_elements(arr)) return mkval(kTypeBool, 0);
    
    ant_offset_t doff = get_dense_buf(arr);
    if (doff) {
      dense = dense_data(doff);
      dense_len = dense_iterable_length(js, arr);
    }
    
    if (dense) for (ant_offset_t i = start; i < dense_len; i++) {
      ant_value_t val = dense[i];
      if (is_empty_slot(val)) continue;
      if (array_includes_matches(js, query, val)) return mkval(kTypeBool, 1);
    }
    
    return mkval(kTypeBool, 0);
  }

  for (ant_offset_t i = start; i < len; i++) {
    ant_value_t val;
    
    char idxstr[24];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    
    val = array_includes_get_array_index_value(js, arr, i, idxstr, idxlen);
    if (is_err(val)) return val;
    
    if (array_includes_matches(js, query, val)) return mkval(kTypeBool, 1);
  }

  return mkval(kTypeBool, 0);
}

static ant_value_t array_includes_generic(
  ant_t *js, ant_value_t arr, const array_includes_query_t *query, ant_args_t
) {
  ant_value_t len_val = array_includes_length_value(js, arr);
  if (is_err(len_val)) return len_val;

  ant_offset_t len = array_includes_length_from_number(js_to_number(js, len_val));
  if (len == 0) return mkval(kTypeBool, 0);
  
  ant_offset_t start = array_includes_start_index(js, args, nargs, len);
  if (start >= len) return mkval(kTypeBool, 0);

  for (ant_offset_t i = start; i < len; i++) {
    char idxstr[24];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);

    ant_value_t val = array_includes_get_index_value(js, arr, i, idxstr, idxlen);
    if (is_err(val)) return val;
    if (array_includes_matches(js, query, val)) return mkval(kTypeBool, 1);
  }
  
  return mkval(kTypeBool, 0);
}

ant_value_t js_array_includes_call(ant_t *js, ant_value_t arr, ant_args_t) {
  arr = array_method_receiver(js, arr, "includes");
  if (is_err(arr)) return arr;
  
  array_includes_query_t query = array_includes_prepare_query(
    (nargs > 0) ? args[0] : Ant_Value_Undefined
  );

  if (real_array_obj_ptr(arr) && !is_proxy(arr)) {
    ant_offset_t len = get_array_length(js, arr);
    if (len == 0) return mkval(kTypeBool, 0);
    
    ant_offset_t start = array_includes_start_index(js, args, nargs, len);
    if (start >= len) return mkval(kTypeBool, 0);
    
    ant_value_t fast = array_includes_dense_fast(js, arr, &query, len, start);
    if (vtype(fast) != kTypeUndefined) return fast;
    
    return array_includes_array_slow(js, arr, &query, len, start);
  }

  return array_includes_generic(js, arr, &query, args, nargs);
}

bool js_is_array_includes_builtin(ant_value_t func) {
  return vtype(func) == kTypeBuiltin && js_cfunc_same_entrypoint(func, builtin_array_includes);
}

ant_value_t builtin_array_includes(ant_params_t) {
  return js_array_includes_call(js, call_this, args, nargs);
}

static ant_value_t builtin_array_every(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "every");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "every");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : Ant_Value_Undefined;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != Ant_Value_True) continue;
    ant_value_t call_args[3] = { val, Ant_Value_FromNumber((double)i), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(result)) return result;
    if (!js_truthy(js, result)) return mkval(kTypeBool, 0);
  }
  
  return mkval(kTypeBool, 1);
}

static ant_value_t builtin_array_forEach(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "forEach");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "forEach");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : Ant_Value_Undefined;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != Ant_Value_True) continue;
    ant_value_t call_args[3] = { val, Ant_Value_FromNumber((double)i), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(result)) return result;
  }
  
  return Ant_Value_Undefined;
}

static __attribute__((noinline)) ant_value_t array_reverse_generic(ant_t *js, ant_value_t arr, ant_offset_t len) {
  for (ant_offset_t lower = 0; lower < len / 2; lower++) {
    ant_offset_t upper = len - lower - 1;
    ant_value_t lower_val = Ant_Value_Undefined, upper_val = Ant_Value_Undefined;

    ant_value_t lower_has = array_method_has_index(js, arr, lower);
    if (is_err(lower_has)) return lower_has;
    bool lower_exists = js_truthy(js, lower_has);
    if (lower_exists) lower_val = array_method_get_index(js, arr, lower);
    if (is_err(lower_val)) return lower_val;

    ant_value_t upper_has = array_method_has_index(js, arr, upper);
    if (is_err(upper_has)) return upper_has;
    bool upper_exists = js_truthy(js, upper_has);
    if (upper_exists) upper_val = array_method_get_index(js, arr, upper);
    if (is_err(upper_val)) return upper_val;

    ant_value_t step = Ant_Value_Undefined;
    if (upper_exists) {
      step = array_set_index_throw(js, arr, lower, upper_val);
      if (!is_err(step)) step = lower_exists
        ? array_set_index_throw(js, arr, upper, lower_val)
        : array_delete_index_throw(js, arr, upper);
    } else if (lower_exists) {
      step = array_delete_index_throw(js, arr, lower);
      if (!is_err(step)) step = array_set_index_throw(js, arr, upper, lower_val);
    }
    if (is_err(step)) return step;
  }
  return arr;
}

static ant_value_t builtin_array_reverse(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "reverse");
  if (is_err(arr)) return arr;
  
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len) && string_len > 1) return array_string_wrapper_write_error(js, "0");
  
  ant_object_t *frozen = is_proxy(arr) ? NULL : js_obj_ptr(js_as_obj(arr));
  if (frozen && frozen->flags.frozen && !Ant_Array_TryUnshare(js, frozen)) {
    ant_offset_t frozen_len = 0;
    ant_value_t len_result = array_like_length_checked(js, arr, &frozen_len);
    
    if (is_err(len_result)) return len_result;
    if (frozen_len > 1) return Ant_Error_CreateReadOnlyProperty(js, arr, Ant_Value_FromNumber(0));
  }

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  if (len <= 1) return arr;

  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    for (ant_offset_t i = 0; i < len / 2; i++) {
      ant_value_t a = dense_get(doff, i);
      ant_value_t b = dense_get(doff, len - 1 - i);
      dense_set_owned(js, doff, i, b);
      dense_set_owned(js, doff, len - 1 - i, a);
    }
    
    return arr;
  }
  if (is_proxy(arr) || !real_array_obj_ptr(arr)) return array_reverse_generic(js, arr, len);

  for (ant_offset_t lower = 0; lower < len / 2; lower++) {
    ant_offset_t upper = len - lower - 1;
    
    bool lower_exists = arr_has(js, arr, lower);
    bool upper_exists = arr_has(js, arr, upper);
    
    ant_value_t lower_val = lower_exists ? arr_get(js, arr, lower) : Ant_Value_Undefined;
    ant_value_t upper_val = upper_exists ? arr_get(js, arr, upper) : Ant_Value_Undefined;

    if (lower_exists && upper_exists) {
      arr_set(js, arr, lower, upper_val);
      arr_set(js, arr, upper, lower_val);
    } else if (upper_exists) {
      arr_set(js, arr, lower, upper_val);
      arr_del(js, arr, upper);
    } else if (lower_exists) {
      arr_set(js, arr, upper, lower_val);
      arr_del(js, arr, lower);
    }
  }
  
  return arr;
}

static ant_value_t builtin_array_map(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "map");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "map");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : Ant_Value_Undefined;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, len, &intrinsic);
  if (is_err(result)) return result;
  bool skipped = false;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != Ant_Value_True) { skipped = true; continue; }
    ant_value_t call_args[3] = { val, Ant_Value_FromNumber((double)i), arr };
    ant_value_t mapped = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(mapped)) return mapped;
    ant_value_t set = array_result_set(js, result, intrinsic, i, mapped);
    if (is_err(set)) return set;
  }
  
  if (intrinsic && !skipped) array_mark_packed(result);
  return result;
}

static ant_value_t builtin_array_filter(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "filter");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "filter");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : Ant_Value_Undefined;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  
  ant_offset_t result_idx = 0;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != Ant_Value_True) continue;
    ant_value_t call_args[3] = { val, Ant_Value_FromNumber((double)i), arr };
    ant_value_t test = sv_callback_call(js->vm, js, &cb, call_args, 3);
    
    if (is_err(test)) return test;
    if (!js_truthy(js, test)) continue;
    ant_value_t set = array_result_set(js, result, intrinsic, result_idx++, val);
    if (is_err(set)) return set;
  }
  
  return result;
}

static ant_value_t builtin_array_reduce(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "reduce");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "reduce");
  if (is_err(callback)) return callback;
  bool has_initial = (nargs >= 2);
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, Ant_Value_Undefined);
  ant_object_t *elems = array_method_elements(arr);
  
  ant_value_t accumulator = has_initial ? args[1] : Ant_Value_Undefined;
  bool first = !has_initial;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != Ant_Value_True) continue;
    if (first) { accumulator = val; first = false; continue; }
    ant_value_t call_args[4] = { accumulator, val, Ant_Value_FromNumber((double)i), arr };
    accumulator = sv_callback_call(js->vm, js, &cb, call_args, 4);
    if (is_err(accumulator)) return accumulator;
  }
  
  if (first) return js_mkerr(js, "reduce of empty array with no initial value");
  return accumulator;
}

static inline ant_value_t *packed_array_data_for_length(ant_t *js, ant_value_t arr, ant_offset_t len) {
  if (vtype(arr) != kTypeArray || array_may_have_holes(arr)) return NULL;

  ant_offset_t doff = get_dense_buf(arr);
  ant_value_t *dense = doff ? dense_data(doff) : NULL;
  if (!dense || dense_iterable_length(js, arr) != len) return NULL;

  for (ant_offset_t i = 0; i < len; i++)
    if (is_empty_slot(dense[i])) return NULL;

  return dense;
}

static inline ant_value_t flat_should_spread(ant_t *js, ant_value_t val, int depth, bool *out) {
  *out = false;
  if (depth <= 0) return Ant_Value_Undefined;

  if (real_array_obj_ptr(val)) {
    *out = true;
    return Ant_Value_Undefined;
  }

  return is_proxy(val) ? js_is_array_value_checked(js, val, out) : Ant_Value_Undefined;
}

static constexpr int FLAT_UNCHECKED_DEPTH = 1024;

static inline ant_value_t flat_helper(ant_t *js, ant_value_t arr, ant_value_t result, bool fresh, ant_offset_t *result_idx, int depth);

static inline __attribute__((always_inline)) ant_value_t flat_into(
  ant_t *js, ant_value_t arr, bool input_is_array, ant_offset_t len,
  ant_value_t result, bool fresh, ant_offset_t *result_idx, int depth
) {
  if (len == 0) return Ant_Value_Undefined;

  ant_value_t *dense = packed_array_data_for_length(js, arr, len);
  if (dense) {
    for (ant_offset_t i = 0; i < len; i++) {
      bool spread = false;
      ant_value_t spread_res = flat_should_spread(js, dense[i], depth, &spread);
      if (is_err(spread_res)) return spread_res;
      if (!spread) {
        ant_value_t set = array_result_set(js, result, fresh, (*result_idx)++, dense[i]);
        if (is_err(set)) return set;
        continue;
      }

      if (depth > FLAT_UNCHECKED_DEPTH && sv_check_c_stack_overflow(js)) return Ant_Error_CreateStackOverflow(js);
      ant_value_t flat_res = flat_helper(js, dense[i], result, fresh, result_idx, depth - 1);
      if (is_err(flat_res)) return flat_res;
    }
    
    return Ant_Value_Undefined;
  }

  for (ant_offset_t i = 0; i < len; i++) {
    if (input_is_array) {
      if (!arr_has(js, arr, i)) continue;
    } else {
      ant_value_t has = array_method_has_index(js, arr, i);
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) continue;
    }

    ant_value_t val = input_is_array ? arr_get(js, arr, i) : array_method_get_index(js, arr, i);
    if (is_err(val)) return val;

    bool spread = false;
    ant_value_t spread_res = flat_should_spread(js, val, depth, &spread);
    if (is_err(spread_res)) return spread_res;
    
    if (!spread) {
      ant_value_t set = array_result_set(js, result, fresh, (*result_idx)++, val);
      if (is_err(set)) return set;
      continue;
    }

    if (depth > FLAT_UNCHECKED_DEPTH && sv_check_c_stack_overflow(js)) return Ant_Error_CreateStackOverflow(js);
    ant_value_t flat_res = flat_helper(js, val, result, fresh, result_idx, depth - 1);
    if (is_err(flat_res)) return flat_res;
  }

  return Ant_Value_Undefined;
}

static inline ant_value_t flat_helper(ant_t *js, ant_value_t arr, ant_value_t result, bool fresh, ant_offset_t *result_idx, int depth) {
  bool input_is_array = real_array_obj_ptr(arr) != NULL;
  ant_offset_t len = input_is_array ? get_array_length(js, arr) : 0;
  if (!input_is_array) {
    ant_value_t len_result = proxy_aware_length(js, arr, &len);
    if (is_err(len_result)) return len_result;
  }
  return flat_into(js, arr, input_is_array, len, result, fresh, result_idx, depth);
}

static inline ant_value_t flat_append_mapped_value(ant_t *js, ant_value_t mapped, ant_value_t result, bool fresh, ant_offset_t *result_idx) {
  bool spread = false;
  ant_value_t spread_res = flat_should_spread(js, mapped, 1, &spread);
  if (is_err(spread_res)) return spread_res;
  if (!spread) return array_result_set(js, result, fresh, (*result_idx)++, mapped);

  return flat_helper(js, mapped, result, fresh, result_idx, 0);
}

static __attribute__((noinline)) ant_value_t array_flat_depth_slow(ant_t *js, ant_value_t *args, int *depth) {
  double depth_num = vtype(args[0]) == kTypeNumber ? Ant_Value_AsNumberUnchecked(args[0]) : js_to_number(js, args[0]);
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  *depth = !(depth_num >= 1) ? 0 : depth_num >= INT32_MAX ? INT32_MAX : (int)depth_num;
  return Ant_Value_Undefined;
}

static inline __attribute__((always_inline)) ant_value_t array_flat_depth(ant_t *js, ant_value_t *args, int nargs, int *depth) {
  *depth = 1;
  if (nargs < 1 || vtype(args[0]) == kTypeUndefined) return Ant_Value_Undefined;
  return array_flat_depth_slow(js, args, depth);
}

static __attribute__((noinline)) ant_value_t array_flat_generic(ant_t *js, ant_value_t arr, ant_value_t *args, int nargs) {
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  int depth;
  ant_value_t depth_err = array_flat_depth(js, args, nargs, &depth);
  if (is_err(depth_err)) return depth_err;

  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  ant_offset_t result_idx = 0;

  ant_value_t flat_res = flat_into(js, arr, false, len, result, intrinsic, &result_idx, depth);
  if (is_err(flat_res)) return flat_res;
  return result;
}

static ant_value_t builtin_array_flat(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "flat");
  if (is_err(arr)) return arr;
  if (__builtin_expect(vtype(arr) != kTypeArray && !real_array_obj_ptr(arr), 0)) return array_flat_generic(js, arr, args, nargs);
  
  int depth;
  ant_value_t depth_err = array_flat_depth(js, args, nargs, &depth);
  if (is_err(depth_err)) return depth_err;
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  ant_offset_t result_idx = 0;
  
  ant_value_t flat_res = flat_helper(js, arr, result, intrinsic, &result_idx, depth);
  if (is_err(flat_res)) return flat_res;
  return result;
}

static ant_value_t builtin_array_concat(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "concat");
  if (is_err(arr)) return arr;
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  
  ant_offset_t result_idx = 0;
  for (int a = -1; a < nargs; a++) {
    ant_value_t arg = (a < 0) ? arr : args[a];
    bool spreadable = false;
    
    ant_object_t *plain_array = real_array_obj_ptr(arg);
    if (plain_array && ant_object_proto(plain_array) == js->sym.array_proto && ant_protector_intact(js, ANT_PROTECTOR_CONCAT_SPREADABLE)) spreadable = true;
    else if (vtype(arg) == kTypeArray || vtype(arg) == kTypeObject) {
      bool array_default_spreadable = real_array_obj_ptr(arg) != NULL;
      if (!array_default_spreadable && is_proxy(arg)) {
        ant_value_t target = proxy_read_target(js, arg);
        array_default_spreadable = real_array_obj_ptr(target) != NULL;
      }
      
      ant_value_t spread_val = js_get_sym(js, arg, js->sym.isConcatSpreadable_sym);
      if (is_err(spread_val)) return spread_val;
      if (vtype(spread_val) == kTypeUndefined) spreadable = array_default_spreadable;
      else spreadable = js_truthy(js, spread_val);
    }
    
    if (spreadable) {
      ant_offset_t arg_len = 0;
      ant_value_t len_result = array_like_length_checked(js, arg, &arg_len);
      if (is_err(len_result)) return len_result;
      
      ant_object_t *src = array_dense_source(js, arg);
      if (src && (arg_len > src->u.array.len || arg_len > src->u.array.cap)) src = NULL;
      if (__builtin_expect(arg_len > UINT32_MAX, 0)) {
        if ((double)result_idx + (double)arg_len > 9007199254740991.0) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid array length");
        continue;
      }
      ant_offset_t rdoff = real_array_obj_ptr(result) && (intrinsic || array_result_extensible(result)) ? get_dense_buf(result) : 0;
      if (rdoff && !Ant_Array_Unshare(js, dense_obj(rdoff))) return js_mkerr(js, "oom");
      if (rdoff && src && dense_capacity(rdoff) < result_idx + arg_len)
        rdoff = dense_grow(js, result, result_idx + arg_len);
      
      for (ant_offset_t i = 0; i < arg_len; i++, result_idx++) {
        ant_value_t elem;
        if (src) {
          elem = src->u.array.data[i];
          if (is_empty_slot(elem)) continue;
        } else {
          ant_value_t has = array_method_has_index(js, arg, i);
          if (is_err(has)) return has;
          if (!js_truthy(js, has)) continue;
          elem = array_method_get_index(js, arg, i);
          if (is_err(elem)) return elem;
        }
        if (rdoff && result_idx < dense_capacity(rdoff) && result_idx <= get_array_length(js, result)) {
          dense_set_owned(js, rdoff, result_idx, elem);
          if (result_idx == get_array_length(js, result)) array_len_set(js, result, result_idx + 1);
        } else {
          ant_value_t set = array_result_set(js, result, intrinsic, result_idx, elem);
          if (is_err(set)) return set;
        }
      }
    } else {
      ant_value_t set = array_result_set(js, result, intrinsic, result_idx++, arg);
      if (is_err(set)) return set;
    }
  }
  
  if (real_array_obj_ptr(result)) {
    if (get_array_length(js, result) < result_idx) {
      array_mark_may_have_holes(result);
      array_len_set(js, result, result_idx);
    }
  } else {
    ant_value_t set = js_setprop(js, result, js->length_str, Ant_Value_FromNumber((double)result_idx));
    if (is_err(set)) return set;
  }
  
  return result;
}

static ant_value_t builtin_array_at(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "at");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  ant_value_t rel_val = nargs > 0 ? js_to_numeric(js, args[0]) : Ant_Value_FromNumber(0);
  if (is_err(rel_val)) return rel_val;
  if (vtype(rel_val) == kTypeBigInt)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
  
  double rel = Ant_Value_AsNumberUnchecked(rel_val);
  rel = isnan(rel) ? 0 : trunc(rel);
  double k = rel < 0 ? (double)len + rel : rel;
  if (k < 0 || k >= (double)len) return Ant_Value_Undefined;
  
  return array_method_get_index(js, arr, (ant_offset_t)k);
}

static ant_value_t builtin_array_fill(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "fill");
  if (is_err(arr)) return arr;

  ant_value_t value = nargs >= 1 ? args[0] : Ant_Value_Undefined;

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  ant_offset_t start = 0, end = len;
  ant_value_t arg_err = array_relative_arg(js, args, nargs, 1, len, 0, &start);
  if (is_err(arg_err)) return arg_err;
  arg_err = array_relative_arg(js, args, nargs, 2, len, len, &end);
  if (is_err(arg_err)) return arg_err;
  
  ant_offset_t string_len;
  if (start < end && array_string_wrapper_length(js, arr, &string_len)) {
    char key[24];
    uint_to_str(key, sizeof(key), (uint64_t)start);
    return array_string_wrapper_write_error(js, key);
  }
  
  ant_object_t *dense = real_array_obj_ptr(arr);
  if (!is_proxy(arr) && array_dense_covers(dense, end) && array_dense_writable(js, dense)) {
    ant_offset_t doff = get_dense_buf(arr);
    for (ant_offset_t i = start; i < end; i++) dense_set_owned(js, doff, i, value);
    return arr;
  }
  
  for (ant_offset_t i = start; i < end; i++) {
    ant_value_t set = array_set_index_throw(js, arr, i, value);
    if (is_err(set)) return set;
  }
  
  return arr;
}

static ant_value_t array_find_impl(ant_native_params_t, ant_value_t call_this, bool return_index, const char *name) {
  ant_value_t arr = array_method_receiver(js, call_this, name);
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : Ant_Value_Undefined;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  if (len == 0) return return_index ? Ant_Value_FromNumber(-1) : Ant_Value_Undefined;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = array_method_get_index(js, arr, i);
    if (is_err(val)) return val;
    
    ant_value_t call_args[3] = { val, Ant_Value_FromNumber((double)i), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return return_index ? Ant_Value_FromNumber((double)i) : val;
  }
  
  return return_index ? Ant_Value_FromNumber(-1) : Ant_Value_Undefined;
}

static ant_value_t builtin_array_find(ant_params_t) {
  return array_find_impl(js, args, nargs, call_this, false, "find");
}

static ant_value_t builtin_array_findIndex(ant_params_t) {
  return array_find_impl(js, args, nargs, call_this, true, "findIndex");
}

static ant_value_t array_find_last_impl(ant_native_params_t, ant_value_t call_this, bool return_index, const char *name) {
  ant_value_t arr = array_method_receiver(js, call_this, name);
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : Ant_Value_Undefined;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  if (len == 0) return return_index ? Ant_Value_FromNumber(-1) : Ant_Value_Undefined;
  
  for (ant_offset_t i = len; i > 0; i--) {
    ant_value_t val = array_method_get_index(js, arr, i - 1);
    if (is_err(val)) return val;
    
    ant_value_t call_args[3] = { val, Ant_Value_FromNumber((double)(i - 1)), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return return_index ? Ant_Value_FromNumber((double)(i - 1)) : val;
  }
  
  return return_index ? Ant_Value_FromNumber(-1) : Ant_Value_Undefined;
}

static ant_value_t builtin_array_findLast(ant_params_t) {
  return array_find_last_impl(js, args, nargs, call_this, false, "findLast");
}

static ant_value_t builtin_array_findLastIndex(ant_params_t) {
  return array_find_last_impl(js, args, nargs, call_this, true, "findLastIndex");
}

static ant_value_t builtin_array_flatMap(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "flatMap");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "flatMap");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : Ant_Value_Undefined;
  bool input_is_array = real_array_obj_ptr(arr) != NULL;
  ant_offset_t len = input_is_array ? get_array_length(js, arr) : 0;
  if (!input_is_array) {
    ant_value_t len_result = proxy_aware_length(js, arr, &len);
    if (is_err(len_result)) return len_result;
  }
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  ant_offset_t result_idx = 0;
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t elem;
    ant_value_t has = array_method_element(js, arr, elems, i, &elem);
    if (is_err(has)) return has;
    if (has != Ant_Value_True) continue;
    ant_value_t call_args[3] = { elem, Ant_Value_FromNumber((double)i), arr };
    ant_value_t mapped = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(mapped)) return mapped;
    ant_value_t flat_res = flat_append_mapped_value(js, mapped, result, intrinsic, &result_idx);
    if (is_err(flat_res)) return flat_res;
  }
  
  return result;
}

static inline bool array_receiver_is_proxy(ant_value_t arr) {
  return vtype(arr) == kTypeObject && ((const ant_object_t *)vptr(arr))->flags.is_exotic && is_proxy(arr);
}

static __attribute__((noinline)) ant_value_t array_search_proxy(
  ant_t *js, ant_value_t arr, ant_value_t search, ant_offset_t from, ant_offset_t to, bool backward
) {
  for (ant_offset_t n = from; n < to; n++) {
    ant_offset_t i = backward ? to - 1 - (n - from) : n;
    ant_value_t has = array_method_has_index(js, arr, i);
    if (is_err(has)) return has;
    if (!js_truthy(js, has)) continue;
    ant_value_t elem = array_method_get_index(js, arr, i);
    if (is_err(elem)) return elem;
    if (strict_eq_values(js, elem, search)) return Ant_Value_FromNumber((double)i);
  }
  return Ant_Value_FromNumber(-1);
}

static ant_value_t builtin_array_indexOf(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "indexOf");
  if (is_err(arr)) return arr;

  if (nargs == 0) return Ant_Value_FromNumber(-1);  
  ant_value_t search = args[0];
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  ant_offset_t start = 0;
  if (len == 0) return Ant_Value_FromNumber(-1);
  ant_value_t arg_err = array_relative_arg(js, args, nargs, 1, len, 0, &start);
  if (is_err(arg_err)) return arg_err;
  
  ant_object_t *dense = array_dense_source(js, arr);
  if (dense && len <= dense->u.array.len && len <= dense->u.array.cap) {
    for (ant_offset_t i = start; i < len; i++) {
      ant_value_t elem = dense->u.array.data[i];
      if (!is_empty_slot(elem) && array_search_eq(js, elem, search)) return Ant_Value_FromNumber((double)i);
    }
    return Ant_Value_FromNumber(-1);
  }
  
  if (array_receiver_is_proxy(arr)) return array_search_proxy(js, arr, search, start, len, false);
  for (ant_offset_t i = start; i < len; i++) {
    ant_value_t elem = array_method_get_index(js, arr, i);
    if (is_err(elem)) return elem;
    if (vtype(elem) == kTypeUndefined) {
      ant_value_t has = array_method_has_index(js, arr, i);
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) continue;
    }
    if (strict_eq_values(js, elem, search)) return Ant_Value_FromNumber((double)i);
  }
  return Ant_Value_FromNumber(-1);
}

static ant_value_t builtin_array_lastIndexOf(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "lastIndexOf");
  if (is_err(arr)) return arr;
  if (nargs == 0) return Ant_Value_FromNumber(-1);
  
  ant_value_t search = args[0];
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  if (len == 0) return Ant_Value_FromNumber(-1);
  ant_offset_t start = len - 1;
  if (nargs >= 2) {
    double from;
    ant_value_t arg_err = array_integer_arg(js, args, nargs, 1, 0, &from);
    if (is_err(arg_err)) return arg_err;
    if (from < 0) from += (double)len;
    if (from < 0) return Ant_Value_FromNumber(-1);
    if (from < (double)start) start = (ant_offset_t)from;
  }
  
  ant_object_t *dense = array_dense_source(js, arr);
  if (dense && len <= dense->u.array.len && len <= dense->u.array.cap) {
    for (int i = (int)start; i >= 0; i--) {
      ant_value_t elem = dense->u.array.data[i];
      if (!is_empty_slot(elem) && array_search_eq(js, elem, search)) return Ant_Value_FromNumber((double)i);
    }
    return Ant_Value_FromNumber(-1);
  }
  
  if (array_receiver_is_proxy(arr)) return array_search_proxy(js, arr, search, 0, start + 1, true);
  for (ant_offset_t i = start + 1; i-- > 0;) {
    ant_value_t elem = array_method_get_index(js, arr, i);
    if (is_err(elem)) return elem;
    if (vtype(elem) == kTypeUndefined) {
      ant_value_t has = array_method_has_index(js, arr, i);
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) continue;
    }
    if (strict_eq_values(js, elem, search)) return Ant_Value_FromNumber((double)i);
  }
  return Ant_Value_FromNumber(-1);
}

static ant_value_t builtin_array_reduceRight(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "reduceRight");
  if (is_err(arr)) return arr;
  if (nargs == 0 || vtype(args[0]) != kTypeFunction) {
    return js_mkerr(js, "reduceRight requires a function argument");
  }
  
  ant_value_t callback = args[0];
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, Ant_Value_Undefined);
  
  ant_object_t *elems = array_method_elements(arr);
  ant_value_t accumulator = nargs >= 2 ? args[1] : Ant_Value_Undefined;
  bool first = nargs < 2;
  
  for (ant_offset_t i = len; i > 0; i--) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i - 1, &val);
    if (is_err(has)) return has;
    if (has != Ant_Value_True) continue;
    if (first) { accumulator = val; first = false; continue; }
    ant_value_t call_args[4] = { accumulator, val, Ant_Value_FromNumber((double)(i - 1)), arr };
    accumulator = sv_callback_call(js->vm, js, &cb, call_args, 4);
    if (is_err(accumulator)) return accumulator;
  }
  
  if (first) return js_mkerr_typed(js, JS_ERR_TYPE, "reduceRight of empty array with no initial value");
  return accumulator;
}

static ant_value_t builtin_array_shift(ant_params_t) {
  ant_object_t *fast = array_mutable_dense(js, call_this);
  if (__builtin_expect(fast != NULL, 1)) {
    uint32_t len = fast->u.array.len;
    if (len == 0) return Ant_Value_Undefined;
    if (__builtin_expect(fast->flags.may_have_holes && !array_proto_chain_plain(js, fast), 0))
      return array_shift_generic(js, call_this);
    ant_value_t *data = fast->u.array.data;
    ant_value_t first = data[0];
    memmove(&data[0], &data[1], sizeof(ant_value_t) * (size_t)(len - 1));
    gc_elements_moved(fast);
    data[len - 1] = T_EMPTY;
    fast->u.array.len = len - 1;
    return is_empty_slot(first) ? Ant_Value_Undefined : first;
  }
  
  if (!Ant_Array_UnshareValue(js, call_this)) return js_mkerr(js, "oom");
  ant_value_t arr = array_method_receiver(js, call_this, "shift");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");
  if (js_is_arguments(arr)) return arguments_shift(js, arr);

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  if (len == 0) return array_length_readonly(js, arr) ? array_shift_generic(js, arr) : Ant_Value_Undefined;

  if (is_proxy(arr)) {
    ant_value_t read_from = proxy_read_target(js, arr);
    ant_value_t first = proxy_get(js, arr, "0", 1);
    if (is_err(first)) return first;

    for (ant_offset_t i = 1; i < len; i++) {
      char src[16];
      size_t srclen = uint_to_str(src, sizeof(src), (unsigned)i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i - 1));
      if (arr_has(js, read_from, i)) {
        ant_value_t elem = proxy_get(js, arr, src, srclen);
        if (is_err(elem)) return elem;
        js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
      } else {
        ant_value_t del = proxy_delete(js, arr, dst, dstlen);
        if (is_err(del)) return del;
      }
    }

    ant_value_t del = proxy_delete_index(js, arr, len - 1);
    if (is_err(del)) return del;
    js_setprop(js, arr, js->length_str, Ant_Value_FromNumber((double)(len - 1)));
    return first;
  }

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr)) {
    ant_offset_t d_len = dense_iterable_length(js, arr);
    ant_object_t *dense = dense_obj(doff);
    if (len != d_len || dense->flags.sealed || array_length_readonly(js, arr)) goto shift_slow;
    if (dense->flags.may_have_holes && !array_proto_chain_plain(js, dense)) goto shift_slow;
    if (d_len == 0) return Ant_Value_Undefined;
    ant_value_t *d = dense_data(doff);
    if (!d) return Ant_Value_Undefined;
    ant_value_t first = dense_get(doff, 0);
    if (is_empty_slot(first)) first = Ant_Value_Undefined;
    memmove(&d[0], &d[1], sizeof(ant_value_t) * (size_t)(d_len - 1));
    gc_elements_moved(dense_obj(doff));
    dense_set_owned(js, doff, d_len - 1, T_EMPTY);
    array_len_set(js, arr, len - 1);
    return first;
  }

  shift_slow:
  return array_shift_generic(js, arr);
}

static ant_value_t builtin_array_unshift(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "unshift");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");
  if (array_length_readonly(js, arr) && !array_refuses_add(arr, nargs)) return array_length_readonly_error(js);
  if (js_is_arguments(arr)) return arguments_unshift(js, arr, args, nargs);

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  if (array_refuses_add(arr, nargs)) {
    if (!Ant_Array_UnshareForAppend(js, arr, (uint32_t)nargs)) return js_mkerr(js, "oom");
    if (array_refuses_add(arr, nargs)) return array_add_refused_error(js, len ? len - 1 + (ant_offset_t)nargs : 0);
  }

  if (is_proxy(arr)) {
    ant_value_t read_from = proxy_read_target(js, arr);
    for (int i = (int)len - 1; i >= 0; i--) {
      char src[16];
      size_t srclen = uint_to_str(src, sizeof(src), (unsigned)i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i + nargs));
      if (arr_has(js, read_from, (ant_offset_t)i)) {
        ant_value_t elem = proxy_get(js, arr, src, srclen);
        if (is_err(elem)) return elem;
        js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
      } else {
        ant_value_t del = proxy_delete(js, arr, dst, dstlen);
        if (is_err(del)) return del;
      }
    }

    for (int i = 0; i < nargs; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      js_setprop(js, arr, js_mkstr(js, idxstr, idxlen), args[i]);
    }

    ant_offset_t new_len = len + nargs;
    js_setprop(js, arr, js->length_str, Ant_Value_FromNumber((double)new_len));
    return Ant_Value_FromNumber((double)new_len);
  }

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr)) {
    ant_offset_t d_len = dense_iterable_length(js, arr);
    if (len != d_len) goto unshift_slow;
    ant_offset_t new_len = len + nargs;
    ant_offset_t cap = dense_capacity(doff);
    if (new_len > cap) {
      doff = dense_grow(js, arr, new_len);
      if (doff == 0) return js_mkerr(js, "oom");
    }
    ant_value_t *d = dense_data(doff);
    if (!d) return js_mkerr(js, "oom");
    memmove(&d[nargs], &d[0], sizeof(ant_value_t) * (size_t)d_len);
    gc_elements_moved(dense_obj(doff));
    for (int i = 0; i < nargs; i++)
      dense_set_owned(js, doff, (ant_offset_t)i, args[i]);
    array_len_set(js, arr, new_len);
    return Ant_Value_FromNumber((double) new_len);
  }

  unshift_slow:
  ant_value_t read_from = is_proxy(arr) ? proxy_read_target(js, arr) : arr;

  for (int i = (int)len - 1; i >= 0; i--) {
    if (arr_has(js, read_from, (ant_offset_t)i)) {
      ant_value_t elem = arr_get(js, read_from, (ant_offset_t)i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i + nargs));
      js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
    }
  }
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    ant_value_t key = js_mkstr(js, idxstr, idxlen);
    js_setprop(js, arr, key, args[i]);
  }
  
  ant_offset_t new_len = len + nargs;
  if (array_obj_ptr(arr)) array_len_set(js, arr, new_len);
  else js_setprop(js, arr, js->length_str, Ant_Value_FromNumber((double) new_len));
  
  return Ant_Value_FromNumber((double) new_len);
}

static ant_value_t builtin_array_some(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "some");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "some");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : Ant_Value_Undefined;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  if (len == 0) return mkval(kTypeBool, 0);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != Ant_Value_True) continue;
    
    ant_value_t call_args[3] = { val, Ant_Value_FromNumber((double)i), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return mkval(kTypeBool, 1);
  }
  
  return mkval(kTypeBool, 0);
}

static constexpr ant_offset_t SORT_INLINE_KEYS = 32;

static inline uint64_t sort_int_string_key(int32_t value) {
  uint64_t digits = value < 0 ? (uint64_t)-(int64_t)value : (uint64_t)value;
  uint64_t count = 1;
  for (uint64_t v = digits; v >= 10; v /= 10) count++;
  for (uint64_t d = count; d < 10; d++) digits *= 10;
  return ((uint64_t)(value >= 0) << 40) | (digits << 4) | count;
}

static inline int sort_string_cmp(ant_t *js, ant_value_t a, ant_value_t b) {
  ant_offset_t len_a, len_b;
  const char *sa = (const char *)(uintptr_t)vstr(js, a, &len_a);
  const char *sb = (const char *)(uintptr_t)vstr(js, b, &len_b);
  return str_compare_utf16(sa, (size_t)len_a, sb, (size_t)len_b);
}

static inline bool array_sort_writes_dense(ant_value_t arr, ant_offset_t len) {
  ant_object_t *ptr = real_array_obj_ptr(arr);
  return
    !is_proxy(arr) && array_dense_storage(ptr) &&
    ptr->u.array.len == len && ptr->u.array.cap >= len && ptr->flags.extensible && !ptr->flags.sealed;
}

static __attribute__((noinline)) ant_value_t array_sort_write_generic(
  ant_t *js, ant_value_t arr, const ant_value_t *vals, ant_offset_t count, ant_offset_t filled, ant_offset_t len
) {
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t step = i < filled
      ? array_set_index_throw(js, arr, i, i < count ? vals[i] : Ant_Value_Undefined)
      : array_delete_index_throw(js, arr, i);
    if (is_err(step)) return step;
  }
  return arr;
}

static ant_value_t builtin_array_sort(ant_params_t) {
  ant_value_t arr = call_this;
  ant_value_t compareFn = Ant_Value_Undefined;
  
  ant_value_t result = arr;
  ant_value_t *vals = NULL, *keys = NULL, *temp_vals = NULL, *temp_keys = NULL;
  
  uint64_t *int_keys = NULL, *temp_int_keys = NULL;
  uint64_t inline_int_keys[SORT_INLINE_KEYS * 2];
  ant_value_t inline_vals[SORT_INLINE_KEYS * 2];
  ant_offset_t count = 0, undef_count = 0, len = 0;
  
  gc_temp_root_scope_t temp_scope = {0};
  bool temp_scope_active = false;
  
  arr = array_method_receiver(js, arr, "sort");
  if (is_err(arr)) return arr;
  result = arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len) && string_len > 1) return array_string_wrapper_write_error(js, "0");
  
  if (nargs >= 1) {
    uint8_t t = vtype(args[0]);
    if (t == kTypeFunction || t == kTypeBuiltin) compareFn = args[0];
    else if (t != kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "compareFn must be a function or undefined");
  }

  gc_temp_root_scope_begin(js, &temp_scope);
  temp_scope_active = true;
  
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_scope, arr))) goto oom;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_scope, compareFn))) goto oom;
  
  ant_value_t len_result = array_like_length_checked(js, arr, &len);
  if (is_err(len_result)) { result = len_result; goto done; }
  if (len < 2) goto done;
  
  ant_object_t *target = is_proxy(arr) ? NULL : js_obj_ptr(js_as_obj(arr));
  if (target && target->flags.frozen && !Ant_Array_TryUnshare(js, target) && len > 1) { 
    result = Ant_Error_CreateReadOnlyProperty(js, arr, Ant_Value_FromNumber(0)); 
    goto done; 
  }
  
  vals = len <= SORT_INLINE_KEYS ? inline_vals : malloc(len * sizeof(ant_value_t));
  if (!vals) goto oom;
  
  ant_object_t *dense = array_dense_source(js, arr);
  bool packed = dense && len <= dense->u.array.len && len <= dense->u.array.cap;
  if (packed) {
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t v = dense->u.array.data[i];
      if (is_empty_slot(v)) continue;
      if (vtype(v) == kTypeUndefined) undef_count++;
      else vals[count++] = v;
    }
  } else for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t has = array_method_has_index(js, arr, i);
    if (is_err(has)) { result = has; goto done; }
    if (has != Ant_Value_True) continue;
    
    ant_value_t v = array_method_get_index(js, arr, i);
    
    if (is_err(v)) { result = v; goto done; }
    if (vtype(v) == kTypeUndefined) undef_count++;
    else vals[count++] = v;
    if (!gc_temp_root_push(&temp_scope, v)) goto oom;
  }
  
  if (count <= 1) goto writeback;
  for (ant_offset_t i = 0; packed && i < count; i++)
    if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_scope, vals[i]))) goto oom;
  
  bool use_cmp = vtype(compareFn) != kTypeUndefined;
  bool use_ints = !use_cmp;
  
  for (ant_offset_t i = 0; use_ints && i < count; i++) use_ints = 
    vtype(vals[i]) == kTypeNumber && 
    Ant_Value_AsNumberUnchecked(vals[i]) == (double)(int32_t)Ant_Value_AsNumberUnchecked(vals[i]);
  
  bool use_keys = !use_cmp && !use_ints;
  sv_callback_t cb = sv_callback_prepare(js, compareFn, Ant_Value_Undefined);

  if (use_ints) {
    int_keys = count <= SORT_INLINE_KEYS ? inline_int_keys : malloc(count * sizeof(*int_keys));
    temp_int_keys = count <= SORT_INLINE_KEYS ? inline_int_keys + SORT_INLINE_KEYS : malloc(count * sizeof(*temp_int_keys));
    if (!int_keys || !temp_int_keys) goto oom;
    for (ant_offset_t i = 0; i < count; i++) int_keys[i] = sort_int_string_key((int32_t)Ant_Value_AsNumberUnchecked(vals[i]));
  }

  if (use_keys) {
    keys = malloc(count * sizeof(ant_value_t));
    if (!keys) goto oom;
    
    for (ant_offset_t i = 0; i < count; i++) {
      ant_value_t key = js_tostring_val(js, vals[i]);
      if (is_err(key)) {
        result = key;
        goto done;
      }
      
      keys[i] = key;
      if (!gc_temp_root_push(&temp_scope, key)) goto oom;
    }
  }
  
  temp_vals = vals == inline_vals ? inline_vals + SORT_INLINE_KEYS : malloc(count * sizeof(ant_value_t));
  if (use_keys) temp_keys = malloc(count * sizeof(ant_value_t));
  if (!temp_vals || (use_keys && !temp_keys)) goto oom;
  
  for (ant_offset_t width = 1; width < count; width *= 2) {
    for (ant_offset_t left = 0; left < count; left += width * 2) {
      ant_offset_t mid = left + width;
      ant_offset_t right = (mid + width < count) ? mid + width : count;
      if (mid >= count) break;
      
      ant_offset_t i = left, j = mid, k = 0;
      while (i < mid && j < right) {
        int cmp;
        if (use_ints) cmp = (int_keys[i] > int_keys[j]) - (int_keys[i] < int_keys[j]);
        else if (use_keys) cmp = sort_string_cmp(js, keys[i], keys[j]);
        else {
          ant_value_t call_args[2] = { vals[i], vals[j] };
          ant_value_t order = sv_callback_call(js->vm, js, &cb, call_args, 2);
          if (vtype(order) != kTypeNumber && !is_err(order)) order = js_to_numeric(js, order);
          if (!is_err(order) && vtype(order) == kTypeBigInt)
            order = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
          if (is_err(order)) { result = order; goto done; }
          double d = Ant_Value_AsNumberUnchecked(order);
          cmp = (d > 0) - (d < 0);
        }
        if (cmp <= 0) {
          temp_vals[k] = vals[i];
          if (use_keys) temp_keys[k] = keys[i];
          if (use_ints) temp_int_keys[k] = int_keys[i];
          k++; i++;
        } else {
          temp_vals[k] = vals[j];
          if (use_keys) temp_keys[k] = keys[j];
          if (use_ints) temp_int_keys[k] = int_keys[j];
          k++; j++;
        }
      }
      while (i < mid) {
        temp_vals[k] = vals[i];
        if (use_keys) temp_keys[k] = keys[i];
        if (use_ints) temp_int_keys[k] = int_keys[i];
        k++; i++;
      }
      while (j < right) {
        temp_vals[k] = vals[j];
        if (use_keys) temp_keys[k] = keys[j];
        if (use_ints) temp_int_keys[k] = int_keys[j];
        k++; j++;
      }
      
      memcpy(&vals[left], temp_vals, k * sizeof(ant_value_t));
      if (use_keys) memcpy(&keys[left], temp_keys, k * sizeof(ant_value_t));
      if (use_ints) memcpy(&int_keys[left], temp_int_keys, k * sizeof(*int_keys));
    }
  }
  
writeback:;
  ant_offset_t filled = count + undef_count;
  if (array_sort_writes_dense(arr, len)) {
    ant_offset_t doff = get_dense_buf(arr);
    for (ant_offset_t i = 0; i < len; i++)
      dense_set_owned(js, doff, i, i < count ? vals[i] : i < filled ? Ant_Value_Undefined : T_EMPTY);
    if (filled < len) array_mark_may_have_holes(arr);
    result = arr;
  } else result = array_sort_write_generic(js, arr, vals, count, filled, len);
  goto done;
  
oom:
  result = Ant_Error_CreateOutOfMemory(js);

done:
  if (temp_scope_active) gc_temp_root_scope_end(&temp_scope);
  
  if (int_keys != inline_int_keys) {
    free(temp_int_keys);
    free(int_keys);
  }
  
  free(temp_keys);
  free(keys);
  if (vals != inline_vals) {
    free(temp_vals);
    free(vals);
  }
  
  return result;
}

static __attribute__((noinline)) ant_value_t array_splice_large(
  ant_t *js, ant_value_t arr, ant_offset_t len, ant_offset_t start, ant_value_t *args, int nargs
) {
  ant_offset_t delete_count = nargs == 0 ? 0 : len - start;
  if (nargs >= 2) {
    double requested;
    ant_value_t arg_err = array_integer_arg(js, args, nargs, 1, 0, &requested);
    if (is_err(arg_err)) return arg_err;
    if (requested < 0) requested = 0;
    if (requested < (double)delete_count) delete_count = (ant_offset_t)requested;
  }

  ant_offset_t insert_count = nargs > 2 ? (ant_offset_t)(nargs - 2) : 0;
  if ((double)len + (double)insert_count - (double)delete_count > 9007199254740991.0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Splicing would exceed the maximum array-like length");

  bool intrinsic = false;
  ant_value_t removed = array_species_create(js, arr, delete_count, &intrinsic);
  if (is_err(removed)) return removed;
  return array_splice_generic(
    js, arr, len, start, delete_count,
    insert_count > 0 ? &args[2] : NULL, insert_count, removed, intrinsic
  );
}

static ant_value_t builtin_array_splice(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "splice");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);

  ant_offset_t start_at = 0;
  ant_value_t arg_err = array_relative_arg(js, args, nargs, 0, len, 0, &start_at);
  if (is_err(arg_err)) return arg_err;
  if (__builtin_expect(len > INT_MAX, 0)) return array_splice_large(js, arr, len, start_at, args, nargs);
  int start = (int)start_at;

  int deleteCount = nargs == 0 ? 0 : (int)len - start;
  if (nargs >= 2) {
    double requested;
    arg_err = array_integer_arg(js, args, nargs, 1, 0, &requested);
    if (is_err(arg_err)) return arg_err;
    if (requested < 0) requested = 0;
    if (requested < (double)deleteCount) deleteCount = (int)requested;
  }

  int insertCount = nargs > 2 ? nargs - 2 : 0;

  bool intrinsic = false;
  ant_value_t removed = array_species_create(js, arr, (ant_offset_t)deleteCount, &intrinsic);
  if (is_err(removed)) return removed;

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr) && !js_is_arguments(arr)) {
    ant_offset_t d_len = dense_iterable_length(js, arr);
    const uint16_t mutable_mask =
      ANT_OBJECT_FLAG_EXTENSIBLE | ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_EXOTIC;
    if (d_len != len || ((dense_obj(doff)->flags.raw & mutable_mask) != ANT_OBJECT_FLAG_EXTENSIBLE && !Ant_Array_TryUnshare(js, dense_obj(doff)))) goto splice_slow;
    bool skipped = false;
    for (int i = 0; i < deleteCount; i++) {
      if (!arr_has(js, arr, (ant_offset_t)(start + i))) { skipped = true; continue; }
      ant_value_t elem = arr_get(js, arr, (ant_offset_t)(start + i));
      ant_value_t set = array_result_set(js, removed, intrinsic, (ant_offset_t)i, elem);
      if (is_err(set)) return set;
    }
    if (intrinsic && !skipped) array_mark_packed(removed);
    if (!intrinsic) {
      ant_value_t set_len = array_set_result_length(js, removed, (ant_offset_t)deleteCount, false);
      if (is_err(set_len)) return set_len;
    }

    int shift = insertCount - deleteCount;
    ant_offset_t new_len = (ant_offset_t)((int)d_len + shift);

    if (shift != 0) {
      if (new_len > dense_capacity(doff)) {
        doff = dense_grow(js, arr, new_len);
        if (doff == 0) return js_mkerr(js, "oom");
      }
      ant_offset_t move_start = (ant_offset_t)(start + deleteCount);
      ant_offset_t move_dest = (ant_offset_t)(start + insertCount);
      ant_offset_t move_count = d_len - move_start;
      ant_value_t *d = dense_data(doff);
      if (!d) return js_mkerr(js, "oom");
      if (move_count > 0) {
        memmove(&d[move_dest], &d[move_start], sizeof(ant_value_t) * (size_t)move_count);
        gc_elements_moved(dense_obj(doff));
      }
    }

    for (int i = 0; i < insertCount; i++)
      dense_set_owned(js, doff, (ant_offset_t)(start + i), args[2 + i]);

    if (shift < 0) {
      for (ant_offset_t i = new_len; i < d_len; i++)
        dense_set_owned(js, doff, i, T_EMPTY);
    }

    array_len_set(js, arr, new_len);
    return removed;
  }

  splice_slow:
  return array_splice_generic(
    js, arr, len, (ant_offset_t)start, (ant_offset_t)deleteCount,
    insertCount > 0 ? &args[2] : NULL, (ant_offset_t)insertCount, removed, intrinsic
  );
}

static ant_value_t builtin_array_copyWithin(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "copyWithin");
  if (is_err(arr)) return arr;

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);

  ant_offset_t target_at = 0, start_at = 0, end_at = len;
  ant_value_t arg_err = array_relative_arg(js, args, nargs, 0, len, 0, &target_at);
  if (is_err(arg_err)) return arg_err;
  arg_err = array_relative_arg(js, args, nargs, 1, len, 0, &start_at);
  if (is_err(arg_err)) return arg_err;
  arg_err = array_relative_arg(js, args, nargs, 2, len, len, &end_at);
  if (is_err(arg_err)) return arg_err;
  
  if (end_at <= start_at || target_at >= len) return arr;
  ant_offset_t span = end_at - start_at < len - target_at ? end_at - start_at : len - target_at;
  
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "0");
  ant_object_t *frozen = vtype(arr) == kTypeArray ? (ant_object_t *)vptr(arr)
    : is_proxy(arr) ? NULL : js_obj_ptr(js_as_obj(arr));
  if (frozen && frozen->flags.frozen && !Ant_Array_TryUnshare(js, frozen))
    return Ant_Error_CreateReadOnlyProperty(js, arr, Ant_Value_FromNumber((double)target_at));

  ant_offset_t doff = get_dense_buf(arr);
  ant_object_t *dense = doff ? frozen : NULL;
  if (dense && array_dense_covers(dense, len) && (!dense->flags.may_have_holes || array_elements_proto_plain(js, dense))) {
    uint32_t target = (uint32_t)target_at, start = (uint32_t)start_at, count = (uint32_t)span;
    bool holes = false;
    if (start < target) {
      for (uint32_t i = count; i-- > 0;) {
        ant_value_t v = dense_get(doff, (ant_offset_t)(start + i));
        holes |= is_empty_slot(v);
        dense_set_owned(js, doff, (ant_offset_t)(target + i), v);
      }
    } else {
      for (uint32_t i = 0; i < count; i++) {
        ant_value_t v = dense_get(doff, (ant_offset_t)(start + i));
        holes |= is_empty_slot(v);
        dense_set_owned(js, doff, (ant_offset_t)(target + i), v);
      }
    }
    if (holes) array_mark_may_have_holes(arr);
    return arr;
  }

  ant_value_t moved = array_move_range_throw(
    js, arr, start_at, target_at, span, start_at < target_at && target_at < start_at + span
  );
  return is_err(moved) ? moved : arr;
}

static ant_value_t builtin_array_toSorted(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "toSorted");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  ant_value_t result = array_copy_spliced(js, arr, len, len, 0, NULL, 0);
  if (is_err(result)) return result;
  
  ant_value_t sorted = sv_call_native_this(js, builtin_array_sort, result, args, nargs);
  if (is_err(sorted)) return sorted;
  return mkval(kTypeArray, vdata(result));
}

static ant_value_t builtin_array_toReversed(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "toReversed");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  if (len > UINT32_MAX) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  ant_value_t result = array_copy_spliced(js, arr, len, len, 0, NULL, 0);
  if (is_err(result)) return result;
  
  ant_value_t reversed = sv_call_native_this(js, builtin_array_reverse, result, NULL, 0);
  if (is_err(reversed)) return reversed;
  return mkval(kTypeArray, vdata(result));
}

static ant_value_t builtin_array_toSpliced(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "toSpliced");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  double dlen = (double)len;
  
  double start_d = 0.0;
  if (nargs >= 1) {
    ant_value_t status = string_to_integer_or_infinity(js, args[0], &start_d);
    if (is_err(status)) return status;
  }
  
  ant_offset_t start = start_d < 0
    ? (start_d + dlen <= 0 ? 0 : (ant_offset_t)(start_d + dlen))
    : (start_d >= dlen ? len : (ant_offset_t)start_d);
  
  ant_offset_t skip = nargs == 0 ? 0 : len - start;
  if (nargs >= 2) {
    double skip_d = 0.0;
    ant_value_t status = string_to_integer_or_infinity(js, args[1], &skip_d);
    if (is_err(status)) return status;
    skip = skip_d <= 0 ? 0 : skip_d >= (double)(len - start) ? len - start : (ant_offset_t)skip_d;
  }
  
  ant_offset_t insert = nargs > 2 ? (ant_offset_t)(nargs - 2) : 0;
  ant_value_t result = array_copy_spliced(js, arr, len, start, skip, insert ? args + 2 : NULL, insert);
  if (is_err(result)) return result;
  
  return mkval(kTypeArray, vdata(result));
}

static ant_value_t builtin_array_with(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "with");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  double relative;
  ant_value_t arg_err = array_integer_arg(js, args, nargs, 0, 0, &relative);
  if (is_err(arg_err)) return arg_err;
  if (relative < 0) relative += (double)len;
  if (relative < 0 || relative >= (double)len) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid index");
  ant_offset_t idx = (ant_offset_t)relative;
  ant_value_t value = nargs > 1 ? args[1] : Ant_Value_Undefined;
  
  ant_value_t result = array_copy_spliced(js, arr, len, idx, 1, &value, 1);
  if (is_err(result)) return result;
  
  return mkval(kTypeArray, vdata(result));
}

static ant_value_t builtin_array_keys(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "keys");
  return is_err(arr) ? arr : make_array_iterator(js, arr, ARR_ITER_KEYS);
}

static ant_value_t builtin_array_values(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "values");
  return is_err(arr) ? arr : make_array_iterator(js, arr, ARR_ITER_VALUES);
}

static ant_value_t builtin_array_entries(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "entries");
  return is_err(arr) ? arr : make_array_iterator(js, arr, ARR_ITER_ENTRIES);
}

static ant_value_t builtin_array_toString(ant_params_t) {
  ant_value_t join_result;
  
  if (js_try_invoke(js, call_this, "join", NULL, 0, &join_result)) {
    if (is_err(join_result)) return join_result;
    return join_result;
  }
  
  return builtin_object_toString(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
}

static bool array_locale_uses_default_string_methods(ant_t *js) {
  ant_value_t string_proto = js->sym.string_proto;
  ant_value_t object_proto = js->sym.object_proto;
  
  ant_object_t *string_ptr = js_obj_ptr(js_as_obj(string_proto));
  ant_object_t *object_ptr = js_obj_ptr(js_as_obj(object_proto));

  if (!string_ptr || !object_ptr || string_ptr->flags.is_exotic || object_ptr->flags.is_exotic) return false;
  if (!same_object_identity(ant_object_proto(string_ptr), object_proto)) return false;
  if (lkp(js, string_proto, "toLocaleString", 14).obj) return false;

  ant_prop_loc_t locale_off = lkp(js, object_proto, "toLocaleString", 14);
  ant_prop_loc_t tostring_off = lkp(js, string_proto, "toString", 8);
  if (!locale_off.obj || !tostring_off.obj) return false;

  const ant_shape_prop_t *locale_meta = prop_shape_meta(locale_off);
  const ant_shape_prop_t *tostring_meta = prop_shape_meta(tostring_off);
  
  if (
    (locale_meta && (locale_meta->has_getter || locale_meta->has_setter)) ||
    (tostring_meta && (tostring_meta->has_getter || tostring_meta->has_setter))
  ) return false;

  ant_value_t locale_method = js_prop_load(locale_off);
  ant_value_t tostring_method = js_prop_load(tostring_off);
  
  return vtype(locale_method) == kTypeBuiltin &&
    js_cfunc_same_entrypoint(locale_method, builtin_object_toLocaleString) &&
    vtype(tostring_method) == kTypeBuiltin &&
    js_cfunc_same_entrypoint(tostring_method, builtin_string_toString);
}

static inline uint8_t merge_ascii_state(uint8_t left, uint8_t right) {
  if (left == STR_ASCII_NO || right == STR_ASCII_NO) return STR_ASCII_NO;
  if (left == STR_ASCII_YES && right == STR_ASCII_YES) return STR_ASCII_YES;
  return STR_ASCII_UNKNOWN;
}

static bool array_try_builtin_string_locale(
  ant_t *js, ant_value_t arr, 
  ant_offset_t len, ant_value_t *out
) {
  ant_object_t *array = array_obj_ptr(arr);
  
  if (
    !array || !array->flags.fast_array || !array->u.array.data ||
    (ant_offset_t)array->u.array.len != len
  ) return false;

  size_t total = len > 1 ? (size_t)len - 1 : 0;
  uint8_t ascii_state = STR_ASCII_YES;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t elem = array->u.array.data[i];
    if (
      vtype(elem) != kTypeString || str_is_heap_rope(elem) ||
      str_is_heap_builder(elem)
    ) return false;

    size_t elem_len = (size_t)vstrlen(js, elem);
    if (elem_len > SIZE_MAX - total) {
      *out = js_mkerr(js, "oom");
      return true;
    }
    total += elem_len;

    ant_flat_string_t *elem_flat = ant_str_flat_ptr(elem);
    uint8_t elem_ascii = elem_flat
      ? str_flat_ascii_state(elem_flat)
      : STR_ASCII_UNKNOWN;
    ascii_state = merge_ascii_state(ascii_state, elem_ascii);
  }

  if (!array_locale_uses_default_string_methods(js)) return false;
  ant_value_t result = js_mkstr(js, NULL, total);
  
  *out = result;
  if (is_err(result)) return true;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  
  ant_flat_string_t *flat = ant_str_flat_ptr(result);
  array = array_obj_ptr(arr);
  size_t pos = 0;
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (i > 0) flat->bytes[pos++] = ',';
    ant_offset_t elem_len = 0;
    ant_offset_t elem_off = vstr(js, array->u.array.data[i], &elem_len);
    memcpy(
      flat->bytes + pos,
      (const char *)(uintptr_t)elem_off,
      (size_t)elem_len
    );
    pos += (size_t)elem_len;
  }
  
  flat->bytes[pos] = '\0';
  str_flat_finish(flat, ascii_state);
  GC_ROOT_RESTORE(js, root_mark);
  
  return true;
}

static ant_value_t array_to_locale_string_body(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, call_this, "toLocaleString");
  if (is_err(arr)) return arr;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, arr);
  
  bool input_is_array = real_array_obj_ptr(arr) != NULL;
  ant_offset_t len = input_is_array ? get_array_length(js, arr) : 0;
  
  if (!input_is_array) {
    ant_value_t len_result = proxy_aware_length(js, arr, &len);
    if (is_err(len_result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return len_result;
    }
  }
  if (len == 0) {
    ant_value_t empty = js_mkstr(js, "", 0);
    GC_ROOT_RESTORE(js, root_mark);
    return empty;
  }
  if (__builtin_expect(len > (ant_offset_t)ANT_STRING_MAX_LENGTH, 0)) {
    GC_ROOT_RESTORE(js, root_mark);
    return array_join_too_long(js, len);
  }
  
  ant_value_t string_result;
  if (
    input_is_array &&
    array_try_builtin_string_locale(js, arr, len, &string_result)
  ) {
    GC_ROOT_RESTORE(js, root_mark);
    return string_result;
  }

  char static_buf[1024];
  string_builder_t sb;
  string_builder_init(&sb, static_buf, sizeof(static_buf));

  ant_object_t *numeric_fast_arr = input_is_array ? array_obj_ptr(arr) : NULL;
  bool all_fast_numbers = numeric_fast_arr && numeric_fast_arr->flags.fast_array &&
    !numeric_fast_arr->flags.may_have_holes && numeric_fast_arr->u.array.data &&
    (ant_offset_t)numeric_fast_arr->u.array.len == len;
  
  for (ant_offset_t i = 0; all_fast_numbers && i < len; i++)
    all_fast_numbers = vtype(numeric_fast_arr->u.array.data[i]) == kTypeNumber;

  ant_prop_loc_t number_locale_off = all_fast_numbers
    ? lkp(js, js->sym.number_proto, "toLocaleString", 14)
    : ANT_PROP_LOC_NONE;
  
  const ant_shape_prop_t *number_locale_meta = prop_shape_meta(number_locale_off);
  ant_value_t number_locale_method = number_locale_off.obj
    ? js_prop_load(number_locale_off)
    : Ant_Value_Undefined;
  
  all_fast_numbers = all_fast_numbers && number_locale_off.obj &&
    (!number_locale_meta || !number_locale_meta->has_getter) &&
    vtype(number_locale_method) == kTypeBuiltin &&
    js_cfunc_same_entrypoint(number_locale_method, builtin_number_toLocaleString);

  if (all_fast_numbers) {
    for (ant_offset_t i = 0; i < len; i++) {
      if (i > 0) {
        if (sb.size < sb.capacity) sb.buffer[sb.size++] = ',';
        else if (!string_builder_append(&sb, ",", 1)) {
          string_builder_dispose(&sb);
          ant_value_t err = js_mkerr(js, "oom");
          GC_ROOT_RESTORE(js, root_mark);
          return err;
        }
      }
      char localized_buf[128];
      bool write_direct = sb.capacity - sb.size >= sizeof(localized_buf);
      char *localized_out = write_direct ? sb.buffer + sb.size : localized_buf;
      size_t localized_capacity = write_direct
        ? sb.capacity - sb.size
        : sizeof(localized_buf);
      size_t localized_len = format_number_to_locale_string(
        numeric_fast_arr->u.array.data[i], localized_out, localized_capacity
      );
      if (localized_len == 0) {
        string_builder_dispose(&sb);
        ant_value_t err = js_mkerr(js, "number formatting failed");
        GC_ROOT_RESTORE(js, root_mark);
        return err;
      }
      if (write_direct) sb.size += localized_len;
      else if (!string_builder_append(&sb, localized_buf, localized_len)) {
        string_builder_dispose(&sb);
        ant_value_t err = js_mkerr(js, "oom");
        GC_ROOT_RESTORE(js, root_mark);
        return err;
      }
    }
    ant_value_t ret = string_builder_finalize(js, &sb);
    GC_ROOT_RESTORE(js, root_mark);
    return ret;
  }
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (i > 0 && !string_builder_append(&sb, ",", 1)) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }

    GC_ROOT_SAVE(elem_mark, js);
    ant_value_t elem = input_is_array ? arr_get(js, arr, i) : array_method_get_index(js, arr, i);
    GC_ROOT_PIN(js, elem);
    if (is_err(elem)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return elem;
    }
    if (vtype(elem) == kTypeNull || vtype(elem) == kTypeUndefined) {
      GC_ROOT_RESTORE(js, elem_mark);
      continue;
    }

    ant_value_t method = js_getprop_fallback(js, elem, "toLocaleString");
    GC_ROOT_PIN(js, method);
    if (is_err(method)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return method;
    }

    uint8_t method_type = vtype(method);
    if (!is_callable(method)) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr_typed(
        js, JS_ERR_TYPE, 
        "toLocaleString is not a function"
      );
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }

    if (vtype(elem) == kTypeNumber && method_type == kTypeBuiltin &&
        js_cfunc_same_entrypoint(method, builtin_number_toLocaleString)) {
      char localized_buf[128];
      size_t localized_len = format_number_to_locale_string(
        elem, localized_buf, 
        sizeof(localized_buf)
      );
      if (localized_len == 0 ||
          !string_builder_append(&sb, localized_buf, localized_len)) {
        string_builder_dispose(&sb);
        ant_value_t err = js_mkerr(js, "oom");
        GC_ROOT_RESTORE(js, elem_mark);
        GC_ROOT_RESTORE(js, root_mark);
        return err;
      }
      GC_ROOT_RESTORE(js, elem_mark);
      continue;
    }

    ant_value_t localized = sv_call_value(js, method, elem, args, nargs);

    GC_ROOT_PIN(js, localized);
    if (is_err(localized)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return localized;
    }

    ant_value_t localized_str = coerce_to_str_hint(js, localized, 1);
    GC_ROOT_PIN(js, localized_str);
    if (is_err(localized_str)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return localized_str;
    }

    ant_offset_t elem_len = 0;
    ant_offset_t elem_off = vstr(js, localized_str, &elem_len);
    if (!string_builder_append(
      &sb, (const char *)(uintptr_t)elem_off, (size_t)elem_len
    )) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
    GC_ROOT_RESTORE(js, elem_mark);
  }
  
  ant_value_t ret = string_builder_finalize(js, &sb);
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

static ant_value_t array_join_guarded(ant_native_params_t, ant_value_t call_this, ant_value_t call_func, ant_value_t new_target, ant_cfunc_t body) {
  for (ant_array_join_frame_t *frame = js->array_join_stack; frame; frame = frame->prev)
    if (frame->receiver == call_this && is_object_type(call_this)) return js_mkstr(js, "", 0);

  ant_array_join_frame_t frame = { js->array_join_stack, call_this };
  js->array_join_stack = &frame;
  
  ant_value_t result = body(js, args, nargs, new_target, call_this, call_func);
  js->array_join_stack = frame.prev;
  
  return result;
}

static ant_value_t builtin_array_join(ant_params_t) {
  return array_join_guarded(js, args, nargs, call_this, call_func, call_new_target, array_join_body);
}

static ant_value_t builtin_array_toLocaleString(ant_params_t) {
  return array_join_guarded(js, args, nargs, call_this, call_func, call_new_target, array_to_locale_string_body);
}

static ant_value_t builtin_Array_isArray(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  bool result = false;
  ant_value_t is_array_res = js_is_array_value_checked(js, args[0], &result);
  if (is_err(is_array_res)) return is_array_res;
  return mkval(kTypeBool, result ? 1 : 0);
}

static ant_value_t builtin_Array_isTemplateObject(ant_params_t) {
  if (nargs == 0) return Ant_Value_False;
  return Ant_Value_FromBool(vtype(args[0]) == kTypeArray && js_check_brand(args[0], BRAND_TEMPLATE_OBJECT));
}

typedef struct {
  ant_value_t result;
  ant_value_t mapFn;
  ant_value_t mapThis;
  ant_offset_t index;
  bool fresh;
} array_from_iter_ctx_t;

static iter_action_t array_from_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  array_from_iter_ctx_t *fctx = (array_from_iter_ctx_t *)ctx;
  ant_value_t elem = value;

  if (is_callable(fctx->mapFn)) {
    ant_value_t call_args[2] = { elem, Ant_Value_FromNumber((double)fctx->index) };
    elem = sv_vm_call(js->vm, js, fctx->mapFn, fctx->mapThis, call_args, 2, NULL, Ant_Value_Undefined);
    if (is_err(elem)) { *out = elem; return ITER_ERROR; }
  }

  ant_value_t set = array_result_set(js, fctx->result, fctx->fresh, fctx->index, elem);
  if (is_err(set)) { *out = set; return ITER_ERROR; }

  fctx->index++;
  return ITER_CONTINUE;
}

static ant_value_t array_from_finish(ant_t *js, array_from_iter_ctx_t *ctx, bool use_ctor) {
  ant_value_t set_len = array_set_result_length(js, ctx->result, ctx->index, ctx->fresh);
  if (is_err(set_len)) return set_len;
  return use_ctor ? ctx->result : mkval(kTypeArray, vdata(ctx->result));
}

static ant_value_t array_from_indexed(ant_t *js, ant_value_t src, ant_offset_t len, array_from_iter_ctx_t *ctx) {
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t unused;
    ant_value_t item = array_method_get_index(js, src, i);
    if (is_err(item)) return item;
    if (array_from_iter_cb(js, item, ctx, &unused) == ITER_ERROR) return unused;
  }
  return Ant_Value_Undefined;
}


static ant_value_t array_from_string(ant_t *js, ant_value_t src, array_from_iter_ctx_t *ctx) {
  if (str_is_heap_rope(src) || str_is_heap_builder(src)) {
    src = str_materialize(js, src);
    if (is_err(src)) return src;
  }
  
  ant_offset_t slen = str_len_fast(js, src);
  for (ant_offset_t i = 0; i < slen; ) {
    ant_offset_t off = vstr(js, src, NULL);
    ant_offset_t cb_len = (ant_offset_t)utf8_char_len_at((const char *)(uintptr_t)off, (size_t)slen, (size_t)i);
    ant_value_t ch = js_mkstr(js, (const void *)(uintptr_t)(off + i), cb_len);
    
    ant_value_t out;
    if (array_from_iter_cb(js, ch, ctx, &out) == ITER_ERROR) return out;
    i += cb_len;
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t array_from_array(ant_t *js, ant_value_t src, array_from_iter_ctx_t *ctx) {
  bool bulk = ctx->fresh ? real_array_obj_ptr(ctx->result) != NULL : array_result_plain(ctx->result);
  if (!is_callable(ctx->mapFn) && bulk && get_array_length(js, ctx->result) == 0) {
    ant_value_t copied = js_arr_spread_into_owned(js, ctx->result, src);
    if (is_err(copied)) return copied;
    ctx->index = get_array_length(js, ctx->result);
    return Ant_Value_Undefined;
  }
  
  ant_offset_t len = 0;
  ant_value_t len_result = array_like_length_checked(js, src, &len);
  if (is_err(len_result)) return len_result;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t unused;
    if (array_from_iter_cb(js, arr_get(js, src, i), ctx, &unused) == ITER_ERROR) return unused;
    len_result = array_like_length_checked(js, src, &len);
    if (is_err(len_result)) return len_result;
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t builtin_Array_from(ant_params_t) {
  ant_value_t src = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t mapFn = nargs >= 2 ? args[1] : Ant_Value_Undefined;
  ant_value_t mapThis = nargs >= 3 ? args[2] : Ant_Value_Undefined;
  if (vtype(mapFn) != kTypeUndefined && !is_callable(mapFn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Array.from: the mapping function must be callable");

  bool use_ctor = call_this != js->sym.array_ctor && js_is_constructor(call_this);
  array_from_iter_ctx_t ctx = { Ant_Value_Undefined, mapFn, mapThis, 0, !use_ctor };
  bool unclosable = ant_protector_intact(js, ANT_PROTECTOR_ITERATOR_RETURN);
  
  bool string_fast = vtype(src) == kTypeString && unclosable && ant_protector_intact(js, ANT_PROTECTOR_STRING_ITERATION);
  bool array_fast = vtype(src) == kTypeArray && unclosable && js_array_iteration_default(js, src);
  
  if (string_fast || array_fast) {
    ctx.result = use_ctor ? array_alloc_from_ctor(js, call_this) : Ant_Array_Create(js);
    if (is_err(ctx.result)) return ctx.result;
    ant_value_t filled = string_fast ? array_from_string(js, src, &ctx) : array_from_array(js, src, &ctx);
    return is_err(filled) ? filled : array_from_finish(js, &ctx, use_ctor);
  }
  
  uint8_t st = vtype(src);
  if (st == kTypeUndefined || st == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not iterable", st == kTypeNull ? "null" : "undefined");
  
  ant_value_t iter_method = js_get_sym(js, src, js->sym.iterator_sym);
  if (is_err(iter_method)) return iter_method;
  
  if (vtype(iter_method) != kTypeUndefined && vtype(iter_method) != kTypeNull) {
    ctx.result = use_ctor ? array_alloc_from_ctor(js, call_this) : Ant_Array_Create(js);
    if (is_err(ctx.result)) return ctx.result;
    
    ant_value_t filled;
    if (vtype(iter_method) == kTypeBuiltin && is_proxy(src) && vtype(proxy_read_target(js, src)) == kTypeArray) {
      PROXY_AWARE_LENGTH_OR_RETURN(src, len);
      filled = array_from_indexed(js, src, len, &ctx);
    } else filled = iter_foreach_method(js, src, iter_method, array_from_iter_cb, &ctx);
    
    return is_err(filled) ? filled : array_from_finish(js, &ctx, use_ctor);
  }
  
  ant_offset_t len = 0;
  ant_value_t len_result = is_object_type(src) ? proxy_aware_length(js, src, &len) : array_like_length_checked(js, src, &len);
  if (is_err(len_result)) return len_result;
  if (!use_ctor && len > UINT32_MAX) return array_length_range_error(js);
  
  ctx.result = use_ctor ? array_alloc_from_ctor_with_length(js, call_this, len) : Ant_Array_Create(js);
  if (is_err(ctx.result)) return ctx.result;
  
  ant_value_t filled = array_from_indexed(js, src, len, &ctx);
  if (is_err(filled)) return filled;
  ctx.index = len;
  
  return array_from_finish(js, &ctx, use_ctor);
}

static ant_value_t builtin_Array_of(ant_params_t) {
  bool use_ctor = call_this != js->sym.array_ctor && js_is_constructor(call_this);
  bool fresh = !use_ctor;
  ant_value_t arr = array_alloc_from_ctor_with_length(js, use_ctor ? call_this : js->sym.array_ctor, (ant_offset_t)nargs);
  if (is_err(arr)) return arr;

  bool plain = fresh || array_result_plain(arr);
  for (int i = 0; i < nargs; i++) {
    ant_value_t set = array_result_set(js, arr, plain, (ant_offset_t)i, args[i]);
    if (is_err(set)) return set;
  }

  ant_value_t set_len = array_set_result_length(js, arr, nargs, fresh);
  if (is_err(set_len)) return set_len;
  if (!use_ctor) return mkval(kTypeArray, vdata(arr));

  return arr;
}

typedef enum {
  FROM_ASYNC_PROMISE,
  FROM_ASYNC_RESULT,
  FROM_ASYNC_SOURCE,
  FROM_ASYNC_NEXT,
  FROM_ASYNC_MAPFN,
  FROM_ASYNC_THIS,
  FROM_ASYNC_INDEX,
  FROM_ASYNC_LENGTH,
  FROM_ASYNC_KIND,
  FROM_ASYNC_REASON,
  FROM_ASYNC_FIELD_COUNT,
} from_async_field_t;

typedef enum {
  FROM_ASYNC_ASYNC_ITERATOR,
  FROM_ASYNC_SYNC_ITERATOR,
  FROM_ASYNC_ARRAY_LIKE,
} from_async_kind_t;

static inline ant_value_t from_async_field(ant_t *js, ant_value_t state, from_async_field_t field) {
  return arr_get(js, state, field);
}

static inline from_async_kind_t from_async_kind(ant_t *js, ant_value_t state) {
  return (from_async_kind_t)Ant_Value_AsNumberUnchecked(from_async_field(js, state, FROM_ASYNC_KIND));
}

static inline double from_async_index(ant_t *js, ant_value_t state) {
  return Ant_Value_AsNumberUnchecked(from_async_field(js, state, FROM_ASYNC_INDEX));
}

static ant_value_t from_async_reject(ant_t *js, ant_value_t state, ant_value_t reason) {
  js_reject_promise(js, from_async_field(js, state, FROM_ASYNC_PROMISE), reason);
  return Ant_Value_Undefined;
}

static ant_value_t from_async_fail(ant_t *js, ant_value_t state, ant_value_t err) {
  return from_async_reject(js, state, Ant_Error_ConsumeMarker(js, err));
}

static ant_value_t from_async_on_rejected(ant_params_t) {
  return from_async_reject(js, js_get_slot(call_func, SLOT_DATA), nargs > 0 ? args[0] : Ant_Value_Undefined);
}

static ant_value_t from_async_on_closed(ant_params_t) {
  ant_value_t state = js_get_slot(call_func, SLOT_DATA);
  return from_async_reject(js, state, from_async_field(js, state, FROM_ASYNC_REASON));
}

static ant_value_t from_async_await(
  ant_t *js, ant_value_t state, ant_value_t value,
  ant_value_t (*on_fulfilled)(ant_params_t), ant_value_t (*on_rejected)(ant_params_t)
) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, state);
  
  ant_value_t promise = js_promise_assimilate_awaitable(js, value);
  if (is_err(promise)) {
    GC_ROOT_RESTORE(js, mark);
    return from_async_fail(js, state, promise);
  }
  
  GC_ROOT_PIN(js, promise);
  ant_value_t fulfill = js_heavy_mkfun(js, on_fulfilled, state);
  GC_ROOT_PIN(js, fulfill);
  ant_value_t reject = js_heavy_mkfun(js, on_rejected, state);
  Ant_Promise_Observe(js, promise, fulfill, reject);
  
  GC_ROOT_RESTORE(js, mark);
  return Ant_Value_Undefined;
}

static ant_value_t from_async_close_and_reject(ant_t *js, ant_value_t state, ant_value_t reason) {
  if (from_async_kind(js, state) == FROM_ASYNC_ARRAY_LIKE) return from_async_reject(js, state, reason);

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, state);
  GC_ROOT_PIN(js, reason);
  arr_set(js, state, FROM_ASYNC_REASON, reason);

  ant_value_t iterator = from_async_field(js, state, FROM_ASYNC_SOURCE);
  ant_value_t ret = js_getprop_fallback(js, iterator, "return");
  bool has_return = !is_err(ret) && is_callable(ret);
  
  ant_value_t closed = has_return ? sv_vm_call(js->vm, js, ret, iterator, NULL, 0, NULL, Ant_Value_Undefined) : ret;
  if (is_err(closed)) Ant_Error_ConsumeMarker(js, closed);

  bool await_close = has_return && !is_err(closed) && from_async_kind(js, state) == FROM_ASYNC_ASYNC_ITERATOR;
  ant_value_t result = await_close
    ? from_async_await(js, state, closed, from_async_on_closed, from_async_on_closed)
    : from_async_reject(js, state, reason);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t from_async_on_close_rejected(ant_params_t) {
  return from_async_close_and_reject(js, js_get_slot(call_func, SLOT_DATA), nargs > 0 ? args[0] : Ant_Value_Undefined);
}

static ant_value_t from_async_step(ant_t *js, ant_value_t state);

static ant_value_t from_async_mapped(ant_t *js, ant_value_t state, ant_value_t value) {
  ant_value_t target = from_async_field(js, state, FROM_ASYNC_RESULT);
  double index = from_async_index(js, state);

  define_desc_raw_t raw = {
    .has = { true, true, true, true, false, false },
    .val = { Ant_Value_True, Ant_Value_True, value, Ant_Value_True, Ant_Value_Undefined, Ant_Value_Undefined },
  };
  
  ant_value_t defined = object_define_property_reporting(js, target, Ant_Value_FromNumber(index), Ant_Value_Undefined, &raw, NULL);
  if (is_err(defined)) return from_async_close_and_reject(js, state, Ant_Error_ConsumeMarker(js, defined));

  arr_set(js, state, FROM_ASYNC_INDEX, Ant_Value_FromNumber(index + 1));
  return from_async_step(js, state);
}

static ant_value_t from_async_on_mapped(ant_params_t) {
  return from_async_mapped(js, js_get_slot(call_func, SLOT_DATA), nargs > 0 ? args[0] : Ant_Value_Undefined);
}

static ant_value_t from_async_value(ant_t *js, ant_value_t state, ant_value_t value) {
  ant_value_t mapfn = from_async_field(js, state, FROM_ASYNC_MAPFN);
  if (vtype(mapfn) == kTypeUndefined) return from_async_mapped(js, state, value);

  ant_value_t call_args[2] = { value, from_async_field(js, state, FROM_ASYNC_INDEX) };
  ant_value_t this_arg = from_async_field(js, state, FROM_ASYNC_THIS);
  ant_value_t mapped = sv_vm_call(js->vm, js, mapfn, this_arg, call_args, 2, NULL, Ant_Value_Undefined);
  
  if (is_err(mapped)) return from_async_close_and_reject(js, state, Ant_Error_ConsumeMarker(js, mapped));
  return from_async_await(js, state, mapped, from_async_on_mapped, from_async_on_close_rejected);
}

static ant_value_t from_async_on_value(ant_params_t) {
  return from_async_value(js, js_get_slot(call_func, SLOT_DATA), nargs > 0 ? args[0] : Ant_Value_Undefined);
}

static ant_value_t from_async_finish(ant_t *js, ant_value_t state) {
  ant_value_t target = from_async_field(js, state, FROM_ASYNC_RESULT);
  ant_value_t set = js_setprop(js, target, js->length_str, from_async_field(js, state, FROM_ASYNC_INDEX));
  
  if (is_err(set)) return from_async_fail(js, state, set);
  js_resolve_promise(js, from_async_field(js, state, FROM_ASYNC_PROMISE), target);
  
  return Ant_Value_Undefined;
}

static ant_value_t from_async_next_result(ant_t *js, ant_value_t state, ant_value_t result) {
  if (!is_object_type(result)) return from_async_fail(
    js, state, js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result %s is not an object", js_str(js, result))
  );

  ant_value_t done = js_getprop_fallback(js, result, "done");
  if (is_err(done)) return from_async_fail(js, state, done);
  if (js_truthy(js, done)) return from_async_finish(js, state);

  ant_value_t value = js_getprop_fallback(js, result, "value");
  if (is_err(value)) return from_async_fail(js, state, value);
  
  if (from_async_kind(js, state) == FROM_ASYNC_ASYNC_ITERATOR) return from_async_value(js, state, value);
  return from_async_await(js, state, value, from_async_on_value, from_async_on_close_rejected);
}

static ant_value_t from_async_on_next_result(ant_params_t) {
  return from_async_next_result(js, js_get_slot(call_func, SLOT_DATA), nargs > 0 ? args[0] : Ant_Value_Undefined);
}

static ant_value_t from_async_step(ant_t *js, ant_value_t state) {
  ant_value_t source = from_async_field(js, state, FROM_ASYNC_SOURCE);
  from_async_kind_t kind = from_async_kind(js, state);

  if (kind == FROM_ASYNC_ARRAY_LIKE) {
    double index = from_async_index(js, state);
    if (index >= Ant_Value_AsNumberUnchecked(from_async_field(js, state, FROM_ASYNC_LENGTH))) return from_async_finish(js, state);
    
    char key[32];
    size_t key_len = uint_to_str(key, sizeof(key), (unsigned)index);
    ant_value_t value = js_getprop_fallback_len(js, source, key, key_len);
    
    if (is_err(value)) return from_async_fail(js, state, value);
    return from_async_await(js, state, value, from_async_on_value, from_async_on_rejected);
  }

  ant_value_t next = from_async_field(js, state, FROM_ASYNC_NEXT);
  ant_value_t result = sv_vm_call(js->vm, js, next, source, NULL, 0, NULL, Ant_Value_Undefined);
  
  if (is_err(result)) return from_async_fail(js, state, result);
  if (kind == FROM_ASYNC_SYNC_ITERATOR) return from_async_next_result(js, state, result);
  
  return from_async_await(js, state, result, from_async_on_next_result, from_async_on_rejected);
}

static ant_value_t from_async_construct(ant_t *js, ant_value_t ctor, bool with_length, ant_offset_t length) {
  if (!js_is_constructor(ctor) || same_ctor_identity(js, ctor, js->sym.array_ctor))
    return array_alloc_intrinsic(js, Ant_Value_Undefined, with_length ? length : 0);

  ant_value_t seed = Ant_Object_CreateBare(js);
  if (is_err(seed)) return seed;
  
  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t ctor_args[1] = { Ant_Value_FromNumber((double)length) };
  ant_value_t constructed = sv_vm_call(js->vm, js, ctor, seed, ctor_args, with_length ? 1 : 0, NULL, ctor);
  
  if (is_err(constructed)) return constructed;
  return is_object_type(constructed) ? constructed : seed;
}

static ant_value_t from_async_open_iterator(ant_t *js, ant_value_t items, ant_value_t method, ant_value_t *next) {
  if (!is_callable(method)) return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", js_str(js, method));
  
  ant_value_t iterator = sv_vm_call(js->vm, js, method, items, NULL, 0, NULL, Ant_Value_Undefined);
  if (is_err(iterator)) return iterator;
  if (!is_object_type(iterator)) return js_mkerr_typed(js, JS_ERR_TYPE, "Result of the Symbol.iterator method is not an object");
  
  *next = js_getprop_fallback(js, iterator, "next");
  return is_err(*next) ? *next : iterator;
}

static ant_value_t from_async_start(
  ant_t *js, ant_value_t call_this, ant_value_t ctor, ant_value_t items,
  ant_value_t mapfn, ant_value_t this_arg, ant_value_t promise
) {
  if (vtype(mapfn) != kTypeUndefined && !is_callable(mapfn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", js_str(js, mapfn));
  if (is_nullish(items)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot read properties of %s (reading 'Symbol(Symbol.asyncIterator)')",
    vtype(items) == kTypeNull ? "null" : "undefined"
  );

  ant_value_t lookup = is_object_type(items) ? items : builtin_Object(js, &items, 1, Ant_Value_Undefined, call_this, Ant_Value_Undefined);
  if (is_err(lookup)) return lookup;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, lookup);

  from_async_kind_t kind = FROM_ASYNC_ASYNC_ITERATOR;
  ant_value_t method = js_get_sym(js, lookup, js->sym.asyncIterator_sym);
  if (!is_err(method) && is_nullish(method)) {
    kind = FROM_ASYNC_SYNC_ITERATOR;
    method = js_get_sym(js, lookup, js->sym.iterator_sym);
  }
  if (!is_err(method) && is_nullish(method)) kind = FROM_ASYNC_ARRAY_LIKE;

  ant_value_t source = lookup;
  ant_value_t next = Ant_Value_Undefined;
  ant_value_t target = method;
  ant_offset_t length = 0;
  
  GC_ROOT_PIN(js, source);
  GC_ROOT_PIN(js, next);
  GC_ROOT_PIN(js, target);

  if (!is_err(target) && kind == FROM_ASYNC_ARRAY_LIKE) {
    target = array_like_length_checked(js, lookup, &length);
    if (!is_err(target)) target = from_async_construct(js, ctor, true, length);
  } else if (!is_err(target)) {
    source = from_async_open_iterator(js, items, method, &next);
    target = is_err(source) ? source : from_async_construct(js, ctor, false, 0);
  }

  ant_value_t state = is_err(target) ? target : Ant_Array_Create(js);
  GC_ROOT_PIN(js, state);
  
  if (!is_err(state)) {
    ant_value_t fields[FROM_ASYNC_FIELD_COUNT] = {
      [FROM_ASYNC_PROMISE] = promise, [FROM_ASYNC_RESULT] = target,
      [FROM_ASYNC_SOURCE] = source, [FROM_ASYNC_NEXT] = next,
      [FROM_ASYNC_MAPFN] = mapfn, [FROM_ASYNC_THIS] = this_arg,
      [FROM_ASYNC_INDEX] = Ant_Value_FromNumber(0), [FROM_ASYNC_LENGTH] = Ant_Value_FromNumber((double)length),
      [FROM_ASYNC_KIND] = Ant_Value_FromNumber((double)kind), [FROM_ASYNC_REASON] = Ant_Value_Undefined,
    };
    
    for (int f = 0; f < FROM_ASYNC_FIELD_COUNT; f++) arr_set(js, state, f, fields[f]);
    state = from_async_step(js, state);
  }

  GC_ROOT_RESTORE(js, mark);
  return state;
}

static ant_value_t builtin_Array_fromAsync(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  if (is_err(promise)) return promise;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, promise);
  
  ant_value_t started = from_async_start(
    js, call_this, call_this, nargs > 0 ? args[0] : Ant_Value_Undefined,
    nargs > 1 ? args[1] : Ant_Value_Undefined, nargs > 2 ? args[2] : Ant_Value_Undefined, promise
  );
  
  if (is_err(started)) js_reject_promise(js, promise, Ant_Error_ConsumeMarker(js, started));
  GC_ROOT_RESTORE(js, mark);

  return promise;
}

void js_array_init_prototype(ant_t *js, ant_value_t array_proto) {
  js->sym.array_push_fn = defmethod(js, array_proto, "push", 4, Ant_Function_CreateWithArity(builtin_array_push, 1));
  defmethod(js, array_proto, "pop", 3, Ant_Function_Create(builtin_array_pop));
  defmethod(js, array_proto, "slice", 5, Ant_Function_CreateWithArity(builtin_array_slice, 2));
  defmethod(js, array_proto, "join", 4, Ant_Function_CreateWithArity(builtin_array_join, 1));
  defmethod(js, array_proto, "includes", 8, Ant_Function_CreateWithArity(builtin_array_includes, 1));
  defmethod(js, array_proto, "every", 5, Ant_Function_CreateWithArity(builtin_array_every, 1));
  defmethod(js, array_proto, "reverse", 7, Ant_Function_Create(builtin_array_reverse));
  defmethod(js, array_proto, "map", 3, Ant_Function_CreateWithArity(builtin_array_map, 1));
  defmethod(js, array_proto, "filter", 6, Ant_Function_CreateWithArity(builtin_array_filter, 1));
  defmethod(js, array_proto, "reduce", 6, Ant_Function_CreateWithArity(builtin_array_reduce, 1));
  defmethod(js, array_proto, "flat", 4, Ant_Function_Create(builtin_array_flat));
  defmethod(js, array_proto, "concat", 6, Ant_Function_CreateWithArity(builtin_array_concat, 1));
  defmethod(js, array_proto, "at", 2, Ant_Function_CreateWithArity(builtin_array_at, 1));
  defmethod(js, array_proto, "fill", 4, Ant_Function_CreateWithArity(builtin_array_fill, 1));
  defmethod(js, array_proto, "find", 4, Ant_Function_CreateWithArity(builtin_array_find, 1));
  defmethod(js, array_proto, "findIndex", 9, Ant_Function_CreateWithArity(builtin_array_findIndex, 1));
  defmethod(js, array_proto, "findLast", 8, Ant_Function_CreateWithArity(builtin_array_findLast, 1));
  defmethod(js, array_proto, "findLastIndex", 13, Ant_Function_CreateWithArity(builtin_array_findLastIndex, 1));
  defmethod(js, array_proto, "flatMap", 7, Ant_Function_CreateWithArity(builtin_array_flatMap, 1));
  defmethod(js, array_proto, "forEach", 7, Ant_Function_CreateWithArity(builtin_array_forEach, 1));
  defmethod(js, array_proto, "indexOf", 7, Ant_Function_CreateWithArity(builtin_array_indexOf, 1));
  defmethod(js, array_proto, "lastIndexOf", 11, Ant_Function_CreateWithArity(builtin_array_lastIndexOf, 1));
  defmethod(js, array_proto, "reduceRight", 11, Ant_Function_CreateWithArity(builtin_array_reduceRight, 1));
  defmethod(js, array_proto, "shift", 5, Ant_Function_Create(builtin_array_shift));
  defmethod(js, array_proto, "unshift", 7, Ant_Function_CreateWithArity(builtin_array_unshift, 1));
  defmethod(js, array_proto, "some", 4, Ant_Function_CreateWithArity(builtin_array_some, 1));
  defmethod(js, array_proto, "sort", 4, Ant_Function_CreateWithArity(builtin_array_sort, 1));
  defmethod(js, array_proto, "splice", 6, Ant_Function_CreateWithArity(builtin_array_splice, 2));
  defmethod(js, array_proto, "copyWithin", 10, Ant_Function_CreateWithArity(builtin_array_copyWithin, 2));
  defmethod(js, array_proto, "toReversed", 10, Ant_Function_Create(builtin_array_toReversed));
  defmethod(js, array_proto, "toSorted", 8, Ant_Function_CreateWithArity(builtin_array_toSorted, 1));
  defmethod(js, array_proto, "toSpliced", 9, Ant_Function_CreateWithArity(builtin_array_toSpliced, 2));
  defmethod(js, array_proto, "with", 4, Ant_Function_CreateWithArity(builtin_array_with, 2));
  defmethod(js, array_proto, "keys", 4, Ant_Function_Create(builtin_array_keys));
  js->sym.array_values_fn = defmethod(js, array_proto, "values", 6, Ant_Function_Create(builtin_array_values));
  defmethod(js, array_proto, "entries", 7, Ant_Function_Create(builtin_array_entries));
  defmethod(js, array_proto, "toString", 8, Ant_Function_Create(builtin_array_toString));
  defmethod(js, array_proto, "toLocaleString", 14, Ant_Function_Create(builtin_array_toLocaleString));
}

void js_array_init_constructor(ant_t *js, ant_value_t arr_ctor_obj, ant_value_t array_proto) {
  js_set_slot(arr_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Array, 1));
  js_setprop_readonly_nonconfigurable(js, arr_ctor_obj, "prototype", 9, array_proto);
  defmethod(js, arr_ctor_obj, "isArray", 7, Ant_Function_CreateWithArity(builtin_Array_isArray, 1));
  defmethod(js, arr_ctor_obj, "isTemplateObject", 16, Ant_Function_Create(builtin_Array_isTemplateObject));
  defmethod(js, arr_ctor_obj, "from", 4, Ant_Function_CreateWithArity(builtin_Array_from, 1));
  defmethod(js, arr_ctor_obj, "fromAsync", 9, Ant_Function_CreateWithArity(builtin_Array_fromAsync, 1));
  defmethod(js, arr_ctor_obj, "of", 2, Ant_Function_Create(builtin_Array_of));
  js_set_function_name(js, arr_ctor_obj, "Array", sizeof("Array") - 1);
}
