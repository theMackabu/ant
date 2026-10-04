// meson test -C build ic-poly-shape-release
//
// A read or store site that sees several receiver shapes keeps the extra
// cases in a per-site block (sv_gf_poly_t, sv_pf_poly_t), each holding a
// reference to its shape. A major bumps the epoch that makes every case miss,
// so it also releases those references (sv_ic_polys_release_shapes): shapes
// that only stale cases still pointed to are freed instead of lingering until
// the site happens to overwrite them.
#include "internal.h"
#include "gc.h"
#include "shapes.h"
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

  // one read site and one store site that each see 8 shapes built from
  // property names used nowhere else, so nothing else keeps those shapes
  run(js,
    "globalThis.readV = function (o) { return o.v; };"
    "globalThis.writeV = function (o, x) { o.v = x; };"
    "globalThis.makeAll = function (round) {"
    "  const out = [];"
    "  for (let k = 0; k < 8; k++) {"
    "    const o = {}; o['u' + round + '_' + k] = k; o.v = k; out.push(o);"
    "  }"
    "  return out;"
    "};");
  gc_run(js);
  size_t before = ant_shape_total_bytes();

  run(js,
    "for (let r = 0; r < 200; r++) {"
    "  const objs = makeAll(0);"
    "  for (const o of objs) { readV(o); writeV(o, r); }"
    "}");
  size_t during = ant_shape_total_bytes();
  assert(during > before);

  // the objects are garbage now; only the sites' stale cases reference them
  gc_run(js);
  gc_run(js);
  size_t after = ant_shape_total_bytes();

  printf("shape bytes: before %zu, during %zu, after %zu\n", before, during, after);
  // most of what the scenario added is gone again
  assert(after - before < (during - before) / 2);

  js_destroy(js);
  puts("PASS a major releases the shapes stale polymorphic cases held");
}
