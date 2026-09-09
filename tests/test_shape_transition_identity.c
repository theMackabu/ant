// cc -std=gnu23 -Iinclude -Ivendor/uthash-2.3.0/src tests/test_shape_transition_identity.c src/shapes.c -o /tmp/test-shape-transitions
#include "shapes.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  char keys[128][16];
  ant_shape_t *first = ant_shape_new();
  ant_shape_t *second = ant_shape_new();
  for (unsigned i = 0; i < 128; i++) {
    snprintf(keys[i], sizeof(keys[i]), "property%u", i);
    assert(ant_shape_add_interned_tr(&first, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    assert(ant_shape_add_interned_tr(&second, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    if (i < 8) assert(first == second);
  }
  // Wide objects leave the transition tree and can grow without changing peers.
  assert(first != second);
  ant_shape_t *wide = first;
  assert(ant_shape_add_interned_tr(&first, "extra", ANT_PROP_ATTR_DEFAULT, NULL));
  assert(first == wide);
  assert(ant_shape_lookup_interned(second, "extra") == -1);
  ant_shape_release(first);
  ant_shape_release(second);

  first = ant_shape_new();
  second = ant_shape_new();
  for (unsigned i = 1; i <= 8; i++) {
    assert(ant_shape_add_symbol_tr(&first, i, ANT_PROP_ATTR_DEFAULT, NULL));
    assert(ant_shape_add_symbol_tr(&second, i, ANT_PROP_ATTR_DEFAULT, NULL));
    assert(first == second);
  }
  ant_shape_release(first);
  ant_shape_release(second);
  ant_gc_shapes_begin();
  ant_gc_shapes_sweep();
  puts("PASS creator and subsequent objects share bounded shape transitions");
}
