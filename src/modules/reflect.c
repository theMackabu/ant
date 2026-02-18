#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"

#include "modules/reflect.h"
#include "modules/symbol.h"

static jsval_t reflect_get(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  
  jsval_t target = args[0];
  jsval_t key = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_mkundef();
  
  if (vtype(key) != T_STR) return js_mkundef();
  
  char *key_str = js_getstr(js, key, NULL);
  if (!key_str) return js_mkundef();
  
  return js_get(js, target, key_str);
}

static jsval_t reflect_set(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 3) return js_false;
  
  jsval_t target = args[0];
  jsval_t key = args[1];
  jsval_t value = args[2];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  if (vtype(key) != T_STR) return js_false;
  
  char *key_str = js_getstr(js, key, NULL);
  if (!key_str) return js_false;
  
  js_set(js, target, key_str, value);
  return js_true; 
}

static jsval_t reflect_has(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_false;
  
  jsval_t target = args[0];
  jsval_t key = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  if (vtype(key) != T_STR) return js_false;
  
  size_t key_len;
  char *key_str = js_getstr(js, key, &key_len);
  if (!key_str) return js_false;
  
  jsoff_t off = lkp_proto(js, target, key_str, key_len);
  return js_bool(off > 0);
}

static jsval_t reflect_delete_property(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_false;
  
  jsval_t target = args[0];
  jsval_t key = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  if (vtype(key) != T_STR) return js_false;
  
  char *key_str = js_getstr(js, key, NULL);
  if (!key_str) return js_false;
  
  bool deleted = js_del(js, target, key_str);
  return js_bool(deleted);
}

static jsval_t reflect_own_keys(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkarr(js);
  
  jsval_t target = args[0];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) {
    return js_mkerr(js, "Reflect.ownKeys called on non-object");
  }
  
  jsval_t keys_arr = js_mkarr(js);
  ant_iter_t iter = js_prop_iter_begin(js, target);
  const char *key; size_t key_len; jsval_t value;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (key_len >= 9 && memcmp(key, STR_PROTO, STR_PROTO_LEN) == 0) continue;
    if (key_len >= 2 && key[0] == '_' && key[1] == '_') continue;
    js_arr_push(js, keys_arr, js_mkstr(js, key, key_len));
  }
  
  js_prop_iter_end(&iter);
  return keys_arr;
}

static jsval_t reflect_construct(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "Reflect.construct requires at least 2 arguments");
  }
  
  jsval_t target = args[0];
  jsval_t args_arr = args[1];
  jsval_t new_target = (nargs >= 3) ? args[2] : target;
  
  if (vtype(target) != T_FUNC && vtype(target) != T_CFUNC) {
    return js_mkerr(js, "Reflect.construct: first argument must be a constructor");
  }
  
  if (vtype(new_target) != T_FUNC && vtype(new_target) != T_CFUNC) {
    return js_mkerr(js, "Reflect.construct: third argument must be a constructor");
  }
  
  jsval_t length_val = js_get(js, args_arr, "length");
  int arg_count = 0;
  if (vtype(length_val) == T_NUM) {
    arg_count = (int)js_getnum(length_val);
  }
  
  jsval_t *call_args = NULL;
  if (arg_count > 0) {
    call_args = malloc(arg_count * sizeof(jsval_t));
    if (!call_args) return js_mkerr(js, "Out of memory");
    
    for (int i = 0; i < arg_count; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%d", i);
      call_args[i] = js_get(js, args_arr, idx);
    }
  }
  
  jsval_t new_obj = js_mkobj(js);
  jsval_t proto = js_get(js, new_target, "prototype");
  if (vtype(proto) == T_OBJ) js_set_proto(js, new_obj, proto);

  jsval_t saved_new_target = js->new_target;
  js->new_target = new_target;
  
  jsval_t result = js_call_with_this(js, target, new_obj, call_args, arg_count);
  js->new_target = saved_new_target;
  if (call_args) free(call_args);
  
  int result_type = vtype(result);
  if (result_type == T_FUNC || is_special_object(result)) return result;
  return new_obj;
}

static jsval_t reflect_apply(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 3) {
    return js_mkerr(js, "Reflect.apply requires 3 arguments");
  }
  
  jsval_t target = args[0];
  jsval_t this_arg = args[1];
  jsval_t args_arr = args[2];
  
  if (vtype(target) != T_FUNC && vtype(target) != T_CFUNC) {
    return js_mkerr(js, "Reflect.apply: first argument must be a function");
  }
  
  jsval_t length_val = js_get(js, args_arr, "length");
  int arg_count = 0;
  if (vtype(length_val) == T_NUM) {
    arg_count = (int)js_getnum(length_val);
  }
  
  jsval_t *call_args = NULL;
  if (arg_count > 0) {
    call_args = malloc(arg_count * sizeof(jsval_t));
    if (!call_args) return js_mkerr(js, "Out of memory");
    
    for (int i = 0; i < arg_count; i++) {
      char idx[16];
      snprintf(idx, sizeof(idx), "%d", i);
      call_args[i] = js_get(js, args_arr, idx);
    }
  }
  
  jsval_t result = js_call_with_this(js, target, this_arg, call_args, arg_count);
  
  if (call_args) free(call_args);
  return result;
}

static jsval_t reflect_get_own_property_descriptor(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();
  
  jsval_t target = args[0];
  jsval_t key = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_mkundef();
  
  if (vtype(key) != T_STR) return js_mkundef();
  
  size_t key_len;
  char *key_str = js_getstr(js, key, &key_len);
  if (!key_str) return js_mkundef();
  
  jsoff_t off = lkp(js, target, key_str, key_len);
  if (off <= 0) return js_mkundef();
  
  jsval_t value = js_get(js, target, key_str);
  jsval_t desc = js_mkobj(js);
  js_set(js, desc, "value", value);
  js_set(js, desc, "writable", js_true);
  js_set(js, desc, "enumerable", js_true);
  js_set(js, desc, "configurable", js_true);
  
  return desc;
}

static jsval_t reflect_define_property(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 3) return js_false;
  
  jsval_t target = args[0];
  jsval_t key = args[1];
  jsval_t descriptor = args[2];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  if (vtype(key) != T_STR) return js_false;
  if (!is_special_object(descriptor)) return js_false;
  
  char *key_str = js_getstr(js, key, NULL);
  if (!key_str) return js_false;
  
  jsval_t value = js_get(js, descriptor, "value");
  js_set(js, target, key_str, value);
  
  return js_true;
}

static jsval_t reflect_get_prototype_of(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Reflect.getPrototypeOf requires an argument");
  }
  
  jsval_t target = args[0];
  
  if (!is_object_type(target)) {
    return js_mkerr(js, "Reflect.getPrototypeOf: argument must be an object");
  }
  
  return js_get_proto(js, target);
}

static jsval_t reflect_set_prototype_of(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_false;
  
  jsval_t target = args[0];
  jsval_t proto = args[1];
  
  int t = vtype(target);
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  int pt = vtype(proto);
  if (pt != T_OBJ && pt != T_FUNC && pt != T_NULL) return js_false;
  
  js_set_proto(js, target, proto);
  return js_true;
}

static jsval_t reflect_is_extensible(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_false;
  
  jsval_t target = args[0];
  int t = vtype(target);
  
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  if (js_get_slot(js, target, SLOT_EXTENSIBLE) == js_false) return js_false;
  if (js_get_slot(js, target, SLOT_FROZEN) == js_true)      return js_false;
  if (js_get_slot(js, target, SLOT_SEALED) == js_true)      return js_false;
  
  return js_true;
}

static jsval_t reflect_prevent_extensions(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_false;
  
  jsval_t target = args[0];
  int t = vtype(target);
  
  if (t != T_OBJ && t != T_FUNC) return js_false;
  
  js_set_slot(js, target, SLOT_EXTENSIBLE, js_false);
  return js_true;
}

void init_reflect_module(void) {
  struct js *js = rt->js;
  jsval_t reflect_obj = js_mkobj(js);
  
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
  
  js_set(js, reflect_obj, get_toStringTag_sym_key(), js_mkstr(js, "Reflect", 7));
  js_set(js, js_glob(js), "Reflect", reflect_obj);
}
