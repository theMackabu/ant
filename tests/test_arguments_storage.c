// meson test -C build arguments-storage
#include "internal.h"
#include "silver/engine.h"
#include "gc/roots.h"
#include "modules/symbol.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, NULL);
  assert(ant_object_flag_masks_match_layout());
  sv_frame_t empty_frame = {0};
  ant_value_t early = js_create_arguments_object(js, js->vm, js_mkundef(), &empty_frame, 0, 0, true);
  assert(!is_err(early) && js_get_slot(early, SLOT_STRICT_ARGS) == js_true);
  init_symbol_module(js);
  const uint32_t counts[] = {0, 1, 2, 7, 8, 9, 33};
  ant_value_t values[33];
  for (uint32_t i = 0; i < 33; i++) values[i] = js_mknum(i);
  for (size_t test = 0; test < sizeof counts / sizeof *counts; test++) {
    uint32_t count = counts[test];
    sv_frame_t frame = {.bp = values, .argc = (int)count};
    GC_ROOT_SAVE(roots, js);
    size_t before = js->alloc_bytes.arrays;
    ant_value_t args = js_create_arguments_object(js, js->vm, js_mkundef(), &frame, (int)count, 0, true);
    assert(!is_err(args));
    GC_ROOT_PIN(js, args);
    ant_object_t *ptr = js_obj_ptr(args);
    uint32_t capacity = count ? count : 1;
    assert(ptr->u.array.cap == capacity && ptr->u.array.len == count);
    assert(js->alloc_bytes.arrays - before == capacity * sizeof(ant_value_t));
    assert(ptr->extra_slots == NULL && ptr->flags.strict_arguments);
    assert(js_get_slot(args, SLOT_STRICT_ARGS) == js_true);
    for (uint32_t i = 0; i < count; i++) assert(js_arr_get(js, args, i) == values[i]);
    js_arr_push(js, args, js_mknum(99));
    assert(js_arr_get(js, args, count) == js_mknum(99));
    assert(ptr->u.array.cap >= count + 1);

    // Preserve the internal slot API for non-marker values as well.
    ant_value_t payload = js_mkobj(js);
    js_set_slot_wb(js, args, SLOT_STRICT_ARGS, payload);
    assert(js_get_slot(args, SLOT_STRICT_ARGS) == payload);
    js_set_slot(args, SLOT_STRICT_ARGS, js_true);
    assert(js_get_slot(args, SLOT_STRICT_ARGS) == js_true);
    GC_ROOT_RESTORE(js, roots);
  }
  js_destroy(js);
  puts("PASS compact arguments capacity, growth and inline strict marker");
}
