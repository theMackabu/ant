#ifndef SV_OBJECTS_H
#define SV_OBJECTS_H

#include "utf8.h"
#include "property.h"
#include "iteration.h"
#include "descriptors.h"
#include "gc/roots.h"

static inline void sv_define_method(
  ant_t *js, sv_func_t *func, uint8_t *ip,
  ant_value_t obj, ant_value_t fn
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  uint8_t flags = sv_get_u8(ip + 5);
  
  sv_atom_t *a = &func->atoms[atom_idx];
  ant_value_t desc_obj = js_as_obj(obj);
  
  bool is_getter = (flags & SV_DEFINE_METHOD_GETTER) != 0;
  bool is_setter = (flags & SV_DEFINE_METHOD_SETTER) != 0;
  
  uint8_t data_attrs = (flags & SV_DEFINE_METHOD_NON_ENUM)
    ? (ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE)
    : ANT_PROP_ATTR_DEFAULT;
  
  uint8_t accessor_desc = (flags & SV_DEFINE_METHOD_NON_ENUM)
    ? JS_DESC_C
    : (JS_DESC_E | JS_DESC_C);
  
  if (flags & SV_DEFINE_METHOD_SET_NAME) {
    const char *prefix = is_getter ? "get " : is_setter ? "set " : "";
    size_t prefix_len = is_getter || is_setter ? 4 : 0;
    ant_value_t key = js_mkstr(js, a->str, a->len);
    ant_value_t named = js_maybe_set_function_name_from_key(js, fn, key, prefix, prefix_len);
    if (is_err(named)) return;
  }
  
  if (is_getter) {
    js_set_getter_desc(js, desc_obj, a->str, a->len, fn, accessor_desc);
    return;
  }
  
  if (is_setter) {
    js_set_setter_desc(js, desc_obj, a->str, a->len, fn, accessor_desc);
    return;
  }
  
  ant_value_t key = js_mkstr(js, a->str, a->len);
  mkprop(js, obj, key, fn, data_attrs);
}

static inline void sv_op_define_method(
  sv_vm_t *vm, ant_t *js, sv_func_t *func, uint8_t *ip
) {
  sv_define_method(js, func, ip, vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]);
  vm->sp--;
}

static inline void sv_define_method_comp(
  ant_t *js, uint8_t flags,
  ant_value_t obj, ant_value_t *key_slot, ant_value_t fn
) {
  ant_value_t key = *key_slot;
  ant_value_t desc_obj = js_as_obj(obj);
  
  bool is_getter = (flags & SV_DEFINE_METHOD_GETTER) != 0;
  bool is_setter = (flags & SV_DEFINE_METHOD_SETTER) != 0;
  
  uint8_t data_attrs = (flags & SV_DEFINE_METHOD_NON_ENUM)
    ? (ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE)
    : ANT_PROP_ATTR_DEFAULT;
  
  uint8_t accessor_desc = (flags & SV_DEFINE_METHOD_NON_ENUM)
    ? JS_DESC_C
    : (JS_DESC_E | JS_DESC_C);
  
  if (flags & SV_DEFINE_METHOD_SET_NAME) {
    const char *prefix = is_getter ? "get " : is_setter ? "set " : "";
    size_t prefix_len = is_getter || is_setter ? 4 : 0;
    ant_value_t named = js_maybe_set_function_name_from_key(js, fn, key, prefix, prefix_len);
    if (is_err(named)) return;
  }
  
  if (vtype(key) == kTypeSymbol) {
    if (is_getter) { js_set_sym_getter_desc(js, desc_obj, key, fn, accessor_desc); return; }
    if (is_setter) { js_set_sym_setter_desc(js, desc_obj, key, fn, accessor_desc); return; }
    mkprop(js, obj, key, fn, data_attrs);
    return;
  }
  
  ant_value_t key_str = sv_key_to_propstr(js, key);
  if (!is_err(key_str)) *key_slot = key_str;
  
  if ((is_getter || is_setter) && vtype(key_str) == kTypeString) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)(uintptr_t)(koff);
    if (is_getter) js_set_getter_desc(js, desc_obj, kptr, klen, fn, accessor_desc);
    else js_set_setter_desc(js, desc_obj, kptr, klen, fn, accessor_desc);
    return;
  }
  
  if (vtype(key_str) == kTypeString) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)(uintptr_t)(koff);
    mkprop(js, obj, js_mkstr(js, kptr, (size_t)klen), fn, data_attrs);
  } else mkprop(js, obj, key_str, fn, data_attrs);
}

static inline void sv_op_define_method_comp(
  sv_vm_t *vm, ant_t *js, uint8_t *ip
) {
  sv_define_method_comp(
    js, sv_get_u8(ip + 1),
    vm->stack[vm->sp - 3], &vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]
  );
  vm->sp -= 2;
}

static inline void sv_set_name(
  ant_t *js, ant_value_t fn,
  const char *str, uint32_t len
) {
  if (vtype(fn) == kTypeFunction) {
    sv_closure_t *c = js_func_closure(fn);
    if (!c->func_obj) {
      c->u.pending.name = str;
      c->u.pending.len = len;
      return;
    }
  }
  js_set_function_name(js, fn, str, len);
}

static inline void sv_op_set_name(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[atom_idx];
  sv_set_name(js, vm->stack[vm->sp - 1], a->str, a->len);
}

static inline void sv_op_set_name_comp(sv_vm_t *vm, ant_t *js) {
  ant_value_t key = vm->stack[vm->sp - 1];
  ant_value_t fn = vm->stack[vm->sp - 2];
  js_set_function_name_from_key(js, fn, key, "", 0);
}

static inline void sv_op_set_proto(sv_vm_t *vm, ant_t *js) {
  ant_value_t proto = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[vm->sp - 1];
  
  js_set_literal_proto(js, obj, proto);
}

static inline void sv_op_set_home_obj(sv_vm_t *vm, ant_t *js) {
  ant_value_t home = vm->stack[vm->sp - 1];
  ant_value_t fn = vm->stack[vm->sp - 2];
  sv_closure_t *c = js_func_closure(fn);
  c->super_val = home;
  c->call_flags |= SV_CALL_HAS_SUPER;
}

static inline void sv_op_append(sv_vm_t *vm, ant_t *js) {
  js_arr_push(js, vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]);
  vm->sp--;
}

static inline ant_value_t sv_op_copy_data_props(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t excluded_count = sv_get_u16(ip + 1);
  ant_value_t *excluded = &vm->stack[vm->sp - excluded_count];
  
  ant_value_t src = vm->stack[vm->sp - excluded_count - 1];
  ant_value_t dst = vm->stack[vm->sp - excluded_count - 2];
  
  ant_value_t result = js_copy_data_props(js, dst, src, excluded, excluded_count);
  vm->sp -= excluded_count;
  
  return result;
}

static inline ant_value_t sv_spread_into(sv_vm_t *vm, ant_t *js, ant_value_t arr, ant_value_t iterable) {
  if (vtype(arr) != kTypeArray) return js_mkerr(js, "spread target is not an array");

  if (vtype(iterable) == kTypeArray && js_array_iteration_default(js, iterable)) {
    ant_value_t spread = js_arr_spread_into(js, arr, iterable);
    return is_err(spread) ? spread : tov(0);
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t state[3] = { js_mkundef(), js_mkundef(), js_mkundef() };
  
  GC_ROOT_PIN(js, state[0]);
  GC_ROOT_PIN(js, state[1]);
  
  ant_value_t status = sv_for_of_open(vm, js, iterable, state);
  while (!is_err(status)) {
    ant_value_t value = js_mkundef();
    bool done = true;
    
    status = sv_iter_advance_values(vm, js, state, &value, &done);
    if (is_err(status) || done) break;
    state[1] = status;
    
    js_arr_push(js, arr, value);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return is_err(status) ? status : tov(0);
}

static inline ant_value_t sv_op_spread(sv_vm_t *vm, ant_t *js) {
  if (vm->sp < 2) return js_mkerr(js, "invalid spread state");
  ant_value_t status = sv_spread_into(vm, js, vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]);
  vm->sp--;
  return status;
}

static inline ant_value_t sv_op_define_class(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  uint8_t cls_flags = sv_get_u8(ip + 5);
  
  uint32_t source_start = sv_get_u32(ip + 6);
  uint32_t source_end = sv_get_u32(ip + 10);
  
  bool has_name = (cls_flags & SV_CLASS_FLAG_HAS_NAME) && (int)atom_idx < func->atom_count;
  bool has_heritage = (cls_flags & SV_CLASS_FLAG_HAS_HERITAGE) != 0;
  
  sv_atom_t *a = has_name ? &func->atoms[atom_idx] : NULL;
  ant_value_t ctor = vm->stack[vm->sp - 1];
  ant_value_t parent = vm->stack[vm->sp - 2];
  ant_value_t proto = js_mkundef();
  
  uint8_t pt = vtype(parent);
  bool parent_is_callable = false;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ctor);
  GC_ROOT_PIN(js, proto);

  bool default_ctor = vtype(ctor) == kTypeUndefined;
  if (default_ctor) {
    ant_value_t ctor_obj = mkobj(js, 0);
    
    ctor = js_obj_to_func_ex(js, ctor_obj, SV_CALL_IS_DEFAULT_CTOR);
    ant_value_t func_proto = js_get_slot(js->global, SLOT_FUNC_PROTO);
    
    if (vtype(func_proto) == kTypeFunction) js_set_proto_init(ctor_obj, func_proto);
    mkprop_interned_exact(js, ctor_obj, js->intern.length, tov(0), ANT_PROP_ATTR_CONFIGURABLE);
  }

  proto = mkobj(js, 0);
  if (!has_heritage) js_set_proto_init(proto, js->sym.object_proto);
  
  else if (pt == kTypeNull) {
    js_set_proto_init(proto, js_mknull());
    ant_value_t func_proto = js_get_slot(js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == kTypeFunction) js_set_proto_wb(js, ctor, func_proto);
  } else {
    if (!js_is_constructor(parent)) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Class extends value is not a constructor");
    }

    ant_value_t parent_proto = js_getprop_fallback(js, parent, "prototype");
    if (is_err(parent_proto)) {
      GC_ROOT_RESTORE(js, root_mark);
      return parent_proto;
    }
    
    if (!is_object_type(parent_proto) && vtype(parent_proto) != kTypeNull) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Class extends value does not have a valid prototype property");
    }
    
    js_set_proto_init(proto, parent_proto);
    js_set_proto_wb(js, ctor, parent);
    parent_is_callable = true;
  }
  
  if (parent_is_callable) if (vtype(ctor) == kTypeFunction) {
    sv_closure_t *c = js_func_closure(ctor);
    c->super_val = parent;
    c->call_flags |= SV_CALL_HAS_SUPER;
  }

  if (vtype(ctor) == kTypeFunction)
    js_mark_constructor(js_func_obj(ctor), true);
  
  if (
    vtype(ctor) == kTypeFunction && func->debug->source &&
    source_end > source_start && source_end <= (uint32_t)func->debug->source_len
  ) {
    size_t source_len = (size_t)(source_end - source_start);
    const char *source = func->debug->source + source_start;
    js_set_func_source(
      js, ctor, source, source_len, 
      func->unit ? JS_FUNC_SOURCE_UNIT : JS_FUNC_SOURCE_PINNED
    );
  }
  
  defhidden(js, proto, "constructor", 11, ctor);
  ant_value_t ctor_obj = (vtype(ctor) == kTypeFunction) ? js_func_obj(ctor) : ctor;
  
  if (has_name || default_ctor) js_set_function_name(js, ctor, a ? a->str : "", a ? a->len : 0);
  mkprop_exact_attrs(js, ctor_obj, ANT_STRING("prototype"), proto, 0);

  vm->stack[vm->sp - 2] = ctor;
  vm->stack[vm->sp - 1] = proto;
  GC_ROOT_RESTORE(js, root_mark);
  
  return js_mkundef();
}

static inline ant_value_t sv_op_define_class_comp(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  ant_value_t name = vm->stack[vm->sp - 1];
  vm->sp--;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, name);
  
  ant_value_t result = sv_op_define_class(vm, js, func, ip);
  GC_ROOT_RESTORE(js, root_mark);
  
  if (is_err(result)) return result;
  vm->stack[vm->sp++] = name;
  
  return result;
}

#endif
