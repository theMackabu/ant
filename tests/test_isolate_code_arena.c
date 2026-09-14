// meson test -C build isolate-code-arena
#include "internal.h"
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void eval_number(ant_t *js, const char *source, double expected) {
  ant_value_t result = js_eval_bytecode_eval(js, source, strlen(source));
  if (vtype(result) != kTypeNumber) fprintf(stderr, "eval failed: %s\nsource: %s\n", js_str(js, result), source);
  assert(vtype(result) == kTypeNumber);
  assert(js_getnum(result) == expected);
}

static const char program[] =
  "globalThis.saved = (() => {"
  "  class Counter { static offset = 2; constructor() { this.value = 40; }"
  "    read() { return this.value + Counter.offset; } }"
  "  const counter = new Counter();"
  "  return function saved() { return counter.read(); };"
  "})();"
  "for (let i = 0; i < 2000; i++) saved();"
  "saved();";

static void test_marks(ant_t *js, ant_t *other) {
  size_t other_bytes = code_arena_get_memory(other);
  code_arena_mark_t start = code_arena_mark(js);
  const char *kept = code_arena_alloc(js, "kept", 4);
  assert(kept && strcmp(kept, "kept") == 0);
  assert(code_arena_alloc(js, "kept", 4) == kept);
  code_arena_mark_t mark = code_arena_mark(js);
  const char *discarded = code_arena_alloc(js, "discarded", 9);
  assert(discarded);
  assert(code_arena_bump(js, CODE_ARENA_BLOCK_SIZE * 2));
  assert(code_arena_alloc(js, "another block", 13));
  code_arena_rewind(js, mark);
  assert(code_arena_alloc(js, "kept", 4) == kept);
  // Reuse the discarded space: stale intern entries must not survive rewind.
  char *overwrite = code_arena_bump(js, 64);
  assert(overwrite);
  memset(overwrite, 'x', 64);
  assert(strcmp(code_arena_alloc(js, "discarded", 9), "discarded") == 0);
  assert(strcmp(code_arena_alloc(js, "another block", 13), "another block") == 0);
  code_arena_rewind(js, start);
  assert(code_arena_get_memory(other) == other_bytes);
}

int main(void) {
  char stack_base;
  ant_t *parent = ant_create();
  assert(parent);
  js_setstackbase(parent, &stack_base);
  eval_number(parent, program, 42);
  assert(parent->jit_ctx);
  const char *parent_source = code_arena_alloc(parent, program, sizeof(program) - 1);
  size_t parent_bytes = code_arena_get_memory(parent);
  assert(parent_bytes > 0);

  code_arena_mark_t scratch_mark = parse_arena_mark();
  char *scratch = parse_arena_bump(16);
  assert(scratch);
  memcpy(scratch, "enclosing parse", 16);
  size_t scratch_bytes = parse_arena_get_memory();

  for (int i = 0; i < 20; i++) {
    ant_t *child = ant_create();
    assert(child);
    js_setstackbase(child, &stack_base);
    assert(code_arena_get_memory(child) == 0);
    test_marks(child, parent);
    assert(code_arena_get_memory(child) == 0);
    eval_number(child, program, 42);
    assert(child->jit_ctx && child->jit_ctx != parent->jit_ctx);
    assert(code_arena_alloc(child, program, sizeof(program) - 1) != parent_source);
    test_marks(child, parent);
    eval_number(child, "saved()", 42);
    parent_bytes = code_arena_get_memory(parent);
    js_destroy(child);
    assert(code_arena_get_memory(parent) == parent_bytes);
    assert(parse_arena_get_memory() == scratch_bytes);
    assert(memcmp(scratch, "enclosing parse", 16) == 0);
    assert(strcmp(parent_source, program) == 0);
    eval_number(parent, "saved()", 42);
    eval_number(parent, "Function('return 6 * 7')()", 42);
    eval_number(parent, "eval('saved()')", 42);
  }

  ant_t *survivor = ant_create();
  assert(survivor);
  js_setstackbase(survivor, &stack_base);
  eval_number(survivor, program, 42);
  size_t survivor_bytes = code_arena_get_memory(survivor);
  js_destroy(parent);
  assert(code_arena_get_memory(survivor) == survivor_bytes);
  eval_number(survivor, "saved()", 42);
  js_destroy(survivor);
  assert(memcmp(scratch, "enclosing parse", 16) == 0);
  parse_arena_rewind(scratch_mark);
  puts("PASS isolate code ownership, interning, rewind and both teardown orders");
}
