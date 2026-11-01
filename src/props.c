#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "cfunc.h"
#include "arrays.h"
#include "props.h"
#include "proto.h"
#include "stringify.h"
#include "protectors.h"
#include "silver/call.h"
#include "modules/bigint.h"
#include "modules/symbol.h"
#include "modules/proxy.h"

bool is_const_prop(ant_prop_loc_t loc) {
  if (!loc.obj || !loc.obj->shape) return false;
  uint8_t attrs = ant_shape_get_attrs(loc.obj->shape, loc.slot);
  return (attrs & ANT_PROP_ATTR_WRITABLE) == 0;
}

const ant_shape_prop_t *prop_shape_meta(ant_prop_loc_t loc) {
  if (!loc.obj || !loc.obj->shape) return NULL;
  return ant_shape_prop_at(loc.obj->shape, loc.slot);
}

bool is_nonconfig_prop(ant_prop_loc_t loc) {
  if (!loc.obj || !loc.obj->shape) return false;
  uint8_t attrs = ant_shape_get_attrs(loc.obj->shape, loc.slot);
  return (attrs & ANT_PROP_ATTR_CONFIGURABLE) == 0;
}

void js_init_intern_cache(ant_t *js) {
  js->intern.length = intern_string(js, "length", 6);
  js->intern.buffer = intern_string(js, "buffer", 6);
  js->intern.prototype = intern_string(js, "prototype", 9);
  js->intern.exec = intern_string(js, "exec", 4);
  js->intern.replace = intern_string(js, "replace", 7);
  js->intern.constructor = intern_string(js, "constructor", 11);
  js->intern.promise = intern_string(js, "Promise", 7);
  js->intern.resolve = intern_string(js, "resolve", 7);
  js->intern.then = intern_string(js, "then", 4);
  js->intern.to_json = intern_string(js, "toJSON", 6);
  js->intern.name = intern_string(js, "name", 4);
  js->intern.message = intern_string(js, "message", 7);
  js->intern.stack_trace_limit = intern_string(js, "stackTraceLimit", 15);
  js->intern.done = intern_string(js, "done", 4);
  js->intern.value = intern_string(js, "value", 5);
  js->intern.get = intern_string(js, "get", 3);
  js->intern.set = intern_string(js, "set", 3);
  js->intern.enumerable = intern_string(js, "enumerable", 10);
  js->intern.configurable = intern_string(js, "configurable", 12);
  js->intern.writable = intern_string(js, "writable", 8);
  js->intern.arguments = intern_string(js, "arguments", 9);
  js->intern.callee = intern_string(js, "callee", 6);
  js->intern.next = intern_string(js, "next", 4);
  js->intern.ret = intern_string(js, "return", 6);
  js->intern.define_property = intern_string(js, "defineProperty", 14);
  js->intern.get_own_property_descriptor = intern_string(js, "getOwnPropertyDescriptor", 24);
  js->intern.module_cache = intern_string(js, "_cache", 6);
}

ant_value_t mkprop_interned_attrs_impl(
  ant_t *js, ant_value_t obj, const char *interned_key, size_t key_len,
  ant_value_t v, uint8_t attrs, mkprop_mode_t mode
) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);

  if (!ptr || !ptr->shape || !interned_key) return js_mkerr(js, "invalid object");
  if ((mode & MKPROP_USE_DEFAULT_ATTRS) && !attrs) attrs = ANT_PROP_ATTR_DEFAULT;

  if (mode & MKPROP_EXPOSE_CFUNC) {
    v = js_expose_cfunc_for_key(js, v, interned_key, key_len);
    if (is_err(v)) return v;
  }

  uint32_t slot = 0;
  bool added = false;
  int32_t found = ant_shape_lookup_interned(ptr->shape, interned_key);

  if (found >= 0) {
    slot = (uint32_t)found;
    if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
    ant_shape_set_attrs_interned(js, ptr->shape, interned_key, attrs);
  } else {
    bool transitioned = (mode & MKPROP_KEYED_STORE)
      ? ant_shape_add_interned_keyed_tr(js, &ptr->shape, interned_key, attrs, &slot)
      : ant_shape_add_interned_tr(js, &ptr->shape, interned_key, attrs, &slot);
    if (!transitioned) return js_mkerr(js, "oom");
    ant_object_invalidate_guarded_absence(js, ptr);
    added = true;
  }

  if (added) ant_property_mutation_invalidate(js, ptr, interned_key);
  if (added && !js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape))) return js_mkerr(js, "oom");
  if (slot >= ptr->prop_count && !js_obj_ensure_prop_capacity(js, ptr, slot + 1)) return js_mkerr(js, "oom");

  ant_object_prop_set_unchecked(ptr, slot, v);
  gc_write_barrier(js, ptr, v);

  if (!added) ant_property_mutation_invalidate(js, ptr, interned_key);
  return v;
}

static ant_value_t mkprop_bytes_attrs_impl(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t v, uint8_t attrs, mkprop_mode_t mode
) {
  const char *interned = intern_string(js, key, key_len);
  if (!interned) return js_mkerr(js, "oom");
  return mkprop_interned_attrs_impl(js, obj, interned, key_len, v, attrs, mode);
}

ant_value_t mkprop_bytes_exact_attrs(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t v, uint8_t attrs
) {
  return mkprop_bytes_attrs_impl(js, obj, key, key_len, v, attrs, 0);
}

ant_value_t mkprop_bytes(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t v, uint8_t attrs
) {
  return mkprop_bytes_attrs_impl(
    js, obj, key, key_len, v, attrs,
    MKPROP_USE_DEFAULT_ATTRS | MKPROP_EXPOSE_CFUNC
  );
}

ant_value_t mkprop_symbol_attrs_impl(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v,
  uint8_t attrs, mkprop_mode_t mode
) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);

  if (!ptr || !ptr->shape) return js_mkerr(js, "invalid object");
  if ((mode & MKPROP_USE_DEFAULT_ATTRS) && !attrs) attrs = ANT_PROP_ATTR_DEFAULT;

  uint32_t slot = 0;
  bool added = false;
  ant_offset_t sym_off = (ant_offset_t)vdata(k);
  int32_t found = ant_shape_lookup_symbol(ptr->shape, sym_off);
  if (found >= 0) {
    slot = (uint32_t)found;
    if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
    ant_shape_set_attrs_symbol(js, ptr->shape, sym_off, attrs);
  } else {
    bool transitioned = (mode & MKPROP_KEYED_STORE)
      ? ant_shape_add_symbol_keyed_tr(js, &ptr->shape, sym_off, attrs, &slot)
      : ant_shape_add_symbol_tr(js, &ptr->shape, sym_off, attrs, &slot);
    if (!transitioned) return js_mkerr(js, "oom");
    ant_object_invalidate_guarded_absence(js, ptr);
    added = true;
  }

  if (added && !js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape))) return js_mkerr(js, "oom");
  if (slot >= ptr->prop_count && !js_obj_ensure_prop_capacity(js, ptr, slot + 1)) return js_mkerr(js, "oom");

  ant_object_prop_set_unchecked(ptr, slot, v);
  gc_write_barrier(js, ptr, v);
  ant_symbol_property_mutation_invalidate(js, ptr, sym_off);
  
  return v;
}

static inline ant_value_t mkprop_impl(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v,
  uint8_t attrs, bool default_attrs
) {
  if (vtype(k) == kTypeSymbol) return mkprop_symbol_attrs_impl(
    js, obj, k, v, attrs, 
    default_attrs ? MKPROP_USE_DEFAULT_ATTRS : 0
  );

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, k, &key_len);
  const char *key = (const char *)(uintptr_t)key_off;
  
  return default_attrs
    ? mkprop_bytes(js, obj, key, (size_t)key_len, v, attrs)
    : mkprop_bytes_exact_attrs(js, obj, key, (size_t)key_len, v, attrs);
}

ant_value_t mkprop(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs
) {
  return mkprop_impl(js, obj, k, v, attrs, true);
}

ant_value_t mkprop_exact_attrs(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs
) {
  return mkprop_impl(js, obj, k, v, attrs, false);
}

ant_value_t mkprop_interned(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs) {
  return mkprop_interned_attrs_impl(
    js, obj, interned_key, strlen(interned_key), v, attrs,
    MKPROP_USE_DEFAULT_ATTRS | MKPROP_EXPOSE_CFUNC
  );
}

ant_value_t mkprop_interned_exact(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs) {
  return mkprop_interned_attrs_impl(
    js, obj, interned_key, 0, v,
    attrs, MKPROP_USE_DEFAULT_ATTRS
  );
}

ant_value_t mkprop_interned_keyed(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v) {
  return mkprop_interned_attrs_impl(
    js, obj, interned_key, 0, v,
    0, MKPROP_USE_DEFAULT_ATTRS | MKPROP_KEYED_STORE
  );
}

ant_value_t js_mkprop_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  const char *interned = intern_string(js, key, len);
  if (!interned) return js_mkerr(js, "oom");
  return mkprop_interned(js, obj, interned, v, 0);
}

ant_value_t mkprop_append_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  const char *interned = intern_string(js, key, len);
  if (!interned) return js_mkerr(js, "oom");

  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return js_mkerr(js, "invalid object");

  uint32_t slot = 0;
  int32_t found = ant_shape_lookup_interned(ptr->shape, interned);
  
  if (found >= 0) slot = (uint32_t)found;
  else {
    if (!ant_shape_add_interned_tr(js, &ptr->shape, interned, ANT_PROP_ATTR_DEFAULT, &slot)) return js_mkerr(js, "oom");
    ant_object_invalidate_guarded_absence(js, ptr);
  }

  if (
    slot >= ptr->prop_count &&
    !js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape))
  ) return js_mkerr(js, "oom");

  ant_object_prop_set_unchecked(ptr, slot, v);
  gc_write_barrier(js, ptr, v);
  ant_property_mutation_invalidate(js, ptr, interned);
  
  return v;
}

static inline void prop_meta_defaults(prop_meta_t *out) {
  *out = (prop_meta_t){
    .has_getter = false,
    .has_setter = false,
    .writable = true,
    .enumerable = true,
    .configurable = true,
    .slot = -1,
    .getter = Ant_Value_Undefined,
    .setter = Ant_Value_Undefined,
  };
}

void prop_meta_from_shape(prop_meta_t *out, const ant_shape_prop_t *prop) {
  out->has_getter = prop->has_getter != 0;
  out->has_setter = prop->has_setter != 0;
  out->writable = (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0;
  out->enumerable = (prop->attrs & ANT_PROP_ATTR_ENUMERABLE) != 0;
  out->configurable = (prop->attrs & ANT_PROP_ATTR_CONFIGURABLE) != 0;
  out->getter = prop->getter;
  out->setter = prop->setter;
}

static inline void prop_meta_from_desc(prop_meta_t *out, const descriptor_entry_t *desc) {
  out->has_getter = desc->has_getter;
  out->has_setter = desc->has_setter;
  out->writable = desc->writable;
  out->enumerable = desc->enumerable;
  out->configurable = desc->configurable;
  out->getter = desc->getter;
  out->setter = desc->setter;
}

bool lookup_prop_meta(
  ant_t *js,
  ant_value_t cur_obj,
  prop_meta_key_t key_kind,
  const char *key,
  size_t klen,
  ant_offset_t sym_off,
  prop_meta_t *out
) {
  if (!js || !out || !is_object_type(cur_obj)) return false;
  if (key_kind == PROP_META_STRING && !key) return false;

  prop_meta_defaults(out);

  ant_object_t *cur_ptr = js_obj_ptr(cur_obj);
  if (!cur_ptr) return false;
  if (key_kind == PROP_META_STRING && key && is_length_key(key, klen) &&
      ant_object_is_array(cur_ptr)) {
    out->has_getter = false;
    out->has_setter = false;
    out->writable = true;
    out->enumerable = false;
    out->configurable = false;
    out->getter = Ant_Value_Undefined;
    out->setter = Ant_Value_Undefined;
    return true;
  }

  if (cur_ptr->shape) {
    int32_t slot = -1;
    if (key_kind == PROP_META_SYMBOL) {
      slot = ant_shape_lookup_symbol(cur_ptr->shape, sym_off);
    } else {
      const char *interned_key = intern_find(js, key, klen);
      if (interned_key) slot = ant_shape_lookup_interned(cur_ptr->shape, interned_key);
    }

    if (slot >= 0) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(cur_ptr->shape, (uint32_t)slot);
      if (!prop) return false;
      prop_meta_from_shape(out, prop);
      out->slot = slot;
      return true;
    }
  }

  if (!cur_ptr->flags.is_exotic) return false;

  descriptor_entry_t *desc = key_kind == PROP_META_SYMBOL
    ? lookup_sym_descriptor(js, cur_obj, sym_off)
    : lookup_descriptor(js, cur_obj, key, klen);

  if (!desc) return false;
  prop_meta_from_desc(out, desc);

  return true;
}

bool js_try_get_own_data_prop_interned(ant_t *js, ant_value_t obj, const char *interned, ant_value_t *out) {
  *out = Ant_Value_Undefined;
  if (!interned) return false;
  
  ant_value_t as_obj = js_as_obj(vtype(obj) == kTypeFunction ? js_func_obj(obj) : obj);
  ant_object_t *ptr = vtype(as_obj) == kTypeObject ? js_obj_ptr(as_obj) : NULL;
  
  if (ptr && ptr->shape && !ptr->flags.is_exotic) {
    int32_t slot = ant_shape_lookup_interned(ptr->shape, interned);
    const ant_shape_prop_t *prop = slot >= 0 ? ant_shape_prop_at(ptr->shape, (uint32_t)slot) : NULL;
    
    if (prop && (prop->has_getter || prop->has_setter)) return false;
    
    if (prop) {
      *out = ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
      return true;
    }
  }
  
  return js_try_get_own_data_prop(js, obj, interned, strlen(interned), out);
}

bool js_try_get_own_data_prop(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  if (out) *out = Ant_Value_Undefined;
  if (!key || !out) return false;

  uint8_t t = vtype(obj);
  if (t == kTypeFunction) obj = js_func_obj(obj);
  else if (t != kTypeObject && t != kTypeArray) return false;

  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  
  if (ptr && ptr->flags.is_exotic && is_proxy(as_obj)) return false;

  if (ptr && ptr->shape && !(real_array_obj_ptr(as_obj) && is_length_key(key, key_len))) {
    const char *interned = intern_find(js, key, key_len);
    int32_t slot = interned ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
    
    if (slot >= 0) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
      if (!prop || prop->has_getter || prop->has_setter) return false;
      
      *out = ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
      return true;
    }
  }

  prop_meta_t meta;
  bool has_meta = lookup_string_prop_meta(js, as_obj, key, key_len, &meta);
  if (has_meta && (meta.has_getter || meta.has_setter)) return false;

  ant_prop_loc_t off = lkp(js, as_obj, key, (ant_offset_t)key_len);
  if (off.obj) {
    *out = js_prop_load(off);
    return true;
  }

  if (real_array_obj_ptr(as_obj) && is_length_key(key, (ant_offset_t)key_len)) {
    *out = Ant_Value_FromNumber((double)get_array_length(js, as_obj));
    return true;
  }

  return false;
}

ant_value_t call_proto_accessor(
  ant_t *js, ant_value_t prim, ant_value_t accessor, bool has_accessor,
  ant_value_t *arg, int arg_count, bool is_setter
) {
  if (!has_accessor || (vtype(accessor) != kTypeFunction && vtype(accessor) != kTypeBuiltin))
    return Ant_Value_Undefined;
  
  if (!is_setter && js_is_symbol_description_getter(accessor)) {
    ant_value_t symbol = prim;
    if (is_object_type(symbol)) symbol = js_get_slot(symbol, SLOT_PRIMITIVE);
    if (vtype(symbol) == kTypeSymbol) return js_symbol_description_value(js, symbol);
  }

  ant_value_t result = sv_vm_call(js->vm, js, accessor, prim, arg, arg_count, NULL, Ant_Value_Undefined);
  if (is_setter) return is_err(result) ? result : (arg ? *arg : Ant_Value_Undefined);

  return result;
}

ant_value_t js_prop_load_from(ant_t *js, ant_value_t receiver, ant_prop_loc_t off) {
  const ant_shape_prop_t *meta = prop_shape_meta(off);
  if (!meta || !(meta->has_getter || meta->has_setter)) return js_prop_load(off);
  return call_proto_accessor(js, receiver, meta->getter, meta->has_getter, NULL, 0, false);
}

ant_value_t js_call_getter(ant_t *js, ant_value_t receiver, ant_value_t getter) {
  if (vtype(getter) != kTypeBuiltin) return call_proto_accessor(js, receiver, getter, true, NULL, 0, false);
  return sv_invoke_builtin(js, getter, receiver, getter, NULL, 0, Ant_Value_Undefined);
}

ant_value_t js_define_own_prop(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t v) {
  obj = js_as_obj(obj);
  if (is_proxy(obj)) {
    ant_value_t result = proxy_set(js, obj, key, klen, v);
    if (is_err(result)) return result;
    return v;
  }

  if (try_dynamic_setter(js, obj, key, klen, v))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : v;
  ant_prop_loc_t existing = lkp(js, obj, key, klen);

  {
    bool has_desc = false;
    bool desc_writable = true;
    bool desc_has_setter = false;
    ant_value_t desc_setter = Ant_Value_Undefined;
    ant_value_t as_obj = js_as_obj(obj);
    ant_object_t *ptr = js_obj_ptr(as_obj);
    const char *interned_key = intern_string(js, key, klen);

    if (ptr && ptr->shape && interned_key) {
      int32_t slot = ant_shape_lookup_interned(ptr->shape, interned_key);
      if (slot >= 0) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
        if (prop) {
          has_desc = true;
          desc_writable = (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0;
          desc_has_setter = prop->has_setter != 0;
          desc_setter = prop->setter;
        }
      }
    }

    if (!has_desc && ptr && ptr->flags.is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(js, as_obj, key, klen);
      if (desc) {
        has_desc = true;
        desc_writable = desc->writable;
        desc_has_setter = desc->has_setter;
        desc_setter = desc->setter;
      }
    }

    if (has_desc) {
      if (!desc_writable && !desc_has_setter) {
        if (sv_is_strict_context(js)) return js_mkerr(js, "assignment to read-only property");
        return v;
      }
      if (desc_has_setter) {
        ant_value_t setter = desc_setter;
        uint8_t setter_type = vtype(setter);
        if (setter_type == kTypeFunction || setter_type == kTypeBuiltin) {
          ant_value_t result = sv_vm_call(js->vm, js, setter, obj, &v, 1, NULL, Ant_Value_Undefined);
          if (is_err(result)) return result;
          return v;
        }
      }
    }
  }

  if (existing.obj) {
    if (is_const_prop(existing)) return js_mkerr(js, "assignment to constant");
    js_prop_store(js, existing, v);
    array_define_or_set_index(js, obj, key, klen);
    return v;
  }

  {
    ant_value_t extensibility_error = check_object_extensibility(js, obj);
    if (is_err(extensibility_error)) return extensibility_error;
    if (extensibility_error == Ant_Value_False) return v;
  }

  const char *interned_key = intern_string(js, key, klen);
  if (!interned_key) return js_mkerr(js, "oom");
  ant_value_t created = mkprop_interned_exact(js, obj, interned_key, v, 0);
  
  if (!is_err(created)) array_define_or_set_index(js, obj, key, klen);
  return is_err(created) ? created : v;
}

ant_prop_loc_t lkp_interned(ant_value_t obj, const char *search_intern) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!search_intern || !ptr || !ptr->shape) return ANT_PROP_LOC_NONE;

  int32_t shape_slot = ant_shape_lookup_interned(ptr->shape, search_intern);
  if (shape_slot < 0) return ANT_PROP_LOC_NONE;
  return (ant_prop_loc_t){ptr, (uint32_t)shape_slot};
}

inline ant_prop_loc_t lkp(ant_t *js, ant_value_t obj, const char *buf, size_t len) {
  const char *search_intern = intern_find(js, buf, len);
  if (!search_intern) return ANT_PROP_LOC_NONE;
  return lkp_interned(obj, search_intern);
}

inline ant_value_t lkp_interned_val(ant_t *js, ant_value_t obj, const char *search_intern) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!search_intern || !ptr || !ptr->shape) return Ant_Value_Undefined;
  int32_t slot = ant_shape_lookup_interned(ptr->shape, search_intern);
  if (slot < 0) return Ant_Value_Undefined;
  return ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
}

ant_value_t lkp_val(ant_t *js, ant_value_t obj, const char *buf, size_t len) {
  const char *interned = intern_find(js, buf, len);
  if (!interned) return Ant_Value_Undefined;
  return lkp_interned_val(js, obj, interned);
}

ant_prop_loc_t lkp_sym(ant_value_t obj, ant_offset_t sym_off) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return ANT_PROP_LOC_NONE;
  int32_t slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
  if (slot < 0) return ANT_PROP_LOC_NONE;
  return (ant_prop_loc_t){ptr, (uint32_t)slot};
}

ant_prop_loc_t lkp_sym_proto(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  ant_value_t cur = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  while (is_object_type(cur)) {
    obj = cur;
    ant_prop_loc_t loc = lkp_sym(obj, sym_off);
    if (loc.obj) return loc;
    ant_value_t proto = get_proto(js, js_as_obj(cur));
    if (!is_object_type(proto)) break;
    cur = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }
  return ANT_PROP_LOC_NONE;
}

ant_value_t lkp_sym_proto_val(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  ant_value_t cur = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  
  while (is_object_type(cur)) {
    ant_value_t as_obj = js_as_obj(cur);
    ant_object_t *ptr = js_obj_ptr(as_obj);
    
    if (ptr && ptr->shape) {
      int32_t slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
      if (slot >= 0) return ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
    }
    
    ant_value_t proto = get_proto(js, as_obj);
    if (!is_object_type(proto)) break;
    
    cur = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }
  
  return Ant_Value_Undefined;
}

uintptr_t lkp_with_getter(ant_t *js, ant_value_t obj, const char *buf, size_t len, ant_value_t *getter_out, bool *has_getter_out) {
  *has_getter_out = false;
  *getter_out = Ant_Value_Undefined;
  const char *search_intern = intern_find(js, buf, len);
  
  ant_value_t current = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  
  while (is_object_type(current)) {
    current = js_as_obj(current);
    uintptr_t current_id = (uintptr_t)vdata(current);

    ant_object_t *ptr = js_obj_ptr(current);
    if (ptr && ptr->shape && search_intern) {
      int32_t slot = ant_shape_lookup_interned(ptr->shape, search_intern);
      if (slot >= 0) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
        if (prop && prop->has_getter) {
          *getter_out = prop->getter;
          *has_getter_out = true;
        }
        return current_id;
      }
    }

    if (ptr && ptr->flags.is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(js, current, buf, len);
      if (desc && desc->has_getter) {
        *getter_out = desc->getter;
        *has_getter_out = true;
        return current_id;
      }
    }
    
    ant_value_t proto = get_proto(js, current);
    if (!is_object_type(proto)) break;
    current = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, current)) break;
  }
  
  return 0;
}

uintptr_t lkp_with_setter(ant_t *js, ant_value_t obj, const char *buf, size_t len, ant_value_t *setter_out, bool *has_setter_out) {
  *has_setter_out = false;
  *setter_out = Ant_Value_Undefined;
  const char *search_intern = intern_find(js, buf, len);
  
  ant_value_t current = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  while (is_object_type(current)) {
    current = js_as_obj(current);
    uintptr_t current_id = (uintptr_t)vdata(current);

    ant_object_t *ptr = js_obj_ptr(current);
    if (ptr && ptr->shape && search_intern) {
      int32_t slot = ant_shape_lookup_interned(ptr->shape, search_intern);
      if (slot >= 0) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
        if (prop && prop->has_setter) {
          *setter_out = prop->setter;
          *has_setter_out = true;
        }
        return current_id;
      }
    }

    if (ptr && ptr->flags.is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(js, current, buf, len);
      if (desc && desc->has_setter) {
        *setter_out = desc->setter;
        *has_setter_out = true;
        return current_id;
      }
    }
    
    ant_value_t proto = get_proto(js, current);
    if (!is_object_type(proto)) break;
    current = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, current)) break;
  }
  
  return 0;
}

ant_prop_loc_t lkp_proto(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  uint8_t t = vtype(obj);
  const char *key_intern = intern_find(js, key, len);
  if (!key_intern) return ANT_PROP_LOC_NONE;

  ant_value_t cur = obj;
  proto_walk_overflow_guard_t guard;
  proto_walk_overflow_guard_init(&guard);

  while (true) {
    if (t == kTypeObject || t == kTypeArray || t == kTypeFunction || t == kTypePromise || t == kTypeGenerator) {
      ant_value_t as_obj = js_as_obj(cur);
      ant_prop_loc_t loc = lkp_interned(as_obj, key_intern);
      if (loc.obj) return loc;
      const ant_object_t *ptr = (const ant_object_t *)vptr(as_obj);
      ant_value_t element;
      if (cur != obj && (vtype(exotic_own_get(js, as_obj, ptr, key, len)) != kTypeUndefined ||
                         array_own_value(js, as_obj, key, len, &element))) break;
    } else if (t == kTypeBuiltin) {
      ant_value_t func_proto = js->sym.function_proto;
      uint8_t ft = vtype(func_proto);
      if (ft == kTypeObject || ft == kTypeArray || ft == kTypeFunction) {
        ant_prop_loc_t loc = lkp_interned(js_as_obj(func_proto), key_intern);
        if (loc.obj) return loc;
      }
      break;
    } else if (!is_boxable_primitive_type(t)) break;

    if (!proto_walk_next(js, &cur, &t, PROTO_WALK_F_LOOKUP)) break;
    if (proto_walk_overflow_guard_hit_cycle(js, &guard, cur, t, PROTO_WALK_F_LOOKUP)) break;
  }

  return ANT_PROP_LOC_NONE;
}

void property_key_view_dispose(property_key_view_t *key) {
  if (key->heap_bytes) free(key->heap_bytes);
  key->js_key = Ant_Value_Undefined;
  key->heap_bytes = NULL;
  key->bytes = NULL;
  key->length = 0;
  key->is_symbol = false;
  key->requires_root = false;
  key->resolved = false;
}

static inline void property_key_view_resolve(ant_t *js, property_key_view_t *key) {
  if (key->resolved || key->is_symbol) return;
  key->resolved = true;
  
  ant_flat_string_t *flat = ant_str_flat_ptr(key->js_key);
  key->interned = flat && flat->bytes == key->bytes
    ? intern_find_flat(js, key->bytes, key->length)
    : intern_find(js, key->bytes, key->length);

  key->index_like = is_length_key(key->bytes, key->length) || is_array_index(key->bytes, (ant_offset_t)key->length);
}

static inline void property_key_view_reset(
  property_key_view_t *key, ant_value_t value
) {
  *key = (property_key_view_t){
    .js_key = value,
    .bytes = NULL,
    .length = 0,
    .heap_bytes = NULL,
    .is_symbol = false,
    .requires_root = false,
  };
}

static inline ant_value_t property_key_view_bind_value(
  ant_t *js, property_key_view_t *key, ant_value_t value
) {
  key->js_key = value;
  if (vtype(value) == kTypeSymbol) {
    key->is_symbol = true;
    return Ant_Value_Undefined;
  }

  ant_offset_t length = 0;
  ant_offset_t offset = vstr(js, value, &length);
  key->bytes = (const char *)(uintptr_t)offset;
  key->length = (size_t)length;
  return Ant_Value_Undefined;
}

static ant_value_t property_key_view_init_slow(
  ant_t *js, ant_value_t value, property_key_view_t *key,
  char *stack_buf, size_t stack_capacity
) {
  if (is_object_type(value) || vtype(value) == kTypeBuiltin) {
    value = js_to_primitive(js, value, 1);
    key->js_key = value;
    if (is_err(value)) return value;
    key->requires_root = vtype(value) == kTypeSymbol || vtype(value) == kTypeString;
  }

  if (vtype(value) == kTypeSymbol || vtype(value) == kTypeString)
    return property_key_view_bind_value(js, key, value);

  switch (vtype(value)) {
    case kTypeNumber:
      key->length = js_number_to_chars(value, stack_buf, stack_capacity);
      key->bytes = stack_buf;
      return Ant_Value_Undefined;
    case kTypeBigInt: {
      size_t length = strbigint(js, value, stack_buf, stack_capacity);
      if (length < stack_capacity) {
        key->length = length;
        key->bytes = stack_buf;
        return Ant_Value_Undefined;
      }

      char *output = (char *)calloc(1, length + 1);
      if (!output) return js_mkerr(js, "oom");
      key->heap_bytes = output;
      key->length = strbigint(js, value, output, length + 1);
      key->bytes = output;
      return Ant_Value_Undefined;
    }
    case kTypeBool:
      key->bytes = vdata(value) ? "true" : "false";
      key->length = vdata(value) ? 4 : 5;
      return Ant_Value_Undefined;
    case kTypeNull:
      key->bytes = "null";
      key->length = 4;
      return Ant_Value_Undefined;
    case kTypeUndefined:
      key->bytes = "undefined";
      key->length = 9;
      return Ant_Value_Undefined;
    default: break;
  }

  ant_value_t string = js_tostring_val(js, value);
  key->js_key = string;
  
  if (is_err(string)) return string;
  key->requires_root = true;

  return property_key_view_bind_value(js, key, string);
}

ant_value_t property_key_view_init(
  ant_t *js, ant_value_t value, property_key_view_t *key,
  char *stack_buf, size_t stack_capacity
) {
  property_key_view_reset(key, value);
  if (vtype(value) == kTypeSymbol || vtype(value) == kTypeString)
    return property_key_view_bind_value(js, key, value);
  return property_key_view_init_slow(
    js, value, key, stack_buf, stack_capacity
  );
}

bool property_key_view_requires_pin(
  const property_key_view_t *key, bool may_materialize
) {
  return key->requires_root || may_materialize;
}

ant_value_t property_key_view_materialize(ant_t *js, property_key_view_t *key) {
  if (key->is_symbol || vtype(key->js_key) == kTypeString) return key->js_key;

  ant_value_t string = js_mkstr(js, key->bytes, key->length);
  key->js_key = string;
  if (is_err(string)) return string;
  key->requires_root = true;

  property_key_view_bind_value(js, key, string);
  return string;
}

static ant_value_t check_frozen_sealed(ant_t *js, ant_value_t obj, const char *action) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return Ant_Value_Undefined;

  if (ptr->flags.frozen) {
    if (sv_is_strict_context(js)) return js_mkerr(js, "cannot %s property of frozen object", action);
    return Ant_Value_False;
  }
  if (ptr->flags.sealed) {
    if (sv_is_strict_context(js)) return js_mkerr(js, "cannot %s property of sealed object", action);
    return Ant_Value_False;
  }
  return Ant_Value_Undefined;
}

ant_value_t js_delete_prop_ordinary(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  if (vtype(obj) == kTypeString) {
    unsigned long idx = 0;
    return Ant_Value_FromBool(!is_length_key(key, len) && !parse_array_index(key, len, str_utf16_len(js, obj), &idx));
  }
  
  if (vtype(obj) == kTypeSymbol || vtype(obj) == kTypeBigInt) return Ant_Value_True;
  
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    if (is_err(promoted)) return promoted;
    obj = promoted;
  }
  
  ant_value_t original_obj = obj;
  obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return Ant_Value_True;
  
  if (is_proxy(obj)) {
    ant_value_t result = proxy_delete(js, obj, key, len);
    return is_err(result) ? result : Ant_Value_FromBool(js_truthy(js, result));
  }

  if (string_wrapper_owns_key(js, obj, key, len)) return Ant_Value_False;

  if (ptr->flags.frozen || ptr->flags.sealed) {
    bool own = 
      lkp(js, obj, key, len).obj || 
      arr_own_dense_element(js, original_obj, key, len, NULL) ||
      (real_array_obj_ptr(original_obj) && is_length_key(key, len));
    return Ant_Value_FromBool(!own);
  }

  if (real_array_obj_ptr(original_obj) && is_length_key(key, len)) return Ant_Value_False;

  if (array_obj_ptr(original_obj)) {
    ant_offset_t doff = get_dense_buf(original_obj);
    unsigned long del_idx = 0;
    
    if (doff && parse_array_index(key, len, get_array_length(js, original_obj), &del_idx)) {
      array_mark_may_have_holes(original_obj);
      ant_offset_t dense_len = dense_iterable_length(js, original_obj);
      if ((ant_offset_t)del_idx < dense_len) dense_set(js, doff, (ant_offset_t)del_idx, T_EMPTY);
    }
  }

  const char *interned = intern_string(js, key, len);
  if (!interned) return Ant_Value_FromBool(exotic_delete_allowed(js, obj, key, len));

  int32_t shape_slot = ant_shape_lookup_interned(ptr->shape, interned);
  if (shape_slot < 0) return Ant_Value_FromBool(exotic_delete_allowed(js, obj, key, len));

  uint8_t attrs = ant_shape_get_attrs(ptr->shape, (uint32_t)shape_slot);
  if ((attrs & ANT_PROP_ATTR_CONFIGURABLE) == 0) return Ant_Value_False;

  if (ptr->flags.is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(js, obj, key, len);
    if (desc && !desc->configurable) return Ant_Value_False;
  }

  uint32_t slot = (uint32_t)shape_slot;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  if (!ant_shape_prop_at(ptr->shape, slot)) return Ant_Value_True;
  if (!ant_shape_remove_slot(js, ptr->shape, slot)) return js_mkerr(js, "oom");
  
  ant_property_mutation_invalidate(js, ptr, interned);
  obj_delete_prop_slot(ptr, slot);
  
  return Ant_Value_True;
}

ant_value_t js_delete_prop(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  ant_value_t result = js_delete_prop_ordinary(js, obj, key, len);
  if (result == Ant_Value_False && sv_is_strict_context(js))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot delete property '%.*s' of object", (int)len, key);
  return result;
}

ant_value_t js_delete_sym_prop(ant_t *js, ant_value_t obj, ant_value_t sym) {
  if (vtype(obj) == kTypeString || vtype(obj) == kTypeSymbol || vtype(obj) == kTypeBigInt) return Ant_Value_True;
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    if (is_err(promoted)) return promoted;
    obj = promoted;
  }
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return Ant_Value_True;
  if (is_proxy(obj)) {
    ant_value_t result = proxy_delete_sym(js, obj, sym);
    return is_err(result) ? result : Ant_Value_FromBool(js_truthy(js, result));
  }

  ant_value_t err = check_frozen_sealed(js, obj, "delete");
  if (vtype(err) != kTypeUndefined) return err;

  ant_offset_t sym_off = (ant_offset_t)vdata(sym);
  int32_t shape_slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
  if (shape_slot < 0) return Ant_Value_True;

  uint8_t attrs = ant_shape_get_attrs(ptr->shape, (uint32_t)shape_slot);
  if ((attrs & ANT_PROP_ATTR_CONFIGURABLE) == 0) {
    if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
    return Ant_Value_False;
  }

  uint32_t slot = (uint32_t)shape_slot;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  if (!ant_shape_prop_at(ptr->shape, slot)) return Ant_Value_True;
  if (!ant_shape_remove_slot(js, ptr->shape, slot)) return js_mkerr(js, "oom");
  ant_symbol_property_mutation_invalidate(js, ptr, sym_off);
  obj_delete_prop_slot(ptr, slot);
  
  return Ant_Value_True;
}

static bool typed_array_has_index(ant_value_t obj, const char *key, size_t key_len) {
  ant_object_t *ptr = is_object_type(obj) ? js_obj_ptr(js_as_obj(obj)) : NULL;
  if (!ptr || !ptr->flags.is_exotic) return false;
  
  TypedArrayData *typed = buffer_get_typedarray_data(js_as_obj(obj));
  unsigned long index = 0;
  
  return
    Ant_Buffer_ViewIsLive(typed) &&
    parse_array_index(key, key_len, (ant_offset_t)typed->length, &index);
}

ant_value_t object_has_own_key(ant_t *js, ant_value_t obj, property_key_view_t *key) {
  uint8_t t = vtype(obj);

  if (t == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) {
      obj = promoted;
      t = kTypeFunction;
    } else {
      if (key->is_symbol) return mkval(kTypeBool, 0);
      ant_value_t value = Ant_Value_Undefined;
      return mkval(kTypeBool, js_cfunc_try_get_own(js, obj, key->bytes, key->length, &value) ? 1 : 0);
    }
  }

  if (key->is_symbol) {
    if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return mkval(kTypeBool, 0);
    ant_value_t as_obj = js_as_obj(obj);
    if (is_proxy(as_obj)) return proxy_has_own(js, as_obj, key->js_key);

    ant_offset_t sym_off = (ant_offset_t)vdata(key->js_key);
    ant_prop_loc_t off = lkp_sym(as_obj, sym_off);
    if (off.obj) return mkval(kTypeBool, 1);
    prop_meta_t meta;
    return mkval(kTypeBool, lookup_symbol_prop_meta(js, as_obj, sym_off, &meta) ? 1 : 0);
  }

  const char *key_str = key->bytes;
  size_t key_len = key->length;
  property_key_view_resolve(js, key);

  if (t == kTypeString) {
    ant_value_t string_exotic_value = Ant_Value_Undefined;
    if (js_try_get_string_own_exotic(js, obj, key_str, key_len, &string_exotic_value, NULL))
      return mkval(kTypeBool, 1);
    return mkval(kTypeBool, 0);
  }

  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return mkval(kTypeBool, 0);
  
  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  
  bool exotic = ptr && ptr->flags.is_exotic;
  if (!exotic && !key->index_like) return Ant_Value_FromBool(lkp_interned(as_obj, key->interned).obj);
  
  if (exotic && is_proxy(as_obj)) {
    ant_value_t materialized = property_key_view_materialize(js, key);
    if (is_err(materialized)) return materialized;
    return proxy_has_own(js, as_obj, materialized);
  }

  if (key->index_like && array_obj_ptr(as_obj)) {
    if (real_array_obj_ptr(as_obj) && is_length_key(key_str, key_len)) return mkval(kTypeBool, 1);
    if (arr_own_dense_element(js, as_obj, key_str, key_len, NULL)) return mkval(kTypeBool, 1);
  }

  if (lkp_interned(as_obj, key->interned).obj) return mkval(kTypeBool, 1);
  
  if (exotic) {
    if (key->index_like && typed_array_has_index(as_obj, key_str, key_len)) return mkval(kTypeBool, 1);
    descriptor_entry_t *desc = lookup_descriptor(js, as_obj, key_str, key_len);
    return mkval(kTypeBool, (desc && (desc->has_getter || desc->has_setter)) ? 1 : 0);
  }

  ant_value_t string_exotic_value = Ant_Value_Undefined;
  
  return mkval(kTypeBool, 
    key->index_like && t == kTypeObject &&
    js_try_get_string_own_exotic(js, obj, key_str, key_len, &string_exotic_value, NULL)
  );
}

ant_value_t Ant_Object_HasOwn(
  ant_t *js, ant_value_t obj, ant_value_t raw_key
) {
  uint8_t t = vtype(obj);
  if (t == kTypeNull || t == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");

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

  ant_value_t result = object_has_own_key(js, obj, &key);
  property_key_view_dispose(&key);
  if (root_key) GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

bool js_is_own_enumerable_prop(
  ant_t *js, ant_value_t source, ant_object_t *source_ptr,
  const ant_iter_key_t *key
) {
  if (!key) return false;

  if (key->is_symbol) {
    if (!source_ptr || source_ptr->flags.is_exotic) {
      prop_meta_t meta;
      return !lookup_symbol_prop_meta(js, js_as_obj(source), key->sym_off, &meta) || meta.enumerable;
    }
    return (ant_shape_get_attrs(source_ptr->shape, key->slot) & ANT_PROP_ATTR_ENUMERABLE) != 0;
  }

  if (!key->str) return false;

  if (!source_ptr || source_ptr->flags.is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(js, js_as_obj(source), key->str, key->key_len);
    return !desc || desc->enumerable;
  }

  return (ant_shape_get_attrs(source_ptr->shape, key->slot) & ANT_PROP_ATTR_ENUMERABLE) != 0;
}

static ant_value_t object_property_is_enumerable_key(
  ant_t *js, ant_value_t obj, property_key_view_t *key
) {
  uint8_t t = vtype(obj);
  ant_value_t string_exotic = Ant_Value_Undefined;
  bool string_length = false;
  
  if (t == kTypeString) return mkval(kTypeBool, !key->is_symbol && js_try_get_string_own_exotic(
    js, obj, key->bytes, key->length, &string_exotic, &string_length
  ) && !string_length);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return mkval(kTypeBool, 0);
  ant_value_t as_obj = js_as_obj(obj);
  bool is_arr_obj = array_obj_ptr(as_obj) != NULL;

  if (key->is_symbol) {
    ant_offset_t sym_off = (ant_offset_t)vdata(key->js_key);
    ant_prop_loc_t off = lkp_sym(as_obj, sym_off);
    if (!off.obj) return mkval(kTypeBool, 0);
    prop_meta_t meta;
    if (lookup_symbol_prop_meta(js, as_obj, sym_off, &meta))
      return mkval(kTypeBool, meta.enumerable ? 1 : 0);
    return mkval(kTypeBool, 1);
  }

  const char *key_str = key->bytes;
  size_t key_len = key->length;
  
  if (real_array_obj_ptr(as_obj) && is_length_key(key_str, key_len)) return mkval(kTypeBool, 0);
  if (is_arr_obj) {
    unsigned long idx;
    if (parse_array_index(key_str, key_len, get_array_length(js, as_obj), &idx))
      return mkval(kTypeBool, arr_has(js, as_obj, (ant_offset_t)idx) ? 1 : 0);
  }

  if (t == kTypeObject && js_try_get_string_own_exotic(js, obj, key_str, key_len, &string_exotic, &string_length))
    return mkval(kTypeBool, string_length ? 0 : 1);
  
  ant_prop_loc_t off = lkp(js, as_obj, key_str, key_len);
  if (!off.obj) return mkval(kTypeBool, 0);

  const ant_shape_prop_t *prop_meta = prop_shape_meta(off);
  if (prop_meta && !js_obj_ptr(as_obj)->flags.is_exotic) {
    bool enumerable = (prop_meta->attrs & ANT_PROP_ATTR_ENUMERABLE) != 0;
    return mkval(kTypeBool, enumerable ? 1 : 0);
  }

  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr && ptr->flags.is_exotic) {
    prop_meta_t meta;
    if (lookup_string_prop_meta(js, as_obj, key_str, (size_t)key_len, &meta))
      return mkval(kTypeBool, meta.enumerable ? 1 : 0);
  }
  return mkval(kTypeBool, 1);
}

ant_value_t Ant_Object_PropertyIsEnumerable(ant_t *js, ant_value_t obj, ant_value_t raw_key) {
  char key_stack[128];
  property_key_view_t key;
  ant_value_t error = property_key_view_init(
    js, raw_key, &key, 
    key_stack, sizeof(key_stack)
  );
  
  ant_value_t result = error;
  if (!is_err(error) && (vtype(obj) == kTypeUndefined || vtype(obj) == kTypeNull))
    result = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
  else if (!is_err(error) && vtype(obj) == kTypeString && !key.is_symbol) {
    unsigned long idx = 0;
    result = Ant_Value_FromBool(parse_array_index(key.bytes, key.length, str_utf16_len(js, obj), &idx));
  } else if (!is_err(error)) result = object_property_is_enumerable_key(js, obj, &key);

  property_key_view_dispose(&key);
  return result;
}
