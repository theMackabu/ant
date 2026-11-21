// meson test -C build array-shared-elements
#include "internal.h"
#include "arrays.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>

static double number_at(ant_t *js, ant_value_t array, ant_offset_t index) {
  ant_value_t value = js_arr_get(js, array, index);
  assert(vtype(value) == kTypeNumber);
  return Ant_Value_AsNumberUnchecked(value);
}

int main(void) {
  char stack_base;
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);
  GC_ROOT_SAVE(roots, js);

  ant_value_t values[4];
  for (unsigned i = 0; i < 4; i++) values[i] = Ant_Value_FromNumber(i + 1);
  gc_shared_elements_t *block = gc_shared_elements_alloc(js, values, 4);
  assert(block && block->refs == 1 && block->count == 4 && !block->has_holes && !block->has_heap);

  ant_value_t length = js_mkstr(js, "length", 6);
  GC_ROOT_PIN(js, length);

  ant_value_t first = Ant_Array_CreateFromBlock(js, block, false);
  GC_ROOT_PIN(js, first);
  ant_value_t second = Ant_Array_CreateFromBlock(js, block, false);
  GC_ROOT_PIN(js, second);
  ant_object_t *a = js_obj_ptr(first);
  ant_object_t *b = js_obj_ptr(second);
  assert(ant_array_is_shared(a) && ant_array_is_shared(b));
  assert(a->u.array.data == block->values && b->u.array.data == block->values);
  assert(block->refs == 3);

  assert(vtype(js_setprop(js, first, length, Ant_Value_FromNumber(0))) != kTypeError);
  assert(ant_array_is_shared(a));
  assert(a->u.array.len == 0 && a->u.array.cap == 0);
  assert(block->refs == 3);

  arr_set(js, first, 0, Ant_Value_FromNumber(9));
  assert(!ant_array_is_shared(a));
  assert(block->refs == 2);
  assert(a->u.array.data && a->u.array.data != block->values);
  assert(a->u.array.cap > 0);
  assert(get_dense_buf(first) != 0);
  assert(a->flags.fast_array && a->flags.extensible && !a->flags.frozen && !a->flags.sealed);
  assert(a->u.array.len == 1 && a->flags.dense_length_fits);
  assert(Ant_Value_AsNumberUnchecked(a->u.array.data[0]) == 9);
  for (uint32_t i = 1; i < a->u.array.cap; i++) assert(is_empty_slot(a->u.array.data[i]));

  js_arr_push(js, first, Ant_Value_FromNumber(8));
  assert(a->u.array.len == 2 && number_at(js, first, 1) == 8);

  assert(ant_array_is_shared(b) && b->u.array.len == 4);
  for (unsigned i = 0; i < 4; i++) assert(number_at(js, second, i) == i + 1);
  for (unsigned i = 0; i < 4; i++) assert(Ant_Value_AsNumberUnchecked(block->values[i]) == i + 1);

  assert(vtype(js_setprop(js, second, length, Ant_Value_FromNumber(0))) != kTypeError);
  assert(vtype(js_setprop(js, second, length, Ant_Value_FromNumber(3))) != kTypeError);
  assert(!ant_array_is_shared(b));
  assert(block->refs == 1);
  assert(b->u.array.len == 3 && b->u.array.cap > 0 && b->u.array.data != block->values);
  assert(vtype(js_arr_get(js, second, 0)) == kTypeUndefined);

  ant_value_t third = Ant_Array_CreateFromBlock(js, block, false);
  GC_ROOT_PIN(js, third);
  ant_object_t *c = js_obj_ptr(third);
  assert(!ant_array_is_shared(c));
  assert(c->u.array.len == 4 && number_at(js, third, 3) == 4);

  gc_run(js);
  assert(number_at(js, first, 0) == 9 && number_at(js, third, 0) == 1);

  GC_ROOT_RESTORE(js, roots);
  gc_run(js);
  gc_shared_elements_release(js, block);
  Ant_Destroy(js);
  puts("PASS shared element blocks unshare into dense storage at any length");
}
