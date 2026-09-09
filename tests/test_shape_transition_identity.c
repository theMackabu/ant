// cc -std=gnu23 -Iinclude -Ivendor/uthash-2.3.0/src tests/test_shape_transition_identity.c src/shapes.c -o /tmp/test-shape-transitions
#include "shapes.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  char keys[256][16];
  ant_shape_t *first = ant_shape_new();
  ant_shape_t *second = ant_shape_new();
  ant_shape_t *prefix = NULL;
  for (unsigned i = 0; i < 256; i++) {
    snprintf(keys[i], sizeof(keys[i]), "property%u", i);
    assert(ant_shape_add_interned_tr(&first, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(ant_shape_add_interned_tr(&second, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(first == second);
    if (i == 7) { prefix = first; ant_shape_retain(prefix); }
  }
  assert(ant_shape_count(prefix) == 8);
  assert(ant_shape_lookup_interned(prefix, keys[8]) == -1);
  assert(ant_shape_prop_at(prefix, 8) == NULL);
  assert(ant_shape_prop_at(prefix, 0) == ant_shape_prop_at(first, 0));

  // Extending a prefix creates a separate branch without changing its sibling.
  ant_shape_t *branch = prefix;
  ant_shape_retain(branch);
  assert(ant_shape_add_interned_tr(&branch, "branch", ANT_PROP_ATTR_DEFAULT, NULL));
  assert(ant_shape_count(branch) == 9);
  assert(ant_shape_lookup_interned(first, "branch") == -1);
  assert(ant_shape_lookup_interned(branch, keys[8]) == -1);
  assert(ant_shape_prop_at(branch, 0) != ant_shape_prop_at(first, 0));

  // A metadata write also detaches a prefix from its descendant's backing.
  assert(ant_shape_set_attrs_interned(prefix, keys[0], 0));
  assert(ant_shape_get_attrs(prefix, 0) == 0);
  assert(ant_shape_get_attrs(first, 0) == ANT_PROP_ATTR_DEFAULT);
  assert(ant_shape_prop_at(prefix, 0) != ant_shape_prop_at(first, 0));

  // Mutating a detached shape never modifies a still-shared descriptor prefix.
  ant_shape_t *private_shape = ant_shape_clone(first);
  assert(private_shape);
  assert(ant_shape_set_attrs_interned(private_shape, keys[0], 0));
  assert(ant_shape_get_attrs(first, 0) == ANT_PROP_ATTR_DEFAULT);
  ant_shape_prop_t *accessor = ant_shape_prop_mut_at(private_shape, 1);
  assert(accessor);
  accessor->has_getter = 1;
  accessor->getter = 123;
  assert(!ant_shape_prop_at(first, 1)->has_getter);
  for (unsigned i = 2; i < 150; i++) assert(ant_shape_remove_slot(private_shape, i));
  assert(ant_shape_compact(private_shape) == 108);
  assert(ant_shape_lookup_interned(private_shape, keys[150]) == 2);
  assert(ant_shape_lookup_interned(first, keys[150]) == 150);
  ant_shape_release(private_shape);
  ant_shape_release(branch);
  ant_shape_release(second);
  ant_shape_release(first);

  // GC can prune a descendant while a prefix still owns the shared backing.
  ant_gc_shapes_begin();
  ant_gc_shapes_mark(prefix);
  ant_gc_shapes_sweep();
  assert(ant_shape_lookup_interned(prefix, keys[7]) == 7);
  assert(ant_shape_add_interned_tr(&prefix, "after_gc", ANT_PROP_ATTR_DEFAULT, NULL));
  assert(ant_shape_lookup_interned(prefix, keys[8]) == -1);
  ant_shape_release(prefix);

  first = ant_shape_new();
  second = ant_shape_new();
  for (unsigned i = 1; i <= 128; i++) {
    assert(ant_shape_add_symbol_tr(&first, i, ANT_PROP_ATTR_DEFAULT, NULL));
    assert(ant_shape_add_symbol_tr(&second, i, ANT_PROP_ATTR_DEFAULT, NULL));
    assert(first == second);
  }
  ant_shape_release(first);
  ant_shape_release(second);
  ant_gc_shapes_begin();
  ant_gc_shapes_sweep();

  // The bounded transition chain still falls back to independently mutable
  // shapes for very wide objects, without losing any descriptors.
  char wide_keys[1100][16];
  first = ant_shape_new();
  second = ant_shape_new();
  for (unsigned i = 0; i < 1100; i++) {
    snprintf(wide_keys[i], sizeof(wide_keys[i]), "wide%u", i);
    assert(ant_shape_add_interned_tr(&first, wide_keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(ant_shape_add_interned_tr(&second, wide_keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
  }
  assert(first != second);
  assert(ant_shape_lookup_interned(first, wide_keys[1099]) == 1099);
  assert(ant_shape_add_interned_tr(&first, "only_first", ANT_PROP_ATTR_DEFAULT, NULL));
  assert(ant_shape_lookup_interned(second, "only_first") == -1);
  ant_shape_release(first);
  ant_shape_release(second);
  ant_gc_shapes_begin();
  ant_gc_shapes_sweep();
  puts("PASS shared descriptor prefixes, branching, mutation and GC");
}
