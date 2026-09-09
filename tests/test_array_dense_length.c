// meson test -C build array-dense-length
#include "internal.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);
  assert(ant_object_flag_masks_match_layout());
  GC_ROOT_SAVE(roots, js);
  ant_value_t array = js_mkarr(js);
  GC_ROOT_PIN(js, array);
  ant_value_t length = js_mkstr(js, "length", 6);
  GC_ROOT_PIN(js, length);
  ant_object_t *obj = js_obj_ptr(array);
  assert(obj->flags.dense_length_fits);
  for (unsigned i = 0; i < 1000; i++) {
    js_arr_push(js, array, js_mknum(i));
    assert(obj->flags.dense_length_fits);
    assert(obj->u.array.len <= obj->u.array.cap);
  }
  assert(vtype(js_setprop(js, array, length, js_mknum(UINT32_MAX))) != kTypeError);
  assert(obj->u.array.len == UINT32_MAX);
  assert(!obj->flags.dense_length_fits);
  assert(js_getnum(js_arr_get(js, array, 999)) == 999);
  assert(vtype(js_setprop(js, array, length, js_mknum(2))) != kTypeError);
  assert(obj->u.array.len == 2);
  assert(obj->flags.dense_length_fits);
  assert(vtype(js_arr_get(js, array, 999)) == kTypeUndefined);
  GC_ROOT_RESTORE(js, roots);
  js_destroy(js);
  puts("PASS dense length flag survives growth, sparse length and truncation");
}
