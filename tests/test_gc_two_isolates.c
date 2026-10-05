// meson test -C build gc-two-isolates
//
// Each isolate keeps its own collection state (js->gc): policy, the 8-bit
// object mark epoch and the mark stack. With one process-wide mark epoch,
// collections in one isolate advanced the epoch another isolate's objects
// were marked with; after 253 of them (the epoch cycles through 254 values)
// an object marked by the other isolate's last collection looked already
// marked to its next one, was not scanned, and what only it referenced was
// freed.
#include "internal.h"
#include "gc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
}

// globalThis.keep.child is an intact object whose tag is `tag`
static void check_child(ant_t *js, double tag) {
  ant_value_t keep = js_get(js, js->global, "keep");
  assert(vtype(keep) == kTypeObject);
  ant_value_t child = js_get(js, keep, "child");
  assert(vtype(child) == kTypeObject);
  assert(js_obj_ptr(child)->mark_epoch != ANT_GC_DEAD);
  ant_value_t value = js_get(js, child, "tag");
  assert(vtype(value) == kTypeNumber && js_getnum(value) == tag);
}

int main(void) {
  char stack_base;
  ant_t *a = ant_create();
  ant_t *b = ant_create();
  assert(a && b);
  js_setstackbase(a, &stack_base);
  js_setstackbase(b, &stack_base);

  run(a, "globalThis.junk = [];");
  run(b, "globalThis.keep = { child: null };");
  gc_run(b);

  for (unsigned gap = 250; gap <= 258; gap++) {
    char source[96];
    snprintf(source, sizeof(source), "keep.child = { tag: %u };", gap);
    run(b, source);

    // collections in A only; B's objects must not notice
    for (unsigned i = 0; i < gap; i++) gc_run_minor(a);

    gc_run(b);
    check_child(b, gap);
  }

  js_destroy(a);
  js_destroy(b);
  puts("PASS collections in one isolate leave another isolate's marks alone");
}
