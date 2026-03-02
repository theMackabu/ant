#include "ant.h"
#include "gc.h"
#include "arena.h"
#include "internal.h" // IWYU pragma: keep

jshdl_t js_root(ant_t *js, jsval_t val) {
  if (js->gc_roots_len >= js->gc_roots_cap) {
    jshdl_t new_cap = js->gc_roots_cap ? js->gc_roots_cap * 2 : GC_ROOTS_INITIAL_CAP;
    jsval_t *new_roots = ant_realloc(js->gc_roots, new_cap * sizeof(jsval_t));
    if (!new_roots) {
      fprintf(stderr, "FATAL: Out of memory allocating GC roots\n");
      abort();
    }
    js->gc_roots = new_roots;
    js->gc_roots_cap = new_cap;
  }
  jshdl_t idx = js->gc_roots_len++;
  js->gc_roots[idx] = val;
  return idx;
}

jsval_t js_deref(ant_t *js, jshdl_t h) {
  if (h < 0 || h >= js->gc_roots_len) return js_mkundef();
  return js->gc_roots[h];
}

void js_unroot(ant_t *js, jshdl_t h) {
  if (h >= 0 && h == js->gc_roots_len - 1) js->gc_roots_len--;
}

void js_root_update(ant_t *js, jshdl_t h, jsval_t val) {
  if (h >= 0 && h < js->gc_roots_len) js->gc_roots[h] = val;
}