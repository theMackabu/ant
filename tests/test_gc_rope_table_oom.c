// meson test -C build gc-rope-table-oom
//
// A major collection that cannot allocate its rope mark table falls back to
// conservative rope marking: ropes and builders are not traced one by one,
// every rope pool is scanned as raw words instead, and rope blocks are kept.
// In that mode the string markers must handle flat strings only, and a flat
// string reachable only through a rope must still be marked by the pool scan.
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// cage base + this offset lies beyond the 47-bit user address range, so a
// marker that reads through one of these words faults
static constexpr uint64_t UNMAPPED_OFFSET = UINT64_C(0x7FFFFFFFF000);
static constexpr uint64_t STR_HEAP_TAG_UNUSED = 0x3;

static bool fail_realloc;
static unsigned failed_reallocs;

static void *FailingRealloc(void *ptr, size_t size) {
  if (fail_realloc) { failed_reallocs++; return NULL; }
  return realloc(ptr, size);
}

// Inject the failure only into the rope collector, without runtime test hooks.
#define realloc FailingRealloc
#include "../src/gc/ropes.c"
#undef realloc

static ant_value_t eval(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
  return result;
}

// Untrusted words in a scanned frame: the fallback markers must ignore the
// rope, builder and unused tags, and must not read through the flat one.
__attribute__((noinline))
static void collect_without_rope_table(ant_t *js) {
  volatile uint64_t on_stack[4];
  static const uint64_t tags[] = {
    STR_HEAP_TAG_FLAT, STR_HEAP_TAG_ROPE, STR_HEAP_TAG_BUILDER, STR_HEAP_TAG_UNUSED
  };
  for (size_t i = 0; i < 4; i++)
    on_stack[i] = mkval(kTypeString, UNMAPPED_OFFSET | tags[i]);

  // drop the table so the collection has to allocate one, then refuse it
  free(js->rope_gc.marks);
  js->rope_gc.marks = NULL;
  js->rope_gc.mark_cap = 0;
  js->rope_gc.mark_count = 0;
  js->rope_gc.last_mark = NULL;

  failed_reallocs = 0;
  fail_realloc = true;
  gc_run(js);
  fail_realloc = false;

  assert(failed_reallocs > 0);
  assert(!js->rope_gc.conservative_marking);
  assert(on_stack[0] == mkval(kTypeString, UNMAPPED_OFFSET | STR_HEAP_TAG_FLAT));
}

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, NULL);

  eval(js,
    "globalThis.keep = (() => {"
    "  const leaves = [];"
    "  for (let i = 0; i < 96; i++) leaves.push('leaf-' + i + '-' + 'x'.repeat(48 + (i & 7)));"
    "  let appended = '';"
    "  for (const leaf of leaves) appended += leaf + '|';"
    "  const left = 'L'.repeat(300) + leaves.length, right = 'R'.repeat(300) + leaves.length;"
    "  return {"
    "    appended, pair: left + right, flat: 'heap-' + leaves.length, literal: 'literal',"
    "    expectedAppended: leaves.join('|') + '|',"
    "    expectedPair: 'L'.repeat(300) + '96' + 'R'.repeat(300) + '96'"
    "  };"
    "})(); 0;");

  volatile int stack_base = 0;
  js_setstackbase(js, (void *)&stack_base);
  collect_without_rope_table(js);
  js_setstackbase(js, NULL);

  // reuse whatever the fallback collection freed before looking at the strings
  eval(js,
    "{ const junk = [];"
    "  for (let i = 0; i < 40000; i++) junk.push('junk-' + i + '-' + 'y'.repeat(40 + (i & 15)));"
    "  globalThis.junkLength = junk.length; } 0;");

  const char *check =
    "keep.appended === keep.expectedAppended && keep.pair === keep.expectedPair &&"
    "keep.flat === 'heap-96' && keep.literal === 'literal' && junkLength === 40000";
  assert(js_truthy(js, eval(js, check)));

  // the next collections get their table back and must agree
  gc_run_minor(js);
  gc_run(js);
  assert(failed_reallocs > 0 && !fail_realloc);
  assert(js->rope_gc.mark_cap > 0);
  assert(js_truthy(js, eval(js, check)));

  js_destroy(js);
  puts("PASS a major collection without a rope mark table keeps ropes and their flat leaves");
  return 0;
}
