#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc/modules.h"

#include "hash.h"
#include "shapes.h"
#include "internal.h"
#include "protectors.h"
#include "descriptors.h"
#include "silver/engine.h"

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

static inline bool desc_registry_accepts_accessor(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  return ptr && ptr->flags.is_exotic && !ptr->shape;
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
    if (ant_object_is_frozen(ptr)) return &arr_length_readonly_desc;
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
    if (!js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape) + 1)) return false;
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
    if (!js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape) + 1)) return false;
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
  prop->attrs = desc_attrs_from_flags(flags, include_writable);
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

bool js_set_getter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, int flags) {
  getter = js_cfunc_expose_property(js, getter, JS_CFUNC_GETTER, key, klen, false);
  assert(is_canonical_desc_obj(obj) && "js_set_getter_desc expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return false;
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

    return true;
  }

  if (!desc_registry_accepts_accessor(obj)) return false;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return false;

  apply_registry_desc_update(
    entry, flags, false,
    true, true, getter,
    false, false, Ant_Value_Undefined
  );
  gc_write_barrier(js, js_obj_ptr(obj), getter);
  return true;
}

bool js_set_setter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t setter, int flags) {
  setter = js_cfunc_expose_property(js, setter, JS_CFUNC_SETTER, key, klen, false);
  assert(is_canonical_desc_obj(obj) && "js_set_setter_desc expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return false;
  
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

    return true;
  }

  if (!desc_registry_accepts_accessor(obj)) return false;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return false;

  apply_registry_desc_update(
    entry, flags, false,
    false, false, Ant_Value_Undefined,
    true, true, setter
  );
  gc_write_barrier(js, js_obj_ptr(obj), setter);
  return true;
}

bool js_define_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags) {
  getter = js_cfunc_expose_property(js, getter, JS_CFUNC_GETTER, key, klen, false);
  setter = js_cfunc_expose_property(js, setter, JS_CFUNC_SETTER, key, klen, false);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  const char *interned = ptr && ptr->shape && !ptr->flags.is_exotic ? intern_string(js, key, klen) : NULL;
  
  uint32_t slot = 0;
  bool fresh = interned && ant_shape_lookup_interned(ptr->shape, interned) < 0;
  if (fresh && !js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape) + 1)) return false;
  
  if (!fresh || !ant_shape_add_accessor_tr(js, &ptr->shape, interned, desc_attrs_from_flags(flags, false), getter, setter, &slot))
    return js_set_accessor_desc(js, obj, key, klen, getter, setter, flags);

  ant_object_invalidate_guarded_absence(js, ptr);
  if (!ensure_added_shape_slot_storage(js, ptr, slot)) return false;
  
  ant_object_prop_set_unchecked(ptr, slot, Ant_Value_Undefined);
  ant_property_mutation_invalidate(js, ptr, interned);
  gc_write_barrier(js, ptr, getter);
  gc_write_barrier(js, ptr, setter);
  
  return true;
}

bool js_set_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags) {
  getter = js_cfunc_expose_property(js, getter, JS_CFUNC_GETTER, key, klen, false);
  setter = js_cfunc_expose_property(js, setter, JS_CFUNC_SETTER, key, klen, false);
  assert(is_canonical_desc_obj(obj) && "js_set_accessor_desc expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return false;
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

    return true;
  }

  if (!desc_registry_accepts_accessor(obj)) return false;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return false;

  apply_registry_desc_update(
    entry, flags, false,
    true, true, getter,
    true, true, setter
  );
  gc_write_barrier(js, js_obj_ptr(obj), getter);
  gc_write_barrier(js, js_obj_ptr(obj), setter);
  return true;
}

bool js_set_sym_getter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t getter, int flags) {
  const char *desc = vtype(sym) == kTypeSymbol ? js_sym_desc(sym) : NULL;
  getter = js_cfunc_expose_property(js, getter, JS_CFUNC_GETTER, desc, desc ? strlen(desc) : 0, true);
  assert(is_canonical_desc_obj(obj) && "js_set_sym_getter_desc expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return false;
  if (vtype(sym) != kTypeSymbol) return false;

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

    return true;
  }

  if (!desc_registry_accepts_accessor(obj)) return false;
  descriptor_entry_t *entry = get_or_create_sym_desc(js, obj, sym_off);
  if (!entry) return false;

  apply_registry_desc_update(
    entry, flags, false,
    true, true, getter,
    false, false, Ant_Value_Undefined
  );
  gc_write_barrier(js, js_obj_ptr(obj), getter);
  return true;
}

bool js_set_sym_setter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t setter, int flags) {
  const char *desc = vtype(sym) == kTypeSymbol ? js_sym_desc(sym) : NULL;
  setter = js_cfunc_expose_property(js, setter, JS_CFUNC_SETTER, desc, desc ? strlen(desc) : 0, true);
  assert(is_canonical_desc_obj(obj) && "js_set_sym_setter_desc expects js_as_obj(...)");

  if (!is_canonical_desc_obj(obj)) return false;
  if (vtype(sym) != kTypeSymbol) return false;

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

    return true;
  }

  if (!desc_registry_accepts_accessor(obj)) return false;
  descriptor_entry_t *entry = get_or_create_sym_desc(js, obj, sym_off);
  if (!entry) return false;

  apply_registry_desc_update(
    entry, flags, false,
    false, false, Ant_Value_Undefined,
    true, true, setter
  );
  gc_write_barrier(js, js_obj_ptr(obj), setter);
  return true;
}

void gc_mark_descriptor_accessors(ant_t *js, ant_object_t *owner, gc_mark_fn mark) {
  uintptr_t obj_off = (uintptr_t)vdata(js_obj_from_ptr(owner));
  descriptor_entry_t *entry, *tmp;
  HASH_ITER(hh, js->desc_registry, entry, tmp) {
    if (entry->obj_off != obj_off) continue;
    if (entry->has_getter) mark(js, entry->getter);
    if (entry->has_setter) mark(js, entry->setter);
  }
}

void gc_clear_descriptors_weak_refs(ant_t *js, gc_weak_key_alive_fn alive) {
  descriptor_entry_t *entry, *tmp;
  HASH_ITER(hh, js->desc_registry, entry, tmp) {
    if (alive(js, mkval(kTypeObject, entry->obj_off))) continue;
    HASH_DEL(js->desc_registry, entry);
    free(entry);
  }
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
  
  js_obj_mark_exotic(ptr);
  descriptor_entry_t *entry = get_or_create_desc(js, obj, "length", 6);
  if (!entry) return false;
  
  entry->writable = false;
  entry->enumerable = false;
  entry->configurable = false;
  ant_ic_epoch_bump(js);
  
  return true;
}
