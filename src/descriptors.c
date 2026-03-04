#include "descriptors.h"
#include "silver/engine.h"
#include "arena.h"
#include "utils.h"

descriptor_entry_t *desc_registry = NULL;

static descriptor_entry_t arr_length_desc = {
  .key = 0, .obj_off = 0,
  .prop_name = "length", .prop_len = 6,
  .writable = true, .enumerable = false, .configurable = false,
  .has_getter = false, .has_setter = false,
  .getter = 0, .setter = 0,
};

uint64_t make_desc_key(ant_offset_t obj_off, const char *key, size_t klen) {
  uint32_t key_hash = (uint32_t)hash_key(key, klen) & 0x7FFFFFFFu;
  return ((uint64_t)obj_off << 32) | key_hash;
}

uint64_t make_sym_desc_key(ant_offset_t obj_off, ant_offset_t sym_off) {
  return ((uint64_t)obj_off << 32) | ((uint32_t)sym_off | 0x80000000u);
}

descriptor_entry_t *lookup_descriptor(ant_t *js, ant_offset_t obj_off, const char *key, size_t klen) {
  if (klen == 6
    && memcmp(key, "length", 6) == 0
    && is_arr_off(js, obj_off)
  ) return &arr_length_desc;

  descriptor_entry_t *entry = NULL;
  uint64_t desc_key = make_desc_key(obj_off, key, klen);
  HASH_FIND(hh, desc_registry, &desc_key, sizeof(uint64_t), entry);

  return entry;
}

descriptor_entry_t *lookup_sym_descriptor(ant_offset_t obj_off, ant_offset_t sym_off) {
  descriptor_entry_t *entry = NULL;
  uint64_t k = make_sym_desc_key(obj_off, sym_off);
  HASH_FIND(hh, desc_registry, &k, sizeof(uint64_t), entry);
  return entry;
}

static descriptor_entry_t *get_or_create_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (vtype(obj) != T_OBJ && vtype(obj) != T_FUNC && vtype(obj) != T_ARR) return NULL;
  obj = js_as_obj(obj);
  ant_offset_t obj_off = (ant_offset_t)vdata(obj);
  uint64_t desc_key = make_desc_key(obj_off, key, klen);

  descriptor_entry_t *entry = NULL;
  HASH_FIND(hh, desc_registry, &desc_key, sizeof(uint64_t), entry);
  if (!entry) {
    entry = (descriptor_entry_t *)ant_calloc(sizeof(descriptor_entry_t) + klen + 1);
    if (!entry) return NULL;

    entry->key = desc_key;
    entry->obj_off = obj_off;
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
  }
  return entry;
}

static void ensure_string_prop_node(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) return;
  if (lkp(js, obj, key, klen) != 0) return;

  ant_value_t key_val = js_mkstr(js, key, klen);
  if (is_err(key_val)) return;
  (void)mkprop(js, obj, key_val, js_mkundef(), 0);
}

static descriptor_entry_t *get_or_create_sym_desc(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) return NULL;
  ant_offset_t obj_off = (ant_offset_t)vdata(obj);
  uint64_t k = make_sym_desc_key(obj_off, sym_off);

  descriptor_entry_t *entry = NULL;
  HASH_FIND(hh, desc_registry, &k, sizeof(uint64_t), entry);
  if (!entry) {
    entry = (descriptor_entry_t *)ant_calloc(sizeof(descriptor_entry_t));
    if (!entry) return NULL;
    entry->key = k;
    entry->obj_off = obj_off;
    entry->sym_off = sym_off;
    entry->prop_name = NULL;
    entry->prop_len = 0;
    entry->writable = true;
    entry->enumerable = true;
    entry->configurable = true;
    entry->getter = js_mkundef();
    entry->setter = js_mkundef();
    HASH_ADD(hh, desc_registry, key, sizeof(uint64_t), entry);
  }
  return entry;
}

static void ensure_symbol_prop_node(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) return;
  if (lkp_sym(js, obj, sym_off) != 0) return;

  ant_value_t sym = mkval(T_SYMBOL, sym_off);
  (void)mkprop(js, obj, sym, js_mkundef(), 0);
}

void js_set_descriptor(ant_t *js, ant_value_t obj, const char *key, size_t klen, int flags) {
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return;
  entry->writable = (flags & JS_DESC_W) != 0;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
}

void js_set_getter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, int flags) {
  ensure_string_prop_node(js, obj, key, klen);
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  entry->has_getter = true;
  entry->getter = getter;
}

void js_set_setter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t setter, int flags) {
  ensure_string_prop_node(js, obj, key, klen);
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  entry->has_setter = true;
  entry->setter = setter;
}

void js_set_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags) {
  ensure_string_prop_node(js, obj, key, klen);
  descriptor_entry_t *entry = get_or_create_desc(js, obj, key, klen);
  if (!entry) return;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  entry->has_getter = true;
  entry->has_setter = true;
  entry->getter = getter;
  entry->setter = setter;
}

void js_set_sym_getter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t getter, int flags) {
  if (vtype(sym) != T_SYMBOL) return;
  ensure_symbol_prop_node(js, obj, (ant_offset_t)vdata(sym));
  descriptor_entry_t *entry = get_or_create_sym_desc(js, obj, (ant_offset_t)vdata(sym));
  if (!entry) return;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  entry->has_getter = true;
  entry->getter = getter;
}

void js_set_sym_setter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t setter, int flags) {
  if (vtype(sym) != T_SYMBOL) return;
  ensure_symbol_prop_node(js, obj, (ant_offset_t)vdata(sym));
  descriptor_entry_t *entry = get_or_create_sym_desc(js, obj, (ant_offset_t)vdata(sym));
  if (!entry) return;
  entry->enumerable = (flags & JS_DESC_E) != 0;
  entry->configurable = (flags & JS_DESC_C) != 0;
  entry->has_setter = true;
  entry->setter = setter;
}
