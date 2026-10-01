#include "gc/verify.h"

#ifdef ANT_GC_VERIFY

#include "gc.h"
#include "silver/engine.h"
#include <stdlib.h>
#include <stdio.h>

void gc_verify_stress(ant_t *js) {
  static long every = -1;
  static long tick = 0;
  
  if (every < 0) {
    const char *v = getenv("ANT_GC_STRESS");
    every = v ? atol(v) : 0;
  }
  
  if (every <= 0 || gc_disabled || js->gc_running || js->gc_objects_running || !js->cstk.base) return;
  if (++tick % every != 0) return;
  
  gc_run(js);
}

void gc_verify_poison_object(ant_object_t *obj) {
  ant_value_t bad = mkval(kTypeObject, UINT64_C(0x7FFFFFFFF000));
  
  obj->proto = bad;
  obj->shape = NULL;
  obj->overflow_prop = (ant_value_t *)(uintptr_t)0xdead0001;
  
  for (size_t i = 0; i < ANT_INOBJ_MAX_SLOTS; i++) obj->inobj[i] = bad;
  
  obj->u.array.data = (ant_value_t *)(uintptr_t)0xdead0002;
  obj->u.array.len = 0x7fffffff;
  obj->prop_count = 0x7fffffff;
}

static bool gc_verify_ref_is_young(ant_t *js, ant_value_t v) {
  if (!gc_value_is_heap_ref(v)) return false;
  if (vtype(v) != kTypeFunction) return gc_value_ref_is_young(v);
  sv_closure_t *closure = js_func_closure(v);
  return closure && fixed_arena_contains(&js->closure_arena, closure) && closure->generation == 0;
}

static const gc_card_table_t *gc_verify_scanned_cards(const ant_object_t *obj) {
  if (obj->type_tag != kTypeArray || !obj->u.array.data) return NULL;
  const gc_card_table_t *cards = gc_cards_of(obj);
  return cards && !cards->all_dirty ? cards : NULL;
}

static void gc_verify_array_cards(ant_t *js, const ant_object_t *arr, const gc_card_table_t *cards) {
  uint32_t n = arr->u.array.len < arr->u.array.cap ? arr->u.array.len : arr->u.array.cap;
  for (uint32_t slot = 0; slot < n; slot++) {
    if (!gc_verify_ref_is_young(js, arr->u.array.data[slot])) continue;

    uint32_t card = slot >> GC_CARD_SHIFT;
    if (card < cards->ncards && gc_card_is_set(cards, card)) continue;

    fprintf(stderr, "gc verify: young reference in clean card: array %p slot %u card %u\n", (void *)arr, slot, card);
    abort();
  }
}

void gc_verify_cards(ant_t *js) {
  for (size_t i = 0; i < js->remember_set_len; i++) {
    ant_object_t *obj = js->remember_set[i];
    const gc_card_table_t *cards = gc_verify_scanned_cards(obj);
    if (cards) gc_verify_array_cards(js, obj, cards);
  }
}

#endif
