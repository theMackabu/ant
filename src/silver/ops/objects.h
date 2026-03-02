#ifndef SV_OBJECTS_H
#define SV_OBJECTS_H

#include "utf8.h"
#include "property.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/symbol.h"

static inline void sv_op_define_method(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  uint8_t flags = sv_get_u8(ip + 5);
  sv_atom_t *a = &func->atoms[atom_idx];
  jsval_t fn = vm->stack[--vm->sp];
  jsval_t obj = vm->stack[vm->sp - 1];
  bool is_getter = (flags & 1) != 0;
  bool is_setter = (flags & 2) != 0;
  if (is_getter) {
    js_set_getter_desc(js, obj, a->str, a->len, fn, JS_DESC_C);
    return;
  }
  if (is_setter) {
    js_set_setter_desc(js, obj, a->str, a->len, fn, JS_DESC_C);
    return;
  }
  jsval_t key = js_mkstr(js, a->str, a->len);
  mkprop(js, obj, key, fn, 0);
}

static inline void sv_op_define_method_comp(
  sv_vm_t *vm, ant_t *js,
  uint8_t *ip
) {
  uint8_t flags = sv_get_u8(ip + 1);
  jsval_t fn = vm->stack[--vm->sp];
  jsval_t key = vm->stack[--vm->sp];
  jsval_t obj = vm->stack[vm->sp - 1];
  bool is_getter = (flags & 1) != 0;
  bool is_setter = (flags & 2) != 0;
  if (vtype(key) == T_SYMBOL && !is_getter && !is_setter) {
    js_set_sym(js, obj, key, fn);
    return;
  }
  jsval_t key_str = sv_key_to_propstr(js, key);
  if ((is_getter || is_setter) && vtype(key_str) == T_STR) {
    jsoff_t klen = 0;
    jsoff_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)&js->mem[koff];
    if (is_getter) js_set_getter_desc(js, obj, kptr, klen, fn, JS_DESC_C);
    else js_set_setter_desc(js, obj, kptr, klen, fn, JS_DESC_C);
    return;
  }
  if (vtype(key_str) == T_STR) {
    jsoff_t klen = 0;
    jsoff_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)&js->mem[koff];
    js_define_own_prop(js, obj, kptr, (size_t)klen, fn);
  } else mkprop(js, obj, key_str, fn, 0);
}

static inline void sv_op_set_name(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[atom_idx];
  jsval_t fn = vm->stack[vm->sp - 1];
  jsval_t name = js_mkstr(js, a->str, a->len);
  setprop_cstr(js, fn, "name", 4, name);
}

static inline void sv_op_set_name_comp(sv_vm_t *vm, ant_t *js) {
  jsval_t key = vm->stack[vm->sp - 1];
  jsval_t fn = vm->stack[vm->sp - 2];
  jsval_t name = coerce_to_str(js, key);
  setprop_cstr(js, fn, "name", 4, name);
}

static inline void sv_op_set_proto(sv_vm_t *vm, ant_t *js) {
  jsval_t proto = vm->stack[--vm->sp];
  jsval_t obj = vm->stack[vm->sp - 1];
  uint8_t pt = vtype(proto);
  if (pt == T_OBJ || pt == T_NULL || pt == T_FUNC || pt == T_ARR)
    js_set_proto(js, obj, proto);
}

static inline void sv_op_set_home_obj(sv_vm_t *vm, ant_t *js) {
  jsval_t home = vm->stack[vm->sp - 1];
  jsval_t fn = vm->stack[vm->sp - 2];
  jsval_t fn_obj = js_func_obj(fn);
  js_set_slot(js, fn_obj, SLOT_SUPER, home);
  sv_closure_t *c = js_func_closure(fn);
  if (c->func != NULL)
    c->call_flags |= SV_CALL_HAS_SUPER;
}

static inline void sv_op_append(sv_vm_t *vm, ant_t *js) {
  jsval_t val = vm->stack[--vm->sp];
  jsval_t arr = vm->stack[vm->sp - 2];
  js_arr_push(js, arr, val);
}

static inline void sv_op_copy_data_props(
  sv_vm_t *vm, ant_t *js,
  uint8_t *ip
) {
  if (vm->sp < 2) return;
  jsval_t src = vm->stack[vm->sp - 1];
  jsval_t dst = vm->stack[vm->sp - 2];
  if (!is_object_type(src) || !is_object_type(dst)) return;
  
  const char *key = NULL;
  size_t key_len = 0;
  jsval_t val = js_mkundef();
  ant_iter_t iter = js_prop_iter_begin(js, src);
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    jsval_t prop_key = js_mkstr(js, key, key_len);
    js_setprop(js, dst, prop_key, val);
  }
  
  js_prop_iter_end(&iter);
}

static inline jsval_t sv_op_spread(sv_vm_t *vm, ant_t *js) {
  if (vm->sp < 2)
    return js_mkerr(js, "invalid spread state");

  jsval_t iterable = vm->stack[--vm->sp];
  jsval_t arr = vm->stack[vm->sp - 1];
  if (vtype(arr) != T_ARR)
    return js_mkerr(js, "spread target is not an array");

  if (vtype(iterable) == T_ARR) {
    jsoff_t len = js_arr_len(js, iterable);
    for (jsoff_t i = 0; i < len; i++)
      js_arr_push(js, arr, js_arr_get(js, iterable, i));
    return tov(0);
  }

  if (vtype(iterable) == T_STR) {
    if (is_rope(js, iterable)) {
      iterable = rope_flatten(js, iterable);
      if (is_err(iterable)) return iterable;
    }
    jsoff_t slen = str_len_fast(js, iterable);
    for (jsoff_t i = 0; i < slen; ) {
      jsoff_t off = vstr(js, iterable, NULL);
      utf8proc_int32_t cp;
      jsoff_t cb_len = (jsoff_t)utf8_next(
        (const utf8proc_uint8_t *)&js->mem[off + i],
        (utf8proc_ssize_t)(slen - i), &cp
      );
      js_arr_push(js, arr, js_mkstr(js, (char *)&js->mem[off + i], cb_len));
      i += cb_len;
    }
    return tov(0);
  }

  jsval_t iter_fn = js_get_sym(js, iterable, get_iterator_sym());
  uint8_t ft = vtype(iter_fn);
  if (ft != T_FUNC && ft != T_CFUNC)
    return js_mkerr(js, "not iterable");

  jsval_t iterator = sv_vm_call(vm, js, iter_fn, iterable, NULL, 0, NULL, false);
  if (is_err(iterator)) return iterator;
  if (!is_object_type(iterator))
    return js_mkerr(js, "not iterable");

  jshdl_t hiter = js_root(js, iterator);
  jsval_t status = tov(0);

  for (;;) {
    jsval_t cur_iter = js_deref(js, hiter);
    jsval_t next_method = js_getprop_fallback(js, cur_iter, "next");
    ft = vtype(next_method);
    if (ft != T_FUNC && ft != T_CFUNC) {
      status = js_mkerr(js, "iterator.next is not a function");
      break;
    }

    jsval_t result = sv_vm_call(vm, js, next_method, cur_iter, NULL, 0, NULL, false);
    if (is_err(result)) {
      status = result;
      break;
    }
    if (!is_object_type(result)) {
      status = js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result is not an object");
      break;
    }

    jsval_t done = js_getprop_fallback(js, result, "done");
    if (js_truthy(js, done))
      break;

    jsval_t value = js_getprop_fallback(js, result, "value");
    js_arr_push(js, arr, value);
  }

  js_unroot(js, hiter);
  return status;
}

static inline void sv_op_define_class(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  uint8_t cls_flags = sv_get_u8(ip + 5);
  
  bool has_name = (cls_flags & 1) && (int)atom_idx < func->atom_count;
  sv_atom_t *a = has_name ? &func->atoms[atom_idx] : NULL;
  jsval_t ctor = vm->stack[vm->sp - 1];
  jsval_t parent = vm->stack[vm->sp - 2];
  
  uint8_t pt = vtype(parent);
  bool parent_is_object = is_object_type(parent);
  bool parent_is_callable = (pt == T_FUNC || pt == T_CFUNC);

  if (vtype(ctor) == T_UNDEF) {
    jsval_t ctor_obj = mkobj(js, 0);
    ctor = js_obj_to_func(ctor_obj);
    js_set_slot(js, ctor_obj, SLOT_DEFAULT_CTOR, js_true);
    jsval_t func_proto = js_get_slot(js, js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC)
      js_set_proto(js, ctor_obj, func_proto);
  }

  jsval_t proto = mkobj(js, 0);
  jshdl_t hp = js_root(js, proto);
  jshdl_t hc = js_root(js, ctor);

  if (pt == T_NULL) {
    js_set_proto(js, js_deref(js, hp), js_mknull());
    jsval_t func_proto = js_get_slot(js, js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC)
      js_set_proto(js, js_deref(js, hc), func_proto);
  } else if (parent_is_object) {
    jsval_t parent_proto = js_getprop_fallback(js, parent, "prototype");
    if (is_object_type(parent_proto))
      js_set_proto(js, js_deref(js, hp), parent_proto);
    js_set_proto(js, js_deref(js, hc), parent);
  } else {
    jsval_t object_ctor = js_getprop_fallback(js, js->global, "Object");
    if (vtype(object_ctor) == T_FUNC) {
      jsval_t object_proto = js_getprop_fallback(js, object_ctor, "prototype");
      if (is_object_type(object_proto))
        js_set_proto(js, js_deref(js, hp), object_proto);
    }
  }
  if (parent_is_callable)
    js_set_slot(js, js_deref(js, hc), SLOT_SUPER, parent);

  ctor = js_deref(js, hc);
  proto = js_deref(js, hp);
  setprop_interned(js, proto, "constructor", 11, ctor);
  setprop_interned(js, ctor, "prototype", 9, proto);
  if (a && a->len > 0)
    setprop_cstr(js, ctor, "name", 4, js_mkstr(js, a->str, a->len));

  js_unroot(js, hc);
  js_unroot(js, hp);

  vm->stack[vm->sp - 2] = ctor;
  vm->stack[vm->sp - 1] = proto;
}

static inline void sv_op_define_class_comp(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  jsval_t name = vm->stack[vm->sp - 1];
  vm->sp--;
  sv_op_define_class(vm, js, func, ip);
  vm->stack[vm->sp++] = name;
}

static inline void sv_op_add_brand(sv_vm_t *vm) {
  vm->sp -= 2;
}

#endif
