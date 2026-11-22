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

static size_t compiles(ant_t *js, const char *expression, double expected) {
  char source[256];
  snprintf(source, sizeof source, "globalThis.out = %s;", expression);
  size_t before = js->code_units.unit_count;
  run(js, source);
  ant_value_t out = js_get(js, js->global, "out");
  assert(vtype(out) == kTypeNumber && Ant_Value_AsNumberUnchecked(out) == expected);
  return js->code_units.unit_count - before;
}

int main(void) {
  char stack_base;
  sv_debug_enable(SV_DEBUG_JITLESS);
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);

  run(js,
    "globalThis.hosts = [];"
    "for (var j = 0; j < 40; j++) hosts.push(new Function('i', 'a', 'src', 'var own' + j + ' = ' + j + '; return eval(src);'));"
    "globalThis.cycle = function (src, scopes, rounds) {"
    "  var sum = 0;"
    "  for (var r = 0; r < rounds; r++) for (var s = 0; s < scopes; s++) sum += hosts[s](r, s, src);"
    "  return sum;"
    "};"
    "globalThis.blocks = function (i, a, src) {"
    "  var sum = 0;"
    "  { let p = 1; sum += eval(src) + p; }"
    "  { let q = 2; sum += eval(src) + q; }"
    "  return sum;"
    "};"
    "globalThis.blocksum = function (src) { var t = 0; for (var k = 0; k < 8; k++) t += blocks(k, 1, src); return t; };"
    "globalThis.template = function (src) { return eval(src); };"
    "globalThis.templates = function (src) { var first = template(src), n = 1; for (var k = 0; k < 5; k++) if (template(src) !== first) n++; return n; };"
    "globalThis.kinds = function (src) { return cycle(src, 40, 8).length; };"
  );

  assert(compiles(js, "cycle('i + a + 1000', 1, 8)", 8028) == 2);
  assert(compiles(js, "cycle('i + a + 1000', 1, 8)", 8028) == 0);

  assert(compiles(js, "hosts[1](5, 6, 'i + a + 1000')", 1011) == 0);
  assert(compiles(js, "hosts[1](5, 6, 'i + a + 1000')", 1011) <= 1);
  assert(compiles(js, "hosts[1](5, 6, 'i + a + 1000')", 1011) == 0);

  assert(compiles(js, "cycle('i + a + 2000', 2, 8)", 32064) <= 3);
  assert(compiles(js, "cycle('i + a + 2000', 2, 8)", 32064) == 0);

  assert(compiles(js, "cycle('i + a + 3000', 4, 8)", 96160) <= 5);
  assert(compiles(js, "cycle('i + a + 3000', 4, 8)", 96160) == 0);

  assert(compiles(js, "cycle('i + a + 4000', 5, 8)", 160220) <= 5);
  assert(compiles(js, "cycle('i + a + 4000', 5, 8)", 160220) == 0);

  assert(compiles(js, "cycle('i + a + 5000', 40, 8)", 1607360) <= 5);
  assert(compiles(js, "cycle('i + a + 5000', 40, 8)", 1607360) == 0);
  assert(compiles(js, "kinds('typeof own39 + typeof own0')", 5713) <= 5);

  assert(compiles(js, "blocksum('i + a + 6000')", 96096) <= 3);
  assert(compiles(js, "blocksum('i + a + 6000')", 96096) == 0);

  assert(compiles(js, "templates('(x => x)`a`')", 6) == 6);

  size_t worst = 0, forked = 0;
  char source[160];
  for (int n = 0; n < 1500; n++) {
    snprintf(source, sizeof source, "cycle('i + a + %d', 2, 12)", 10000 + n);
    size_t count = compiles(js, source, 24.0 * (10000 + n) + 144);
    if (count > worst) worst = count;
    if (count == 3) forked++;
  }
  assert(worst == 3 && forked > 1400);

  Ant_Destroy(js);
  puts("ok");
  return 0;
}
