// meson test -C build gc-exact-edges
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>

static uint64_t conservative_words[2];

static void mark_conservative_words(ant_t *js) {
  gc_mark_conservative_range(js, conservative_words, sizeof(conservative_words));
}

int main(void) {
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  // Exact heap edges must retain children without help from C-stack copies.
  js_setstackbase(js, NULL);
  GC_ROOT_SAVE(roots, js);
  ant_value_t root = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, root);
  ant_value_t array = Ant_Array_Create(js);
  js_set(js, root, "children", array);
  ant_value_t children[32];
  for (unsigned i = 0; i < 32; i++) {
    children[i] = Ant_Object_CreateBare(js);
    js_set(js, children[i], "parent", root);
    js_arr_push(js, array, children[i]);
  }
  gc_run(js);
  assert(gc_obj_is_marked(js, js_obj_ptr(array)));
  for (unsigned i = 0; i < 32; i++)
    assert(gc_obj_is_marked(js, js_obj_ptr(children[i])));

  // The old array's remembered edge must still retain a newly allocated child.
  ant_value_t young = Ant_Object_CreateBare(js);
  js_arr_push(js, array, young);
  gc_run_minor(js);
  assert(gc_obj_is_marked(js, js_obj_ptr(young)));
  gc_run(js);
  assert(gc_obj_is_marked(js, js_obj_ptr(young)));

  // Raw and tagged conservative candidates still take the validated path.
  ant_value_t raw_root = Ant_Object_CreateBare(js), tagged_root = Ant_Object_CreateBare(js);
  ant_value_t raw_child = Ant_Object_CreateBare(js), tagged_child = Ant_Object_CreateBare(js);
  js_set(js, raw_root, "child", raw_child);
  js_set(js, tagged_root, "child", tagged_child);
  conservative_words[0] = (uintptr_t)js_obj_ptr(raw_root);
  conservative_words[1] = tagged_root;
  gc_objects_run(js, mark_conservative_words);
  assert(gc_obj_is_marked(js, js_obj_ptr(raw_root)));
  assert(gc_obj_is_marked(js, js_obj_ptr(tagged_root)));
  assert(gc_obj_is_marked(js, js_obj_ptr(raw_child)));
  assert(gc_obj_is_marked(js, js_obj_ptr(tagged_child)));
  conservative_words[0] = conservative_words[1] = 0;

  GC_ROOT_RESTORE(js, roots);
  Ant_Destroy(js);
  puts("PASS exact object/array edges, remembered children and conservative roots");
}
