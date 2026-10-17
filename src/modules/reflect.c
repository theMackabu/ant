#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "silver/call.h"

#include "modules/reflect.h"

static ant_value_t reflect_target(ant_t *js, ant_value_t *args, int nargs, const char *method) {
  ant_value_t target = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(target) == kTypeBuiltin) target = js_cfunc_promote(js, target);
  if (is_err(target) || is_object_type(target)) return target;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Reflect.%s called on non-object", method);
}

static ant_value_t reflect_get(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "get");
  if (is_err(target)) return target;
  
  ant_value_t key = nargs > 1 ? args[1] : js_mkundef();
  bool has_receiver = nargs > 2;
  ant_value_t receiver = has_receiver ? args[2] : target;

  ant_value_t prop_key = key;
  if (is_object_type(prop_key)) {
    prop_key = js_to_primitive(js, prop_key, 1);
    if (is_err(prop_key)) return prop_key;
  }

  if (vtype(prop_key) == kTypeSymbol) return has_receiver
    ? js_get_sym_with_receiver(js, target, prop_key, receiver)
    : js_get_sym(js, target, prop_key);

  if (vtype(prop_key) != kTypeString) {
    prop_key = js_tostring_val(js, prop_key);
    if (is_err(prop_key)) return prop_key;
  }

  char *key_str = js_getstr(js, prop_key, NULL);
  if (!key_str) return js_mkundef();

  if (!has_receiver || strict_eq_values(js, target, receiver))
    return js_getprop_fallback(js, target, key_str);
  return js_getprop_super(js, target, receiver, key_str);
}

static ant_value_t reflect_set(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "set");
  if (is_err(target)) return target;
  
  ant_value_t key = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t value = nargs > 2 ? args[2] : js_mkundef();
  
  if (is_object_type(key)) {
    key = js_to_primitive(js, key, 1);
    if (is_err(key)) return key;
  }
  
  if (vtype(key) != kTypeSymbol && vtype(key) != kTypeString) {
    key = js_tostring_val(js, key);
    if (is_err(key)) return key;
  }
  
  ant_value_t result = nargs > 3 || vtype(args[0]) == kTypeBuiltin
    ? js_set_with_receiver(js, target, key, value, nargs > 3 ? args[3] : args[0])
    : js_set_reporting(js, target, key, value);
  
  if (is_err(result)) return result;
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  
  return result;
}

static ant_value_t reflect_has(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "has");
  if (is_err(target)) return target;
  return do_in(js, nargs >= 2 ? args[1] : js_mkundef(), target);
}

static ant_value_t reflect_delete_property(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "deleteProperty");
  if (is_err(target)) return target;
  
  ant_value_t key = nargs > 1 ? args[1] : js_mkundef();
  if (vtype(key) != kTypeString) return js_false;
  
  char *key_str = js_getstr(js, key, NULL);
  if (!key_str) return js_false;
  
  ant_value_t del_result = js_delete_prop_ordinary(js, target, key_str, strlen(key_str));
  return is_err(del_result) ? del_result : js_bool(js_truthy(js, del_result));
}

static ant_value_t reflect_own_keys(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "ownKeys");
  if (is_err(target)) return target;
  return js_own_property_keys(js, target, true, false);
}

static ant_value_t reflect_construct(ant_params_t) {
  if (nargs < 2) {
    return js_mkerr(js, "Reflect.construct requires at least 2 arguments");
  }
  
  ant_value_t target = args[0];
  ant_value_t args_arr = args[1];
  ant_value_t new_target = (nargs >= 3) ? args[2] : target;
  
  if (!js_is_constructor(target)) {
    return js_mkerr(js, "Reflect.construct: first argument must be a constructor");
  }
  
  if (nargs >= 3 && !js_is_constructor(new_target)) {
    return js_mkerr(js, "Reflect.construct: third argument must be a constructor");
  }
  
  if (!is_object_type(args_arr))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Reflect.construct: second argument must be an array-like object");
  
  ant_arg_list_t list;
  ant_value_t err = Ant_ArgList_FromArrayLike(js, args_arr, &list);
  if (is_err(err)) return err;
  
  ant_value_t result;
  if (vtype(target) == kTypeObject && is_proxy(target)) {
    result = js_proxy_construct(js, target, list.args, list.argc, new_target);
  } else {
    ant_value_t effective_new_target = new_target;
    ant_value_t proto = sv_prepare_construct_meta(js, target, new_target, &effective_new_target);
    
    if (is_err(proto)) {
      Ant_ArgList_Release(&list);
      return proto;
    }

    ant_value_t new_obj = Ant_Object_CreateBare(js);
    if (is_object_type(proto)) js_set_proto_init(new_obj, proto);

    ant_value_t ctor_this = new_obj;
    result = sv_vm_call(
      js->vm, js, target, new_obj, list.args, 
      list.argc, &ctor_this, effective_new_target
    );
    
    if (!is_err(result) && !is_object_type(result))
      result = is_object_type(ctor_this) ? ctor_this : new_obj;
  }
  
  Ant_ArgList_Release(&list);
  return result;
}

static ant_value_t reflect_apply(ant_params_t) {
  if (nargs < 3) {
    return js_mkerr(js, "Reflect.apply requires 3 arguments");
  }
  
  ant_value_t target = args[0];
  ant_value_t this_arg = args[1];
  ant_value_t args_arr = args[2];
  
  if (vtype(target) != kTypeFunction && vtype(target) != kTypeBuiltin) {
    return js_mkerr(js, "Reflect.apply: first argument must be a function");
  }

  if (vtype(args_arr) == kTypeUndefined || vtype(args_arr) == kTypeNull) return js_mkerr_typed(
    js, JS_ERR_TYPE,
    "Reflect.apply: third argument must be an array-like object"
  );

  if (!is_object_type(args_arr))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Reflect.apply: third argument must be an array-like object");

  ant_arg_list_t list;
  ant_value_t err = Ant_ArgList_FromArrayLike(js, args_arr, &list);
  if (is_err(err)) return err;

  ant_value_t result = sv_vm_call_explicit_this(js->vm, js, target, this_arg, list.args, list.argc);
  Ant_ArgList_Release(&list);
  
  return result;
}

static ant_value_t reflect_get_own_property_descriptor(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "getOwnPropertyDescriptor");
  if (is_err(target)) return target;
  
  ant_value_t call_args[2] = { target, nargs > 1 ? args[1] : js_mkundef() };
  return builtin_object_getOwnPropertyDescriptor(js, call_args, 2, js_mkundef());
}

static ant_value_t reflect_define_property(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "defineProperty");
  if (is_err(target)) return target;
  return js_define_property(js, target, nargs > 1 ? args[1] : js_mkundef(), nargs > 2 ? args[2] : js_mkundef(), true);
}

static ant_value_t reflect_get_prototype_of(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "getPrototypeOf");
  if (is_err(target)) return target;
  return js_get_proto(js, target);
}

static ant_value_t reflect_set_prototype_of(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "setPrototypeOf");
  if (is_err(target)) return target;
  
  ant_value_t proto = nargs > 1 ? args[1] : js_mkundef();
  if (vtype(proto) == kTypeBuiltin) {
    proto = js_cfunc_promote(js, proto);
    if (is_err(proto)) return proto;
  }
  if (!is_object_type(proto) && vtype(proto) != kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object prototype may only be an Object or null");
  if (vtype(proto) != kTypeNull && proto_chain_contains(js, proto, target)) return js_false;
  
  js_set_proto_wb(js, target, proto);
  
  return js_true;
}

static ant_value_t reflect_is_extensible(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "isExtensible");
  if (is_err(target)) return target;

  ant_object_t *obj = js_obj_ptr(js_as_obj(target));
  if (!obj) return js_false;
  if (obj->flags.frozen || obj->flags.sealed) return js_false;
  
  return js_bool(obj->flags.extensible);
}

static ant_value_t reflect_prevent_extensions(ant_params_t) {
  ant_value_t target = reflect_target(js, args, nargs, "preventExtensions");
  if (is_err(target)) return target;
  if (is_proxy(js_as_obj(target))) return js_proxy_prevent_extensions(js, js_as_obj(target));

  ant_object_t *obj = js_obj_ptr(js_as_obj(target));
  if (!obj) return js_false;
  obj->flags.extensible = 0;
  
  return js_true;
}

void init_reflect_module(ant_t *js) {
  ant_value_t reflect_obj = Ant_Object_Create(js);
  
  defmethod(js, reflect_obj, "get", 3, Ant_Function_CreateWithArity(reflect_get, 2));
  defmethod(js, reflect_obj, "set", 3, Ant_Function_CreateWithArity(reflect_set, 3));
  defmethod(js, reflect_obj, "has", 3, Ant_Function_CreateWithArity(reflect_has, 2));
  defmethod(js, reflect_obj, "deleteProperty", 14, Ant_Function_CreateWithArity(reflect_delete_property, 2));
  defmethod(js, reflect_obj, "ownKeys", 7, Ant_Function_CreateWithArity(reflect_own_keys, 1));
  defmethod(js, reflect_obj, "construct", 9, Ant_Function_CreateWithArity(reflect_construct, 2));
  defmethod(js, reflect_obj, "apply", 5, Ant_Function_CreateWithArity(reflect_apply, 3));
  defmethod(js, reflect_obj, "getOwnPropertyDescriptor", 24, Ant_Function_CreateWithArity(reflect_get_own_property_descriptor, 2));
  defmethod(js, reflect_obj, "defineProperty", 14, Ant_Function_CreateWithArity(reflect_define_property, 3));
  defmethod(js, reflect_obj, "getPrototypeOf", 14, Ant_Function_CreateWithArity(reflect_get_prototype_of, 1));
  defmethod(js, reflect_obj, "setPrototypeOf", 14, Ant_Function_CreateWithArity(reflect_set_prototype_of, 2));
  defmethod(js, reflect_obj, "isExtensible", 12, Ant_Function_CreateWithArity(reflect_is_extensible, 1));
  defmethod(js, reflect_obj, "preventExtensions", 17, Ant_Function_CreateWithArity(reflect_prevent_extensions, 1));
  
  js_set_tostringtag(js, reflect_obj, js_mkstr(js, "Reflect", 7));
  js_set_global_builtin(js, "Reflect", reflect_obj);
}
