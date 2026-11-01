#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "gc.h"
#include "hash.h"
#include "shapes.h"
#include "stringify.h"
#include "arguments.h"
#include "arrays.h"
#include "keys.h"
#include "props.h"
#include "internal.h"
#include "protectors.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/proxy.h"
#include "modules/object.h"
#include "modules/function.h"
#include "modules/buffer.h"
#include "modules/date.h"

static descriptor_entry_t arr_length_desc = {
  .key = 0,
  .obj_off = 0,
  .prop_name = "length",
  .prop_len = 6,
  .writable = true,
  .enumerable = false,
  .configurable = false,
  .has_getter = false,
  .has_setter = false,
  .getter = 0,
  .setter = 0,
};

static descriptor_entry_t arr_length_readonly_desc = {
  .key = 0,
  .obj_off = 0,
  .prop_name = "length",
  .prop_len = 6,
  .writable = false,
  .enumerable = false,
  .configurable = false,
  .has_getter = false,
  .has_setter = false,
  .getter = 0,
  .setter = 0,
};

static inline bool is_canonical_desc_obj(ant_value_t obj) {
  return vtype(obj) == kTypeObject;
}

static inline bool is_exotic_desc_obj(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  return ptr && ptr->flags.is_exotic;
}

static inline bool desc_registry_allowed(ant_value_t obj) {
  if (is_exotic_desc_obj(obj)) return true;
#ifndef NDEBUG
  assert(false && "desc_registry is exotic-only; ordinary objects must use shape metadata");
#endif
  return false;
}

static bool ensure_added_shape_slot_storage(ant_t *js, ant_object_t *ptr, uint32_t slot) {
  if (!ptr || !ptr->shape) return false;
  if (!js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape))) return false;
  if (slot >= ptr->prop_count && !js_obj_ensure_prop_capacity(js, ptr, slot + 1)) return false;
  return true;
}

static inline uint8_t attrs_from_flags(int flags, bool include_writable) {
  uint8_t attrs = 0;
  if (include_writable && (flags & JS_DESC_W)) attrs |= ANT_PROP_ATTR_WRITABLE;
  if (flags & JS_DESC_E) attrs |= ANT_PROP_ATTR_ENUMERABLE;
  if (flags & JS_DESC_C) attrs |= ANT_PROP_ATTR_CONFIGURABLE;
  return attrs;
}

uint64_t make_desc_key(ant_value_t obj, const char *key, size_t klen) {
  assert(is_canonical_desc_obj(obj) && "descriptor APIs require canonical js_as_obj(...) value");
  if (!is_canonical_desc_obj(obj)) return hash_key(key, klen);
  uintptr_t obj_off = (uintptr_t)vdata(obj);
  uint64_t key_hash = hash_key(key, klen);
  uint64_t h = (uint64_t)obj_off ^ ((uint64_t)obj_off >> 33);
  h ^= key_hash + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t make_sym_desc_key(ant_value_t obj, ant_offset_t sym_off) {
  assert(is_canonical_desc_obj(obj) && "descriptor APIs require canonical js_as_obj(...) value");
  if (!is_canonical_desc_obj(obj)) return ((uint64_t)sym_off << 1) | 1u;
  uintptr_t obj_off = (uintptr_t)vdata(obj);
  uint64_t h = (uint64_t)obj_off ^ ((uint64_t)obj_off >> 33);
  h ^= ((uint64_t)sym_off << 1) | 1u;
  h += 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

static descriptor_entry_t *registry_lookup_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  descriptor_entry_t *entry = NULL;
  uint64_t desc_key = make_desc_key(obj, key, klen);
  HASH_FIND(hh, js->desc_registry, &desc_key, sizeof(uint64_t), entry);
  return entry;
}

static descriptor_entry_t *registry_lookup_sym_desc(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  descriptor_entry_t *entry = NULL;
  uint64_t k = make_sym_desc_key(obj, sym_off);
  HASH_FIND(hh, js->desc_registry, &k, sizeof(uint64_t), entry);
  return entry;
}

descriptor_entry_t *lookup_descriptor(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  assert(js && is_canonical_desc_obj(obj) && "lookup_descriptor expects an isolate and js_as_obj(...)");
  if (!js || !is_canonical_desc_obj(obj)) return NULL;

  ant_object_t *ptr = js_obj_ptr(obj);
  if (klen == 6 && memcmp(key, "length", 6) == 0 && ant_object_is_array(ptr)) {
    if (ptr->flags.frozen) return &arr_length_readonly_desc;
    descriptor_entry_t *own = ptr->flags.is_exotic ? registry_lookup_desc(js, obj, key, klen) : NULL;
    return own ? own : &arr_length_desc;
  }

  if (!is_exotic_desc_obj(obj)) return NULL;
  return registry_lookup_desc(js, obj, key, klen);
}


descriptor_entry_t *lookup_sym_descriptor(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  assert(js && is_canonical_desc_obj(obj) && "lookup_sym_descriptor expects an isolate and js_as_obj(...)");
  if (!js || !is_canonical_desc_obj(obj)) return NULL;

  if (!is_exotic_desc_obj(obj)) return NULL;
  return registry_lookup_sym_desc(js, obj, sym_off);
}

static descriptor_entry_t *get_or_create_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (!desc_registry_allowed(obj)) return NULL;
  descriptor_entry_t *entry = registry_lookup_desc(js, obj, key, klen);
  if (entry) return entry;

  entry = (descriptor_entry_t *)calloc(1, sizeof(descriptor_entry_t) + klen + 1);
  if (!entry) return NULL;

  entry->key = make_desc_key(obj, key, klen);
  entry->obj_off = (uintptr_t)vdata(obj);
  entry->prop_name = (char *)(entry + 1);
  memcpy(entry->prop_name, key, klen);
  entry->prop_name[klen] = '\0';
  entry->prop_len = klen;
  entry->writable = true;
  entry->enumerable = true;
  entry->configurable = true;
  entry->has_getter = false;
  entry->has_setter = false;
  entry->getter = Ant_Value_Undefined;
  entry->setter = Ant_Value_Undefined;

  HASH_ADD(hh, js->desc_registry, key, sizeof(uint64_t), entry);
  return entry;
}

static descriptor_entry_t *get_or_create_sym_desc(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  if (!desc_registry_allowed(obj)) return NULL;
  descriptor_entry_t *entry = registry_lookup_sym_desc(js, obj, sym_off);
  if (entry) return entry;

  entry = (descriptor_entry_t *)calloc(1, sizeof(descriptor_entry_t));
  if (!entry) return NULL;

  entry->key = make_sym_desc_key(obj, sym_off);
  entry->obj_off = (uintptr_t)vdata(obj);
  entry->sym_off = sym_off;
  entry->writable = true;
  entry->enumerable = true;
  entry->configurable = true;
  entry->has_getter = false;
  entry->has_setter = false;
  entry->getter = Ant_Value_Undefined;
  entry->setter = Ant_Value_Undefined;

  HASH_ADD(hh, js->desc_registry, key, sizeof(uint64_t), entry);
  return entry;
}

static bool ensure_string_shape_slot(ant_t *js, ant_value_t obj, const char *key, size_t klen, uint32_t *out_slot) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return false;

  const char *interned = intern_string(js, key, klen);
  if (!interned) return false;

  int32_t slot = ant_shape_lookup_interned(ptr->shape, interned);
  if (slot < 0) {
    uint32_t added_slot = 0;
    if (!ant_shape_add_interned_tr(js, &ptr->shape, interned, ANT_PROP_ATTR_DEFAULT, &added_slot)) return false;
    ant_object_invalidate_guarded_absence(js, ptr);
    if (!ensure_added_shape_slot_storage(js, ptr, added_slot)) return false;
    slot = (int32_t)added_slot;
  }

  if (out_slot) *out_slot = (uint32_t)slot;
  return true;
}

static bool ensure_symbol_shape_slot(ant_t *js, ant_value_t obj, ant_offset_t sym_off, uint32_t *out_slot) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return false;

  int32_t slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
  if (slot < 0) {
    ant_value_t sym = mkval(kTypeSymbol, sym_off);
    if (is_err(mkprop(js, obj, sym, Ant_Value_Undefined, 0))) return false;
    slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
    if (slot < 0) return false;
  }

  if (out_slot) *out_slot = (uint32_t)slot;
  return true;
}

static ant_shape_prop_t *ensure_string_shape_prop_at(
  ant_t *js, ant_value_t obj, const char *key, size_t klen, uint32_t *slot_out
) {
  uint32_t slot = 0;
  if (!ensure_string_shape_slot(js, obj, key, klen, &slot)) return NULL;
  if (slot_out) *slot_out = slot;
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return NULL;
  if (!js_obj_ensure_unique_shape(ptr)) return NULL;
  
  return ant_shape_prop_mut_at(ptr->shape, slot);
}

static ant_shape_prop_t *ensure_symbol_shape_prop_at(
  ant_t *js, ant_value_t obj, ant_offset_t sym_off, uint32_t *slot_out
) {
  uint32_t slot = 0;
  if (!ensure_symbol_shape_slot(js, obj, sym_off, &slot)) return NULL;
  if (slot_out) *slot_out = slot;
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return NULL;
  if (!js_obj_ensure_unique_shape(ptr)) return NULL;
  
  return ant_shape_prop_mut_at(ptr->shape, slot);
}

static inline ant_shape_prop_t *ensure_string_shape_prop(
  ant_t *js, ant_value_t obj, const char *key, size_t klen
) {
  return ensure_string_shape_prop_at(js, obj, key, klen, NULL);
}

static inline ant_shape_prop_t *ensure_symbol_shape_prop(
  ant_t *js, ant_value_t obj, ant_offset_t sym_off
) {
  return ensure_symbol_shape_prop_at(js, obj, sym_off, NULL);
}

static void clear_accessor_slot(ant_value_t obj, uint32_t slot) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (ptr && slot < ptr->prop_count) ant_object_prop_set_unchecked(ptr, slot, Ant_Value_Undefined);
}

static void apply_shape_desc_update(
  ant_shape_prop_t *prop,
  int flags,
  bool include_writable,
  bool set_getter_flag,
  bool has_getter,
  ant_value_t getter,
  bool set_setter_flag,
  bool has_setter,
  ant_value_t setter
) {
  if (!prop) return;
  prop->attrs = attrs_from_flags(flags, include_writable);
  if (set_getter_flag) {
    prop->has_getter = has_getter ? 1 : 0;
    prop->getter = has_getter ? getter : Ant_Value_Undefined;
  }
  if (set_setter_flag) {
    prop->has_setter = has_setter ? 1 : 0;
    prop->setter = has_setter ? setter : Ant_Value_Undefined;
  }
}

static void apply_registry_desc_update(
  descriptor_entry_t *entry,
  int flags,
  bool include_writable,
  bool set_getter_flag,
  bool has_getter,
  ant_value_t getter,
  bool set_setter_flag,
  bool has_setter,
  ant_value_t setter
) {
  if (!entry) return;
  if (include_writable) entry->writable = (flags & JS_DESC_W) != 0;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  if (set_getter_flag) {
    entry->has_getter = has_getter;
    entry->getter = has_getter ? getter : Ant_Value_Undefined;
  }
  if (set_setter_flag) {
    entry->has_setter = has_setter;
    entry->setter = has_setter ? setter : Ant_Value_Undefined;
  }
}

void js_set_descriptor(ant_t *js, ant_value_t obj, const char *key, size_t klen, int flags) {
  assert(is_canonical_desc_obj(obj) && "js_set_descriptor expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return;
  ant_shape_prop_t *prop = ensure_string_shape_prop(js, obj, key, klen);

  if (prop) {
    apply_shape_desc_update(
      prop, flags, true,
      false, false, Ant_Value_Undefined,
      false, false, Ant_Value_Undefined
    );

    ant_ic_epoch_bump(js);
    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);

  apply_registry_desc_update(
    entry, flags, true,
    false, false, Ant_Value_Undefined,
    false, false, Ant_Value_Undefined
  );
}

void js_set_sym_descriptor(ant_t *js, ant_value_t obj, ant_value_t sym, int flags) {
  assert(is_canonical_desc_obj(obj) && "js_set_sym_descriptor expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj) || vtype(sym) != kTypeSymbol) return;
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);
  ant_shape_prop_t *prop = ensure_symbol_shape_prop(js, obj, sym_off);

  if (prop) {
    apply_shape_desc_update(
      prop, flags, true,
      false, false, Ant_Value_Undefined,
      false, false, Ant_Value_Undefined
    );

    ant_symbol_property_mutation_invalidate(js, js_obj_ptr(obj), sym_off);
    ant_ic_epoch_bump(js);
    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_sym_desc(js, obj, sym_off);

  apply_registry_desc_update(
    entry, flags, true,
    false, false, Ant_Value_Undefined,
    false, false, Ant_Value_Undefined
  );
}

void js_set_getter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, int flags) {
  getter = js_cfunc_expose_property(js, getter, JS_CFUNC_GETTER, key, klen, false);
  assert(is_canonical_desc_obj(obj) && "js_set_getter_desc expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return;
  uint32_t slot = 0;
  ant_shape_prop_t *prop = ensure_string_shape_prop_at(js, obj, key, klen, &slot);

  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      true, true, getter,
      false, false, Ant_Value_Undefined
    );
    clear_accessor_slot(obj, slot);

    ant_property_mutation_invalidate(
      js, js_obj_ptr(obj),
      prop->key.interned
    );

    ant_ic_epoch_bump(js);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), getter);

    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);

  apply_registry_desc_update(
    entry, flags, false,
    true, true, getter,
    false, false, Ant_Value_Undefined
  );
}

void js_set_setter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t setter, int flags) {
  setter = js_cfunc_expose_property(js, setter, JS_CFUNC_SETTER, key, klen, false);
  assert(is_canonical_desc_obj(obj) && "js_set_setter_desc expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return;
  
  uint32_t slot = 0;
  ant_shape_prop_t *prop = ensure_string_shape_prop_at(js, obj, key, klen, &slot);

  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      false, false, Ant_Value_Undefined,
      true, true, setter
    );
    
    clear_accessor_slot(obj, slot);
    
    ant_property_mutation_invalidate(
      js, js_obj_ptr(obj),
      prop->key.interned
    );

    ant_ic_epoch_bump(js);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), setter);

    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);

  apply_registry_desc_update(
    entry, flags, false,
    false, false, Ant_Value_Undefined,
    true, true, setter
  );
}

void js_define_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags) {
  getter = js_cfunc_expose_property(js, getter, JS_CFUNC_GETTER, key, klen, false);
  setter = js_cfunc_expose_property(js, setter, JS_CFUNC_SETTER, key, klen, false);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  const char *interned = ptr && ptr->shape && !ptr->flags.is_exotic ? intern_string(js, key, klen) : NULL;
  
  uint32_t slot = 0;
  if (
    !interned || ant_shape_lookup_interned(ptr->shape, interned) >= 0 ||
    !ant_shape_add_accessor_tr(js, &ptr->shape, interned, attrs_from_flags(flags, false), getter, setter, &slot)
  ) {
    js_set_accessor_desc(js, obj, key, klen, getter, setter, flags);
    return;
  }

  ant_object_invalidate_guarded_absence(js, ptr);
  if (!ensure_added_shape_slot_storage(js, ptr, slot)) return;
  
  ant_object_prop_set_unchecked(ptr, slot, Ant_Value_Undefined);
  ant_property_mutation_invalidate(js, ptr, interned);
  gc_write_barrier(js, ptr, getter);
  gc_write_barrier(js, ptr, setter);
}

void js_set_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags) {
  getter = js_cfunc_expose_property(js, getter, JS_CFUNC_GETTER, key, klen, false);
  setter = js_cfunc_expose_property(js, setter, JS_CFUNC_SETTER, key, klen, false);
  assert(is_canonical_desc_obj(obj) && "js_set_accessor_desc expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return;
  uint32_t slot = 0;
  ant_shape_prop_t *prop = ensure_string_shape_prop_at(js, obj, key, klen, &slot);

  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      true, true, getter,
      true, true, setter
    );
    clear_accessor_slot(obj, slot);

    ant_property_mutation_invalidate(
      js, js_obj_ptr(obj),
      prop->key.interned
    );

    ant_ic_epoch_bump(js);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), getter);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), setter);

    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);

  apply_registry_desc_update(
    entry, flags, false,
    true, true, getter,
    true, true, setter
  );
}

void js_set_sym_getter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t getter, int flags) {
  const char *desc = vtype(sym) == kTypeSymbol ? js_sym_desc(sym) : NULL;
  getter = js_cfunc_expose_property(js, getter, JS_CFUNC_GETTER, desc, desc ? strlen(desc) : 0, true);
  assert(is_canonical_desc_obj(obj) && "js_set_sym_getter_desc expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return;
  if (vtype(sym) != kTypeSymbol) return;

  ant_offset_t sym_off = (ant_offset_t)vdata(sym);
  uint32_t slot = 0;
  ant_shape_prop_t *prop = ensure_symbol_shape_prop_at(js, obj, sym_off, &slot);

  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      true, true, getter,
      false, false, Ant_Value_Undefined
    );
    clear_accessor_slot(obj, slot);

    ant_symbol_property_mutation_invalidate(js, js_obj_ptr(obj), sym_off);
    ant_ic_epoch_bump(js);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), getter);

    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_sym_desc(js, obj, sym_off);

  apply_registry_desc_update(
    entry, flags, false,
    true, true, getter,
    false, false, Ant_Value_Undefined
  );
}

void js_set_sym_setter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t setter, int flags) {
  const char *desc = vtype(sym) == kTypeSymbol ? js_sym_desc(sym) : NULL;
  setter = js_cfunc_expose_property(js, setter, JS_CFUNC_SETTER, desc, desc ? strlen(desc) : 0, true);
  assert(is_canonical_desc_obj(obj) && "js_set_sym_setter_desc expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return;
  if (vtype(sym) != kTypeSymbol) return;

  ant_offset_t sym_off = (ant_offset_t)vdata(sym);
  uint32_t slot = 0;
  ant_shape_prop_t *prop = ensure_symbol_shape_prop_at(js, obj, sym_off, &slot);

  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      false, false, Ant_Value_Undefined,
      true, true, setter
    );
    clear_accessor_slot(obj, slot);

    ant_symbol_property_mutation_invalidate(js, js_obj_ptr(obj), sym_off);
    ant_ic_epoch_bump(js);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), setter);

    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_sym_desc(js, obj, sym_off);

  apply_registry_desc_update(
    entry, flags, false,
    false, false, Ant_Value_Undefined,
    true, true, setter
  );
}

void js_descriptor_registry_cleanup(ant_t *js) {
  if (!js) return;

  descriptor_entry_t *entry, *tmp;
  HASH_ITER(hh, js->desc_registry, entry, tmp) {
    HASH_DEL(js->desc_registry, entry);
    free(entry);
  }
}

bool js_array_make_length_readonly(ant_t *js, ant_value_t arr) {
  ant_value_t obj = js_as_obj(arr);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != kTypeArray) return false;
  
  ptr->flags.is_exotic = 1;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, "length", 6);
  if (!entry) return false;
  
  entry->writable = false;
  entry->enumerable = false;
  entry->configurable = false;
  ant_ic_epoch_bump(js);
  
  return true;
}

static bool define_lookup_existing_meta(
  ant_t *js, ant_value_t obj,
  bool sym_key,
  ant_offset_t sym_off,
  const char *prop_str,
  ant_offset_t prop_len,
  ant_prop_loc_t existing,
  prop_meta_t *out
) {
  if (sym_key && lookup_symbol_prop_meta(js, obj, sym_off, out)) return true;
  if (!sym_key && lookup_string_prop_meta(js, obj, prop_str, (size_t)prop_len, out)) return true;
  if (!existing.obj || !existing.obj->shape) return false;

  const ant_shape_prop_t *prop = ant_shape_prop_at(existing.obj->shape, existing.slot);
  if (!prop) return false;

  prop_meta_from_shape(out, prop);
  return true;
}

static bool define_desc_field(
  ant_t *js, ant_value_t descriptor, const char *name, size_t name_len,
  bool *has_out, ant_value_t *val_out, ant_value_t *err_out
) {
  ant_prop_loc_t off = lkp(js, descriptor, name, name_len);
  if (off.obj) {
    const ant_shape_prop_t *meta = prop_shape_meta(off);
    if (!meta || (!meta->has_getter && !meta->has_setter)) {
      *has_out = true;
      *val_out = js_prop_load(off);
      return true;
    }
  } else if (!lkp_proto(js, descriptor, name, name_len).obj) return true;

  ant_value_t v = js_getprop_fallback_len(js, descriptor, name, name_len);

  if (is_err(v)) {
    *err_out = v;
    return false;
  }

  if (Ant_Exception_Pending(js)) {
    *err_out = js_throw(js, js_take_thrown(js, Ant_Value_Undefined));
    return false;
  }

  *has_out = true;
  *val_out = v;
  
  return true;
}

static bool define_desc_read_plain(ant_t *js, ant_value_t descriptor, define_desc_raw_t *raw) {
  ant_object_t *ptr = js_obj_ptr(descriptor);
  if (!ptr || ptr->flags.is_exotic || !ptr->shape) return false;

  const char *names[DESC_FIELD_COUNT] = {
    js->intern.enumerable,
    js->intern.configurable,
    js->intern.value,
    js->intern.writable,
    js->intern.get,
    js->intern.set,
  };

  bool missing = false;
  for (int f = 0; f < DESC_FIELD_COUNT; f++) {
    int32_t slot = ant_shape_lookup_interned(ptr->shape, names[f]);
    const ant_shape_prop_t *prop = slot >= 0 ? ant_shape_prop_at(ptr->shape, (uint32_t)slot) : NULL;
    if (!prop) { missing = true; continue; }
    if (prop->has_getter || prop->has_setter) return false;
    raw->has[f] = true;
    raw->val[f] = ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
  }

  int depth = 0;
  for (ant_value_t proto = ptr->proto; missing && is_object_type(proto);) {
    const ant_object_t *p = js_obj_ptr(proto);
    if (!p || p->flags.is_exotic || !p->shape || ++depth > MAX_PROTO_CHAIN_DEPTH) return false;
    for (int f = 0; f < DESC_FIELD_COUNT; f++)
      if (!raw->has[f] && ant_shape_lookup_interned(p->shape, names[f]) >= 0) return false;
    proto = p->proto;
  }
  
  return true;
}

static inline bool define_desc_accessor_invalid(ant_value_t fn) {
  uint8_t t = vtype(fn);
  return t != kTypeFunction && t != kTypeBuiltin && t != kTypeUndefined;
}

static const struct { const char *name; size_t len; } define_desc_field_names[DESC_FIELD_COUNT] = {
  { "enumerable", 10 },
  { "configurable", 12 },
  { "value", 5 },
  { "writable", 8 },
  { "get", 3 },
  { "set", 3 },
};

static ant_value_t describe_join_omitted(ant_t *js, ant_value_t head, ant_value_t tail) {
  static const char omitted[] = "...<omitted>...";
  size_t head_len = 0, tail_len = 0;
  
  const char *head_bytes = js_getstr(js, head, &head_len);
  const char *tail_bytes = js_getstr(js, tail, &tail_len);
  size_t total = head_len + sizeof(omitted) - 1 + tail_len;

  char *buf = malloc(total);
  if (!buf) return js_mkerr(js, "oom");
  memcpy(buf, head_bytes, head_len);
  memcpy(buf + head_len, omitted, sizeof(omitted) - 1);
  memcpy(buf + head_len + sizeof(omitted) - 1, tail_bytes, tail_len);

  ant_value_t result = js_mkstr(js, buf, total);
  free(buf);
  
  return result;
}

static ant_value_t describe_function(ant_t *js, ant_value_t fn) {
  ant_value_t source = Ant_Function_ToString(js, fn);
  if (is_err(source)) return source;

  double units = (double)str_utf16_len(js, source);
  if (units <= 128) return source;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, source);
  
  ant_value_t head = js_string_slice(js, source, 0, 111);
  GC_ROOT_PIN(js, head);
  
  ant_value_t tail = is_err(head) ? head : js_string_slice(js, source, units - 2, units);
  ant_value_t result = is_err(tail) ? tail : describe_join_omitted(js, head, tail);
  GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

static ant_value_t describe_data_prop(ant_t *js, ant_value_t obj, const char *interned, ant_value_t sym) {
  ant_value_t cur = obj;
  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    ant_value_t as_obj = js_as_obj(cur);
    if (is_proxy(as_obj)) return Ant_Value_Undefined;

    ant_prop_loc_t loc = vtype(sym) == kTypeSymbol
      ? lkp_sym(as_obj, (ant_offset_t)vdata(sym))
      : lkp_interned(as_obj, interned);

    if (loc.obj) {
      const ant_shape_prop_t *meta = ant_shape_prop_at(loc.obj->shape, loc.slot);
      return meta && (meta->has_getter || meta->has_setter) ? Ant_Value_Undefined : js_prop_load(loc);
    }
    cur = js_get_proto(js, cur);
  }
  return Ant_Value_Undefined;
}

static ant_value_t describe_text(ant_t *js, const char *a, size_t a_len, const char *b, size_t b_len, const char *c, size_t c_len) {
  size_t total = a_len + b_len + c_len;
  char stack_buf[160];
  char *buf = total <= sizeof(stack_buf) ? stack_buf : malloc(total);
  if (!buf) return js_mkerr(js, "oom");
  memcpy(buf, a, a_len);
  memcpy(buf + a_len, b, b_len);
  memcpy(buf + a_len + b_len, c, c_len);
  ant_value_t result = js_mkstr(js, buf, total);
  if (buf != stack_buf) free(buf);
  return result;
}

static ant_value_t describe_error(ant_t *js, ant_value_t value) {
  ant_value_t name = describe_data_prop(js, value, js->intern.name, Ant_Value_Undefined);
  ant_value_t message = describe_data_prop(js, value, js->intern.message, Ant_Value_Undefined);

  size_t name_len = 5, message_len = 0;
  const char *name_bytes = vtype(name) == kTypeString ? js_getstr(js, name, &name_len) : "Error";
  const char *message_bytes = vtype(message) == kTypeString ? js_getstr(js, message, &message_len) : "";

  if (name_len == 0) return describe_text(js, message_bytes, message_len, "", 0, "", 0);
  if (message_len == 0) return describe_text(js, name_bytes, name_len, "", 0, "", 0);
  return describe_text(js, name_bytes, name_len, ": ", 2, message_bytes, message_len);
}

static const char *describe_class_name(ant_value_t value) {
  TypedArrayData *typed = buffer_get_typedarray_data(value);
  if (typed) return buffer_typedarray_type_name(typed->type);
  if (vtype(value) == kTypeArray) return js_is_arguments(value) ? "Arguments" : "Array";

  ant_object_t *ptr = js_obj_ptr(js_as_obj(value));
  if (ptr && ptr->flags.regexp_brand) return "RegExp";
  if (is_date_instance(value)) return "Date";

  switch (vtype(js_get_slot(value, SLOT_PRIMITIVE))) {
    case kTypeNumber: return "Number";
    case kTypeString: return "String";
    case kTypeBool: return "Boolean";
    case kTypeBigInt: return "BigInt";
    case kTypeSymbol: return "Symbol";
    default: return "Object";
  }
}

static bool describe_is_object_to_string(ant_value_t fn) {
  return vtype(fn) == kTypeBuiltin && js_as_cfunc_meta(fn)->fn == builtin_object_toString;
}

static ant_value_t describe_object(ant_t *js, ant_value_t value) {
  if (js_get_slot(value, SLOT_ERROR_BRAND) == Ant_Value_True) return describe_error(js, value);

  ant_value_t to_string = describe_data_prop(js, value, intern_string(js, "toString", 8), Ant_Value_Undefined);
  if (describe_is_object_to_string(to_string)) {
    ant_value_t ctor = describe_data_prop(js, value, js->intern.constructor, Ant_Value_Undefined);
    ant_offset_t name_len = 0;
    const char *name = is_callable(ctor) ? get_func_name(js, ctor, &name_len) : NULL;
    if (name && name_len > 0) return describe_text(js, "#<", 2, name, (size_t)name_len, ">", 1);
  }

  ant_value_t tag = describe_data_prop(js, value, NULL, js->sym.toStringTag_sym);
  size_t tag_len = 0;
  const char *tag_bytes = vtype(tag) == kTypeString ? js_getstr(js, tag, &tag_len) : NULL;
  if (!tag_bytes) {
    tag_bytes = describe_class_name(value);
    tag_len = strlen(tag_bytes);
  }

  return describe_text(js, "[object ", 8, tag_bytes, tag_len, "]", 1);
}

ant_value_t Ant_Value_Describe(ant_t *js, ant_value_t value) {
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH && is_object_type(value) && is_proxy(js_as_obj(value)); depth++) {
    ant_proxy_state_t *data = get_proxy_data(js_as_obj(value));
    if (!data || data->revoked || !is_object_type(data->target)) break;
    value = data->target;
  }

  uint8_t t = vtype(value);
  if (t == kTypeFunction || t == kTypeBuiltin) return describe_function(js, value);
  if (is_object_type(value)) return describe_object(js, value);
  
  char stack_buf[96];
  js_cstr_t text = js_to_cstr(js, value, stack_buf, sizeof(stack_buf));
  
  ant_value_t result = js_mkstr(js, text.ptr ? text.ptr : "", text.ptr ? text.len : 0);
  if (text.needs_free) free((void *)text.ptr);
  
  return result;
}

static __attribute__((noinline, cold)) ant_value_t descriptor_value_error(ant_t *js, const char *prefix, ant_value_t value) {
  ant_value_t text = Ant_Value_Describe(js, value);
  if (is_err(text)) return text;
  size_t len = 0;
  const char *bytes = js_getstr(js, text, &len);
  return js_mkerr(js, "%s%.*s", prefix, (int)len, bytes);
}

static ant_value_t define_desc_check(ant_t *js, ant_value_t descriptor, const define_desc_raw_t *raw) {
  if (raw->has[DESC_GET] && define_desc_accessor_invalid(raw->val[DESC_GET]))
    return descriptor_value_error(js, "Getter must be a function: ", raw->val[DESC_GET]);
  if (raw->has[DESC_SET] && define_desc_accessor_invalid(raw->val[DESC_SET]))
    return descriptor_value_error(js, "Setter must be a function: ", raw->val[DESC_SET]);
  if ((raw->has[DESC_VALUE] || raw->has[DESC_WRITABLE]) && (raw->has[DESC_GET] || raw->has[DESC_SET]))
    return descriptor_value_error(js, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute, ", descriptor);
  return Ant_Value_Undefined;
}

ant_value_t define_desc_read(ant_t *js, ant_value_t descriptor, define_desc_raw_t *raw) {
  const __typeof__(define_desc_field_names[0]) *fields = define_desc_field_names;

  bool proxy = is_proxy(descriptor);
  if (proxy || !define_desc_read_plain(js, descriptor, raw)) for (int f = 0; f < DESC_FIELD_COUNT; f++) {
    ant_value_t err = 0;
    
    if (proxy) {
      ant_value_t has = proxy_has(js, descriptor, fields[f].name, fields[f].len);
      
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) continue;
      
      ant_value_t val = proxy_get(js, descriptor, fields[f].name, fields[f].len);
      if (is_err(val)) return val;
      
      raw->has[f] = true;
      raw->val[f] = val;
    } 
    
    else if (!define_desc_field(js, descriptor, fields[f].name, fields[f].len, &raw->has[f], &raw->val[f], &err)) return err;
    if (f == DESC_GET && raw->has[f] && define_desc_accessor_invalid(raw->val[f]))
      return descriptor_value_error(js, "Getter must be a function: ", raw->val[f]);
  }

  return define_desc_check(js, descriptor, raw);
}

static void array_materialize_dense_for_define(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (!key || !array_obj_ptr(obj)) return;

  unsigned long idx = 0;
  if (!parse_array_index(key, klen, ((ant_offset_t)UINT32_MAX), &idx)) return;

  ant_offset_t doff = get_dense_buf(obj);
  ant_offset_t cap = doff ? dense_capacity(doff) : 0;

  for (ant_offset_t i = 0; i < cap; i++) {
    ant_value_t existing = dense_get(doff, i);
    if (is_empty_slot(existing)) continue;

    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);

    dense_set(js, doff, i, T_EMPTY);
    mkprop(
      js, obj, js_mkstr(js, idxstr, idxlen), existing,
      ANT_PROP_ATTR_ENUMERABLE | ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE
    );
  }

  ant_object_t *ptr = array_obj_ptr(obj);
  if (ptr) ptr->flags.fast_array = 0;
  array_mark_may_have_holes(obj);
}

static __attribute__((noinline)) bool string_wrapper_define(
  ant_t *js, ant_value_t obj, ant_value_t as_obj, const char *key, size_t key_len,
  const define_desc_fields_t *desc, ant_value_t *result
) {
  ant_value_t current = Ant_Value_Undefined;
  bool is_length = false;
  if (!js_try_get_string_own_exotic(js, as_obj, key, key_len, &current, &is_length)) return false;

  bool compatible =
    !desc->accessor && !(desc->has_writable && desc->writable) &&
    !(desc->has_configurable && desc->configurable) &&
    !(desc->has_enumerable && desc->enumerable == is_length) &&
    !(desc->has_value && !same_value_values(js, desc->value, current));
    
  *result = compatible ? obj : js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine property: %.*s", (int)key_len, key);
  return true;
}

static ant_value_t define_desc_object(ant_t *js, const define_desc_raw_t *raw) {
  GC_ROOT_SAVE(mark, js);
  define_desc_raw_t pinned = *raw;
  for (int f = 0; f < DESC_FIELD_COUNT; f++) GC_ROOT_PIN(js, pinned.val[f]);
  
  ant_value_t object = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, object);
  
  for (int f = 0; f < DESC_FIELD_COUNT && !is_err(object); f++) if (pinned.has[f]) {
    ant_value_t key = js_mkstr(js, define_desc_field_names[f].name, define_desc_field_names[f].len);
    ant_value_t set = js_setprop(js, object, key, pinned.val[f]);
    if (is_err(set)) object = set;
  }
  
  GC_ROOT_RESTORE(js, mark);
  return object;
}

typedef struct {
  ant_value_t obj;
  ant_value_t as_obj;
  ant_value_t prop;
  const char *str;
  ant_offset_t len;
  ant_offset_t sym_off;
  bool sym_key;
} define_target_t;

typedef struct {
  bool has_value, has_get, has_set;
  bool has_writable, has_enumerable, has_configurable;
  bool writable, enumerable, configurable;
  ant_value_t value, getter, setter;
} define_desc_t;

typedef struct {
  ant_prop_loc_t loc;
  prop_meta_t meta;
  ant_object_t *ptr;
  bool has_meta;
  bool found;
} define_existing_t;

static ant_value_t define_via_proxy(
  ant_t *js, ant_value_t obj, ant_value_t as_obj, property_key_view_t *key,
  ant_value_t descriptor, const define_desc_raw_t *pre_raw
) {
  if (!is_object_type(descriptor)) descriptor = define_desc_object(js, pre_raw);
  if (is_err(descriptor)) return descriptor;
  
  ant_value_t prop = property_key_view_materialize(js, key);
  if (is_err(prop)) return prop;
  
  ant_value_t result = proxy_define_property(js, as_obj, prop, descriptor);
  if (is_err(result)) return result;
  if (!js_truthy(js, result)) return proxy_trap_error(js, "defineProperty", "trap returned falsy");
  
  return obj;
}

static define_target_t define_target_init(ant_value_t obj, ant_value_t as_obj, const property_key_view_t *key) {
  define_target_t t = { .obj = obj, .as_obj = as_obj, .prop = key->js_key, .sym_key = key->is_symbol };
  
  if (t.sym_key) {
    t.sym_off = (ant_offset_t)vdata(t.prop);
    const char *desc = js_sym_desc(t.prop);
    t.str = desc ? desc : "symbol";
    t.len = (ant_offset_t)strlen(t.str);
  } else {
    t.str = key->bytes;
    t.len = (ant_offset_t)key->length;
  }
  
  return t;
}

static ant_value_t define_desc_parse(
  ant_t *js, ant_value_t descriptor, const define_desc_raw_t *pre_raw, define_desc_t *d
) {
  define_desc_raw_t raw = {0};
  if (pre_raw) raw = *pre_raw;
  else {
    ant_value_t read_err = define_desc_read(js, descriptor, &raw);
    if (is_err(read_err)) return read_err;
  }

  *d = (define_desc_t){
    .has_value = raw.has[DESC_VALUE],
    .has_get = raw.has[DESC_GET],
    .has_set = raw.has[DESC_SET],
    .has_writable = raw.has[DESC_WRITABLE],
    .has_enumerable = raw.has[DESC_ENUMERABLE],
    .has_configurable = raw.has[DESC_CONFIGURABLE],
    .value = raw.has[DESC_VALUE] ? raw.val[DESC_VALUE] : Ant_Value_Undefined,
    .getter = raw.has[DESC_GET] ? raw.val[DESC_GET] : Ant_Value_Undefined,
    .setter = raw.has[DESC_SET] ? raw.val[DESC_SET] : Ant_Value_Undefined,
  };
  
  d->enumerable = d->has_enumerable && js_truthy(js, raw.val[DESC_ENUMERABLE]);
  d->configurable = d->has_configurable && js_truthy(js, raw.val[DESC_CONFIGURABLE]);
  d->writable = d->has_writable && js_truthy(js, raw.val[DESC_WRITABLE]);

  if ((d->has_value || d->has_writable) && (d->has_get || d->has_set))
    return descriptor_value_error(js, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute, ", descriptor);
  
  return Ant_Value_Undefined;
}

static inline define_desc_fields_t define_desc_fields(const define_desc_t *d) {
  return (define_desc_fields_t){
    .accessor = d->has_get || d->has_set, .has_value = d->has_value,
    .has_writable = d->has_writable, .writable = d->writable,
    .has_enumerable = d->has_enumerable, .enumerable = d->enumerable,
    .has_configurable = d->has_configurable, .configurable = d->configurable,
    .value = d->value,
  };
}

static inline int define_desc_flags(const define_desc_t *d, bool include_writable) {
  return
    (include_writable && d->writable ? JS_DESC_W : 0) |
    (d->enumerable ? JS_DESC_E : 0) |
    (d->configurable ? JS_DESC_C : 0);
}

static __attribute__((noinline, cold)) ant_value_t define_redefine_error(
  ant_t *js, const char *str, ant_offset_t len, const char *reason
) {
  return js_mkerr(js, "Cannot redefine property %.*s: %s", (int)len, str, reason);
}

static inline ant_value_t define_done(ant_t *js, const define_target_t *t) {
  if (!t->sym_key) array_define_or_set_index(js, t->as_obj, t->str, (size_t)t->len);
  return t->obj;
}

static bool define_exotic(
  ant_t *js, const define_target_t *t, const define_desc_t *d,
  define_desc_fields_t *fields, ant_value_t *result
) {
  const ant_object_t *ptr = js_obj_ptr(t->as_obj);
  if (t->sym_key || !ptr || !ptr->flags.is_exotic || !ptr->exotic_ops || !ptr->exotic_ops->definer) return false;
  
  bool plain_data =
    !d->has_get && !d->has_set && !(d->has_configurable && !d->configurable) &&
    !(d->has_enumerable && !d->enumerable) && !(d->has_writable && !d->writable);
    
  ant_value_t defined = ptr->exotic_ops->definer(
    js, t->as_obj, t->str, (size_t)t->len, 
    d->value, d->has_value, plain_data
  );
  
  if (defined == T_EMPTY) *result = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot define property %.*s", (int)t->len, t->str);
  else if (is_err(defined)) *result = define_abrupt(fields, defined);
  else if (vtype(defined) != kTypeUndefined) *result = t->obj;
  else return false;
  
  return true;
}

static ant_value_t define_array_length(
  ant_t *js, const define_target_t *t, const define_desc_t *d,
  define_desc_fields_t *fields, ant_object_t *arr_ptr
) {
  ant_value_t as_obj = t->as_obj;
  if (d->has_get || d->has_set) return js_mkerr(js, "Invalid property descriptor. Cannot use accessors for array length");
  if ((d->has_enumerable && d->enumerable) || (d->has_configurable && d->configurable))
    return js_mkerr(js, "Cannot redefine array length attributes");

  ant_offset_t new_len = (ant_offset_t)arr_ptr->u.array.len;
  if (d->has_value) {
    ant_value_t value = d->value;
    if (vtype(value) != kTypeNumber) {
      double number = js_to_number(js, value);
      if (Ant_Exception_Pending(js)) return define_abrupt(fields, Ant_Exception_Current(js));
      value = Ant_Value_FromNumber(number);
    }
    ant_value_t len_err = validate_array_length(js, value);
    if (is_err(len_err)) return define_abrupt(fields, len_err);
    new_len = (ant_offset_t)Ant_Value_AsNumberUnchecked(value);
  }
  
  bool make_readonly = d->has_writable && !d->writable;
  if (array_length_readonly(js, as_obj)) {
    if ((d->has_writable && d->writable) || new_len != get_array_length(js, as_obj))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine property: length");
    return t->obj;
  }

  if (__builtin_expect(!array_length_has_guarded_elements(arr_ptr), 1)) {
    array_length_set_unguarded(js, as_obj, new_len);
    if (make_readonly && !js_array_make_length_readonly(js, as_obj)) return js_mkerr(js, "oom");
    return t->obj;
  }

  ant_offset_t final_len = new_len;
  ant_value_t truncated = array_truncate_length(js, as_obj, new_len, &final_len);
  
  if (is_err(truncated)) return define_abrupt(fields, truncated);
  if (make_readonly && !js_array_make_length_readonly(js, as_obj)) return js_mkerr(js, "oom");
  if (final_len != new_len) return array_truncate_stopped_error(js, final_len);
  
  return t->obj;
}

static bool define_array_index(ant_t *js, const define_target_t *t, const define_desc_t *d, ant_value_t *result) {
  ant_value_t as_obj = t->as_obj;
  unsigned long idx = 0;
  
  if (
    real_array_obj_ptr(as_obj) &&
    parse_array_index(t->str, (size_t)t->len, ((ant_offset_t)UINT32_MAX), &idx) &&
    (ant_offset_t)idx >= get_array_length(js, as_obj) && array_length_readonly(js, as_obj)
  ) {
    *result = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot define property %lu, object is not extensible", idx);
    return true;
  }
  
  bool plain_data =
    !d->has_get && !d->has_set && d->has_value &&
    d->has_writable && d->writable &&
    d->has_enumerable && d->enumerable &&
    d->has_configurable && d->configurable;
  if (!plain_data) return false;
  
  ant_object_t *arr = array_obj_ptr(as_obj);
  if (
    !arr || !arr->flags.fast_array ||
    !arr->flags.extensible || arr->flags.sealed || arr->flags.frozen ||
    !parse_array_index(t->str, (size_t)t->len, ((ant_offset_t)UINT32_MAX), &idx)
  ) return false;
  
  ant_offset_t doff = get_dense_buf(as_obj);
  if (!doff || (ant_offset_t)idx >= dense_capacity(doff)) return false;
  
  dense_set(js, doff, (ant_offset_t)idx, d->value);
  *result = define_done(js, t);
  
  return true;
}

static define_existing_t define_lookup_existing(ant_t *js, const define_target_t *t) {
  define_existing_t e;
  e.loc = t->sym_key ? lkp_sym(t->as_obj, t->sym_off) : lkp(js, t->as_obj, t->str, t->len);
  
  e.has_meta = define_lookup_existing_meta(
    js, t->as_obj, t->sym_key, t->sym_off, 
    t->str, t->len, e.loc, &e.meta
  );
  
  e.found = e.loc.obj || e.has_meta;
  e.ptr = js_obj_ptr(t->as_obj);
  
  return e;
}

static ant_value_t define_validate_existing(
  ant_t *js, const define_target_t *t, const define_desc_t *d, const define_existing_t *e
) {
  const ant_object_t *ptr = e->ptr;
  if (!e->found) {
    if (ptr && (ptr->flags.frozen || ptr->flags.sealed || !ptr->flags.extensible))
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)t->len, t->str);
    return Ant_Value_Undefined;
  }

  bool frozen = ptr && ptr->flags.frozen;
  bool nonconfig =
    (e->has_meta && !e->meta.configurable) ||
    (e->loc.obj && is_nonconfig_prop(e->loc)) ||
    (ptr && ptr->flags.sealed) || frozen;
  if (!nonconfig) return Ant_Value_Undefined;
  
  bool accessor = e->has_meta && (e->meta.has_getter || e->meta.has_setter);
  bool readonly =
    (e->has_meta && !accessor && !e->meta.writable) ||
    (e->loc.obj && is_const_prop(e->loc)) || frozen;

  if (d->has_configurable && d->configurable)
    return define_redefine_error(js, t->str, t->len, "cannot change configurable from false to true");

  if (e->has_meta) {
    if (d->has_enumerable && d->enumerable != e->meta.enumerable)
      return define_redefine_error(js, t->str, t->len, "cannot change enumerable of a non-configurable property");
    if (!e->meta.writable && d->has_writable && d->writable)
      return define_redefine_error(js, t->str, t->len, "cannot change writable from false to true");
    if (accessor && (d->has_value || d->has_writable))
      return define_redefine_error(js, t->str, t->len, "cannot convert accessor property to data property");
    if (!accessor && (d->has_get || d->has_set))
      return define_redefine_error(js, t->str, t->len, "cannot convert data property to accessor property");
    if (accessor && d->has_get && d->getter != (e->meta.has_getter ? e->meta.getter : Ant_Value_Undefined))
      return define_redefine_error(js, t->str, t->len, "cannot replace getter of a non-configurable property");
    if (accessor && d->has_set && d->setter != (e->meta.has_setter ? e->meta.setter : Ant_Value_Undefined))
      return define_redefine_error(js, t->str, t->len, "cannot replace setter of a non-configurable property");
  }

  if (readonly && d->has_writable && d->writable)
    return define_redefine_error(js, t->str, t->len, "cannot change writable from false to true");
  if (readonly && d->has_value && (!e->loc.obj || !same_value_values(js, js_prop_load(e->loc), d->value)))
    return js_mkerr(js, "Cannot assign to read-only property '%.*s'", (int)t->len, t->str);
  
  return Ant_Value_Undefined;
}

static inline __attribute__((always_inline)) void define_set_accessor(
  ant_t *js, const define_target_t *t,
  bool has_get, ant_value_t getter, bool has_set, ant_value_t setter, int desc_flags
) {
  if (t->sym_key) {
    if (has_get) js_set_sym_getter_desc(js, t->as_obj, t->prop, getter, desc_flags);
    if (has_set) js_set_sym_setter_desc(js, t->as_obj, t->prop, setter, desc_flags);
    return;
  }

  if (has_get && has_set) js_set_accessor_desc(js, t->as_obj, t->str, t->len, getter, setter, desc_flags);
  else if (has_get) js_set_getter_desc(js, t->as_obj, t->str, t->len, getter, desc_flags);
  else js_set_setter_desc(js, t->as_obj, t->str, t->len, setter, desc_flags);
}

static ant_value_t define_update_attrs(
  ant_t *js, const define_target_t *t, const define_desc_t *d, const define_existing_t *e
) {
  int desc_flags = define_desc_flags(d, true);
  const prop_meta_t *m = &e->meta;

  if (e->has_meta && (m->has_getter || m->has_setter))
    define_set_accessor(js, t, m->has_getter, m->getter, m->has_setter, m->setter, desc_flags);
  else if (!t->sym_key)
    js_set_descriptor(js, t->as_obj, t->str, t->len, desc_flags);
  else if (e->loc.obj) {
    if (!js_obj_ensure_unique_shape(e->loc.obj)) return js_mkerr(js, "oom");
    ant_shape_set_attrs_symbol(js, e->loc.obj->shape, t->sym_off, attrs_from_flags(desc_flags, true));
  }

  return define_done(js, t);
}

static ant_value_t define_data(
  ant_t *js, const define_target_t *t, const define_desc_t *d, const define_existing_t *e
) {
  int desc_flags = define_desc_flags(d, true);
  uint8_t attrs = attrs_from_flags(desc_flags, true);

  if (!e->loc.obj) {
    ant_value_t added = t->sym_key
      ? mkprop_exact_attrs(js, t->as_obj, t->prop, d->value, attrs)
      : mkprop_bytes_exact_attrs(js, t->as_obj, t->str, (size_t)t->len, d->value, attrs);
    return is_err(added) ? added : define_done(js, t);
  }

  bool frozen = e->ptr && e->ptr->flags.frozen;
  bool nonconfig = is_nonconfig_prop(e->loc) || frozen;
  bool readonly = is_const_prop(e->loc) || frozen;

  if (nonconfig && d->configurable)
    return define_redefine_error(js, t->str, t->len, "cannot change configurable from false to true");
  if (nonconfig && readonly && d->has_writable && d->writable)
    return define_redefine_error(js, t->str, t->len, "cannot change writable from false to true");
  if (nonconfig && readonly && d->has_value && !same_value_values(js, js_prop_load(e->loc), d->value))
    return js_mkerr(js, "Cannot assign to read-only property '%.*s'", (int)t->len, t->str);

  if (d->has_value) {
    js_prop_store(js, e->loc, d->value);
    if (t->sym_key) ant_symbol_property_mutation_invalidate(js, e->loc.obj, t->sym_off);
  }
  
  if (!t->sym_key) js_set_descriptor(js, t->as_obj, t->str, t->len, desc_flags);
  if (!js_obj_ensure_unique_shape(e->loc.obj)) return js_mkerr(js, "oom");
  
  ant_shape_t *shape = e->loc.obj->shape;
  if (t->sym_key) ant_shape_set_attrs_symbol(js, shape, t->sym_off, attrs);
  else ant_shape_set_attrs_interned(js, shape, intern_string(js, t->str, t->len), attrs);
  ant_shape_clear_accessor_slot(js, shape, e->loc.slot);

  return define_done(js, t);
}

static ant_value_t object_define_property_keyed(
  ant_t *js, ant_value_t obj, property_key_view_t *key, 
  ant_value_t descriptor, const define_desc_raw_t *pre_raw, define_desc_fields_t *fields
) {
  if (vtype(descriptor) == kTypeBuiltin) descriptor = js_cfunc_promote(js, descriptor);
  if (!pre_raw && !is_object_type(descriptor)) return descriptor_value_error(js, "Property description must be an object: ", descriptor);
  
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return define_via_proxy(js, obj, as_obj, key, descriptor, pre_raw);
  
  define_target_t t = define_target_init(obj, as_obj, key);
  define_desc_t d;
  
  ant_value_t err = define_desc_parse(js, descriptor, pre_raw, &d);
  if (is_err(err)) return err;

  if (fields) *fields = define_desc_fields(&d);

  ant_value_t result;
  if (define_exotic(js, &t, &d, fields, &result)) return result;

  if (!t.sym_key) {
    ant_object_t *wrapper_ptr = vtype(as_obj) == kTypeObject ? (ant_object_t *)vptr(as_obj) : NULL;
    if (wrapper_ptr && wrapper_ptr->extra_slots) {
      define_desc_fields_t wrapper_desc = define_desc_fields(&d);
      if (string_wrapper_define(js, obj, as_obj, t.str, (size_t)t.len, &wrapper_desc, &result)) return result;
    }

    ant_object_t *arr_ptr = is_length_key(t.str, t.len) ? real_array_obj_ptr(as_obj) : NULL;
    if (arr_ptr) return define_array_length(js, &t, &d, fields, arr_ptr);
    if (define_array_index(js, &t, &d, &result)) return result;
    
    array_materialize_dense_for_define(js, as_obj, t.str, (size_t)t.len);
  }

  define_existing_t e = define_lookup_existing(js, &t);
  err = define_validate_existing(js, &t, &d, &e);
  if (is_err(err)) return err;

  if (e.has_meta) {
    if (!d.has_writable) d.writable = e.meta.writable;
    if (!d.has_enumerable) d.enumerable = e.meta.enumerable;
    if (!d.has_configurable) d.configurable = e.meta.configurable;
  }

  if (e.found && !d.has_value && !d.has_writable && !d.has_get && !d.has_set)
    return define_update_attrs(js, &t, &d, &e);

  if (d.has_get || d.has_set) {
    define_set_accessor(js, &t, d.has_get, d.getter, d.has_set, d.setter, define_desc_flags(&d, false));
    return define_done(js, &t);
  }

  return define_data(js, &t, &d, &e);
}

ant_value_t object_define_property_reporting(
  ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor,
  const define_desc_raw_t *raw, bool *abrupt
) {
  if (vtype(obj) == kTypeBuiltin) obj = js_cfunc_promote(js, obj);
  if (!is_object_type(obj))
    return js_mkerr(js, "Object.defineProperty called on non-object");

  char key_stack[128];
  property_key_view_t key;
  ant_value_t error = property_key_view_init(
    js, prop, &key, 
    key_stack, sizeof(key_stack)
  );
  
  if (is_err(error)) {
    property_key_view_dispose(&key);
    return error;
  }

  bool root_key = property_key_view_requires_pin(&key, is_proxy(js_as_obj(obj)));
  size_t root_mark = root_key ? gc_root_scope(js) : 0;
  if (root_key) GC_ROOT_PIN(js, key.js_key);

  define_desc_fields_t fields = {0};
  ant_value_t result = object_define_property_keyed(js, obj, &key, descriptor, raw, &fields);
  
  if (!is_err(result) && !key.is_symbol && js_is_arguments(obj))
    js_arguments_after_define(js, obj, key.bytes, key.length, &fields);
  
  property_key_view_dispose(&key);
  if (root_key) GC_ROOT_RESTORE(js, root_mark);
  if (abrupt) *abrupt = fields.abrupt;
  
  return result;
}

ant_value_t object_define_property(
  ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor
) {
  return object_define_property_reporting(js, obj, prop, descriptor, NULL, NULL);
}

static ant_value_t define_desc_snapshot(ant_t *js, ant_value_t descriptor) {
  if (vtype(descriptor) == kTypeBuiltin) descriptor = js_cfunc_promote(js, descriptor);
  if (!is_object_type(descriptor)) return descriptor_value_error(js, "Property description must be an object: ", descriptor);
  
  define_desc_raw_t raw = {0};
  ant_value_t err = define_desc_read(js, descriptor, &raw);
  return is_err(err) ? err : define_desc_object(js, &raw);
}

static ant_value_t reflect_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, obj);
  
  char key_stack[128];
  property_key_view_t key;
  
  ant_value_t key_val = property_key_view_init(js, prop, &key, key_stack, sizeof(key_stack));
  if (!is_err(key_val)) key_val = property_key_view_materialize(js, &key);
  property_key_view_dispose(&key);
  
  if (is_err(key_val)) {
    GC_ROOT_RESTORE(js, mark);
    return key_val;
  }
  
  GC_ROOT_PIN(js, key_val);
  ant_value_t snapshot = define_desc_snapshot(js, descriptor);
  
  if (is_err(snapshot)) {
    GC_ROOT_RESTORE(js, mark);
    return snapshot;
  }
  
  GC_ROOT_PIN(js, snapshot);
  if (is_object_type(obj) && is_proxy(js_as_obj(obj))) {
    ant_value_t trapped = proxy_define_property(js, js_as_obj(obj), key_val, snapshot);
    GC_ROOT_RESTORE(js, mark);
    return is_err(trapped) ? trapped : Ant_Value_FromBool(js_truthy(js, trapped));
  }
  
  ant_value_t saved = Ant_Exception_Peek(js);
  GC_ROOT_PIN(js, saved);
  bool abrupt = false;
  ant_value_t result = object_define_property_reporting(js, obj, key_val, snapshot, NULL, &abrupt);
  
  if (is_err(result) && !abrupt) {
    Ant_Exception_Set(js, saved);
    result = Ant_Value_False;
  } else if (!is_err(result)) result = Ant_Value_True;
  
  GC_ROOT_RESTORE(js, mark);
  return result;
}

ant_value_t js_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor, bool reflect_mode) {
  return reflect_mode
    ? reflect_define_property(js, obj, prop, descriptor)
    : object_define_property(js, obj, prop, descriptor);
}

static ant_value_t object_define_properties_from_slots(
  ant_t *js, ant_value_t obj, ant_object_t *ptr, uint32_t count,
  const define_desc_raw_t *raws, bool symbols
) {
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || !(ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE)) continue;
    if ((prop->type == ANT_SHAPE_KEY_SYMBOL) != symbols) continue;
    ant_value_t key = symbols
      ? mkval(kTypeSymbol, prop->key.sym_off)
      : js_mkstr(js, prop->key.interned, intern_length(prop->key.interned));
    ant_value_t desc = ant_object_prop_get_unchecked(ptr, i);
    ant_value_t result = object_define_property_reporting(js, obj, key, desc, &raws[i], NULL);
    if (is_err(result)) return result;
  }
  return obj;
}

static ant_value_t object_define_properties_from_data(ant_t *js, ant_value_t obj, ant_value_t props, bool *handled) {
  *handled = false;
  
  if (vtype(props) != kTypeObject || is_proxy(props) || is_proxy(js_as_obj(obj))) return Ant_Value_Undefined;
  if (js_as_obj(obj) == props) return Ant_Value_Undefined;
  ant_object_t *ptr = js_obj_ptr(props);
  
  if (!ptr || !ptr->shape || ptr->flags.is_exotic || ant_object_extra_count(ptr) != 0) return Ant_Value_Undefined;
  if (ant_shape_may_have_index_keys(ptr->shape)) return Ant_Value_Undefined;

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || i >= ptr->prop_count) return Ant_Value_Undefined;
    if (prop->has_getter || prop->has_setter) return Ant_Value_Undefined;
  }

  define_desc_raw_t stack_raws[32];
  define_desc_raw_t *raws = count <= 32 ? stack_raws : calloc(count, sizeof(*raws));
  
  if (!raws) return js_mkerr(js, "oom");
  if (raws == stack_raws) memset(stack_raws, 0, count * sizeof(*raws));

  ant_value_t result = Ant_Value_Undefined;
  bool target_is_descriptor = false;
  
  for (uint32_t i = 0; i < count; i++) {
    if (!(ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE)) continue;
    ant_value_t desc = ant_object_prop_get_unchecked(ptr, i);
    if (vtype(desc) == kTypeBuiltin) goto done;
    target_is_descriptor |= desc == js_as_obj(obj);
    
    if (!is_object_type(desc)) {
      *handled = true;
      result = descriptor_value_error(js, "Property description must be an object: ", desc);
      goto done;
    }
    
    if (!define_desc_read_plain(js, desc, &raws[i])) goto done;
    
    ant_value_t err = define_desc_check(js, desc, &raws[i]);
    if (is_err(err)) {
      *handled = true;
      result = err;
      goto done;
    }
  }

  *handled = true;
  GC_ROOT_SAVE(root_mark, js);
  for (uint32_t i = 0; target_is_descriptor && i < count; i++)
    for (int f = 0; f < DESC_FIELD_COUNT; f++) if (raws[i].has[f]) GC_ROOT_PIN(js, raws[i].val[f]);

  result = object_define_properties_from_slots(js, obj, ptr, count, raws, false);
  if (!is_err(result)) result = object_define_properties_from_slots(js, obj, ptr, count, raws, true);
  GC_ROOT_RESTORE(js, root_mark);

done:
  if (raws != stack_raws) free(raws);
  return result;
}

static ant_value_t object_define_properties_get(ant_t *js, ant_value_t source, bool proxied, ant_value_t key) {
  if (proxied) return proxy_get_val(js, source, key);
  if (vtype(key) == kTypeSymbol) return js_get_sym(js, source, key);
  
  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key, &key_len);
  
  return js_getprop_fallback_len(js, source, (const char *)(uintptr_t)key_off, (size_t)key_len);
}

ant_value_t object_define_properties(ant_t *js, ant_value_t obj, ant_value_t props) {
  uint8_t pt = vtype(props);
  if (pt == kTypeUndefined || pt == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");

  if (pt == kTypeString) {
    props = builtin_Object(js, &props, 1, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
    if (is_err(props)) return props;
  } else if (pt == kTypeBuiltin) props = js_cfunc_promote(js, props);
  else if (!is_object_type(props)) return obj;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, props);

  bool handled = false;
  ant_value_t result = object_define_properties_from_data(js, obj, props, &handled);
  if (handled) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t source = js_as_obj(props);
  bool proxied = is_proxy(source);
  
  ant_value_t keys = proxied ? proxy_enum(js, source, OBJ_ENUM_KEYS) : js_own_property_keys(js, props, true, true);
  GC_ROOT_PIN(js, keys);
  
  ant_value_t descriptors = is_err(keys) ? keys : Ant_Array_Create(js);
  GC_ROOT_PIN(js, descriptors);
  
  if (is_err(descriptors)) {
    GC_ROOT_RESTORE(js, root_mark);
    return descriptors;
  }

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t desc = object_define_properties_get(js, source, proxied, js_arr_get(js, keys, i));
    if (vtype(desc) == kTypeBuiltin) desc = js_cfunc_promote(js, desc);
    if (!is_err(desc) && !is_object_type(desc))
      desc = descriptor_value_error(js, "Property description must be an object: ", desc);
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    js_arr_push(js, descriptors, desc);
  }

  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t desc = define_desc_snapshot(js, js_arr_get(js, descriptors, i));
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    arr_set(js, descriptors, i, desc);
  }

  for (ant_offset_t i = 0; i < key_count; i++) {
    result = object_define_property(js, obj, js_arr_get(js, keys, i), js_arr_get(js, descriptors, i));
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return obj;
}

static ant_value_t descriptor_template(ant_t *js, bool accessor) {
  ant_value_t *slot = accessor ? &js->builtins.accessor_desc_template : &js->builtins.data_desc_template;
  if (vtype(*slot) == kTypeObject) return *slot;
  
  ant_value_t template = Ant_Object_Create(js);
  if (is_err(template)) return template;
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, template);
  
  ant_value_t first = mkprop_interned(js, template, accessor ? js->intern.get : js->intern.value, Ant_Value_Undefined, 0);
  ant_value_t second = mkprop_interned(js, template, accessor ? js->intern.set : js->intern.writable, accessor ? Ant_Value_Undefined : Ant_Value_False, 0);
  
  ant_value_t third = mkprop_interned(js, template, js->intern.enumerable, Ant_Value_False, 0);
  ant_value_t fourth = mkprop_interned(js, template, js->intern.configurable, Ant_Value_False, 0);
  
  GC_ROOT_RESTORE(js, mark);

  if (is_err(first)) return first;
  if (is_err(second)) return second;
  if (is_err(third)) return third;
  if (is_err(fourth)) return fourth;
  
  ant_object_t *ptr = js_obj_ptr(template);
  if (!ptr->shape || ant_shape_count(ptr->shape) != 4 || ptr->prop_count != 4)
    return js_mkerr(js, "invalid descriptor template");
  
  *slot = template;
  return template;
}

static ant_value_t from_property_descriptor(
  ant_t *js, bool accessor, ant_value_t first, ant_value_t second, bool enumerable, bool configurable
) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, first);
  GC_ROOT_PIN(js, second);
  
  ant_value_t template = descriptor_template(js, accessor);
  ant_value_t desc = is_err(template) ? template : js_mkobj_from_template(js, template);
  GC_ROOT_RESTORE(js, mark);
  if (is_err(desc)) return desc;
  
  ant_object_t *obj = js_obj_ptr(desc);
  ant_object_prop_set_unchecked(obj, 0, first);
  ant_object_prop_set_unchecked(obj, 1, second);
  ant_object_prop_set_unchecked(obj, 2, Ant_Value_FromBool(enumerable));
  ant_object_prop_set_unchecked(obj, 3, Ant_Value_FromBool(configurable));
  gc_write_barrier(js, obj, first);
  gc_write_barrier(js, obj, second);
  
  return desc;
}

ant_value_t js_from_data_descriptor(ant_t *js, ant_value_t value, bool writable, bool enumerable, bool configurable) {
  return from_property_descriptor(js, false, value, Ant_Value_FromBool(writable), enumerable, configurable);
}

ant_value_t js_from_accessor_descriptor(ant_t *js, ant_value_t getter, ant_value_t setter, bool enumerable, bool configurable) {
  return from_property_descriptor(js, true, getter, setter, enumerable, configurable);
}

ant_value_t js_get_own_property_descriptor(ant_t *js, ant_value_t obj, ant_value_t key) {
  ant_value_t args[2] = { obj, key };
  return builtin_object_getOwnPropertyDescriptor(js, args, 2, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
}
