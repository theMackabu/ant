#pragma once

#include "types.h"
#include "shapes.h"
#include <uthash.h>

enum: int {
  JS_DESC_W = 1 << 0,
  JS_DESC_E = 1 << 1,
  JS_DESC_C = 1 << 2,
};

enum: int {
  #define DESC_FIELD(name, key) DESC_##name,
  #include "descriptors/fields.h"
  DESC_FIELD_COUNT
};

typedef struct descriptor_entry {
  uint64_t key;
  uintptr_t obj_off;
  ant_offset_t sym_off;
  char *prop_name;
  size_t prop_len;
  bool writable;
  bool enumerable;
  bool configurable;
  bool has_getter;
  bool has_setter;
  ant_value_t getter;
  ant_value_t setter;
  UT_hash_handle hh;
} descriptor_entry_t;

typedef struct {
  bool has[DESC_FIELD_COUNT];
  ant_value_t val[DESC_FIELD_COUNT];
} define_desc_raw_t;

descriptor_entry_t *lookup_descriptor(ant_t *js, ant_value_t obj, const char *key, size_t klen);
descriptor_entry_t *lookup_sym_descriptor(ant_t *js, ant_value_t obj, ant_offset_t sym_off);
bool js_array_make_length_readonly(ant_t *js, ant_value_t arr);

uint64_t make_desc_key(ant_value_t obj, const char *key, size_t klen);
uint64_t make_sym_desc_key(ant_value_t obj, ant_offset_t sym_off);

void js_set_descriptor(ant_t *js, ant_value_t obj, const char *key, size_t klen, int flags);
void js_set_sym_descriptor(ant_t *js, ant_value_t obj, ant_value_t sym, int flags);
bool js_set_getter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, int flags);
bool js_set_setter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t setter, int flags);
bool js_set_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags);
bool js_define_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags);

bool js_set_sym_getter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t getter, int flags);
bool js_set_sym_setter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t setter, int flags);
void js_descriptor_registry_cleanup(ant_t *js);

ant_value_t object_define_properties(ant_t *js, ant_value_t obj, ant_value_t props);
ant_value_t object_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor);
ant_value_t object_define_property_reporting(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor, const define_desc_raw_t *raw, bool *abrupt);

ant_value_t define_desc_read(ant_t *js, ant_value_t descriptor, define_desc_raw_t *raw);
ant_value_t define_desc_object(ant_t *js, const define_desc_raw_t *raw);
ant_value_t reflect_define_property_raw(ant_t *js, ant_value_t obj, ant_value_t key_val, const define_desc_raw_t *raw);
ant_value_t js_from_data_descriptor(ant_t *js, ant_value_t value, bool writable, bool enumerable, bool configurable);
ant_value_t js_from_accessor_descriptor(ant_t *js, ant_value_t getter, ant_value_t setter, bool enumerable, bool configurable);

static inline uint8_t desc_attrs_from_flags(int flags, bool include_writable) {
  uint8_t attrs = 0;
  if (include_writable && (flags & JS_DESC_W)) attrs |= ANT_PROP_ATTR_WRITABLE;
  if (flags & JS_DESC_E) attrs |= ANT_PROP_ATTR_ENUMERABLE;
  if (flags & JS_DESC_C) attrs |= ANT_PROP_ATTR_CONFIGURABLE;
  return attrs;
}
