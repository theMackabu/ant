// meson test -C build gc-shape-refs
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "gc/weak.h"
#include <assert.h>
#include <stdio.h>

static void check_prefix_copy(void) {
  ant_shape_t *shape = ant_shape_new();
  assert(ant_shape_add_interned_tr(&shape, "gc_plain_prefix", ANT_PROP_ATTR_DEFAULT, NULL));
  assert(!ant_shape_may_have_gc_refs(shape));
  ant_shape_t *prefix = shape;
  ant_shape_retain(prefix);
  assert(ant_shape_add_symbol_tr(&shape, 1234, ANT_PROP_ATTR_DEFAULT, NULL));
  assert(ant_shape_may_have_gc_refs(shape));
  ant_shape_t *copy = ant_shape_clone(prefix);
  assert(copy && !ant_shape_may_have_gc_refs(copy));
  ant_shape_prop_t *prop = ant_shape_prop_mut_at(copy, 0);
  assert(prop && ant_shape_may_have_gc_refs(copy));
  prop->has_getter = 1;
  prop->getter = js_mknum(42);
  ant_shape_t *with_accessor = ant_shape_clone(copy);
  assert(with_accessor && ant_shape_may_have_gc_refs(with_accessor));
  assert(ant_shape_clear_accessor_slot(with_accessor, 0));
  ant_shape_t *cleared = ant_shape_clone(with_accessor);
  assert(cleared && !ant_shape_may_have_gc_refs(cleared));
  ant_shape_release(cleared);
  ant_shape_release(with_accessor);
  ant_shape_release(copy);
  ant_shape_release(shape);
  ant_shape_release(prefix);
}

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  check_prefix_copy();
  // Only explicit roots keep these objects alive. Conservative C-stack copies
  // must not hide a missing ordinary-value or descriptor edge.
  js_setstackbase(js, NULL);
  GC_ROOT_SAVE(roots, js);
  ant_value_t owner = js_mkobj(js);
  GC_ROOT_PIN(js, owner);
  ant_value_t ordinary = js_mkobj(js);
  js_set(js, owner, "gc_ordinary_value", ordinary);
  ant_object_t *ptr = js_obj_ptr(owner);
  assert(!ant_shape_may_have_gc_refs(ptr->shape));
  gc_run(js);
  assert(gc_obj_is_marked(js_obj_ptr(ordinary)));

  js_set(js, owner, "gc_accessor", js_mkundef());
  int32_t slot = ant_shape_lookup_interned(ptr->shape, intern_string("gc_accessor", 11));
  assert(slot >= 0);
  ant_shape_prop_t *prop = ant_shape_prop_mut_at(ptr->shape, (uint32_t)slot);
  assert(prop && ant_shape_may_have_gc_refs(ptr->shape));
  // Object markers exercise the descriptor edges without invoking accessors.
  ant_value_t getter = js_mkobj(js), setter = js_mkobj(js);
  prop->has_getter = prop->has_setter = 1;
  prop->getter = getter;
  prop->setter = setter;
  ant_value_t symbol = js_mksym(js, "gc_shape_key");
  assert(!is_err(js_setprop(js, owner, symbol, js_mknum(7))));
  gc_run(js);
  assert(gc_obj_is_marked(js_obj_ptr(ordinary)));
  assert(gc_obj_is_marked(js_obj_ptr(getter)));
  assert(gc_obj_is_marked(js_obj_ptr(setter)));
  assert(js_symbol_gc_is_marked(symbol, gc_get_epoch()));

  ant_value_t young = js_mkobj(js);
  slot = ant_shape_lookup_interned(ptr->shape, intern_string("gc_accessor", 11));
  prop = ant_shape_prop_mut_at(ptr->shape, (uint32_t)slot);
  assert(prop);
  prop->getter = young;
  gc_write_barrier(js, ptr, young);
  gc_run_minor(js);
  assert(gc_obj_is_marked(js_obj_ptr(young)));
  GC_ROOT_RESTORE(js, roots);
  js_destroy(js);
  puts("PASS GC preserves ordinary values, descriptor references and copied prefixes");
}
