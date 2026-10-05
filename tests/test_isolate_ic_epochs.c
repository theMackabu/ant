// meson test -C build isolate-ic-epochs
//
// Inline-cache epochs (js->ic.epoch, js->ic.obj_epoch) and the string and
// bigint mark tables belong to each isolate. Edits in one isolate leave the
// other's caches valid, while an isolate's own edits still invalidate its
// interpreter and JIT caches, which read the epoch through the isolate.
#include "internal.h"
#include "gc.h"
#include "silver/engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  if (is_err(result)) fprintf(stderr, "%s\n-> %s\n", source, js_str(js, js_tostring_val(js, Ant_Exception_Current(js))));
  assert(!is_err(result));
}

static double num(ant_t *js, const char *expr) {
  char source[256];
  snprintf(source, sizeof(source), "globalThis.result = (%s);", expr);
  run(js, source);
  ant_value_t result = js_get(js, js->global, "result");
  assert(vtype(result) == kTypeNumber);
  return js_getnum(result);
}

int main(void) {
  char stack_base;
  ant_t *a = ant_create();
  ant_t *b = ant_create();
  assert(a && b);
  js_setstackbase(a, &stack_base);
  js_setstackbase(b, &stack_base);

  // a hot property read in A, long enough to be compiled
  run(a,
    "globalThis.Point = class { constructor(x) { this.x = x; } };"
    "globalThis.readX = function (n) { let s = 0; const p = new Point(1); for (let i = 0; i < n; i++) s += p.x; return s; };");
  assert(num(a, "readX(200000)") == 200000);
  run(a, "for (let i = 0; i < 3000; i++) readX(100);");
  sv_func_t *read_x = js_func_closure(js_get(a, a->global, "readX"))->func;
  assert(read_x->jit_code);

  // B edits prototypes and shapes; A's epochs do not move
  uint32_t a_epoch = a->ic.epoch, a_obj_epoch = a->ic.obj_epoch;
  uint32_t b_epoch = b->ic.epoch;
  run(b,
    "for (let i = 0; i < 1000; i++) {"
    "  const o = { a: i }; Object.setPrototypeOf(o, { b: i });"
    "  Object.defineProperty(o, 'a', { value: i, writable: false });"
    "}");
  assert(b->ic.epoch != b_epoch);
  assert(a->ic.epoch == a_epoch && a->ic.obj_epoch == a_obj_epoch);
  assert(num(a, "readX(1000)") == 1000);

  // A's own prototype edit invalidates its caches, compiled code included
  run(a, "Object.defineProperty(Point.prototype, 'x', { get() { return 2; }, configurable: true });");
  run(a, "globalThis.Point = class { constructor() {} }; Object.defineProperty(Point.prototype, 'x', { get() { return 3; } });");
  assert(a->ic.epoch != a_epoch);
  assert(num(a, "readX(1000)") == 3000);

  // a compiled global read: redefining the global as an accessor edits the
  // global object's shape in place, so only the epoch tells compiled code
  run(a,
    "globalThis.G = 1;"
    "globalThis.readG = function (n) { let s = 0; for (let i = 0; i < n; i++) s += G; return s; };"
    "for (let i = 0; i < 3000; i++) readG(100);");
  assert(js_func_closure(js_get(a, a->global, "readG"))->func->jit_code);
  run(b, "for (let i = 0; i < 100; i++) Object.defineProperty(globalThis, 'H' + i, { get() { return i; } });");
  assert(num(a, "readG(1000)") == 1000);
  run(a, "Object.defineProperty(globalThis, 'G', { get() { return 5; }, configurable: true });");
  assert(num(a, "readG(1000)") == 5000);

  // strings and bigints kept in B survive A's collections
  run(b, "globalThis.kept = { s: 'kept-' + 'string'.repeat(64), n: 2n ** 200n };");
  for (int i = 0; i < 300; i++) {
    run(a, "globalThis.junk = Array.from({ length: 64 }, (_, i) => 'junk' + i + 'x'.repeat(i) + (7n ** 40n * 3n));");
    gc_run_minor(a);
    if (i % 50 == 0) gc_run(a);
  }
  gc_run(b);
  assert(num(b, "kept.s.length") == 5 + 6 * 64);
  assert(num(b, "Number(kept.n % 1000003n)") == num(b, "Number((2n ** 200n) % 1000003n)"));

  js_destroy(a);
  js_destroy(b);
  puts("PASS each isolate owns its IC epochs and string and bigint marks");
}
