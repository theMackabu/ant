#include <compat.h> // IWYU pragma: keep

#include <string.h>

#include "cfunc.h"
#include "builder.h"
#include "stringify.h"

#include "silver/call.h"

#include "modules/iterator.h"
#include "modules/date.h"

#include "arguments.h"
#include "arrays.h"
#include "convert.h"
#include "keys.h"
#include "modules/object.h"
#include "modules/proxy.h"
#include "props.h"

static bool is_object_constructor(ant_value_t value) {
  if (vtype(value) != kTypeFunction) return false;
  ant_value_t builtin = js_get_slot(js_func_obj(value), SLOT_BUILTIN);
  return vtype(builtin) == kTypeNumber && Ant_Value_AsNumberUnchecked(builtin) == (double)BUILTIN_OBJECT;
}

ant_value_t builtin_Object(ant_params_t) {
  bool constructing = vtype(call_new_target) != kTypeUndefined;
  if (constructing && !is_object_constructor(call_new_target) && is_object_type(call_this)) return call_this;

  if (nargs == 0 || vtype(args[0]) == kTypeNull || vtype(args[0]) == kTypeUndefined) {
    ant_value_t obj_proto = js->sym.object_proto;
    if (constructing && is_unboxed_obj(js, call_this, obj_proto)) return call_this;
    return Ant_Object_Create(js);
  }
  
  ant_value_t arg = args[0];
  uint8_t t = vtype(arg);
  
  if (t == kTypeObject || t == kTypeArray || t == kTypeFunction) return arg;
  if (is_boxable_primitive_type(t))
    return js_normalize_sloppy_this(js, arg);
  
  return arg;
}

static ant_value_t builtin_object_is(ant_params_t) {
  if (nargs < 2) return Ant_Value_False;
  return same_value_values(js, args[0], args[1]) ? Ant_Value_True : Ant_Value_False;
}

static ant_value_t object_nullish_error(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
}

static ant_value_t builtin_object_keys(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(source) == kTypeNull || vtype(source) == kTypeUndefined) return object_nullish_error(js);
  
  ant_value_t obj = js_object_view(args[0]);
  if (vtype(obj) == kTypeString) return js_for_in_keys(js, obj);

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) return Ant_Array_Create(js);
    obj = promoted;
  }

  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray && vtype(obj) != kTypeFunction) return Ant_Array_Create(js);
  if (is_proxy(obj)) return proxy_enum(js, obj, OBJ_ENUM_KEYS);

  return js_own_property_keys(js, obj, false, true);
}

static ant_value_t builtin_object_values(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(source) == kTypeNull || vtype(source) == kTypeUndefined) return object_nullish_error(js);
  ant_value_t obj = js_object_view(args[0]);
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) return Ant_Array_Create(js);
    obj = promoted;
  }
  if (vtype(obj) == kTypeString) return object_enum_collect(js, obj, OBJ_ENUM_VALUES);
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray && vtype(obj) != kTypeFunction) return Ant_Array_Create(js);
  if (is_proxy(obj)) return proxy_enum(js, obj, OBJ_ENUM_VALUES);
  return object_enum(js, obj, OBJ_ENUM_VALUES);
}

static ant_value_t builtin_object_entries(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(source) == kTypeNull || vtype(source) == kTypeUndefined) return object_nullish_error(js);
  ant_value_t obj = js_object_view(args[0]);
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) return Ant_Array_Create(js);
    obj = promoted;
  }
  if (vtype(obj) == kTypeString) return object_enum_collect(js, obj, OBJ_ENUM_ENTRIES);
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray && vtype(obj) != kTypeFunction) return Ant_Array_Create(js);
  if (is_proxy(obj)) return proxy_enum(js, obj, OBJ_ENUM_ENTRIES);
  return object_enum(js, obj, OBJ_ENUM_ENTRIES);
}

static ant_value_t builtin_object_getPrototypeOf(ant_params_t) {
  if (nargs == 0) return js_mkerr(js, "Object.getPrototypeOf requires an argument");
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t == kTypeString || t == kTypeNumber || t == kTypeBool || t == kTypeBigInt)
    return js_primitive_prototype(js, t);
  if (t == kTypeBuiltin) return js_get_proto(js, obj);
  
  if (is_object_type(obj)) {
    if (is_proxy(obj)) return proxy_get_prototype_of(js, obj);
    return get_proto(js, obj);
  }
  
  return Ant_Value_Null;
}

static ant_value_t builtin_object_setPrototypeOf(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "Object.setPrototypeOf requires 2 arguments");
  
  ant_value_t obj = args[0];
  ant_value_t proto = args[1];
  
  uint8_t t = vtype(obj);
  if (t == kTypeBuiltin) {
    obj = js_cfunc_promote(js, obj);
    t = kTypeFunction;
  }

  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    return js_mkerr(js, "Object.setPrototypeOf: first argument must be an object");
  }
  
  uint8_t pt = vtype(proto);
  if (pt == kTypeBuiltin) {
    proto = js_cfunc_promote(js, proto);
    pt = kTypeFunction;
  }

  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction && pt != kTypeNull) {
    return js_mkerr(js, "Object.setPrototypeOf: prototype must be an object or null");
  }
  
  if (pt != kTypeNull && proto_chain_contains(js, proto, obj))
    return js_mkerr(js, "Cyclic __proto__ value");

  if (is_proxy(obj)) {
    ant_value_t result = proxy_set_prototype_of(js, obj, proto);
    if (is_err(result)) return result;
    if (!js_truthy(js, result))
      return proxy_trap_error(js, "setPrototypeOf", "trap returned falsy");
    return obj;
  }
  
  set_proto(js, obj, proto);
  return obj;
}

static ant_value_t builtin_proto_getter(ant_params_t) {
  uint8_t t = vtype(call_this);
  
  if (t == kTypeUndefined || t == kTypeNull) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot read property '__proto__' of %s", typestr(t));
  }
  
  if (t == kTypeObject || t == kTypeArray || t == kTypeFunction) {
    return get_proto(js, call_this);
  }
  
  return js_primitive_prototype(js, t);
}

static ant_value_t builtin_proto_setter(ant_params_t) {
  uint8_t t = vtype(call_this);
  
  if (t == kTypeUndefined || t == kTypeNull) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property '__proto__' of %s", typestr(t));
  }
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    return Ant_Value_Undefined;
  }
  
  if (nargs == 0) return Ant_Value_Undefined;
  
  ant_value_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction && pt != kTypeNull) {
    return Ant_Value_Undefined;
  }
  
  if (pt != kTypeNull && proto_chain_contains(js, proto, call_this))
    return Ant_Value_Undefined;
  
  set_proto(js, call_this, proto);
  return Ant_Value_Undefined;
}

static ant_value_t legacy_accessor_this_obj(ant_t *js, ant_value_t this_val) {
  uint8_t t = vtype(this_val);
  
  if (t == kTypeUndefined || t == kTypeNull) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot convert undefined or null to object"
  );
  
  if (t == kTypeBuiltin) return js_cfunc_promote(js, this_val);
  if (t == kTypeObject || t == kTypeArray || t == kTypeFunction) return js_as_obj(this_val);
  
  if (t == kTypeString || t == kTypeNumber || t == kTypeBool || t == kTypeBigInt) {
    ant_value_t boxed = builtin_Object(js, &this_val, 1, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
    if (is_err(boxed)) return boxed;
    if (is_object_type(boxed)) return js_as_obj(boxed);
  }
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "Legacy accessor methods require an object receiver");
}

static bool legacy_accessor_key(
  ant_t *js, ant_value_t key,
  ant_value_t *key_val_out,
  const char **key_str_out,
  ant_offset_t *key_len_out,
  ant_offset_t *sym_off_out
) {
  if (key_val_out) *key_val_out = key;
  if (key_str_out) *key_str_out = NULL;
  if (key_len_out) *key_len_out = 0;
  if (sym_off_out) *sym_off_out = 0;

  if (vtype(key) == kTypeSymbol) {
    if (sym_off_out) *sym_off_out = (ant_offset_t)vdata(key);
    return true;
  }

  ant_value_t key_val = (vtype(key) == kTypeString) ? key : coerce_to_str(js, key);
  if (is_err(key_val)) return false;
  if (key_val_out) *key_val_out = key_val;
  
  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key_val, &key_len);
  
  if (key_str_out) *key_str_out = (const char *)(uintptr_t)key_off;
  if (key_len_out) *key_len_out = key_len;
  
  return true;
}

static ant_value_t builtin_object___defineGetter__(ant_params_t) {
  ant_value_t obj = legacy_accessor_this_obj(js, call_this);
  if (is_err(obj)) return obj;
  if (nargs < 2) return Ant_Value_Undefined;

  ant_value_t getter = args[1];
  uint8_t gt = vtype(getter);
  
  if (gt != kTypeFunction && gt != kTypeBuiltin) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object.prototype.__defineGetter__: Expecting function");
  }

  ant_value_t key_val = Ant_Value_Undefined;
  const char *key_str = NULL;
  ant_offset_t key_len = 0;
  ant_offset_t sym_off = 0;
  
  if (!legacy_accessor_key(js, args[0], &key_val, &key_str, &key_len, &sym_off)) {
    return key_val;
  }

  if (is_proxy(js_as_obj(obj))) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, obj);
    GC_ROOT_PIN(js, key_val);
    GC_ROOT_PIN(js, getter);
    
    ant_value_t desc = Ant_Object_CreateBare(js);
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    
    GC_ROOT_PIN(js, desc);
    js_setprop(js, desc, js_mkstr(js, "get", 3), getter);
    js_setprop(js, desc, js_mkstr(js, "enumerable", 10), Ant_Value_True);
    js_setprop(js, desc, js_mkstr(js, "configurable", 12), Ant_Value_True);
    
    ant_value_t result = object_define_property(js, obj, key_val, desc);
    GC_ROOT_RESTORE(js, root_mark);
    if (is_err(result)) return result;
    
    return Ant_Value_Undefined;
  }

  if (vtype(key_val) == kTypeSymbol) {
    js_set_sym_getter_desc(js, js_as_obj(obj), key_val, getter, JS_DESC_E | JS_DESC_C);
  } else js_set_getter_desc(js, js_as_obj(obj), key_str, (size_t)key_len, getter, JS_DESC_E | JS_DESC_C);
  
  return Ant_Value_Undefined;
}

static ant_value_t builtin_object___defineSetter__(ant_params_t) {
  ant_value_t obj = legacy_accessor_this_obj(js, call_this);
  if (is_err(obj)) return obj;
  if (nargs < 2) return Ant_Value_Undefined;

  ant_value_t setter = args[1];
  uint8_t st = vtype(setter);
  
  if (st != kTypeFunction && st != kTypeBuiltin) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object.prototype.__defineSetter__: Expecting function");
  }

  ant_value_t key_val = Ant_Value_Undefined;
  const char *key_str = NULL;
  ant_offset_t key_len = 0;
  ant_offset_t sym_off = 0;
  
  if (!legacy_accessor_key(js, args[0], &key_val, &key_str, &key_len, &sym_off)) {
    return key_val;
  }

  if (is_proxy(js_as_obj(obj))) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, obj);
    GC_ROOT_PIN(js, key_val);
    GC_ROOT_PIN(js, setter);
    
    ant_value_t desc = Ant_Object_CreateBare(js);
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    
    GC_ROOT_PIN(js, desc);
    js_setprop(js, desc, js_mkstr(js, "set", 3), setter);
    js_setprop(js, desc, js_mkstr(js, "enumerable", 10), Ant_Value_True);
    js_setprop(js, desc, js_mkstr(js, "configurable", 12), Ant_Value_True);
    
    ant_value_t result = object_define_property(js, obj, key_val, desc);
    GC_ROOT_RESTORE(js, root_mark);
    if (is_err(result)) return result;
    
    return Ant_Value_Undefined;
  }

  if (vtype(key_val) == kTypeSymbol) {
    js_set_sym_setter_desc(js, js_as_obj(obj), key_val, setter, JS_DESC_E | JS_DESC_C);
  } else js_set_setter_desc(js, js_as_obj(obj), key_str, (size_t)key_len, setter, JS_DESC_E | JS_DESC_C);
  
  return Ant_Value_Undefined;
}

static ant_value_t legacy_lookup_accessor_from_descriptor(ant_t *js, ant_value_t desc, bool want_getter) {
  if (vtype(desc) != kTypeObject) return Ant_Value_Undefined;
  ant_prop_loc_t off = lkp_interned(desc, want_getter ? js->intern.get : js->intern.set);
  if (!off.obj) return Ant_Value_Undefined;
  return js_prop_load(off);
}

ant_value_t proxy_proto_hop(ant_t *js, ant_value_t proxy, uint32_t *hops) {
  if (++*hops > PROXY_PROTO_HOP_LIMIT)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Maximum call stack size exceeded");
  return proxy_get_prototype_of(js, proxy);
}

static ant_value_t legacy_lookup_accessor(ant_t *js, ant_value_t this_val, ant_value_t key, bool want_getter) {
  ant_value_t obj = legacy_accessor_this_obj(js, this_val);
  if (is_err(obj)) return obj;

  ant_value_t key_val = Ant_Value_Undefined;
  const char *key_str = NULL;
  ant_offset_t key_len = 0;
  ant_offset_t sym_off = 0;
  
  if (!legacy_accessor_key(js, key, &key_val, &key_str, &key_len, &sym_off)) {
    return key_val;
  }

  uint32_t proxy_hops = 0;
  for (ant_value_t cur = obj; is_object_type(cur); ) {
    if (is_proxy(cur)) {
      ant_value_t desc = proxy_get_own_property_descriptor(js, cur, key_val);
      if (is_err(desc)) return desc;
      if (vtype(desc) == kTypeObject) return legacy_lookup_accessor_from_descriptor(js, desc, want_getter);
      cur = proxy_proto_hop(js, cur, &proxy_hops);
      if (is_err(cur)) return cur;
      continue;
    }

    prop_meta_t meta;
    bool has_meta = (vtype(key_val) == kTypeSymbol)
      ? lookup_symbol_prop_meta(js, cur, sym_off, &meta)
      : lookup_string_prop_meta(js, cur, key_str, (size_t)key_len, &meta);
    if (has_meta) {
      if (want_getter) return meta.has_getter ? meta.getter : Ant_Value_Undefined;
      return meta.has_setter ? meta.setter : Ant_Value_Undefined;
    }
    
    if (vtype(key_val) == kTypeSymbol) {
      if (lkp_sym(cur, sym_off).obj) return Ant_Value_Undefined;
    } else {
      if (real_array_obj_ptr(cur) && is_length_key(key_str, key_len)) return Ant_Value_Undefined;
      if (array_obj_ptr(cur) && is_array_index(key_str, key_len)) {
        unsigned long idx = 0;
        if (
          parse_array_index(key_str, key_len, get_array_length(js, cur), &idx) &&
          arr_has(js, cur, (ant_offset_t)idx)
        ) return Ant_Value_Undefined;
      }
      if (lkp(js, cur, key_str, key_len).obj) return Ant_Value_Undefined;
    }
    cur = get_proto(js, cur);
  }

  return Ant_Value_Undefined;
}

static ant_value_t builtin_object___lookupGetter__(ant_params_t) {
  if (nargs < 1) return Ant_Value_Undefined;
  return legacy_lookup_accessor(js, call_this, args[0], true);
}

static ant_value_t builtin_object___lookupSetter__(ant_params_t) {
  if (nargs < 1) return Ant_Value_Undefined;
  return legacy_lookup_accessor(js, call_this, args[0], false);
}

static ant_value_t builtin_object_create(ant_params_t) {
  if (nargs == 0) return js_mkerr(js, "Object.create requires a prototype argument");
  
  ant_value_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction && pt != kTypeNull)
    return js_mkerr(js, "Object.create: prototype must be an object or null");

  ant_value_t obj = js_mkobj_with_proto(js, proto);
  if (is_err(obj)) return obj;

  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    ant_value_t result = object_define_properties(js, obj, args[1]);
    if (is_err(result)) return result;
  }

  return obj;
}

static ant_value_t builtin_object_hasOwn(ant_params_t) {
  ant_value_t target = nargs >= 1 ? js_object_view(args[0]) : Ant_Value_Undefined;
  ant_value_t key = nargs >= 2 ? args[1] : Ant_Value_Undefined;
  return Ant_Object_HasOwn(js, target, key);
}

typedef struct {
  sv_callback_t cb;
  js_group_add_fn add;
  void *ctx;
  ant_offset_t index;
} group_by_ctx_t;

static iter_action_t group_by_step(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  group_by_ctx_t *group_by = ctx;
  ant_value_t cb_args[2] = { value, Ant_Value_FromNumber((double)group_by->index++) };
  ant_value_t key = sv_callback_call(js->vm, js, &group_by->cb, cb_args, 2);
  if (is_err(key)) { *out = key; return ITER_ERROR; }
  
  ant_value_t added = group_by->add(js, group_by->ctx, key, value);
  if (is_err(added)) { *out = added; return ITER_ERROR; }
  
  return ITER_CONTINUE;
}

ant_value_t js_group_by(ant_t *js, ant_value_t items, ant_value_t callback, js_group_add_fn add, void *ctx) {
  if (!is_callable(callback)) return js_mkerr_typed(js, JS_ERR_TYPE, "callback is not a function");
  group_by_ctx_t group_by = { sv_callback_prepare(js, callback, Ant_Value_Undefined), add, ctx, 0 };

  if (array_obj_ptr(items) && js_array_iteration_default(js, items)) {
    ant_offset_t len = 0;
    ant_value_t len_result = array_like_length_checked(js, items, &len);
    if (is_err(len_result)) return len_result;
    
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t value = array_method_get_index(js, items, i);
      if (is_err(value)) return value;
      ant_value_t err = Ant_Value_Undefined;
      if (group_by_step(js, value, &group_by, &err) == ITER_ERROR) return err;
      len_result = array_like_length_checked(js, items, &len);
      if (is_err(len_result)) return len_result;
    }
    
    return Ant_Value_Undefined;
  }

  if (vtype(js->sym.iterator_sym) != kTypeSymbol || !is_callable(js_get_sym(js, items, js->sym.iterator_sym)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");
  
  ant_value_t iterated = iter_foreach(js, items, group_by_step, &group_by);
  return is_err(iterated) ? iterated : Ant_Value_Undefined;
}

static ant_value_t object_group_add(ant_t *js, void *ctx, ant_value_t key, ant_value_t value) {
  ant_value_t result = *(ant_value_t *)ctx;
  
  if (vtype(key) == kTypeSymbol) {
    ant_value_t group = js_get_sym(js, result, key);
    if (vtype(group) != kTypeArray) {
      group = Ant_Array_Create(js);
      js_set_sym(js, result, key, group);
    }
    
    js_arr_push(js, group, value);
    return Ant_Value_Undefined;
  }
  
  ant_value_t key_str = js_tostring_val(js, key);
  if (is_err(key_str)) return key_str;

  ant_offset_t klen;
  ant_offset_t koff = vstr(js, key_str, &klen);
  ant_prop_loc_t grp_off = lkp(js, result, (const char *)(uintptr_t)koff, klen);
  ant_value_t group;
  
  if (grp_off.obj) group = js_prop_load(grp_off);
  else {
    group = Ant_Array_Create(js);
    js_setprop(js, result, key_str, group);
  }
  
  js_arr_push(js, group, value);
  return Ant_Value_Undefined;
}

static ant_value_t builtin_object_groupBy(ant_params_t) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Object.groupBy requires 2 arguments");
  
  ant_value_t result = Ant_Object_CreateBare(js);
  js_set_proto_init(result, Ant_Value_Null);
  
  ant_value_t grouped = js_group_by(js, args[0], args[1], object_group_add, &result);
  return is_err(grouped) ? grouped : result;
}

static ant_value_t builtin_object_defineProperty(ant_params_t) {
  if (nargs < 3) return js_mkerr(js, "Object.defineProperty requires 3 arguments");
  return object_define_property(js, args[0], args[1], args[2]);
}

static ant_value_t builtin_object_defineProperties(ant_params_t) {
  ant_value_t obj = nargs >= 1 ? args[0] : Ant_Value_Undefined;
  uint8_t t = vtype(obj);
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction)
    return js_mkerr(js, "Object.defineProperties called on non-object");
  return object_define_properties(js, obj, nargs >= 2 ? args[1] : Ant_Value_Undefined);
}

static ant_value_t builtin_object_assign(ant_params_t) {
  if (nargs == 0) return js_mkerr(js, "Object.assign requires at least 1 argument");
  
  ant_value_t target = args[0];
  uint8_t t = vtype(target);
  
  if (t == kTypeNull || t == kTypeUndefined) {
    return js_mkerr(js, "Cannot convert undefined or null to object");
  }
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    target = Ant_Object_CreateBare(js);
  }
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, target);
  
  copy_ctx_t cx = { .assign = true };
  ant_value_t as_obj = js_as_obj(target);
  ant_value_t result = Ant_Value_Undefined;
  
  for (int i = 1; i < nargs && !is_err(result); i++)
    result = copy_data_props(js, as_obj, args[i], &cx);
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : target;
}

static ant_value_t object_integrity_target(ant_t *js, ant_value_t obj) {
  return vtype(obj) == kTypeBuiltin ? js_cfunc_promote(js, obj) : obj;
}

static __attribute__((noinline)) ant_value_t object_integrity_view(ant_t *js, ant_value_t obj) {
  return js_cfunc_lookup_promoted(js, obj);
}

static __attribute__((noinline)) ant_value_t object_scan_integrity(ant_t *js, ant_value_t obj, bool frozen) {
  ant_value_t keys = js_own_property_keys(js, obj, true, false);
  if (is_err(keys)) return keys;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, keys);

  ant_value_t result = Ant_Value_True;
  ant_offset_t count = js_arr_len(js, keys);
  
  for (ant_offset_t i = 0; i < count && result == Ant_Value_True; i++) {
    ant_value_t desc = js_get_own_property_descriptor(js, obj, js_arr_get(js, keys, i));
    if (is_err(desc)) { result = desc; break; }
    if (!is_object_type(desc)) continue;

    GC_ROOT_PIN(js, desc);
    if (js_truthy(js, js_get(js, desc, "configurable"))) result = Ant_Value_False;
    else if (frozen && js_truthy(js, js_get(js, desc, "writable"))) result = Ant_Value_False;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static inline ant_value_t object_test_integrity(ant_t *js, ant_value_t obj, ant_object_t *ptr, bool frozen) {
  if (!ptr) return Ant_Value_True;
  
  uint16_t flags = ptr->flags.raw;
  uint16_t settled = frozen ? ANT_OBJECT_FLAG_FROZEN : ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_SEALED;
  
  if (flags & settled) return Ant_Value_True;
  if ((flags & (ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_EXTENSIBLE)) == ANT_OBJECT_FLAG_EXTENSIBLE) return Ant_Value_False;
  
  return object_scan_integrity(js, obj, frozen);
}

ant_value_t builtin_object_freeze(ant_params_t) {
  if (nargs == 0) return Ant_Value_Undefined;
  
  ant_value_t obj = args[0];
  ant_value_t target = object_integrity_target(js, obj);
  
  if (is_err(target)) return target;
  uint8_t t = vtype(target);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return obj;
  ant_value_t as_obj = js_as_obj(target);
  if (is_proxy(as_obj)) return proxy_set_integrity_level(js, as_obj, true);

  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr || !ptr->shape) return obj;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  if (js_is_arguments(target)) js_arguments_unmap_all(js, target);

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop) continue;
    
    uint8_t attrs = ant_shape_get_attrs(ptr->shape, i);
    attrs &= (uint8_t)~ANT_PROP_ATTR_WRITABLE;
    attrs &= (uint8_t)~ANT_PROP_ATTR_CONFIGURABLE;
    
    if (prop->type == ANT_SHAPE_KEY_STRING) {
      const char *key = prop->key.interned;
      ant_shape_set_attrs_interned(js, ptr->shape, key, attrs);
    } else ant_shape_set_attrs_symbol(js, ptr->shape, prop->key.sym_off, attrs);
  }
  
  ptr->flags.frozen = 1;
  ptr->flags.sealed = 1;
  ptr->flags.extensible = 0;
  
  return obj;
}

static ant_value_t builtin_object_isFrozen(ant_params_t) {
  if (nargs == 0) return Ant_Value_True;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    if (t != kTypeBuiltin) return Ant_Value_True;
    obj = object_integrity_view(js, obj);
    if (vtype(obj) == kTypeBuiltin) return Ant_Value_False;
  }
  
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_test_integrity_level(js, as_obj, true);
  
  return object_test_integrity(js, obj, js_obj_ptr(as_obj), true);
}

static ant_value_t builtin_object_seal(ant_params_t) {
  if (nargs == 0) return Ant_Value_Undefined;
  
  ant_value_t obj = args[0];
  ant_value_t target = object_integrity_target(js, obj);
  if (is_err(target)) return target;
  uint8_t t = vtype(target);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return obj;
  ant_value_t as_obj = js_as_obj(target);
  if (is_proxy(as_obj)) return proxy_set_integrity_level(js, as_obj, false);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr || !ptr->shape) return obj;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  
  ptr->flags.sealed = 1;
  ptr->flags.extensible = 0;

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop) continue;

    uint8_t attrs = ant_shape_get_attrs(ptr->shape, i);
    attrs &= (uint8_t)~ANT_PROP_ATTR_CONFIGURABLE;

    if (prop->type == ANT_SHAPE_KEY_STRING) {
      const char *key = prop->key.interned;
      ant_shape_set_attrs_interned(js, ptr->shape, key, attrs);
    } else {
      ant_shape_set_attrs_symbol(js, ptr->shape, prop->key.sym_off, attrs);
    }
  }
  
  return obj;
}

static ant_value_t builtin_object_isSealed(ant_params_t) {
  if (nargs == 0) return Ant_Value_True;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    if (t != kTypeBuiltin) return Ant_Value_True;
    obj = object_integrity_view(js, obj);
    if (vtype(obj) == kTypeBuiltin) return Ant_Value_False;
  }
  
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_test_integrity_level(js, as_obj, false);
  
  return object_test_integrity(js, obj, js_obj_ptr(as_obj), false);
}

static iter_action_t object_from_entries_iter_cb(ant_t *js, ant_value_t entry, void *ctx, ant_value_t *out) {
  ant_value_t *result = ctx;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, *result);
  GC_ROOT_PIN(js, entry);

  if (vtype(entry) != kTypeArray && vtype(entry) != kTypeObject) {
    *out = js_mkerr(js, "Object.fromEntries iterable values must be entry objects");
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t key = arr_get(js, entry, 0);
  GC_ROOT_PIN(js, key);
  ant_value_t val = arr_get(js, entry, 1);
  GC_ROOT_PIN(js, val);

  char key_stack[128];
  property_key_view_t key_view;
  ant_value_t error = property_key_view_init(
    js, key, &key_view, 
    key_stack, sizeof(key_stack)
  );
  
  if (is_err(error)) {
    *out = error;
    property_key_view_dispose(&key_view);
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  if (property_key_view_requires_pin(&key_view, false))
    GC_ROOT_PIN(js, key_view.js_key);

  ant_value_t added;
  if (key_view.is_symbol) added = mkprop_symbol_attrs_impl(
    js, *result, key_view.js_key, val, 0, MKPROP_USE_DEFAULT_ATTRS | MKPROP_KEYED_STORE
  );
  else {
    const char *interned = intern_string(js, key_view.bytes, key_view.length);
    added = interned
      ? mkprop_interned_keyed(js, *result, interned, val)
      : js_mkerr(js, "oom");
  }
    
  property_key_view_dispose(&key_view);
  if (is_err(added)) {
    *out = added;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  *out = *result;
  GC_ROOT_RESTORE(js, root_mark);

  return ITER_CONTINUE;
}

static ant_value_t builtin_object_fromEntries(ant_params_t) {
  if (nargs == 0) return js_mkerr(js, "Object.fromEntries requires an iterable argument");

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t iterable = args[0];
  GC_ROOT_PIN(js, iterable);

  ant_value_t result = Ant_Object_Create(js);
  GC_ROOT_PIN(js, result);

  ant_value_t iter_result = iter_foreach(js, iterable, object_from_entries_iter_cb, &result);
  if (is_err(iter_result)) {
    if (vtype(iterable) == kTypeArray || vtype(iterable) == kTypeObject || vtype(iterable) == kTypeFunction) {
      GC_ROOT_RESTORE(js, root_mark);
      return iter_result;
    }
    ant_value_t err = js_mkerr(js, "Object.fromEntries requires an iterable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t object_get_own_property_descriptor_keyed(
  ant_t *js, ant_value_t obj, property_key_view_t *key
) {
  uint8_t t = vtype(obj);

  if (t == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) { obj = promoted; t = kTypeFunction; }
  }
  
  if (t == kTypeBuiltin) {
    if (key->is_symbol) return Ant_Value_Undefined;
    
    ant_value_t value = Ant_Value_Undefined;
    if (!js_cfunc_try_get_own(js, obj, key->bytes, key->length, &value))
      return Ant_Value_Undefined;
    
    return js_from_data_descriptor(js, value, false, false, true);
  }

  const char *key_str;
  ant_offset_t key_len;
  
  bool is_sym = key->is_symbol;
  if (is_sym) {
    const char *d = js_sym_desc(key->js_key);
    key_str = d ? d : "symbol";
    key_len = (ant_offset_t)strlen(key_str);
  } else {
    key_str = key->bytes;
    key_len = (ant_offset_t)key->length;
  }

  ant_value_t string_exotic_value = Ant_Value_Undefined;
  bool string_exotic_length = false;
  bool has_string_exotic = false;

  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    has_string_exotic = !is_sym && js_try_get_string_own_exotic(
      js, obj, key_str, (size_t)key_len, &string_exotic_value, &string_exotic_length
    );
    if (!has_string_exotic) return Ant_Value_Undefined;

    return js_from_data_descriptor(js, string_exotic_value, false, !string_exotic_length, false);
  }
  
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) {
    ant_value_t materialized = property_key_view_materialize(js, key);
    if (is_err(materialized)) return materialized;
    return proxy_get_own_property_descriptor(js, as_obj, materialized);
  }

  bool is_arr_obj = array_obj_ptr(as_obj) != NULL;
  bool is_arr_length = !is_sym && real_array_obj_ptr(as_obj) && is_length_key(key_str, key_len);
  bool has_arr_index = false;
  ant_value_t arr_index_val = Ant_Value_Undefined;
  
  if (!is_sym && is_arr_obj)
    has_arr_index = arr_own_dense_element(js, as_obj, key_str, (size_t)key_len, &arr_index_val);

  ant_offset_t sym_off = is_sym ? (ant_offset_t)vdata(key->js_key) : 0;
  prop_meta_t sym_meta; prop_meta_t str_meta;
  
  bool has_sym_meta = is_sym ? lookup_symbol_prop_meta(js, as_obj, sym_off, &sym_meta) : false;
  bool has_str_meta = is_sym ? false : lookup_string_prop_meta(js, as_obj, key_str, (size_t)key_len, &str_meta);

  ant_prop_loc_t prop_off = is_sym ? lkp_sym(as_obj, sym_off) : lkp(js, as_obj, key_str, key_len);
  if (!prop_off.obj && !(is_sym ? has_sym_meta : has_str_meta) && !is_arr_length && !has_arr_index) {
    has_string_exotic = !is_sym && t == kTypeObject && js_try_get_string_own_exotic(
      js, obj, key_str, (size_t)key_len, 
      &string_exotic_value, &string_exotic_length
    );
    
    if (!has_string_exotic) {
      const ant_object_t *exotic_ptr = js_obj_ptr(as_obj);
      ant_value_t element = !is_sym && exotic_ptr
        ? exotic_own_get(js, as_obj, exotic_ptr, key_str, (size_t)key_len) : Ant_Value_Undefined;
      if (vtype(element) == kTypeUndefined || element == T_EMPTY) return Ant_Value_Undefined;
      
      return js_from_data_descriptor(js, element, true, true, true);
    }
  }

  bool has_getter = false;
  bool has_setter = false;
  
  ant_value_t getter = Ant_Value_Undefined;
  ant_value_t setter = Ant_Value_Undefined;
  
  bool writable = true;
  bool enumerable = true;
  bool configurable = true;

  if (is_sym && has_sym_meta) {
    has_getter = sym_meta.has_getter;
    has_setter = sym_meta.has_setter;
    getter = sym_meta.getter;
    setter = sym_meta.setter;
    writable = sym_meta.writable;
    enumerable = sym_meta.enumerable;
    configurable = sym_meta.configurable;
  } else if (!is_sym && has_str_meta) {
    has_getter = str_meta.has_getter;
    has_setter = str_meta.has_setter;
    getter = str_meta.getter;
    setter = str_meta.setter;
    writable = str_meta.writable;
    enumerable = str_meta.enumerable;
    configurable = str_meta.configurable;
  }

  if (has_getter || has_setter) return js_from_accessor_descriptor(
    js, has_getter ? getter : Ant_Value_Undefined, has_setter ? setter : Ant_Value_Undefined, enumerable, configurable
  );

  ant_value_t prop_val = Ant_Value_Undefined;
  if (prop_off.obj) prop_val = js_prop_load(prop_off);
  else if (has_string_exotic) {
    prop_val = string_exotic_value;
    writable = false;
    enumerable = !string_exotic_length;
    configurable = false;
  } else if (has_arr_index) {
    prop_val = arr_index_val;
    ant_object_t *arr_ptr = js_obj_ptr(as_obj);
    if (arr_ptr && (arr_ptr->flags.frozen || arr_ptr->flags.sealed)) configurable = false;
    if (arr_ptr && arr_ptr->flags.frozen) writable = false;
  } else if (is_arr_length) {
    prop_val = Ant_Value_FromNumber((double)get_array_length(js, as_obj));
    writable = !array_length_readonly(js, as_obj);
    enumerable = false;
    configurable = false;
  }
  
  return js_from_data_descriptor(js, prop_val, writable, enumerable, configurable);
}

ant_value_t builtin_object_getOwnPropertyDescriptor(ant_params_t) {
  if (nargs < 1) return Ant_Value_Undefined;

  ant_value_t obj = js_object_view(args[0]);
  ant_value_t raw_key = nargs >= 2 ? args[1] : Ant_Value_Undefined;
  char key_stack[128];
  
  property_key_view_t key;
  ant_value_t error = property_key_view_init(
    js, raw_key, &key, 
    key_stack, sizeof(key_stack)
  );
  
  if (is_err(error)) {
    property_key_view_dispose(&key);
    return error;
  }

  bool may_materialize = is_object_type(obj) && is_proxy(js_as_obj(obj));
  bool root_key = property_key_view_requires_pin(&key, may_materialize);
  
  size_t root_mark = root_key ? gc_root_scope(js) : 0;
  if (root_key) GC_ROOT_PIN(js, key.js_key);

  ant_value_t result = object_get_own_property_descriptor_keyed(js, obj, &key);
  property_key_view_dispose(&key);
  if (root_key) GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

static inline bool own_prop_names_is_dense_shadow(
  ant_t *js, ant_value_t obj,
  const char *key, ant_offset_t key_len
) {
  ant_offset_t doff = get_dense_buf(obj);
  if (!doff) return false;
  
  ant_offset_t dense_len = dense_iterable_length(js, obj);
  if (dense_len <= 0 || !is_array_index(key, key_len)) return false;
  
  unsigned long dense_idx = 0;
  return parse_array_index(key, (size_t)key_len, dense_len, &dense_idx);
}

static ant_value_t builtin_object_getOwnPropertyNames(ant_params_t) {
  if (nargs == 0) return Ant_Array_Create(js);
  ant_value_t obj = js_object_view(args[0]);

  if (vtype(obj) == kTypeString) {
    ant_value_t arr = Ant_Array_Create(js);
    ant_offset_t idx = 0;
    ant_offset_t slen = str_utf16_len(js, obj);
    
    for (ant_offset_t i = 0; i < slen; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
      arr_set(js, arr, idx++, js_mkstr(js, idxstr, idxlen));
    }
    
    arr_set(js, arr, idx++, js->length_str);
    return mkval(kTypeArray, vdata(arr));
  }

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) obj = promoted;
  }

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t arr = Ant_Array_Create(js);
    ant_offset_t idx = 0;
    arr_set(js, arr, idx++, js->length_str);
    
    if (vtype(js_cfunc_name_value(js, obj)) == kTypeString) arr_set(js, arr, idx++, Ant_String_FromLiteral(js, "name"));
    if (js_cfunc_has_prototype(obj)) arr_set(js, arr, idx++, Ant_String_FromLiteral(js, "prototype"));
    
    return mkval(kTypeArray, vdata(arr));
  }
  
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray && vtype(obj) != kTypeFunction) return Ant_Array_Create(js);
  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return Ant_Array_Create(js);
  if (vtype(obj) != kTypeArray) return js_own_property_keys(js, obj, false, false);
  bool is_arr_obj = (vtype(obj) == kTypeArray);
  
  ant_value_t arr = Ant_Array_Create(js);
  ant_offset_t idx = 0;
  
  if (is_arr_obj) {
  for (ant_offset_t i = 0;; i++) {
    ant_offset_t doff = get_dense_buf(obj);
    if (!doff) break;
    
    ant_offset_t dense_len = dense_iterable_length(js, obj);
    if (i >= dense_len) break;

    ant_value_t v = dense_get(doff, i);
    if (is_empty_slot(v)) continue;
    
    char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    arr_set(js, arr, idx++, js_mkstr(js, idxstr, idxlen));
  }}
  
  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
    const char *key = prop->key.interned;
    ant_offset_t klen = (ant_offset_t)intern_length(key);

    if (is_arr_obj && own_prop_names_is_dense_shadow(js, obj, key, klen)) continue;
    arr_set(js, arr, idx++, js_mkstr(js, key, (size_t)klen));
  }
  
  if (is_arr_obj && !ptr->flags.arguments_object) arr_set(js, arr, idx++, js->length_str);
  return mkval(kTypeArray, vdata(arr));
}

static ant_value_t proxy_own_symbols(ant_t *js, ant_value_t proxy) {
  ant_value_t keys = proxy_own_keys_filtered(js, proxy, true, false);
  if (is_err(keys)) return keys;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, keys);
  ant_value_t arr = Ant_Array_Create(js);
  GC_ROOT_PIN(js, arr);
  
  ant_offset_t key_count = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key = arr_get(js, keys, i);
    if (vtype(key) == kTypeSymbol) js_arr_push(js, arr, key);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return arr;
}

static ant_value_t builtin_object_getOwnPropertySymbols(ant_params_t) {
  if (nargs == 0) return Ant_Array_Create(js);
  ant_value_t obj = args[0];

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) obj = promoted;
  }
  
  uint8_t t = vtype(obj);
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return Ant_Array_Create(js);
  if (t == kTypeFunction) obj = js_func_obj(obj);
  if (is_proxy(obj)) return proxy_own_symbols(js, obj);

  ant_object_t *ptr = js_obj_ptr(obj);
  ant_value_t arr = Ant_Array_Create(js);
  ant_offset_t idx = 0;
  if (!ptr || !ptr->shape) return mkval(kTypeArray, vdata(arr));

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
    arr_set(js, arr, idx++, mkval(kTypeSymbol, prop->key.sym_off));
  }

  return mkval(kTypeArray, vdata(arr));
}

static ant_value_t object_add_descriptors_for_keys(
  ant_t *js,
  ant_value_t result,
  ant_value_t obj,
  ant_value_t keys
) {
  ant_offset_t len = get_array_length(js, keys);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t key = arr_get(js, keys, i);
    if (vtype(key) == kTypeUndefined) continue;

    ant_value_t desc_args[2] = { obj, key };
    ant_value_t desc = builtin_object_getOwnPropertyDescriptor(js, desc_args, 2, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
    if (is_err(desc)) return desc;
    if (vtype(desc) == kTypeUndefined) continue;

    ant_value_t set_result = js_setprop(js, result, key, desc);
    if (is_err(set_result)) return set_result;
  }

  return Ant_Value_Undefined;
}

static ant_value_t builtin_object_getOwnPropertyDescriptors(ant_params_t) {
  ant_value_t result = Ant_Object_CreateBare(js);
  
  if (is_object_type(js->sym.object_proto)) js_set_proto_init(result, js->sym.object_proto);
  if (nargs == 0 || vtype(args[0]) == kTypeNull || vtype(args[0]) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
  }

  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    obj = builtin_Object(js, &obj, 1, Ant_Value_Undefined, call_this, call_func);
    if (is_err(obj)) return obj;
    t = vtype(obj);
  }
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return result;
  ant_value_t names = builtin_object_getOwnPropertyNames(js, &obj, 1, Ant_Value_Undefined, call_this, call_func);
  if (is_err(names)) return names;
  
  ant_value_t err = object_add_descriptors_for_keys(js, result, obj, names);
  if (is_err(err)) return err;
  
  ant_value_t symbols = builtin_object_getOwnPropertySymbols(js, &obj, 1, Ant_Value_Undefined, call_this, call_func);
  if (is_err(symbols)) return symbols;
  
  err = object_add_descriptors_for_keys(js, result, obj, symbols);
  if (is_err(err)) return err;

  return result;
}

static ant_value_t builtin_object_isExtensible(ant_params_t) {
  if (nargs == 0) return Ant_Value_False;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    if (t != kTypeBuiltin) return Ant_Value_False;
    obj = object_integrity_view(js, obj);
    if (vtype(obj) == kTypeBuiltin) return Ant_Value_True;
  }
  
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_is_extensible(js, as_obj);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr) return Ant_Value_True;
  if (ptr->flags.frozen || ptr->flags.sealed) return Ant_Value_False;
  
  return Ant_Value_FromBool(ptr->flags.extensible);
}

static ant_value_t builtin_object_preventExtensions(ant_params_t) {
  if (nargs == 0) return Ant_Value_Undefined;
  
  ant_value_t obj = args[0];
  ant_value_t target = object_integrity_target(js, obj);
  
  if (is_err(target)) return target;
  uint8_t t = vtype(target);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return obj;
  ant_value_t as_obj = js_as_obj(target);
  if (is_proxy(as_obj)) {
    ant_value_t result = proxy_prevent_extensions(js, as_obj);
    if (is_err(result)) return result;
    
    if (!js_truthy(js, result)) return proxy_not_prevented_error(
      js, as_obj, "Cannot prevent extensions", 
      "'preventExtensions' on proxy: trap returned falsy"
    );
    
    return obj;
  }
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr) ptr->flags.extensible = 0;
  return obj;
}

static ant_value_t builtin_object_hasOwnProperty(ant_params_t) {
  ant_value_t obj = js_object_view(call_this);
  ant_value_t key = nargs >= 1 ? args[0] : Ant_Value_Undefined;
  return Ant_Object_HasOwn(js, obj, key);
}

ant_value_t builtin_object_isPrototypeOf(ant_params_t) {
  return js_object_is_prototype_of(js, call_this, nargs > 0 ? args[0] : Ant_Value_Undefined, NULL);
}

static ant_value_t builtin_object_propertyIsEnumerable(ant_params_t) {
  return Ant_Object_PropertyIsEnumerable(js, call_this, nargs >= 1 ? args[0] : Ant_Value_Undefined);
}

typedef enum {
  OBJECT_TAG_OBJECT,
  OBJECT_TAG_UNDEFINED,
  OBJECT_TAG_NULL,
  OBJECT_TAG_ARRAY,
  OBJECT_TAG_ARGUMENTS,
  OBJECT_TAG_FUNCTION,
  OBJECT_TAG_ERROR,
  OBJECT_TAG_BOOLEAN,
  OBJECT_TAG_NUMBER,
  OBJECT_TAG_STRING,
  OBJECT_TAG_DATE,
  OBJECT_TAG_BIGINT,
  OBJECT_TAG_PROMISE,
  OBJECT_TAG_REGEXP,
  OBJECT_TAG_COUNT,
} object_tag_t;

static_assert(
  OBJECT_TAG_COUNT == sizeof(((ant_t *)0)->mutable_roots.object_tag_strings) / sizeof(ant_value_t), 
  "resize object_tag_strings in isolate_values.h"
);
#define OBJECT_TAG_TEXT(name) { "[object " name "]", sizeof("[object " name "]") - 1 }


static const struct { const char *text; uint8_t len; } object_tag_texts[OBJECT_TAG_COUNT] = {
  [OBJECT_TAG_OBJECT]    = OBJECT_TAG_TEXT("Object"),
  [OBJECT_TAG_UNDEFINED] = OBJECT_TAG_TEXT("Undefined"),
  [OBJECT_TAG_NULL]      = OBJECT_TAG_TEXT("Null"),
  [OBJECT_TAG_ARRAY]     = OBJECT_TAG_TEXT("Array"),
  [OBJECT_TAG_ARGUMENTS] = OBJECT_TAG_TEXT("Arguments"),
  [OBJECT_TAG_FUNCTION]  = OBJECT_TAG_TEXT("Function"),
  [OBJECT_TAG_ERROR]     = OBJECT_TAG_TEXT("Error"),
  [OBJECT_TAG_BOOLEAN]   = OBJECT_TAG_TEXT("Boolean"),
  [OBJECT_TAG_NUMBER]    = OBJECT_TAG_TEXT("Number"),
  [OBJECT_TAG_STRING]    = OBJECT_TAG_TEXT("String"),
  [OBJECT_TAG_DATE]      = OBJECT_TAG_TEXT("Date"),
  [OBJECT_TAG_BIGINT]    = OBJECT_TAG_TEXT("BigInt"),
  [OBJECT_TAG_PROMISE]   = OBJECT_TAG_TEXT("Promise"),
  [OBJECT_TAG_REGEXP]    = OBJECT_TAG_TEXT("RegExp"),
};

static const uint8_t object_type_tags[kTypeSourceCode + 1] = {
  [kTypeUndefined] = OBJECT_TAG_UNDEFINED,
  [kTypeNull]      = OBJECT_TAG_NULL,
  [kTypeBool]      = OBJECT_TAG_BOOLEAN,
  [kTypeNumber]    = OBJECT_TAG_NUMBER,
  [kTypeString]    = OBJECT_TAG_STRING,
  [kTypeArray]     = OBJECT_TAG_ARRAY,
  [kTypeFunction]  = OBJECT_TAG_FUNCTION,
  [kTypeBuiltin]   = OBJECT_TAG_FUNCTION,
  [kTypeError]     = OBJECT_TAG_ERROR,
  [kTypeBigInt]    = OBJECT_TAG_BIGINT,
  [kTypePromise]   = OBJECT_TAG_PROMISE,
};

static object_tag_t object_object_tag(ant_t *js, ant_value_t obj, ant_object_t *ptr, ant_value_t *err) {
  if (ptr->flags.is_exotic && is_proxy(obj)) {
    bool is_array = false;
    *err = js_is_array_value_checked(js, obj, &is_array);
    if (is_array) return OBJECT_TAG_ARRAY;
  }
  
  if (is_callable(obj)) return OBJECT_TAG_FUNCTION;
  if (ptr->flags.regexp_brand) return OBJECT_TAG_REGEXP;
  if (ant_object_extra_count(ptr) == 0) return OBJECT_TAG_OBJECT;
  if (js_get_slot(obj, SLOT_ERROR_BRAND) == Ant_Value_True) return OBJECT_TAG_ERROR;
  
  switch (vtype(js_get_slot(obj, SLOT_PRIMITIVE))) {
    case kTypeBool:   return OBJECT_TAG_BOOLEAN;
    case kTypeNumber: return OBJECT_TAG_NUMBER;
    case kTypeString: return OBJECT_TAG_STRING;
    default: break;
  }
  
  return is_date_instance(obj) ? OBJECT_TAG_DATE : OBJECT_TAG_OBJECT;
}

static ant_value_t object_tag_string(ant_t *js, object_tag_t tag) {
  ant_value_t *cached = &js->mutable_roots.object_tag_strings[tag];
  if (!*cached) *cached = js_mkstr(js, object_tag_texts[tag].text, object_tag_texts[tag].len);
  return *cached;
}

static ant_value_t object_tag_string_for(ant_t *js, ant_value_t tag_val) {
  size_t way = (size_t)(vdata(tag_val) >> 4) & 7;
  ant_value_t *key = &js->mutable_roots.object_tag_cache_keys[way];
  ant_value_t *value = &js->mutable_roots.object_tag_cache_values[way];
  if (*key == tag_val) return *value;
  
  ant_offset_t tag_len = 0;
  const char *tag = (const char *)(uintptr_t)vstr(js, tag_val, &tag_len);
  
  char static_buf[64];
  string_builder_t sb;
  string_builder_init(&sb, static_buf, sizeof(static_buf));
  string_builder_append(&sb, "[object ", 8);
  string_builder_append(&sb, tag, tag_len);
  string_builder_append(&sb, "]", 1);
  
  ant_value_t result = string_builder_finalize(js, &sb);
  if (!is_err(result)) { *key = tag_val; *value = result; }
  return result;
}

ant_value_t Ant_Object_ToStringTag(ant_t *js, ant_value_t obj) {
  uint8_t t = vtype(obj);
  
  if (t != kTypeUndefined && t != kTypeNull) {
    ant_value_t tag_val = js_get_sym(js, obj, js->sym.toStringTag_sym);
    if (is_err(tag_val)) return tag_val;
    if (vtype(tag_val) == kTypeString) return object_tag_string_for(js, tag_val);
  }
  
  ant_value_t err = Ant_Value_Undefined;
  object_tag_t tag = js_is_arguments(obj) ? OBJECT_TAG_ARGUMENTS
    : t == kTypeObject ? object_object_tag(js, obj, js_obj_ptr(obj), &err)
    : t <= kTypeSourceCode ? (object_tag_t)object_type_tags[t] : OBJECT_TAG_OBJECT;
  
  return is_err(err) ? err : object_tag_string(js, tag);
}

ant_value_t builtin_object_toString(ant_params_t) {
  return Ant_Object_ToStringTag(js, call_this);
}

static ant_value_t builtin_object_valueOf(ant_params_t) {
  if (vtype(call_this) == kTypeUndefined || vtype(call_this) == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
  if (is_boxable_primitive_type(vtype(call_this))) return js_normalize_sloppy_this(js, call_this);
  return call_this;
}

ant_value_t builtin_object_toLocaleString(ant_params_t) {
  ant_value_t result;
  if (!js_try_invoke(js, call_this, "toString", NULL, 0, &result))
    return js_mkerr_typed(js, JS_ERR_TYPE, "toString is not a function");
  return result;
}

void js_object_init_prototype(ant_t *js, ant_value_t object_proto) {
  defmethod(js, object_proto, "toString", 8, Ant_Function_Create(builtin_object_toString));
  defmethod(js, object_proto, "valueOf", 7, Ant_Function_Create(builtin_object_valueOf));
  defmethod(js, object_proto, "toLocaleString", 14, Ant_Function_Create(builtin_object_toLocaleString));
  defmethod(js, object_proto, "hasOwnProperty", 14, Ant_Function_CreateWithArity(builtin_object_hasOwnProperty, 1));
  defmethod(js, object_proto, "isPrototypeOf", 13, Ant_Function_CreateWithArity(builtin_object_isPrototypeOf, 1));
  defmethod(js, object_proto, "propertyIsEnumerable", 20, Ant_Function_CreateWithArity(builtin_object_propertyIsEnumerable, 1));
  defmethod(js, object_proto, "__defineGetter__", 16, Ant_Function_CreateWithArity(builtin_object___defineGetter__, 2));
  defmethod(js, object_proto, "__defineSetter__", 16, Ant_Function_CreateWithArity(builtin_object___defineSetter__, 2));
  defmethod(js, object_proto, "__lookupGetter__", 16, Ant_Function_CreateWithArity(builtin_object___lookupGetter__, 1));
  defmethod(js, object_proto, "__lookupSetter__", 16, Ant_Function_CreateWithArity(builtin_object___lookupSetter__, 1));
  
  ant_value_t proto_getter = Ant_Function_Create(builtin_proto_getter);
  ant_value_t proto_setter = Ant_Function_Create(builtin_proto_setter);
  js_set_accessor_desc(
    js, object_proto, "__proto__", sizeof("__proto__") - 1,
    proto_getter, proto_setter, JS_DESC_C
  );
}

void js_object_init_constructor(ant_t *js, ant_value_t obj_func_obj, ant_value_t object_proto) {
  js_set_slot(obj_func_obj, SLOT_BUILTIN, Ant_Value_FromNumber(BUILTIN_OBJECT));
  js_mark_constructor(obj_func_obj, true);
  
  defmethod(js, obj_func_obj, "keys", 4, Ant_Function_CreateWithArity(builtin_object_keys, 1));
  defmethod(js, obj_func_obj, "values", 6, Ant_Function_CreateWithArity(builtin_object_values, 1));
  defmethod(js, obj_func_obj, "entries", 7, Ant_Function_CreateWithArity(builtin_object_entries, 1));
  defmethod(js, obj_func_obj, "is", 2, Ant_Function_CreateWithArity(builtin_object_is, 2));
  defmethod(js, obj_func_obj, "getPrototypeOf", 14, Ant_Function_CreateWithArity(builtin_object_getPrototypeOf, 1));
  defmethod(js, obj_func_obj, "setPrototypeOf", 14, Ant_Function_CreateWithArity(builtin_object_setPrototypeOf, 2));
  defmethod(js, obj_func_obj, "create", 6, Ant_Function_CreateWithArity(builtin_object_create, 2));
  defmethod(js, obj_func_obj, "hasOwn", 6, Ant_Function_CreateWithArity(builtin_object_hasOwn, 2));
  defmethod(js, obj_func_obj, "groupBy", 7, Ant_Function_CreateWithArity(builtin_object_groupBy, 2));
  defmethod(js, obj_func_obj, "defineProperty", 14, Ant_Function_CreateWithArity(builtin_object_defineProperty, 3));
  defmethod(js, obj_func_obj, "defineProperties", 16, Ant_Function_CreateWithArity(builtin_object_defineProperties, 2));
  defmethod(js, obj_func_obj, "assign", 6, Ant_Function_CreateWithArity(builtin_object_assign, 2));
  defmethod(js, obj_func_obj, "freeze", 6, Ant_Function_CreateWithArity(builtin_object_freeze, 1));
  defmethod(js, obj_func_obj, "isFrozen", 8, Ant_Function_CreateWithArity(builtin_object_isFrozen, 1));
  defmethod(js, obj_func_obj, "seal", 4, Ant_Function_CreateWithArity(builtin_object_seal, 1));
  defmethod(js, obj_func_obj, "isSealed", 8, Ant_Function_CreateWithArity(builtin_object_isSealed, 1));
  defmethod(js, obj_func_obj, "fromEntries", 11, Ant_Function_CreateWithArity(builtin_object_fromEntries, 1));
  defmethod(js, obj_func_obj, "getOwnPropertyDescriptor", 24, Ant_Function_CreateWithArity(builtin_object_getOwnPropertyDescriptor, 2));
  defmethod(js, obj_func_obj, "getOwnPropertyDescriptors", 25, Ant_Function_CreateWithArity(builtin_object_getOwnPropertyDescriptors, 1));
  defmethod(js, obj_func_obj, "getOwnPropertyNames", 19, Ant_Function_CreateWithArity(builtin_object_getOwnPropertyNames, 1));
  defmethod(js, obj_func_obj, "getOwnPropertySymbols", 21, Ant_Function_CreateWithArity(builtin_object_getOwnPropertySymbols, 1));
  defmethod(js, obj_func_obj, "isExtensible", 12, Ant_Function_CreateWithArity(builtin_object_isExtensible, 1));
  defmethod(js, obj_func_obj, "preventExtensions", 17, Ant_Function_CreateWithArity(builtin_object_preventExtensions, 1));
  
  js_define_function_length(js, obj_func_obj, 1);
  js_set_function_name(js, obj_func_obj, "Object", sizeof("Object") - 1);
  js_setprop_readonly_nonconfigurable(js, obj_func_obj, "prototype", 9, object_proto);
}
