#include "ant.h"
#include "gc.h"
#include "arena.h"
#include "internal.h" // IWYU pragma: keep

ant_handle_t js_root(ant_t *js, ant_value_t val) {
  if (js->gc_roots_len >= js->gc_roots_cap) {
    ant_handle_t new_cap = js->gc_roots_cap ? js->gc_roots_cap * 2 : GC_ROOTS_INITIAL_CAP;
    ant_value_t *new_roots = ant_realloc(js->gc_roots, new_cap * sizeof(ant_value_t));
    if (!new_roots) {
      fprintf(stderr, "FATAL: Out of memory allocating GC roots\n");
      abort();
    }
    js->gc_roots = new_roots;
    js->gc_roots_cap = new_cap;
  }
  ant_handle_t idx = js->gc_roots_len++;
  js->gc_roots[idx] = val;
  return idx;
}

ant_value_t js_deref(ant_t *js, ant_handle_t h) {
  if (h < 0 || h >= js->gc_roots_len) return js_mkundef();
  return js->gc_roots[h];
}

void js_unroot(ant_t *js, ant_handle_t h) {
  if (h >= 0 && h == js->gc_roots_len - 1) js->gc_roots_len--;
}

void js_root_update(ant_t *js, ant_handle_t h, ant_value_t val) {
  if (h >= 0 && h < js->gc_roots_len) js->gc_roots[h] = val;
}