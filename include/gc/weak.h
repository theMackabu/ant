#ifndef ANT_GC_WEAK_H
#define ANT_GC_WEAK_H

#include "types.h"

typedef void (*gc_weak_drain_fn)(ant_t *js);
typedef void (*gc_weak_mark_fn)(ant_t *js, ant_value_t value);
typedef bool (*gc_weak_key_alive_fn)(ant_t *js, ant_value_t key);
typedef bool (*gc_weak_collection_live_fn)(const ant_object_t *obj);

bool js_symbol_gc_mark(ant_value_t sym, uint64_t epoch);
bool js_symbol_gc_is_marked(ant_value_t sym, uint64_t epoch);
bool js_symbol_gc_is_permanent(ant_value_t sym);

void gc_weak_cleanup(ant_t *js);
void gc_weak_register(ant_t *js, ant_object_t *obj);
void gc_weak_remember_set(ant_t *js, ant_object_t *obj, ant_value_t key);
void gc_weak_keep_alive(ant_t *js, ant_value_t value);
void gc_weak_clear_kept_alive(ant_t *js);
void gc_weak_mark_kept_alive(ant_t *js, gc_weak_mark_fn mark);
void gc_weak_key_marked(ant_t *js, ant_value_t key);
void gc_weak_collection_marked(ant_t *js, ant_object_t *obj);

void gc_weak_remember_map(
  ant_t *js, ant_object_t *obj, 
  ant_value_t key, ant_value_t value
);

void gc_weak_process(
  ant_t *js, bool minor, gc_weak_mark_fn mark, 
  gc_weak_drain_fn drain, gc_weak_key_alive_fn key_alive,
  gc_weak_collection_live_fn collection_live
);

#endif
