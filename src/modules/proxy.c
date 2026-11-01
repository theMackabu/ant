#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "keys.h"
#include "cfunc.h"
#include "arrays.h"
#include "props.h"
#include "proto.h"
#include "silver/call.h"
#include "modules/proxy.h"

static bool proxy_key_same(ant_t *js, ant_value_t a, ant_value_t b) {
  if (vtype(a) != vtype(b)) return false;
  if (vtype(a) == kTypeSymbol) return vdata(a) == vdata(b);
  if (vtype(a) != kTypeString) return false;

  ant_offset_t a_len = 0, b_len = 0;
  ant_offset_t a_off = vstr(js, a, &a_len);
  ant_offset_t b_off = vstr(js, b, &b_len);
  return a_len == b_len &&
    memcmp((const void *)(uintptr_t)a_off, (const void *)(uintptr_t)b_off, a_len) == 0;
}

static bool proxy_keys_include(ant_t *js, ant_value_t keys, ant_value_t key) {
  if (vtype(keys) != kTypeArray) return false;
  ant_offset_t len = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < len; i++) {
    if (proxy_key_same(js, arr_get(js, keys, i), key)) return true;
  }
  return false;
}

static bool proxy_target_key_is_nonconfigurable(ant_t *js, ant_value_t target, ant_value_t key) {
  if (vtype(key) == kTypeSymbol) {
    ant_prop_loc_t off = lkp_sym(target, (ant_offset_t)vdata(key));
    if (off.obj && is_nonconfig_prop(off)) return true;
    prop_meta_t meta;
    return lookup_symbol_prop_meta(js, js_as_obj(target), (ant_offset_t)vdata(key), &meta) && !meta.configurable;
  }

  if (vtype(key) != kTypeString) return false;
  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key, &key_len);
  const char *key_ptr = (const char *)(uintptr_t)key_off;
  ant_prop_loc_t off = lkp(js, target, key_ptr, (size_t)key_len);
  if (off.obj && is_nonconfig_prop(off)) return true;
  prop_meta_t meta;
  return lookup_string_prop_meta(js, js_as_obj(target), key_ptr, (size_t)key_len, &meta) && !meta.configurable;
}

static ant_value_t proxy_host_own_keys(
  ant_t *js, ant_proxy_state_t *data, bool include_symbols, bool enumerable_only
) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t keys = data->host->keys(js, data->target);
  GC_ROOT_PIN(js, keys);
  
  ant_value_t own = is_err(keys) ? keys : js_own_property_keys(js, data->target, include_symbols, enumerable_only);
  GC_ROOT_PIN(js, own);
  
  ant_offset_t own_count = is_err(own) ? 0 : get_array_length(js, own);
  for (ant_offset_t i = 0; i < own_count; i++) {
    ant_value_t key = arr_get(js, own, i);
    size_t key_len = 0;
    const char *key_str = vtype(key) == kTypeString ? js_getstr(js, key, &key_len) : NULL;
    if (key_str && vtype(data->host->get(js, data->target, key_str, key_len)) != kTypeUndefined) continue;
    js_arr_push(js, keys, key);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(own) ? own : keys;
}

ant_value_t proxy_host_pairs(ant_t *js, ant_proxy_state_t *data, bool include_symbols) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t pairs = data->host->flat_entries(js, data->target);
  GC_ROOT_PIN(js, pairs);
  
  ant_value_t own = is_err(pairs) ? pairs : js_own_property_keys(js, data->target, include_symbols, true);
  GC_ROOT_PIN(js, own);
  
  ant_offset_t own_count = is_err(own) ? 0 : get_array_length(js, own);
  for (ant_offset_t i = 0; i < own_count && !is_err(own); i++) {
    GC_ROOT_SAVE(key_mark, js);
    ant_value_t key = arr_get(js, own, i);
    
    size_t key_len = 0;
    bool sym = vtype(key) == kTypeSymbol;
    const char *key_str = sym ? NULL : js_getstr(js, key, &key_len);
    
    if (sym || (key_str && vtype(data->host->get(js, data->target, key_str, key_len)) == kTypeUndefined)) {
      ant_value_t val = sym ? js_get_sym(js, data->target, key) : js_get(js, data->target, key_str);
      GC_ROOT_PIN(js, val);
      if (is_err(val)) own = val;
      else {
        js_arr_push(js, pairs, key);
        js_arr_push(js, pairs, val);
      }
    }
    
    GC_ROOT_RESTORE(js, key_mark);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(own) ? own : pairs;
}

ant_proxy_state_t *get_proxy_data(ant_value_t obj) {
  if (vtype(obj) != kTypeObject) return NULL;
  ant_object_t *ptr = js_obj_ptr(obj);
  return ptr ? ant_object_proxy_state(ptr) : NULL;
}

bool is_proxy(ant_value_t obj) {
  if (vtype(obj) != kTypeObject) return false;
  ant_object_t *ptr = (ant_object_t *)vptr(obj);
  return ptr && ptr->flags.is_exotic && ant_object_proxy_state(ptr) != NULL;
}

bool js_proxy_is_host(ant_value_t obj) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  return data && data->host;
}

ant_value_t proxy_key_enumerable(ant_t *js, ant_value_t proxy, ant_value_t key) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (data && data->host && vtype(key) == kTypeString) {
    size_t key_len = 0;
    const char *key_str = js_getstr(js, key, &key_len);
    if (key_str && !lkp(js, data->target, key_str, key_len).obj) return Ant_Value_True;
  }
  
  ant_value_t desc = proxy_get_own_property_descriptor(js, proxy, key);
  if (is_err(desc)) return desc;
  
  return Ant_Value_FromBool(vtype(desc) != kTypeUndefined && descriptor_bool_value(js, desc, "enumerable", 10, false));
}

static __attribute__((noinline, cold)) ant_value_t proxy_revoked_error(ant_t *js, const char *trap);
static ant_value_t proxy_get_method(ant_t *js, ant_value_t handler, const char *name);
static bool proxy_target_is_extensible(ant_value_t obj);

ant_value_t proxy_own_keys_raw(ant_t *js, ant_value_t obj) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_proxy_state_t *data = get_proxy_data(obj);
  if (!data) {
    GC_ROOT_RESTORE(js, root_mark);
    return Ant_Array_Create(js);
  }
  
  if (data->revoked) {
    GC_ROOT_RESTORE(js, root_mark);
    return proxy_revoked_error(js, "ownKeys");
  }
  
  if (data->host) {
    GC_ROOT_RESTORE(js, root_mark);
    return proxy_host_own_keys(js, data, true, false);
  }

  ant_value_t keys = Ant_Array_Create(js);
  GC_ROOT_PIN(js, keys);

  ant_value_t trap = proxy_get_method(js, data->handler, "ownKeys");
  if (is_err(trap)) {
    GC_ROOT_RESTORE(js, root_mark);
    return trap;
  }
  if (vtype(trap) == kTypeUndefined) {
    keys = js_own_property_keys(js, data->target, true, false);
    GC_ROOT_RESTORE(js, root_mark);
    return keys;
  }

  ant_value_t trap_args[1] = { data->target };
  ant_value_t result = sv_vm_call(js->vm, js, trap, data->handler, trap_args, 1, NULL, Ant_Value_Undefined);
  if (is_err(result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }
  if (vtype(result) != kTypeArray) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap must return an array");
  }
  GC_ROOT_PIN(js, result);

  ant_offset_t len = get_array_length(js, result);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t ki = arr_get(js, result, i);
    if (vtype(ki) != kTypeString && vtype(ki) != kTypeSymbol) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must contain only strings or symbols");
    }
    for (ant_offset_t j = 0; j < i; j++) {
      ant_value_t kj = arr_get(js, result, j);
      if (vtype(ki) != vtype(kj)) continue;
      if (vtype(ki) == kTypeSymbol) {
        if (vdata(ki) == vdata(kj)) {
          GC_ROOT_RESTORE(js, root_mark);
          return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must not contain duplicate entries");
        }
        continue;
      }

      ant_offset_t ki_len;
      ant_offset_t ki_off = vstr(js, ki, &ki_len);
      
      ant_offset_t kj_len;
      ant_offset_t kj_off = vstr(js, kj, &kj_len);
      
      if (ki_len == kj_len && memcmp((const void *)(uintptr_t)ki_off, (const void *)(uintptr_t)kj_off, ki_len) == 0) {
        GC_ROOT_RESTORE(js, root_mark);
        return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must not contain duplicate entries");
      }
    }
  }

  ant_value_t target_keys = js_own_property_keys(js, data->target, true, false);
  GC_ROOT_PIN(js, target_keys);
  
  if (is_err(target_keys)) {
    GC_ROOT_RESTORE(js, root_mark);
    return target_keys;
  }

  ant_offset_t target_len = get_array_length(js, target_keys);
  bool target_extensible = proxy_target_is_extensible(data->target);
  
  for (ant_offset_t i = 0; i < target_len; i++) {
    ant_value_t target_key = arr_get(js, target_keys, i);
    
    if (proxy_target_key_is_nonconfigurable(js, data->target, target_key) && !proxy_keys_include(js, result, target_key)) {
      GC_ROOT_RESTORE(js, root_mark);
      return proxy_trap_error(js, "ownKeys", "trap result did not include non-configurable key");
    }
    
    if (!target_extensible && !proxy_keys_include(js, result, target_key)) {
      GC_ROOT_RESTORE(js, root_mark);
      return proxy_trap_error(js, "ownKeys", "trap result did not include all target keys");
    }
  }

  if (!target_extensible) for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t result_key = arr_get(js, result, i);
    if (!proxy_keys_include(js, target_keys, result_key)) {
      GC_ROOT_RESTORE(js, root_mark);
      return proxy_trap_error(js, "ownKeys", "trap result included extra key for non-extensible target");
    }
  }

  keys = result;
  GC_ROOT_RESTORE(js, root_mark);
  
  return keys;
}

ant_value_t proxy_own_keys_filtered(ant_t *js, ant_value_t obj, bool include_symbols, bool enumerable_only) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  if (data && data->host) return proxy_host_own_keys(js, data, include_symbols, enumerable_only);
  
  ant_value_t keys = proxy_own_keys_raw(js, obj);
  if (is_err(keys) || (include_symbols && !enumerable_only)) return keys;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, keys);
  
  ant_value_t out = Ant_Array_Create(js);
  GC_ROOT_PIN(js, out);
  
  ant_offset_t key_count = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key = arr_get(js, keys, i);
    if (vtype(key) == kTypeSymbol && !include_symbols) continue;

    ant_value_t enumerable = enumerable_only ? proxy_key_enumerable(js, obj, key) : Ant_Value_True;
    if (is_err(enumerable)) { out = enumerable; break; }
    if (js_truthy(js, enumerable)) js_arr_push(js, out, key);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

ant_value_t proxy_not_prevented_error(ant_t *js, ant_value_t proxy, const char *host_message, const char *message) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s", data && data->host ? host_message : message);
}

ant_value_t proxy_set_integrity_level(ant_t *js, ant_value_t obj, bool freeze) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t prevent_result = proxy_prevent_extensions(js, obj);
  if (is_err(prevent_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return prevent_result;
  }
  
  if (!js_truthy(js, prevent_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return proxy_not_prevented_error(js, obj, freeze ? "Cannot freeze" : "Cannot seal", "Proxy preventExtensions trap returned falsy");
  }

  ant_value_t keys = proxy_own_keys_raw(js, obj);
  GC_ROOT_PIN(js, keys);
  if (is_err(keys)) {
    GC_ROOT_RESTORE(js, root_mark);
    return keys;
  }

  ant_offset_t len = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < len; i++) {
    GC_ROOT_SAVE(iter_mark, js);
    
    ant_value_t key = arr_get(js, keys, i);
    GC_ROOT_PIN(js, key);

    ant_value_t desc = Ant_Object_CreateBare(js);
    GC_ROOT_PIN(js, desc);
    
    js_setprop(js, desc, js_mkstr(js, "configurable", 12), Ant_Value_False);
    if (freeze) js_setprop(js, desc, js_mkstr(js, "writable", 8), Ant_Value_False);

    ant_value_t result = proxy_define_property(js, obj, key, desc);
    if (is_err(result) || !js_truthy(js, result)) {
      GC_ROOT_RESTORE(js, iter_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return is_err(result) ? result : js_mkerr_typed(js, JS_ERR_TYPE, "Proxy defineProperty trap returned falsy");
    }
    
    GC_ROOT_RESTORE(js, iter_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return obj;
}

ant_value_t proxy_test_integrity_level(ant_t *js, ant_value_t obj, bool frozen) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t extensible = proxy_is_extensible(js, obj);
  if (is_err(extensible)) {
    GC_ROOT_RESTORE(js, root_mark);
    return extensible;
  }
  if (js_truthy(js, extensible)) {
    GC_ROOT_RESTORE(js, root_mark);
    return Ant_Value_False;
  }

  ant_value_t keys = proxy_own_keys_raw(js, obj);
  GC_ROOT_PIN(js, keys);
  if (is_err(keys)) {
    GC_ROOT_RESTORE(js, root_mark);
    return keys;
  }

  ant_offset_t len = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < len; i++) {
    GC_ROOT_SAVE(iter_mark, js);
    ant_value_t key = arr_get(js, keys, i);
    GC_ROOT_PIN(js, key);

    ant_value_t desc = proxy_get_own_property_descriptor(js, obj, key);
    GC_ROOT_PIN(js, desc);
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, iter_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    if (vtype(desc) == kTypeUndefined ||
        descriptor_bool_value(js, desc, "configurable", 12, false) ||
        (frozen && descriptor_bool_value(js, desc, "writable", 8, false))) {
      GC_ROOT_RESTORE(js, iter_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return Ant_Value_False;
    }

    GC_ROOT_RESTORE(js, iter_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return Ant_Value_True;
}

static ant_value_t proxy_enum_host_pairs(ant_t *js, ant_proxy_state_t *data, enum obj_enum_mode mode) {
  ant_value_t pairs = proxy_host_pairs(js, data, false);
  if (is_err(pairs)) return pairs;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, pairs);
  ant_value_t out = Ant_Array_Create(js);
  GC_ROOT_PIN(js, out);
  
  ant_offset_t len = get_array_length(js, pairs);
  for (ant_offset_t i = 0; i + 1 < len; i += 2) {
    GC_ROOT_SAVE(item_mark, js);
    ant_value_t value = arr_get(js, pairs, i + 1);
    ant_value_t item = mode == OBJ_ENUM_VALUES ? value : map_to_entry(js, arr_get(js, pairs, i), value);
    GC_ROOT_PIN(js, item);
    js_arr_push(js, out, item);
    GC_ROOT_RESTORE(js, item_mark);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

ant_value_t proxy_enum(ant_t *js, ant_value_t obj, enum obj_enum_mode mode) {
  if (mode == OBJ_ENUM_KEYS) return proxy_own_keys_filtered(js, obj, false, true);
  
  ant_proxy_state_t *host_data = get_proxy_data(obj);
  if (host_data && host_data->host && host_data->host->flat_entries) return proxy_enum_host_pairs(js, host_data, mode);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t keys = proxy_own_keys_raw(js, obj);
  GC_ROOT_PIN(js, keys);
  
  ant_value_t out = is_err(keys) ? keys : Ant_Array_Create(js);
  GC_ROOT_PIN(js, out);

  ant_offset_t key_count = is_err(keys) ? 0 : get_array_length(js, keys);
  for (ant_offset_t i = 0; i < key_count && !is_err(out); i++) {
    GC_ROOT_SAVE(iter_mark, js);
    
    ant_value_t key = arr_get(js, keys, i);
    GC_ROOT_PIN(js, key);
    
    ant_value_t enumerable = vtype(key) == kTypeString ? proxy_key_enumerable(js, obj, key) : Ant_Value_False;
    if (is_err(enumerable)) out = enumerable;
    
    else if (js_truthy(js, enumerable)) {
      ant_value_t value = proxy_get_val(js, obj, key);
      GC_ROOT_PIN(js, value);
      
      ant_value_t item = is_err(value) || mode == OBJ_ENUM_VALUES ? value : map_to_entry(js, key, value);
      GC_ROOT_PIN(js, item);
      
      if (is_err(item)) out = item;
      else js_arr_push(js, out, item);
    }
    
    GC_ROOT_RESTORE(js, iter_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t proxy_host_load(ant_t *js, ant_value_t target, ant_prop_loc_t loc, const char *key, size_t key_len) {
  const ant_shape_prop_t *meta = prop_shape_meta(loc);
  if (meta && meta->has_getter) return js_getprop_fallback_len(js, target, key, key_len);
  return js_prop_load(loc);
}

static ant_value_t proxy_host_get(ant_t *js, ant_proxy_state_t *data, const char *key, size_t key_len) {
  ant_value_t target = data->target;
  ant_prop_loc_t loc = lkp(js, target, key, key_len);
  if (loc.obj) return proxy_host_load(js, target, loc, key, key_len);
  
  ant_value_t value = data->host->get(js, target, key, key_len);
  if (vtype(value) != kTypeUndefined) return value;
  
  ant_value_t proto = js_get_proto(js, target);
  if (!is_object_type(proto)) return Ant_Value_Undefined;
  
  ant_prop_loc_t proto_loc = lkp_proto(js, proto, key, key_len);
  return proto_loc.obj ? proxy_host_load(js, target, proto_loc, key, key_len) : Ant_Value_Undefined;
}

bool js_try_host_proxy_get(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  if (!data || !data->host || data->revoked) return false;
  *out = proxy_host_get(js, data, key, key_len);
  return true;
}

static ant_value_t proxy_host_desc(ant_t *js, ant_proxy_state_t *data, ant_value_t key_val) {
  size_t key_len;
  const char *key = js_getstr(js, key_val, &key_len);
  ant_value_t target = data->target;
  
  if (!key || lkp(js, target, key, key_len).obj) {
    ant_value_t args[2] = { target, key_val };
    return builtin_object_getOwnPropertyDescriptor(js, args, 2, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  }
  
  ant_value_t value = data->host->get(js, target, key, key_len);
  if (vtype(value) == kTypeUndefined) return value;
  
  return js_from_data_descriptor(js, value, true, true, true);
}

static __attribute__((noinline, cold)) ant_value_t proxy_revoked_error(ant_t *js, const char *trap) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform '%s' on a proxy that has been revoked", trap);
}

__attribute__((noinline, cold)) ant_value_t proxy_trap_error(ant_t *js, const char *trap, const char *reason) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "'%s' on proxy: %s", trap, reason);
}

static ant_value_t proxy_get_method(ant_t *js, ant_value_t handler, const char *name) {
  ant_value_t trap = js_getprop_fallback(js, handler, name);
  if (is_err(trap)) return trap;
  if (vtype(trap) == kTypeUndefined || vtype(trap) == kTypeNull) return Ant_Value_Undefined;
  if (!is_callable(trap))
    return js_mkerr_typed(js, JS_ERR_TYPE, "'%s' proxy trap is not callable", name);
  return trap;
}

static bool proxy_target_is_extensible(ant_value_t obj) {
  uint8_t t = vtype(obj);
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return false;
  if (ptr->flags.frozen || ptr->flags.sealed) return false;
  return ptr->flags.extensible != 0;
}

typedef struct {
  const char *str;
  size_t len;
  ant_offset_t sym;
  bool is_sym;
} proxy_key_t;

static inline proxy_key_t proxy_key_str(const char *str, size_t len) {
  return (proxy_key_t){ .str = str, .len = len };
}

static inline proxy_key_t proxy_key_sym(ant_value_t sym) {
  return (proxy_key_t){ .sym = (ant_offset_t)vdata(sym), .is_sym = true };
}

static ant_value_t proxy_to_key(ant_t *js, ant_value_t val, ant_value_t *key_val, proxy_key_t *out) {
  if (is_object_type(val)) {
    val = js_to_primitive(js, val, 1);
    if (is_err(val)) return val;
  }
  
  if (vtype(val) == kTypeSymbol) {
    *key_val = val;
    *out = proxy_key_sym(val);
    return Ant_Value_Undefined;
  }

  val = js_tostring_val(js, val);
  if (is_err(val)) return val;

  ant_offset_t len = 0;
  *key_val = val;
  *out = proxy_key_str((const char *)(uintptr_t)vstr(js, val, &len), (size_t)len);
  
  return Ant_Value_Undefined;
}

static bool proxy_target_meta(ant_t *js, ant_value_t target, proxy_key_t key, prop_meta_t *meta) {
  ant_value_t obj = js_as_obj(target);
  return key.is_sym
    ? lookup_symbol_prop_meta(js, obj, key.sym, meta)
    : lookup_string_prop_meta(js, obj, key.str, key.len, meta);
}

static bool proxy_target_value_differs(ant_t *js, ant_value_t target, proxy_key_t key, const prop_meta_t *meta, ant_value_t value) {
  if (meta->slot >= 0) return !same_value_values(
    js, value, ant_object_prop_get_unchecked(js_obj_ptr(js_as_obj(target)), 
    (uint32_t)meta->slot)
  );
  
  ant_prop_loc_t own = key.is_sym 
    ? lkp_sym(target, key.sym) 
    : lkp(js, target, key.str, key.len);
  
  return own.obj && !same_value_values(js, value, js_prop_load(own));
}

static ant_value_t proxy_check_get(ant_t *js, ant_value_t target, proxy_key_t key, ant_value_t result) {
  prop_meta_t meta;
  if (!proxy_target_meta(js, target, key, &meta) || meta.configurable) return result;

  if (meta.has_getter || meta.has_setter) {
    if ((meta.has_getter && vtype(meta.getter) != kTypeUndefined) || vtype(result) == kTypeUndefined) return result;
    return proxy_trap_error(js, "get", "trap returned non-undefined for property with undefined getter");
  }

  if (meta.writable || !proxy_target_value_differs(js, target, key, &meta, result)) return result;
  return proxy_trap_error(js, "get", "trap returned invalid value for non-configurable, non-writable property");
}

static ant_value_t proxy_check_set(ant_t *js, ant_value_t target, proxy_key_t key, ant_value_t value) {
  prop_meta_t meta;
  if (!proxy_target_meta(js, target, key, &meta) || meta.configurable) return Ant_Value_True;

  if (meta.has_getter || meta.has_setter) {
    if (meta.has_setter && vtype(meta.setter) != kTypeUndefined) return Ant_Value_True;
    return proxy_trap_error(js, "set", "trap returned truthy for property with undefined setter");
  }

  if (meta.writable || !proxy_target_value_differs(js, target, key, &meta, value)) return Ant_Value_True;
  return proxy_trap_error(js, "set", "trap returned truthy for non-configurable, non-writable property with different value");
}

static ant_value_t proxy_check_has(ant_t *js, ant_value_t target, proxy_key_t key, ant_value_t result) {
  prop_meta_t meta;
  if (js_truthy(js, result) || !proxy_target_meta(js, target, key, &meta)) return result;
  if (!meta.configurable) return proxy_trap_error(js, "has", "trap returned falsy for non-configurable property");
  if (!proxy_target_is_extensible(target))
    return proxy_trap_error(js, "has", "trap returned falsy for existing property on non-extensible target");
  return result;
}

static ant_value_t proxy_check_delete(ant_t *js, ant_value_t target, proxy_key_t key, ant_value_t result) {
  prop_meta_t meta;
  if (!js_truthy(js, result) || !proxy_target_meta(js, target, key, &meta)) return result;
  if (!meta.configurable) return proxy_trap_error(js, "deleteProperty", "trap returned truthy for non-configurable property");
  if (!proxy_target_is_extensible(target))
    return proxy_trap_error(js, "deleteProperty", "trap returned truthy for property on non-extensible target");
  return result;
}

static ant_value_t proxy_get_from_target(ant_t *js, ant_value_t target, const char *key, size_t key_len, ant_value_t receiver) {
  char key_buf[256];
  char *key_cstr = key_len < sizeof(key_buf) ? key_buf : malloc(key_len + 1);
  if (!key_cstr) return js_mkerr(js, "oom");

  memcpy(key_cstr, key, key_len);
  key_cstr[key_len] = '\0';

  ant_value_t result = js_getprop_super(js, target, receiver, key_cstr);
  if (key_cstr != key_buf) free(key_cstr);
  return result;
}

static ant_value_t proxy_has_on_target(ant_t *js, ant_value_t target, const char *key, size_t key_len) {
  if (vtype(target) == kTypeArray) {
    if (is_length_key(key, key_len) && real_array_obj_ptr(target)) return Ant_Value_True;
    unsigned long idx = 0;
    if (parse_array_index(key, key_len, get_array_length(js, target), &idx))
      return Ant_Value_FromBool(arr_has(js, target, (ant_offset_t)idx));
  }
  return Ant_Value_FromBool(lkp_proto(js, target, key, key_len).obj);
}

ant_value_t proxy_get_with_receiver(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t receiver) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_Undefined;

  if (data->revoked) return proxy_revoked_error(js, "get");
  if (data->host) return proxy_host_get(js, data, key, key_len);

  ant_value_t target = data->target;
  ant_value_t get_trap = proxy_get_method(js, data->handler, "get");
  if (is_err(get_trap)) return get_trap;
  if (vtype(get_trap) == kTypeUndefined) return proxy_get_from_target(js, target, key, key_len, receiver);

  ant_value_t args[3] = { target, js_mkstr(js, key, key_len), receiver };
  ant_value_t result = sv_vm_call(js->vm, js, get_trap, Ant_Value_Undefined, args, 3, NULL, Ant_Value_Undefined);
  if (is_err(result)) return result;

  return proxy_check_get(js, target, proxy_key_str(key, key_len), result);
}

ant_value_t proxy_get(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  return proxy_get_with_receiver(js, proxy, key, key_len, proxy);
}

static __attribute__((noinline)) ant_value_t proxy_set_define_refused(
  ant_t *js, ant_value_t proxy, ant_value_t target, ant_value_t key, const ant_value_t *receiver_desc
) {
  ant_value_t own = receiver_desc ? *receiver_desc : proxy_get_own_property_descriptor(js, proxy, key);
  if (is_err(own)) return own;
  if (is_object_type(own)) return Ant_Value_False;

  ant_value_t cur = target;
  for (int depth = 0; is_object_type(cur); depth++) {
    if (depth >= MAX_PROTO_CHAIN_DEPTH || is_proxy(js_as_obj(cur))) return Ant_Value_False;

    ant_value_t desc = js_get_own_property_descriptor(js, cur, key);
    if (is_err(desc)) return desc;
    if (is_object_type(desc)) {
      if (cur == target) return Ant_Value_False;
      if (vtype(js_get(js, desc, "get")) != kTypeUndefined || vtype(js_get(js, desc, "set")) != kTypeUndefined) return Ant_Value_False;
      return Ant_Value_FromBool(js_truthy(js, js_get(js, desc, "writable")));
    }
    cur = js_get_proto(js, cur);
  }

  return Ant_Value_True;
}

static ant_value_t proxy_receiver_desc_error(ant_t *js, ant_value_t holder, ant_value_t receiver, ant_value_t key, ant_value_t desc) {
  if (vtype(js_get(js, desc, "get")) != kTypeUndefined || vtype(js_get(js, desc, "set")) != kTypeUndefined)
    return vtype(js_get(js, desc, "set")) == kTypeUndefined ? Ant_Error_CreateGetterOnlyProperty(js, holder, key) : Ant_Value_False;
  return js_truthy(js, js_get(js, desc, "writable")) ? Ant_Value_Undefined : Ant_Error_CreateReadOnlyProperty(js, receiver, key);
}

__attribute__((noinline, cold)) ant_value_t proxy_receiver_set_error(
  ant_t *js, ant_value_t proxy, ant_value_t receiver, const char *key_bytes, size_t key_len
) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data || !is_object_type(data->target)) return Ant_Value_False;

  ant_value_t key = js_mkstr(js, key_bytes, key_len);
  if (is_err(key)) return key;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, key);
  ant_value_t result = Ant_Value_False;

  ant_value_t cur = data->target;
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH && is_object_type(cur) && !is_proxy(js_as_obj(cur)); depth++) {
    ant_value_t desc = js_get_own_property_descriptor(js, cur, key);
    if (is_err(desc)) { result = desc; goto done; }
    if (is_object_type(desc)) {
      ant_value_t error = proxy_receiver_desc_error(js, cur, receiver, key, desc);
      if (vtype(error) != kTypeUndefined) { result = error; goto done; }
      break;
    }
    cur = js_get_proto(js, cur);
  }

  if (!is_object_type(receiver)) { result = Ant_Error_CreatePropertyOnPrimitive(js, receiver, key); goto done; }
  if (is_proxy(js_as_obj(receiver))) goto done;

  ant_value_t own = js_get_own_property_descriptor(js, receiver, key);
  if (is_err(own)) { result = own; goto done; }
  if (is_object_type(own)) {
    ant_value_t error = proxy_receiver_desc_error(js, receiver, receiver, key, own);
    if (vtype(error) != kTypeUndefined) result = error;
    goto done;
  }

  ant_value_t refused = check_object_extensibility(js, receiver, key_bytes, key_len);
  if (is_err(refused)) result = refused;

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static __attribute__((noinline, cold)) ant_value_t proxy_define_not_extensible(ant_t *js, ant_value_t key) {
  if (!sv_is_strict_context(js)) return Ant_Value_False;
  ant_value_t text = vtype(key) == kTypeSymbol ? js_symbol_to_string(js, key) : key;
  if (is_err(text)) return text;
  size_t len = 0;
  const char *bytes = js_getstr(js, text, &len);
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot define property %.*s, object is not extensible", (int)len, bytes);
}

static inline bool proxy_target_refuses_add(ant_value_t target) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(target));
  return ptr && (ptr->flags.frozen || ptr->flags.sealed || !ptr->flags.extensible);
}

static bool proxy_handler_plain_for_set(ant_t *js, ant_proxy_state_t *data) {
  if (data->plain_set_epoch == js->ic.epoch + 1) return true;
  if (!is_object_type(data->handler)) return false;

  if (
    !chain_lacks_plain_keys(data->handler, js->intern.set, js->intern.define_property) ||
    !chain_lacks_plain_keys(data->handler, js->intern.get_own_property_descriptor, NULL)
  ) return false;

  for (ant_value_t cur = data->handler; is_object_type(cur);) {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
    ant_object_guard_absence(ptr);
    cur = ptr->proto;
  }

  data->plain_set_epoch = js->ic.epoch + 1;
  return true;
}

ant_value_t proxy_set_with_receiver_reason(
  ant_t *js, ant_value_t proxy, const char *key, size_t key_len,
  ant_value_t value, ant_value_t receiver, bool *trapless
) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  
  if (!data) return Ant_Value_Undefined;
  if (data->revoked) return proxy_revoked_error(js, "set");
  
  if (data->host && same_object_identity(receiver, proxy))
    return data->host->set(js, data->target, key, key_len, value);

  if (
    !data->host && same_object_identity(receiver, proxy) &&
    !proxy_target_refuses_add(data->target) && proxy_handler_plain_for_set(js, data)
  ) {
    const char *interned = intern_string(js, key, key_len);
    ant_value_t key_value = interned ? intern_value(js, interned) : js_mkerr(js, "oom");
    if (is_err(key_value)) return key_value;

    bool declined = false;
    ant_value_t stored = setprop_impl_receiver(js, data->target, proxy, key_value, value, false, &declined);
    if (is_err(stored)) return stored;
    
    return Ant_Value_FromBool(!declined);
  }
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_value_t set_trap = proxy_get_method(js, handler, "set");
  if (is_err(set_trap)) return set_trap;
  if (vtype(set_trap) != kTypeUndefined) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[4] = { target, key_val, value, receiver };
      ant_value_t result = sv_vm_call(js->vm, js, set_trap, Ant_Value_Undefined, args, 4, NULL, Ant_Value_Undefined);
      if (is_err(result)) return result;
      if (!js_truthy(js, result)) return Ant_Value_False;
      return proxy_check_set(js, target, proxy_key_str(key, key_len), value);
  }
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, proxy);
  GC_ROOT_PIN(js, receiver);
  GC_ROOT_PIN(js, value);
  
  ant_value_t key_str = js_mkstr(js, key, key_len);
  GC_ROOT_PIN(js, key_str);
  
  if (is_err(key_str)) {
    GC_ROOT_RESTORE(js, root_mark);
    return key_str;
  }

  ant_value_t own_desc = proxy_get_own_property_descriptor(js, proxy, key_str);
  GC_ROOT_PIN(js, own_desc);
  
  if (is_err(own_desc)) {
    GC_ROOT_RESTORE(js, root_mark);
    return own_desc;
  }

  ant_value_t define_trap = proxy_get_method(js, handler, "defineProperty");
  if (is_err(define_trap)) {
    GC_ROOT_RESTORE(js, root_mark);
    return define_trap;
  }
  
  if (vtype(define_trap) == kTypeUndefined) {
    ant_value_t set_result;
    if (same_object_identity(receiver, proxy)) {
      ant_value_t refused = proxy_target_refuses_add(target)
        ? proxy_set_define_refused(js, proxy, target, key_str, &own_desc)
        : Ant_Value_False;
      
      if (is_err(refused) || refused == Ant_Value_True) {
        ant_value_t result = is_err(refused) ? refused : proxy_define_not_extensible(js, key_str);
        GC_ROOT_RESTORE(js, root_mark);
        return result;
      }

      bool declined = false;
      set_result = setprop_impl_receiver(js, target, proxy, key_str, value, false, &declined);
      if (!is_err(set_result) && declined) {
        GC_ROOT_RESTORE(js, root_mark);
        return Ant_Value_False;
      }
    } else {
      ant_value_t result = js_set_with_receiver(js, target, key_str, value, receiver);
      if (result == Ant_Value_False && trapless) *trapless = true;
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
    
    GC_ROOT_RESTORE(js, root_mark);
    if (is_err(set_result)) return set_result;
    
    return Ant_Value_True;
  }

  ant_value_t desc = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, desc);
  
  if (is_err(desc)) {
    GC_ROOT_RESTORE(js, root_mark);
    return desc;
  }
  
  js_setprop(js, desc, js_mkstr(js, "value", 5), value);
  if (vtype(own_desc) == kTypeUndefined) {
    js_setprop(js, desc, js_mkstr(js, "writable", 8), Ant_Value_True);
    js_setprop(js, desc, js_mkstr(js, "enumerable", 10), Ant_Value_True);
    js_setprop(js, desc, js_mkstr(js, "configurable", 12), Ant_Value_True);
  }

  ant_value_t define_result = proxy_define_property(js, proxy, key_str, desc);
  GC_ROOT_RESTORE(js, root_mark);
  if (is_err(define_result)) return define_result;
  
  return Ant_Value_FromBool(js_truthy(js, define_result));
}

ant_value_t proxy_set_with_receiver(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value, ant_value_t receiver) {
  return proxy_set_with_receiver_reason(js, proxy, key, key_len, value, receiver, NULL);
}

ant_value_t proxy_set(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value) {
  return proxy_set_with_receiver(js, proxy, key, key_len, value, proxy);
}

ant_value_t proxy_set_rejected(ant_t *js, const char *key, size_t key_len, ant_value_t value, bool *declined) {
  if (!sv_is_strict_context(js)) return set_declined(declined, value);
  return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned falsish for property '%.*s'", (int)key_len, key);
}

__attribute__((noinline, cold)) ant_value_t proxy_set_rejected_sym(ant_t *js, ant_value_t sym, ant_value_t value, bool *declined) {
  char desc[128];
  size_t desc_len = tostr(js, sym, desc, sizeof(desc));
  return proxy_set_rejected(js, desc, desc_len < sizeof(desc) ? desc_len : sizeof(desc) - 1, value, declined);
}

ant_value_t proxy_set_sym(
  ant_t *js, ant_value_t proxy, ant_value_t sym, ant_value_t value,
  ant_value_t receiver, ant_value_t *target_out
) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  *target_out = Ant_Value_Undefined;
  if (!data) return Ant_Value_Undefined;
  if (data->revoked) return proxy_revoked_error(js, "set");
  if (data->host) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", "Cannot convert a Symbol value to a string");
  
  ant_value_t target = data->target;
  *target_out = target;
  ant_value_t set_trap = proxy_get_method(js, data->handler, "set");
  if (is_err(set_trap)) return set_trap;

  if (vtype(set_trap) == kTypeUndefined) {
    if (!same_object_identity(receiver, proxy)) return set_trap;
    ant_value_t refused = proxy_target_refuses_add(target)
      ? proxy_set_define_refused(js, proxy, target, sym, NULL)
      : Ant_Value_False;
    if (is_err(refused)) return refused;
    return refused == Ant_Value_True ? proxy_define_not_extensible(js, sym) : set_trap;
  }
  
  ant_value_t args[4] = { target, sym, value, receiver };
  ant_value_t result = sv_vm_call(js->vm, js, set_trap, Ant_Value_Undefined, args, 4, NULL, Ant_Value_Undefined);
  if (is_err(result)) return result;
  if (!js_truthy(js, result)) return Ant_Value_False;

  return proxy_check_set(js, target, proxy_key_sym(sym), value);
}

ant_value_t js_proxy_set(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value) {
  return proxy_set(js, proxy, key, key_len, value);
}

ant_value_t js_proxy_set_sym(ant_t *js, ant_value_t proxy, ant_value_t sym, ant_value_t value) {
  ant_value_t target = Ant_Value_Undefined;
  
  ant_value_t result = proxy_set_sym(js, proxy, sym, value, proxy, &target);
  if (is_err(result) || result == Ant_Value_True || result == Ant_Value_False) return result;

  
  bool declined = false;
  ant_value_t stored = setprop_impl_receiver(js, target, proxy, sym, value, false, &declined);
  if (is_err(stored)) return stored;
  
  return Ant_Value_FromBool(!declined);
}

ant_value_t js_set_reporting(ant_t *js, ant_value_t target, ant_value_t key, ant_value_t value) {
  sv_vm_t *vm = js->vm;
  int saved_fp = vm->jit.mode_fp;
  bool saved_strict = vm->jit.mode_strict;
  
  vm->jit.mode_fp = vm->fp;
  vm->jit.mode_strict = false;
  bool declined = false;
  
  ant_value_t stored = setprop_impl(js, target, key, value, false, &declined);
  vm->jit.mode_fp = saved_fp;
  vm->jit.mode_strict = saved_strict;
  
  if (is_err(stored)) return stored;
  return Ant_Value_FromBool(!declined);
}

ant_value_t proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  
  if (!data) return Ant_Value_False;
  if (data->revoked) return proxy_revoked_error(js, "has");
  
  if (data->host) return Ant_Value_FromBool(
    lkp_proto(js, data->target, key, key_len).obj ||
    vtype(data->host->get(js, data->target, key, key_len)) != kTypeUndefined
  );
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_value_t has_trap = proxy_get_method(js, handler, "has");
  if (is_err(has_trap)) return has_trap;
  if (vtype(has_trap) != kTypeUndefined) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, has_trap, Ant_Value_Undefined, args, 2, NULL, Ant_Value_Undefined);
      if (is_err(result)) return result;
      return proxy_check_has(js, target, proxy_key_str(key, key_len), result);
  }

  return proxy_has_on_target(js, target, key, key_len);
}

ant_value_t js_proxy_prevent_extensions(ant_t *js, ant_value_t proxy) {
  return proxy_prevent_extensions(js, proxy);
}

ant_value_t js_proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  return proxy_has(js, proxy, key, key_len);
}

ant_value_t proxy_delete(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  
  if (!data) return Ant_Value_True;
  if (data->revoked) return proxy_revoked_error(js, "deleteProperty");
  
  if (data->host) {
    ant_value_t result = js_delete_prop_ordinary(js, data->target, key, key_len);
    if (is_err(result) || !js_truthy(js, result)) return result;
    return data->host->del(js, data->target, key, key_len);
  }
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_value_t delete_trap = proxy_get_method(js, handler, "deleteProperty");
  if (is_err(delete_trap)) return delete_trap;
  if (vtype(delete_trap) != kTypeUndefined) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, delete_trap, Ant_Value_Undefined, args, 2, NULL, Ant_Value_Undefined);
      if (is_err(result)) return result;
      return proxy_check_delete(js, target, proxy_key_str(key, key_len), result);
  }
  
  return js_delete_prop_ordinary(js, target, key, key_len);
}

ant_value_t proxy_get_val(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_Undefined;
  if (data->revoked) return proxy_revoked_error(js, "get");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t prop_key;
  proxy_key_t key;

  ant_value_t key_err = proxy_to_key(js, key_val, &prop_key, &key);
  if (is_err(key_err)) return key_err;
  if (data->host && !key.is_sym) return proxy_host_get(js, data, key.str, key.len);

  ant_value_t get_trap = proxy_get_method(js, handler, "get");
  if (is_err(get_trap)) return get_trap;

  if (vtype(get_trap) == kTypeUndefined) return key.is_sym
    ? js_get_sym_with_receiver(js, target, prop_key, proxy)
    : proxy_get_from_target(js, target, key.str, key.len, proxy);

  ant_value_t args[3] = { target, prop_key, proxy };
  ant_value_t result = sv_vm_call(js->vm, js, get_trap, Ant_Value_Undefined, args, 3, NULL, Ant_Value_Undefined);
  if (is_err(result)) return result;

  return proxy_check_get(js, target, key, result);
}

ant_value_t proxy_get_prototype_of(ant_t *js, ant_value_t proxy) {
  for (uint32_t hops = 0;; hops++) {
    ant_proxy_state_t *data = get_proxy_data(proxy);
    if (!data) return Ant_Value_Null;
    
    if (data->revoked)
      return proxy_revoked_error(js, "getPrototypeOf");

    ant_value_t target = data->target;
    ant_value_t handler = data->handler;
    ant_value_t trap = proxy_get_method(js, handler, "getPrototypeOf");
    if (is_err(trap)) return trap;
    
    if (vtype(trap) == kTypeUndefined) {
      if (!is_proxy(target)) return get_proto(js, target);
      if (hops >= PROXY_PROTO_HOP_LIMIT)
        return js_mkerr_typed(js, JS_ERR_RANGE, "Maximum call stack size exceeded");
      proxy = target;
      continue;
    }
    
    ant_value_t args[1] = { target };
    ant_value_t result = sv_vm_call(js->vm, js, trap, Ant_Value_Undefined, args, 1, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    if (!is_object_type(result) && vtype(result) != kTypeNull)
      return proxy_trap_error(js, "getPrototypeOf", "trap returned neither object nor null");
    if (!proxy_target_is_extensible(target)) {
      ant_value_t target_proto = get_proto(js, target);
      if (!same_object_identity(result, target_proto))
        return proxy_trap_error(js, "getPrototypeOf", "trap returned incompatible prototype for non-extensible target");
    }
    return result;
  }
}

ant_value_t proxy_set_prototype_of(ant_t *js, ant_value_t proxy, ant_value_t proto) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_False;
  if (data->revoked)
    return proxy_revoked_error(js, "setPrototypeOf");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "setPrototypeOf");
  if (is_err(trap)) return trap;

  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[2] = { target, proto };
    ant_value_t result = sv_vm_call(js->vm, js, trap, Ant_Value_Undefined, args, 2, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    if (js_truthy(js, result) && !proxy_target_is_extensible(target)) {
      ant_value_t current = get_proto(js, target);
      if (!same_object_identity(current, proto))
        return proxy_trap_error(js, "setPrototypeOf", "trap returned truthy for incompatible prototype on non-extensible target");
    }
    return Ant_Value_FromBool(js_truthy(js, result));
  }

  set_proto(js, target, proto);
  return Ant_Value_True;
}

ant_value_t proxy_is_extensible(ant_t *js, ant_value_t proxy) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_False;
  if (data->revoked)
    return proxy_revoked_error(js, "isExtensible");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  bool target_extensible = proxy_target_is_extensible(target);
  ant_value_t trap = proxy_get_method(js, handler, "isExtensible");
  if (is_err(trap)) return trap;
  if (vtype(trap) == kTypeUndefined) return Ant_Value_FromBool(target_extensible);

  ant_value_t args[1] = { target };
  ant_value_t result = sv_vm_call(js->vm, js, trap, Ant_Value_Undefined, args, 1, NULL, Ant_Value_Undefined);
  if (is_err(result)) return result;
  bool trap_extensible = js_truthy(js, result);
  if (trap_extensible != target_extensible)
    return proxy_trap_error(js, "isExtensible", "trap result does not reflect target extensibility");
  return Ant_Value_FromBool(trap_extensible);
}

ant_value_t proxy_prevent_extensions(ant_t *js, ant_value_t proxy) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_False;

  if (data->revoked)
    return proxy_revoked_error(js, "preventExtensions");
  
  if (data->host) return Ant_Value_False;

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "preventExtensions");
  if (is_err(trap)) return trap;

  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[1] = { target };
    ant_value_t result = sv_vm_call(js->vm, js, trap, Ant_Value_Undefined, args, 1, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    bool ok = js_truthy(js, result);
    if (ok && proxy_target_is_extensible(target))
      return proxy_trap_error(js, "preventExtensions", "trap returned truthy but target is extensible");
    return Ant_Value_FromBool(ok);
  }

  ant_object_t *ptr = js_obj_ptr(js_as_obj(target));
  if (ptr) ptr->flags.extensible = 0;
  return Ant_Value_True;
}

ant_value_t proxy_has_val(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_False;
  if (data->revoked) return proxy_revoked_error(js, "has");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t prop_key;
  proxy_key_t key;

  ant_value_t key_err = proxy_to_key(js, key_val, &prop_key, &key);
  if (is_err(key_err)) return key_err;
  
  if (data->host && !key.is_sym) return proxy_has(js, proxy, key.str, key.len);

  ant_value_t has_trap = proxy_get_method(js, handler, "has");
  if (is_err(has_trap)) return has_trap;
  if (vtype(has_trap) == kTypeUndefined) return key.is_sym
    ? Ant_Value_FromBool(lkp_sym_proto(js, target, key.sym).obj)
    : proxy_has_on_target(js, target, key.str, key.len);

  ant_value_t args[2] = { target, prop_key };
  ant_value_t result = sv_vm_call(js->vm, js, has_trap, Ant_Value_Undefined, args, 2, NULL, Ant_Value_Undefined);
  if (is_err(result)) return result;

  return proxy_check_has(js, target, key, result);
}

typedef struct {
  bool accessor;
  bool enumerable;
  bool configurable;
  bool writable;
  ant_value_t first;
  ant_value_t second;
} proxy_full_desc_t;

static ant_value_t proxy_to_complete_descriptor(ant_t *js, ant_value_t obj, proxy_full_desc_t *out) {
  define_desc_raw_t raw = {0};
  ant_value_t err = define_desc_read(js, obj, &raw);
  if (is_err(err)) return err;
  
  bool accessor = raw.has[DESC_GET] || raw.has[DESC_SET];
  if (accessor && (raw.has[DESC_VALUE] || raw.has[DESC_WRITABLE])) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute"
  );
  
  *out = (proxy_full_desc_t){
    .accessor = accessor,
    .enumerable = raw.has[DESC_ENUMERABLE] && js_truthy(js, raw.val[DESC_ENUMERABLE]),
    .configurable = raw.has[DESC_CONFIGURABLE] && js_truthy(js, raw.val[DESC_CONFIGURABLE]),
    .writable = raw.has[DESC_WRITABLE] && js_truthy(js, raw.val[DESC_WRITABLE]),
    .first = raw.has[accessor ? DESC_GET : DESC_VALUE] ? raw.val[accessor ? DESC_GET : DESC_VALUE] : Ant_Value_Undefined,
    .second = raw.has[DESC_SET] ? raw.val[DESC_SET] : Ant_Value_Undefined,
  };
  
  return Ant_Value_Undefined;
}

static bool proxy_desc_compatible(ant_t *js, bool extensible, const proxy_full_desc_t *desc, const proxy_full_desc_t *current, bool exists) {
  if (!exists) return extensible;
  if (current->configurable) return true;
  if (desc->configurable || desc->enumerable != current->enumerable) return false;
  if (desc->accessor != current->accessor) return false;
  
  if (current->accessor) return
    same_value_values(js, desc->first, current->first) &&
    same_value_values(js, desc->second, current->second);
    
  if (current->writable) return true;
  return !desc->writable && same_value_values(js, desc->first, current->first);
}

ant_value_t proxy_get_own_property_descriptor(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_Undefined;
  
  if (data->revoked)
    return proxy_revoked_error(js, "getOwnPropertyDescriptor");
  
  if (data->host && vtype(key_val) == kTypeString) return proxy_host_desc(js, data, key_val);

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "getOwnPropertyDescriptor");
  
  if (is_err(trap)) return trap;
  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[2] = { target, key_val };
    ant_value_t result = sv_vm_call(js->vm, js, trap, Ant_Value_Undefined, args, 2, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    if (vtype(result) != kTypeUndefined && vtype(result) != kTypeObject)
      return proxy_trap_error(js, "getOwnPropertyDescriptor", "trap returned neither object nor undefined");

    ant_value_t target_args[2] = { target, key_val };
    ant_value_t target_desc = builtin_object_getOwnPropertyDescriptor(js, target_args, 2, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
    if (is_err(target_desc)) return target_desc;
    bool target_exists = vtype(target_desc) != kTypeUndefined;
    
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, result);
    GC_ROOT_PIN(js, target_desc);
    
    proxy_full_desc_t current = {0};
    if (target_exists) {
      ant_value_t read = proxy_to_complete_descriptor(js, target_desc, &current);
      if (is_err(read)) { GC_ROOT_RESTORE(js, mark); return read; }
      GC_ROOT_PIN(js, current.first);
      GC_ROOT_PIN(js, current.second);
    }
    bool target_extensible = proxy_target_is_extensible(target);

    if (vtype(result) == kTypeUndefined) {
      GC_ROOT_RESTORE(js, mark);
      if (target_exists && (!current.configurable || !target_extensible))
        return proxy_trap_error(js, "getOwnPropertyDescriptor", "trap reported existing property as undefined");
      return result;
    }

    proxy_full_desc_t desc = {0};
    ant_value_t read = proxy_to_complete_descriptor(js, result, &desc);
    
    if (is_err(read)) { 
      GC_ROOT_RESTORE(js, mark); 
      return read;
    }
    
    GC_ROOT_PIN(js, desc.first);
    GC_ROOT_PIN(js, desc.second);
    
    const char *invalid = NULL;
    if (!proxy_desc_compatible(js, target_extensible, &desc, &current, target_exists))
      invalid = "trap returned descriptor incompatible with the target property";
    else if (!desc.configurable && (!target_exists || current.configurable))
      invalid = "trap reported non-configurability for a configurable or missing property";
    else if (!desc.configurable && !desc.accessor && !desc.writable && target_exists && current.writable)
      invalid = "trap reported non-configurable and writable property as non-writable";
    
    ant_value_t completed = invalid
      ? proxy_trap_error(js, "getOwnPropertyDescriptor", invalid)
      : desc.accessor
      ? js_from_accessor_descriptor(js, desc.first, desc.second, desc.enumerable, desc.configurable)
      : js_from_data_descriptor(js, desc.first, desc.writable, desc.enumerable, desc.configurable);
    GC_ROOT_RESTORE(js, mark);
    
    return completed;
  }

  ant_value_t args[2] = { target, key_val };
  return builtin_object_getOwnPropertyDescriptor(js, args, 2, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
}

ant_value_t proxy_has_own(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_value_t desc = proxy_get_own_property_descriptor(js, proxy, key_val);
  if (is_err(desc)) return desc;
  return Ant_Value_FromBool(vtype(desc) != kTypeUndefined);
}

static ant_value_t proxy_host_define(
  ant_t *js, ant_proxy_state_t *data, const char *key, size_t key_len, ant_value_t descriptor
) {
  bool plain_data =
    descriptor_has_own_key(js, descriptor, "value", 5) &&
    descriptor_bool_value(js, descriptor, "configurable", 12, false) &&
    descriptor_bool_value(js, descriptor, "writable", 8, false) &&
    descriptor_bool_value(js, descriptor, "enumerable", 10, false);
  
  if (!plain_data) return js_mkerr_typed(
    js, JS_ERR_TYPE, 
    "'%s' only accepts a configurable, writable, and enumerable data descriptor", data->host->name
  );
  
  ant_value_t value = js_get(js, descriptor, "value");
  if (is_err(value)) return value;
  
  return data->host->set(js, data->target, key, key_len, value);
}

ant_value_t proxy_define_property(ant_t *js, ant_value_t proxy, ant_value_t key_val, ant_value_t descriptor) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_False;
  
  if (data->revoked)
    return proxy_revoked_error(js, "defineProperty");

  ant_value_t target = data->target;
  if (data->host && vtype(key_val) == kTypeSymbol) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", "Cannot convert a Symbol value to a string");
  if (data->host && vtype(key_val) == kTypeString) {
    size_t key_len;
    const char *key = js_getstr(js, key_val, &key_len);
    if (key && !lkp(js, target, key, key_len).obj) return proxy_host_define(js, data, key, key_len, descriptor);
  }
  
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "defineProperty");
  if (is_err(trap)) return trap;
  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[3] = { target, key_val, descriptor };
    ant_value_t result = sv_vm_call(js->vm, js, trap, Ant_Value_Undefined, args, 3, NULL, Ant_Value_Undefined);
    if (is_err(result) || !js_truthy(js, result)) return result;

    ant_value_t target_args[2] = { target, key_val };
    ant_value_t target_desc = builtin_object_getOwnPropertyDescriptor(js, target_args, 2, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
    if (is_err(target_desc)) return target_desc;

    bool target_exists = vtype(target_desc) != kTypeUndefined;
    bool target_configurable = target_exists
      ? descriptor_bool_value(js, target_desc, "configurable", 12, false)
      : true;
    bool target_extensible = proxy_target_is_extensible(target);
    bool desc_has_configurable = descriptor_has_own_key(js, descriptor, "configurable", 12);
    bool desc_configurable = descriptor_bool_value(js, descriptor, "configurable", 12, false);

    if (!target_exists && !target_extensible)
      return proxy_trap_error(js, "defineProperty", "trap returned truthy for adding property to non-extensible target");
    if (desc_has_configurable && !desc_configurable && (!target_exists || target_configurable))
      return proxy_trap_error(js, "defineProperty", "trap returned truthy for defining invalid non-configurable property");

    return Ant_Value_True;
  }

  ant_value_t result = object_define_property(js, target, key_val, descriptor);
  if (is_err(result)) return result;
  
  return Ant_Value_True;
}

ant_value_t proxy_delete_sym(ant_t *js, ant_value_t proxy, ant_value_t sym) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return Ant_Value_True;
  if (data->revoked) return proxy_revoked_error(js, "deleteProperty");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;

  ant_value_t delete_trap = proxy_get_method(js, handler, "deleteProperty");
  if (is_err(delete_trap)) return delete_trap;
  
  if (vtype(delete_trap) != kTypeUndefined) {
    ant_value_t args[2] = { target, sym };
    ant_value_t result = sv_vm_call(js->vm, js, delete_trap, Ant_Value_Undefined, args, 2, NULL, Ant_Value_Undefined);
    if (is_err(result)) return result;
    return proxy_check_delete(js, target, proxy_key_sym(sym), result);
  }
  
  return js_delete_sym_prop_ordinary(js, target, sym);
}

ant_value_t js_proxy_apply(ant_t *js, ant_value_t proxy, ant_value_t this_arg, ant_value_t *args, int argc) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "object is not a function");
  if (data->revoked) return proxy_revoked_error(js, "apply");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  uint8_t target_type = vtype(target);

  if (target_type != kTypeFunction && target_type != kTypeBuiltin && !(target_type == kTypeObject && is_proxy(target)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(target_type));

  ant_value_t trap = proxy_get_method(js, handler, "apply");
  if (is_err(trap)) return trap;
  if (vtype(trap) != kTypeUndefined) {
      ant_value_t args_arr = Ant_Array_Create(js);
      for (int i = 0; i < argc; i++)
        js_arr_push(js, args_arr, args[i]);
      ant_value_t trap_args[3] = { target, this_arg, args_arr };
      return sv_vm_call(js->vm, js, trap, handler, trap_args, 3, NULL, Ant_Value_Undefined);
  }

  return sv_vm_call(js->vm, js, target, this_arg, args, argc, NULL, Ant_Value_Undefined);
}

ant_value_t js_proxy_construct(ant_t *js, ant_value_t proxy, ant_value_t *args, int argc, ant_value_t new_target) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  if (data->revoked) return proxy_revoked_error(js, "construct");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;

  if (!js_is_constructor(target))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  
  ant_value_t trap = proxy_get_method(js, handler, "construct");
  if (is_err(trap)) return trap;
  
  if (vtype(trap) != kTypeUndefined) {
      ant_value_t args_arr = Ant_Array_Create(js);
      for (int i = 0; i < argc; i++)
        js_arr_push(js, args_arr, args[i]);
      ant_value_t trap_args[3] = { target, args_arr, new_target };
      ant_value_t result = sv_vm_call(js->vm, js, trap, Ant_Value_Undefined, trap_args, 3, NULL, Ant_Value_Undefined);
      if (is_err(result)) return result;
      if (!is_object_type(result))
        return proxy_trap_error(js, "construct", "trap returned non-Object");
      return result;
  }

  if (vtype(target) == kTypeObject && is_proxy(target))
    return js_proxy_construct(js, target, args, argc, new_target);

  ant_value_t effective_new_target = new_target;
  ant_value_t proto;
  
  if (new_target != target) {
    proto = js_getprop_fallback(js, new_target, "prototype");
    if (!is_err(proto) && !is_object_type(proto)) proto = js->sym.object_proto;
  } else proto = sv_prepare_construct_meta(js, target, new_target, &effective_new_target);
  if (is_err(proto)) return proto;

  ant_value_t obj = mkobj(js, 0);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(
    js->vm, js, target, obj, args, 
    argc, &ctor_this, effective_new_target
  );
  
  if (is_err(result)) return result;
  
  return is_object_type(result)
    ? result
    : (is_object_type(ctor_this) ? ctor_this : obj);
}

static ant_value_t mkproxy(ant_t *js, ant_value_t target, ant_value_t handler) {
  ant_value_t proxy_obj = mkobj(js, 0);
  ant_object_t *proxy_ptr = js_obj_ptr(proxy_obj);
  if (!proxy_ptr) return Ant_Error_CreateOutOfMemory(js);

  ant_proxy_state_t *data = (ant_proxy_state_t *)calloc(1, sizeof(ant_proxy_state_t));
  if (!data) return Ant_Error_CreateOutOfMemory(js);

  data->target = target;
  data->handler = handler;
  data->revoked = false;

  proxy_ptr->flags.is_exotic = 1;
  proxy_ptr->flags.is_callable = is_callable(target) ? 1u : 0u;
  js_mark_constructor(proxy_obj, js_is_constructor(target));
  
  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(proxy_ptr);
  if (!sidecar) {
    free(data);
    return Ant_Error_CreateOutOfMemory(js);
  }
  
  sidecar->proxy_state = data;
  return proxy_obj;
}

ant_value_t js_mkproxy(ant_t *js, ant_value_t target, ant_value_t handler) {
  return mkproxy(js, target, handler);
}

ant_value_t js_mkproxy_host(ant_t *js, ant_value_t target, const ant_host_proxy_ops_t *ops) {
  Ant_Assert(ops && ops->name && ops->get && ops->set && ops->del && ops->keys, "host proxies need every hook");
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, target);
  
  ant_value_t handler = mkobj(js, 0);
  GC_ROOT_PIN(js, handler);
  
  ant_value_t proxy = mkproxy(js, target, handler);
  if (!is_err(proxy)) get_proxy_data(proxy)->host = ops;
  
  GC_ROOT_RESTORE(js, root_mark);
  return proxy;
}

static ant_value_t create_proxy_checked(ant_native_params_t, ant_value_t new_target, bool require_new) {
  if (require_new && vtype(new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Proxy constructor requires 'new'");
  }
  if (nargs < 2) return js_mkerr(js, "Proxy requires two arguments: target and handler");

  ant_value_t target = args[0];
  ant_value_t handler = args[1];

  uint8_t target_type = vtype(target);
  if (target_type != kTypeObject && target_type != kTypeFunction && target_type != kTypeArray) {
    return js_mkerr(js, "Proxy target must be an object");
  }

  uint8_t handler_type = vtype(handler);
  if (handler_type != kTypeObject && handler_type != kTypeFunction) {
    return js_mkerr(js, "Proxy handler must be an object");
  }

  return mkproxy(js, target, handler);
}

static ant_value_t builtin_Proxy(ant_params_t) {
  return create_proxy_checked(js, args, nargs, call_new_target, true);
}

static ant_value_t proxy_revoke_fn(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t ref_slot = get_slot(call_func, SLOT_PROXY_REF);
  
  if (vtype(ref_slot) != kTypeUndefined && vdata(ref_slot) != 0) {
    ant_proxy_state_t *data = get_proxy_data(ref_slot);
    if (data) data->revoked = true;
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t builtin_Proxy_revocable(ant_params_t) {
  ant_value_t proxy = create_proxy_checked(js, args, nargs, Ant_Value_Undefined, false);
  if (is_err(proxy)) return proxy;
  
  ant_value_t revoke_obj = mkobj(js, 0);
  set_slot(revoke_obj, SLOT_CFUNC, Ant_Function_Create(proxy_revoke_fn));
  set_slot(revoke_obj, SLOT_PROXY_REF, proxy);
  
  ant_value_t revoke_func = js_obj_to_func(js, revoke_obj);
  ant_value_t result = Ant_Object_Create(js);
  
  js_setprop(js, result, js_mkstr(js, "proxy", 5), proxy);
  js_setprop(js, result, js_mkstr(js, "revoke", 6), revoke_func);
  
  return result;
}

void js_proxy_init_constructor(ant_t *js, ant_value_t proxy_ctor_obj) {
  set_slot(proxy_ctor_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Proxy));
  js_define_function_length(js, proxy_ctor_obj, 2);
  js_mark_constructor(proxy_ctor_obj, true);
  defmethod(js, proxy_ctor_obj, "revocable", 9, Ant_Function_CreateWithArity(builtin_Proxy_revocable, 2));
  js_set_function_name(js, proxy_ctor_obj, "Proxy", sizeof("Proxy") - 1);
}
