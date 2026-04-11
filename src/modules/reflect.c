#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "silver/engine.h"

#include "modules/reflect.h"
#include "modules/symbol.h"

static ant_value_t reflect_get(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  
  ant_value_t target = args[0];
  ant_value_t key = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_mkundef();
  
  if (vtype(key) != T_STR) return js_mkundef();
  
  char *key_str = js_getstr(js, key, NULL);
  if (!key_str) return js_mkundef();
  
  return js_get(js, target, key_str);
}

static ant_value_t reflect_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_false;
  
  ant_value_t target = args[0];
  ant_value_t key = args[1];
  ant_value_t value = args[2];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  if (vtype(key) != T_STR) return js_false;
  
  char *key_str = js_getstr(js, key, NULL);
  if (!key_str) return js_false;
  
  js_set(js, target, key_str, value);
  return js_true; 
}

static ant_value_t reflect_has(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_false;
  
  ant_value_t target = args[0];
  ant_value_t key = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  if (vtype(key) != T_STR) return js_false;
  
  size_t key_len;
  char *key_str = js_getstr(js, key, &key_len);
  if (!key_str) return js_false;
  
  ant_offset_t off = lkp_proto(js, target, key_str, key_len);
  return js_bool(off > 0);
}

static ant_value_t reflect_delete_property(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_false;
  
  ant_value_t target = args[0];
  ant_value_t key = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  if (vtype(key) != T_STR) return js_false;
  
  char *key_str = js_getstr(js, key, NULL);
  if (!key_str) return js_false;
  
  ant_value_t del_result = js_delete_prop(js, target, key_str, strlen(key_str));
  bool deleted = !is_err(del_result) && js_truthy(js, del_result);
  return js_bool(deleted);
}

static ant_value_t reflect_own_keys(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkarr(js);
  
  ant_value_t target = args[0];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) {
    return js_mkerr(js, "Reflect.ownKeys called on non-object");
  }
  
  ant_value_t keys_arr = js_mkarr(js);
  ant_iter_t iter = js_prop_iter_begin(js, target);
  const char *key; size_t key_len; ant_value_t value;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (key_len >= 9 && memcmp(key, STR_PROTO, STR_PROTO_LEN) == 0) continue;
    if (key_len >= 2 && key[0] == '_' && key[1] == '_') continue;
    js_arr_push(js, keys_arr, js_mkstr(js, key, key_len));
  }
  
  js_prop_iter_end(&iter);
  return keys_arr;
}

static ant_value_t reflect_construct(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "Reflect.construct requires at least 2 arguments");
  }
  
  ant_value_t target = args[0];
  ant_value_t args_arr = args[1];
  ant_value_t new_target = (nargs >= 3) ? args[2] : target;
  
  if (vtype(target) != T_FUNC && vtype(target) != T_CFUNC) {
    return js_mkerr(js, "Reflect.construct: first argument must be a constructor");
  }
  
  if (vtype(new_target) != T_FUNC && vtype(new_target) != T_CFUNC) {
    return js_mkerr(js, "Reflect.construct: third argument must be a constructor");
  }
  
  ant_value_t length_val = js_get(js, args_arr, "length");
  int arg_count = 0;
  if (vtype(length_val) == T_NUM) {
    arg_count = (int)js_getnum(length_val);
  }
  
  ant_value_t *call_args = NULL;
  if (arg_count > 0) {
    call_args = malloc(arg_count * sizeof(ant_value_t));
    if (!call_args) return js_mkerr(js, "Out of memory");
    
    for (int i = 0; i < arg_count; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%d", i);
      call_args[i] = js_get(js, args_arr, idx);
    }
  }
  
  ant_value_t new_obj = js_mkobj(js);
  ant_value_t proto = js_get(js, new_target, "prototype");
  if (vtype(proto) == T_OBJ) js_set_proto_init(new_obj, proto);

  ant_value_t saved_new_target = js->new_target;
  js->new_target = new_target;

  ant_value_t result = sv_vm_call(js->vm, js, target, new_obj, call_args, arg_count, NULL, true);
  js->new_target = saved_new_target;
  
  if (call_args) free(call_args);
  if (is_object_type(result)) return result;
  
  return new_obj;
}

static ant_value_t reflect_apply(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Reflect.apply requires 3 arguments");
  }
  
  ant_value_t target = args[0];
  ant_value_t this_arg = args[1];
  ant_value_t args_arr = args[2];
  
  if (vtype(target) != T_FUNC && vtype(target) != T_CFUNC) {
    return js_mkerr(js, "Reflect.apply: first argument must be a function");
  }

  if (vtype(args_arr) == T_UNDEF || vtype(args_arr) == T_NULL) return js_mkerr_typed(
    js, JS_ERR_TYPE,
    "Reflect.apply: third argument must be an array-like object"
  );

  ant_value_t result;
  if (vtype(args_arr) == T_ARR) {
    ant_value_t *call_args = NULL;
    int arg_count = extract_array_args(js, args_arr, &call_args);
    result = sv_vm_call_explicit_this(
      js->vm, js, target, this_arg, 
      call_args, arg_count
    );
    if (call_args) free(call_args);
    return result;
  }
  
  ant_value_t length_val = js_get(js, args_arr, "length");
  int arg_count = 0;
  if (vtype(length_val) == T_NUM) {
    arg_count = (int)js_getnum(length_val);
  }
  
  ant_value_t *call_args = NULL;
  if (arg_count > 0) {
    call_args = malloc(arg_count * sizeof(ant_value_t));
    if (!call_args) return js_mkerr(js, "Out of memory");
    
    for (int i = 0; i < arg_count; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%d", i);
      call_args[i] = js_get(js, args_arr, idx);
    }
  }
  
  result = sv_vm_call_explicit_this(
    js->vm, js, target, this_arg, 
    call_args, arg_count
  );
  
  if (call_args) free(call_args);
  return result;
}

static ant_value_t reflect_get_own_property_descriptor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  
  ant_value_t target = args[0];
  ant_value_t key = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_mkundef();
  
  if (vtype(key) != T_STR) return js_mkundef();
  
  size_t key_len;
  char *key_str = js_getstr(js, key, &key_len);
  if (!key_str) return js_mkundef();
  
  ant_offset_t off = lkp(js, target, key_str, key_len);
  if (off <= 0) return js_mkundef();
  
  ant_value_t value = js_get(js, target, key_str);
  ant_value_t desc = js_mkobj(js);
  js_set(js, desc, "value", value);
  js_set(js, desc, "writable", js_true);
  js_set(js, desc, "enumerable", js_true);
  js_set(js, desc, "configurable", js_true);
  
  return desc;
}

static ant_value_t reflect_define_property(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_false;
  return js_define_property(js, args[0], args[1], args[2], true);
}

static ant_value_t reflect_get_prototype_of(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Reflect.getPrototypeOf requires an argument");
  }
  
  ant_value_t target = args[0];
  
  if (!is_object_type(target)) {
    return js_mkerr(js, "Reflect.getPrototypeOf: argument must be an object");
  }
  
  return js_get_proto(js, target);
}

static ant_value_t reflect_set_prototype_of(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_false;
  
  ant_value_t target = args[0];
  ant_value_t proto = args[1];
  
  if (!is_object_type(target)) return js_false;
  if (!is_object_type(proto) && vtype(proto) != T_NULL) return js_false;
  if (vtype(proto) != T_NULL && proto_chain_contains(js, proto, target)) return js_false;
  
  js_set_proto_wb(js, target, proto);
  
  return js_true;
}

static ant_value_t reflect_is_extensible(ant_t *js, ant_value_t *args, int nargs) {
  (void)js;
  if (nargs < 1) return js_false;
  
  ant_value_t target = args[0];
  int t = vtype(target);
  
  if (t != T_OBJ && t != T_FUNC) return js_false;

  ant_object_t *obj = js_obj_ptr(js_as_obj(target));
  if (!obj) return js_false;
  if (obj->frozen || obj->sealed) return js_false;
  return js_bool(obj->extensible);
}

static ant_value_t reflect_prevent_extensions(ant_t *js, ant_value_t *args, int nargs) {
  (void)js;
  if (nargs < 1) return js_false;
  
  ant_value_t target = args[0];
  int t = vtype(target);
  
  if (t != T_OBJ && t != T_FUNC) return js_false;

  ant_object_t *obj = js_obj_ptr(js_as_obj(target));
  if (!obj) return js_false;
  obj->extensible = 0;
  return js_true;
}

void init_reflect_module(void) {
  ant_t *js = rt->js;
  ant_value_t reflect_obj = js_mkobj(js);
  
  js_set(js, reflect_obj, "get", js_mkfun(reflect_get));
  js_set(js, reflect_obj, "set", js_mkfun(reflect_set));
  js_set(js, reflect_obj, "has", js_mkfun(reflect_has));
  js_set(js, reflect_obj, "deleteProperty", js_mkfun(reflect_delete_property));
  js_set(js, reflect_obj, "ownKeys", js_mkfun(reflect_own_keys));
  js_set(js, reflect_obj, "construct", js_mkfun(reflect_construct));
  js_set(js, reflect_obj, "apply", js_mkfun(reflect_apply));
  js_set(js, reflect_obj, "getOwnPropertyDescriptor", js_mkfun(reflect_get_own_property_descriptor));
  js_set(js, reflect_obj, "defineProperty", js_mkfun(reflect_define_property));
  js_set(js, reflect_obj, "getPrototypeOf", js_mkfun(reflect_get_prototype_of));
  js_set(js, reflect_obj, "setPrototypeOf", js_mkfun(reflect_set_prototype_of));
  js_set(js, reflect_obj, "isExtensible", js_mkfun(reflect_is_extensible));
  js_set(js, reflect_obj, "preventExtensions", js_mkfun(reflect_prevent_extensions));
  
  js_set_sym(js, reflect_obj, get_toStringTag_sym(), js_mkstr(js, "Reflect", 7));
  js_set(js, js_glob(js), "Reflect", reflect_obj);
}
