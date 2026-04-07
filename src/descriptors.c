#include "gc.h"
#include "utils.h"
#include "shapes.h"
#include "internal.h"
#include "descriptors.h"
#include "silver/engine.h"

#include <assert.h>
#include <string.h>

descriptor_entry_t *desc_registry = NULL;

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

static inline bool is_canonical_desc_obj(ant_value_t obj) {
  return vtype(obj) == T_OBJ;
}

static inline bool is_exotic_desc_obj(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  return ptr && ptr->is_exotic;
}

static inline bool desc_registry_allowed(ant_value_t obj) {
  if (is_exotic_desc_obj(obj)) return true;
#ifndef NDEBUG
  assert(false && "desc_registry is exotic-only; ordinary objects must use shape metadata");
#endif
  return false;
}

static bool ensure_added_shape_slot_storage(ant_object_t *ptr, uint32_t slot) {
  if (!ptr || !ptr->shape) return false;
  if (!js_obj_ensure_prop_capacity(ptr, ant_shape_count(ptr->shape))) return false;
  if (slot >= ptr->prop_count && !js_obj_ensure_prop_capacity(ptr, slot + 1)) return false;
  
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

static descriptor_entry_t *registry_lookup_desc(ant_value_t obj, const char *key, size_t klen) {
  descriptor_entry_t *entry = NULL;
  uint64_t desc_key = make_desc_key(obj, key, klen);
  HASH_FIND(hh, desc_registry, &desc_key, sizeof(uint64_t), entry);
  return entry;
}

static descriptor_entry_t *registry_lookup_sym_desc(ant_value_t obj, ant_offset_t sym_off) {
  descriptor_entry_t *entry = NULL;
  uint64_t k = make_sym_desc_key(obj, sym_off);
  HASH_FIND(hh, desc_registry, &k, sizeof(uint64_t), entry);
  return entry;
}

descriptor_entry_t *lookup_descriptor(ant_value_t obj, const char *key, size_t klen) {
  assert(is_canonical_desc_obj(obj) && "lookup_descriptor expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return NULL;

  ant_object_t *ptr = js_obj_ptr(obj);
  if (klen == 6 && memcmp(key, "length", 6) == 0 && ptr && ptr->type_tag == T_ARR)
    return &arr_length_desc;

  if (!is_exotic_desc_obj(obj)) return NULL;
  return registry_lookup_desc(obj, key, klen);
}

descriptor_entry_t *lookup_sym_descriptor(ant_value_t obj, ant_offset_t sym_off) {
  assert(is_canonical_desc_obj(obj) && "lookup_sym_descriptor expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return NULL;

  if (!is_exotic_desc_obj(obj)) return NULL;
  return registry_lookup_sym_desc(obj, sym_off);
}

static descriptor_entry_t *get_or_create_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (!desc_registry_allowed(obj)) return NULL;
  descriptor_entry_t *entry = registry_lookup_desc(obj, key, klen);
  if (entry) return entry;

  entry = (descriptor_entry_t *)ant_calloc(sizeof(descriptor_entry_t) + klen + 1);
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
  entry->getter = js_mkundef();
  entry->setter = js_mkundef();

  HASH_ADD(hh, desc_registry, key, sizeof(uint64_t), entry);
  return entry;
}

static descriptor_entry_t *get_or_create_sym_desc(ant_value_t obj, ant_offset_t sym_off) {
  if (!desc_registry_allowed(obj)) return NULL;
  descriptor_entry_t *entry = registry_lookup_sym_desc(obj, sym_off);
  if (entry) return entry;

  entry = (descriptor_entry_t *)ant_calloc(sizeof(descriptor_entry_t));
  if (!entry) return NULL;

  entry->key = make_sym_desc_key(obj, sym_off);
  entry->obj_off = (uintptr_t)vdata(obj);
  entry->sym_off = sym_off;
  entry->writable = true;
  entry->enumerable = true;
  entry->configurable = true;
  entry->has_getter = false;
  entry->has_setter = false;
  entry->getter = js_mkundef();
  entry->setter = js_mkundef();

  HASH_ADD(hh, desc_registry, key, sizeof(uint64_t), entry);
  return entry;
}

static bool ensure_string_shape_slot(ant_t *js, ant_value_t obj, const char *key, size_t klen, uint32_t *out_slot) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return false;

  const char *interned = intern_string(key, klen);
  if (!interned) return false;

  int32_t slot = ant_shape_lookup_interned(ptr->shape, interned);
  if (slot < 0) {
    uint32_t added_slot = 0;
    if (!ant_shape_add_interned_tr(&ptr->shape, interned, ANT_PROP_ATTR_DEFAULT, &added_slot)) return false;
    if (!ensure_added_shape_slot_storage(ptr, added_slot)) return false;
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
    ant_value_t sym = mkval(T_SYMBOL, sym_off);
    if (is_err(mkprop(js, obj, sym, js_mkundef(), 0))) return false;
    slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
    if (slot < 0) return false;
  }

  if (out_slot) *out_slot = (uint32_t)slot;
  return true;
}

static ant_shape_prop_t *ensure_string_shape_prop(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  uint32_t slot = 0;
  if (!ensure_string_shape_slot(js, obj, key, klen, &slot)) return NULL;
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return NULL;
  if (!js_obj_ensure_unique_shape(ptr)) return NULL;
  return ant_shape_prop_mut_at(ptr->shape, slot);
}

static ant_shape_prop_t *ensure_symbol_shape_prop(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  uint32_t slot = 0;
  if (!ensure_symbol_shape_slot(js, obj, sym_off, &slot)) return NULL;
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return NULL;
  if (!js_obj_ensure_unique_shape(ptr)) return NULL;
  return ant_shape_prop_mut_at(ptr->shape, slot);
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
    prop->getter = has_getter ? getter : js_mkundef();
  }
  if (set_setter_flag) {
    prop->has_setter = has_setter ? 1 : 0;
    prop->setter = has_setter ? setter : js_mkundef();
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
    entry->getter = has_getter ? getter : js_mkundef();
  }
  if (set_setter_flag) {
    entry->has_setter = has_setter;
    entry->setter = has_setter ? setter : js_mkundef();
  }
}

void js_set_descriptor(ant_t *js, ant_value_t obj, const char *key, size_t klen, int flags) {
  assert(is_canonical_desc_obj(obj) && "js_set_descriptor expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return;

  ant_shape_prop_t *prop = ensure_string_shape_prop(js, obj, key, klen);
  if (prop) {
    apply_shape_desc_update(
      prop, flags, true,
      false, false, js_mkundef(),
      false, false, js_mkundef()
    );
    ant_ic_epoch_bump();
    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  apply_registry_desc_update(
    entry, flags, true,
    false, false, js_mkundef(),
    false, false, js_mkundef()
  );
}

void js_set_getter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, int flags) {
  assert(is_canonical_desc_obj(obj) && "js_set_getter_desc expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return;

  ant_shape_prop_t *prop = ensure_string_shape_prop(js, obj, key, klen);
  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      true, true, getter,
      false, false, js_mkundef()
    );
    ant_ic_epoch_bump();
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), getter);
    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  apply_registry_desc_update(
    entry, flags, false,
    true, true, getter,
    false, false, js_mkundef()
  );
}

void js_set_setter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t setter, int flags) {
  assert(is_canonical_desc_obj(obj) && "js_set_setter_desc expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return;

  ant_shape_prop_t *prop = ensure_string_shape_prop(js, obj, key, klen);
  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      false, false, js_mkundef(),
      true, true, setter
    );
    ant_ic_epoch_bump();
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), setter);
    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  apply_registry_desc_update(
    entry, flags, false,
    false, false, js_mkundef(),
    true, true, setter
  );
}

void js_set_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags) {
  assert(is_canonical_desc_obj(obj) && "js_set_accessor_desc expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return;

  ant_shape_prop_t *prop = ensure_string_shape_prop(js, obj, key, klen);
  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      true, true, getter,
      true, true, setter
    );
    ant_ic_epoch_bump();
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
  assert(is_canonical_desc_obj(obj) && "js_set_sym_getter_desc expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return;
  if (vtype(sym) != T_SYMBOL) return;
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);

  ant_shape_prop_t *prop = ensure_symbol_shape_prop(js, obj, sym_off);
  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      true, true, getter,
      false, false, js_mkundef()
    );
    ant_ic_epoch_bump();
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), getter);
    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_sym_desc(obj, sym_off);
  apply_registry_desc_update(
    entry, flags, false,
    true, true, getter,
    false, false, js_mkundef()
  );
}

void js_set_sym_setter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t setter, int flags) {
  assert(is_canonical_desc_obj(obj) && "js_set_sym_setter_desc expects js_as_obj(...)");
  if (!is_canonical_desc_obj(obj)) return;
  if (vtype(sym) != T_SYMBOL) return;
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);

  ant_shape_prop_t *prop = ensure_symbol_shape_prop(js, obj, sym_off);
  if (prop) {
    apply_shape_desc_update(
      prop, flags, false,
      false, false, js_mkundef(),
      true, true, setter
    );
    ant_ic_epoch_bump();
    gc_write_barrier(js, js_obj_ptr(js_as_obj(obj)), setter);
    return;
  }

  if (!desc_registry_allowed(obj)) return;
  descriptor_entry_t *entry = get_or_create_sym_desc(obj, sym_off);
  apply_registry_desc_update(
    entry, flags, false,
    false, false, js_mkundef(),
    true, true, setter
  );
}
