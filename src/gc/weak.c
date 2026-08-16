#include "gc/weak.h"
#include "gc/objects.h"

#include "internal.h"
#include "ptr.h"
#include "modules/collections.h"

#include <stdlib.h>
#include <uthash.h>

typedef struct gc_weak_pending_entry {
  ant_value_t key;
  ant_value_t value;
  UT_hash_handle hh;
} gc_weak_pending_entry_t;

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
  if (!js || value <= NANBOX_PREFIX) return;
  
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
  gc_weak_pending_entry_t *head = js ? js->weak_gc.pending : NULL;
  gc_weak_pending_entry_t *entry, *tmp;
  HASH_ITER(hh, head, entry, tmp) {
    HASH_DEL(head, entry);
    free(entry);
  }
  if (js) js->weak_gc.pending = NULL;
}

void gc_weak_cleanup(ant_t *js) {
  if (!js) return;
  gc_weak_clear_pending(js);
  free(js->weak_gc.collections);
  free(js->weak_gc.kept_alive);
  free(js->weak_gc.minor_edges);
  js->weak_gc = (typeof(js->weak_gc)){0};
}

static bool gc_weak_is_collection(const ant_object_t *obj) {
  return obj && (obj->type_tag == T_WEAKMAP ||
    obj->type_tag == T_WEAKSET || obj->native.tag == WEAKREF_NATIVE_TAG);
}

static void gc_weak_add_pending(
  ant_t *js, ant_value_t key, ant_value_t value
) {
  gc_weak_pending_entry_t *entry = malloc(sizeof(*entry));
  if (!entry) {
    js->weak_gc.pending_oom = true;
    return;
  }
  entry->key = key;
  entry->value = value;
  gc_weak_pending_entry_t *head = js->weak_gc.pending;
  HASH_ADD(hh, head, key, sizeof(key), entry);
  js->weak_gc.pending = head;
}

void gc_weak_key_marked(ant_t *js, ant_value_t key) {
  if (!js || !js->weak_gc.pending_active || !js->weak_gc.mark) return;
  gc_weak_pending_entry_t *head = js->weak_gc.pending;
  gc_weak_pending_entry_t *entry = NULL;
  do {
    HASH_FIND(hh, head, &key, sizeof(key), entry);
    if (!entry) break;
    ant_value_t value = entry->value;
    HASH_DEL(head, entry);
    free(entry);
    js->weak_gc.pending = head;
    js->weak_gc.mark(js, value);
    head = js->weak_gc.pending;
  } while (true);
}

static void gc_weak_process_map(ant_t *js, ant_object_t *obj) {
  if (!obj || obj->type_tag != T_WEAKMAP ||
      !js->weak_gc.mark || !js->weak_gc.key_alive) return;
  weakmap_entry_t **head = js_get_native(
    js_obj_from_ptr(obj), WEAKMAP_NATIVE_TAG
  );
  if (!head) return;
  weakmap_entry_t *entry, *tmp;
  HASH_ITER(hh, *head, entry, tmp) {
    if (js->weak_gc.key_alive(js, entry->key_obj))
      js->weak_gc.mark(js, entry->value);
    else gc_weak_add_pending(js, entry->key_obj, entry->value);
  }
}

void gc_weak_collection_marked(ant_t *js, ant_object_t *obj) {
  if (js && js->weak_gc.pending_active && obj->type_tag == T_WEAKMAP)
    gc_weak_process_map(js, obj);
}

static void gc_weak_mark_all_in_collection(ant_t *js, ant_object_t *obj) {
  ant_value_t collection = js_obj_from_ptr(obj);
  weakref_state_t *weakref = js_get_native(collection, WEAKREF_NATIVE_TAG);
  if (weakref) js->weak_gc.mark(js, weakref->target);

  if (obj->type_tag == T_WEAKMAP) {
    weakmap_entry_t **head = js_get_native(collection, WEAKMAP_NATIVE_TAG);
    if (!head) return;
    weakmap_entry_t *entry, *tmp;
    HASH_ITER(hh, *head, entry, tmp) {
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
    weakmap_entry_t **head = js_get_native(collection, WEAKMAP_NATIVE_TAG);
    if (!head) return;
    weakmap_entry_t *entry, *tmp;
    HASH_ITER(hh, *head, entry, tmp) {
      if (js->weak_gc.key_alive(js, entry->key_obj)) continue;
      HASH_DEL(*head, entry);
      free(entry);
    }
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
      else
        gc_weak_add_pending(js, edge->key, edge->value);
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
      weakmap_entry_t **head = js_get_native(collection, WEAKMAP_NATIVE_TAG);
      weakmap_entry_t *entry = NULL;
      if (head) HASH_FIND(hh, *head, &edge->key, sizeof(edge->key), entry);
      if (entry) {
        HASH_DEL(*head, entry);
        free(entry);
      }
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

  js->weak_gc.pending_active = true;
  js->weak_gc.pending_oom = false;
  for (size_t i = 0; i < js->weak_gc.collection_len; i++) {
    ant_object_t *obj = js->weak_gc.collections[i];
    if (collection_live(obj) && obj->type_tag == T_WEAKMAP &&
        (!minor || obj->flags.generation == 0))
      gc_weak_process_map(js, obj);
  }
  if (minor) gc_weak_process_minor_edges(js);
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
