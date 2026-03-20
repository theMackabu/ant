#ifndef ANT_DESCRIPTORS_H
#define ANT_DESCRIPTORS_H

#include "types.h"
#include <uthash.h>

#define JS_DESC_W (1 << 0)
#define JS_DESC_E (1 << 1)
#define JS_DESC_C (1 << 2)

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

extern descriptor_entry_t *desc_registry;

descriptor_entry_t *lookup_descriptor(ant_value_t obj, const char *key, size_t klen);
descriptor_entry_t *lookup_sym_descriptor(ant_value_t obj, ant_offset_t sym_off);

uint64_t make_desc_key(ant_value_t obj, const char *key, size_t klen);
uint64_t make_sym_desc_key(ant_value_t obj, ant_offset_t sym_off);

void js_set_descriptor(ant_t *js, ant_value_t obj, const char *key, size_t klen, int flags);
void js_set_getter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, int flags);
void js_set_setter_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t setter, int flags);
void js_set_accessor_desc(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t getter, ant_value_t setter, int flags);

void js_set_sym_getter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t getter, int flags);
void js_set_sym_setter_desc(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t setter, int flags);

#endif
