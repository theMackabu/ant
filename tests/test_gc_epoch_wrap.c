#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>

static bool alive(ant_t *js, ant_value_t value) {
  return js_obj_ptr(value)->mark_epoch != ANT_GC_DEAD;
}

static uint8_t next_epoch(uint8_t epoch) {
  epoch++;
  return epoch == 0 || epoch == ANT_GC_DEAD ? 1 : epoch;
}

static void minors_between_majors(ant_t *js, unsigned gap) {
  GC_ROOT_SAVE(roots, js);
  ant_value_t root = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, root);
  ant_value_t parents = Ant_Array_Create(js);
  js_set(js, root, "parents", parents);
  gc_run(js);

  for (unsigned i = 0; i < gap; i++) {
    ant_value_t parent = Ant_Object_CreateBare(js);
    js_arr_push(js, parents, parent);
    gc_run_minor(js);
    js_set(js, parent, "child", Ant_Object_CreateBare(js));
  }

  gc_run(js);
  assert(gc_obj_is_marked(js, js_obj_ptr(root)));
  assert(gc_obj_is_marked(js, js_obj_ptr(parents)));
  for (unsigned i = 0; i < gap; i++) {
    ant_value_t parent = js_arr_get(js, parents, i);
    ant_value_t child = js_get(js, parent, "child");
    assert(gc_obj_is_marked(js, js_obj_ptr(parent)));
    assert(is_object_type(child) && alive(js, child) && gc_obj_is_marked(js, js_obj_ptr(child)));
  }
  GC_ROOT_RESTORE(js, roots);
}

static void stale_permanent_mark(ant_t *js) {
  GC_ROOT_SAVE(roots, js);
  ant_value_t holder = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, holder);
  ant_value_t permanent = Ant_Object_CreateBare(js);
  js_set(js, holder, "permanent", permanent);
  gc_run(js);
  uint8_t stale = js->gc.obj_epoch;

  js_set(js, holder, "permanent", Ant_Value_Undefined);
  gc_pin_existing_objects(js);

  for (unsigned n = 0; next_epoch(js->gc.obj_epoch) != stale; n++) {
    if (n % 10 == 9) gc_run(js);
    else gc_run_minor(js);
  }

  ant_value_t child = Ant_Object_CreateBare(js);
  js_set(js, permanent, "child", child);
  js_set(js, holder, "permanent", permanent);
  gc_run(js);
  assert(js->gc.obj_epoch == stale);
  assert(alive(js, child) && gc_obj_is_marked(js, js_obj_ptr(child)));
  GC_ROOT_RESTORE(js, roots);
}

int main(void) {
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, NULL);

  for (unsigned gap = 248; gap <= 262; gap++) minors_between_majors(js, gap);

  stale_permanent_mark(js);

  Ant_Destroy(js);
  puts("PASS mark epochs never collide with live objects across 8-bit wraps");
}
