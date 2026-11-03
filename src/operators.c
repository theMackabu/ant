#include <compat.h> // IWYU pragma: keep
#include <string.h>

#include "tokens.h"
#include "arrays.h"
#include "keys.h"
#include "props.h"
#include "proto.h"
#include "stringify.h"
#include "numbers.h"
#include "operators.h"

#include "silver/call.h"
#include "modules/array.h"
#include "modules/function.h"
#include "modules/proxy.h"

ant_value_t do_string_num_concat(ant_t *js, ant_value_t str, ant_value_t num, bool num_first) {
  bool flat_str = !str_is_heap_rope(str) && !str_is_heap_builder(str);
  size_t str_len = flat_str ? (size_t)ant_str_flat_ptr(str)->len : 0;

  int32_t integer;
  bool is_integer = ant_number_as_int32(Ant_Value_AsNumberUnchecked(num), &integer);
  if (flat_str && str_len == 0 && is_integer) return js_mkstr_int32(js, integer);

  char digits[32];
  size_t num_len;
  if (is_integer) {
    num_len = ant_int32_chars_len(integer);
    ant_int32_write(digits, num_len, integer);
  } else num_len = js_number_to_chars(num, digits, sizeof(digits));

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  ant_value_t result;

  if (flat_str && str_len == 0) {
    GC_ROOT_RESTORE(js, root_mark);
    return mkstr_with_ascii(js, digits, num_len, true);
  }

  if (flat_str && num_len > 0 && str_concat_stays_flat(str_len + num_len, ant_str_flat_ptr(str)->bytes, NULL)) {
    size_t total_len = str_len + num_len;
    result = js_mkstr(js, NULL, total_len);
    
    if (!is_err(result)) {
      ant_flat_string_t *in = ant_str_flat_ptr(str);
      uint8_t in_ascii = str_flat_ascii_state(in);
      if (num_first) str_flat_fill_concat(
        ant_str_flat_ptr(result), digits, num_len, STR_ASCII_YES, in->bytes, str_len, in_ascii);
      else str_flat_fill_concat(
        ant_str_flat_ptr(result), in->bytes, str_len, in_ascii, digits, num_len, STR_ASCII_YES);
    }
  } else {
    ant_value_t num_str = mkstr_with_ascii(js, digits, num_len, true);
    result = is_err(num_str) ? num_str : (num_first
      ? do_string_op(js, TOK_PLUS, num_str, str)
      : do_string_op(js, TOK_PLUS, str, num_str));
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

ant_value_t do_string_op(ant_t *js, uint8_t op, ant_value_t l, ant_value_t r) {
  if (op == TOK_PLUS) {
    if (str_is_heap_builder(l)) {
      l = builder_flatten(js, l);
      if (is_err(l)) return l;
    }
    
    if (str_is_heap_builder(r)) {
      r = builder_flatten(js, r);
      if (is_err(r)) return r;
    }
    
    ant_offset_t n1 = str_len_fast(js, l);
    ant_offset_t n2 = str_len_fast(js, r);
    ant_offset_t total_len = n1 + n2;
    
    if (n2 == 0) return l;
    if (n1 == 0) return r;

    if (
      !str_is_heap_rope(l) && !str_is_heap_rope(r) &&
      str_concat_stays_flat((size_t)total_len, ant_str_flat_ptr(l)->bytes, ant_str_flat_ptr(r)->bytes)
    ) {
      GC_ROOT_SAVE(root_mark, js);
      GC_ROOT_PIN(js, l);
      GC_ROOT_PIN(js, r);
      ant_value_t flat = js_mkstr(js, NULL, (size_t)total_len);
      if (!is_err(flat)) {
        ant_flat_string_t *left = ant_str_flat_ptr(l);
        ant_flat_string_t *right = ant_str_flat_ptr(r);
        str_flat_fill_concat(
          ant_str_flat_ptr(flat),
          left->bytes, (size_t)n1, str_flat_ascii_state(left),
          right->bytes, (size_t)n2, str_flat_ascii_state(right)
        );
      }
      GC_ROOT_RESTORE(js, root_mark);
      return flat;
    }
    
    uint16_t left_depth = str_is_heap_rope(l) ? rope_depth(l) : 0;
    uint16_t right_depth = str_is_heap_rope(r) ? rope_depth(r) : 0;
    unsigned int new_depth = (unsigned int)(left_depth > right_depth ? left_depth : right_depth) + 1u;

    if (new_depth > ANT_ROPE_DEPTH_SATURATED)
      new_depth = ANT_ROPE_DEPTH_SATURATED;
    return js_mkrope(js, l, r, total_len, (uint16_t)new_depth);
  }
  
  ant_offset_t n1, off1 = vstr(js, l, &n1);
  ant_offset_t n2, off2 = vstr(js, r, &n2);
  
  if (op == TOK_EQ) {
    bool eq = n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
    return mkval(kTypeBool, eq ? 1 : 0);
  } else if (op == TOK_NE) {
    bool eq = n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
    return mkval(kTypeBool, eq ? 0 : 1);
  } else if (op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE) {
    int cmp = str_compare_utf16(
      (const char *)(uintptr_t)off1, (size_t)n1,
      (const char *)(uintptr_t)off2, (size_t)n2);
    
    switch (op) {
      case TOK_LT: return mkval(kTypeBool, cmp < 0 ? 1 : 0);
      case TOK_LE: return mkval(kTypeBool, cmp <= 0 ? 1 : 0);
      case TOK_GT: return mkval(kTypeBool, cmp > 0 ? 1 : 0);
      case TOK_GE: return mkval(kTypeBool, cmp >= 0 ? 1 : 0);
      default:     return js_mkerr(js, "bad str op");
    }
  } else return js_mkerr(js, "bad str op");
}

bool js_chain_may_override_has_instance(ant_t *js, ant_value_t obj, bool guard_absence) {
  ant_offset_t sym_off = (ant_offset_t)vdata(js->sym.hasInstance_sym);
  ant_object_t *function_proto = js_obj_ptr(js->sym.function_proto);
  
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH && is_object_type(obj); depth++) {
    ant_value_t o = js_as_obj(obj);
    ant_object_t *ptr = js_obj_ptr(o);
    if (ptr && ptr == function_proto) return false;
    if (!ptr || !ptr->shape || is_proxy(o) || ant_shape_lookup_symbol(ptr->shape, sym_off) >= 0) return true;
    if (ptr->flags.is_exotic && lookup_sym_descriptor(js, o, sym_off)) return true;
    if (guard_absence) ant_object_guard_absence(ptr);
    obj = ptr->proto;
  }
  
  return is_object_type(obj);
}

static bool instanceof_via_has_instance(ant_t *js, ant_value_t l, ant_value_t r, ant_value_t *out) {
  ant_value_t has_instance = js_get_sym(js, r, js->sym.hasInstance_sym);
  uint8_t hit = vtype(has_instance);
  
  if (is_err(has_instance)) *out = has_instance;
  else if (hit == kTypeFunction || hit == kTypeBuiltin) {
    ant_value_t args[1] = { l };
    ant_value_t result = sv_vm_call(js->vm, js, has_instance, r, args, 1, NULL, Ant_Value_Undefined);
    *out = is_err(result) ? result : Ant_Value_FromBool(js_truthy(js, result));
  } else if (hit != kTypeUndefined && hit != kTypeNull)
    *out = js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.hasInstance is not callable");
  else return false;
  
  return true;
}

static ant_value_t proxy_ordinary_has_instance(ant_t *js, ant_value_t l, ant_value_t r, uint8_t ltype) {
  if (ltype != kTypeObject && ltype != kTypeArray && ltype != kTypeFunction && ltype != kTypePromise && ltype != kTypeGenerator)
    return Ant_Value_False;
  
  ant_value_t proto_val = proxy_get(js, r, "prototype", 9);
  if (is_err(proto_val)) return proto_val;
  
  uint8_t pt = vtype(proto_val);
  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function has non-object prototype in instanceof check");
  
  return walk_prototype_chain(js, l, proto_val);
}

static ant_value_t handle_proxy_instanceof(ant_t *js, ant_value_t l, ant_value_t r, uint8_t ltype) {
  ant_value_t handled;
  if (instanceof_via_has_instance(js, l, r, &handled)) return handled;
  
  uint8_t ttype = vtype(proxy_read_target(js, r));
  if (ttype != kTypeFunction && ttype != kTypeBuiltin)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  return proxy_ordinary_has_instance(js, l, r, ltype);
}

static ant_value_t handle_cfunc_instanceof(ant_t *js, ant_value_t l, ant_value_t r, uint8_t ltype) {
  ant_cfunc_t fn = js_as_cfunc(r);
    
  if (fn == builtin_Array) {
    bool result = false;
    ant_value_t is_array_res = js_is_array_value_checked(js, l, &result);
    if (is_err(is_array_res)) return is_array_res;
    return mkval(kTypeBool, result ? 1 : 0);
  }
  
  if (fn == builtin_Object)
    return mkval(kTypeBool, ltype == kTypeObject ? 1 : 0);
  
  if (fn == builtin_Function)
    return mkval(kTypeBool, (ltype == kTypeFunction || ltype == kTypeBuiltin) ? 1 : 0);
  
  if (js_is_promise_ctor_cfunc(fn))
    return mkval(kTypeBool, ltype == kTypePromise ? 1 : 0);
  
  return mkval(kTypeBool, 0);
}

ant_value_t ordinary_has_instance(ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == kTypeBuiltin) l = js_cfunc_promote(js, l);
  if (is_err(l)) return l;
  
  uint8_t ltype = vtype(l);
  if (is_proxy(r)) {
    uint8_t ttype = vtype(proxy_read_target(js, r));
    return ttype == kTypeFunction || ttype == kTypeBuiltin ? proxy_ordinary_has_instance(js, l, r, ltype) : Ant_Value_False;
  }
  if (vtype(r) == kTypeBuiltin) return handle_cfunc_instanceof(js, l, r, ltype);
  if (vtype(r) != kTypeFunction) return Ant_Value_False;

  ant_value_t bound_target = js_resolve_bound_target(r);
  if (bound_target != r) return do_instanceof(js, l, bound_target);

  if (ltype != kTypeObject && ltype != kTypeArray && ltype != kTypeFunction && ltype != kTypePromise && ltype != kTypeGenerator)
    return Ant_Value_False;
  
  ant_prop_loc_t proto_off = lkp_interned(js_func_obj(r), js->intern.prototype);
  ant_value_t ctor_proto = proto_off.obj ? js_prop_load(proto_off) : js_get(js, r, "prototype");
  if (is_err(ctor_proto)) return ctor_proto;
  
  uint8_t pt = vtype(ctor_proto);
  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function has non-object prototype in instanceof check");
  
  return walk_prototype_chain(js, l, ctor_proto);
}

ant_value_t do_instanceof(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t ltype = vtype(l);
  uint8_t rtype = vtype(r);
  
  if (rtype != kTypeFunction && rtype != kTypeBuiltin) {
    if (is_proxy(r)) return handle_proxy_instanceof(js, l, r, ltype);
    ant_value_t handled;
    if (is_object_type(r) && instanceof_via_has_instance(js, l, r, &handled)) return handled;
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  }
  
  if (rtype == kTypeBuiltin) {
    return handle_cfunc_instanceof(js, l, r, ltype);
  }

  ant_value_t func_obj = js_func_obj(r);
  bool use_slow_has_instance = js_chain_may_override_has_instance(js, func_obj, false);

  ant_value_t handled;
  if (use_slow_has_instance && instanceof_via_has_instance(js, l, r, &handled)) return handled;
  return ordinary_has_instance(js, l, r);
}


static inline bool try_array_key_membership(
  ant_t *js,
  ant_value_t array,
  const char *name,
  ant_offset_t name_len,
  ant_value_t *result
) {
  if (is_length_key(name, name_len) && real_array_obj_ptr(array)) {
    *result = Ant_Value_True;
    return true;
  }

  unsigned long index;
  ant_offset_t array_len = get_array_length(js, array);
  if (!parse_array_index(name, name_len, array_len, &index)) return false;

  if (!arr_has(js, array, (ant_offset_t)index)) return false;
  *result = Ant_Value_True;
  
  return true;
}

static inline bool array_index_in(ant_t *js, ant_value_t arr, double d, ant_value_t *out) {
  ant_object_t *dense = array_dense_source(js, arr);
  
  if (!dense || ant_shape_may_have_index_keys(dense->shape)) return false;
  if (!(d >= 0 && d < 4294967295.0) || d != (double)(uint32_t)d) return false;
  
  uint32_t idx = (uint32_t)d;
  *out = Ant_Value_FromBool(
    idx < dense_iterable_length(js, arr) && idx < get_array_length(js, arr) &&
    !is_empty_slot(dense_get(get_dense_buf(arr), idx))
  );
  
  return true;
}

static ant_value_t in_lookup(ant_t *js, ant_value_t r, property_key_view_t *key) {
  if (!is_object_type(r)) return js_mkerr_typed(
    js, JS_ERR_TYPE, 
    "Cannot use 'in' operator to search for '%.*s' in non-object", 
    (int)key->length, key->bytes
  );

  ant_value_t array_result;
  if (
    !key->is_symbol && vtype(r) == kTypeArray && 
    try_array_key_membership(js, r, key->bytes, (ant_offset_t)key->length, &array_result)
  ) return array_result;
  
  ant_value_t cur = r;
  uint8_t cur_t = vtype(cur);
  
  proto_walk_overflow_guard_t guard;
  proto_walk_overflow_guard_init(&guard);

  do {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
    if (ptr->flags.is_exotic && is_proxy(js_as_obj(cur))) {
      ant_value_t result = key->is_symbol
        ? proxy_has_val(js, js_as_obj(cur), key->js_key)
        : proxy_has(js, js_as_obj(cur), key->bytes, (ant_offset_t)key->length);
      return is_err(result) ? result : Ant_Value_FromBool(js_truthy(js, result));
    }
    
    ant_value_t exotic = key->is_symbol ? Ant_Value_Undefined : exotic_own_get(js, js_as_obj(cur), ptr, key->bytes, key->length);
    if (vtype(exotic) != kTypeUndefined) return Ant_Value_FromBool(exotic != T_EMPTY);
    
    ant_value_t own = object_has_own_key(js, js_object_view(cur), key);
    if (own != Ant_Value_False) return own;
  } while (
    proto_walk_next(js, &cur, &cur_t, PROTO_WALK_F_LOOKUP) &&
    !proto_walk_overflow_guard_hit_cycle(js, &guard, cur, cur_t, PROTO_WALK_F_LOOKUP)
  );

  return Ant_Value_False;
}

ant_value_t js_in_str(ant_t *js, ant_value_t r, const char *key, size_t key_len) {
  if (vtype(r) == kTypeBuiltin) r = js_cfunc_promote(js, r);
  if (is_err(r)) return r;
  
  property_key_view_t view = { .js_key = Ant_Value_Undefined, .bytes = key, .length = key_len };
  return in_lookup(js, r, &view);
}

ant_value_t do_in(ant_t *js, ant_value_t l, ant_value_t r) {
  ant_offset_t prop_len;
  const char *prop_name;
  
  char num_buf[32];
  ant_value_t indexed;
  
  if (
    vtype(l) == kTypeNumber && vtype(r) == kTypeArray && 
    array_index_in(js, r, Ant_Value_AsNumberUnchecked(l), &indexed)
  ) return indexed;

  if (vtype(r) == kTypeBuiltin) r = js_cfunc_promote(js, r);
  if (is_err(r)) return r;

  ant_value_t key = vtype(l) == kTypeString ? l : js_to_primitive(js, l, 1);
  if (is_err(key)) return key;
  bool is_sym = (vtype(key) == kTypeSymbol);
  
  if (vtype(key) == kTypeString) {
    ant_offset_t prop_off = vstr(js, key, &prop_len);
    prop_name = (char *)(uintptr_t)(prop_off);
  } else if (is_sym) {
    const char *d = js_sym_desc(key);
    prop_name = d ? d : "symbol";
    prop_len = (ant_offset_t)strlen(prop_name);
  } else if (vtype(key) == kTypeNumber) {
    prop_len = (ant_offset_t)js_number_to_chars(key, num_buf, sizeof(num_buf));
    prop_name = num_buf;
  } else {
    ant_value_t key_str = js_tostring_val(js, key);
    if (is_err(key_str)) return key_str;
    ant_offset_t prop_off = vstr(js, key_str, &prop_len);
    prop_name = (char *)(uintptr_t)(prop_off);
  }

  property_key_view_t view = { 
    .js_key = key, 
    .bytes = prop_name, 
    .length = prop_len, 
    .is_symbol = is_sym,
  };
  
  return in_lookup(js, r, &view);
}
