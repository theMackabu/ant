// meson test -C build gc-plain-arrays
#include "internal.h"
#include "gc/objects.h"
#include <assert.h>
#include <stdio.h>

static unsigned finalized;
static void test_finalizer(ant_t *js, ant_object_t *obj) {
  (void)js;
  assert(obj->type_tag == kTypeArray && obj->u.array.data);
  finalized++;
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);

  for (unsigned variant = 0; variant < 4; variant++) {
    ant_value_t array = js_mkarr(js);
    for (unsigned i = 0; i < 10; i++) js_arr_push(js, array, js_mknum(i));
    if (variant == 1) js_set_finalizer(array, test_finalizer);
    if (variant == 2) js_set_slot(array, SLOT_CTOR, js_mknum(42));
    if (variant == 3) {
      for (unsigned i = 0; i <= ANT_INOBJ_MAX_SLOTS; i++) {
        char key[16];
        int len = snprintf(key, sizeof(key), "property%u", i);
        ant_value_t result = js_setprop(js, array, js_mkstr(js, key, (size_t)len), js_mknum(i));
        assert(!is_err(result));
      }
    }
    ant_object_t *obj = js_obj_ptr(array);
    assert(obj && obj->u.array.len == 10 && obj->u.array.data);
    if (variant == 2) assert(obj->extra_slots);
    if (variant == 3) assert(obj->overflow_prop);
    size_t bytes = (size_t)obj->u.array.cap * sizeof(*obj->u.array.data);
    size_t before_bytes = js->alloc_bytes.arrays;
    size_t before_live = js->obj_arena.live_count;
    unsigned before_finalized = finalized;
    // Invoke the deallocator with the same list ownership as a sweep, without
    // relying on conservative-stack liveness to decide when this object dies.
    assert(js->objects == obj);
    js->objects = obj->next;
    gc_object_free(js, obj);
    assert(js->alloc_bytes.arrays == before_bytes - bytes);
    assert(js->obj_arena.live_count == before_live - 1);
    assert(finalized == before_finalized + (variant == 1));
  }
  js_destroy(js);
  puts("PASS array reclamation releases dense storage and preserves stateful cleanup");
}
