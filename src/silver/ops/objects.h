#pragma once

#include "utf8.h"
#include "property.h"
#include "iteration.h"
#include "descriptors.h"
#include "props.h"
#include "arrays.h"
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

ant_value_t sv_define_field(
  ant_t *js, uint8_t flags,
  ant_value_t obj, ant_value_t key, ant_value_t value
);

static inline ant_value_t sv_define_method_comp(
  ant_t *js, uint8_t flags,
  ant_value_t obj, ant_value_t *key_slot, ant_value_t fn
) {
  ant_value_t key = *key_slot;

  if (vtype(key) != kTypeString && vtype(key) != kTypeSymbol) {
    key = is_object_type(key) || vtype(key) == kTypeBuiltin
      ? js_to_property_key(js, key)
      : sv_key_to_propstr(js, key);
    if (is_err(key)) return key;
    *key_slot = key;
  }
  
  if (flags & SV_DEFINE_METHOD_FIELD) return sv_define_field(js, flags, obj, key, fn);
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
    if (is_err(named)) return named;
  }
  
  if (vtype(key) == kTypeSymbol) {
    if (is_getter) js_set_sym_getter_desc(js, desc_obj, key, fn, accessor_desc);
    else if (is_setter) js_set_sym_setter_desc(js, desc_obj, key, fn, accessor_desc);
    else mkprop_symbol_attrs_impl(js, obj, key, fn, data_attrs, MKPROP_USE_DEFAULT_ATTRS);
    return Ant_Value_Undefined;
  }
  
  ant_offset_t klen = 0;
  const char *kptr = (const char *)(uintptr_t)vstr(js, key, &klen);

  if (
    (flags & SV_DEFINE_METHOD_NON_ENUM) && vtype(obj) == kTypeFunction &&
    klen == 9 && memcmp(kptr, "prototype", 9) == 0
  ) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine property: prototype");
  
  if (is_getter) js_set_getter_desc(js, desc_obj, kptr, klen, fn, accessor_desc);
  else if (is_setter) js_set_setter_desc(js, desc_obj, kptr, klen, fn, accessor_desc);
  else mkprop(js, obj, key, fn, data_attrs);
  
  return Ant_Value_Undefined;
}

static inline ant_value_t sv_op_define_method_comp(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t result = sv_define_method_comp(
    js, sv_get_u8(ip + 1),
    vm->stack[vm->sp - 3], &vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]
  );
  vm->sp -= 2;
  return result;
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

ant_value_t sv_spread_into(sv_vm_t *vm, ant_t *js, ant_value_t arr, ant_value_t iterable);

static inline ant_value_t sv_op_spread(sv_vm_t *vm, ant_t *js) {
  if (vm->sp < 2) return js_mkerr(js, "invalid spread state");
  ant_value_t status = sv_spread_into(vm, js, vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]);
  vm->sp--;
  return status;
}

ant_value_t sv_op_define_class(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
);

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

#ifdef SV_OPS_IMPLEMENTATION

static bool sv_field_key_special_on_function(ant_t *js, ant_value_t key) {
  if (vtype(key) != kTypeString) return true;
  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  if (!str) return true;
  
  switch (len) {
    case 4: return memcmp(str, "name", 4) == 0;
    case 6: return memcmp(str, "length", 6) == 0 || memcmp(str, "caller", 6) == 0;
    case 9: return memcmp(str, "prototype", 9) == 0 || memcmp(str, "arguments", 9) == 0;
    default: return false;
  }
}

static bool sv_field_key_special_on_array(ant_t *js, ant_value_t key) {
  if (vtype(key) != kTypeString) return false;
  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  return !str || is_length_key(str, len) || is_array_index(str, (ant_offset_t)len);
}

static __attribute__((noinline)) bool sv_field_target_numeric_exotic(ant_t *js, ant_value_t obj, ant_value_t key) {
  static constexpr uint16_t mask = ANT_OBJECT_FLAG_EXTENSIBLE | ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_EXOTIC;
  if (vtype(obj) != kTypeObject) return false;
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || (ptr->flags.raw & mask) != (ANT_OBJECT_FLAG_EXTENSIBLE | ANT_OBJECT_FLAG_EXOTIC)) return false;
  
  const ant_exotic_ops_t *ops = ant_object_exotic_ops(ptr);
  if (!ops || ops->keys != ANT_EXOTIC_KEYS_NUMERIC || is_proxy(obj)) return false;
  if (vtype(key) == kTypeSymbol) return true;
  
  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  return str && !ant_key_may_be_numeric(str, len);
}

static __attribute__((noinline)) bool sv_field_key_special_on_string_wrapper(
  ant_t *js, ant_value_t primitive, ant_value_t key
) {
  if (vtype(primitive) != kTypeString || vtype(key) != kTypeString) return false;
  
  size_t len = 0;
  const char *str = js_getstr(js, key, &len);
  return !str || ant_key_may_be_numeric(str, len);
}

static bool sv_field_target_plain(ant_t *js, ant_value_t obj, ant_value_t key) {
  static constexpr uint16_t mask = ANT_OBJECT_FLAG_EXTENSIBLE | ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_EXOTIC;
  uint8_t type = vtype(obj);
  bool function = type == kTypeFunction;
  
  if (type != kTypeObject) {
    if (function) { if (sv_field_key_special_on_function(js, key)) return false; }
    else if (type == kTypeArray) { if (sv_field_key_special_on_array(js, key)) return false; }
    else if (type != kTypePromise && type != kTypeGenerator) return false;
  }
  
  ant_value_t holder = function ? js_as_obj(obj) : obj;
  ant_object_t *ptr = js_obj_ptr(holder);
  if (!ptr || (ptr->flags.raw & mask) != ANT_OBJECT_FLAG_EXTENSIBLE) return false;
  if (type != kTypeObject || !ptr->extra_slots) return true;
  
  const ant_extra_slot_t *primitive = ant_object_extra_slot(ptr, SLOT_PRIMITIVE);
  return !primitive || !sv_field_key_special_on_string_wrapper(js, primitive->value, key);
}

ant_value_t sv_define_field(
  ant_t *js, uint8_t flags,
  ant_value_t obj, ant_value_t key, ant_value_t value
) {
  if (flags & SV_DEFINE_METHOD_SET_NAME) {
    ant_value_t named = js_maybe_set_function_name_from_key(js, value, key, "", 0);
    if (is_err(named)) return named;
  }
  
  if (!sv_field_target_plain(js, obj, key) && !sv_field_target_numeric_exotic(js, obj, key))
    return js_create_data_property_or_throw(js, obj, key, value);
  
  ant_value_t defined;
  if (vtype(key) == kTypeSymbol) {
    defined = mkprop_symbol_attrs_impl(js, obj, key, value, 0, MKPROP_USE_DEFAULT_ATTRS | MKPROP_DATA_DEFINE);
  } else {
    ant_offset_t klen = 0;
    const char *kptr = (const char *)(uintptr_t)vstr(js, key, &klen);
    defined = mkprop_define_data(js, obj, kptr, (size_t)klen, value);
  }
  
  if (defined == T_EMPTY) return js_create_data_property_or_throw(js, obj, key, value);
  return is_err(defined) ? defined : Ant_Value_Undefined;
}

ant_value_t sv_spread_into(sv_vm_t *vm, ant_t *js, ant_value_t arr, ant_value_t iterable) {
  if (vtype(arr) != kTypeArray) return js_mkerr(js, "spread target is not an array");

  if (vtype(iterable) == kTypeArray && js_array_iteration_default(js, iterable)) {
    ant_value_t spread = js_arr_spread_into(js, arr, iterable);
    return is_err(spread) ? spread : Ant_Value_FromNumber(0);
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t state[3] = { Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined };
  
  GC_ROOT_PIN(js, state[0]);
  GC_ROOT_PIN(js, state[1]);
  
  ant_value_t status = sv_for_of_open(vm, js, iterable, state);
  while (!is_err(status)) {
    ant_value_t value = Ant_Value_Undefined;
    bool done = true;
    
    status = sv_iter_advance_values(vm, js, state, &value, &done);
    if (is_err(status) || done) break;
    state[1] = status;
    
    js_arr_push(js, arr, value);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return is_err(status) ? status : Ant_Value_FromNumber(0);
}

ant_value_t sv_op_define_class(
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
  ant_value_t proto = Ant_Value_Undefined;
  
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
    mkprop_interned_exact(js, ctor_obj, js->intern.length, Ant_Value_FromNumber(0), ANT_PROP_ATTR_CONFIGURABLE);
  }

  proto = mkobj(js, 0);
  if (!has_heritage) js_set_proto_init(proto, js->sym.object_proto);
  
  else if (pt == kTypeNull) {
    js_set_proto_init(proto, Ant_Value_Null);
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
  
  if (vtype(ctor) == kTypeFunction) {
    sv_closure_t *c = js_func_closure(ctor);
    if (parent_is_callable) {
      c->super_val = parent;
      c->call_flags |= SV_CALL_HAS_SUPER;
    } else if (!has_heritage && c->func && c->func->uses_super) {
      c->super_val = proto;
      c->call_flags |= SV_CALL_HAS_SUPER;
    }
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
  mkprop_exact_attrs(js, ctor_obj, Ant_String_FromLiteral(js, "prototype"), proto, 0);

  vm->stack[vm->sp - 2] = ctor;
  vm->stack[vm->sp - 1] = proto;
  GC_ROOT_RESTORE(js, root_mark);
  
  return Ant_Value_Undefined;
}

#endif
