#include "gc/weak.h"
#include "gc/objects.h"

#include "internal.h"
#include "ptr.h"
#include "modules/collections.h"

#include <stdlib.h>
#include <string.h>
#include <uthash.h>

typedef struct gc_weak_pending_pair {
  ant_value_t key;
  ant_value_t value;
} gc_weak_pending_pair_t;

typedef struct gc_weak_pending_index_entry {
  ant_value_t key;
  uint32_t head;
} gc_weak_pending_index_entry_t;

typedef struct gc_weak_pending_table {
  gc_weak_pending_pair_t *pairs;
  uint32_t *next;
  gc_weak_pending_index_entry_t *index;
  uint32_t pair_count;
  uint32_t pair_capacity;
  uint32_t next_capacity;
  uint32_t index_capacity;
  uint32_t index_count;
  uint32_t index_tombstones;
} gc_weak_pending_table_t;

void gc_weak_register(ant_t *js, ant_object_t *obj) {
  if (!js || !obj || js->weak_gc.registry_overflow) return;
  if (js->weak_gc.collection_len == js->weak_gc.collection_cap) {
    size_t next_cap = js->weak_gc.collection_cap
      ? js->weak_gc.collection_cap * 2
      : 32;
    ant_object_t **grown = realloc(
      js->weak_gc.collections, 
      next_cap * sizeof(*grown)
    );
    if (!grown) {
      js->weak_gc.registry_overflow = true;
      return;
    }
    js->weak_gc.collections = grown;
    js->weak_gc.collection_cap = next_cap;
  }
  js->weak_gc.collections[js->weak_gc.collection_len++] = obj;
}

static void gc_weak_remember_edge(
  ant_t *js, ant_object_t *obj, ant_value_t key,
  ant_value_t value, uint8_t kind
) {
  if (!js || !obj || obj->flags.generation == 0 ||
      js->weak_gc.minor_edge_overflow) return;
  if (js->weak_gc.minor_edge_len == js->weak_gc.minor_edge_cap) {
    size_t next_cap = js->weak_gc.minor_edge_cap
      ? js->weak_gc.minor_edge_cap * 2
      : 64;
    void *grown = realloc(
      js->weak_gc.minor_edges,
      next_cap * sizeof(*js->weak_gc.minor_edges)
    );
    if (!grown) {
      js->weak_gc.minor_edge_overflow = true;
      return;
    }
    js->weak_gc.minor_edges = grown;
    js->weak_gc.minor_edge_cap = next_cap;
  }
  size_t index = js->weak_gc.minor_edge_len++;
  js->weak_gc.minor_edges[index].owner = obj;
  js->weak_gc.minor_edges[index].key = key;
  js->weak_gc.minor_edges[index].value = value;
  js->weak_gc.minor_edges[index].kind = kind;
}

void gc_weak_remember_map(
  ant_t *js, ant_object_t *obj, ant_value_t key, ant_value_t value
) {
  gc_weak_remember_edge(js, obj, key, value, T_WEAKMAP);
}

void gc_weak_remember_set(ant_t *js, ant_object_t *obj, ant_value_t key) {
  gc_weak_remember_edge(js, obj, key, js_mkundef(), T_WEAKSET);
}

void gc_weak_keep_alive(ant_t *js, ant_value_t value) {
  if (!js || !is_tagged(value)) return;
  
  if (
    js->weak_gc.kept_alive_len &&
    js->weak_gc.kept_alive[js->weak_gc.kept_alive_len - 1] == value
  ) return;

  if (js->weak_gc.kept_alive_len == js->weak_gc.kept_alive_cap) {
    size_t next_cap = js->weak_gc.kept_alive_cap
      ? js->weak_gc.kept_alive_cap * 2
      : 16;
      
    ant_value_t *grown = realloc(
      js->weak_gc.kept_alive, 
      next_cap * sizeof(*grown)
    );
    
    if (!grown) {
      js->weak_gc.kept_alive_overflow = true;
      return;
    }
    
    js->weak_gc.kept_alive = grown;
    js->weak_gc.kept_alive_cap = next_cap;
  }
  
  js->weak_gc.kept_alive[js->weak_gc.kept_alive_len++] = value;
}

void gc_weak_clear_kept_alive(ant_t *js) {
  if (!js) return;
  js->weak_gc.kept_alive_len = 0;
  js->weak_gc.kept_alive_overflow = false;
}

static void gc_weak_mark_all_weakrefs(ant_t *js, ant_object_t *objects, gc_weak_mark_fn mark) {
for (ant_object_t *obj = objects; obj; obj = obj->next) {
  weakref_state_t *state = js_get_native(js_obj_from_ptr(obj),  WEAKREF_NATIVE_TAG);
  if (!state) continue;
  mark(js, js_obj_from_ptr(obj));
  mark(js, state->target);
}}

void gc_weak_mark_kept_alive(ant_t *js, gc_weak_mark_fn mark) {
  if (!js || !mark) return;
  for (size_t i = 0; i < js->weak_gc.kept_alive_len; i++)
    mark(js, js->weak_gc.kept_alive[i]);

  if (__builtin_expect(js->weak_gc.kept_alive_overflow, 0)) {
    gc_weak_mark_all_weakrefs(js, js->objects, mark);
    gc_weak_mark_all_weakrefs(js, js->objects_old, mark);
    gc_weak_mark_all_weakrefs(js, js->permanent_objects, mark);
  }
}

static void gc_weak_clear_pending(ant_t *js) {
  gc_weak_pending_table_t *table = js ? js->weak_gc.pending : NULL;
  if (!table) return;
  table->pair_count = 0;
  if (table->index)
    memset(table->index, 0, table->index_capacity * sizeof(*table->index));
  table->index_count = 0;
  table->index_tombstones = 0;
}

void gc_weak_cleanup(ant_t *js) {
  if (!js) return;
  gc_weak_pending_table_t *pending = js->weak_gc.pending;
  if (pending) {
    free(pending->pairs);
    free(pending->next);
    free(pending->index);
    free(pending);
  }
  free(js->weak_gc.collections);
  free(js->weak_gc.kept_alive);
  free(js->weak_gc.minor_edges);
  js->weak_gc = (typeof(js->weak_gc)){0};
}

static bool gc_weak_is_collection(const ant_object_t *obj) {
  return obj && (obj->type_tag == T_WEAKMAP ||
    obj->type_tag == T_WEAKSET || obj->native.tag == WEAKREF_NATIVE_TAG);
}

static uint32_t gc_weak_pending_index_find_slot(
  gc_weak_pending_table_t *table, ant_value_t key,
  uint32_t hash, bool *found
) {
  uint32_t mask = table->index_capacity - 1;
  uint32_t slot = hash & mask;
  uint32_t first_tombstone = UINT32_MAX;

  for (;;) {
    ant_value_t stored_key = table->index[slot].key;
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

static bool gc_weak_pending_rehash(
  gc_weak_pending_table_t *table, uint32_t new_capacity
) {
  gc_weak_pending_index_entry_t *new_index = calloc(
    new_capacity, sizeof(*new_index)
  );
  if (!new_index) return false;

  gc_weak_pending_index_entry_t *old_index = table->index;
  uint32_t old_capacity = table->index_capacity;
  table->index = new_index;
  table->index_capacity = new_capacity;
  table->index_count = 0;
  table->index_tombstones = 0;

  for (uint32_t i = 0; i < old_capacity; i++) {
    gc_weak_pending_index_entry_t entry = old_index[i];
    if (entry.key == 0 || entry.key == 1) continue;
    bool found = false;
    uint32_t slot = gc_weak_pending_index_find_slot(
      table, entry.key, weak_collection_key_hash(entry.key), &found
    );
    table->index[slot] = entry;
    table->index_count++;
  }

  free(old_index);
  return true;
}

static gc_weak_pending_table_t *gc_weak_pending_get(ant_t *js) {
  gc_weak_pending_table_t *table = js->weak_gc.pending;
  if (table) return table;
  table = calloc(1, sizeof(*table));
  if (table) js->weak_gc.pending = table;
  return table;
}

static bool gc_weak_pending_ensure_next(
  gc_weak_pending_table_t *table, uint32_t capacity
) {
  if (capacity <= table->next_capacity) return true;
  if ((size_t)capacity > SIZE_MAX / sizeof(*table->next)) return false;
  uint32_t *next = realloc(table->next, (size_t)capacity * sizeof(*next));
  if (!next) return false;
  table->next = next;
  table->next_capacity = capacity;
  return true;
}

static bool gc_weak_pending_index_add(
  gc_weak_pending_table_t *table, uint32_t pair_index
) {
  ant_value_t key = table->pairs[pair_index].key;
  if (table->index_capacity == 0 && !gc_weak_pending_rehash(table, 64))
    return false;

  uint32_t hash = weak_collection_key_hash(key);
  bool found = false;
  uint32_t slot = gc_weak_pending_index_find_slot(
    table, key, hash, &found
  );
  if (!found) {
    uint64_t used = (uint64_t)table->index_count +
      table->index_tombstones + 1;
    if (used * 4 >= (uint64_t)table->index_capacity * 3) {
      if (table->index_capacity > UINT32_MAX / 2) return false;
      if (!gc_weak_pending_rehash(
        table, table->index_capacity * 2
      )) return false;
      slot = gc_weak_pending_index_find_slot(
        table, key, hash, &found
      );
    }

    if (table->index[slot].key == 1) table->index_tombstones--;
    table->index[slot].key = key;
    table->index[slot].head = 0;
    table->index_count++;
  }

  table->next[pair_index] = table->index[slot].head;
  table->index[slot].head = pair_index + 1;
  return true;
}

static void gc_weak_add_pending(
  ant_t *js, ant_value_t key, ant_value_t value
) {
  gc_weak_pending_table_t *table = gc_weak_pending_get(js);
  if (!table) {
    js->weak_gc.pending_oom = true;
    return;
  }

  if (table->pair_count == table->pair_capacity) {
    uint32_t new_capacity = table->pair_capacity
      ? table->pair_capacity * 2
      : 64;
    if (new_capacity < table->pair_capacity) {
      js->weak_gc.pending_oom = true;
      return;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*table->pairs)) {
      js->weak_gc.pending_oom = true;
      return;
    }
    gc_weak_pending_pair_t *pairs = realloc(
      table->pairs, (size_t)new_capacity * sizeof(*pairs)
    );
    if (!pairs) {
      js->weak_gc.pending_oom = true;
      return;
    }
    table->pairs = pairs;
    table->pair_capacity = new_capacity;
  }

  uint32_t pair_index = table->pair_count++;
  table->pairs[pair_index] = (gc_weak_pending_pair_t){
    .key = key,
    .value = value
  };

  if (js->weak_gc.pending_active &&
      (!gc_weak_pending_ensure_next(table, table->pair_capacity) ||
       !gc_weak_pending_index_add(table, pair_index)))
    js->weak_gc.pending_oom = true;
}

void gc_weak_key_marked(ant_t *js, ant_value_t key) {
  if (!js || !js->weak_gc.pending_active || !js->weak_gc.mark) return;
  gc_weak_pending_table_t *table = js->weak_gc.pending;
  if (!table || table->index_capacity == 0) return;

  bool found = false;
  uint32_t slot = gc_weak_pending_index_find_slot(
    table, key, weak_collection_key_hash(key), &found
  );
  if (!found) return;

  gc_weak_pending_index_entry_t *entry = &table->index[slot];
  uint32_t next = entry->head;
  entry->key = 1;
  entry->head = 0;
  table->index_count--;
  table->index_tombstones++;

  while (next) {
    uint32_t pair_index = next - 1;
    next = table->next[pair_index];
    js->weak_gc.mark(js, table->pairs[pair_index].value);
  }
}

static bool gc_weak_prepare_pending(ant_t *js) {
  gc_weak_pending_table_t *table = js->weak_gc.pending;
  if (!table || table->pair_count == 0) return true;

  bool has_live_key = false;
  for (uint32_t i = 0; i < table->pair_count; i++) {
    if (js->weak_gc.key_alive(js, table->pairs[i].key)) {
      has_live_key = true;
      break;
    }
  }
  if (!has_live_key) return true;

  if (!gc_weak_pending_ensure_next(table, table->pair_capacity))
    return false;
  for (uint32_t i = 0; i < table->pair_count; i++)
    if (!gc_weak_pending_index_add(table, i)) return false;

  js->weak_gc.pending_active = true;
  for (uint32_t i = 0; i < table->pair_count; i++) {
    ant_value_t key = table->pairs[i].key;
    if (js->weak_gc.key_alive(js, key)) gc_weak_key_marked(js, key);
  }
  return true;
}

static void gc_weak_process_map(ant_t *js, ant_object_t *obj) {
  if (!obj || obj->type_tag != T_WEAKMAP ||
      !js->weak_gc.mark || !js->weak_gc.key_alive) return;
  weakmap_table_t *table = js_get_native(
    js_obj_from_ptr(obj), WEAKMAP_NATIVE_TAG
  );
  if (!table) return;
  for (uint32_t i = 0; i < table->capacity; i++) {
    weakmap_entry_t *entry = &table->entries[i];
    if (!weakmap_entry_is_occupied(entry)) continue;
    if (js->weak_gc.pending_oom) {
      js->weak_gc.mark(js, entry->key_obj);
      js->weak_gc.mark(js, entry->value);
      continue;
    }
    if (js->weak_gc.key_alive(js, entry->key_obj))
      js->weak_gc.mark(js, entry->value);
    else gc_weak_add_pending(
      js, entry->key_obj, entry->value
    );
  }
}

void gc_weak_collection_marked(ant_t *js, ant_object_t *obj) {
  if (js && js->weak_gc.mark && obj->type_tag == T_WEAKMAP)
    gc_weak_process_map(js, obj);
}

static void gc_weak_mark_all_in_collection(ant_t *js, ant_object_t *obj) {
  ant_value_t collection = js_obj_from_ptr(obj);
  weakref_state_t *weakref = js_get_native(collection, WEAKREF_NATIVE_TAG);
  if (weakref) js->weak_gc.mark(js, weakref->target);

  if (obj->type_tag == T_WEAKMAP) {
    weakmap_table_t *table = js_get_native(collection, WEAKMAP_NATIVE_TAG);
    if (!table) return;
    for (uint32_t i = 0; i < table->capacity; i++) {
      weakmap_entry_t *entry = &table->entries[i];
      if (!weakmap_entry_is_occupied(entry)) continue;
      js->weak_gc.mark(js, entry->key_obj);
      js->weak_gc.mark(js, entry->value);
    }
  } else if (obj->type_tag == T_WEAKSET) {
    weakset_entry_t **head = js_get_native(collection, WEAKSET_NATIVE_TAG);
    if (!head) return;
    weakset_entry_t *entry, *tmp;
    HASH_ITER(hh, *head, entry, tmp)
      js->weak_gc.mark(js, entry->value_obj);
  }
}

static void gc_weak_prune_collection(ant_t *js, ant_object_t *obj) {
  ant_value_t collection = js_obj_from_ptr(obj);
  weakref_state_t *weakref = js_get_native(collection, WEAKREF_NATIVE_TAG);
  if (weakref && !js->weak_gc.key_alive(js, weakref->target))
    weakref->target = js_mkundef();

  if (obj->type_tag == T_WEAKMAP) {
    weakmap_table_t *table = js_get_native(collection, WEAKMAP_NATIVE_TAG);
    if (!table) return;
    for (uint32_t i = 0; i < table->capacity; i++) {
      weakmap_entry_t *entry = &table->entries[i];
      if (!weakmap_entry_is_occupied(entry)) continue;
      if (js->weak_gc.key_alive(js, entry->key_obj)) continue;
      entry->key_obj = 1;
      entry->value = 0;
      table->count--;
      table->tombstones++;
    }
    weakmap_table_finish_prune(table);
  } else if (obj->type_tag == T_WEAKSET) {
    weakset_entry_t **head = js_get_native(collection, WEAKSET_NATIVE_TAG);
    if (!head) return;
    weakset_entry_t *entry, *tmp;
    HASH_ITER(hh, *head, entry, tmp) {
      if (js->weak_gc.key_alive(js, entry->value_obj)) continue;
      HASH_DEL(*head, entry);
      free(entry);
    }
  }
}

static void gc_weak_process_minor_edges(ant_t *js) {
  for (size_t i = 0; i < js->weak_gc.minor_edge_len; i++) {
    typeof(*js->weak_gc.minor_edges) *edge = &js->weak_gc.minor_edges[i];
    if (!js->weak_gc.key_alive || !edge->owner ||
        edge->owner->mark_epoch == ANT_GC_DEAD) continue;
    if (edge->kind == T_WEAKMAP) {
      if (js->weak_gc.key_alive(js, edge->key))
        js->weak_gc.mark(js, edge->value);
      else {
        gc_weak_add_pending(js, edge->key, edge->value);
      }
    }
  }
}

static void gc_weak_prune_minor_edges(ant_t *js) {
  for (size_t i = 0; i < js->weak_gc.minor_edge_len; i++) {
    typeof(*js->weak_gc.minor_edges) *edge = &js->weak_gc.minor_edges[i];
    ant_object_t *obj = edge->owner;
    if (!obj || obj->mark_epoch == ANT_GC_DEAD ||
        js->weak_gc.key_alive(js, edge->key)) continue;
    ant_value_t collection = js_obj_from_ptr(obj);
    if (edge->kind == T_WEAKMAP) {
      weakmap_table_t *table = js_get_native(
        collection, WEAKMAP_NATIVE_TAG
      );
      if (table && weakmap_table_delete(table, edge->key))
        table->gc_prune_pending = true;
    } else if (edge->kind == T_WEAKSET) {
      weakset_entry_t **head = js_get_native(collection, WEAKSET_NATIVE_TAG);
      weakset_entry_t *entry = NULL;
      if (head) HASH_FIND(hh, *head, &edge->key, sizeof(edge->key), entry);
      if (entry) {
        HASH_DEL(*head, entry);
        free(entry);
      }
    }
  }

  for (size_t i = 0; i < js->weak_gc.minor_edge_len; i++) {
    typeof(*js->weak_gc.minor_edges) *edge = &js->weak_gc.minor_edges[i];
    ant_object_t *obj = edge->owner;
    if (edge->kind != T_WEAKMAP || !obj ||
        obj->mark_epoch == ANT_GC_DEAD) continue;
    weakmap_table_t *table = js_get_native(
      js_obj_from_ptr(obj), WEAKMAP_NATIVE_TAG
    );
    if (table && table->gc_prune_pending)
      weakmap_table_finish_prune(table);
  }
}

static void gc_weak_mark_all_in_list(
  ant_t *js, ant_object_t *objects, gc_weak_collection_live_fn live
) {
  for (ant_object_t *obj = objects; obj; obj = obj->next)
    if (live(obj) && gc_weak_is_collection(obj))
      gc_weak_mark_all_in_collection(js, obj);
}

static void gc_weak_rebuild_registry_from_list(
  ant_t *js, ant_object_t *objects, gc_weak_collection_live_fn live
) {
  for (ant_object_t *obj = objects; obj; obj = obj->next)
    if (live(obj) && gc_weak_is_collection(obj)) gc_weak_register(js, obj);
}

static void gc_weak_rebuild_registry(
  ant_t *js, gc_weak_collection_live_fn live
) {
  js->weak_gc.collection_len = 0;
  js->weak_gc.registry_overflow = false;
  gc_weak_rebuild_registry_from_list(js, js->objects, live);
  gc_weak_rebuild_registry_from_list(js, js->objects_old, live);
  gc_weak_rebuild_registry_from_list(js, js->permanent_objects, live);
}

void gc_weak_process(
  ant_t *js, bool minor, gc_weak_mark_fn mark, gc_weak_drain_fn drain,
  gc_weak_key_alive_fn key_alive,
  gc_weak_collection_live_fn collection_live
) {
  js->weak_gc.mark = mark;
  js->weak_gc.key_alive = key_alive;

  if (js->weak_gc.registry_overflow ||
      (minor && js->weak_gc.minor_edge_overflow)) {
    gc_weak_mark_all_in_list(js, js->objects, collection_live);
    gc_weak_mark_all_in_list(js, js->objects_old, collection_live);
    gc_weak_mark_all_in_list(js, js->permanent_objects, collection_live);
    drain(js);
    gc_weak_rebuild_registry(js, collection_live);
    js->weak_gc.minor_edge_len = 0;
    js->weak_gc.minor_edge_overflow = false;
    goto done;
  }

  js->weak_gc.pending_active = false;
  js->weak_gc.pending_oom = false;
  for (size_t i = 0; i < js->weak_gc.collection_len; i++) {
    ant_object_t *obj = js->weak_gc.collections[i];
    if (collection_live(obj) && obj->type_tag == T_WEAKMAP &&
        (!minor || obj->flags.generation == 0))
      gc_weak_process_map(js, obj);
  }
  if (minor) gc_weak_process_minor_edges(js);
  drain(js);

  if (!js->weak_gc.pending_oom && !gc_weak_prepare_pending(js))
    js->weak_gc.pending_oom = true;
  if (!js->weak_gc.pending_oom && js->weak_gc.pending_active)
    drain(js);

  if (js->weak_gc.pending_oom) {
    gc_weak_clear_pending(js);
    js->weak_gc.pending_active = false;
    for (size_t i = 0; i < js->weak_gc.collection_len; i++) {
      ant_object_t *obj = js->weak_gc.collections[i];
      if (collection_live(obj)) gc_weak_mark_all_in_collection(js, obj);
    }
    drain(js);
  } else {
    gc_weak_clear_pending(js);
    js->weak_gc.pending_active = false;
    for (size_t i = 0; i < js->weak_gc.collection_len; i++) {
      ant_object_t *obj = js->weak_gc.collections[i];
      if (collection_live(obj) && (!minor || obj->flags.generation == 0))
        gc_weak_prune_collection(js, obj);
    }
    if (minor) gc_weak_prune_minor_edges(js);
  }

  size_t live = 0;
  for (size_t i = 0; i < js->weak_gc.collection_len; i++) {
    ant_object_t *obj = js->weak_gc.collections[i];
    if (collection_live(obj)) {
      js->weak_gc.collections[live++] = obj;
    }
  }
  js->weak_gc.collection_len = live;
  js->weak_gc.minor_edge_len = 0;
  js->weak_gc.minor_edge_overflow = false;

done:
  gc_weak_clear_pending(js);
  js->weak_gc.pending_active = false;
  js->weak_gc.mark = NULL;
  js->weak_gc.key_alive = NULL;
}
