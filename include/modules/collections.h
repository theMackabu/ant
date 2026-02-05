#ifndef COLLECTIONS_H
#define COLLECTIONS_H

#include <uthash.h>
#include "types.h"
#include "gc.h"

typedef struct map_entry {
  char *key;
  jsval_t value;
  UT_hash_handle hh;
} map_entry_t;

typedef struct set_entry {
  jsval_t value;
  char *key;
  UT_hash_handle hh;
} set_entry_t;

typedef struct weakmap_entry {
  jsval_t key_obj;
  jsval_t value;
  UT_hash_handle hh;
} weakmap_entry_t;

typedef struct weakset_entry {
  jsval_t value_obj;
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

typedef struct map_registry_entry {
  map_entry_t **head;
  jsoff_t obj_offset;
} map_registry_entry_t;

typedef struct set_registry_entry {
  set_entry_t **head;
  jsoff_t obj_offset;
} set_registry_entry_t;

void init_collections_module(void);
void cleanup_collections_module(void);
size_t collections_get_external_memory(void);

void collections_gc_reserve_roots(GC_OP_VAL_ARGS);
void collections_gc_update_roots(jsoff_t (*fwd_off)(void *ctx, jsoff_t old), GC_OP_VAL_ARGS);

map_entry_t **get_map_from_obj(ant_t *js, jsval_t obj);
set_entry_t **get_set_from_obj(ant_t *js, jsval_t obj);

#endif
