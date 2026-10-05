// meson test -C build isolate-code-arenas
//
// Each isolate owns its code arena (bytecode, IC slots and source text of
// script and module code), its parse arena and its code-text intern table.
// With one process-wide arena, js_destroy freed the code of every isolate,
// and the survivor ran freed bytecode.
#include "internal.h"
#include "gc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ant_value_t run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
  return result;
}

// script completion values are not returned, so the result goes through a global
static double num(ant_t *js, const char *expr) {
  char source[256];
  snprintf(source, sizeof(source), "globalThis.result = (%s);", expr);
  run(js, source);
  ant_value_t result = js_get(js, js->global, "result");
  assert(vtype(result) == kTypeNumber);
  return js_getnum(result);
}

// script code whose bytecode, ICs and interned source live in the arena
static void load(ant_t *js, int seed) {
  char source[512];
  snprintf(source, sizeof(source),
    "globalThis.seed = %d;"
    "globalThis.point = (x, y) => ({ x, y });"
    "globalThis.sum = function (n) { let s = 0; for (let i = 0; i < n; i++) { const p = point(i, seed); s += p.x + p.y; } return s; };"
    "globalThis.Counter = class { constructor() { this.n = seed; } bump() { return ++this.n; } };"
    "globalThis.counter = new Counter();",
    seed);
  run(js, source);
}

static void check(ant_t *js, int seed) {
  assert(num(js, "sum(100)") == 4950 + 100.0 * seed);
  assert(num(js, "counter.bump()") > seed);
  assert(num(js, "String(function fresh() { return 1 }).length") > 0);
  assert(num(js, "new Function('a', 'return a * 2')(21)") == 42);
}

int main(void) {
  char stack_base;

  for (int destroy_first = 0; destroy_first < 2; destroy_first++) {
    ant_t *a = ant_create();
    ant_t *b = ant_create();
    assert(a && b);
    js_setstackbase(a, &stack_base);
    js_setstackbase(b, &stack_base);

    load(a, 1);
    load(b, 2);
    check(a, 1);
    check(b, 2);

    ant_t *gone = destroy_first ? a : b;
    ant_t *kept = destroy_first ? b : a;
    js_destroy(gone);

    // the survivor's existing code still runs, and it can compile more
    check(kept, destroy_first ? 2 : 1);
    load(kept, 3);
    gc_run(kept);
    check(kept, 3);
    js_destroy(kept);
  }

  puts("PASS each isolate owns its code and parse arenas");
}
