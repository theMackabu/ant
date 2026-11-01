// meson test -C build array-dense-length
#include "internal.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  char stack_base;
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, &stack_base);
  assert(ant_object_flag_masks_match_layout());
  GC_ROOT_SAVE(roots, js);
  ant_value_t array = Ant_Array_Create(js);
  GC_ROOT_PIN(js, array);
  ant_value_t length = js_mkstr(js, "length", 6);
  GC_ROOT_PIN(js, length);
  ant_object_t *obj = js_obj_ptr(array);
  assert(obj->flags.dense_length_fits);
  for (unsigned i = 0; i < 1000; i++) {
    js_arr_push(js, array, Ant_Value_FromNumber(i));
    assert(obj->flags.dense_length_fits);
    assert(obj->u.array.len <= obj->u.array.cap);
  }
  assert(vtype(js_setprop(js, array, length, Ant_Value_FromNumber(UINT32_MAX))) != kTypeError);
  assert(obj->u.array.len == UINT32_MAX);
  assert(!obj->flags.dense_length_fits);
  assert(Ant_Value_AsNumberUnchecked(js_arr_get(js, array, 999)) == 999);
  assert(vtype(js_setprop(js, array, length, Ant_Value_FromNumber(2))) != kTypeError);
  assert(obj->u.array.len == 2);
  assert(obj->flags.dense_length_fits);
  assert(vtype(js_arr_get(js, array, 999)) == kTypeUndefined);
  GC_ROOT_RESTORE(js, roots);
  Ant_Destroy(js);
  puts("PASS dense length flag survives growth, sparse length and truncation");
}
