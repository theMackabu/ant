// meson test -C build shape-transition-identity
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
  // Computed-key stores share only a short prefix, then go private so
  // dictionaries do not pin long chains. Named stores keep sharing.
  char keyed_keys[48][16];
  first = ant_shape_new();
  second = ant_shape_new();
  for (unsigned i = 0; i < 48; i++) {
    snprintf(keyed_keys[i], sizeof(keyed_keys[i]), "keyed%u", i);
    assert(ant_shape_add_interned_keyed_tr(&first, keyed_keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(ant_shape_add_interned_keyed_tr(&second, keyed_keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    if (i < 32) assert(first == second);
    else assert(first != second);
    assert(ant_shape_lookup_interned(first, keyed_keys[i]) == (int32_t)i);
    assert(ant_shape_lookup_interned(second, keyed_keys[i]) == (int32_t)i);
  }
  assert(!ant_shape_is_shared(first));
  assert(ant_shape_add_interned_tr(&first, "named_after_keyed", ANT_PROP_ATTR_DEFAULT, NULL));
  assert(ant_shape_lookup_interned(second, "named_after_keyed") == -1);
  ant_shape_release(first);
  ant_shape_release(second);
  ant_gc_shapes_begin();
  ant_gc_shapes_sweep();

  // Keyed stores reuse a named layout beyond their new-transition limit.
  // Exercise both key kinds, then fork on a key with no existing transition.
  for (unsigned symbols = 0; symbols < 2; symbols++) {
    first = ant_shape_new();
    second = ant_shape_new();
    for (unsigned i = 0; i < 64; i++) {
      assert(symbols
        ? ant_shape_add_symbol_tr(&first, i + 1, ANT_PROP_ATTR_DEFAULT, NULL)
        : ant_shape_add_interned_tr(&first, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    }
    for (unsigned i = 0; i < 64; i++) {
      assert(symbols
        ? ant_shape_add_symbol_keyed_tr(&second, i + 1, ANT_PROP_ATTR_DEFAULT, NULL)
        : ant_shape_add_interned_keyed_tr(&second, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    }
    assert(first == second);
    assert(symbols
      ? ant_shape_add_symbol_keyed_tr(&second, 65, ANT_PROP_ATTR_DEFAULT, NULL)
      : ant_shape_add_interned_keyed_tr(&second, keys[64], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(first != second);
    assert(!ant_shape_is_shared(second));
    assert(ant_shape_count(first) == 64);
    assert(ant_shape_count(second) == 65);
    ant_shape_release(first);
    ant_shape_release(second);
    ant_gc_shapes_begin();
    ant_gc_shapes_sweep();
  }

  // Fan-out: the first child is stored inline, later ones in the table.
  // Every child must be found, and pruning any subset must leave the rest.
  ant_shape_t *root = ant_shape_new();
  ant_shape_t *children[5];
  const char *fan_keys[5] = { "fan0", "fan1", "fan2", "fan3", "fan4" };
  for (unsigned i = 0; i < 5; i++) {
    children[i] = root;
    ant_shape_retain(children[i]);
    assert(ant_shape_add_interned_tr(&children[i], fan_keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
  }
  for (unsigned i = 0; i < 5; i++) {
    ant_shape_t *again = root;
    ant_shape_retain(again);
    assert(ant_shape_add_interned_tr(&again, fan_keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(again == children[i]);
    ant_shape_release(again);
  }
  ant_shape_t *grandchild = children[0];
  ant_shape_retain(grandchild);
  assert(ant_shape_add_interned_tr(&grandchild, "deep", ANT_PROP_ATTR_DEFAULT, NULL));
  ant_shape_release(children[0]);
  ant_shape_release(children[2]);
  ant_gc_shapes_begin();
  ant_gc_shapes_mark(grandchild);
  ant_gc_shapes_mark(children[1]);
  ant_gc_shapes_mark(children[3]);
  ant_gc_shapes_mark(children[4]);
  ant_gc_shapes_sweep();
  for (unsigned i = 0; i < 5; i++) {
    if (i == 2) continue;
    ant_shape_t *again = root;
    ant_shape_retain(again);
    assert(ant_shape_add_interned_tr(&again, fan_keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(ant_shape_lookup_interned(again, fan_keys[i]) == 0);
    if (i != 0) assert(again == children[i]);
    ant_shape_release(again);
  }
  ant_shape_release(grandchild);
  for (unsigned i = 1; i < 5; i++) if (i != 2) ant_shape_release(children[i]);
  ant_shape_release(root);
  ant_gc_shapes_begin();
  ant_gc_shapes_sweep();
  // A collected tail no longer forces surviving prefixes to copy their table.
  prefix = ant_shape_new();
  for (unsigned i = 0; i < 8; i++)
    assert(ant_shape_add_interned_tr(&prefix, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
  first = prefix;
  ant_shape_retain(first);
  for (unsigned i = 8; i < 12; i++)
    assert(ant_shape_add_interned_tr(&first, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
  ant_shape_release(first);
  ant_gc_shapes_begin();
  ant_gc_shapes_mark(prefix);
  ant_gc_shapes_sweep();
  const ant_shape_prop_t *backing = ant_shape_prop_at(prefix, 0);
  first = prefix;
  ant_shape_retain(first);
  // Reusing a dead tail key also verifies removal of stale index entries.
  assert(ant_shape_add_interned_tr(&first, keys[10], ANT_PROP_ATTR_DEFAULT, NULL));
  assert(ant_shape_prop_at(first, 0) == backing);
  assert(ant_shape_prop_at(prefix, 0) == backing);
  assert(ant_shape_lookup_interned(first, keys[10]) == 8);
  assert(ant_shape_lookup_interned(prefix, keys[10]) == -1);
  assert(ant_shape_lookup_interned(first, keys[8]) == -1);
  ant_shape_release(first);
  ant_shape_release(prefix);
  ant_gc_shapes_begin();
  ant_gc_shapes_sweep();

  // Branch reservation includes the index capacity needed by the next append.
  first = ant_shape_new();
  for (unsigned i = 0; i < 32; i++) {
    ant_shape_t *copy = shape_clone_reserve(first, 1);
    assert(copy);
    size_t reserved = ant_shape_storage_bytes(copy);
    assert(ant_shape_add_interned(copy, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(ant_shape_storage_bytes(copy) == reserved);
    ant_shape_release(copy);
    assert(ant_shape_add_interned_tr(&first, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
  }
  ant_shape_release(first);
  ant_gc_shapes_begin();
  ant_gc_shapes_sweep();
  puts("PASS shared descriptors, tail reclamation, index reservation, mutation and GC");
}
