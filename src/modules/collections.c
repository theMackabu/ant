#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "gc.h"
#include "errors.h"
#include "internal.h"
#include "hash.h"
#include "numbers.h"
#include "gc/weak.h"
#include "silver/call.h"
#include "descriptors.h"
#include "protectors.h"
#include "arrays.h"

#include "modules/bigint.h"
#include "modules/collections.h"
#include "modules/microtasks.h"
#include "modules/process.h"
#include "modules/symbol.h"
#include "modules/iterator.h"

#undef HASH_INITIAL_NUM_BUCKETS
#undef HASH_INITIAL_NUM_BUCKETS_LOG2
#undef HASH_BKT_CAPACITY_THRESH

#define HASH_INITIAL_NUM_BUCKETS 8U
#define HASH_INITIAL_NUM_BUCKETS_LOG2 3U
#define HASH_BKT_CAPACITY_THRESH 6U

static bool can_be_held_weakly(ant_value_t value) {
  if (is_object_type(value) || vtype(value) == kTypeBuiltin) return true;
  return vtype(value) == kTypeSymbol && js_sym_key(value) == NULL;
}

enum {
  COLLECTION_KEY_VALUE,
  COLLECTION_KEY_STRING,
  COLLECTION_KEY_BIGINT,
};

typedef struct {
  ant_value_t key;
  const unsigned char *bytes;
  size_t len;
  uint32_t hash;
  uint8_t kind;
  bool negative;
} collection_probe_t;

typedef set_entry_t collection_slot_t;

static constexpr size_t MAP_STRIDE = sizeof(map_entry_t);
static constexpr size_t SET_STRIDE = sizeof(set_entry_t);

static constexpr uint32_t COLLECTION_MIN_CAPACITY = 8;
static constexpr uint32_t COLLECTION_MAX_CAPACITY = UINT32_C(1) << 30;
static constexpr ant_value_t COLLECTION_CANONICAL_NAN = UINT64_C(0x7FF8000000000000);
static constexpr uint64_t COLLECTION_BIGINT_SIGN = UINT64_C(0x9e3779b97f4a7c15);

static const uint32_t collection_empty_buckets[1] = { COLLECTION_END };
static collection_table_t collection_empty_table = { .buckets = (uint32_t *)collection_empty_buckets };

static inline uint32_t collection_value_hash(ant_value_t key) {
  return (uint32_t)ant_hash_mix(key ^ ant_hash_secret[0], ant_hash_secret[1]);
}

static inline uint32_t collection_string_hash(const unsigned char *bytes, size_t len) {
  return (uint32_t)hash_key((const char *)bytes, len);
}

static const uint32_t *collection_bigint_limbs(ant_t *js, ant_value_t value, size_t *count, bool *negative) {
  const uint32_t *limbs = bigint_limbs(js, value, count);
  while (*count > 1 && limbs[*count - 1] == 0) (*count)--;
  *negative = bigint_is_negative(js, value) && !(*count == 1 && limbs[0] == 0);
  return limbs;
}

static void collection_probe_bigint(ant_t *js, ant_value_t key, collection_probe_t *probe) {
  size_t count = 0;
  const uint32_t *limbs = collection_bigint_limbs(js, key, &count, &probe->negative);
  probe->key = key;
  probe->bytes = (const unsigned char *)limbs;
  probe->len = count * sizeof(uint32_t);
  probe->kind = COLLECTION_KEY_BIGINT;
  probe->hash = (uint32_t)(hash_key((const char *)limbs, probe->len) ^ (probe->negative ? COLLECTION_BIGINT_SIGN : 0));
}

static void collection_probe_flat(ant_value_t key, const ant_flat_string_t *flat, collection_probe_t *probe) {
  probe->key = key;
  probe->bytes = (const unsigned char *)flat->bytes;
  probe->len = (size_t)flat->len;
  probe->kind = COLLECTION_KEY_STRING;
}

static bool collection_probe_string(ant_t *js, ant_value_t key, collection_probe_t *probe) {
  ant_flat_string_t *flat = ant_str_flat_ptr(key);
  if (!flat) {
    key = str_materialize(js, key);
    if (is_err(key) || !(flat = ant_str_flat_ptr(key))) return false;
  }
  collection_probe_flat(key, flat, probe);
  probe->hash = collection_string_hash(probe->bytes, probe->len);
  return true;
}

static inline ant_value_t collection_normalize_number(ant_value_t key) {
  double number = Ant_Value_AsNumberUnchecked(key);
  if (number == 0.0) return Ant_Value_FromNumber(0.0);
  if (number != number) return COLLECTION_CANONICAL_NAN;
  return key;
}

__attribute__((always_inline))
static inline bool collection_probe_init(ant_t *js, ant_value_t input, collection_probe_t *probe) {
  uint8_t type = vtype(input);
  probe->kind = COLLECTION_KEY_VALUE;
  if (type == kTypeString) return collection_probe_string(js, input, probe);
  if (type == kTypeBigInt) {
    collection_probe_bigint(js, input, probe);
    return true;
  }
  probe->key = type == kTypeNumber ? collection_normalize_number(input) : input;
  probe->hash = collection_value_hash(probe->key);
  return true;
}

static void collection_probe_stored(ant_t *js, ant_value_t key, uint32_t hash, collection_probe_t *probe) {
  uint8_t type = vtype(key);
  probe->key = key;
  probe->kind = COLLECTION_KEY_VALUE;
  if (type == kTypeString) collection_probe_flat(key, ant_str_flat_ptr(key), probe);
  else if (type == kTypeBigInt) collection_probe_bigint(js, key, probe);
  probe->hash = hash;
}

static inline collection_slot_t *collection_slot(const collection_table_t *table, uint32_t index, size_t stride) {
  return (collection_slot_t *)(table->entries + (size_t)index * stride);
}

static inline bool collection_short_equal(const unsigned char *a, const unsigned char *b, size_t len) {
  if (len >= 8) {
    uint64_t a0, b0, a1, b1;
    memcpy(&a0, a, 8);
    memcpy(&b0, b, 8);
    memcpy(&a1, a + len - 8, 8);
    memcpy(&b1, b + len - 8, 8);
    return ((a0 ^ b0) | (a1 ^ b1)) == 0;
  }
  if (len >= 4) {
    uint32_t a0, b0, a1, b1;
    memcpy(&a0, a, 4);
    memcpy(&b0, b, 4);
    memcpy(&a1, a + len - 4, 4);
    memcpy(&b1, b + len - 4, 4);
    return ((a0 ^ b0) | (a1 ^ b1)) == 0;
  }
  for (size_t i = 0; i < len; i++) if (a[i] != b[i]) return false;
  return true;
}

static inline const ant_flat_string_t *collection_stored_string(ant_value_t stored, size_t len) {
  if (vtype(stored) != kTypeString) return NULL;
  const ant_flat_string_t *flat = (const ant_flat_string_t *)vptr(stored);
  return (size_t)flat->len == len ? flat : NULL;
}

static bool collection_bigint_equal(ant_t *js, ant_value_t stored, const collection_probe_t *probe) {
  if (vtype(stored) != kTypeBigInt) return false;
  if (stored == probe->key) return true;
  size_t count = 0;
  bool negative = false;
  const uint32_t *limbs = collection_bigint_limbs(js, stored, &count, &negative);
  return negative == probe->negative && count * sizeof(uint32_t) == probe->len && memcmp(limbs, probe->bytes, probe->len) == 0;
}

__attribute__((noinline))
static collection_slot_t *collection_find_long_string(const collection_table_t *table, const collection_probe_t *probe, size_t stride) {
  for (uint32_t index = table->buckets[probe->hash & table->mask]; index != COLLECTION_END;) {
    collection_slot_t *slot = collection_slot(table, index, stride);
    if (slot->hash == probe->hash) {
      if (slot->key == probe->key) return slot;
      const ant_flat_string_t *flat = collection_stored_string(slot->key, probe->len);
      if (flat && memcmp(flat->bytes, probe->bytes, probe->len) == 0) return slot;
    }
    index = slot->chain;
  }
  return NULL;
}

__attribute__((noinline))
static collection_slot_t *collection_find_string(const collection_table_t *table, const collection_probe_t *probe, size_t stride) {
  if (probe->len > 16) return collection_find_long_string(table, probe, stride);
  for (uint32_t index = table->buckets[probe->hash & table->mask]; index != COLLECTION_END;) {
    collection_slot_t *slot = collection_slot(table, index, stride);
    if (slot->hash == probe->hash) {
      if (slot->key == probe->key) return slot;
      const ant_flat_string_t *flat = collection_stored_string(slot->key, probe->len);
      if (flat && collection_short_equal((const unsigned char *)flat->bytes, probe->bytes, probe->len)) return slot;
    }
    index = slot->chain;
  }
  return NULL;
}

__attribute__((noinline))
static collection_slot_t *collection_find_bigint(
  ant_t *js, const collection_table_t *table, const collection_probe_t *probe, size_t stride
) {
  for (uint32_t index = table->buckets[probe->hash & table->mask]; index != COLLECTION_END;) {
    collection_slot_t *slot = collection_slot(table, index, stride);
    if (slot->hash == probe->hash && collection_bigint_equal(js, slot->key, probe)) return slot;
    index = slot->chain;
  }
  return NULL;
}

__attribute__((always_inline))
static inline collection_slot_t *collection_find(
  ant_t *js, const collection_table_t *table, const collection_probe_t *probe, size_t stride
) {
  if (__builtin_expect(probe->kind == COLLECTION_KEY_VALUE, 1)) {
    for (uint32_t index = table->buckets[probe->hash & table->mask]; index != COLLECTION_END;) {
      collection_slot_t *slot = collection_slot(table, index, stride);
      if (slot->key == probe->key) return slot;
      index = slot->chain;
    }
    return NULL;
  }
  if (probe->kind == COLLECTION_KEY_STRING) return collection_find_string(table, probe, stride);
  return collection_find_bigint(js, table, probe, stride);
}

static void collection_remap_iters(collection_table_t *table, const uint32_t *remap, uint32_t live) {
  for (collection_iter_state_t *it = table->iters; it; it = it->next)
    it->index = it->index < table->used ? remap[it->index] : live;
}

static void collection_link_chains(collection_table_t *table, size_t stride) {
  memset(table->buckets, 0xff, ((size_t)table->mask + 1) * sizeof(uint32_t));
  for (uint32_t i = 0; i < table->used; i++) {
    collection_slot_t *slot = collection_slot(table, i, stride);
    uint32_t *bucket = &table->buckets[slot->hash & table->mask];
    slot->chain = *bucket;
    *bucket = i;
  }
}

static bool collection_resize(collection_table_t *table, uint32_t capacity, size_t stride) {
  unsigned char *block = malloc((size_t)capacity * (stride + sizeof(uint32_t)));
  if (!block) return false;

  uint32_t *remap = table->iters ? table->buckets : NULL;
  uint32_t live = 0;

  for (uint32_t i = 0; i < table->used; i++) {
    collection_slot_t *from = collection_slot(table, i, stride);
    if (remap) remap[i] = live;
    if (from->key == COLLECTION_HOLE) continue;
    memcpy(block + (size_t)live * stride, from, stride);
    live++;
  }

  if (remap) collection_remap_iters(table, remap, live);
  free(table->entries);

  table->entries = block;
  table->buckets = (uint32_t *)(block + (size_t)capacity * stride);
  table->capacity = capacity;
  table->mask = capacity - 1;
  table->used = live;
  table->first = 0;
  collection_link_chains(table, stride);

  return true;
}

static void collection_compact(collection_table_t *table, size_t stride) {
  uint32_t *remap = table->iters ? table->buckets : NULL;
  uint32_t live = 0;

  for (uint32_t i = 0; i < table->used; i++) {
    collection_slot_t *from = collection_slot(table, i, stride);
    if (remap) remap[i] = live;
    if (from->key == COLLECTION_HOLE) continue;
    if (i != live) memcpy(collection_slot(table, live, stride), from, stride);
    live++;
  }

  if (remap) collection_remap_iters(table, remap, live);
  table->used = live;
  table->first = 0;
  collection_link_chains(table, stride);
}

static bool collection_make_room(collection_table_t *table, size_t stride) {
  if (!table->entries) return collection_resize(table, COLLECTION_MIN_CAPACITY, stride);
  if (table->size <= table->capacity / 2) {
    collection_compact(table, stride);
    return true;
  }
  if (table->capacity >= COLLECTION_MAX_CAPACITY) return false;
  return collection_resize(table, table->capacity * 2, stride);
}

__attribute__((always_inline))
static inline collection_slot_t *collection_append(collection_table_t *table, ant_value_t key, uint32_t hash, size_t stride) {
  if (__builtin_expect(table->used == table->capacity, 0) && !collection_make_room(table, stride)) return NULL;
  uint32_t index = table->used++;
  collection_slot_t *slot = collection_slot(table, index, stride);
  uint32_t *bucket = &table->buckets[hash & table->mask];
  slot->key = key;
  slot->hash = hash;
  slot->chain = *bucket;
  *bucket = index;
  table->size++;
  return slot;
}

static void collection_remove(collection_table_t *table, collection_slot_t *slot, size_t stride) {
  slot->key = COLLECTION_HOLE;
  if (stride == MAP_STRIDE) ((map_entry_t *)slot)->value = Ant_Value_Undefined;
  table->size--;

  uint32_t index = (uint32_t)(((unsigned char *)slot - table->entries) / stride);
  if (index == table->first) {
    uint32_t first = index + 1;
    while (first < table->used && collection_slot(table, first, stride)->key == COLLECTION_HOLE) first++;
    table->first = first;
  }

  if (table->size < table->capacity / 4 && table->capacity > COLLECTION_MIN_CAPACITY)
    (void)collection_resize(table, table->capacity / 2, stride);
}

static void collection_clear(collection_table_t *table) {
  if (table == &collection_empty_table) return;
  for (collection_iter_state_t *it = table->iters; it; it = it->next) it->index = 0;
  free(table->entries);
  table->entries = NULL;
  table->buckets = (uint32_t *)collection_empty_buckets;
  table->used = table->size = table->capacity = table->mask = table->first = 0;
}

static bool collection_copy(collection_table_t *dst, const collection_table_t *src, size_t stride) {
  if (src->size == 0) return true;
  if (dst->entries) return false;

  uint32_t capacity = src->capacity;
  while (capacity > COLLECTION_MIN_CAPACITY && src->size < capacity / 4) capacity /= 2;
  unsigned char *block = malloc((size_t)capacity * (stride + sizeof(uint32_t)));
  if (!block) return false;

  dst->entries = block;
  dst->buckets = (uint32_t *)(block + (size_t)capacity * stride);
  dst->capacity = capacity;
  dst->mask = capacity - 1;
  dst->first = 0;

  if (capacity == src->capacity && src->used == src->size) {
    memcpy(dst->entries, src->entries, (size_t)src->used * stride);
    memcpy(dst->buckets, src->buckets, (size_t)capacity * sizeof(uint32_t));
    dst->used = dst->size = src->used;
    return true;
  }

  uint32_t live = 0;
  for (uint32_t i = 0; i < src->used; i++) {
    collection_slot_t *from = collection_slot(src, i, stride);
    if (from->key == COLLECTION_HOLE) continue;
    memcpy(block + (size_t)live * stride, from, stride);
    live++;
  }
  dst->used = dst->size = live;
  collection_link_chains(dst, stride);
  return true;
}

static inline void collection_barrier(ant_t *js, const collection_table_t *table, ant_value_t value) {
  if (table->owner) gc_write_barrier(js, table->owner, value);
}

static void collection_barrier_all(ant_t *js, const collection_table_t *table, size_t stride) {
  if (!table->owner || table->owner->flags.generation != 1) return;
  for (uint32_t i = 0; i < table->used; i++) {
    collection_slot_t *slot = collection_slot(table, i, stride);
    collection_barrier(js, table, slot->key);
    if (stride == MAP_STRIDE) collection_barrier(js, table, ((map_entry_t *)slot)->value);
  }
}

static void collection_iter_link(collection_iter_state_t *st, collection_table_t *table, iter_type_t type) {
  st->type = type;
  st->table = table;
  st->prev = NULL;
  st->next = NULL;
  if (!table || table == &collection_empty_table) {
    st->index = 0;
    return;
  }
  st->index = table->first;
  st->next = table->iters;
  if (table->iters) table->iters->prev = st;
  table->iters = st;
}

void collections_iter_finish(collection_iter_state_t *st) {
  collection_table_t *table = st->table;
  if (!table) return;
  if (table == &collection_empty_table) {
    st->table = NULL;
    return;
  }
  if (st->prev) st->prev->next = st->next;
  else table->iters = st->next;
  if (st->next) st->next->prev = st->prev;
  st->table = NULL;
  st->prev = st->next = NULL;
}

void collections_map_iter_open(map_iterator_state_t *st, collection_table_t *map, iter_type_t type) {
  collection_iter_link(st, map, type);
}

void collections_set_iter_open(set_iterator_state_t *st, collection_table_t *set, iter_type_t type) {
  collection_iter_link(st, set, type);
}

void collections_map_iter_close(map_iterator_state_t *st) {
  collections_iter_finish(st);
}

void collections_set_iter_close(set_iterator_state_t *st) {
  collections_iter_finish(st);
}

static collection_table_t *collection_table_new(ant_object_t *owner) {
  collection_table_t *table = malloc(sizeof(*table));
  if (!table) return NULL;
  *table = (collection_table_t){ .buckets = (uint32_t *)collection_empty_buckets, .owner = owner };
  return table;
}

void collections_table_free(collection_table_t *table) {
  if (!table || table == &collection_empty_table) return;
  for (collection_iter_state_t *it = table->iters, *next; it; it = next) {
    next = it->next;
    it->table = NULL;
    it->prev = it->next = NULL;
  }
  free(table->entries);
  free(table);
}

static void collections_table_attach_empty(ant_value_t obj, uint8_t type) {
  js_obj_ptr(obj)->type_tag = type;
  js_set_native(obj, &collection_empty_table, type == kTypeMap ? MAP_NATIVE_TAG : SET_NATIVE_TAG);
}

static collection_table_t *collection_writable(ant_value_t obj, uint32_t tag) {
  collection_table_t *table = js_get_native(obj, tag);
  if (table != &collection_empty_table) return table;
  table = collection_table_new(js_obj_ptr(obj));
  if (table) js_set_native(obj, table, tag);
  return table;
}

collection_table_t *collections_table_attach(ant_t *js, ant_value_t obj, uint8_t type) {
  ant_object_t *ptr = js_obj_ptr(obj);
  collection_table_t *table = collection_table_new(ptr);
  if (!table) return NULL;
  ptr->type_tag = type;
  js_set_native(obj, table, type == kTypeMap ? MAP_NATIVE_TAG : SET_NATIVE_TAG);
  return table;
}

__attribute__((always_inline))
static inline map_entry_t *map_find(ant_t *js, const collection_table_t *map, const collection_probe_t *probe) {
  return (map_entry_t *)collection_find(js, map, probe, MAP_STRIDE);
}

__attribute__((always_inline))
static inline set_entry_t *set_find(ant_t *js, const collection_table_t *set, const collection_probe_t *probe) {
  return collection_find(js, set, probe, SET_STRIDE);
}

__attribute__((always_inline))
static inline map_entry_t *map_lookup(ant_t *js, const collection_table_t *map, ant_value_t key) {
  collection_probe_t probe;
  if (!collection_probe_init(js, key, &probe)) return NULL;
  return map_find(js, map, &probe);
}

__attribute__((always_inline))
static inline set_entry_t *set_lookup(ant_t *js, const collection_table_t *set, ant_value_t value) {
  collection_probe_t probe;
  if (!collection_probe_init(js, value, &probe)) return NULL;
  return set_find(js, set, &probe);
}

map_entry_t *Ant_Map_Find(ant_t *js, collection_table_t *map, ant_value_t key) {
  return map_lookup(js, map, key);
}

set_entry_t *Ant_Set_Find(ant_t *js, collection_table_t *set, ant_value_t value) {
  return set_lookup(js, set, value);
}

static map_entry_t *map_append(ant_t *js, collection_table_t *map, const collection_probe_t *probe, ant_value_t value) {
  map_entry_t *entry = (map_entry_t *)collection_append(map, probe->key, probe->hash, MAP_STRIDE);
  if (!entry) return NULL;
  entry->value = value;
  collection_barrier(js, map, probe->key);
  collection_barrier(js, map, value);
  return entry;
}

static bool map_store(ant_t *js, collection_table_t *map, ant_value_t key, ant_value_t value) {
  collection_probe_t probe;
  if (!collection_probe_init(js, key, &probe)) return false;
  map_entry_t *entry = map_find(js, map, &probe);
  if (!entry) return map_append(js, map, &probe, value) != NULL;
  entry->value = value;
  collection_barrier(js, map, value);
  return true;
}

static bool set_append(ant_t *js, collection_table_t *set, ant_value_t key, uint32_t hash) {
  if (!collection_append(set, key, hash, SET_STRIDE)) return false;
  collection_barrier(js, set, key);
  return true;
}

static bool set_store(ant_t *js, collection_table_t *set, ant_value_t value) {
  collection_probe_t probe;
  if (!collection_probe_init(js, value, &probe)) return false;
  if (set_find(js, set, &probe)) return true;
  return set_append(js, set, probe.key, probe.hash);
}

static bool map_remove_key(ant_t *js, collection_table_t *map, ant_value_t key) {
  map_entry_t *entry = map_lookup(js, map, key);
  if (!entry) return false;
  collection_remove(map, (collection_slot_t *)entry, MAP_STRIDE);
  return true;
}

static bool set_remove_key(ant_t *js, collection_table_t *set, ant_value_t value) {
  set_entry_t *entry = set_lookup(js, set, value);
  if (!entry) return false;
  collection_remove(set, entry, SET_STRIDE);
  return true;
}

ant_value_t collections_entry_pair(ant_t *js, ant_value_t first, ant_value_t second) {
  ant_value_t pair[2] = { first, second };
  return mkarr_dense_literal_capacity(js, pair, 2, true);
}

bool collections_map_store_cloned_entry(ant_t *js, collection_table_t *map, ant_value_t key, ant_value_t value) {
  return map_store(js, map, key, value);
}

bool collections_set_store_cloned_entry(ant_t *js, collection_table_t *set, ant_value_t value) {
  return set_store(js, set, value);
}

static ant_value_t collection_create(ant_t *js, uint8_t type, ant_value_t proto, collection_table_t **out) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) return obj;
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  collection_table_t *table = collections_table_attach(js, obj, type);
  if (!table) return Ant_Error_CreateOutOfMemory(js);
  if (out) *out = table;
  return obj;
}

ant_value_t collections_map_create(ant_t *js, collection_table_t **out_map) {
  return collection_create(js, kTypeMap, js->builtins.map_proto, out_map);
}

ant_value_t collections_set_create(ant_t *js, collection_table_t **out_set) {
  return collection_create(js, kTypeSet, js->builtins.set_proto, out_set);
}

collection_table_t *get_map_from_obj(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != kTypeMap) return NULL;
  return (collection_table_t *)js_get_native(obj, MAP_NATIVE_TAG);
}

collection_table_t *get_set_from_obj(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != kTypeSet) return NULL;
  return (collection_table_t *)js_get_native(obj, SET_NATIVE_TAG);
}

static weakmap_table_t *get_weakmap_from_obj(
  ant_value_t obj, ant_object_t **object_out
) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != kTypeWeakMap) return NULL;
  if (object_out) *object_out = ptr;
  return (weakmap_table_t *)js_get_native(obj, WEAKMAP_NATIVE_TAG);
}

static uint32_t weakmap_table_find_slot(
  weakmap_table_t *table, ant_value_t key, uint32_t hash,
  bool *found
) {
  uint32_t mask = table->capacity - 1;
  uint32_t slot = hash & mask;
  uint32_t first_tombstone = UINT32_MAX;

  for (;;) {
    ant_value_t stored_key = table->entries[slot].key_obj;
    if (stored_key == 0) {
      *found = false;
      return first_tombstone != UINT32_MAX ? first_tombstone : slot;
    }
    if (stored_key == key) {
      *found = true;
      return slot;
    }
    if (stored_key == 1 && first_tombstone == UINT32_MAX)
      first_tombstone = slot;
    slot = (slot + 1) & mask;
  }
}

static bool weakmap_table_rehash(
  weakmap_table_t *table, uint32_t new_capacity
) {
  weakmap_entry_t *new_entries = calloc(
    new_capacity, sizeof(*new_entries)
  );
  if (!new_entries) return false;

  weakmap_entry_t *old_entries = table->entries;
  uint32_t old_capacity = table->capacity;
  table->entries = new_entries;
  table->capacity = new_capacity;
  table->count = 0;
  table->tombstones = 0;

  for (uint32_t i = 0; i < old_capacity; i++) {
    weakmap_entry_t entry = old_entries[i];
    if (!weakmap_entry_is_occupied(&entry)) continue;

    bool found = false;
    uint32_t slot = weakmap_table_find_slot(
      table, entry.key_obj, weak_collection_key_hash(entry.key_obj), &found
    );
    table->entries[slot] = entry;
    table->count++;
  }

  free(old_entries);
  return true;
}

weakmap_entry_t *weakmap_table_find(
  weakmap_table_t *table, ant_value_t key
) {
  if (!table || table->capacity == 0) return NULL;
  bool found = false;
  uint32_t slot = weakmap_table_find_slot(
    table, key, weak_collection_key_hash(key), &found
  );
  return found ? &table->entries[slot] : NULL;
}

static bool weakmap_table_store(
  weakmap_table_t *table, ant_value_t key, ant_value_t value
) {
  if (!table) return false;
  if (table->capacity == 0 && !weakmap_table_rehash(table, 8))
    return false;

  uint32_t hash = weak_collection_key_hash(key);
  bool found = false;
  uint32_t slot = weakmap_table_find_slot(table, key, hash, &found);
  if (found) {
    table->entries[slot].value = value;
    return true;
  }

  uint64_t used = (uint64_t)table->count + table->tombstones + 1;
  if (used * 4 >= (uint64_t)table->capacity * 3) {
    uint32_t new_capacity = table->capacity;
    if (table->tombstones <= table->count) {
      if (new_capacity > UINT32_MAX / 2) return false;
      new_capacity *= 2;
    }
    if (!weakmap_table_rehash(table, new_capacity)) return false;
    slot = weakmap_table_find_slot(table, key, hash, &found);
  }

  if (table->entries[slot].key_obj == 1) table->tombstones--;
  table->entries[slot] = (weakmap_entry_t){
    .key_obj = key,
    .value = value
  };
  table->count++;
  return true;
}

bool weakmap_table_delete(weakmap_table_t *table, ant_value_t key) {
  weakmap_entry_t *entry = weakmap_table_find(table, key);
  if (!entry) return false;
  entry->key_obj = 1;
  entry->value = 0;
  table->count--;
  table->tombstones++;
  return true;
}

void weakmap_table_finish_prune(weakmap_table_t *table) {
  if (!table) return;
  table->gc_prune_pending = false;
  if (table->capacity == 0 || table->tombstones == 0) return;

  if (table->count == 0) {
    memset(table->entries, 0, table->capacity * sizeof(*table->entries));
    table->tombstones = 0;
    return;
  }

  if (table->tombstones > table->count)
    (void)weakmap_table_rehash(table, table->capacity);
}

void weakmap_table_free(weakmap_table_t *table) {
  if (!table) return;
  free(table->entries);
  free(table);
}

static inline bool weakmap_store_entry(
  ant_t *js,
  ant_object_t *object,
  weakmap_table_t *table,
  ant_value_t key,
  ant_value_t value
) {
  if (!weakmap_table_store(table, key, value)) return false;
  if (object && object->flags.generation == 1) {
    gc_weak_remember_map(js, object, key, value);
    gc_write_barrier(js, object, key);
    gc_write_barrier(js, object, value);
  }
  return true;
}

static weakset_entry_t **get_weakset_from_obj(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != kTypeWeakSet) return NULL;
  return (weakset_entry_t **)js_get_native(obj, WEAKSET_NATIVE_TAG);
}

map_iterator_state_t *get_map_iter_state(ant_value_t obj) {
  if (vtype(obj) == kTypeObject) {
    ant_object_t *ptr = (ant_object_t *)vptr(obj);
    if (ptr && ptr->native_tag == MAP_ITER_NATIVE_TAG) return (map_iterator_state_t *)ptr->native_ptr;
  }
  
  return (map_iterator_state_t *)js_get_native(obj, MAP_ITER_NATIVE_TAG);
}

set_iterator_state_t *get_set_iter_state(ant_value_t obj) {
  if (vtype(obj) == kTypeObject) {
    ant_object_t *ptr = (ant_object_t *)vptr(obj);
    if (ptr && ptr->native_tag == SET_ITER_NATIVE_TAG) return (set_iterator_state_t *)ptr->native_ptr;
  }

  return (set_iterator_state_t *)js_get_native(obj, SET_ITER_NATIVE_TAG);
}

static ant_value_t collection_receiver_error(ant_t *js, const char *method) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Method %s called on incompatible receiver", method);
}

static inline ant_value_t collection_arg(ant_args_t, int i) {
  return i < nargs ? args[i] : Ant_Value_Undefined;
}

static ant_value_t map_set(ant_params_t) {
  if (!get_map_from_obj(call_this)) return collection_receiver_error(js, "Map.prototype.set");
  collection_table_t *map = collection_writable(call_this, MAP_NATIVE_TAG);

  if (!map || !map_store(js, map, collection_arg(args, nargs, 0), collection_arg(args, nargs, 1)))
    return Ant_Error_CreateOutOfMemory(js);

  return call_this;
}

ant_value_t collections_map_get(ant_t *js, ant_value_t map, ant_value_t key) {
  collection_table_t *table = get_map_from_obj(map);
  if (!table) return collection_receiver_error(js, "Map.prototype.get");
  map_entry_t *entry = map_lookup(js, table, key);
  return entry ? entry->value : Ant_Value_Undefined;
}

static ant_value_t map_get(ant_params_t) {
  return collections_map_get(js, call_this, collection_arg(args, nargs, 0));
}

bool collections_is_map_get_builtin(ant_value_t func) {
  return vtype(func) == kTypeBuiltin && js_cfunc_same_entrypoint(func, map_get);
}

static size_t collection_number_to_string(double value, char *buf, size_t len) {
  if (__builtin_expect(isnan(value), 0)) {
    if (len >= 3) memcpy(buf, "NaN", 3);
    return 3;
  }
  if (__builtin_expect(isinf(value), 0)) {
    const char *text = value > 0 ? "Infinity" : "-Infinity";
    size_t text_len = value > 0 ? 8 : 9;
    if (len >= text_len) memcpy(buf, text, text_len);
    return text_len;
  }
  return ant_number_to_shortest(value, buf, len);
}

static map_entry_t *map_find_bytes(ant_t *js, const collection_table_t *map, const unsigned char *bytes, size_t len) {
  collection_probe_t probe = {
    .key = Ant_Value_Undefined,
    .bytes = bytes,
    .len = len,
    .hash = collection_string_hash(bytes, len),
    .kind = COLLECTION_KEY_STRING,
  };
  return map_find(js, map, &probe);
}

__attribute__((aligned(64))) ant_value_t collections_map_get_numeric_pair(
  ant_t *js, ant_value_t map, ant_value_t left,
  const char *separator, size_t separator_len, ant_value_t right
) {
  collection_table_t *table = get_map_from_obj(map);
  if (!table) return collection_receiver_error(js, "Map.prototype.get");

  unsigned char inline_key[128];
  char right_buf[32];

  size_t left_len = collection_number_to_string(Ant_Value_AsNumberUnchecked(left), (char *)inline_key, sizeof(right_buf));
  if (left_len > sizeof(right_buf)) return Ant_Error_CreateOutOfMemory(js);

  bool right_inline = separator_len <= sizeof(inline_key) - left_len - sizeof(right_buf);
  char *right_out = right_inline ? (char *)inline_key + left_len + separator_len : right_buf;
  size_t right_len = collection_number_to_string(Ant_Value_AsNumberUnchecked(right), right_out, sizeof(right_buf));

  if (right_len > sizeof(right_buf) ||
    separator_len > SIZE_MAX - left_len - right_len)
    return Ant_Error_CreateOutOfMemory(js);

  size_t key_len = left_len + separator_len + right_len;
  unsigned char *key = inline_key;

  if (key_len > sizeof(inline_key)) {
    key = malloc(key_len);
    if (!key) return Ant_Error_CreateOutOfMemory(js);
    memcpy(key, inline_key, left_len);
  }

  if (separator_len > 0) memcpy(key + left_len, separator, separator_len);
  if (!right_inline) memcpy(key + left_len + separator_len, right_buf, right_len);

  map_entry_t *entry = map_find_bytes(js, table, key, key_len);
  if (key != inline_key) free(key);
  return entry ? entry->value : Ant_Value_Undefined;
}

ant_value_t collections_map_has(ant_t *js, ant_value_t map, ant_value_t key) {
  collection_table_t *table = get_map_from_obj(map);
  if (!table) return collection_receiver_error(js, "Map.prototype.has");
  return Ant_Value_FromBool(map_lookup(js, table, key) != NULL);
}

static ant_value_t map_has(ant_params_t) {
  return collections_map_has(js, call_this, collection_arg(args, nargs, 0));
}

bool collections_is_map_has_builtin(ant_value_t func) {
  return vtype(func) == kTypeBuiltin && js_cfunc_same_entrypoint(func, map_has);
}

static ant_value_t map_numeric_template_general(
  ant_t *js, collection_table_t *map, bool return_presence,
  const ant_value_t *substitutions, const sv_map_template_desc_t *desc
) {
  uint8_t substitution_count = desc->substitution_count;
  char number_bufs[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS][32];

  size_t number_lens[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS] = {0};
  size_t key_len = 0;

  for (uint8_t i = 0; i < substitution_count; i++) {
    number_lens[i] = collection_number_to_string(
      Ant_Value_AsNumberUnchecked(substitutions[i]), number_bufs[i], sizeof(number_bufs[i]));
    if (number_lens[i] > sizeof(number_bufs[i]) ||
        desc->segments[i].len > SIZE_MAX - key_len ||
        number_lens[i] > SIZE_MAX - key_len - desc->segments[i].len)
      return Ant_Error_CreateOutOfMemory(js);
    key_len += desc->segments[i].len + number_lens[i];
  }

  if (desc->segments[substitution_count].len > SIZE_MAX - key_len) return Ant_Error_CreateOutOfMemory(js);
  key_len += desc->segments[substitution_count].len;

  unsigned char inline_key[64];
  unsigned char *key = inline_key;

  if (key_len > sizeof(inline_key)) {
    key = malloc(key_len);
    if (!key) return Ant_Error_CreateOutOfMemory(js);
  }

  unsigned char *out = key;

  for (uint8_t i = 0; i < substitution_count; i++) {
    const sv_atom_t *segment = &desc->segments[i];
    if (segment->len > 0) {
      memcpy(out, segment->str, segment->len);
      out += segment->len;
    }
    memcpy(out, number_bufs[i], number_lens[i]);
    out += number_lens[i];
  }

  const sv_atom_t *tail = &desc->segments[substitution_count];
  if (tail->len > 0) memcpy(out, tail->str, tail->len);

  map_entry_t *entry = map_find_bytes(js, map, key, key_len);
  if (key != inline_key) free(key);

  return return_presence
    ? Ant_Value_FromBool(entry != NULL)
    : (entry ? entry->value : Ant_Value_Undefined);
}

ant_value_t collections_map_numeric_template(
  ant_t *js, ant_value_t map, bool return_presence,
  const ant_value_t *substitutions,
  const sv_map_template_desc_t *desc
) {
  if (!substitutions || !desc || desc->substitution_count == 0 ||
      desc->substitution_count > SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS)
    return js_mkerr(js, "invalid numeric map template");

  for (uint8_t i = 0; i < desc->substitution_count; i++)
    if (vtype(substitutions[i]) != kTypeNumber)
      return js_mkerr(js, "invalid numeric map template substitution");

  if (sv_map_template_is_canonical_pair_get(desc))
    return collections_map_get_numeric_pair(
      js, map, substitutions[0],
      desc->segments[1].str, desc->segments[1].len, substitutions[1]);

  collection_table_t *table = get_map_from_obj(map);
  if (!table) return collection_receiver_error(js, return_presence ? "Map.prototype.has" : "Map.prototype.get");

  return map_numeric_template_general(
    js, table, return_presence, substitutions, desc);
}

static ant_value_t map_upsert(ant_params_t) {
  collection_table_t *map = get_map_from_obj(call_this);
  if (!map) return collection_receiver_error(js, "Map.prototype.upsert");

  ant_value_t key = collection_arg(args, nargs, 0);
  ant_value_t update_fn = collection_arg(args, nargs, 1);
  ant_value_t insert_fn = collection_arg(args, nargs, 2);

  if (!is_callable(update_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map.upsert update callback must be callable");

  if (!is_callable(insert_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map.upsert insert callback must be callable");

  map_entry_t *entry = map_lookup(js, map, key);
  ant_value_t value;

  if (entry) {
    ant_value_t call_args[3] = { entry->value, key, call_this };
    value = sv_vm_call(js->vm, js, update_fn, Ant_Value_Undefined, call_args, 3, NULL, Ant_Value_Undefined);
  } else {
    ant_value_t call_args[2] = { key, call_this };
    value = sv_vm_call(js->vm, js, insert_fn, Ant_Value_Undefined, call_args, 2, NULL, Ant_Value_Undefined);
  }

  if (is_err(value)) return value;
  map = collection_writable(call_this, MAP_NATIVE_TAG);
  if (!map || !map_store(js, map, key, value)) return Ant_Error_CreateOutOfMemory(js);

  return value;
}

static ant_value_t map_get_or_insert(ant_params_t) {
  collection_table_t *map = get_map_from_obj(call_this);
  if (!map) return collection_receiver_error(js, "Map.prototype.getOrInsert");

  collection_probe_t probe;
  ant_value_t value = collection_arg(args, nargs, 1);
  if (!collection_probe_init(js, collection_arg(args, nargs, 0), &probe)) return Ant_Error_CreateOutOfMemory(js);

  map_entry_t *entry = map_find(js, map, &probe);
  if (entry) return entry->value;
  map = collection_writable(call_this, MAP_NATIVE_TAG);
  if (!map || !map_append(js, map, &probe, value)) return Ant_Error_CreateOutOfMemory(js);

  return value;
}

static ant_value_t map_get_or_insert_computed(ant_params_t) {
  collection_table_t *map = get_map_from_obj(call_this);
  if (!map) return collection_receiver_error(js, "Map.prototype.getOrInsertComputed");

  ant_value_t key = collection_arg(args, nargs, 0);
  ant_value_t callback = collection_arg(args, nargs, 1);

  if (!is_callable(callback)) return js_mkerr_typed(js, JS_ERR_TYPE, "Map.prototype.getOrInsertComputed callback must be callable");
  if (vtype(key) == kTypeNumber && Ant_Value_AsNumberUnchecked(key) == 0) key = Ant_Value_FromNumber(0.0);

  map_entry_t *entry = map_lookup(js, map, key);
  if (entry) return entry->value;

  ant_value_t call_args[1] = { key };
  ant_value_t value = sv_vm_call(js->vm, js, callback, Ant_Value_Undefined, call_args, 1, NULL, Ant_Value_Undefined);
  if (is_err(value)) return value;
  map = collection_writable(call_this, MAP_NATIVE_TAG);
  if (!map || !map_store(js, map, key, value)) return Ant_Error_CreateOutOfMemory(js);

  return value;
}

static ant_value_t map_delete(ant_params_t) {
  collection_table_t *map = get_map_from_obj(call_this);
  if (!map) return collection_receiver_error(js, "Map.prototype.delete");
  return Ant_Value_FromBool(map_remove_key(js, map, collection_arg(args, nargs, 0)));
}

static ant_value_t map_clear(ant_params_t) {
  collection_table_t *map = get_map_from_obj(call_this);
  if (!map) return collection_receiver_error(js, "Map.prototype.clear");
  collection_clear(map);
  return Ant_Value_Undefined;
}

static ant_value_t map_size(ant_params_t) {
  collection_table_t *map = get_map_from_obj(call_this);
  if (!map) return collection_receiver_error(js, "get Map.prototype.size");
  return Ant_Value_FromNumber((double)map->size);
}

static ant_value_t map_forEach(ant_params_t) {
  collection_table_t *map = get_map_from_obj(call_this);
  if (!map) return collection_receiver_error(js, "Map.prototype.forEach");

  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "forEach requires a callback function");

  sv_callback_t cb = sv_callback_prepare(js, args[0], nargs > 1 ? args[1] : Ant_Value_Undefined);

  map_iterator_state_t it;
  collections_map_iter_open(&it, map, ITER_TYPE_MAP_ENTRIES);
  ant_value_t result = Ant_Value_Undefined;

  for (map_entry_t *entry; (entry = map_iter_take(&it));) {
    ant_value_t call_args[3] = { entry->value, entry->key, call_this };
    result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(result)) break;
  }

  collections_map_iter_close(&it);
  return is_err(result) ? result : Ant_Value_Undefined;
}

static void collection_iter_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  uint32_t tag = obj->native_tag;
  if (tag != MAP_ITER_NATIVE_TAG && tag != SET_ITER_NATIVE_TAG) return;
  collection_iter_state_t *st = js_get_native(value, tag);
  if (st) {
    collections_iter_finish(st);
    free(st);
  }
  js_clear_native(value, tag);
}

bool advance_map(ant_t *js, iterator_t *it, ant_value_t *out) {
  map_iterator_state_t *state = get_map_iter_state(it->iterator);
  map_entry_t *entry = state ? map_iter_take(state) : NULL;
  if (!entry) return false;
  *out = map_iter_value(js, state, entry);
  return true;
}

static ant_value_t map_iter_next(ant_params_t) {
  if (!js_check_native_tag(call_this, MAP_ITER_NATIVE_TAG)) return collection_receiver_error(js, "Map Iterator.prototype.next");
  return js_iter_next_result(js, call_this, advance_map);
}

static ant_value_t create_collection_iterator(
  ant_t *js, ant_value_t source, collection_table_t *table,
  ant_value_t proto, uint32_t tag, iter_type_t type
) {
  ant_value_t iter = Ant_Object_CreateBare(js);
  if (is_err(iter)) return iter;

  collection_iter_state_t *state = malloc(sizeof(*state));
  if (!state) return Ant_Error_CreateOutOfMemory(js);
  collection_iter_link(state, table, type);

  js_set_proto_init(iter, proto);
  js_set_slot_wb(js, iter, SLOT_DATA, source);
  js_set_native(iter, state, tag);
  js_set_finalizer(iter, collection_iter_finalize);

  return iter;
}

static ant_value_t create_map_iterator(ant_t *js, ant_value_t map_obj, iter_type_t type, const char *method) {
  if (!get_map_from_obj(map_obj)) return collection_receiver_error(js, method);
  collection_table_t *map = collection_writable(map_obj, MAP_NATIVE_TAG);
  if (!map) return Ant_Error_CreateOutOfMemory(js);
  return create_collection_iterator(js, map_obj, map, js->builtins.map_iter_proto, MAP_ITER_NATIVE_TAG, type);
}

static ant_value_t map_values(ant_params_t) {
  return create_map_iterator(js, call_this, ITER_TYPE_MAP_VALUES, "Map.prototype.values");
}

static ant_value_t map_keys(ant_params_t) {
  return create_map_iterator(js, call_this, ITER_TYPE_MAP_KEYS, "Map.prototype.keys");
}

static ant_value_t map_entries(ant_params_t) {
  return create_map_iterator(js, call_this, ITER_TYPE_MAP_ENTRIES, "Map.prototype.entries");
}

bool advance_set(ant_t *js, iterator_t *it, ant_value_t *out) {
  set_iterator_state_t *state = get_set_iter_state(it->iterator);
  set_entry_t *entry = state ? set_iter_take(state) : NULL;
  if (!entry) return false;
  *out = set_iter_value(js, state, entry);
  return true;
}

static ant_value_t set_iter_next(ant_params_t) {
  if (!js_check_native_tag(call_this, SET_ITER_NATIVE_TAG)) return collection_receiver_error(js, "Set Iterator.prototype.next");
  return js_iter_next_result(js, call_this, advance_set);
}

static ant_value_t create_set_iterator(ant_t *js, ant_value_t set_obj, iter_type_t type, const char *method) {
  if (!get_set_from_obj(set_obj)) return collection_receiver_error(js, method);
  collection_table_t *set = collection_writable(set_obj, SET_NATIVE_TAG);
  if (!set) return Ant_Error_CreateOutOfMemory(js);
  return create_collection_iterator(js, set_obj, set, js->builtins.set_iter_proto, SET_ITER_NATIVE_TAG, type);
}

static ant_value_t set_add(ant_params_t) {
  if (!get_set_from_obj(call_this)) return collection_receiver_error(js, "Set.prototype.add");
  collection_table_t *set = collection_writable(call_this, SET_NATIVE_TAG);

  if (!set || !set_store(js, set, collection_arg(args, nargs, 0)))
    return Ant_Error_CreateOutOfMemory(js);

  return call_this;
}

ant_value_t collections_set_has(ant_t *js, ant_value_t set, ant_value_t value) {
  collection_table_t *table = get_set_from_obj(set);
  if (!table) return collection_receiver_error(js, "Set.prototype.has");
  return Ant_Value_FromBool(set_lookup(js, table, value) != NULL);
}

static ant_value_t set_has(ant_params_t) {
  return collections_set_has(js, call_this, collection_arg(args, nargs, 0));
}

bool collections_is_set_has_builtin(ant_value_t func) {
  return vtype(func) == kTypeBuiltin && js_cfunc_same_entrypoint(func, set_has);
}

static ant_value_t set_delete(ant_params_t) {
  collection_table_t *set = get_set_from_obj(call_this);
  if (!set) return collection_receiver_error(js, "Set.prototype.delete");
  return Ant_Value_FromBool(set_remove_key(js, set, collection_arg(args, nargs, 0)));
}

static ant_value_t set_clear(ant_params_t) {
  collection_table_t *set = get_set_from_obj(call_this);
  if (!set) return collection_receiver_error(js, "Set.prototype.clear");
  collection_clear(set);
  return Ant_Value_Undefined;
}

static ant_value_t set_size(ant_params_t) {
  collection_table_t *set = get_set_from_obj(call_this);
  if (!set) return collection_receiver_error(js, "get Set.prototype.size");
  return Ant_Value_FromNumber((double)set->size);
}

static ant_value_t set_values(ant_params_t) {
  return create_set_iterator(js, call_this, ITER_TYPE_SET_VALUES, "Set.prototype.values");
}

static ant_value_t set_entries(ant_params_t) {
  return create_set_iterator(js, call_this, ITER_TYPE_SET_ENTRIES, "Set.prototype.entries");
}

static ant_value_t set_forEach(ant_params_t) {
  collection_table_t *set = get_set_from_obj(call_this);
  if (!set) return collection_receiver_error(js, "Set.prototype.forEach");

  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "forEach requires a callback function");

  sv_callback_t cb = sv_callback_prepare(js, args[0], nargs > 1 ? args[1] : Ant_Value_Undefined);

  set_iterator_state_t it;
  collections_set_iter_open(&it, set, ITER_TYPE_SET_VALUES);
  ant_value_t result = Ant_Value_Undefined;

  for (set_entry_t *entry; (entry = set_iter_take(&it));) {
    ant_value_t call_args[3] = { entry->key, entry->key, call_this };
    result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(result)) break;
  }

  collections_set_iter_close(&it);
  return is_err(result) ? result : Ant_Value_Undefined;
}

static bool set_result_add(ant_t *js, collection_table_t *set, ant_value_t value) {
  return set_store(js, set, value);
}

static bool set_result_clone(ant_t *js, collection_table_t *set, const set_entry_t *from) {
  return set_append(js, set, from->key, from->hash);
}

static bool set_result_copy(ant_t *js, collection_table_t *set, const collection_table_t *from) {
  if (!collection_copy(set, from, SET_STRIDE)) return false;
  collection_barrier_all(js, set, SET_STRIDE);
  return true;
}

static void set_result_delete(ant_t *js, collection_table_t *set, ant_value_t value) {
  set_remove_key(js, set, value);
}

typedef struct {
  ant_value_t obj;
  ant_value_t has;
  ant_value_t keys;
  double size;
  collection_table_t *native;
} set_record_t;

typedef enum {
  SET_KEY_CONTINUE,
  SET_KEY_STOP,
} set_key_status_t;

typedef set_key_status_t (*set_key_cb)(
  ant_t *js,
  ant_value_t value,
  ant_value_t *result,
  void *ctx
);

static ant_value_t get_set_record(ant_t *js, ant_value_t value, const char *method, set_record_t *out) {
  if (!is_object_type(value))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set.%s() requires a set-like object", method);

  ant_value_t size = js_getprop_fallback(js, value, "size");
  if (is_err(size)) return size;

  if (vtype(size) == kTypeBigInt || vtype(size) == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set.%s() requires a numeric size", method);
  double num_size = js_to_number(js, size);
  if (isnan(num_size))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set.%s() requires a numeric size", method);
  double int_size = (num_size == 0.0 || !isfinite(num_size))
    ? num_size
    : (num_size < 0 ? -floor(-num_size) : floor(num_size));
  if (int_size < 0)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Set.%s() requires a non-negative size", method);

  ant_value_t has = js_getprop_fallback(js, value, "has");
  if (is_err(has)) return has;
  if (!is_callable(has))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set.%s() requires a callable has method", method);

  ant_value_t keys = js_getprop_fallback(js, value, "keys");
  if (is_err(keys)) return keys;
  if (!is_callable(keys))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set.%s() requires a callable keys method", method);

  out->obj = value;
  out->has = has;
  out->keys = keys;
  out->size = int_size;
  out->native = vtype(has) == kTypeBuiltin && js_cfunc_same_entrypoint(has, set_has) ? get_set_from_obj(value) : NULL;

  return Ant_Value_Undefined;
}

static ant_value_t set_record_has(ant_t *js, set_record_t *record, ant_value_t value, bool *out) {
  ant_value_t result = sv_vm_call(js->vm, js, record->has, record->obj, &value, 1, NULL, Ant_Value_Undefined);
  if (is_err(result)) return result;
  *out = js_truthy(js, result);
  return Ant_Value_Undefined;
}

static set_entry_t *set_find_entry(ant_t *js, const collection_table_t *set, const set_entry_t *entry) {
  collection_probe_t probe;
  collection_probe_stored(js, entry->key, entry->hash, &probe);
  return set_find(js, set, &probe);
}

static ant_value_t set_record_has_entry(ant_t *js, set_record_t *record, const set_entry_t *entry, bool *out) {
  if (!record->native) return set_record_has(js, record, entry->key, out);
  *out = set_find_entry(js, record->native, entry) != NULL;
  return Ant_Value_Undefined;
}

static ant_value_t set_record_close_keys_iterator(ant_t *js, ant_value_t iterator) {
  iterator_t it = { .iterator = iterator };
  js_iter_close(js, &it);
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : Ant_Value_Undefined;
}

static ant_value_t set_record_for_each_key(ant_t *js, set_record_t *record, set_key_cb cb, void *ctx) {
  ant_value_t iterator = sv_vm_call(js->vm, js, record->keys, record->obj, NULL, 0, NULL, Ant_Value_Undefined);
  if (is_err(iterator)) return iterator;
  if (!is_object_type(iterator))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set keys() result is not an iterator");

  ant_value_t next_fn = js_getprop_fallback(js, iterator, "next");
  if (is_err(next_fn)) return next_fn;
  if (!is_callable(next_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set keys() iterator has no callable next method");

  while (true) {
    ant_value_t next = sv_vm_call(js->vm, js, next_fn, iterator, NULL, 0, NULL, Ant_Value_Undefined);
    if (is_err(next)) return next;
    if (!is_object_type(next))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Set keys() iterator result is not an object");

    ant_value_t done = js_getprop_fallback(js, next, "done");
    if (is_err(done)) return done;
    if (js_truthy(js, done)) return Ant_Value_Undefined;

    ant_value_t value = js_getprop_fallback(js, next, "value");
    if (is_err(value)) return value;

    ant_value_t result = Ant_Value_Undefined;
    set_key_status_t status = cb(js, value, &result, ctx);

    if (is_err(result)) {
      ant_value_t close_result = set_record_close_keys_iterator(js, iterator);
      return is_err(close_result) ? close_result : result;
    }

    if (status == SET_KEY_STOP) {
      ant_value_t close_result = set_record_close_keys_iterator(js, iterator);
      return is_err(close_result) ? close_result : Ant_Value_Undefined;
    }
  }
}

typedef struct {
  collection_table_t *out_set;
  collection_table_t *this_set;
} set_build_ctx_t;

static set_key_status_t set_add_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_build_ctx_t *build = (set_build_ctx_t *)ctx;
  if (!set_result_add(js, build->out_set, value))
    *result = Ant_Error_CreateOutOfMemory(js);
  return SET_KEY_CONTINUE;
}

static ant_value_t set_union(ant_params_t) {
  collection_table_t *this_set = get_set_from_obj(call_this);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.union");

  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "union", &other);
  if (is_err(rec)) return rec;

  collection_table_t *out_set = NULL;
  ant_value_t out = collections_set_create(js, &out_set);
  if (is_err(out)) return out;
  set_build_ctx_t build = { out_set, this_set };

  if (!set_result_copy(js, out_set, this_set)) return Ant_Error_CreateOutOfMemory(js);
  ant_value_t result = set_record_for_each_key(js, &other, set_add_key_cb, &build);

  return is_err(result) ? result : out;
}

static set_key_status_t set_intersection_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_build_ctx_t *build = (set_build_ctx_t *)ctx;
  if (
    set_lookup(js, build->this_set, value) &&
    !set_result_add(js, build->out_set, value)
  ) *result = Ant_Error_CreateOutOfMemory(js);
  return SET_KEY_CONTINUE;
}

static ant_value_t set_intersection(ant_params_t) {
  collection_table_t *this_set = get_set_from_obj(call_this);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.intersection");

  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "intersection", &other);
  if (is_err(rec)) return rec;

  collection_table_t *out_set = NULL;
  ant_value_t out = collections_set_create(js, &out_set);
  if (is_err(out)) return out;

  if ((double)this_set->size <= other.size) {
    set_iterator_state_t it;
    collections_set_iter_open(&it, this_set, ITER_TYPE_SET_VALUES);
    ant_value_t result = out;
    for (set_entry_t *entry; (entry = set_iter_take(&it));) {
      bool has = false;
      ant_value_t value = entry->key;
      uint32_t hash = entry->hash;
      result = set_record_has_entry(js, &other, entry, &has);
      if (is_err(result)) break;
      result = out;
      if (!has) continue;
      collection_probe_t probe;
      collection_probe_stored(js, value, hash, &probe);
      if (set_find(js, out_set, &probe)) continue;
      if (!set_append(js, out_set, value, hash)) {
        result = Ant_Error_CreateOutOfMemory(js);
        break;
      }
    }
    collections_set_iter_close(&it);
    return result;
  }

  set_build_ctx_t build = { out_set, this_set };
  ant_value_t result = set_record_for_each_key(js, &other, set_intersection_key_cb, &build);

  return is_err(result) ? result : out;
}

static set_key_status_t set_delete_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_build_ctx_t *build = (set_build_ctx_t *)ctx;
  set_result_delete(js, build->out_set, value);
  return SET_KEY_CONTINUE;
}

static ant_value_t set_difference(ant_params_t) {
  collection_table_t *this_set = get_set_from_obj(call_this);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.difference");

  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "difference", &other);
  if (is_err(rec)) return rec;

  collection_table_t *out_set = NULL;
  ant_value_t out = collections_set_create(js, &out_set);
  if (is_err(out)) return out;

  bool scan = (double)this_set->size <= other.size;
  if (scan && other.native) {
    for (uint32_t index = 0; index < this_set->used;) {
      set_entry_t *each = collections_set_next(this_set, &index);
      if (!each) break;
      if (!set_find_entry(js, other.native, each) && !set_result_clone(js, out_set, each))
        return Ant_Error_CreateOutOfMemory(js);
    }
    return out;
  }

  if (!set_result_copy(js, out_set, this_set)) return Ant_Error_CreateOutOfMemory(js);

  if (scan) {
    set_iterator_state_t it;
    collections_set_iter_open(&it, this_set, ITER_TYPE_SET_VALUES);
    ant_value_t result = out;
    for (set_entry_t *each; (each = set_iter_take(&it));) {
      bool has = false;
      ant_value_t value = each->key;
      uint32_t hash = each->hash;
      result = set_record_has_entry(js, &other, each, &has);
      if (is_err(result)) break;
      result = out;
      if (!has) continue;
      collection_probe_t probe;
      collection_probe_stored(js, value, hash, &probe);
      set_entry_t *twin = set_find(js, out_set, &probe);
      if (twin) collection_remove(out_set, twin, SET_STRIDE);
    }
    collections_set_iter_close(&it);
    return result;
  }

  set_build_ctx_t build = { out_set, this_set };
  ant_value_t result = set_record_for_each_key(js, &other, set_delete_key_cb, &build);

  return is_err(result) ? result : out;
}

static set_key_status_t set_symmetric_difference_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_build_ctx_t *build = (set_build_ctx_t *)ctx;
  if (set_lookup(js, build->this_set, value)) {
    set_result_delete(js, build->out_set, value);
  } else if (!set_result_add(js, build->out_set, value)) *result = Ant_Error_CreateOutOfMemory(js);
  return SET_KEY_CONTINUE;
}

static ant_value_t set_symmetricDifference(ant_params_t) {
  collection_table_t *this_set = get_set_from_obj(call_this);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.symmetricDifference");

  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "symmetricDifference", &other);
  if (is_err(rec)) return rec;

  collection_table_t *out_set = NULL;
  ant_value_t out = collections_set_create(js, &out_set);
  if (is_err(out)) return out;

  if (!set_result_copy(js, out_set, this_set)) return Ant_Error_CreateOutOfMemory(js);
  set_build_ctx_t build = { out_set, this_set };
  ant_value_t result = set_record_for_each_key(js, &other, set_symmetric_difference_key_cb, &build);
  return is_err(result) ? result : out;
}

static ant_value_t set_isSubsetOf(ant_params_t) {
  collection_table_t *this_set = get_set_from_obj(call_this);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.isSubsetOf");

  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "isSubsetOf", &other);

  if (is_err(rec)) return rec;
  if ((double)this_set->size > other.size) return Ant_Value_False;

  set_iterator_state_t it;
  collections_set_iter_open(&it, this_set, ITER_TYPE_SET_VALUES);
  ant_value_t result = Ant_Value_True;
  for (set_entry_t *entry; (entry = set_iter_take(&it));) {
    bool has = false;
    result = set_record_has_entry(js, &other, entry, &has);
    if (is_err(result)) break;
    result = Ant_Value_FromBool(has);
    if (!has) break;
  }
  collections_set_iter_close(&it);
  return result;
}

typedef struct {
  collection_table_t *this_set;
  bool result;
} set_predicate_ctx_t;

static set_key_status_t set_superset_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_predicate_ctx_t *pred = (set_predicate_ctx_t *)ctx;
  if (!set_lookup(js, pred->this_set, value)) {
    pred->result = false;
    return SET_KEY_STOP;
  }
  return SET_KEY_CONTINUE;
}

static ant_value_t set_isSupersetOf(ant_params_t) {
  collection_table_t *this_set = get_set_from_obj(call_this);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.isSupersetOf");

  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "isSupersetOf", &other);
  if (is_err(rec)) return rec;
  if ((double)this_set->size < other.size) return Ant_Value_False;

  set_predicate_ctx_t pred = { this_set, true };
  ant_value_t result = set_record_for_each_key(js, &other, set_superset_key_cb, &pred);
  if (is_err(result)) return result;
  return Ant_Value_FromBool(pred.result);
}

static set_key_status_t set_disjoint_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_predicate_ctx_t *pred = (set_predicate_ctx_t *)ctx;
  if (set_lookup(js, pred->this_set, value)) {
    pred->result = false;
    return SET_KEY_STOP;
  }
  return SET_KEY_CONTINUE;
}

static ant_value_t set_isDisjointFrom(ant_params_t) {
  collection_table_t *this_set = get_set_from_obj(call_this);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.isDisjointFrom");

  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "isDisjointFrom", &other);
  if (is_err(rec)) return rec;

  if ((double)this_set->size <= other.size) {
    set_iterator_state_t it;
    collections_set_iter_open(&it, this_set, ITER_TYPE_SET_VALUES);
    ant_value_t result = Ant_Value_True;
    for (set_entry_t *entry; (entry = set_iter_take(&it));) {
      bool has = false;
      result = set_record_has_entry(js, &other, entry, &has);
      if (is_err(result)) break;
      result = Ant_Value_FromBool(!has);
      if (has) break;
    }
    collections_set_iter_close(&it);
    return result;
  }

  set_predicate_ctx_t pred = { this_set, true };
  ant_value_t result = set_record_for_each_key(js, &other, set_disjoint_key_cb, &pred);
  if (is_err(result)) return result;

  return Ant_Value_FromBool(pred.result);
}

static ant_value_t weakmap_set(ant_params_t) {
  ant_object_t *object = NULL;
  weakmap_table_t *table = get_weakmap_from_obj(call_this, &object);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.set");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid value used as weak map key");

  if (!weakmap_store_entry(js, object, table, key, collection_arg(args, nargs, 1)))
    return Ant_Error_CreateOutOfMemory(js);
  
  return call_this;
}

static ant_value_t weakmap_get(ant_params_t) {
  weakmap_table_t *table = get_weakmap_from_obj(call_this, NULL);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.get");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key)) return Ant_Value_Undefined;
  
  weakmap_entry_t *entry = weakmap_table_find(table, key);
  return entry ? entry->value : Ant_Value_Undefined;
}

static ant_value_t weakmap_has(ant_params_t) {
  weakmap_table_t *table = get_weakmap_from_obj(call_this, NULL);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.has");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key)) return Ant_Value_False;
  
  return Ant_Value_FromBool(weakmap_table_find(table, key) != NULL);
}

static ant_value_t weakmap_upsert(ant_params_t) {
  ant_object_t *object = NULL;
  weakmap_table_t *table = get_weakmap_from_obj(call_this, &object);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.upsert");

  ant_value_t key_obj = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid value used as weak map key");

  ant_value_t update_fn = collection_arg(args, nargs, 1);
  ant_value_t insert_fn = collection_arg(args, nargs, 2);
  
  if (!is_callable(update_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap.upsert update callback must be callable");
  if (!is_callable(insert_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap.upsert insert callback must be callable");

  weakmap_entry_t *entry = weakmap_table_find(table, key_obj);

  ant_value_t value;
  if (entry) {
    ant_value_t call_args[3] = { entry->value, key_obj, call_this };
    value = sv_vm_call(js->vm, js, update_fn, Ant_Value_Undefined, call_args, 3, NULL, Ant_Value_Undefined);
  } else {
    ant_value_t call_args[2] = { key_obj, call_this };
    value = sv_vm_call(js->vm, js, insert_fn, Ant_Value_Undefined, call_args, 2, NULL, Ant_Value_Undefined);
  }

  if (is_err(value)) return value;

  if (!weakmap_store_entry(js, object, table, key_obj, value))
    return Ant_Error_CreateOutOfMemory(js);

  return value;
}

static ant_value_t weakmap_get_or_insert(ant_params_t) {
  ant_object_t *object = NULL;
  weakmap_table_t *table = get_weakmap_from_obj(call_this, &object);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.getOrInsert");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid value used as weak map key");

  weakmap_entry_t *entry = weakmap_table_find(table, key);
  if (entry) return entry->value;

  ant_value_t value = collection_arg(args, nargs, 1);
  if (!weakmap_store_entry(js, object, table, key, value)) return Ant_Error_CreateOutOfMemory(js);

  return value;
}

static ant_value_t weakmap_get_or_insert_computed(ant_params_t) {
  ant_object_t *object = NULL;
  weakmap_table_t *table = get_weakmap_from_obj(call_this, &object);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.getOrInsertComputed");

  ant_value_t key = collection_arg(args, nargs, 0);
  ant_value_t callback = collection_arg(args, nargs, 1);

  if (!can_be_held_weakly(key)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid value used as weak map key");
  if (!is_callable(callback)) return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap.prototype.getOrInsertComputed callback must be callable");

  weakmap_entry_t *entry = weakmap_table_find(table, key);
  if (entry) return entry->value;

  ant_value_t call_args[1] = { key };
  ant_value_t value = sv_vm_call(js->vm, js, callback, Ant_Value_Undefined, call_args, 1, NULL, Ant_Value_Undefined);
  if (is_err(value)) return value;

  table = get_weakmap_from_obj(call_this, &object);
  if (!table || !weakmap_store_entry(js, object, table, key, value)) return Ant_Error_CreateOutOfMemory(js);
  
  return value;
}

static ant_value_t weakmap_delete(ant_params_t) {
  weakmap_table_t *table = get_weakmap_from_obj(call_this, NULL);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.delete");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key)) return Ant_Value_False;
  
  return Ant_Value_FromBool(weakmap_table_delete(table, key));
}

static bool weakset_store_entry(ant_t *js, ant_value_t ws_obj, weakset_entry_t **ws_ptr, ant_value_t value_obj) {
  unsigned hashv;
  weakset_entry_t *entry;
  HASH_VALUE(&value_obj, sizeof(ant_value_t), hashv);
  HASH_FIND_BYHASHVALUE(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), hashv, entry);
  
  if (!entry) {
    entry = calloc(1, sizeof(weakset_entry_t));
    if (!entry) return false;
    entry->value_obj = value_obj;
    HASH_ADD_BYHASHVALUE(hh, *ws_ptr, value_obj, sizeof(ant_value_t), hashv, entry);
  }

  ant_object_t *object = js_obj_ptr(ws_obj);
  if (object) {
    gc_weak_remember_set(js, object, value_obj);
    gc_write_barrier(js, object, value_obj);
  }
  
  return true;
}

static ant_value_t weakset_add(ant_params_t) {
  weakset_entry_t **ws_ptr = get_weakset_from_obj(call_this);
  if (!ws_ptr) return collection_receiver_error(js, "WeakSet.prototype.add");

  ant_value_t value_obj = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(value_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid value used in weak set");
  
  if (!weakset_store_entry(js, call_this, ws_ptr, value_obj)) return Ant_Error_CreateOutOfMemory(js);
  return call_this;
}

static ant_value_t weakset_has(ant_params_t) {
  weakset_entry_t **ws_ptr = get_weakset_from_obj(call_this);
  if (!ws_ptr) return collection_receiver_error(js, "WeakSet.prototype.has");

  ant_value_t value_obj = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(value_obj)) return Ant_Value_False;
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), entry);
  return Ant_Value_FromBool(entry != NULL);
}

static ant_value_t weakset_delete(ant_params_t) {
  weakset_entry_t **ws_ptr = get_weakset_from_obj(call_this);
  if (!ws_ptr) return collection_receiver_error(js, "WeakSet.prototype.delete");

  ant_value_t value_obj = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(value_obj)) return Ant_Value_False;
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), entry);
  
  if (entry) {
    HASH_DEL(*ws_ptr, entry);
    free(entry);
    return Ant_Value_True;
  }
  return Ant_Value_False;
}

static void weakref_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  weakref_state_t *state = js_get_native(value, WEAKREF_NATIVE_TAG);
  free(state);
  js_clear_native(value, WEAKREF_NATIVE_TAG);
}

static ant_value_t weakref_attach(ant_t *js, ant_value_t wr_obj, ant_value_t target) {
  weakref_state_t *state = calloc(1, sizeof(*state));
  if (!state) return Ant_Error_CreateOutOfMemory(js);
  
  state->target = target;
  js_set_native(wr_obj, state, WEAKREF_NATIVE_TAG);
  js_set_finalizer(wr_obj, weakref_finalize);
  gc_weak_register(js, js_obj_ptr(wr_obj));
  
  return wr_obj;
}

ant_value_t js_weakref_create(ant_t *js, ant_value_t target) {
  ant_value_t wr_obj = Ant_Object_CreateBare(js);
  return is_err(wr_obj) ? wr_obj : weakref_attach(js, wr_obj, target);
}

bool js_weakref_target(ant_value_t ref, ant_value_t *target) {
  weakref_state_t *state = vtype(ref) == kTypeObject ? js_get_native(ref, WEAKREF_NATIVE_TAG) : NULL;
  if (state) *target = state->target;
  return state != NULL;
}

static ant_value_t builtin_WeakRef(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakRef constructor requires 'new'");

  if (nargs < 1 || !can_be_held_weakly(args[0]))
    return js_mkerr(js, "WeakRef target must be an object");
  
  ant_value_t wr_obj = call_this;
  if (vtype(wr_obj) != kTypeObject) wr_obj = Ant_Object_CreateBare(js);
  if (is_err(wr_obj)) return wr_obj;

  ant_value_t wr_proto = js->builtins.weakref_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, wr_proto, call_new_target);
  if (is_special_object(instance_proto)) js_set_proto_init(wr_obj, instance_proto);

  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin)
    js_set_slot(wr_obj, SLOT_CTOR, call_new_target);
  
  ant_value_t attached = weakref_attach(js, wr_obj, args[0]);
  if (!is_err(attached)) gc_weak_keep_alive(js, args[0]);
  
  return attached;
}

static ant_value_t weakref_deref(ant_params_t) {
  weakref_state_t *state = vtype(call_this) == kTypeObject
    ? js_get_native(call_this, WEAKREF_NATIVE_TAG) : NULL;
    
  if (!state) return js_mkerr_typed(
    js, JS_ERR_TYPE,
    "WeakRef.prototype.deref called on incompatible receiver"
  );

  ant_value_t target = state->target;
  if (vtype(target) != kTypeUndefined) gc_weak_keep_alive(js, target);
  
  return target;
}

enum {
  FINREG_ENTRY_TARGET,
  FINREG_ENTRY_HELD,
  FINREG_ENTRY_TOKEN,
  FINREG_ENTRY_FIELDS,
};

static ant_value_t finreg_weak_cell(ant_t *js, ant_value_t target) {
  ant_value_t cell = js_weakref_create(js, target);
  if (is_err(cell)) return cell;
  weakref_state_t *state = js_get_native(cell, WEAKREF_NATIVE_TAG);
  state->notifies_registry = true;
  return cell;
}

static ant_value_t finreg_cell_target(ant_value_t cell) {
  ant_value_t target = Ant_Value_Undefined;
  return js_weakref_target(cell, &target) ? target : Ant_Value_Undefined;
}

static ant_value_t builtin_FinalizationRegistry(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "FinalizationRegistry constructor requires 'new'");
  if (nargs < 1 || (vtype(args[0]) != kTypeFunction && vtype(args[0]) != kTypeBuiltin))
    return js_mkerr_typed(js, JS_ERR_TYPE, "FinalizationRegistry callback must be a function");
  
  GC_ROOT_SAVE(mark, js);
  ant_value_t fr_obj = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, fr_obj);
  ant_value_t fr_proto = js_instance_proto_from_new_target(js, js->builtins.finreg_proto, call_new_target);
  if (is_special_object(fr_proto)) js_set_proto_init(fr_obj, fr_proto);
  
  js_set_slot_wb(js, fr_obj, SLOT_MAP, Ant_Array_Create(js));
  js_set_slot_wb(js, fr_obj, SLOT_DATA, args[0]);
  
  ant_value_t registries = js_get_slot(js->builtins.finreg_proto, SLOT_DATA);
  ant_value_t cell = finreg_weak_cell(js, fr_obj);
  GC_ROOT_PIN(js, cell);
  if (!is_err(cell) && vtype(registries) == kTypeArray) js_arr_set_index(js, registries, js_arr_len(js, registries), cell);
  
  GC_ROOT_RESTORE(js, mark);
  return is_err(cell) ? cell : fr_obj;
}

static void finreg_truncate(ant_t *js, ant_value_t list, ant_offset_t kept, ant_offset_t len) {
  if (kept == len) return;
  for (ant_offset_t i = kept; i < len; i++) js_arr_set_index(js, list, i, Ant_Value_Undefined);
  js_arr_set_dense_length(js, list, kept);
}

static ant_value_t finreg_registrations(ant_value_t registry) {
  if (vtype(registry) != kTypeObject) return Ant_Value_Undefined;
  ant_value_t registrations = js_get_slot(registry, SLOT_MAP);
  return vtype(registrations) == kTypeArray ? registrations : Ant_Value_Undefined;
}

static ant_value_t finreg_register(ant_params_t) {
  ant_value_t registrations = finreg_registrations(call_this);
  if (vtype(registrations) != kTypeArray)
    return js_mkerr_typed(js, JS_ERR_TYPE, "FinalizationRegistry.prototype.register called on an incompatible receiver");
  
  if (nargs < 1 || !can_be_held_weakly(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "FinalizationRegistry.register target must be an object");
  
  ant_value_t target = args[0];
  ant_value_t held_value = nargs > 1 ? args[1] : Ant_Value_Undefined;
  ant_value_t unregister_token = nargs > 2 ? args[2] : Ant_Value_Undefined;
  
  if (target == held_value)
    return js_mkerr_typed(js, JS_ERR_TYPE, "target and held value must not be the same");
  
  if (vtype(unregister_token) != kTypeUndefined && !can_be_held_weakly(unregister_token))
    return js_mkerr_typed(js, JS_ERR_TYPE, "FinalizationRegistry.register token must be an object");
  
  GC_ROOT_SAVE(mark, js);
  ant_value_t fields[FINREG_ENTRY_FIELDS] = {Ant_Value_Undefined, held_value, Ant_Value_Undefined};
  GC_ROOT_PIN(js, fields[FINREG_ENTRY_TARGET]);
  GC_ROOT_PIN(js, fields[FINREG_ENTRY_TOKEN]);
  
  fields[FINREG_ENTRY_TARGET] = finreg_weak_cell(js, target);
  if (!is_err(fields[FINREG_ENTRY_TARGET]) && vtype(unregister_token) != kTypeUndefined)
    fields[FINREG_ENTRY_TOKEN] = unregister_token == target ? fields[FINREG_ENTRY_TARGET] : js_weakref_create(js, unregister_token);
  
  ant_value_t result = is_err(fields[FINREG_ENTRY_TARGET]) ? fields[FINREG_ENTRY_TARGET] : fields[FINREG_ENTRY_TOKEN];
  if (!is_err(result)) {
    ant_value_t entry = Ant_Array_CreateFromElements(js, fields, FINREG_ENTRY_FIELDS, FINREG_ENTRY_FIELDS, false);
    js_arr_set_index(js, registrations, js_arr_len(js, registrations), entry);
    result = Ant_Value_Undefined;
  }
  
  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t finreg_unregister(ant_params_t) {
  ant_value_t registrations = finreg_registrations(call_this);
  if (vtype(registrations) != kTypeArray)
    return js_mkerr_typed(js, JS_ERR_TYPE, "FinalizationRegistry.prototype.unregister called on an incompatible receiver");
  
  if (nargs < 1 || !can_be_held_weakly(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "FinalizationRegistry.unregister token must be an object");
  
  ant_value_t token = args[0];
  ant_offset_t len = js_arr_len(js, registrations);
  ant_offset_t kept = 0;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, registrations, i);
    if (finreg_cell_target(js_arr_get(js, entry, FINREG_ENTRY_TOKEN)) == token) continue;
    if (kept != i) js_arr_set_index(js, registrations, kept, entry);
    kept++;
  }
  
  finreg_truncate(js, registrations, kept, len);
  return Ant_Value_FromBool(kept != len);
}

static void finreg_collect(ant_t *js, ant_value_t registry, ant_value_t jobs) {
  ant_value_t registrations = finreg_registrations(registry);
  if (vtype(registrations) != kTypeArray) return;
  
  ant_value_t callback = js_get_slot(registry, SLOT_DATA);
  ant_offset_t len = js_arr_len(js, registrations);
  ant_offset_t kept = 0;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, registrations, i);
    if (vtype(finreg_cell_target(js_arr_get(js, entry, FINREG_ENTRY_TARGET))) != kTypeUndefined) {
      if (kept != i) js_arr_set_index(js, registrations, kept, entry);
      kept++;
      continue;
    }
    ant_offset_t at = js_arr_len(js, jobs);
    js_arr_set_index(js, jobs, at, callback);
    js_arr_set_index(js, jobs, at + 1, js_arr_get(js, entry, FINREG_ENTRY_HELD));
  }
  
  finreg_truncate(js, registrations, kept, len);
}

void js_finalization_run(ant_t *js) {
  js->weak_gc.finalization_pending = false;
  ant_value_t registries = js_get_slot(js->builtins.finreg_proto, SLOT_DATA);
  if (vtype(registries) != kTypeArray) return;
  
  GC_ROOT_SAVE(mark, js);
  ant_value_t jobs = Ant_Array_Create(js);
  GC_ROOT_PIN(js, jobs);
  
  ant_offset_t len = js_arr_len(js, registries);
  ant_offset_t kept = 0;
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t cell = js_arr_get(js, registries, i);
    ant_value_t registry = finreg_cell_target(cell);
    if (vtype(registry) == kTypeUndefined) continue;
    finreg_collect(js, registry, jobs);
    if (kept != i) js_arr_set_index(js, registries, kept, cell);
    kept++;
  }
  finreg_truncate(js, registries, kept, len);
  
  ant_offset_t count = js_arr_len(js, jobs);
  for (ant_offset_t i = 0; i + 1 < count; i += 2) {
    ant_value_t held = js_arr_get(js, jobs, i + 1);
    ant_value_t result = sv_call_value(js, js_arr_get(js, jobs, i), Ant_Value_Undefined, &held, 1);
    if (is_err(result)) process_report_uncaught_exception_if_pending(js);
  }
  
  GC_ROOT_RESTORE(js, mark);
  if (count > 0) process_microtasks(js);
}

static ant_value_t map_group_add(ant_t *js, void *ctx, ant_value_t key, ant_value_t value) {
  collection_table_t *map = ctx;
  collection_probe_t probe;
  if (!collection_probe_init(js, key, &probe)) return Ant_Error_CreateOutOfMemory(js);
  
  map_entry_t *entry = map_find(js, map, &probe);
  ant_value_t group;

  if (entry) group = entry->value; else {
    group = Ant_Array_Create(js);
    if (is_err(group)) return group;
    if (!map_append(js, map, &probe, group)) return Ant_Error_CreateOutOfMemory(js);
  }
  
  js_arr_push(js, group, value);
  return Ant_Value_Undefined;
}

static ant_value_t map_groupBy(ant_params_t) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Map.groupBy requires 2 arguments");
  
  collection_table_t *map = NULL;
  ant_value_t map_obj = collections_map_create(js, &map);
  if (is_err(map_obj)) return map_obj;
  
  ant_value_t grouped = js_group_by(js, args[0], args[1], map_group_add, map);
  return is_err(grouped) ? grouped : map_obj;
}

static bool is_original_collection_adder(ant_value_t adder, ant_cfunc_t fn) {
  return vtype(adder) == kTypeBuiltin && js_cfunc_same_entrypoint(adder, fn);
}

static ant_value_t map_init_from_iterable(ant_t *js, ant_value_t map_obj, collection_table_t *map, ant_value_t iterable) {
  ant_value_t adder = js_getprop_fallback(js, map_obj, "set");
  if (is_err(adder)) return adder;
  if (!is_callable(adder))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map constructor requires a callable set method");
  
  bool use_fast_path 
    = is_original_collection_adder(adder, map_set);

  iterator_t it;
  if (!js_iter_open(js, iterable, &it))
    return Ant_Exception_Pending(js)
      ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "Map constructor argument is not iterable");

  ant_value_t result = Ant_Value_Undefined;
  ant_value_t entry;
  
  while (js_iter_next(js, &it, &entry)) {
    uint8_t entry_t = vtype(entry);
    if (entry_t != kTypeArray && entry_t != kTypeObject) {
      result = js_mkerr_typed(js, JS_ERR_TYPE, "Map iterable entries must be pair sequences");
      goto close_iter;
    }
    
    ant_offset_t entry_len = js_arr_len(js, entry);
    if (entry_len < 2) {
      result = js_mkerr_typed(js, JS_ERR_TYPE, "Map iterable entries must have at least 2 items");
      goto close_iter;
    }
    
    if (use_fast_path) {
      if (!map_store(js, map, js_arr_get(js, entry, 0), js_arr_get(js, entry, 1))) {
        result = Ant_Error_CreateOutOfMemory(js);
        goto close_iter;
      }
      continue;
    }
    
    ant_value_t call_args[2] = { js_arr_get(js, entry, 0), js_arr_get(js, entry, 1) };
    result = sv_vm_call(js->vm, js, adder, map_obj, call_args, 2, NULL, Ant_Value_Undefined);
    if (is_err(result)) goto close_iter;
  }

  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : result;

close_iter:
  js_iter_close(js, &it);
  return result;
}

static ant_value_t set_init_from_iterable(ant_t *js, ant_value_t set_obj, collection_table_t *set, ant_value_t iterable) {
  ant_value_t adder = js_getprop_fallback(js, set_obj, "add");
  if (is_err(adder)) return adder;

  if (!is_callable(adder))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set constructor requires a callable add method");
  
  bool use_fast_path  = is_original_collection_adder(adder, set_add);

  iterator_t it;
  if (!js_iter_open(js, iterable, &it))
    return Ant_Exception_Pending(js)
      ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "Set constructor argument is not iterable");

  ant_value_t result = Ant_Value_Undefined;
  ant_value_t value;
  
  while (js_iter_next(js, &it, &value)) {
    if (use_fast_path) {
      if (!set_store(js, set, value)) {
        result = Ant_Error_CreateOutOfMemory(js);
        goto close_iter;
      }
      continue;
    }
    
    result = sv_vm_call(js->vm, js, adder, set_obj, &value, 1, NULL, Ant_Value_Undefined);
    if (is_err(result)) goto close_iter;
  }

  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : result;

close_iter:
  js_iter_close(js, &it);
  return result;
}

static ant_value_t weakmap_init_from_iterable(
  ant_t *js, ant_value_t wm_obj,
  weakmap_table_t *table, ant_value_t iterable
) {
  ant_object_t *wm_object = js_obj_ptr(wm_obj);
  ant_value_t adder = js_getprop_fallback(js, wm_obj, "set");
  if (is_err(adder)) return adder;
  if (!is_callable(adder))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap constructor requires a callable set method");
  
  bool use_fast_path = is_original_collection_adder(adder, weakmap_set);

  iterator_t it;
  if (!js_iter_open(js, iterable, &it))
    return Ant_Exception_Pending(js)
      ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap constructor argument is not iterable");

  ant_value_t result = Ant_Value_Undefined;
  ant_value_t entry;
  
  while (js_iter_next(js, &it, &entry)) {
    uint8_t entry_t = vtype(entry);
    if (entry_t != kTypeArray && entry_t != kTypeObject) {
      result = js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap iterable entries must be pair sequences");
      goto close_iter;
    }
    
    ant_offset_t entry_len = js_arr_len(js, entry);
    if (entry_len < 2) {
      result = js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap iterable entries must have at least 2 items");
      goto close_iter;
    }
    
    if (!use_fast_path) {
      ant_value_t call_args[2] = { js_arr_get(js, entry, 0), js_arr_get(js, entry, 1) };
      result = sv_vm_call(js->vm, js, adder, wm_obj, call_args, 2, NULL, Ant_Value_Undefined);
      if (is_err(result)) goto close_iter;
      continue;
    }

    ant_value_t key = js_arr_get(js, entry, 0);
    ant_value_t value = js_arr_get(js, entry, 1);
    if (!can_be_held_weakly(key)) {
      result = js_mkerr(js, "WeakMap key must be an object");
      goto close_iter;
    }

    if (!weakmap_store_entry(js, wm_object, table, key, value)) {
      result = Ant_Error_CreateOutOfMemory(js);
      goto close_iter;
    }
  }

  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : result;

close_iter:
  js_iter_close(js, &it);
  return result;
}

static ant_value_t weakset_init_from_iterable(ant_t *js, ant_value_t ws_obj, weakset_entry_t **ws_head, ant_value_t iterable) {
  ant_value_t adder = js_getprop_fallback(js, ws_obj, "add");
  if (is_err(adder)) return adder;
  if (!is_callable(adder))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakSet constructor requires a callable add method");
  
  bool use_fast_path = is_original_collection_adder(adder, weakset_add);

  iterator_t it;
  if (!js_iter_open(js, iterable, &it))
    return Ant_Exception_Pending(js)
      ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "WeakSet constructor argument is not iterable");

  ant_value_t result = Ant_Value_Undefined;
  ant_value_t value;
  
  while (js_iter_next(js, &it, &value)) {
    if (!use_fast_path) {
      result = sv_vm_call(js->vm, js, adder, ws_obj, &value, 1, NULL, Ant_Value_Undefined);
      if (is_err(result)) goto close_iter;
      continue;
    }
    
    if (!can_be_held_weakly(value)) {
      result = js_mkerr(js, "WeakSet value must be an object");
      goto close_iter;
    }
    
    if (!weakset_store_entry(js, ws_obj, ws_head, value)) {
      result = Ant_Error_CreateOutOfMemory(js);
      goto close_iter;
    }
  }

  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : result;

close_iter:
  js_iter_close(js, &it);
  return result;
}

static ant_value_t builtin_Map(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map constructor requires 'new'");
  }
  
  ant_value_t map_obj = call_this;
  if (vtype(map_obj) != kTypeObject) map_obj = Ant_Object_CreateBare(js);
  if (is_err(map_obj)) return map_obj;
  js_obj_ptr(map_obj)->type_tag = kTypeMap;
  
  ant_value_t map_proto = js->builtins.map_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, map_proto, call_new_target);
  
  if (is_special_object(instance_proto)) js_set_proto_init(map_obj, instance_proto);
  
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin)
    js_set_slot(map_obj, SLOT_CTOR, call_new_target);
  
  if (nargs == 0 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) {
    collections_table_attach_empty(map_obj, kTypeMap);
    return map_obj;
  }

  collection_table_t *map = collections_table_attach(js, map_obj, kTypeMap);
  if (!map) return Ant_Error_CreateOutOfMemory(js);
  
  ant_value_t init_result = map_init_from_iterable(js, map_obj, map, args[0]);
  if (is_err(init_result)) return init_result;
  
  return map_obj;
}

static ant_value_t builtin_Set(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set constructor requires 'new'");
  }
  
  ant_value_t set_obj = call_this;
  if (vtype(set_obj) != kTypeObject) set_obj = Ant_Object_CreateBare(js);
  if (is_err(set_obj)) return set_obj;
  js_obj_ptr(set_obj)->type_tag = kTypeSet;
  
  ant_value_t set_proto = js->builtins.set_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, set_proto, call_new_target);

  if (is_special_object(instance_proto)) js_set_proto_init(set_obj, instance_proto);
  
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin)
    js_set_slot(set_obj, SLOT_CTOR, call_new_target);
  
  if (nargs == 0 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) {
    collections_table_attach_empty(set_obj, kTypeSet);
    return set_obj;
  }

  collection_table_t *set = collections_table_attach(js, set_obj, kTypeSet);
  if (!set) return Ant_Error_CreateOutOfMemory(js);
  
  ant_value_t init_result = set_init_from_iterable(js, set_obj, set, args[0]);
  if (is_err(init_result)) return init_result;
  
  return set_obj;
}

static ant_value_t builtin_WeakMap(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap constructor requires 'new'");
  }
  
  ant_value_t wm_obj = call_this;
  if (vtype(wm_obj) != kTypeObject) wm_obj = Ant_Object_CreateBare(js);
  if (is_err(wm_obj)) return wm_obj;
  js_obj_ptr(wm_obj)->type_tag = kTypeWeakMap;
  
  ant_value_t wm_proto = js->builtins.weakmap_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, wm_proto, call_new_target);

  if (is_special_object(instance_proto)) js_set_proto_init(wm_obj, instance_proto);
  
  weakmap_table_t *table = calloc(1, sizeof(*table));
  if (!table) return Ant_Error_CreateOutOfMemory(js);
  
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin)
    js_set_slot(wm_obj, SLOT_CTOR, call_new_target);
  js_set_native(wm_obj, table, WEAKMAP_NATIVE_TAG);
  gc_weak_register(js, js_obj_ptr(wm_obj));
  
  if (nargs == 0 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) return wm_obj;
  ant_value_t init_result = weakmap_init_from_iterable(
    js, wm_obj, table, args[0]
  );
  if (is_err(init_result)) return init_result;
  
  return wm_obj;
}

static ant_value_t builtin_WeakSet(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakSet constructor requires 'new'");
  }
  
  ant_value_t ws_obj = call_this;
  if (vtype(ws_obj) != kTypeObject) ws_obj = Ant_Object_CreateBare(js);
  if (is_err(ws_obj)) return ws_obj;
  js_obj_ptr(ws_obj)->type_tag = kTypeWeakSet;
  
  ant_value_t ws_proto = js->builtins.weakset_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, ws_proto, call_new_target);

  if (is_special_object(instance_proto)) js_set_proto_init(ws_obj, instance_proto);
  
  weakset_entry_t **ws_head = calloc(1, sizeof(weakset_entry_t *));
  if (!ws_head) return Ant_Error_CreateOutOfMemory(js);
  *ws_head = NULL;
  
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin)
    js_set_slot(ws_obj, SLOT_CTOR, call_new_target);
  js_set_native(ws_obj, ws_head, WEAKSET_NATIVE_TAG);
  gc_weak_register(js, js_obj_ptr(ws_obj));
  
  if (nargs == 0 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) return ws_obj;
  ant_value_t init_result = weakset_init_from_iterable(js, ws_obj, ws_head, args[0]);
  if (is_err(init_result)) return init_result;
  
  return ws_obj;
}

void init_collections_module(ant_t *js) {
  ant_value_t object_proto = js->sym.object_proto;  
  ant_value_t iter_sym = js->sym.iterator_sym;
  
  js->builtins.map_iter_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.map_iter_proto, js->sym.iterator_proto);
  defmethod(js, js->builtins.map_iter_proto, "next", 4, Ant_Function_Create(map_iter_next));
  js_set_tostringtag(js, js->builtins.map_iter_proto, js_mkstr(js, "Map Iterator", 12));
  js_iter_register_advance(js, js->builtins.map_iter_proto, advance_map);
  ant_iterator_protector_holder_mark(js->builtins.map_iter_proto);
  
  js->builtins.set_iter_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.set_iter_proto, js->sym.iterator_proto);
  defmethod(js, js->builtins.set_iter_proto, "next", 4, Ant_Function_Create(set_iter_next));
  js_set_tostringtag(js, js->builtins.set_iter_proto, js_mkstr(js, "Set Iterator", 12));
  js_iter_register_advance(js, js->builtins.set_iter_proto, advance_set);
  ant_iterator_protector_holder_mark(js->builtins.set_iter_proto);
  
  ant_value_t map_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(map_proto, object_proto);
  js->builtins.map_proto = map_proto;
  defmethod(js, map_proto, "set", 3, Ant_Function_CreateWithArity(map_set, 2));
  defmethod(js, map_proto, "get", 3, Ant_Function_CreateWithArity(map_get, 1));
  defmethod(js, map_proto, "has", 3, Ant_Function_CreateWithArity(map_has, 1));
  defmethod(js, map_proto, "upsert", 6, Ant_Function_CreateWithArity(map_upsert, 3));
  defmethod(js, map_proto, "getOrInsert", 11, Ant_Function_CreateWithArity(map_get_or_insert, 2));
  defmethod(js, map_proto, "getOrInsertComputed", 19, Ant_Function_CreateWithArity(map_get_or_insert_computed, 2));
  defmethod(js, map_proto, "delete", 6, Ant_Function_CreateWithArity(map_delete, 1));
  defmethod(js, map_proto, "clear", 5, Ant_Function_Create(map_clear));
  js_set_getter_desc(js, map_proto, "size", 4, Ant_Function_Create(map_size), JS_DESC_C);
  
  defmethod(js, map_proto, "entries", 7, Ant_Function_Create(map_entries));
  defmethod(js, map_proto, "keys", 4, Ant_Function_Create(map_keys));
  defmethod(js, map_proto, "values", 6, Ant_Function_Create(map_values));
  defmethod(js, map_proto, "forEach", 7, Ant_Function_CreateWithArity(map_forEach, 1));
  defalias_sym_of(js, map_proto, iter_sym, "entries");
  js_set_tostringtag(js, map_proto, js_mkstr(js, "Map", 3));
  
  ant_value_t map_ctor = Ant_Function_CreateIntrinsicConstructor(js, builtin_Map, map_proto, "Map", 0);
  defmethod(js, map_ctor, "groupBy", 7, Ant_Function_CreateWithArity(map_groupBy, 2));
  js_define_species_getter(js, map_ctor);
  js_set_global_builtin(js, "Map", map_ctor);
  
  ant_value_t set_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(set_proto, object_proto);
  js->builtins.set_proto = set_proto;
  defmethod(js, set_proto, "add", 3, Ant_Function_CreateWithArity(set_add, 1));
  defmethod(js, set_proto, "has", 3, Ant_Function_CreateWithArity(set_has, 1));
  defmethod(js, set_proto, "delete", 6, Ant_Function_CreateWithArity(set_delete, 1));
  defmethod(js, set_proto, "clear", 5, Ant_Function_Create(set_clear));
  js_set_getter_desc(js, set_proto, "size", 4, Ant_Function_Create(set_size), JS_DESC_C);
  
  defmethod(js, set_proto, "values", 6, Ant_Function_Create(set_values));
  defalias_of(js, set_proto, "keys", "values");
  defmethod(js, set_proto, "entries", 7, Ant_Function_Create(set_entries));
  defmethod(js, set_proto, "forEach", 7, Ant_Function_CreateWithArity(set_forEach, 1));
  defmethod(js, set_proto, "union", 5, Ant_Function_CreateWithArity(set_union, 1));
  defmethod(js, set_proto, "intersection", 12, Ant_Function_CreateWithArity(set_intersection, 1));
  defmethod(js, set_proto, "difference", 10, Ant_Function_CreateWithArity(set_difference, 1));
  defmethod(js, set_proto, "symmetricDifference", 19, Ant_Function_CreateWithArity(set_symmetricDifference, 1));
  defmethod(js, set_proto, "isSubsetOf", 10, Ant_Function_CreateWithArity(set_isSubsetOf, 1));
  defmethod(js, set_proto, "isSupersetOf", 12, Ant_Function_CreateWithArity(set_isSupersetOf, 1));
  defmethod(js, set_proto, "isDisjointFrom", 14, Ant_Function_CreateWithArity(set_isDisjointFrom, 1));
  defalias_sym_of(js, set_proto, iter_sym, "values");
  js_set_tostringtag(js, set_proto, js_mkstr(js, "Set", 3));
  
  ant_value_t set_ctor = Ant_Function_CreateIntrinsicConstructor(js, builtin_Set, set_proto, "Set", 0);
  js_define_species_getter(js, set_ctor);
  js_set_global_builtin(js, "Set", set_ctor);
  
  ant_value_t weakmap_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(weakmap_proto, object_proto);
  js->builtins.weakmap_proto = weakmap_proto;
  defmethod(js, weakmap_proto, "set", 3, Ant_Function_CreateWithArity(weakmap_set, 2));
  defmethod(js, weakmap_proto, "get", 3, Ant_Function_CreateWithArity(weakmap_get, 1));
  defmethod(js, weakmap_proto, "has", 3, Ant_Function_CreateWithArity(weakmap_has, 1));
  defmethod(js, weakmap_proto, "upsert", 6, Ant_Function_CreateWithArity(weakmap_upsert, 3));
  defmethod(js, weakmap_proto, "getOrInsert", 11, Ant_Function_CreateWithArity(weakmap_get_or_insert, 2));
  defmethod(js, weakmap_proto, "getOrInsertComputed", 19, Ant_Function_CreateWithArity(weakmap_get_or_insert_computed, 2));
  defmethod(js, weakmap_proto, "delete", 6, Ant_Function_CreateWithArity(weakmap_delete, 1));
  js_set_tostringtag(js, weakmap_proto, js_mkstr(js, "WeakMap", 7));
  
  ant_value_t weakmap_ctor = Ant_Function_CreateIntrinsicConstructor(js, builtin_WeakMap, weakmap_proto, "WeakMap", 0);
  js_set_global_builtin(js, "WeakMap", weakmap_ctor);
  
  ant_value_t weakset_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(weakset_proto, object_proto);
  js->builtins.weakset_proto = weakset_proto;
  defmethod(js, weakset_proto, "add", 3, Ant_Function_CreateWithArity(weakset_add, 1));
  defmethod(js, weakset_proto, "has", 3, Ant_Function_CreateWithArity(weakset_has, 1));
  defmethod(js, weakset_proto, "delete", 6, Ant_Function_CreateWithArity(weakset_delete, 1));
  js_set_tostringtag(js, weakset_proto, js_mkstr(js, "WeakSet", 7));
  
  ant_value_t weakset_ctor = Ant_Function_CreateIntrinsicConstructor(js, builtin_WeakSet, weakset_proto, "WeakSet", 0);
  js_set_global_builtin(js, "WeakSet", weakset_ctor);
  
  ant_value_t weakref_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(weakref_proto, object_proto);
  js->builtins.weakref_proto = weakref_proto;
  defmethod(js, weakref_proto, "deref", 5, Ant_Function_Create(weakref_deref));
  js_set_tostringtag(js, weakref_proto, js_mkstr(js, "WeakRef", 7));
  
  ant_value_t weakref_ctor = Ant_Function_CreateIntrinsicConstructor(js, builtin_WeakRef, weakref_proto, "WeakRef", 1);
  js_set_global_builtin(js, "WeakRef", weakref_ctor);
  
  ant_value_t finreg_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(finreg_proto, object_proto);
  js->builtins.finreg_proto = finreg_proto;
  js_set_slot_wb(js, finreg_proto, SLOT_DATA, Ant_Array_Create(js));
  defmethod(js, finreg_proto, "register", 8, Ant_Function_CreateWithArity(finreg_register, 2));
  defmethod(js, finreg_proto, "unregister", 10, Ant_Function_CreateWithArity(finreg_unregister, 1));
  js_set_tostringtag(js, finreg_proto, js_mkstr(js, "FinalizationRegistry", 20));
  
  ant_value_t finreg_ctor = Ant_Function_CreateIntrinsicConstructor(js, builtin_FinalizationRegistry, finreg_proto, "FinalizationRegistry", 1);
  js_set_global_builtin(js, "FinalizationRegistry", finreg_ctor);
}
