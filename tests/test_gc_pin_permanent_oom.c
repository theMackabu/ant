// meson test -C build gc-pin-permanent-oom
//
// gc_pin_permanent flags the object and stores it in the permanent-root
// vector. Only the vector makes the collector trace it; the flag just dedupes
// pins and reports the object live to weak collections. If the vector cannot
// grow, the object must stay unflagged: a flagged object that was never
// stored makes the retry return early, so the next sweep frees it while
// WeakRefs and WeakMaps still report it alive.
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, NULL);

  GC_ROOT_SAVE(roots, js);
  ant_value_t parent = js_mkobj(js);
  GC_ROOT_PIN(js, parent);
  ant_value_t child = js_mkobj(js);
  js_set(js, parent, "child", child);

  // A full vector this long asks realloc for 2^63 bytes, which cannot succeed.
  size_t len = js->permanent_root_len, cap = js->permanent_root_cap;
  js->permanent_root_len = js->permanent_root_cap = (size_t)1 << 59;
  bool pinned = gc_pin_permanent(js, parent);
  size_t len_after_failure = js->permanent_root_len;
  js->permanent_root_len = len;
  js->permanent_root_cap = cap;

  assert(!pinned);
  assert(len_after_failure == (size_t)1 << 59);
  assert(!js_obj_ptr(parent)->flags.gc_permanent);

  assert(gc_pin_permanent(js, parent));
  assert(js_obj_ptr(parent)->flags.gc_permanent);
  assert(js->permanent_root_len == len + 1);
  assert(js->permanent_roots[len] == js_obj_ptr(parent));

  // pinning twice must not store a second entry
  assert(gc_pin_permanent(js, parent));
  assert(js->permanent_root_len == len + 1);

  // the pin alone must now keep the parent traced, and so its child alive
  GC_ROOT_RESTORE(js, roots);
  gc_run(js);
  assert(gc_obj_is_marked(js_obj_ptr(parent)));
  assert(gc_obj_is_marked(js_obj_ptr(child)));
  gc_run_minor(js);
  gc_run(js);
  assert(gc_obj_is_marked(js_obj_ptr(child)));

  js_destroy(js);
  puts("PASS a failed permanent-root grow leaves the object unpinned and retryable");
  return 0;
}
