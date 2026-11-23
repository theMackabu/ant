#include "internal.h"
#include "gc.h"
#include "silver/engine.h"
#include "debug.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  if (is_err(result)) fprintf(stderr, "%s\n", js_str(js, js_tostring_val(js, Ant_Exception_Current(js))));
  assert(!is_err(result));
}

static double number(ant_t *js, const char *name) {
  ant_value_t value = js_get(js, js->global, name);
  assert(vtype(value) == kTypeNumber);
  return Ant_Value_AsNumberUnchecked(value);
}

int main(void) {
  char stack_base;
  sv_debug_enable(SV_DEBUG_JITLESS);
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);

  run(js,
    "globalThis.src = 'a + b';"
    "globalThis.leaf = function (a, b) { return eval(src); };"
    "globalThis.sum = 0;"
    "for (var i = 0; i < 6; i++) sum += leaf(i, 1);"
  );
  assert(number(js, "sum") == 21);
  assert(vtype(js->eval_leaf_env) == kTypeObject);

  ant_value_t first = js->eval_leaf_env;
  run(js, "for (var i = 0; i < 6; i++) sum += leaf(i, 2);");
  assert(number(js, "sum") == 48);
  assert(js->eval_leaf_env == first);

  for (int i = 0; i < 4; i++) gc_run(js);
  assert(js->eval_leaf_env == Ant_Value_Undefined);

  run(js, "for (var i = 0; i < 6; i++) sum += leaf(i, 3);");
  assert(number(js, "sum") == 81);
  assert(vtype(js->eval_leaf_env) == kTypeObject);

  run(js,
    "globalThis.outer = 'inner(a + 1) + a';"
    "globalThis.inner = function (b) { return eval(src2); };"
    "globalThis.src2 = 'b * 2';"
    "globalThis.host = function (a) { return eval(outer); };"
    "sum = 0; for (var i = 0; i < 6; i++) sum += host(i);"
  );
  assert(number(js, "sum") == 57);

  Ant_Destroy(js);
  puts("ok");
  return 0;
}
