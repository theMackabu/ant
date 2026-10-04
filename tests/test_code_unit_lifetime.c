// meson test -C build code-unit-lifetime
//
// What may and may not keep a code unit (eval and Function code) alive:
// - the error site can be left pointing at unit code (reportError reads the
//   call location and keeps it), but it does not pin the unit; the unit dies
//   with the rest of the eval and clears the site;
// - an allocation bigger than a code block gets a block of its own, which is
//   released with its unit even when a small unit compiled right after it is
//   still alive;
// - IC entries in unit code keep their shape references off the global
//   registry (SV_IC_UNIT_OWNED); the unit releases them itself.
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

static void collect(ant_t *js) {
  for (int i = 0; i < 4; i++) gc_run(js);
}

int main(void) {
  char stack_base;
  // compiled code would make the units immortal (sv_code_unit_make_immortal)
  sv_debug_enable(SV_DEBUG_JITLESS);
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);

  collect(js);
  size_t units = js->code_units.unit_count;

  // an error site left in eval code, as reportError leaves it
  run(js, "globalThis.reporter = new Function('return 1');");
  sv_func_t *reporter = js_func_closure(js_get(js, js->global, "reporter"))->func;
  assert(reporter->unit);
  js_set_error_site_from_bc(js, reporter, 0, NULL);
  assert(js->errsite.unit == reporter->unit);
  run(js, "globalThis.reporter = undefined;");
  collect(js);
  assert(js->errsite.unit == NULL);
  assert(js->code_units.unit_count == units);

  // a 64 KiB literal, then a small unit that stays alive
  size_t held = js->code_units.held_bytes;
  run(js,
    "globalThis.bigLen = new Function('return \"' + 'x'.repeat(65536) + '\".length')();"
    "globalThis.keep = new Function('return 7');");
  collect(js);
  assert(js->code_units.unit_count == units + 1);
  assert(js->code_units.held_bytes < held + 32 * 1024);

  // property ICs in unit code
  run(js,
    "globalThis.icFn = new Function(`"
    "  let s = 0;"
    "  for (let i = 0; i < 2000; i++) { const o = { a: i, b: 2 }; o.c = 3; s += o.a + o.b + o.c; }"
    "  return s;"
    "`);"
    "globalThis.icSum = icFn();");
  sv_func_t *ic_fn = js_func_closure(js_get(js, js->global, "icFn"))->func;
  assert(ic_fn->unit && ic_fn->ic_count > 0);
  
  int cached = 0;
  for (uint16_t i = 0; i < ic_fn->ic_count; i++) {
    assert(ic_fn->ic_slots[i].shape_ref_mask & SV_IC_UNIT_OWNED);
    if (ic_fn->ic_slots[i].shape_ref_mask & SV_IC_SHAPE_REF_CACHED) cached++;
  }
  assert(cached > 0);
  
  const char *ic_lo = (const char *)ic_fn->ic_slots;
  const char *ic_hi = (const char *)(ic_fn->ic_slots + ic_fn->ic_count);
  for (size_t i = 0; i < js->ic.shape_ref_len; i++) {
    const char *slot = (const char *)js->ic.shape_ref_slots[i];
    assert(slot < ic_lo || slot >= ic_hi);
  }
  
  run(js, "globalThis.icFn = undefined;");
  collect(js);

  run(js, "globalThis.keep = undefined;");
  collect(js);
  assert(js->code_units.unit_count == units);

  js_destroy(js);
  puts("PASS errors, oversized blocks and ICs do not keep code units alive");
}
