#include "internal.h"
#include "gc.h"
#include "runtime.h"
#include "silver/ast.h"
#include "silver/engine.h"
#include "debug.h"
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { FAIL_ONCE, FAIL_REST } fail_mode_t;

typedef struct {
  unsigned calls;
  unsigned fail_at;
  fail_mode_t mode;
} fault_t;

static fault_t grow_fault;
static fault_t ctx_fault;

static bool fault_hit(fault_t *fault) {
  fault->calls++;
  if (!fault->fail_at) return false;
  return fault->calls == fault->fail_at || (fault->mode == FAIL_REST && fault->calls > fault->fail_at);
}

static void *GrowRealloc(void *ptr, size_t size) { return fault_hit(&grow_fault) ? NULL : realloc(ptr, size); }
static void *CtxRealloc(void *ptr, size_t size) { return fault_hit(&ctx_fault) ? NULL : realloc(ptr, size); }
static void *CtxMalloc(size_t size) { return fault_hit(&ctx_fault) ? NULL : malloc(size); }

#define realloc GrowRealloc
#include "../src/silver/compiler/emit.c"
#undef realloc

#define realloc CtxRealloc
#define malloc CtxMalloc
#include "../src/silver/compile_ctx.c"
#undef malloc
#undef realloc

typedef enum { SOURCE_SCRIPT, SOURCE_MODULE, SOURCE_FUNCTION } source_kind_t;

typedef struct {
  char *text;
  size_t len;
  size_t cap;
} text_t;

__attribute__((format(printf, 2, 3)))
static void put(text_t *t, const char *fmt, ...) {
  va_list ap;
  for (;;) {
    va_start(ap, fmt);
    int n = vsnprintf(t->text + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    assert(n >= 0);
    if (t->len + (size_t)n < t->cap) {
      t->len += (size_t)n;
      return;
    }
    t->cap = t->cap ? t->cap * 2 : 4096;
    t->text = realloc(t->text, t->cap);
    assert(t->text);
  }
}

static void put_script_body(text_t *t) {
  put(t, "var m = new Map(), obj = { p: 1 }, fns = [];\n");
  put(t, "function wide(a0");
  for (int i = 1; i < 12; i++) put(t, ", a%d", i);
  put(t, ", d = a0 + 1, { e, f: [g = 2] = [] } = {}, ...rest) {\n");
  for (int i = 0; i < 40; i++) put(t, "  let v%d = %d.5 + a%d;\n", i, i, i % 12);
  for (int i = 0; i < 24; i++) put(t, "  obj.q%d = 'k%d' + v%d;\n", i, i, i);
  for (int i = 0; i < 12; i++) put(t, "  m.get(`t%d${v%d}x`); m.has(`h${v%d}y${a0}`);\n", i, i, i);
  put(t, "  const chain = obj?.a?.b?.c?.d?.e?.f ?? v1;\n");
  put(t, "  const run = [0.5");
  for (int i = 1; i < 40; i++) put(t, ", %d.5", i);
  put(t, "];\n");
  put(t, "  outer: for (let i = 0; i < 2; i++) {\n");
  put(t, "    for (const x of [1]) { for (const k in obj) { while (0) { do { switch (i) { case 0: let q = 1; break; case 1: break; default: continue outer; } } while (0); } } }\n");
  for (int i = 0; i < 6; i++) put(t, "    if (i == %d) break;\n", i);
  put(t, "  }\n");
  put(t, "  for (let b0 = 0");
  for (int i = 1; i < 10; i++) put(t, ", b%d = %d", i, i);
  put(t, "; b0 < 1; b0++) fns.push(() => b0");
  for (int i = 1; i < 10; i++) put(t, " + b%d", i);
  put(t, ");\n");
  for (int i = 0; i < 10; i++) put(t, "  try {\n");
  put(t, "  v0++;\n");
  for (int i = 0; i < 10; i++) put(t, "  } catch (e%d) { v1++; } finally { v2++; }\n", i);
  for (int i = 0; i < 6; i++) put(t, "  { using r%d = null;\n", i);
  put(t, "  v3++;\n");
  for (int i = 0; i < 6; i++) put(t, "  }\n");
  for (int i = 0; i < 6; i++) put(t, "  with (obj) {\n");
  put(t, "  p = v4 + a0; q0 = chain;\n");
  for (int i = 0; i < 6; i++) put(t, "  }\n");
  put(t, "  class K extends Object {\n");
  for (int i = 0; i < 10; i++) put(t, "    #p%d = %d; static #s%d = v%d;\n", i, i, i, i);
  put(t, "    static { this.z = v5; }\n");
  put(t, "    m() { return this.#p0 + super.toString + v6; }\n");
  put(t, "    get g() { return this.#p1; } static sm() { return K.#s0; } [`k${v7}`]() { return 1; } f = () => this.#p2;\n");
  put(t, "    constructor() { super(); this.h = v8; }\n");
  put(t, "  }\n");
  put(t, "  function* gen() { yield* [v9]; }\n");
  put(t, "  async function af() { await v10; for await (const z of []) {} }\n");
  put(t, "  const tpl = `${v11}-${v12}-${a1}`;\n");
  put(t, "  eval('v13');\n");
  put(t, "  return [v0, ...rest, tpl, `${a2}`.replace(/a/, `b${a3}c`), new K(), gen, af];\n");
  put(t, "}\n");
  for (int i = 0; i < 14; i++) put(t, "function n%d(u%d) { let w%d = u%d + %d.25;\n", i, i, i, i, i);
  put(t, "return () => [w0");
  for (int i = 1; i < 14; i++) put(t, ", w%d, u%d", i, i);
  put(t, "];\n");
  for (int i = 0; i < 14; i++) put(t, "}\n");
  put(t, "function captures() {\n");
  for (int i = 0; i < 40; i++) put(t, "  let c%d = %d;\n", i, i);
  put(t, "  return function () { return c0");
  for (int i = 1; i < 40; i++) put(t, " + c%d", i);
  put(t, "; };\n}\n");
  put(t, "function typed() { let x = 1, y = 2, z = 0; for (let i = 0; i < 2; i++) {");
  for (int i = 0; i < 48; i++) put(t, " obj.f%d = i;", i);
  put(t, " z = x * y + i - x / y; x = 'k'; } return z; }\n");
  put(t, "class Derived extends Object { constructor() { try { super(); return; } finally { fns.push(1); } } }\n");
}

static char *make_script(void) {
  text_t t = {0};
  put_script_body(&t);
  return t.text;
}

static char *make_module(void) {
  text_t t = {0};
  put(&t, "import { x0 } from './dep.js';\n");
  put(&t, "export var e0 = 0");
  for (int i = 1; i < 12; i++) put(&t, ", e%d = %d", i, i);
  put(&t, ";\n");
  put(&t, "export function f() { return e0 + x0; }\n");
  put(&t, "export default class { #a = 1; get a() { return this.#a; } }\n");
  put(&t, "for (let i = 0; i < 1; i++) { await null; }\n");
  return t.text;
}

static char *make_function_body(void) {
  text_t t = {0};
  for (int i = 0; i < 30; i++) put(&t, "var z%d = a + %d.5;\n", i, i);
  put(&t, "for (const k of [b]) { try { z0 += k; } finally { z1++; } }\n");
  put(&t, "return function inner(...xs) { return z0 + z29 + xs.length; };\n");
  return t.text;
}

static char *make_spills(void) {
  text_t t = {0};
  put(&t, "function big() {\n");
  for (int i = 0; i < SV_SPILL_THRESHOLD + 1200; i++) put(&t, "let s%d = %d;\n", i, i);
  put(&t, "with ({}) { s%d; } return () => s0 + s%d; }\n", SV_SPILL_THRESHOLD + 1199, SV_SPILL_THRESHOLD + 1100);
  return t.text;
}

static char *make_lines(void) {
  text_t t = {0};
  for (int i = 0; i < 400; i++) put(&t, "x%d = %d;\n", i % 7, i);
  return t.text;
}

static sv_func_t *compile_source(ant_t *js, source_kind_t kind, const char *text) {
  size_t len = strlen(text);
  if (kind == SOURCE_FUNCTION) {
    static const sv_param_t params[] = { SV_PARAM("a"), SV_PARAM("b") };
    return sv_compile_function_with_params(js, params, 2, text, len, false);
  }
  code_arena_mark_t mark = parse_arena_mark(js);
  sv_ast_t *program = sv_parse(
    js, text, (ant_offset_t)len,
    kind == SOURCE_MODULE ? SV_PARSE_MODULE : SV_PARSE_SCRIPT, false);
  assert(program && !Ant_Exception_Pending(js));
  sv_func_t *func = sv_compile(
    js, program, kind == SOURCE_MODULE ? SV_COMPILE_MODULE : SV_COMPILE_SCRIPT,
    text, (ant_offset_t)len, 0, Ant_Value_Undefined);
  parse_arena_rewind(js, mark);
  return func;
}

static ant_value_t pending_value(ant_t *js) {
  return Ant_Exception_Value(js, Ant_Exception_Peek(js));
}

static bool pending_out_of_memory(ant_t *js) {
  ant_value_t message = js_get(js, pending_value(js), "message");
  return vtype(message) == kTypeString && strstr(js_str(js, message), "Memory allocation failed");
}

typedef struct {
  unsigned points;
  unsigned failed;
  unsigned degraded;
} sweep_t;

static sweep_t sweep(ant_t *js, fault_t *fault, const char *name, source_kind_t kind, const char *text, bool may_degrade) {
  *fault = (fault_t){0};
  sv_func_t *clean = compile_source(js, kind, text);
  assert(clean && !Ant_Exception_Pending(js));
  sweep_t result = { .points = fault->calls };
  assert(result.points > 0);

  unsigned runs = 0;
  for (int mode = FAIL_ONCE; mode <= FAIL_REST; mode++) {
    for (unsigned n = 1; n <= result.points; n++) {
      *fault = (fault_t){ .fail_at = n, .mode = (fail_mode_t)mode };
      sv_func_t *func = compile_source(js, kind, text);
      assert(fault->calls >= n);
      *fault = (fault_t){0};
      if (func) {
        assert(may_degrade && !Ant_Exception_Pending(js));
        result.degraded++;
      } else {
        assert(Ant_Exception_Pending(js));
        if (!pending_out_of_memory(js)) {
          fprintf(stderr, "%s: failure point %u (mode %d) left a different error: %s\n",
            name, n, mode, js_str(js, js_tostring_val(js, pending_value(js))));
          assert(false);
        }
        Ant_Exception_Clear(js);
        result.failed++;
      }
      if (++runs % 64 == 0) gc_run(js);
    }
  }

  printf("%s: %u failure points, %u failed compiles, %u degraded compiles\n",
    name, result.points, result.failed, result.degraded);
  return result;
}

static void check_line_table(void) {
  char *text = make_lines();
  ant_offset_t len = (ant_offset_t)strlen(text);
  ctx_fault = (fault_t){0};
  sv_line_table_t *lt = sv_compile_ctx_build_line_table(text, len);
  assert(lt && lt->count == 401);
  unsigned points = ctx_fault.calls;
  assert(points >= 3);
  sv_compile_ctx_free_line_table(lt);
  for (unsigned n = 1; n <= points; n++) {
    ctx_fault = (fault_t){ .fail_at = n };
    assert(sv_compile_ctx_build_line_table(text, len) == NULL);
  }
  ctx_fault = (fault_t){0};
  printf("line table: %u failure points\n", points);
  free(text);
}

static void run(ant_t *js, const char *source, double expected) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  if (is_err(result)) fprintf(stderr, "%s\n", js_str(js, js_tostring_val(js, pending_value(js))));
  assert(!is_err(result));
  ant_value_t out = js_get(js, js->global, "out");
  assert(vtype(out) == kTypeNumber && Ant_Value_AsNumberUnchecked(out) == expected);
}

int main(void) {
  char stack_base;
  sv_debug_enable(SV_DEBUG_JITLESS);
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);

  char *script = make_script();
  char *module = make_module();
  char *body = make_function_body();
  char *lines = make_lines();
  char *spills = make_spills();

  unsigned growth = 0;
  growth += sweep(js, &grow_fault, "script", SOURCE_SCRIPT, script, false).points;
  growth += sweep(js, &grow_fault, "module", SOURCE_MODULE, module, false).points;
  growth += sweep(js, &grow_fault, "function", SOURCE_FUNCTION, body, false).points;
  growth += sweep(js, &grow_fault, "spills", SOURCE_SCRIPT, spills, false).points;
  assert(growth > 300);

  sweep(js, &ctx_fault, "context script", SOURCE_SCRIPT, script, true);
  sweep(js, &ctx_fault, "context lines", SOURCE_SCRIPT, lines, true);
  check_line_table();

  gc_run(js);
  run(js, "var total = 0; for (let i = 0; i < 4; i++) { try { total += i; } finally { total++; } } globalThis.out = total;", 10);

  free(script);
  free(module);
  free(body);
  free(lines);
  free(spills);
  Ant_Destroy(js);
  printf("compiler growth failures recovered\n");
  return 0;
}
