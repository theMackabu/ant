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
  ant_value_t fn = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[vm->sp - 1];
  ant_value_t desc_obj = js_as_obj(obj);
  
  bool is_getter = (flags & 1) != 0;
  bool is_setter = (flags & 2) != 0;
  
  if (is_getter) {
    js_set_getter_desc(js, desc_obj, a->str, a->len, fn, JS_DESC_E | JS_DESC_C);
    return;
  }
  
  if (is_setter) {
    js_set_setter_desc(js, desc_obj, a->str, a->len, fn, JS_DESC_E | JS_DESC_C);
    return;
  }
  
  ant_value_t key = js_mkstr(js, a->str, a->len);
  mkprop(js, obj, key, fn, 0);
}

static inline void sv_op_define_method_comp(
  sv_vm_t *vm, ant_t *js,
  uint8_t *ip
) {
  uint8_t flags = sv_get_u8(ip + 1);
  ant_value_t fn = vm->stack[--vm->sp];
  ant_value_t key = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[vm->sp - 1];
  ant_value_t desc_obj = js_as_obj(obj);
  bool is_getter = (flags & 1) != 0;
  bool is_setter = (flags & 2) != 0;
  if (vtype(key) == T_SYMBOL) {
    if (is_getter) { js_set_sym_getter_desc(js, desc_obj, key, fn, JS_DESC_E | JS_DESC_C); return; }
    if (is_setter) { js_set_sym_setter_desc(js, desc_obj, key, fn, JS_DESC_E | JS_DESC_C); return; }
    js_set_sym(js, obj, key, fn);
    return;
  }
  ant_value_t key_str = sv_key_to_propstr(js, key);
  if ((is_getter || is_setter) && vtype(key_str) == T_STR) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)(uintptr_t)(koff);
    if (is_getter) js_set_getter_desc(js, desc_obj, kptr, klen, fn, JS_DESC_E | JS_DESC_C);
    else js_set_setter_desc(js, desc_obj, kptr, klen, fn, JS_DESC_E | JS_DESC_C);
    return;
  }
  if (vtype(key_str) == T_STR) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)(uintptr_t)(koff);
    js_define_own_prop(js, obj, kptr, (size_t)klen, fn);
  } else mkprop(js, obj, key_str, fn, 0);
}

static inline void sv_op_set_name(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[atom_idx];
  ant_value_t fn = vm->stack[vm->sp - 1];
  ant_value_t name = js_mkstr(js, a->str, a->len);
  setprop_cstr(js, fn, "name", 4, name);
}

static inline void sv_op_set_name_comp(sv_vm_t *vm, ant_t *js) {
  ant_value_t key = vm->stack[vm->sp - 1];
  ant_value_t fn = vm->stack[vm->sp - 2];
  ant_value_t name = coerce_to_str(js, key);
  setprop_cstr(js, fn, "name", 4, name);
}

static inline void sv_op_set_proto(sv_vm_t *vm, ant_t *js) {
  ant_value_t proto = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[vm->sp - 1];
  
  uint8_t pt = vtype(proto);
  if (pt == T_OBJ || pt == T_NULL || pt == T_FUNC || pt == T_ARR) js_set_proto_wb(js, obj, proto);
}

static inline void sv_op_set_home_obj(sv_vm_t *vm, ant_t *js) {
  ant_value_t home = vm->stack[vm->sp - 1];
  ant_value_t fn = vm->stack[vm->sp - 2];
  sv_closure_t *c = js_func_closure(fn);
  c->super_val = home;
  c->call_flags |= SV_CALL_HAS_SUPER;
}

static inline void sv_op_append(sv_vm_t *vm, ant_t *js) {
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t arr = vm->stack[vm->sp - 2];
  js_arr_push(js, arr, val);
}

static inline void sv_op_copy_data_props(
  sv_vm_t *vm, ant_t *js,
  uint8_t *ip
) {
  if (vm->sp < 2) return;
  ant_value_t src = vm->stack[vm->sp - 1];
  ant_value_t dst = vm->stack[vm->sp - 2];
  if (!is_object_type(src) || !is_object_type(dst)) return;
  
  const char *key = NULL;
  size_t key_len = 0;
  ant_iter_t iter = js_prop_iter_begin(js, src);
  
  while (js_prop_iter_next(&iter, &key, &key_len, NULL)) {
    ant_value_t val = js_get(js, src, key);
    ant_value_t prop_key = js_mkstr(js, key, key_len);
    js_setprop(js, dst, prop_key, val);
  }
  
  js_prop_iter_end(&iter);
}

static inline ant_value_t sv_op_spread(sv_vm_t *vm, ant_t *js) {
  if (vm->sp < 2)
    return js_mkerr(js, "invalid spread state");

  ant_value_t iterable = vm->stack[--vm->sp];
  ant_value_t arr = vm->stack[vm->sp - 1];
  if (vtype(arr) != T_ARR)
    return js_mkerr(js, "spread target is not an array");

  if (vtype(iterable) == T_ARR) {
    ant_offset_t len = js_arr_len(js, iterable);
    for (ant_offset_t i = 0; i < len; i++)
      js_arr_push(js, arr, js_arr_get(js, iterable, i));
    return tov(0);
  }

  if (vtype(iterable) == T_STR) {
    if (str_is_heap_rope(iterable)) {
      iterable = rope_flatten(js, iterable);
      if (is_err(iterable)) return iterable;
    }
    ant_offset_t slen = str_len_fast(js, iterable);
    for (ant_offset_t i = 0; i < slen; ) {
      ant_offset_t off = vstr(js, iterable, NULL);
      utf8proc_int32_t cp;
      ant_offset_t cb_len = (ant_offset_t)utf8_next(
        (const utf8proc_uint8_t *)(uintptr_t)(off + i),
        (utf8proc_ssize_t)(slen - i), &cp
      );
      js_arr_push(js, arr, js_mkstr(js, (const void *)(uintptr_t)(off + i), cb_len));
      i += cb_len;
    }
    return tov(0);
  }

  ant_value_t iter_fn = js_get_sym(js, iterable, get_iterator_sym());
  uint8_t ft = vtype(iter_fn);
  if (ft != T_FUNC && ft != T_CFUNC)
    return js_mkerr(js, "not iterable");

  ant_value_t iterator = sv_vm_call(vm, js, iter_fn, iterable, NULL, 0, NULL, false);
  if (is_err(iterator)) return iterator;
  if (!is_object_type(iterator))
    return js_mkerr(js, "not iterable");

  ant_value_t status = tov(0);

  for (;;) {
    ant_value_t next_method = js_getprop_fallback(js, iterator, "next");
    ft = vtype(next_method);
    if (ft != T_FUNC && ft != T_CFUNC) {
      status = js_mkerr(js, "iterator.next is not a function");
      break;
    }

    ant_value_t result = sv_vm_call(vm, js, next_method, iterator, NULL, 0, NULL, false);
    if (is_err(result)) {
      status = result;
      break;
    }
    if (!is_object_type(result)) {
      status = js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result is not an object");
      break;
    }

    ant_value_t done = js_getprop_fallback(js, result, "done");
    if (js_truthy(js, done))
      break;

    ant_value_t value = js_getprop_fallback(js, result, "value");
    js_arr_push(js, arr, value);
  }

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
  ant_value_t ctor = vm->stack[vm->sp - 1];
  ant_value_t parent = vm->stack[vm->sp - 2];
  
  uint8_t pt = vtype(parent);
  bool parent_is_object = is_object_type(parent);
  bool parent_is_callable = (pt == T_FUNC || pt == T_CFUNC);

  if (vtype(ctor) == T_UNDEF) {
    ant_value_t ctor_obj = mkobj(js, 0);
    ctor = js_obj_to_func_ex(ctor_obj, SV_CALL_IS_DEFAULT_CTOR);
    ant_value_t func_proto = js_get_slot(js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC)
      js_set_proto_init(ctor_obj, func_proto);
  }

  ant_value_t proto = mkobj(js, 0);

  if (pt == T_NULL) {
    js_set_proto_init(proto, js_mknull());
    ant_value_t func_proto = js_get_slot(js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) js_set_proto_wb(js, ctor, func_proto);
  } else if (parent_is_object) {
    ant_value_t parent_proto = js_getprop_fallback(js, parent, "prototype");
    if (is_object_type(parent_proto)) js_set_proto_init(proto, parent_proto);
    js_set_proto_wb(js, ctor, parent);
  } else {
    ant_value_t object_ctor = js_getprop_fallback(js, js->global, "Object");
    if (vtype(object_ctor) == T_FUNC) {
      ant_value_t object_proto = js_getprop_fallback(js, object_ctor, "prototype");
      if (is_object_type(object_proto)) js_set_proto_init(proto, object_proto);
    }
  }
  if (parent_is_callable) {
    if (vtype(ctor) == T_FUNC) {
      sv_closure_t *c = js_func_closure(ctor);
      c->super_val = parent;
      c->call_flags |= SV_CALL_HAS_SUPER;
    }
  }

  if (vtype(ctor) == T_FUNC) js_mark_constructor(js_func_obj(ctor), true);
  setprop_interned(js, proto, "constructor", 11, ctor);
  setprop_interned(js, ctor, "prototype", 9, proto);
  if (a && a->len > 0)
    setprop_cstr(js, ctor, "name", 4, js_mkstr(js, a->str, a->len));

  vm->stack[vm->sp - 2] = ctor;
  vm->stack[vm->sp - 1] = proto;
}

static inline void sv_op_define_class_comp(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  ant_value_t name = vm->stack[vm->sp - 1];
  vm->sp--;
  sv_op_define_class(vm, js, func, ip);
  vm->stack[vm->sp++] = name;
}

static inline void sv_op_add_brand(sv_vm_t *vm) {
  vm->sp -= 2;
}

#endif
