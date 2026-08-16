#ifndef COLLECTIONS_H
#define COLLECTIONS_H

#include <uthash.h>
#include "types.h"
#include "modules/symbol.h"

typedef struct map_entry {
  unsigned char *key;
  size_t key_len;
  ant_value_t key_val;
  ant_value_t value;
  UT_hash_handle hh;
} map_entry_t;

typedef struct set_entry {
  ant_value_t value;
  unsigned char *key;
  size_t key_len;
  UT_hash_handle hh;
} set_entry_t;

typedef struct weakmap_entry {
  ant_value_t key_obj;
  ant_value_t value;
} weakmap_entry_t;

typedef struct weakmap_table {
  weakmap_entry_t *entries;
  uint32_t count;
  uint32_t capacity;
  uint32_t tombstones;
} weakmap_table_t;

typedef struct weakset_entry {
  ant_value_t value_obj;
  UT_hash_handle hh;
} weakset_entry_t;

typedef struct weakref_state {
  ant_value_t target;
} weakref_state_t;

typedef enum {
  ITER_TYPE_MAP_VALUES,
  ITER_TYPE_MAP_KEYS,
  ITER_TYPE_MAP_ENTRIES,
  ITER_TYPE_SET_VALUES,
  ITER_TYPE_SET_ENTRIES
} iter_type_t;

typedef struct map_iterator_state {
  map_entry_t **head;
  map_entry_t *current;
  iter_type_t type;
} map_iterator_state_t;

typedef struct set_iterator_state {
  set_entry_t **head;
  set_entry_t *current;
  iter_type_t type;
} set_iterator_state_t;

enum {
  MAP_NATIVE_TAG = 0x4d415050u, // MAPP
  SET_NATIVE_TAG = 0x53455450u, // SETP
  
  WEAKMAP_NATIVE_TAG = 0x574d4150u, // WMAP
  WEAKSET_NATIVE_TAG = 0x57534554u, // WSET
  WEAKREF_NATIVE_TAG = 0x57524546u, // WREF
  
  MAP_ITER_NATIVE_TAG = 0x4d495452u, // MITR
  SET_ITER_NATIVE_TAG = 0x53495452u  // SITR
};

void init_collections_module(ant_t *js);

map_entry_t **get_map_from_obj(ant_value_t obj);
set_entry_t **get_set_from_obj(ant_value_t obj);

map_iterator_state_t *get_map_iter_state(ant_value_t obj);
set_iterator_state_t *get_set_iter_state(ant_value_t obj);

bool advance_map(ant_t *js, js_iter_t *it, ant_value_t *out);
bool advance_set(ant_t *js, js_iter_t *it, ant_value_t *out);

ant_value_t collections_make_weakmap(ant_t *js);
ant_value_t collections_weakmap_get(ant_value_t weakmap, ant_value_t key);

bool collections_weakmap_set(
  ant_t *js, ant_value_t weakmap,
  ant_value_t key, ant_value_t value
);

weakmap_entry_t *weakmap_table_find(
  weakmap_table_t *table, ant_value_t key
);

bool weakmap_table_delete(weakmap_table_t *table, ant_value_t key);
void weakmap_table_finish_prune(weakmap_table_t *table);
void weakmap_table_free(weakmap_table_t *table);

static inline bool weakmap_entry_is_occupied(const weakmap_entry_t *entry) {
  return entry->key_obj != 0 && entry->key_obj != 1;
}

static inline uint32_t weak_collection_key_hash(ant_value_t key) {
  uint32_t hash = (uint32_t)key ^ (uint32_t)(key >> 32);
  hash = ((hash >> 16) ^ hash) * UINT32_C(0x45d9f3b);
  return (hash >> 16) ^ hash;
}

#endif
