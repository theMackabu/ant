#pragma once

#include "internal.h"

enum obj_enum_mode { 
  OBJ_ENUM_KEYS,
  OBJ_ENUM_VALUES,
  OBJ_ENUM_ENTRIES
};

typedef struct {
  uint32_t slot;
  unsigned long index;
} own_index_key_t;

typedef struct {
  own_index_key_t *items;
  uint32_t count;
  uint32_t cap;
  own_index_key_t stack[32];
} own_index_list_t;

typedef struct {
  ant_value_t sym;
  const char *interned;
} copy_key_t;

typedef struct {
  const copy_key_t *keys;
  int count;
  bool assign;
  bool collect;
  enum obj_enum_mode mode;
} copy_ctx_t;

bool is_plain_set_target(const ant_object_t *ptr);
bool own_writable_data_slot(const ant_object_t *ptr, int32_t slot);
bool own_index_list_push(own_index_list_t *list, uint32_t slot, unsigned long index);
bool chain_lacks_plain_keys(ant_value_t start, const char *first, const char *second);

void own_index_list_init(own_index_list_t *list);
void own_index_list_free(own_index_list_t *list);
void own_index_keys_sort(own_index_key_t *keys, uint32_t count);
void store_own_data_slot(ant_t *js, ant_object_t *ptr, uint32_t slot, const char *interned, ant_value_t val);

ant_value_t js_object_view(ant_value_t v);
ant_value_t object_enum(ant_t *js, ant_value_t obj, enum obj_enum_mode mode);
ant_value_t map_to_entry(ant_t *js, ant_value_t key, ant_value_t val);

ant_value_t copy_data_props(ant_t *js, ant_value_t dst, ant_value_t src, const copy_ctx_t *cx);
ant_value_t object_enum_collect(ant_t *js, ant_value_t obj, enum obj_enum_mode mode);
