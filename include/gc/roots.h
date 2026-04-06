#ifndef ANT_GC_ROOTS_H
#define ANT_GC_ROOTS_H

#include "types.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#define GC_MAX_STATIC_ROOTS 128

typedef struct gc_temp_root_scope {
  ant_t *js;
  ant_value_t *items;
  size_t len;
  size_t cap;
  struct gc_temp_root_scope *prev;
} gc_temp_root_scope_t;

typedef struct {
  gc_temp_root_scope_t *scope;
  size_t index;
} gc_temp_root_handle_t;

typedef void (*gc_root_visitor_t)(
  ant_t *js,
  ant_value_t v
);

size_t gc_root_scope(ant_t *js);

bool gc_push_root(ant_t *js, ant_value_t *slot);
bool gc_temp_root_set(gc_temp_root_handle_t handle, ant_value_t value);

void gc_register_root(ant_value_t *slot);
void gc_pop_roots(ant_t *js, size_t mark);
void gc_visit_roots(ant_t *js, gc_root_visitor_t visitor);
void gc_temp_root_scope_begin(ant_t *js, gc_temp_root_scope_t *scope);
void gc_temp_root_scope_end(gc_temp_root_scope_t *scope);

ant_value_t gc_temp_root_get(gc_temp_root_handle_t handle);
gc_temp_root_handle_t gc_temp_root_add(gc_temp_root_scope_t *scope, ant_value_t value);

#define GC_ROOT_PIN(js, slot) do {                 \
  bool _gc_root_ok = gc_push_root((js), &(slot));  \
  assert(_gc_root_ok && "gc_push_root failed");    \
  (void)_gc_root_ok;                               \
} while (0)

#define GC_ROOT_SAVE(name, js)    size_t name = gc_root_scope((js))
#define GC_ROOT_RESTORE(js, mark) gc_pop_roots((js), (mark))

static inline bool gc_temp_root_handle_valid(gc_temp_root_handle_t handle) {
  return handle.scope != NULL;
}

#endif
