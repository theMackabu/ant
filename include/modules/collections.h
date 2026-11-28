#pragma once

#include <uthash.h>
#include "types.h"
#include "value.h"
#include "native_tag.h"
#include "gc/roots.h"

typedef struct set_entry {
  ant_value_t key;
  uint32_t hash;
  uint32_t chain;
} set_entry_t;

typedef struct map_entry {
  ant_value_t key;
  uint32_t hash;
  uint32_t chain;
  ant_value_t value;
} map_entry_t;

static constexpr ant_value_t COLLECTION_HOLE = Ant_SentinelTag("HOLE");
static constexpr uint32_t COLLECTION_END = UINT32_MAX;

static_assert(offsetof(map_entry_t, key) == offsetof(set_entry_t, key), "map and set entries share a prefix");
static_assert(offsetof(map_entry_t, hash) == offsetof(set_entry_t, hash), "map and set entries share a prefix");
static_assert(offsetof(map_entry_t, chain) == offsetof(set_entry_t, chain), "map and set entries share a prefix");

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
  bool notifies_registry;
} weakref_state_t;

typedef enum {
  ITER_TYPE_MAP_VALUES,
  ITER_TYPE_MAP_KEYS,
  ITER_TYPE_MAP_ENTRIES,
  ITER_TYPE_SET_VALUES,
  ITER_TYPE_SET_ENTRIES
} iter_type_t;

typedef struct collection_iter_state collection_iter_state_t;

typedef struct collection_table {
  unsigned char *entries;
  uint32_t *buckets;
  uint32_t used;
  uint32_t size;
  uint32_t capacity;
  uint32_t mask;
  uint32_t first;
  collection_iter_state_t *iters;
  ant_object_t *owner;
} collection_table_t;

struct collection_iter_state {
  collection_table_t *table;
  uint32_t index;
  iter_type_t type;
  collection_iter_state_t *prev;
  collection_iter_state_t *next;
};

typedef collection_iter_state_t map_iterator_state_t;
typedef collection_iter_state_t set_iterator_state_t;

typedef struct {
  const collection_table_t *table;
  uint32_t start;
  uint32_t limit;
  bool map;
} collections_snapshot_t;

ant_value_t collections_entry_pair(ant_t *js, ant_value_t first, ant_value_t second);
ant_value_t *collections_snapshot(gc_temp_root_scope_t *scope, collections_snapshot_t snap, uint32_t *out_len);

bool collections_is_primitive(const collection_table_t *table, bool map);
bool collections_copy(ant_t *js, collection_table_t *dst, const collection_table_t *src, bool map);

static inline map_entry_t *collections_map_next(const collection_table_t *table, uint32_t *index) {
  map_entry_t *entries = (map_entry_t *)table->entries;
  for (uint32_t i = *index, used = table->used; i < used; i++) if (entries[i].key != COLLECTION_HOLE) {
    *index = i + 1;
    return &entries[i];
  }
  *index = table->used;
  return NULL;
}

static inline set_entry_t *collections_set_next(const collection_table_t *table, uint32_t *index) {
  set_entry_t *entries = (set_entry_t *)table->entries;
  for (uint32_t i = *index, used = table->used; i < used; i++) if (entries[i].key != COLLECTION_HOLE) {
    *index = i + 1;
    return &entries[i];
  }
  *index = table->used;
  return NULL;
}

static inline ant_value_t map_iter_value(ant_t *js, const map_iterator_state_t *st, const map_entry_t *entry) {
  if (st->type == ITER_TYPE_MAP_VALUES) return entry->value;
  if (st->type == ITER_TYPE_MAP_KEYS) return entry->key;
  return collections_entry_pair(js, entry->key, entry->value);
}

static inline ant_value_t set_iter_value(ant_t *js, const set_iterator_state_t *st, const set_entry_t *entry) {
  if (st->type == ITER_TYPE_SET_ENTRIES) return collections_entry_pair(js, entry->key, entry->key);
  return entry->key;
}

static constexpr uint32_t MAP_NATIVE_TAG = Ant_NativeTag("MAPP");
static constexpr uint32_t SET_NATIVE_TAG = Ant_NativeTag("SETP");

static constexpr uint32_t WEAKMAP_NATIVE_TAG = Ant_NativeTag("WMAP");
static constexpr uint32_t WEAKSET_NATIVE_TAG = Ant_NativeTag("WSET");
static constexpr uint32_t WEAKREF_NATIVE_TAG = Ant_NativeTag("WREF");

static constexpr uint32_t MAP_ITER_NATIVE_TAG = Ant_NativeTag("MITR");
static constexpr uint32_t SET_ITER_NATIVE_TAG = Ant_NativeTag("SITR");

__attribute__((cold)) 
void init_collections_module(ant_t *js);
void js_finalization_run(ant_t *js);
void weakmap_table_free(weakmap_table_t *table);
void weakmap_table_finish_prune(weakmap_table_t *table);
void collections_iter_finish(collection_iter_state_t *st);

collection_table_t *get_map_from_obj(ant_value_t obj);
collection_table_t *get_set_from_obj(ant_value_t obj);

collection_table_t *collections_table_attach(ant_t *js, ant_value_t obj, uint8_t type);
collection_table_t *collections_table_create(void);
void collections_table_free(collection_table_t *table);

map_iterator_state_t *get_map_iter_state(ant_value_t obj);
set_iterator_state_t *get_set_iter_state(ant_value_t obj);

map_entry_t *Ant_Map_Find(ant_t *js, collection_table_t *map, ant_value_t key);
set_entry_t *Ant_Set_Find(ant_t *js, collection_table_t *set, ant_value_t value);

void collections_map_iter_open(map_iterator_state_t *st, collection_table_t *map, iter_type_t type);
void collections_set_iter_open(set_iterator_state_t *st, collection_table_t *set, iter_type_t type);
void collections_map_iter_close(map_iterator_state_t *st);
void collections_set_iter_close(set_iterator_state_t *st);

bool advance_map(ant_t *js, iterator_t *it, ant_value_t *out);
bool advance_set(ant_t *js, iterator_t *it, ant_value_t *out);
bool collections_is_map_get_builtin(ant_value_t func);
bool collections_is_map_has_builtin(ant_value_t func);
bool collections_is_set_has_builtin(ant_value_t func);
bool js_weakref_target(ant_value_t ref, ant_value_t *target);
bool weakmap_table_delete(weakmap_table_t *table, ant_value_t key);

ant_value_t js_weakref_create(ant_t *js, ant_value_t target);
weakmap_entry_t *weakmap_table_find(weakmap_table_t *table, ant_value_t key);

ant_value_t collections_map_create(ant_t *js, collection_table_t **out_map);
ant_value_t collections_set_create(ant_t *js, collection_table_t **out_set);
ant_value_t collections_map_get(ant_t *js, ant_value_t map, ant_value_t key);
ant_value_t collections_map_has(ant_t *js, ant_value_t map, ant_value_t key);
ant_value_t collections_set_has(ant_t *js, ant_value_t set, ant_value_t value);

bool collections_set_store_cloned_entry(ant_t *js, collection_table_t *set, ant_value_t value);
bool collections_map_store_cloned_entry(ant_t *js, collection_table_t *map, ant_value_t key, ant_value_t value);

ant_value_t collections_map_numeric_template(
  ant_t *js, ant_value_t map, bool return_presence,
  const ant_value_t *substitutions, const sv_map_template_desc_t *desc
);

ant_value_t collections_map_get_numeric_pair(
  ant_t *js, ant_value_t map,
  ant_value_t left, const char *separator, size_t separator_len, ant_value_t right
);

static inline ant_value_t collections_iter_slot(collection_iter_state_t *st) {
  return Ant_Value_FromNumber((double)(uintptr_t)st);
}

static inline collection_iter_state_t *collections_iter_from_slot(ant_value_t slot) {
  return (collection_iter_state_t *)(uintptr_t)Ant_Value_AsNumberUnchecked(slot);
}

// key_obj 0 is an empty slot; key_obj 1 is a tombstone.
static inline bool weakmap_entry_is_occupied(const weakmap_entry_t *entry) {
  return entry->key_obj != 0 && entry->key_obj != 1;
}

static inline uint32_t weak_collection_key_hash(ant_value_t key) {
  uint32_t hash = (uint32_t)key ^ (uint32_t)(key >> 32);
  hash = ((hash >> 16) ^ hash) * UINT32_C(0x45d9f3b);
  return (hash >> 16) ^ hash;
}

static inline map_entry_t *map_iter_take(map_iterator_state_t *st) {
  collection_table_t *table = st->table;
  if (!table) return NULL;
  
  map_entry_t *entry = collections_map_next(table, &st->index);
  if (!entry) collections_iter_finish(st);
  
  return entry;
}

static inline set_entry_t *set_iter_take(set_iterator_state_t *st) {
  collection_table_t *table = st->table;
  if (!table) return NULL;
  
  set_entry_t *entry = collections_set_next(table, &st->index);
  if (!entry) collections_iter_finish(st);
  
  return entry;
}
