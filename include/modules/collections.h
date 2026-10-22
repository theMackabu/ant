#ifndef COLLECTIONS_H
#define COLLECTIONS_H

#include <uthash.h>
#include "types.h"
#include "value.h"
#include "native_tag.h"

typedef struct map_entry {
  unsigned char *key;
  uint32_t key_len;
  uint32_t cursored;
  ant_value_t key_val;
  ant_value_t value;
  UT_hash_handle hh;
} map_entry_t;

typedef struct set_entry {
  ant_value_t value;
  unsigned char *key;
  uint32_t key_len;
  uint32_t cursored;
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

typedef struct collection_cursor collection_cursor_t;

typedef struct collection_cursors {
  collection_cursor_t *head;
  collection_cursor_t *end;
  uint32_t flag_offset;
} collection_cursors_t;

struct collection_cursor {
  void *entry;
  collection_cursors_t *owner;
  collection_cursor_t *forward;
  collection_cursor_t *prev;
  collection_cursor_t *next;
  uint32_t refs;
  bool embedded;
};

typedef struct collection_iter_state {
  collection_cursor_t *at;
  iter_type_t type;
  bool live;
  collection_cursor_t own;
} collection_iter_state_t;

typedef collection_iter_state_t map_iterator_state_t;
typedef collection_iter_state_t set_iterator_state_t;

typedef struct map_store {
  map_entry_t *head;
  collection_cursors_t cursors;
} map_store_t;

typedef struct set_store {
  set_entry_t *head;
  collection_cursors_t cursors;
} set_store_t;

static inline ant_value_t collections_iter_slot(collection_iter_state_t *st) {
  return tov((double)(uintptr_t)st);
}

static inline collection_iter_state_t *collections_iter_from_slot(ant_value_t slot) {
  return (collection_iter_state_t *)(uintptr_t)tod(slot);
}

map_entry_t *collections_map_iter_step(map_iterator_state_t *st);
set_entry_t *collections_set_iter_step(set_iterator_state_t *st);
ant_value_t collections_entry_pair(ant_t *js, ant_value_t first, ant_value_t second);

static inline map_entry_t *map_iter_take(map_iterator_state_t *st) {
  collection_cursor_t *at = st->at;
  if (!at) return NULL;
  
  map_entry_t *entry = (map_entry_t *)at->entry;
  map_entry_t *next = entry ? (map_entry_t *)entry->hh.next : NULL;
  
  if (!next || next->cursored || at->refs != 1)
    return collections_map_iter_step(st);
  
  entry->cursored = 0;
  next->cursored = 1;
  at->entry = next;
  
  return entry;
}

static inline set_entry_t *set_iter_take(set_iterator_state_t *st) {
  collection_cursor_t *at = st->at;
  if (!at) return NULL;
  
  set_entry_t *entry = (set_entry_t *)at->entry;
  set_entry_t *next = entry ? (set_entry_t *)entry->hh.next : NULL;
  
  if (!next || next->cursored || at->refs != 1)
    return collections_set_iter_step(st);
  
  entry->cursored = 0;
  next->cursored = 1;
  at->entry = next;
  
  return entry;
}

static inline ant_value_t map_iter_value(ant_t *js, const map_iterator_state_t *st, const map_entry_t *entry) {
  if (st->type == ITER_TYPE_MAP_VALUES) return entry->value;
  if (st->type == ITER_TYPE_MAP_KEYS) return entry->key_val;
  return collections_entry_pair(js, entry->key_val, entry->value);
}

static inline ant_value_t set_iter_value(ant_t *js, const set_iterator_state_t *st, const set_entry_t *entry) {
  if (st->type == ITER_TYPE_SET_ENTRIES) return collections_entry_pair(js, entry->value, entry->value);
  return entry->value;
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
void collections_map_remove(map_entry_t **head, map_entry_t *entry);
void collections_set_remove(set_entry_t **head, set_entry_t *entry);
void collections_map_clear(map_entry_t **head);
void collections_set_clear(set_entry_t **head);

void collections_map_iter_open(map_iterator_state_t *st, map_entry_t **head, iter_type_t type);
void collections_set_iter_open(set_iterator_state_t *st, set_entry_t **head, iter_type_t type);
void collections_map_iter_close(map_iterator_state_t *st);
void collections_set_iter_close(set_iterator_state_t *st);

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
