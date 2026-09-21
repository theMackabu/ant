// meson test -C build gc-untrusted-string-words
//
// Conservative scanning hands the string markers raw words. A word can carry
// the string type tag and any of the four heap-tag values while pointing at
// nothing, so no marker may read through it before validating the address.
// Every word below decodes to an address outside user space: a marker that
// dereferences one faults, which fails this test.
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/strings.h"
#include <assert.h>
#include <stdio.h>

// cage base + this offset lies beyond the 47-bit user address range
static constexpr uint64_t UNMAPPED_OFFSET = UINT64_C(0x7FFFFFFFF000);
static constexpr uint64_t STR_HEAP_TAG_UNUSED = 0x3;

static uint64_t words[4];

static ant_value_t untrusted_string_word(uint64_t heap_tag) {
  return mkval(kTypeString, UNMAPPED_OFFSET | heap_tag);
}

static void fill_words(uint64_t *out) {
  out[0] = untrusted_string_word(STR_HEAP_TAG_FLAT);
  out[1] = untrusted_string_word(STR_HEAP_TAG_ROPE);
  out[2] = untrusted_string_word(STR_HEAP_TAG_BUILDER);
  out[3] = untrusted_string_word(STR_HEAP_TAG_UNUSED);
}

static void mark_untrusted_words(ant_t *js) {
  gc_mark_conservative_range(js, words, sizeof(words));
  // gc_mark_str pops tags it has no handler for instead of treating them as flat
  gc_mark_value(js, untrusted_string_word(STR_HEAP_TAG_UNUSED));
  gc_mark_str(js, untrusted_string_word(STR_HEAP_TAG_UNUSED));
}

// Keeps the words in this frame so the C-stack scan of a real collection,
// with its string and rope tables populated, walks over them.
__attribute__((noinline))
static void collect_with_words_on_stack(ant_t *js) {
  volatile uint64_t on_stack[4];
  fill_words((uint64_t *)on_stack);
  gc_run_minor(js);
  gc_run(js);
  assert(on_stack[0] == untrusted_string_word(STR_HEAP_TAG_FLAT));
  assert(on_stack[3] == untrusted_string_word(STR_HEAP_TAG_UNUSED));
}

int main(void) {
  static_assert((STR_HEAP_TAG_UNUSED & STR_HEAP_TAG_MASK) == STR_HEAP_TAG_UNUSED);
  static_assert(STR_HEAP_TAG_UNUSED != STR_HEAP_TAG_FLAT);
  static_assert(STR_HEAP_TAG_UNUSED != STR_HEAP_TAG_ROPE);
  static_assert(STR_HEAP_TAG_UNUSED != STR_HEAP_TAG_BUILDER);

  ant_t *js = ant_create();
  assert(js);

  fill_words(words);
  js_setstackbase(js, NULL);
  gc_objects_run(js, mark_untrusted_words);

  volatile int stack_base = 0;
  js_setstackbase(js, (void *)&stack_base);
  collect_with_words_on_stack(js);
  js_setstackbase(js, NULL);

  js_destroy(js);
  puts("PASS untrusted string words of every heap tag are never dereferenced");
  return 0;
}
