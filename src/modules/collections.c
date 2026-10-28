#include <stddef.h>

static void *collection_alloc(size_t size);
static void collection_release(void *block, size_t size);

#define uthash_malloc(size) collection_alloc(size)
#define uthash_free(block, size) collection_release(block, size)

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

#include "modules/bigint.h"
#include "modules/collections.h"
#include "modules/symbol.h"
#include "modules/iterator.h"

#undef HASH_INITIAL_NUM_BUCKETS
#undef HASH_INITIAL_NUM_BUCKETS_LOG2
#undef HASH_BKT_CAPACITY_THRESH

#define HASH_INITIAL_NUM_BUCKETS 8U
#define HASH_INITIAL_NUM_BUCKETS_LOG2 3U
#define HASH_BKT_CAPACITY_THRESH 6U

enum {
  COLLECTION_POOL_GRAIN = 16,
  COLLECTION_POOL_CLASSES = 32,
  COLLECTION_POOL_CAP = 128,
};

typedef struct collection_block {
  struct collection_block *next;
} collection_block_t;

typedef struct {
  collection_block_t *head[COLLECTION_POOL_CLASSES];
  uint32_t count[COLLECTION_POOL_CLASSES];
} collection_pool_t;

static _Thread_local collection_pool_t collection_pool;

static inline size_t collection_pool_class(size_t size) {
  return size == 0 ? COLLECTION_POOL_CLASSES : (size - 1) / COLLECTION_POOL_GRAIN;
}

static void *collection_alloc(size_t size) {
  size_t cls = collection_pool_class(size);
  if (cls >= COLLECTION_POOL_CLASSES) return malloc(size);
  collection_pool_t *pool = &collection_pool;
  collection_block_t *block = pool->head[cls];
  if (!block) return malloc((cls + 1) * COLLECTION_POOL_GRAIN);
  pool->head[cls] = block->next;
  pool->count[cls]--;
  return block;
}

static void *collection_zalloc(size_t size) {
  void *block = collection_alloc(size);
  if (block) memset(block, 0, size);
  return block;
}

static void collection_release(void *block, size_t size) {
  if (!block) return;
  size_t cls = collection_pool_class(size);
  collection_pool_t *pool = &collection_pool;
  if (cls >= COLLECTION_POOL_CLASSES || pool->count[cls] >= COLLECTION_POOL_CAP) {
    free(block);
    return;
  }
  ((collection_block_t *)block)->next = pool->head[cls];
  pool->head[cls] = block;
  pool->count[cls]++;
}

static bool can_be_held_weakly(ant_value_t value) {
  if (is_object_type(value) || vtype(value) == kTypeBuiltin) return true;
  return vtype(value) == kTypeSymbol && js_sym_key(value) == NULL;
}

typedef struct {
  unsigned char stack[32];
  unsigned char *bytes;
  size_t len;
} collection_key_t;

static ant_value_t normalize_map_key(ant_value_t key) {
  if (vtype(key) == kTypeNumber) {
    double d = tod(key);
    if (d == 0.0 && signbit(d)) return js_mknum(0.0);
  }
  return key;
}

static void collection_key_reset(collection_key_t *key) {
  key->bytes = key->stack;
  key->len = 0;
}

static void collection_key_free(collection_key_t *key) {
  if (key->bytes != key->stack) free(key->bytes);
  collection_key_reset(key);
}

static bool collection_key_reserve(collection_key_t *key, size_t len) {
  if (len <= sizeof(key->stack)) return true;
  unsigned char *heap = malloc(len);
  if (!heap) return false;
  key->bytes = heap;
  return true;
}

static bool collection_key_init(ant_t *js, ant_value_t input, collection_key_t *out) {
  collection_key_reset(out);

  ant_value_t key = normalize_map_key(input);
  uint8_t tag = (uint8_t)vtype(key);

  if (vtype(key) == kTypeString) {
    size_t str_len = 0;
    const char *str = js_getstr(js, key, &str_len);
    out->len = 1 + str_len;
    if (!collection_key_reserve(out, out->len)) return false;
    out->bytes[0] = tag;
    if (str_len > 0) memcpy(out->bytes + 1, str, str_len);
    return true;
  }

  if (vtype(key) == kTypeBigInt) {
    size_t str_len = bigint_digits_len(js, key) + (bigint_is_negative(js, key) ? 1 : 0);
    out->len = 1 + str_len;
    if (!collection_key_reserve(out, out->len)) return false;
    out->bytes[0] = tag;
    if (str_len > 0) strbigint(js, key, (char *)(out->bytes + 1), str_len + 1);
    return true;
  }

  out->len = 1 + sizeof(ant_value_t);
  if (!collection_key_reserve(out, out->len)) return false;
  out->bytes[0] = tag;
  memcpy(out->bytes + 1, &key, sizeof(ant_value_t));

  return true;
}

static inline unsigned collection_key_hash(const collection_key_t *key) {
  return (unsigned)hash_key((const char *)key->bytes, key->len);
}

map_entry_t *Ant_Map_Find(ant_t *js, map_entry_t **map_ptr, ant_value_t key_val) {
  collection_key_t key;
  if (!collection_key_init(js, key_val, &key)) return NULL;

  map_entry_t *entry = NULL;
  unsigned key_hash = collection_key_hash(&key);
  
  HASH_FIND_BYHASHVALUE(hh, *map_ptr, key.bytes, key.len, key_hash, entry);
  collection_key_free(&key);
  
  return entry;
}

set_entry_t *Ant_Set_Find(ant_t *js, set_entry_t **set_ptr, ant_value_t value) {
  collection_key_t key;
  if (!collection_key_init(js, value, &key)) return NULL;
  
  set_entry_t *entry = NULL;
  unsigned key_hash = collection_key_hash(&key);
  
  HASH_FIND_BYHASHVALUE(hh, *set_ptr, key.bytes, key.len, key_hash, entry);
  collection_key_free(&key);
  
  return entry;
}

static map_store_t *map_store_of(map_entry_t **head) {
  return (map_store_t *)head;
}

static set_store_t *set_store_of(set_entry_t **head) {
  return (set_store_t *)head;
}

map_entry_t **collections_map_store_new(void) {
  map_store_t *store = collection_zalloc(sizeof(*store));
  if (!store) return NULL;
  store->cursors.flag_offset = offsetof(map_entry_t, cursored);
  return &store->head;
}

set_entry_t **collections_set_store_new(void) {
  set_store_t *store = collection_zalloc(sizeof(*store));
  if (!store) return NULL;
  store->cursors.flag_offset = offsetof(set_entry_t, cursored);
  return &store->head;
}

static collection_cursor_t *cursor_new(collection_cursors_t *cursors) {
  collection_cursor_t *cursor = collection_alloc(sizeof(*cursor));
  if (cursor) *cursor = (collection_cursor_t){ .owner = cursors };
  return cursor;
}

static uint32_t *cursor_flag(collection_cursors_t *cursors, void *entry) {
  return (uint32_t *)((char *)entry + cursors->flag_offset);
}

static void cursor_link(collection_cursors_t *cursors, collection_cursor_t *cursor) {
  cursor->prev = NULL;
  cursor->next = cursors->head;
  if (cursors->head) cursors->head->prev = cursor;
  cursors->head = cursor;
}

static void cursor_detach(collection_cursor_t *cursor) {
  collection_cursors_t *cursors = cursor->owner;
  if (cursors && cursors->end == cursor) cursors->end = NULL;
  else if (cursors && cursor->entry) {
    if (cursor->prev) cursor->prev->next = cursor->next;
    else cursors->head = cursor->next;
    if (cursor->next) cursor->next->prev = cursor->prev;
    *cursor_flag(cursors, cursor->entry) = 0;
  }
  cursor->entry = NULL;
  cursor->prev = cursor->next = NULL;
}

static void cursor_park(collection_cursors_t *cursors, collection_cursor_t *cursor, void *entry) {
  cursor->owner = cursors;
  if (!entry) {
    cursors->end = cursor;
    return;
  }
  cursor->entry = entry;
  *cursor_flag(cursors, entry) = 1;
  cursor_link(cursors, cursor);
}

static void cursor_forward(collection_cursor_t *cursor, collection_cursor_t *to) {
  cursor->forward = to;
  cursor->owner = NULL;
  to->refs++;
}

static collection_cursor_t *cursor_find(collection_cursors_t *cursors, const void *entry) {
  collection_cursor_t *cursor = cursors->head;
  while (cursor && cursor->entry != entry) cursor = cursor->next;
  return cursor;
}

static collection_cursor_t *cursor_at(collection_cursors_t *cursors, void *entry) {
  if (!entry) return cursors->end;
  return *cursor_flag(cursors, entry) ? cursor_find(cursors, entry) : NULL;
}

static void cursor_idle(collection_cursor_t *cursor) {
  collection_iter_state_t *state = (collection_iter_state_t *)((char *)cursor - offsetof(collection_iter_state_t, own));
  if (!state->live) collection_release(state, sizeof(*state));
}

static void cursor_release(collection_cursor_t *cursor) {
  while (cursor && --cursor->refs == 0) {
    collection_cursor_t *forward = cursor->forward;
    if (!forward) cursor_detach(cursor);
    cursor->forward = NULL;
    cursor->owner = NULL;
    if (cursor->embedded) cursor_idle(cursor);
    else collection_release(cursor, sizeof(*cursor));
    cursor = forward;
  }
}

static collection_cursor_t *cursor_resolve(collection_cursor_t **slot) {
  collection_cursor_t *cursor = *slot;
  while (cursor && cursor->forward) {
    collection_cursor_t *to = cursor->forward;
    to->refs++;
    cursor_release(cursor);
    cursor = to;
  }
  return *slot = cursor;
}

static void cursor_place(collection_cursors_t *cursors, collection_cursor_t *cursor, void *entry) {
  collection_cursor_t *there = cursor_at(cursors, entry);
  if (there) cursor_forward(cursor, there);
  else cursor_park(cursors, cursor, entry);
}

static collection_cursor_t *cursor_join(collection_cursors_t *cursors, void *entry) {
  collection_cursor_t *cursor = cursor_at(cursors, entry);
  if (!cursor) {
    if (!(cursor = cursor_new(cursors))) return NULL;
    cursor_park(cursors, cursor, entry);
  }
  cursor->refs++;
  return cursor;
}

static collection_cursor_t *cursor_open(collection_cursors_t *cursors, void *entry, collection_cursor_t *own) {
  if (!own || cursor_at(cursors, entry)) return cursor_join(cursors, entry);
  *own = (collection_cursor_t){ .refs = 1, .embedded = true };
  cursor_park(cursors, own, entry);
  return own;
}

static void cursors_resume(collection_cursors_t *cursors, void *entry) {
  collection_cursor_t *cursor = cursors->end;
  cursors->end = NULL;
  cursor_park(cursors, cursor, entry);
}

static void cursors_rewind(collection_cursors_t *cursors) {
  for (collection_cursor_t *cursor = cursors->head, *next; cursor; cursor = next) {
    next = cursor->next;
    cursor_detach(cursor);
    if (cursors->end) cursor_forward(cursor, cursors->end);
    else cursors->end = cursor;
  }
  cursors->head = NULL;
}

static void cursor_orphan(collection_cursor_t *cursor) {
  *cursor = (collection_cursor_t){ .refs = cursor->refs, .embedded = cursor->embedded };
}

static void cursors_orphan(collection_cursors_t *cursors) {
  for (collection_cursor_t *cursor = cursors->head, *next; cursor; cursor = next) {
    next = cursor->next;
    cursor_orphan(cursor);
  }
  if (cursors->end) cursor_orphan(cursors->end);
  cursors->head = cursors->end = NULL;
}

static void iter_state_open(collection_iter_state_t *st, collection_cursors_t *cursors, void *first, iter_type_t type, bool owned) {
  st->type = type;
  st->live = owned;
  st->own = (collection_cursor_t){ .embedded = true };
  st->at = cursors ? cursor_open(cursors, first, owned ? &st->own : NULL) : NULL;
}

static void iter_state_dispose(collection_iter_state_t *st) {
  collection_cursor_t *at = st->at;
  st->at = NULL;
  cursor_release(at);
  st->live = false;
  if (st->own.refs == 0) collection_release(st, sizeof(*st));
}

void collections_map_iter_open(map_iterator_state_t *st, map_entry_t **head, iter_type_t type) {
  iter_state_open(st, head ? &map_store_of(head)->cursors : NULL, head ? *head : NULL, type, false);
}

void collections_set_iter_open(set_iterator_state_t *st, set_entry_t **head, iter_type_t type) {
  iter_state_open(st, head ? &set_store_of(head)->cursors : NULL, head ? *head : NULL, type, false);
}

void collections_map_iter_close(map_iterator_state_t *st) {
  cursor_release(st->at);
  st->at = NULL;
}

void collections_set_iter_close(set_iterator_state_t *st) {
  cursor_release(st->at);
  st->at = NULL;
}

static bool iter_state_finish(collection_iter_state_t *st, collection_cursor_t *at) {
  if (at->entry || at->forward || at->refs != 1) return false;
  collection_cursors_t *cursors = at->owner;
  if (cursors && cursors->end == at) cursors->end = NULL;
  st->at = NULL;
  at->refs = 0;
  at->owner = NULL;
  if (at->embedded) cursor_idle(at);
  else collection_release(at, sizeof(*at));
  return true;
}

static bool iter_state_last(collection_cursor_t *at, uint32_t *flag) {
  collection_cursors_t *cursors = at->owner;
  if (at->forward || at->refs != 1 || cursors->end) return false;
  if (at->prev) at->prev->next = at->next;
  else cursors->head = at->next;
  if (at->next) at->next->prev = at->prev;
  at->prev = at->next = NULL;
  at->entry = NULL;
  *flag = 0;
  cursors->end = at;
  return true;
}

static void iter_state_step(collection_iter_state_t *st, collection_cursor_t *at, void *next) {
  if (at->refs == 1) {
    collection_cursors_t *cursors = at->owner;
    cursor_detach(at);
    cursor_place(cursors, at, next);
  } else {
    at->refs--;
    st->at = cursor_join(at->owner, next);
  }
}

map_entry_t *collections_map_iter_step(map_iterator_state_t *st) {
  collection_cursor_t *at = st->at;
  if (!at || iter_state_finish(st, at)) return NULL;
  map_entry_t *last = (map_entry_t *)at->entry;
  if (last && !last->hh.next && iter_state_last(at, &last->cursored)) return last;

  at = cursor_resolve(&st->at);
  map_entry_t *entry = at ? (map_entry_t *)at->entry : NULL;
  if (!entry) {
    collections_map_iter_close(st);
    return NULL;
  }
  iter_state_step(st, at, entry->hh.next);
  return entry;
}

set_entry_t *collections_set_iter_step(set_iterator_state_t *st) {
  collection_cursor_t *at = st->at;
  if (!at || iter_state_finish(st, at)) return NULL;
  set_entry_t *last = (set_entry_t *)at->entry;
  if (last && !last->hh.next && iter_state_last(at, &last->cursored)) return last;

  at = cursor_resolve(&st->at);
  set_entry_t *entry = at ? (set_entry_t *)at->entry : NULL;
  if (!entry) {
    collections_set_iter_close(st);
    return NULL;
  }
  iter_state_step(st, at, entry->hh.next);
  return entry;
}

void collections_map_remove(map_entry_t **head, map_entry_t *entry) {
  if (entry->cursored) {
    map_store_t *store = map_store_of(head);
    collection_cursor_t *cursor = cursor_find(&store->cursors, entry);
    map_entry_t *next = (map_entry_t *)entry->hh.next;
    if (next && !next->cursored) {
      cursor->entry = next;
      next->cursored = 1;
    } else {
      cursor_detach(cursor);
      cursor_place(&store->cursors, cursor, next);
    }
  }
  HASH_DEL(*head, entry);
  collection_release(entry, sizeof(map_entry_t) + entry->key_len);
}

void collections_set_remove(set_entry_t **head, set_entry_t *entry) {
  if (entry->cursored) {
    set_store_t *store = set_store_of(head);
    collection_cursor_t *cursor = cursor_find(&store->cursors, entry);
    set_entry_t *next = (set_entry_t *)entry->hh.next;
    if (next && !next->cursored) {
      cursor->entry = next;
      next->cursored = 1;
    } else {
      cursor_detach(cursor);
      cursor_place(&store->cursors, cursor, next);
    }
  }
  HASH_DEL(*head, entry);
  collection_release(entry, sizeof(set_entry_t) + entry->key_len);
}

static void map_free_entries(map_entry_t **head) {
  map_entry_t *entry = *head, *next;
  HASH_CLEAR(hh, *head);
  for (; entry; entry = next) {
    next = (map_entry_t *)entry->hh.next;
    collection_release(entry, sizeof(map_entry_t) + entry->key_len);
  }
}

void collections_map_clear(map_entry_t **head) {
  cursors_rewind(&map_store_of(head)->cursors);
  map_free_entries(head);
}

static void set_free_entries(set_entry_t **head) {
  set_entry_t *entry = *head, *next;
  HASH_CLEAR(hh, *head);
  for (; entry; entry = next) {
    next = (set_entry_t *)entry->hh.next;
    collection_release(entry, sizeof(set_entry_t) + entry->key_len);
  }
}

void collections_set_clear(set_entry_t **head) {
  cursors_rewind(&set_store_of(head)->cursors);
  set_free_entries(head);
}

void collections_map_store_free(map_entry_t **head) {
  if (!head) return;
  map_store_t *store = map_store_of(head);
  cursors_orphan(&store->cursors);
  map_free_entries(&store->head);
  collection_release(store, sizeof(*store));
}

void collections_set_store_free(set_entry_t **head) {
  if (!head) return;
  set_store_t *store = set_store_of(head);
  cursors_orphan(&store->cursors);
  set_free_entries(&store->head);
  collection_release(store, sizeof(*store));
}

ant_value_t collections_entry_pair(ant_t *js, ant_value_t first, ant_value_t second) {
  ant_value_t pair[2] = { first, second };
  return js_mkarr_dense_literal(js, pair, 2);
}

static bool map_store_entry(
  ant_t *js,
  map_entry_t **map_ptr,
  ant_value_t raw_key,
  ant_value_t key_val,
  ant_value_t value
) {
  collection_key_t key;
  if (!collection_key_init(js, raw_key, &key)) return false;

  map_entry_t *entry = NULL;
  unsigned key_hash = collection_key_hash(&key);
  HASH_FIND_BYHASHVALUE(hh, *map_ptr, key.bytes, key.len, key_hash, entry);
  
  if (entry) {
    entry->key_val = key_val;
    entry->value = value;
    collection_key_free(&key);
    return true;
  }

  entry = collection_alloc(sizeof(map_entry_t) + key.len);
  if (!entry) {
    collection_key_free(&key);
    return false;
  }

  entry->key = (unsigned char *)(entry + 1);
  memcpy(entry->key, key.bytes, key.len);
  entry->key_len = (uint32_t)key.len;
  entry->cursored = 0;
  entry->key_val = key_val;
  entry->value = value;
  
  HASH_ADD_KEYPTR_BYHASHVALUE(hh, *map_ptr, entry->key, entry->key_len, key_hash, entry);
  map_store_t *store = map_store_of(map_ptr);
  if (store->cursors.end) cursors_resume(&store->cursors, entry);
  collection_key_free(&key);
  
  return true;
}

static bool set_store_entry(ant_t *js, set_entry_t **set_ptr, ant_value_t value) {
  ant_value_t stored_value = normalize_map_key(value);
  
  collection_key_t key;
  if (!collection_key_init(js, stored_value, &key)) return false;

  set_entry_t *entry = NULL;
  unsigned key_hash = collection_key_hash(&key);
  HASH_FIND_BYHASHVALUE(hh, *set_ptr, key.bytes, key.len, key_hash, entry);
  
  if (entry) {
    collection_key_free(&key);
    return true;
  }

  entry = collection_alloc(sizeof(set_entry_t) + key.len);
  if (!entry) {
    collection_key_free(&key);
    return false;
  }

  entry->key = (unsigned char *)(entry + 1);
  memcpy(entry->key, key.bytes, key.len);
  entry->key_len = (uint32_t)key.len;
  entry->cursored = 0;
  entry->value = stored_value;
  
  HASH_ADD_KEYPTR_BYHASHVALUE(hh, *set_ptr, entry->key, entry->key_len, key_hash, entry);
  set_store_t *store = set_store_of(set_ptr);
  if (store->cursors.end) cursors_resume(&store->cursors, entry);
  collection_key_free(&key);
  
  return true;
}

bool collections_map_store_cloned_entry(
  ant_t *js, map_entry_t **map_ptr, ant_value_t key, ant_value_t value
) {
  ant_value_t key_val = normalize_map_key(key);
  return map_store_entry(js, map_ptr, key, key_val, value);
}

ant_value_t collections_map_create(ant_t *js, map_entry_t ***out_map) {
  ant_value_t map_obj = Ant_Object_CreateBare(js);
  if (is_err(map_obj)) return map_obj;
  js_obj_ptr(map_obj)->type_tag = kTypeMap;

  ant_value_t map_proto = js->builtins.map_proto;
  if (is_special_object(map_proto)) js_set_proto_init(map_obj, map_proto);

  map_entry_t **map_head = collections_map_store_new();
  if (!map_head) return Ant_Error_CreateOutOfMemory(js);

  js_set_native(map_obj, map_head, MAP_NATIVE_TAG);
  if (out_map) *out_map = map_head;
  return map_obj;
}

bool collections_set_store_cloned_entry(ant_t *js, set_entry_t **set_ptr, ant_value_t value) {
  return set_store_entry(js, set_ptr, value);
}

map_entry_t **get_map_from_obj(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != kTypeMap) return NULL;
  return (map_entry_t **)js_get_native(obj, MAP_NATIVE_TAG);
}

set_entry_t **get_set_from_obj(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != kTypeSet) return NULL;
  return (set_entry_t **)js_get_native(obj, SET_NATIVE_TAG);
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

static inline ant_value_t collection_arg(ant_value_t *args, int nargs, int i) {
  return i < nargs ? args[i] : js_mkundef();
}

static ant_value_t map_set(ant_params_t) {
  ant_value_t this_val = js->this_val;
  
  map_entry_t **map_ptr = get_map_from_obj(this_val);
  if (!map_ptr) return collection_receiver_error(js, "Map.prototype.set");

  ant_value_t key = collection_arg(args, nargs, 0);
  ant_value_t value = collection_arg(args, nargs, 1);
  ant_value_t key_val = normalize_map_key(key);
  
  if (!map_store_entry(js, map_ptr, key, key_val, value))
    return Ant_Error_CreateOutOfMemory(js);

  ant_object_t *map_obj = js_obj_ptr(this_val);
  if (map_obj) {
    gc_write_barrier(js, map_obj, key_val);
    gc_write_barrier(js, map_obj, value);
  }

  return this_val;
}

static ant_value_t map_get(ant_params_t) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(this_val);
  
  if (!map_ptr) return collection_receiver_error(js, "Map.prototype.get");
  map_entry_t *entry = Ant_Map_Find(js, map_ptr, collection_arg(args, nargs, 0));
  
  return entry ? entry->value : js_mkundef();
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

__attribute__((aligned(64))) ant_value_t collections_map_get_numeric_pair(
  ant_t *js, ant_value_t map, ant_value_t left, 
  const char *separator, size_t separator_len, ant_value_t right
) {
  map_entry_t **map_ptr = get_map_from_obj(map);
  if (!map_ptr) return collection_receiver_error(js, "Map.prototype.get");

  unsigned char inline_key[128];
  char right_buf[32];
  
  size_t left_len = collection_number_to_string(tod(left), (char *)inline_key + 1, sizeof(right_buf));  
  if (left_len > sizeof(right_buf)) return Ant_Error_CreateOutOfMemory(js);

  bool right_inline = separator_len <= sizeof(inline_key) - 1 - left_len - sizeof(right_buf);
  char *right_out = right_inline ? (char *)inline_key + 1 + left_len + separator_len : right_buf;
  size_t right_len = collection_number_to_string(tod(right), right_out, sizeof(right_buf));

  if (right_len > sizeof(right_buf) ||
    separator_len > SIZE_MAX - 1 - left_len - right_len)
    return Ant_Error_CreateOutOfMemory(js);

  size_t key_len = 1 + left_len + separator_len + right_len;
  unsigned char *key = inline_key;
  
  if (key_len > sizeof(inline_key)) {
    key = malloc(key_len);
    if (!key) return Ant_Error_CreateOutOfMemory(js);
    memcpy(key + 1, inline_key + 1, left_len);
  }

  key[0] = (uint8_t)kTypeString;
  if (separator_len > 0) memcpy(key + 1 + left_len, separator, separator_len);
  if (!right_inline) memcpy(key + 1 + left_len + separator_len, right_buf, right_len);
  map_entry_t *entry = NULL;
  
  unsigned key_hash = (unsigned)hash_key((const char *)key, key_len);
  HASH_FIND_BYHASHVALUE(hh, *map_ptr, key, key_len, key_hash, entry);
  
  if (key != inline_key) free(key);
  return entry ? entry->value : js_mkundef();
}

static ant_value_t map_has(ant_params_t) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(this_val);

  if (!map_ptr) return collection_receiver_error(js, "Map.prototype.has");
  map_entry_t *entry = Ant_Map_Find(js, map_ptr, collection_arg(args, nargs, 0));
  
  return js_bool(entry != NULL);
}

bool collections_is_map_has_builtin(ant_value_t func) {
  return vtype(func) == kTypeBuiltin && js_cfunc_same_entrypoint(func, map_has);
}

static ant_value_t map_numeric_template_general(
  ant_t *js, map_entry_t **map_ptr, bool return_presence,
  const ant_value_t *substitutions, const sv_map_template_desc_t *desc
) {
  uint8_t substitution_count = desc->substitution_count;
  char number_bufs[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS][32];
  
  size_t number_lens[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS] = {0};
  size_t key_len = 1;
  
  for (uint8_t i = 0; i < substitution_count; i++) {
    number_lens[i] = collection_number_to_string(
      tod(substitutions[i]), number_bufs[i], sizeof(number_bufs[i]));
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
  *out++ = (uint8_t)kTypeString;
  
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

  map_entry_t *entry = NULL;
  unsigned key_hash = (unsigned)hash_key((const char *)key, key_len);
  
  HASH_FIND_BYHASHVALUE(hh, *map_ptr, key, key_len, key_hash, entry);
  if (key != inline_key) free(key);
  
  return return_presence 
    ? js_bool(entry != NULL)
    : (entry ? entry->value : js_mkundef());
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

  map_entry_t **map_ptr = get_map_from_obj(map);
  if (!map_ptr) return collection_receiver_error(js, return_presence ? "Map.prototype.has" : "Map.prototype.get");
  
  return map_numeric_template_general(
    js, map_ptr, return_presence, substitutions, desc);
}

static ant_value_t map_upsert(ant_params_t) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(this_val);
  if (!map_ptr) return collection_receiver_error(js, "Map.prototype.upsert");

  ant_value_t key = collection_arg(args, nargs, 0);
  ant_value_t update_fn = collection_arg(args, nargs, 1);
  ant_value_t insert_fn = collection_arg(args, nargs, 2);
  
  if (!is_callable(update_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map.upsert update callback must be callable");
  
  if (!is_callable(insert_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map.upsert insert callback must be callable");

  map_entry_t *entry = Ant_Map_Find(js, map_ptr, key);
  ant_value_t value;

  if (entry) {
    ant_value_t call_args[3] = { entry->value, key, this_val };
    value = sv_vm_call(js->vm, js, update_fn, js_mkundef(), call_args, 3, NULL, js_mkundef());
  } else {
    ant_value_t call_args[2] = { key, this_val };
    value = sv_vm_call(js->vm, js, insert_fn, js_mkundef(), call_args, 2, NULL, js_mkundef());
  }

  if (is_err(value)) return value;

  ant_value_t key_val = normalize_map_key(key);
  if (!map_store_entry(js, map_ptr, key, key_val, value))
    return Ant_Error_CreateOutOfMemory(js);

  ant_object_t *map_obj = js_obj_ptr(this_val);
  if (map_obj) {
    gc_write_barrier(js, map_obj, key_val);
    gc_write_barrier(js, map_obj, value);
  }

  return value;
}

static ant_value_t map_delete(ant_params_t) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(this_val);

  if (!map_ptr) return collection_receiver_error(js, "Map.prototype.delete");
  map_entry_t *entry = Ant_Map_Find(js, map_ptr, collection_arg(args, nargs, 0));

  if (entry) {
    collections_map_remove(map_ptr, entry);
    return js_true;
  }

  return js_false;
}

static ant_value_t map_clear(ant_params_t) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(this_val);
  
  if (!map_ptr) return collection_receiver_error(js, "Map.prototype.clear");
  collections_map_clear(map_ptr);
  
  return js_mkundef();
}

static ant_value_t map_size(ant_params_t) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(this_val);
  if (!map_ptr) return collection_receiver_error(js, "get Map.prototype.size");
  
  return js_mknum((double)HASH_COUNT(*map_ptr));
}

static ant_value_t map_forEach(ant_params_t) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(this_val);
  if (!map_ptr) return collection_receiver_error(js, "Map.prototype.forEach");

  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "forEach requires a callback function");
  
  sv_callback_t cb = sv_callback_prepare(js, args[0], nargs > 1 ? args[1] : js_mkundef());
  
  map_iterator_state_t it;
  collections_map_iter_open(&it, map_ptr, ITER_TYPE_MAP_ENTRIES);
  ant_value_t result = js_mkundef();
  
  for (map_entry_t *entry; (entry = map_iter_take(&it));) {
    ant_value_t call_args[3] = { entry->value, entry->key_val, this_val };
    result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(result)) break;
  }
  
  collections_map_iter_close(&it);
  return is_err(result) ? result : js_mkundef();
}

static void collection_iter_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  uint32_t tag = obj->native_tag;
  if (tag != MAP_ITER_NATIVE_TAG && tag != SET_ITER_NATIVE_TAG) return;
  iter_state_dispose(js_get_native(value, tag));
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
  if (!js_check_native_tag(js->this_val, MAP_ITER_NATIVE_TAG)) return collection_receiver_error(js, "Map Iterator.prototype.next");
  return js_iter_next_result(js, advance_map);
}

static ant_value_t create_map_iterator(ant_t *js, ant_value_t map_obj, iter_type_t type, const char *method) {
  map_entry_t **map_ptr = get_map_from_obj(map_obj);
  if (!map_ptr) return collection_receiver_error(js, method);

  map_iterator_state_t *state = collection_alloc(sizeof(map_iterator_state_t));
  if (!state) return Ant_Error_CreateOutOfMemory(js);
  iter_state_open(state, &map_store_of(map_ptr)->cursors, *map_ptr, type, true);

  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_proto_init(iter, js->builtins.map_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, map_obj);
  js_set_native(iter, state, MAP_ITER_NATIVE_TAG);
  js_set_finalizer(iter, collection_iter_finalize);
  
  return iter;
}

static ant_value_t map_values(ant_params_t) {
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_VALUES, "Map.prototype.values");
}

static ant_value_t map_keys(ant_params_t) {
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_KEYS, "Map.prototype.keys");
}

static ant_value_t map_entries(ant_params_t) {
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_ENTRIES, "Map.prototype.entries");
}

bool advance_set(ant_t *js, iterator_t *it, ant_value_t *out) {
  set_iterator_state_t *state = get_set_iter_state(it->iterator);
  set_entry_t *entry = state ? set_iter_take(state) : NULL;
  if (!entry) return false;
  *out = set_iter_value(js, state, entry);
  return true;
}

static ant_value_t set_iter_next(ant_params_t) {
  if (!js_check_native_tag(js->this_val, SET_ITER_NATIVE_TAG)) return collection_receiver_error(js, "Set Iterator.prototype.next");
  return js_iter_next_result(js, advance_set);
}

static ant_value_t create_set_iterator(ant_t *js, ant_value_t set_obj, iter_type_t type, const char *method) {
  set_entry_t **set_ptr = get_set_from_obj(set_obj);
  if (!set_ptr) return collection_receiver_error(js, method);

  set_iterator_state_t *state = collection_alloc(sizeof(set_iterator_state_t));
  if (!state) return Ant_Error_CreateOutOfMemory(js);
  iter_state_open(state, &set_store_of(set_ptr)->cursors, *set_ptr, type, true);

  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_proto_init(iter, js->builtins.set_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, set_obj);
  js_set_native(iter, state, SET_ITER_NATIVE_TAG);
  js_set_finalizer(iter, collection_iter_finalize);
  
  return iter;
}

static ant_value_t set_add(ant_params_t) {
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(this_val);
  if (!set_ptr) return collection_receiver_error(js, "Set.prototype.add");

  ant_value_t value = collection_arg(args, nargs, 0);
  if (!set_store_entry(js, set_ptr, value))
    return Ant_Error_CreateOutOfMemory(js);

  ant_object_t *set_obj = js_obj_ptr(this_val);
  if (set_obj) gc_write_barrier(js, set_obj, value);

  return this_val;
}

static ant_value_t set_has(ant_params_t) {
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(this_val);
  if (!set_ptr) return collection_receiver_error(js, "Set.prototype.has");

  set_entry_t *entry = Ant_Set_Find(js, set_ptr, collection_arg(args, nargs, 0));
  return js_bool(entry != NULL);
}

static ant_value_t set_delete(ant_params_t) {
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(this_val);
  if (!set_ptr) return collection_receiver_error(js, "Set.prototype.delete");

  set_entry_t *entry = Ant_Set_Find(js, set_ptr, collection_arg(args, nargs, 0));
  
  if (entry) {
    collections_set_remove(set_ptr, entry);
    return js_true;
  }

  return js_false;
}

static ant_value_t set_clear(ant_params_t) {
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(this_val);
  if (!set_ptr) return collection_receiver_error(js, "Set.prototype.clear");

  collections_set_clear(set_ptr);
  return js_mkundef();
}

static ant_value_t set_size(ant_params_t) {
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(this_val);
  if (!set_ptr) return collection_receiver_error(js, "get Set.prototype.size");
  
  return js_mknum((double)HASH_COUNT(*set_ptr));
}

static ant_value_t set_values(ant_params_t) {
  return create_set_iterator(js, js->this_val, ITER_TYPE_SET_VALUES, "Set.prototype.values");
}

static ant_value_t set_entries(ant_params_t) {
  return create_set_iterator(js, js->this_val, ITER_TYPE_SET_ENTRIES, "Set.prototype.entries");
}

static ant_value_t set_forEach(ant_params_t) {
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(this_val);
  if (!set_ptr) return collection_receiver_error(js, "Set.prototype.forEach");

  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "forEach requires a callback function");
  
  sv_callback_t cb = sv_callback_prepare(js, args[0], nargs > 1 ? args[1] : js_mkundef());
  
  set_iterator_state_t it;
  collections_set_iter_open(&it, set_ptr, ITER_TYPE_SET_VALUES);
  ant_value_t result = js_mkundef();
  
  for (set_entry_t *entry; (entry = set_iter_take(&it));) {
    ant_value_t call_args[3] = { entry->value, entry->value, this_val };
    result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(result)) break;
  }
  
  collections_set_iter_close(&it);
  return is_err(result) ? result : js_mkundef();
}

ant_value_t collections_set_create(ant_t *js, set_entry_t ***out_set) {
  ant_value_t set_obj = Ant_Object_CreateBare(js);
  if (is_err(set_obj)) return set_obj;
  js_obj_ptr(set_obj)->type_tag = kTypeSet;

  ant_value_t set_proto = js->builtins.set_proto;
  if (is_special_object(set_proto)) js_set_proto_init(set_obj, set_proto);

  set_entry_t **set_head = collections_set_store_new();
  if (!set_head) return Ant_Error_CreateOutOfMemory(js);
  *set_head = NULL;
  
  js_set_native(set_obj, set_head, SET_NATIVE_TAG);
  if (out_set) *out_set = set_head;
  
  return set_obj;
}

static bool set_result_add(ant_t *js, ant_value_t set_obj, set_entry_t **set_ptr, ant_value_t value) {
  if (!set_store_entry(js, set_ptr, value)) return false;
  ant_object_t *obj = js_obj_ptr(set_obj);
  if (obj) gc_write_barrier(js, obj, value);
  return true;
}

static bool set_result_clone(ant_t *js, ant_value_t set_obj, set_entry_t **set_ptr, const set_entry_t *from) {
  set_entry_t *entry = collection_alloc(sizeof(set_entry_t) + from->key_len);
  if (!entry) return false;
  entry->key = (unsigned char *)(entry + 1);
  memcpy(entry->key, from->key, from->key_len);
  entry->key_len = from->key_len;
  entry->cursored = 0;
  entry->value = from->value;
  HASH_ADD_KEYPTR_BYHASHVALUE(hh, *set_ptr, entry->key, entry->key_len, from->hh.hashv, entry);
  ant_object_t *obj = js_obj_ptr(set_obj);
  if (obj) gc_write_barrier(js, obj, entry->value);
  return true;
}

static bool set_result_copy(ant_t *js, ant_value_t set_obj, set_entry_t **set_ptr, set_entry_t **from) {
  for (set_entry_t *each = *from; each; each = (set_entry_t *)each->hh.next) {
    if (!set_result_clone(js, set_obj, set_ptr, each)) return false;
  }
  return true;
}

static void set_result_delete(ant_t *js, set_entry_t **set_ptr, ant_value_t value) {
  set_entry_t *entry = Ant_Set_Find(js, set_ptr, value);
  if (entry) collections_set_remove(set_ptr, entry);
}

typedef struct {
  ant_value_t obj;
  ant_value_t has;
  ant_value_t keys;
  double size;
  set_entry_t **native;
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
  
  return js_mkundef();
}

static ant_value_t set_record_has(ant_t *js, set_record_t *record, ant_value_t value, bool *out) {
  ant_value_t result = sv_vm_call(js->vm, js, record->has, record->obj, &value, 1, NULL, js_mkundef());
  if (is_err(result)) return result;
  *out = js_truthy(js, result);
  return js_mkundef();
}

static ant_value_t set_record_has_entry(ant_t *js, set_record_t *record, const set_entry_t *entry, bool *out) {
  if (!record->native) return set_record_has(js, record, entry->value, out);
  set_entry_t *found = NULL;
  HASH_FIND_BYHASHVALUE(hh, *record->native, entry->key, entry->key_len, entry->hh.hashv, found);
  *out = found != NULL;
  return js_mkundef();
}

static ant_value_t set_record_close_keys_iterator(ant_t *js, ant_value_t iterator) {
  iterator_t it = { .iterator = iterator };
  js_iter_close(js, &it);
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_mkundef();
}

static ant_value_t set_record_for_each_key(ant_t *js, set_record_t *record, set_key_cb cb, void *ctx) {
  ant_value_t iterator = sv_vm_call(js->vm, js, record->keys, record->obj, NULL, 0, NULL, js_mkundef());
  if (is_err(iterator)) return iterator;
  if (!is_object_type(iterator))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set keys() result is not an iterator");

  ant_value_t next_fn = js_getprop_fallback(js, iterator, "next");
  if (is_err(next_fn)) return next_fn;
  if (!is_callable(next_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set keys() iterator has no callable next method");

  while (true) {
    ant_value_t next = sv_vm_call(js->vm, js, next_fn, iterator, NULL, 0, NULL, js_mkundef());
    if (is_err(next)) return next;
    if (!is_object_type(next))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Set keys() iterator result is not an object");
    
    ant_value_t done = js_getprop_fallback(js, next, "done");
    if (is_err(done)) return done;
    if (js_truthy(js, done)) return js_mkundef();
    
    ant_value_t value = js_getprop_fallback(js, next, "value");
    if (is_err(value)) return value;
    
    ant_value_t result = js_mkundef();
    set_key_status_t status = cb(js, value, &result, ctx);
    
    if (is_err(result)) {
      ant_value_t close_result = set_record_close_keys_iterator(js, iterator);
      return is_err(close_result) ? close_result : result;
    }
    
    if (status == SET_KEY_STOP) {
      ant_value_t close_result = set_record_close_keys_iterator(js, iterator);
      return is_err(close_result) ? close_result : js_mkundef();
    }
  }
}

typedef struct {
  ant_value_t out;
  set_entry_t **out_set;
} set_build_ctx_t;

static set_key_status_t set_add_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_build_ctx_t *build = (set_build_ctx_t *)ctx;
  if (!set_result_add(js, build->out, build->out_set, value))
    *result = Ant_Error_CreateOutOfMemory(js);
  return SET_KEY_CONTINUE;
}

static ant_value_t set_union(ant_params_t) {
  set_entry_t **this_set = get_set_from_obj(js->this_val);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.union");
  
  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "union", &other);
  if (is_err(rec)) return rec;

  set_entry_t **out_set = NULL;
  ant_value_t out = collections_set_create(js, &out_set);
  if (is_err(out)) return out;
  set_build_ctx_t build = { out, out_set };

  if (!set_result_copy(js, out, out_set, this_set)) return Ant_Error_CreateOutOfMemory(js);
  ant_value_t result = set_record_for_each_key(js, &other, set_add_key_cb, &build);
  
  return is_err(result) ? result : out;
}

typedef struct {
  ant_value_t out;
  set_entry_t **out_set;
  set_entry_t **this_set;
} set_compare_build_ctx_t;

static set_key_status_t set_intersection_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_compare_build_ctx_t *build = (set_compare_build_ctx_t *)ctx;
  if (
    Ant_Set_Find(js, build->this_set, value) && 
    !set_result_add(js, build->out, build->out_set, value)
  ) *result = Ant_Error_CreateOutOfMemory(js);
  return SET_KEY_CONTINUE;
}

static ant_value_t set_intersection(ant_params_t) {
  set_entry_t **this_set = get_set_from_obj(js->this_val);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.intersection");
  
  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "intersection", &other);
  if (is_err(rec)) return rec;

  set_entry_t **out_set = NULL;
  ant_value_t out = collections_set_create(js, &out_set);
  if (is_err(out)) return out;

  double this_size = (double)HASH_COUNT(*this_set);
  if (this_size <= other.size) {
    set_iterator_state_t it;
    collections_set_iter_open(&it, this_set, ITER_TYPE_SET_VALUES);
    ant_value_t result = out;
    for (set_entry_t *entry; (entry = set_iter_take(&it));) {
      bool has = false;
      ant_value_t value = entry->value;
      result = set_record_has_entry(js, &other, entry, &has);
      if (is_err(result)) break;
      if (has && !set_result_add(js, out, out_set, value)) {
        result = Ant_Error_CreateOutOfMemory(js);
        break;
      }
      result = out;
    }
    collections_set_iter_close(&it);
    return result;
  }

  set_compare_build_ctx_t build = { out, out_set, this_set };
  ant_value_t result = set_record_for_each_key(js, &other, set_intersection_key_cb, &build);
  
  return is_err(result) ? result : out;
}

static set_key_status_t set_delete_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_build_ctx_t *build = (set_build_ctx_t *)ctx;
  set_result_delete(js, build->out_set, value);
  return SET_KEY_CONTINUE;
}

static ant_value_t set_difference(ant_params_t) {
  set_entry_t **this_set = get_set_from_obj(js->this_val);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.difference");
  
  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "difference", &other);
  if (is_err(rec)) return rec;

  set_entry_t **out_set = NULL;
  ant_value_t out = collections_set_create(js, &out_set);
  if (is_err(out)) return out;

  bool scan = (double)HASH_COUNT(*this_set) <= other.size;
  if (scan && other.native) {
    for (set_entry_t *each = *this_set; each; each = (set_entry_t *)each->hh.next) {
      bool has = false;
      set_record_has_entry(js, &other, each, &has);
      if (!has && !set_result_clone(js, out, out_set, each)) return Ant_Error_CreateOutOfMemory(js);
    }
    return out;
  }

  if (!set_result_copy(js, out, out_set, this_set)) return Ant_Error_CreateOutOfMemory(js);

  if (scan) {
    set_iterator_state_t it;
    collections_set_iter_open(&it, this_set, ITER_TYPE_SET_VALUES);
    ant_value_t result = out;
    for (set_entry_t *each; (each = set_iter_take(&it));) {
      bool has = false;
      set_entry_t *twin = NULL;
      HASH_FIND_BYHASHVALUE(hh, *out_set, each->key, each->key_len, each->hh.hashv, twin);
      result = set_record_has_entry(js, &other, each, &has);
      if (is_err(result)) break;
      if (has && twin) collections_set_remove(out_set, twin);
      result = out;
    }
    collections_set_iter_close(&it);
    return result;
  }

  set_build_ctx_t build = { out, out_set };
  ant_value_t result = set_record_for_each_key(js, &other, set_delete_key_cb, &build);
  
  return is_err(result) ? result : out;
}

typedef struct {
  ant_value_t out;
  set_entry_t **out_set;
  set_entry_t **this_set;
} set_symdiff_ctx_t;

static set_key_status_t set_symmetric_difference_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_symdiff_ctx_t *build = (set_symdiff_ctx_t *)ctx;
  if (Ant_Set_Find(js, build->this_set, value)) {
    set_result_delete(js, build->out_set, value);
  } else if (!Ant_Set_Find(js, build->out_set, value))
    if (!set_result_add(js, build->out, build->out_set, value)) *result = Ant_Error_CreateOutOfMemory(js);
  return SET_KEY_CONTINUE;
}

static ant_value_t set_symmetricDifference(ant_params_t) {
  set_entry_t **this_set = get_set_from_obj(js->this_val);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.symmetricDifference");
  
  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "symmetricDifference", &other);
  if (is_err(rec)) return rec;

  set_entry_t **out_set = NULL;
  ant_value_t out = collections_set_create(js, &out_set);
  if (is_err(out)) return out;

  if (!set_result_copy(js, out, out_set, this_set)) return Ant_Error_CreateOutOfMemory(js);
  set_symdiff_ctx_t build = { out, out_set, this_set };
  ant_value_t result = set_record_for_each_key(js, &other, set_symmetric_difference_key_cb, &build);
  return is_err(result) ? result : out;
}

static ant_value_t set_isSubsetOf(ant_params_t) {
  set_entry_t **this_set = get_set_from_obj(js->this_val);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.isSubsetOf");
  
  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "isSubsetOf", &other);
  
  if (is_err(rec)) return rec;
  if ((double)HASH_COUNT(*this_set) > other.size) return js_false;

  set_iterator_state_t it;
  collections_set_iter_open(&it, this_set, ITER_TYPE_SET_VALUES);
  ant_value_t result = js_true;
  for (set_entry_t *entry; (entry = set_iter_take(&it));) {
    bool has = false;
    result = set_record_has_entry(js, &other, entry, &has);
    if (is_err(result)) break;
    result = js_bool(has);
    if (!has) break;
  }
  collections_set_iter_close(&it);
  return result;
}

typedef struct {
  set_entry_t **this_set;
  bool result;
} set_predicate_ctx_t;

static set_key_status_t set_superset_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_predicate_ctx_t *pred = (set_predicate_ctx_t *)ctx;
  if (!Ant_Set_Find(js, pred->this_set, value)) {
    pred->result = false;
    return SET_KEY_STOP;
  }
  return SET_KEY_CONTINUE;
}

static ant_value_t set_isSupersetOf(ant_params_t) {
  set_entry_t **this_set = get_set_from_obj(js->this_val);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.isSupersetOf");
  
  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "isSupersetOf", &other);
  if (is_err(rec)) return rec;
  if ((double)HASH_COUNT(*this_set) < other.size) return js_false;

  set_predicate_ctx_t pred = { this_set, true };
  ant_value_t result = set_record_for_each_key(js, &other, set_superset_key_cb, &pred);
  if (is_err(result)) return result;
  return js_bool(pred.result);
}

static set_key_status_t set_disjoint_key_cb(ant_t *js, ant_value_t value, ant_value_t *result, void *ctx) {
  set_predicate_ctx_t *pred = (set_predicate_ctx_t *)ctx;
  if (Ant_Set_Find(js, pred->this_set, value)) {
    pred->result = false;
    return SET_KEY_STOP;
  }
  return SET_KEY_CONTINUE;
}

static ant_value_t set_isDisjointFrom(ant_params_t) {
  set_entry_t **this_set = get_set_from_obj(js->this_val);
  if (!this_set) return collection_receiver_error(js, "Set.prototype.isDisjointFrom");
  
  set_record_t other;
  ant_value_t rec = get_set_record(js, collection_arg(args, nargs, 0), "isDisjointFrom", &other);
  if (is_err(rec)) return rec;

  if ((double)HASH_COUNT(*this_set) <= other.size) {
    set_iterator_state_t it;
    collections_set_iter_open(&it, this_set, ITER_TYPE_SET_VALUES);
    ant_value_t result = js_true;
    for (set_entry_t *entry; (entry = set_iter_take(&it));) {
      bool has = false;
      result = set_record_has_entry(js, &other, entry, &has);
      if (is_err(result)) break;
      result = js_bool(!has);
      if (has) break;
    }
    collections_set_iter_close(&it);
    return result;
  }

  set_predicate_ctx_t pred = { this_set, true };
  ant_value_t result = set_record_for_each_key(js, &other, set_disjoint_key_cb, &pred);
  if (is_err(result)) return result;
  
  return js_bool(pred.result);
}

static ant_value_t weakmap_set(ant_params_t) {
  ant_value_t this_val = js->this_val;
  ant_object_t *object = NULL;
  weakmap_table_t *table = get_weakmap_from_obj(this_val, &object);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.set");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid value used as weak map key");

  if (!weakmap_store_entry(js, object, table, key, collection_arg(args, nargs, 1)))
    return Ant_Error_CreateOutOfMemory(js);
  
  return this_val;
}

static ant_value_t weakmap_get(ant_params_t) {
  ant_value_t this_val = js->this_val;
  weakmap_table_t *table = get_weakmap_from_obj(this_val, NULL);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.get");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key)) return js_mkundef();
  
  weakmap_entry_t *entry = weakmap_table_find(table, key);
  return entry ? entry->value : js_mkundef();
}

static ant_value_t weakmap_has(ant_params_t) {
  ant_value_t this_val = js->this_val;
  weakmap_table_t *table = get_weakmap_from_obj(this_val, NULL);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.has");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key)) return js_false;
  
  return js_bool(weakmap_table_find(table, key) != NULL);
}

static ant_value_t weakmap_upsert(ant_params_t) {
  ant_value_t this_val = js->this_val;
  ant_object_t *object = NULL;
  weakmap_table_t *table = get_weakmap_from_obj(this_val, &object);
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
    ant_value_t call_args[3] = { entry->value, key_obj, this_val };
    value = sv_vm_call(js->vm, js, update_fn, js_mkundef(), call_args, 3, NULL, js_mkundef());
  } else {
    ant_value_t call_args[2] = { key_obj, this_val };
    value = sv_vm_call(js->vm, js, insert_fn, js_mkundef(), call_args, 2, NULL, js_mkundef());
  }

  if (is_err(value)) return value;

  if (!weakmap_store_entry(js, object, table, key_obj, value))
    return Ant_Error_CreateOutOfMemory(js);

  return value;
}

static ant_value_t weakmap_delete(ant_params_t) {
  ant_value_t this_val = js->this_val;
  weakmap_table_t *table = get_weakmap_from_obj(this_val, NULL);
  if (!table) return collection_receiver_error(js, "WeakMap.prototype.delete");

  ant_value_t key = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(key)) return js_false;
  
  return js_bool(weakmap_table_delete(table, key));
}

static ant_value_t weakset_add(ant_params_t) {
  ant_value_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(this_val);
  if (!ws_ptr) return collection_receiver_error(js, "WeakSet.prototype.add");

  ant_value_t value_obj = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(value_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid value used in weak set");
  
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), entry);
  
  if (!entry) {
    entry = calloc(1, sizeof(weakset_entry_t));
    if (!entry) return Ant_Error_CreateOutOfMemory(js);
    entry->value_obj = value_obj;
    HASH_ADD(hh, *ws_ptr, value_obj, sizeof(ant_value_t), entry);
  }

  ant_object_t *ws_obj = js_obj_ptr(this_val);
  if (ws_obj) {
    gc_weak_remember_set(js, ws_obj, value_obj);
    gc_write_barrier(js, ws_obj, value_obj);
  }
  
  return this_val;
}

static ant_value_t weakset_has(ant_params_t) {
  ant_value_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(this_val);
  if (!ws_ptr) return collection_receiver_error(js, "WeakSet.prototype.has");

  ant_value_t value_obj = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(value_obj)) return js_false;
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), entry);
  return js_bool(entry != NULL);
}

static ant_value_t weakset_delete(ant_params_t) {
  ant_value_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(this_val);
  if (!ws_ptr) return collection_receiver_error(js, "WeakSet.prototype.delete");

  ant_value_t value_obj = collection_arg(args, nargs, 0);
  if (!can_be_held_weakly(value_obj)) return js_false;
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), entry);
  
  if (entry) {
    HASH_DEL(*ws_ptr, entry);
    free(entry);
    return js_true;
  }
  return js_false;
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
  
  ant_value_t wr_obj = js->this_val;
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
  ant_value_t this_val = js->this_val;
  weakref_state_t *state = vtype(this_val) == kTypeObject
    ? js_get_native(this_val, WEAKREF_NATIVE_TAG) : NULL;
    
  if (!state) return js_mkerr_typed(
    js, JS_ERR_TYPE,
    "WeakRef.prototype.deref called on incompatible receiver"
  );

  ant_value_t target = state->target;
  if (vtype(target) != kTypeUndefined) gc_weak_keep_alive(js, target);
  
  return target;
}

static ant_value_t builtin_FinalizationRegistry(ant_params_t) {
  if (nargs < 1 || (vtype(args[0]) != kTypeFunction && vtype(args[0]) != kTypeBuiltin)) {
    return js_mkerr(js, "FinalizationRegistry callback must be a function");
  }
  
  ant_value_t fr_obj = Ant_Object_CreateBare(js);
  ant_value_t fr_proto = js->builtins.finreg_proto;
  if (is_special_object(fr_proto)) js_set_proto_init(fr_obj, fr_proto);
  
  js_set_slot(fr_obj, SLOT_MAP, js_mkarr(js));
  js_set_slot(fr_obj, SLOT_DATA, args[0]);
  
  return fr_obj;
}

static ant_value_t finreg_register(ant_params_t) {
  ant_value_t this_val = js->this_val;
  if (vtype(this_val) != kTypeObject) return js_mkundef();
  
  if (nargs < 1 || !can_be_held_weakly(args[0]))
    return js_mkerr(js, "FinalizationRegistry.register target must be an object");
  
  ant_value_t target = args[0];
  ant_value_t held_value = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t unregister_token = nargs > 2 ? args[2] : js_mkundef();
  
  if (target == held_value)
    return js_mkerr(js, "target and held value must not be the same");
  
  if (vtype(unregister_token) != kTypeUndefined && !can_be_held_weakly(unregister_token))
    return js_mkerr(js, "FinalizationRegistry.register token must be an object");
  
  ant_value_t registrations = js_get_slot(this_val, SLOT_MAP);
  if (vtype(registrations) != kTypeArray) return js_mkundef();
  
  ant_value_t entry = js_mkarr(js);
  ant_offset_t len = js_arr_len(js, registrations);
  
  char idx[16];
  size_t idx_len = uint_to_str(idx, sizeof(idx), 0);
  js_setprop(js, entry, js_mkstr(js, idx, idx_len), target);
  idx_len = uint_to_str(idx, sizeof(idx), 1);
  js_setprop(js, entry, js_mkstr(js, idx, idx_len), held_value);
  idx_len = uint_to_str(idx, sizeof(idx), 2);
  js_setprop(js, entry, js_mkstr(js, idx, idx_len), unregister_token);
  js_setprop(js, entry, js->length_str, tov(3.0));
  
  idx_len = uint_to_str(idx, sizeof(idx), len);
  js_setprop(js, registrations, js_mkstr(js, idx, idx_len), entry);
  js_setprop(js, registrations, js->length_str, tov((double)(len + 1)));
  
  return js_mkundef();
}

static ant_value_t finreg_unregister(ant_params_t) {
  ant_value_t this_val = js->this_val;
  if (vtype(this_val) != kTypeObject) return js_false;
  
  if (nargs < 1 || !can_be_held_weakly(args[0]))
    return js_mkerr(js, "FinalizationRegistry.unregister token must be an object");
  
  ant_value_t token = args[0];
  ant_value_t registrations = js_get_slot(this_val, SLOT_MAP);
  if (vtype(registrations) != kTypeArray) return js_false;
  
  ant_offset_t len = js_arr_len(js, registrations);
  bool removed = false;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, registrations, i);
    if (vtype(entry) != kTypeArray) continue;
    ant_value_t entry_token = js_arr_get(js, entry, 2);
    if (entry_token == token) {
      char idx[16];
      size_t idx_len = uint_to_str(idx, sizeof(idx), i);
      js_setprop(js, registrations, js_mkstr(js, idx, idx_len), js_mkundef());
      removed = true;
    }
  }
  
  return js_bool(removed);
}

static ant_value_t map_group_add(ant_t *js, void *ctx, ant_value_t key, ant_value_t value) {
  map_entry_t **map_head = ctx;
  key = normalize_map_key(key);
  
  map_entry_t *entry = Ant_Map_Find(js, map_head, key);
  ant_value_t group;

  if (entry) group = entry->value; else {
    group = js_mkarr(js);
    if (!map_store_entry(js, map_head, key, key, group)) return Ant_Error_CreateOutOfMemory(js);
  }
  
  js_arr_push(js, group, value);
  return js_mkundef();
}

static ant_value_t map_groupBy(ant_params_t) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Map.groupBy requires 2 arguments");
  
  ant_value_t map_obj = Ant_Object_CreateBare(js);
  js_obj_ptr(map_obj)->type_tag = kTypeMap;
  
  ant_value_t map_proto = js->builtins.map_proto;
  if (is_special_object(map_proto)) js_set_proto_init(map_obj, map_proto);
  
  map_entry_t **map_head = collections_map_store_new();
  if (!map_head) return Ant_Error_CreateOutOfMemory(js);
  *map_head = NULL;
  js_set_native(map_obj, map_head, MAP_NATIVE_TAG);
  
  ant_value_t grouped = js_group_by(js, args[0], args[1], map_group_add, map_head);
  return is_err(grouped) ? grouped : map_obj;
}

static bool is_original_collection_adder(ant_value_t adder, ant_cfunc_t fn) {
  return vtype(adder) == kTypeBuiltin && js_cfunc_same_entrypoint(adder, fn);
}

static ant_value_t map_init_from_iterable(ant_t *js, ant_value_t map_obj, map_entry_t **map_head, ant_value_t iterable) {
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

  ant_value_t result = js_mkundef();
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
      ant_value_t key = normalize_map_key(js_arr_get(js, entry, 0));
      ant_value_t value = js_arr_get(js, entry, 1);
      if (!map_store_entry(js, map_head, key, key, value)) {
        result = Ant_Error_CreateOutOfMemory(js);
        goto close_iter;
      }
      continue;
    }
    
    ant_value_t call_args[2] = { js_arr_get(js, entry, 0), js_arr_get(js, entry, 1) };
    result = sv_vm_call(js->vm, js, adder, map_obj, call_args, 2, NULL, js_mkundef());
    if (is_err(result)) goto close_iter;
  }

  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : result;

close_iter:
  js_iter_close(js, &it);
  return result;
}

static ant_value_t set_init_from_iterable(ant_t *js, ant_value_t set_obj, set_entry_t **set_head, ant_value_t iterable) {
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

  ant_value_t result = js_mkundef();
  ant_value_t value;
  
  while (js_iter_next(js, &it, &value)) {
    if (use_fast_path) {
      if (!set_store_entry(js, set_head, value)) {
        result = Ant_Error_CreateOutOfMemory(js);
        goto close_iter;
      }
      continue;
    }
    
    result = sv_vm_call(js->vm, js, adder, set_obj, &value, 1, NULL, js_mkundef());
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

  ant_value_t result = js_mkundef();
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
      result = sv_vm_call(js->vm, js, adder, wm_obj, call_args, 2, NULL, js_mkundef());
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

  ant_value_t result = js_mkundef();
  ant_value_t value;
  
  while (js_iter_next(js, &it, &value)) {
    if (!use_fast_path) {
      result = sv_vm_call(js->vm, js, adder, ws_obj, &value, 1, NULL, js_mkundef());
      if (is_err(result)) goto close_iter;
      continue;
    }
    
    if (!can_be_held_weakly(value)) {
      result = js_mkerr(js, "WeakSet value must be an object");
      goto close_iter;
    }
    
    weakset_entry_t *entry;
    HASH_FIND(hh, *ws_head, &value, sizeof(ant_value_t), entry);
    if (entry) continue;
    
    entry = calloc(1, sizeof(weakset_entry_t));
    if (!entry) {
      result = Ant_Error_CreateOutOfMemory(js);
      goto close_iter;
    }
    
    entry->value_obj = value;
    HASH_ADD(hh, *ws_head, value_obj, sizeof(ant_value_t), entry);
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
  
  ant_value_t map_obj = js->this_val;
  if (vtype(map_obj) != kTypeObject) map_obj = Ant_Object_CreateBare(js);
  if (is_err(map_obj)) return map_obj;
  js_obj_ptr(map_obj)->type_tag = kTypeMap;
  
  ant_value_t map_proto = js->builtins.map_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, map_proto, call_new_target);
  
  if (is_special_object(instance_proto)) js_set_proto_init(map_obj, instance_proto);
  
  map_entry_t **map_head = collections_map_store_new();
  if (!map_head) return Ant_Error_CreateOutOfMemory(js);
  *map_head = NULL;
  
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin)
    js_set_slot(map_obj, SLOT_CTOR, call_new_target);
  js_set_native(map_obj, map_head, MAP_NATIVE_TAG);
  
  if (nargs == 0 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) return map_obj;
  ant_value_t init_result = map_init_from_iterable(js, map_obj, map_head, args[0]);
  if (is_err(init_result)) return init_result;
  
  return map_obj;
}

static ant_value_t builtin_Set(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set constructor requires 'new'");
  }
  
  ant_value_t set_obj = js->this_val;
  if (vtype(set_obj) != kTypeObject) set_obj = Ant_Object_CreateBare(js);
  if (is_err(set_obj)) return set_obj;
  js_obj_ptr(set_obj)->type_tag = kTypeSet;
  
  ant_value_t set_proto = js->builtins.set_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, set_proto, call_new_target);

  if (is_special_object(instance_proto)) js_set_proto_init(set_obj, instance_proto);
  
  set_entry_t **set_head = collections_set_store_new();
  if (!set_head) return Ant_Error_CreateOutOfMemory(js);
  *set_head = NULL;
  
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin)
    js_set_slot(set_obj, SLOT_CTOR, call_new_target);
  js_set_native(set_obj, set_head, SET_NATIVE_TAG);
  
  if (nargs == 0 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) return set_obj;
  ant_value_t init_result = set_init_from_iterable(js, set_obj, set_head, args[0]);
  if (is_err(init_result)) return init_result;
  
  return set_obj;
}

static ant_value_t builtin_WeakMap(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap constructor requires 'new'");
  }
  
  ant_value_t wm_obj = js->this_val;
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
  
  ant_value_t ws_obj = js->this_val;
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
  defmethod(js, finreg_proto, "register", 8, Ant_Function_CreateWithArity(finreg_register, 2));
  defmethod(js, finreg_proto, "unregister", 10, Ant_Function_CreateWithArity(finreg_unregister, 1));
  js_set_tostringtag(js, finreg_proto, js_mkstr(js, "FinalizationRegistry", 20));
  
  ant_value_t finreg_ctor = Ant_Function_CreateIntrinsicConstructor(js, builtin_FinalizationRegistry, finreg_proto, "FinalizationRegistry", 1);
  js_set_global_builtin(js, "FinalizationRegistry", finreg_ctor);
}
