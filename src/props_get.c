#include <compat.h> // IWYU pragma: keep
#include <string.h>

#include "utf8.h"
#include "cfunc.h"
#include "arrays.h"
#include "convert.h"
#include "props.h"
#include "proto.h"
#include "stringify.h"
#include "protectors.h"
#include "esm/import.h"
#include "silver/call.h"
#include "arguments.h"
#include "modules/proxy.h"

const char *index_key_find(ant_t *js, uint32_t idx, char *buf, size_t *len) {
  if (idx < INTERN_INDEX_CACHE_SIZE && js->intern.idx[idx]) return js->intern.idx[idx];
  *len = uint_to_str(buf, 16, idx);
  const char *interned = intern_find(js, buf, *len);
  if (interned && idx < INTERN_INDEX_CACHE_SIZE) js->intern.idx[idx] = interned;
  return interned;
}

const char *index_key_intern(ant_t *js, uint32_t idx, char *buf, size_t *len) {
  if (!*len) *len = uint_to_str(buf, 16, idx);
  const char *interned = intern_string(js, buf, *len);
  if (interned && idx < INTERN_INDEX_CACHE_SIZE) js->intern.idx[idx] = interned;
  return interned;
}

const char *index_key_get_or_intern(ant_t *js, uint32_t idx) {
  if (idx < INTERN_INDEX_CACHE_SIZE && js->intern.idx[idx]) return js->intern.idx[idx];
  char buf[16];
  const char *interned = intern_string(js, buf, uint_to_str(buf, sizeof(buf), idx));
  if (interned && idx < INTERN_INDEX_CACHE_SIZE) js->intern.idx[idx] = interned;
  return interned;
}

bool index_keys_absent_from_protos(const ant_object_t *ptr) {
  for (ant_value_t proto = ant_object_proto(ptr); is_object_type(proto);) {
    const ant_object_t *p = js_obj_ptr(proto);
    if (!p || p->flags.is_exotic || p->type_tag == kTypeArray || ant_shape_may_have_index_keys(p->shape)) return false;
    proto = ant_object_proto(p);
  }
  return true;
}

bool js_get_plain_index(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t *out) {
  ant_object_t *ptr = vtype(obj) == kTypeObject ? js_obj_ptr(obj) : NULL;
  if (!ptr) return false;
  
  if (ptr->flags.is_exotic) return buffer_typedarray_get_index(js, obj, idx, out);
  if (!ptr->shape) return false;
  
  ant_value_t str = ptr->extra_slots 
    ? js_string_wrapper_value(obj) 
    : Ant_Value_Undefined;
  
  if (vtype(str) == kTypeString) {
    ant_offset_t byte_len = 0;
    const char *data = (const char *)(uintptr_t)vstr(js, str, &byte_len);
    uint32_t code_unit = utf16_code_unit_at(data, (size_t)byte_len, idx);
    
    if (code_unit != 0xFFFFFFFF) {
      *out = js_string_from_utf16_code_unit(js, code_unit);
      return true;
    }
  }
  
  char key[16];
  size_t key_len = 0;
  
  const char *interned = index_key_find(js, idx, key, &key_len);
  int32_t slot = interned ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
  
  if (slot >= 0) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    if (!prop || prop->has_getter || prop->has_setter) return false;
    
    *out = ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
    return true;
  }
  
  if (!index_keys_absent_from_protos(ptr)) return false;
  *out = Ant_Value_Undefined;
  
  return true;
}

bool js_string_past_end_is_undefined(ant_t *js, ant_value_t str) {
  const ant_object_t *ptr = js_obj_ptr(vtype(str) == kTypeString ? js->sym.string_proto : str);
  return ptr && !ptr->flags.is_exotic && !ant_shape_may_have_index_keys(ptr->shape) && index_keys_absent_from_protos(ptr);
}

ant_value_t js_get_index_fast(ant_t *js, ant_value_t obj, ant_value_t key) {
  if (vtype(key) != kTypeNumber) return T_EMPTY;
  
  double d = Ant_Value_AsNumberUnchecked(key);
  if (!(d >= 0 && d < (double)UINT32_MAX)) return T_EMPTY;
  
  uint32_t idx = (uint32_t)d;
  if (d != (double)idx) return T_EMPTY;

  if (vtype(obj) == kTypeString) {
    ant_offset_t byte_len = 0;
    const char *data = (const char *)(uintptr_t)vstr(js, obj, &byte_len);
    uint32_t code_unit = utf16_code_unit_at(data, (size_t)byte_len, idx);
    if (code_unit != 0xFFFFFFFF) return js_string_from_utf16_code_unit(js, code_unit);
    return js_string_past_end_is_undefined(js, obj) ? Ant_Value_Undefined : T_EMPTY;
  }

  ant_object_t *ptr = vtype(obj) == kTypeObject ? (ant_object_t *)vptr(obj) : NULL;
  if (!ptr || ptr->flags.is_exotic || !ptr->shape) return T_EMPTY;
  
  const char *interned = idx < INTERN_INDEX_CACHE_SIZE ? js->intern.idx[idx] : NULL;
  int32_t slot = interned ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
  
  if (slot >= 0) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    if (!prop || prop->has_getter || prop->has_setter) return T_EMPTY;
    return ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
  }

  ant_value_t out;
  return js_get_plain_index(js, obj, idx, &out) ? out : T_EMPTY;
}

size_t utf16_code_unit_to_wtf8(uint16_t code_unit, char out[4]) {
  if (code_unit >= 0xD800 && code_unit <= 0xDFFF) {
    out[0] = (char)(0xE0 | (code_unit >> 12));
    out[1] = (char)(0x80 | ((code_unit >> 6) & 0x3F));
    out[2] = (char)(0x80 | (code_unit & 0x3F));
    return 3;
  }
  return (size_t)utf8_encode(code_unit, out);
}

ant_value_t js_mk_ascii_char_cached(ant_t *js, unsigned char ch) {
  ant_value_t cached = js->ascii_chars[ch];
  if (cached != 0) return cached;

  char byte = (char)ch;
  cached = js_mkstr_permanent(js, &byte, 1);
  if (!is_err(cached)) js->ascii_chars[ch] = cached;
  return cached;
}

ant_value_t js_string_from_utf16_code_unit(ant_t *js, uint32_t code_unit) {
  if (code_unit < 128) return js_mk_ascii_char_cached(js, (unsigned char)code_unit);
  char buf[4];
  size_t out_len = utf16_code_unit_to_wtf8((uint16_t)code_unit, buf);
  return js_mkstr(js, buf, out_len);
}

static bool js_try_get_string_index(
  ant_t *js, ant_value_t str,
  const char *key, size_t key_len, ant_value_t *out
) {
  if (!is_array_index(key, (ant_offset_t)key_len)) return false;

  unsigned long idx = 0;
  ant_offset_t byte_len = 0;
  ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  ant_offset_t str_len = str_utf16_len(js, str);
  if (!parse_array_index(key, key_len, str_len, &idx)) return false;

  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, (ant_offset_t)idx);
  if (code_unit == 0xFFFFFFFF) return false;

  *out = js_string_from_utf16_code_unit(js, code_unit);
  return true;
}

bool js_try_get_string_own_exotic(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out, bool *is_length
) {
  if (is_length) *is_length = false;
  
  bool length_key = is_length_key(key, key_len);
  if (!length_key && !is_array_index(key, (ant_offset_t)key_len)) return false;

  ant_value_t str = vtype(obj) == kTypeObject ? js_string_wrapper_value(obj) : obj;
  if (vtype(str) != kTypeString) return false;

  if (length_key) {
    *out = Ant_Value_FromNumber((double)str_utf16_len(js, str));
    if (is_length) *is_length = true;
    return true;
  }

  return js_try_get_string_index(js, str, key, key_len, out);
}

static ant_value_t try_dynamic_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  return ptr ? exotic_own_get(js, obj, ptr, key, key_len) : Ant_Value_Undefined;
}

bool try_dynamic_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->flags.is_exotic) return false;
  if (!ant_object_exotic_ops(ptr) || !ant_object_exotic_ops(ptr)->setter) return false;
  return ant_object_exotic_ops(ptr)->setter(js, obj, key, key_len, value);
}

bool exotic_delete_allowed(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->flags.is_exotic || !ant_object_exotic_ops(ptr) || !ant_object_exotic_ops(ptr)->deleter) return true;
  return ant_object_exotic_ops(ptr)->deleter(js, obj, key, key_len);
}

bool try_accessor_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  ant_value_t getter = Ant_Value_Undefined;
  bool has_getter = false;
  lkp_with_getter(js, obj, key, key_len, &getter, &has_getter);

  ant_value_t result = call_proto_accessor(js, obj, getter, has_getter, NULL, 0, false);
  if (!has_getter && vtype(result) == kTypeUndefined) return false;

  *out = result;
  return true;
}

static __attribute__((noinline)) ant_value_t js_get_cached_miss(
  ant_t *js, ant_value_t obj, ant_object_t *ptr,
  const char *key, size_t key_len, ant_get_cache_t *cache
) {
  ant_value_t value = js_getprop_fallback_len(js, obj, key, key_len);
  if (!ptr || !ptr->shape || ptr->flags.is_exotic || is_err(value)) return value;

  const char *interned = intern_find(js, key, key_len);
  if (!interned) return value;

  ant_object_t *holder = ptr;
  ant_prop_loc_t loc = lkp_interned(obj, interned);
  
  for (int depth = 0; !loc.obj; depth++) {
    ant_value_t next = ant_object_proto(holder);
    if (depth >= MAX_PROTO_CHAIN_DEPTH || !is_object_type(next) || is_proxy(next)) return value;
    holder = js_obj_ptr(js_as_obj(next));
    if (!holder || holder->flags.is_exotic || !holder->shape) return value;
    loc = lkp_interned(js_as_obj(next), interned);
  }

  const ant_shape_prop_t *meta = loc.obj == holder ? prop_shape_meta(loc) : NULL;
  if (!meta || meta->has_getter || meta->has_setter) return value;

  ant_shape_retain(ptr->shape);
  if (cache->shape) ant_shape_release(cache->shape);
  
  cache->shape = ptr->shape;
  cache->proto = ant_object_proto(ptr);
  cache->holder = holder == ptr ? NULL : holder;
  cache->key = interned;
  cache->slot = loc.slot;
  cache->ic_epoch = js->ic.epoch;

  return value;
}

ant_value_t js_get_cached(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_get_cache_t *cache
) {
  ant_object_t *ptr = vtype(obj) == kTypeObject ? js_obj_ptr(obj) : NULL;
  
  if (
    ptr && ptr->shape && cache->shape == ptr->shape &&
    cache->proto == ant_object_proto(ptr) && cache->ic_epoch == js->ic.epoch
  ) {
    ant_object_t *src = cache->holder ? cache->holder : ptr;
    const ant_shape_prop_t *prop = src->shape && cache->slot < src->prop_count
      ? ant_shape_prop_at(src->shape, cache->slot) : NULL;
    if (
      prop && prop->type == ANT_SHAPE_KEY_STRING && prop->key.interned == cache->key &&
      !prop->has_getter && !prop->has_setter
    ) return ant_object_prop_get_unchecked(src, cache->slot);
  }

  return js_get_cached_miss(js, obj, ptr, key, key_len, cache);
}

void js_get_cache_release(ant_get_cache_t *cache) {
  if (cache->shape) ant_shape_release(cache->shape);
  *cache = (ant_get_cache_t){0};
}

ant_value_t js_get_then_property(ant_t *js, ant_value_t value) {
  uint8_t type = vtype(value);
  ant_object_t *obj = type == kTypeObject || type == kTypeArray ? js_obj_ptr(js_as_obj(value)) : NULL;
  if (!obj || obj->flags.is_exotic) return js_getprop_fallback(js, value, "then");
  
  if (type == kTypeObject && obj->shape && ant_object_proto(obj) == js->sym.object_proto && ant_protector_intact(js, ANT_PROTECTOR_OBJECT_THEN)) {
    ant_object_t *object_proto = js_obj_ptr(js->sym.object_proto);
    if (object_proto && !is_object_type(ant_object_proto(object_proto)) && ant_shape_lookup_interned(obj->shape, js->intern.then) < 0)
      return Ant_Value_Undefined;
  }
  
  ant_absent_key_cache_t *cache = &js->runtime_cache.then_absent[((uintptr_t)obj->shape >> 6) & 7];
  if (cache->ic_epoch == js->ic.epoch && cache->shape == obj->shape && cache->proto == ant_object_proto(obj) && obj->shape)
    return Ant_Value_Undefined;
  
  ant_value_t then = js_getprop_fallback(js, value, "then");
  if (!is_undefined(then) || !obj->shape || lkp_proto(js, value, "then", 4).obj) return then;
  if (!js_guard_proto_chain_absence(js, obj)) return then;
  
  ant_shape_retain(obj->shape);
  if (cache->shape) ant_shape_release(cache->shape);
  cache->shape = obj->shape;
  cache->proto = ant_object_proto(obj);
  cache->ic_epoch = js->ic.epoch;
  
  return then;
}

bool js_try_get_sym_with_receiver(
  ant_t *js, ant_value_t obj, ant_value_t sym,
  ant_value_t receiver, ant_value_t *out
) {
  if (vtype(sym) != kTypeSymbol) return false;
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction)
      return js_try_get_sym_with_receiver(js, promoted, sym, receiver, out);
  }

  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);
  uint8_t ot = vtype(obj);
  
  if (!is_object_type(obj)) {
    if (ot == kTypeString || ot == kTypeNumber || ot == kTypeBool || ot == kTypeBigInt || ot == kTypeSymbol || ot == kTypeBuiltin) {
      ant_value_t proto = js_primitive_prototype(js, ot);
      if (!is_object_type(proto)) return false;
      obj = js_as_obj(proto);
    } else return false;
  } else obj = js_as_obj(obj);

  if (is_proxy(obj)) {
    *out = proxy_get_val(js, obj, sym);
    return true;
  }

  ant_value_t cur = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  
  while (is_object_type(cur)) {
    ant_value_t cur_obj = js_as_obj(cur);
    ant_prop_loc_t off = lkp_sym(cur_obj, sym_off);
    if (off.obj) {
      const ant_shape_prop_t *meta = prop_shape_meta(off);
      if (meta && meta->has_getter) {
        *out = is_callable(meta->getter) ? js_call_getter(js, receiver, meta->getter) : Ant_Value_Undefined;
        return true;
      }
      *out = (meta && meta->has_setter) ? Ant_Value_Undefined : js_prop_load(off);
      return true;
    }

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    
    cur = js_as_obj(proto);
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }

  return false;
}

ant_value_t js_get_sym_with_receiver(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t receiver) {
  ant_value_t result;
  return js_try_get_sym_with_receiver(js, obj, sym, receiver, &result) ? result : Ant_Value_Undefined;
}

ant_value_t js_get_sym(ant_t *js, ant_value_t obj, ant_value_t sym) {
  return js_get_sym_with_receiver(js, obj, sym, obj);
}

ant_value_t js_get_symbol(ant_t *js, ant_value_t obj, const char *key) {
  ant_value_t sym = js_mksym_for(js, key);
  if (is_err(sym)) return sym;
  return js_get_sym(js, obj, sym);
}

static bool js_try_get_len(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction && js_try_get_len(js, promoted, key, key_len, out)) return true;
    if (js_cfunc_try_get_own(js, obj, key, key_len, out)) return true;
    ant_prop_loc_t off = lkp_proto(js, obj, key, key_len);
    if (off.obj) {
      *out = js_prop_load_from(js, obj, off);
      return true;
    }
    return false;
  }
  
  if (vtype(obj) == kTypeFunction) {
    if (sv_vm_is_strict(js->vm) &&
        ((key_len == 6 && memcmp(key, "caller", 6) == 0) ||
         (key_len == 9 && memcmp(key, "arguments", 9) == 0))) {
      *out = js_mkerr_typed(
        js, JS_ERR_TYPE,
        "'%.*s' not allowed on functions in strict mode",
        (int)key_len, key
      );
      return true;
    }

    ant_value_t func_obj = js_func_obj(obj);
    if (key_len == 4 && memcmp(key, "meta", 4) == 0 &&
      js_try_import_meta(js, func_obj, out)) return true;

    ant_prop_loc_t off = lkp(js, func_obj, key, key_len);
    if (!off.obj) {
      ant_value_t accessor_result;
      if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
        *out = accessor_result;
        return true;
      }
      off = lkp_proto(js, obj, key, key_len);
      if (off.obj) {
        *out = js_prop_load_from(js, obj, off);
        return true;
      }
      return false;
    }

    *out = js_prop_load_from(js, obj, off);
    return true;
  }
  
  if (array_obj_ptr(obj)) {
    if (real_array_obj_ptr(obj) && is_length_key(key, key_len)) {
      *out = Ant_Value_FromNumber((double)get_array_length(js, obj));
      return true;
    }
    
    unsigned long idx;
    ant_offset_t arr_len = get_array_length(js, obj);
    
    if (parse_array_index(key, key_len, arr_len, &idx)) {
      if (js_arguments_state(obj)) {
        *out = js_arguments_getter(js, obj, key, key_len);
        return true;
      }
      if (arr_has(js, obj, (ant_offset_t)idx)) {
        *out = arr_get(js, obj, (ant_offset_t)idx);
        return true;
      } 
      
      return false;
    }
    
    ant_value_t arr_obj = js_as_obj(obj);
    ant_prop_loc_t off = lkp(js, arr_obj, key, key_len);
    
    if (!off.obj) {
      ant_value_t accessor_result;
      if (try_accessor_getter(js, arr_obj, key, key_len, &accessor_result)) {
        *out = accessor_result; return true;
      }
      
      return false;
    }
    
    *out = js_prop_load_from(js, obj, off);
    return true;
  }

  uint8_t t = vtype(obj);
  bool is_promise = (t == kTypePromise);
  if (t == kTypeObject && is_proxy(obj)) {
    *out = proxy_get(js, obj, key, key_len);
    return true;
  }
  
  if (is_boxable_primitive_type(t)) {
    if (t == kTypeString && is_length_key(key, key_len)) {
      *out = Ant_Value_FromNumber((double)str_utf16_len(js, obj));
      return true;
    }
    
    if (t == kTypeString && js_try_get_string_index(js, obj, key, key_len, out)) return true;

    ant_value_t prim_proto = js_primitive_prototype(js, t);
    if (is_object_type(prim_proto)) {
      ant_prop_loc_t poff = lkp_proto(js, prim_proto, key, key_len);
      if (poff.obj) {
        *out = js_prop_load_from(js, obj, poff);
        return true;
      }
    }

    return false;
  }
  
  if (is_promise || t == kTypeGenerator) obj = js_as_obj(obj);
  else if (t != kTypeObject) return false;
  ant_prop_loc_t off = lkp(js, obj, key, key_len);
  
  if (!off.obj && t == kTypeObject && js_try_get_string_own_exotic(js, obj, key, key_len, out, NULL)) return true;
  if (!off.obj) {
    ant_value_t result = try_dynamic_getter(js, obj, key, key_len);
    if (result == T_EMPTY) { *out = Ant_Value_Undefined; return true; }
    if (vtype(result) != kTypeUndefined) { *out = result; return true; }
  }
  
  if (!off.obj) {
    off = lkp_proto(js, obj, key, key_len);
    if (off.obj) {
      *out = js_prop_load_from(js, obj, off);
      return true;
    }
  }
  
  if (!off.obj) {
    ant_value_t accessor_result;
    if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
      *out = accessor_result; return true;
    }
    return false;
  }
  
  *out = js_prop_load_from(js, obj, off);
  return true;
}

ant_value_t js_get(ant_t *js, ant_value_t obj, const char *key) {
  return js_getprop_fallback_len(js, obj, key, strlen(key));
}

ant_value_t js_getprop_proto(ant_t *js, ant_value_t obj, const char *key) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) obj = promoted;
  }
  size_t key_len = strlen(key);
  ant_prop_loc_t off = lkp_proto(js, obj, key, key_len);
  return !off.obj ? Ant_Value_Undefined : js_prop_load(off);
}

static bool js_try_get(ant_t *js, ant_value_t obj, const char *key, ant_value_t *out) {
  return js_try_get_len(js, obj, key, strlen(key), out);
}

ant_value_t js_getprop_fallback_len(ant_t *js, ant_value_t obj, const char *name, size_t key_len) {
  ant_value_t val;
  if (js_try_get_len(js, obj, name, key_len, &val)) return val;

  ant_value_t cur = obj;
  uint8_t t = vtype(cur);
  const char *interned = intern_find(js, name, key_len);
  proto_walk_overflow_guard_t guard;
  proto_walk_overflow_guard_init(&guard);

  while (proto_walk_next(js, &cur, &t, PROTO_WALK_F_LOOKUP)) {
    if (is_object_type(cur)) {
      ant_value_t cur_obj = js_as_obj(cur);
      if (is_proxy(cur_obj))
        return proxy_get_with_receiver(js, cur_obj, name, key_len, obj);
      ant_prop_loc_t off = interned ? lkp_interned(cur_obj, interned) : ANT_PROP_LOC_NONE;
      if (off.obj) return js_prop_load_from(js, obj, off);
      ant_value_t element;
      if (array_own_value(js, cur_obj, name, key_len, &element)) return element;
      ant_value_t exotic = exotic_own_get(js, cur_obj, (const ant_object_t *)vptr(cur_obj), name, key_len);
      if (vtype(exotic) != kTypeUndefined) return exotic == T_EMPTY ? Ant_Value_Undefined : exotic;
    } else if (t == kTypeBuiltin) {
      ant_value_t promoted = js_cfunc_lookup_promoted(js, cur);
      if (interned && vtype(promoted) == kTypeFunction) {
        ant_prop_loc_t off = lkp_interned(js_func_obj(promoted), interned);
        if (off.obj) return js_prop_load_from(js, obj, off);
      }
    }
    if (proto_walk_overflow_guard_hit_cycle(js, &guard, cur, t, PROTO_WALK_F_LOOKUP)) break;
  }

  return Ant_Value_Undefined;
}

ant_value_t js_getprop_fallback(ant_t *js, ant_value_t obj, const char *name) {
  return js_getprop_fallback_len(js, obj, name, strlen(name));
}

// TODO: cleanup
ant_value_t js_getprop_super(ant_t *js, ant_value_t super_obj, ant_value_t receiver, const char *name) {
  if (!name) return Ant_Value_Undefined;

  if (vtype(super_obj) == kTypeArray && (is_length_key(name, strlen(name)) || (name[0] >= '0' && name[0] <= '9'))) {
    ant_value_t own_value;
    if (js_try_get(js, super_obj, name, &own_value)) return own_value;
  }

  if (vtype(super_obj) == kTypeFunction) super_obj = js_func_obj(super_obj);
  if (!is_object_type(super_obj)) return Ant_Value_Undefined;

  size_t key_len = strlen(name);
  if (is_proxy(super_obj)) return proxy_get(js, super_obj, name, key_len);

  const char *key_intern = intern_string(js, name, key_len);
  if (!key_intern) return Ant_Value_Undefined;

  ant_value_t cur = super_obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  while (is_object_type(cur)) {
    ant_value_t cur_obj = js_as_obj(cur);
    ant_object_t *cur_ptr = js_obj_ptr(cur_obj);
    bool handled = false;

    if (cur_ptr && cur_ptr->shape) {
      int32_t slot = ant_shape_lookup_interned(cur_ptr->shape, key_intern);
      if (slot >= 0) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(cur_ptr->shape, (uint32_t)slot);
        if (prop && prop->has_getter) {
          ant_value_t getter = prop->getter;
          if (vtype(getter) == kTypeFunction || vtype(getter) == kTypeBuiltin)
            return sv_vm_call(js->vm, js, getter, receiver, NULL, 0, NULL, Ant_Value_Undefined);
          return Ant_Value_Undefined;
        }
        if (prop && prop->has_setter) return Ant_Value_Undefined;
        handled = true;
      }
    }

    ant_value_t element;
    if (!handled && array_own_value(js, cur_obj, name, key_len, &element)) return element;

    if (!handled && cur_ptr && cur_ptr->flags.is_exotic) {
      ant_value_t exotic = exotic_own_get(js, cur_obj, cur_ptr, name, key_len);
      if (vtype(exotic) != kTypeUndefined) return exotic == T_EMPTY ? Ant_Value_Undefined : exotic;
      descriptor_entry_t *desc = lookup_descriptor(js, cur_obj, name, key_len);
      if (desc) {
        if (desc->has_getter) {
          ant_value_t getter = desc->getter;
          if (vtype(getter) == kTypeFunction || vtype(getter) == kTypeBuiltin)
            return sv_vm_call(js->vm, js, getter, receiver, NULL, 0, NULL, Ant_Value_Undefined);
          return Ant_Value_Undefined;
        }
        if (desc->has_setter) return Ant_Value_Undefined;
      }
    }

    ant_prop_loc_t prop_off = lkp_interned(cur_obj, key_intern);
    if (prop_off.obj) return js_prop_load(prop_off);

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    cur = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }

  return Ant_Value_Undefined;
}
