// meson test -C build gc-array-cards
//
// Card marking for old arrays (see gc_card_table_t): element stores of young
// references dirty only their card, a minor scans those cards and clears
// them, and every store or move that could put a young reference outside a
// dirty card makes the whole table dirty instead.
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static gc_card_table_t *cards_of(ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? sidecar->gc_cards : NULL;
}

static bool card_is_set(const gc_card_table_t *cards, uint32_t card) {
  return card < cards->ncards && ((cards->bits[card / 64u] >> (card % 64u)) & 1u);
}

static unsigned dirty_cards(const gc_card_table_t *cards) {
  unsigned n = 0;
  for (uint32_t card = 0; card < cards->ncards; card++) n += card_is_set(cards, card);
  return n;
}

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
}

static ant_object_t *global_object(ant_t *js, const char *name) {
  ant_value_t value = js_get(js, js->global, name);
  assert(vtype(value) == kTypeArray || vtype(value) == kTypeObject);
  return js_obj_ptr(value);
}

// the element holds an intact object whose tag field is `tag`
static void check_tag(ant_t *js, const char *array_name, uint32_t idx, double tag) {
  ant_value_t element = js_arr_get(js, js_get(js, js->global, array_name), idx);
  assert(vtype(element) == kTypeObject);
  assert(js_obj_ptr(element)->mark_epoch != ANT_GC_DEAD);
  ant_value_t value = js_get(js, element, "tag");
  assert(vtype(value) == kTypeNumber && js_getnum(value) == tag);
}

static ant_object_t *old_array(ant_t *js, const char *name, unsigned length) {
  char source[128];
  snprintf(source, sizeof(source),
    "globalThis.%s = []; for (let i = 0; i < %u; i++) %s.push(i);", name, length, name);
  run(js, source);
  gc_run(js); // a major promotes every survivor
  ant_object_t *arr = global_object(js, name);
  assert(arr->flags.generation == 1 && !arr->flags.in_remember_set && !cards_of(arr));
  return arr;
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);

  ant_object_t *big = old_array(js, "big", 4096);
  assert(big->u.array.cap >= GC_CARD_MIN_CAP);

  // element stores dirty only their own cards
  run(js, "big[5] = {tag: 1}; big[1000] = {tag: 2}; big[3000] = {tag: 3};");
  gc_card_table_t *cards = cards_of(big);
  assert(cards && !cards->all_dirty && big->flags.in_remember_set);
  assert(dirty_cards(cards) == 3);
  assert(card_is_set(cards, 5 >> GC_CARD_SHIFT));
  assert(card_is_set(cards, 1000 >> GC_CARD_SHIFT));
  assert(card_is_set(cards, 3000 >> GC_CARD_SHIFT));

  // a minor keeps what the cards cover and clears them
  gc_run_minor(js);
  check_tag(js, "big", 5, 1);
  check_tag(js, "big", 1000, 2);
  check_tag(js, "big", 3000, 3);
  cards = cards_of(big);
  assert(!big->flags.in_remember_set && dirty_cards(cards) == 0 && !cards->all_dirty);

  // a store that doesn't go through the element barrier dirties every card
  run(js, "big.named = {tag: 4};");
  assert(cards_of(big)->all_dirty);
  gc_run_minor(js);
  assert(!cards_of(big)->all_dirty);

  // moving elements dirties every card; the young value survives where it moved
  run(js, "big[2000] = {tag: 5}; big.shift();");
  assert(cards_of(big)->all_dirty);
  gc_run_minor(js);
  check_tag(js, "big", 1999, 5);

  run(js, "big[2500] = {tag: 6}; big.splice(10, 3);");
  assert(cards_of(big)->all_dirty);
  gc_run_minor(js);
  check_tag(js, "big", 2497, 6);

  run(js, "big[100] = {tag: 7}; big.unshift(0, 0);");
  assert(cards_of(big)->all_dirty);
  gc_run_minor(js);
  check_tag(js, "big", 102, 7);

  // a table created for an array already remembered as a whole starts dirty:
  // the earlier store left no card behind
  ant_object_t *late = old_array(js, "late", 2048);
  run(js, "late.named = {tag: 8}; late[7] = {tag: 9};");
  assert(cards_of(late) && cards_of(late)->all_dirty);
  gc_run_minor(js);
  check_tag(js, "late", 7, 9);

  // small arrays keep whole-object remembering
  ant_object_t *small = old_array(js, "small", 16);
  run(js, "small[3] = {tag: 10};");
  assert(small->flags.in_remember_set && !cards_of(small));
  gc_run_minor(js);
  check_tag(js, "small", 3, 10);

  js_destroy(js);
  puts("PASS old arrays remember young stores by card and fall back to full scans");
}
