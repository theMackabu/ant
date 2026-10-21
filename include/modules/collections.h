#ifndef COLLECTIONS_H
#define COLLECTIONS_H

#include <uthash.h>
#include "types.h"
#include "native_tag.h"

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
  bool gc_prune_pending;
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

typedef struct map_store map_store_t;
typedef struct set_store set_store_t;

typedef struct map_iterator_state {
  map_entry_t **head;
  map_entry_t *last;
  map_store_t *owner;
  struct map_iterator_state *prev_iter;
  struct map_iterator_state *next_iter;
  iter_type_t type;
  bool started;
  bool done;
} map_iterator_state_t;

typedef struct set_iterator_state {
  set_entry_t **head;
  set_entry_t *last;
  set_store_t *owner;
  struct set_iterator_state *prev_iter;
  struct set_iterator_state *next_iter;
  iter_type_t type;
  bool started;
  bool done;
} set_iterator_state_t;

struct map_store {
  map_entry_t *head;
  map_iterator_state_t *iterators;
};

struct set_store {
  set_entry_t *head;
  set_iterator_state_t *iterators;
};

static inline map_entry_t *map_iter_take(map_iterator_state_t *st) {
  if (st->done || !st->head) return NULL;
  map_entry_t *next = st->started && st->last ? (map_entry_t *)st->last->hh.next : *st->head;
  
  if (!next) {
    st->done = true;
    return NULL;
  }
  
  st->started = true;
  st->last = next;
  
  return next;
}

static inline set_entry_t *set_iter_take(set_iterator_state_t *st) {
  if (st->done || !st->head) return NULL;
  set_entry_t *next = st->started && st->last ? (set_entry_t *)st->last->hh.next : *st->head;
  
  if (!next) {
    st->done = true;
    return NULL;
  }
  
  st->started = true;
  st->last = next;
  
  return next;
}

static constexpr uint32_t MAP_NATIVE_TAG = Ant_NativeTag("MAPP");
static constexpr uint32_t SET_NATIVE_TAG = Ant_NativeTag("SETP");

static constexpr uint32_t WEAKMAP_NATIVE_TAG = Ant_NativeTag("WMAP");
static constexpr uint32_t WEAKSET_NATIVE_TAG = Ant_NativeTag("WSET");
static constexpr uint32_t WEAKREF_NATIVE_TAG = Ant_NativeTag("WREF");

static constexpr uint32_t MAP_ITER_NATIVE_TAG = Ant_NativeTag("MITR");
static constexpr uint32_t SET_ITER_NATIVE_TAG = Ant_NativeTag("SITR");

void init_collections_module(ant_t *js);
void weakmap_table_free(weakmap_table_t *table);
void weakmap_table_finish_prune(weakmap_table_t *table);

map_entry_t **get_map_from_obj(ant_value_t obj);
set_entry_t **get_set_from_obj(ant_value_t obj);

map_entry_t **collections_map_store_new(void);
set_entry_t **collections_set_store_new(void);

map_iterator_state_t *get_map_iter_state(ant_value_t obj);
set_iterator_state_t *get_set_iter_state(ant_value_t obj);

map_entry_t *Ant_Map_Find(ant_t *js, map_entry_t **map_ptr, ant_value_t key);
set_entry_t *Ant_Set_Find(ant_t *js, set_entry_t **set_ptr, ant_value_t value);

void collections_map_store_free(map_entry_t **head);
void collections_set_store_free(set_entry_t **head);
void collections_map_after_clear(map_entry_t **head);
void collections_set_after_clear(set_entry_t **head);
void collections_map_before_delete(map_entry_t **head, map_entry_t *entry);
void collections_set_before_delete(set_entry_t **head, set_entry_t *entry);

bool advance_map(ant_t *js, iterator_t *it, ant_value_t *out);
bool advance_set(ant_t *js, iterator_t *it, ant_value_t *out);
bool collections_is_map_get_builtin(ant_value_t func);
bool collections_is_map_has_builtin(ant_value_t func);
bool js_weakref_target(ant_value_t ref, ant_value_t *target);
bool weakmap_table_delete(weakmap_table_t *table, ant_value_t key);

ant_value_t js_weakref_create(ant_t *js, ant_value_t target);
weakmap_entry_t *weakmap_table_find(weakmap_table_t *table, ant_value_t key);

ant_value_t collections_map_create(ant_t *js, map_entry_t ***out_map);
ant_value_t collections_set_create(ant_t *js, set_entry_t ***out_set);
bool collections_set_store_cloned_entry(ant_t *js, set_entry_t **set_ptr, ant_value_t value);

bool collections_map_store_cloned_entry(
  ant_t *js, map_entry_t **map_ptr,
  ant_value_t key, ant_value_t value
);

ant_value_t collections_map_numeric_template(
  ant_t *js, ant_value_t map, bool return_presence,
  const ant_value_t *substitutions,
  const sv_map_template_desc_t *desc
);

ant_value_t collections_map_get_numeric_pair(
  ant_t *js, ant_value_t map,
  ant_value_t left, const char *separator, size_t separator_len,
  ant_value_t right
);

// key_obj 0 is an empty slot; key_obj 1 is a tombstone.
static inline bool weakmap_entry_is_occupied(const weakmap_entry_t *entry) {
  return entry->key_obj != 0 && entry->key_obj != 1;
}

static inline uint32_t weak_collection_key_hash(ant_value_t key) {
  uint32_t hash = (uint32_t)key ^ (uint32_t)(key >> 32);
  hash = ((hash >> 16) ^ hash) * UINT32_C(0x45d9f3b);
  return (hash >> 16) ^ hash;
}

#endif
