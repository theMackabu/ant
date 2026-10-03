// meson test -C build code-unit-ctor-proto
//
// A constructor compiled by the Function constructor lives in a code unit.
// `new` caches the slot of its `prototype` in the function's sidecar, which
// registers the cached shape (sv_ic_shape_ref_register). When the unit is
// freed, that registration must go with it: the sidecar is freed memory, and
// teardown (sv_ic_shape_refs_cleanup) writes through every registered slot.
#include "internal.h"
#include "gc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);

  // a hot, compiled caller constructs short-lived constructors, each called
  // too few times to be compiled itself, so every unit can die
  run(js,
    "globalThis.build = function (C) { return new C(); };"
    "for (let i = 0; i < 5000; i++) build(function () { this.y = i; });");
  gc_run(js);
  size_t registered = js->ic.shape_ref_len;

  run(js,
    "for (let round = 0; round < 64; round++) {"
    "  const C = new Function('this.x = ' + round + ';');"
    "  C.prototype.m = function () {};"
    "  for (let i = 0; i < 4; i++) build(C);"
    "}");
  assert(js->code_units.unit_count == 64);
  for (int i = 0; i < 4; i++) gc_run(js);

  // every unit is gone, and so is every registration into its memory: a
  // stale constructor cache would leave one entry per unit (64), while
  // sites outside the units add a few of their own
  assert(js->code_units.unit_count == 0);
  assert(js->ic.shape_ref_len < registered + 32);

  js_destroy(js);
  puts("PASS freed code units drop their constructor prototype caches");
}
