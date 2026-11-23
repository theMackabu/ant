#include "internal.h"
#include "cfunc.h"
#include "gc.h"
#include "silver/engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t armed_epoch;

static ant_value_t collect_at_next_string(ant_params_t) {
  armed_epoch = js->gc.epoch;
  js->gc.pool_alloc = gc_pool_major_threshold(js);
  return Ant_Value_Undefined;
}

static ant_value_t collect(ant_params_t) {
  for (int i = 0; i < 3; i++) gc_run(js);
  return Ant_Value_Undefined;
}

static ant_value_t collected(ant_params_t) {
  return js->gc.epoch != armed_epoch ? Ant_Value_True : Ant_Value_False;
}

static ant_value_t compiled(ant_params_t) {
  assert(nargs == 1 && vtype(args[0]) == kTypeFunction);
  return js_func_closure(args[0])->func->jit_code != NULL ? Ant_Value_True : Ant_Value_False;
}

static ant_value_t bailouts(ant_params_t) {
  assert(nargs == 1 && vtype(args[0]) == kTypeFunction);
  return Ant_Value_FromNumber(js_func_closure(args[0])->func->jit_bailout_count);
}

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
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);

  js_set(js, js->global, "collectAtNextString", Ant_Function_Create(collect_at_next_string));
  js_set(js, js->global, "collect", Ant_Function_Create(collect));
  js_set(js, js->global, "collected", Ant_Function_Create(collected));
  js_set(js, js->global, "bailouts", Ant_Function_Create(bailouts));
  js_set(js, js->global, "compiled", Ant_Function_Create(compiled));

  run(js,
    "globalThis.box = { o: null };"
    "globalThis.warm = function (mk) {"
    "  box.o = Object.create({ base: 5 });"
    "  let t = 0;"
    "  for (let i = 0; i < 20000; i++) t += mk(box.o)('abc');"
    "  return t;"
    "};"
    "globalThis.other = Object.create({ base: 7 });"
    "globalThis.piece = 'x'.repeat(100);"
    "globalThis.dropped = 0;"
    "globalThis.wrong = 0;"
    "for (let round = 0; round < 6; round++) {"
    "  const mk = new Function('return (a) => (b) => a.base + b.length;')();"
    "  const fire = new Function('mk', 'other', 'piece', 'box',"
    "    'let s = \"\";' + 's += piece;'.repeat(8) +"
    "    'collect(); box.o = null; collectAtNextString(); const r = mk(other)(s); return collected() ? r : -1;');"
    "  if (warm(mk) !== 20000 * 8) wrong++;"
    "  const inner = mk(other);"
    "  const before = compiled(inner);"
    "  if (fire(mk, other, piece, box) !== 807) wrong++;"
    "  if (before && !compiled(inner) && bailouts(inner) === 0) dropped++;"
    "}"
  );

  assert(number(js, "wrong") == 0);
  assert(number(js, "dropped") > 0);

  Ant_Destroy(js);
  puts("ok");
  return 0;
}
