#include "gc/roots.h"
#include "internal.h"

#include <stddef.h>
#include <stdlib.h>

static ant_value_t *g_roots[GC_MAX_STATIC_ROOTS];
static size_t g_root_count = 0;

void gc_register_root(ant_value_t *slot) {
  if (g_root_count < GC_MAX_STATIC_ROOTS)
    g_roots[g_root_count++] = slot;
}

size_t gc_root_scope(ant_t *js) {
  return js ? js->c_root_count : 0;
}

bool gc_push_root(ant_t *js, ant_value_t *slot) {
  if (!js || !slot) return false;

  if (js->c_root_count >= js->c_root_cap) {
    size_t new_cap = js->c_root_cap ? js->c_root_cap * 2 : 64;
    ant_value_t **next = realloc(js->c_roots, new_cap * sizeof(*next));
    if (!next) return false;
    js->c_roots = next;
    js->c_root_cap = new_cap;
  }

  js->c_roots[js->c_root_count++] = slot;
  return true;
}

void gc_pop_roots(ant_t *js, size_t mark) {
  if (!js) return;
  js->c_root_count = (mark <= js->c_root_count) ? mark : 0;
}

void gc_visit_roots(ant_t *js, gc_root_visitor_t visitor) {
  for (size_t i = 0; i < g_root_count; i++) {
    if (g_roots[i] && *g_roots[i]) visitor(js, *g_roots[i]);
  }

  if (!js) return;
  for (size_t i = 0; i < js->c_root_count; i++) {
    ant_value_t *slot = js->c_roots[i];
    if (slot && *slot) visitor(js, *slot);
  }
}
