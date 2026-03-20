#ifndef ANT_GC_ROOTS_H
#define ANT_GC_ROOTS_H

#include "types.h"
#include <assert.h>
#include <stdbool.h>

#define GC_MAX_STATIC_ROOTS 64

typedef void (*gc_root_visitor_t)(ant_t *js, ant_value_t v);

size_t gc_root_scope(ant_t *js);
bool gc_push_root(ant_t *js, ant_value_t *slot);

void gc_register_root(ant_value_t *slot);
void gc_pop_roots(ant_t *js, size_t mark);
void gc_visit_roots(ant_t *js, gc_root_visitor_t visitor);

#define GC_ROOT_PIN(js, slot) do {                 \
  bool _gc_root_ok = gc_push_root((js), &(slot));  \
  assert(_gc_root_ok && "gc_push_root failed");    \
  (void)_gc_root_ok;                               \
} while (0)

#define GC_ROOT_SAVE(name, js)    size_t name = gc_root_scope((js))
#define GC_ROOT_RESTORE(js, mark) gc_pop_roots((js), (mark))

#endif
