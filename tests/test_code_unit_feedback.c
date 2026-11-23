// meson test -C build code-unit-feedback
//
// Call-target feedback is weak: a function outside any code unit (script or
// module code, never freed) may keep a target in a unit that dies. Majors
// clear such targets for every function that has recorded one, whether or
// not that major traced the function (code_units.fb_watch); otherwise the
// target would point into freed unit memory for the JIT to read later.
#include "internal.h"
#include "gc.h"
#include "silver/engine.h"
#include "debug.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
}

static int unit_targets(const sv_func_t *func) {
  int n = 0;
  for (int i = 0; i < func->call_target_fb_count; i++) {
    const sv_func_t *target = func->call_target_fb[i].target;
    if (target && target->unit) n++;
  }
  return n;
}

int main(void) {
  char stack_base;
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);

  // inner is script code; it calls a function from a unit often enough to
  // record feedback, then everything that could reach inner is dropped
  run(js,
    "globalThis.outer = function () {"
    "  return function inner(f) { let s = 0; for (let i = 0; i < 8; i++) s += f(i); return s; };"
    "};"
    "globalThis.inner = outer();"
    "globalThis.dyn = new Function('x', 'return x + 1');"
    "inner(dyn); inner(dyn); inner(dyn); inner(dyn);");

  sv_closure_t *closure = js_func_closure(js_get(js, js->global, "inner"));
  assert(closure && closure->func && !closure->func->unit);
  sv_func_t *inner = closure->func;
  sv_func_t *dyn = js_func_closure(js_get(js, js->global, "dyn"))->func;
  assert(dyn && dyn->unit);
  assert(!inner->jit_code && !dyn->jit_code);
  assert(unit_targets(inner) == 1);
  assert(inner->fb_unit_watched);

  run(js, "globalThis.outer = undefined; globalThis.inner = undefined; globalThis.dyn = undefined;");
  size_t units_before = js->code_units.unit_count;
  for (int i = 0; i < 4; i++) gc_run(js);
  assert(js->code_units.unit_count < units_before);

  // inner was not traced, and its unit target is gone with the unit
  assert(unit_targets(inner) == 0);

  Ant_Destroy(js);
  puts("PASS untraced functions drop feedback targets in freed code units");
}
