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
  UT_hash_handle hh;
} weakmap_entry_t;

typedef struct weakset_entry {
  ant_value_t value_obj;
  UT_hash_handle hh;
} weakset_entry_t;

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
  
  MAP_ITER_NATIVE_TAG = 0x4d495452u, // MITR
  SET_ITER_NATIVE_TAG = 0x53495452u  // SITR
};

void init_collections_module(void);

map_entry_t **get_map_from_obj(ant_value_t obj);
set_entry_t **get_set_from_obj(ant_value_t obj);

map_iterator_state_t *get_map_iter_state(ant_value_t obj);
set_iterator_state_t *get_set_iter_state(ant_value_t obj);


bool advance_map(ant_t *js, struct js_iter_t *it, ant_value_t *out);
bool advance_set(ant_t *js, struct js_iter_t *it, ant_value_t *out);

#endif
