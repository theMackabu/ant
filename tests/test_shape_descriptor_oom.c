// cc -std=gnu23 -Iinclude -Ivendor/uthash-2.3.0/src tests/test_shape_descriptor_oom.c -o /tmp/test-shape-oom
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static unsigned fail_at;
static unsigned allocation;
static bool fail_allocation(void) { return fail_at && ++allocation == fail_at; }
static void *test_malloc(size_t size) { return fail_allocation() ? NULL : malloc(size); }
static void *test_calloc(size_t count, size_t size) { return fail_allocation() ? NULL : calloc(count, size); }
static void *test_realloc(void *ptr, size_t size) { return fail_allocation() ? NULL : realloc(ptr, size); }
// uthash has a separate fatal-OOM policy; inject only recoverable shape failures.
static void *hash_malloc(size_t size) { return malloc(size); }

#define uthash_malloc(size) hash_malloc(size)
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#include "../src/shapes.c"
#undef malloc
#undef calloc
#undef realloc

int main(void) {
  char keys[24][16];
  ant_shape_t *root = ant_shape_new();
  ant_shape_release(root);
  size_t empty_bytes = ant_shape_total_bytes();
  for (unsigned failure = 1; failure <= 24; failure++) {
    ant_shape_t *prefix = ant_shape_new();
    for (unsigned i = 0; i < 24; i++) {
      snprintf(keys[i], sizeof(keys[i]), "key%u", i);
      if (i < 8) assert(ant_shape_add_interned_tr(&prefix, keys[i], ANT_PROP_ATTR_DEFAULT, NULL));
    }
    ant_shape_t *child = prefix;
    ant_shape_retain(child);
    allocation = 0;
    fail_at = failure;
    bool added = ant_shape_add_interned_tr(&child, keys[8], ANT_PROP_ATTR_DEFAULT, NULL);
    fail_at = 0;
    assert(ant_shape_count(prefix) == 8);
    assert(ant_shape_lookup_interned(prefix, keys[8]) == -1);
    assert(ant_shape_count(child) == (added ? 9 : 8));
    if (!added) assert(child == prefix);
    assert(ant_shape_add_interned_tr(&child, keys[9], ANT_PROP_ATTR_DEFAULT, NULL));
    for (unsigned i = 0; i < 8; i++) {
      assert(ant_shape_lookup_interned(prefix, keys[i]) == (int32_t)i);
      assert(ant_shape_lookup_interned(child, keys[i]) == (int32_t)i);
    }
    allocation = 0;
    fail_at = failure;
    bool changed = ant_shape_set_attrs_interned(prefix, keys[0], 0);
    fail_at = 0;
    assert(ant_shape_get_attrs(prefix, 0) == (changed ? 0 : ANT_PROP_ATTR_DEFAULT));
    assert(ant_shape_get_attrs(child, 0) == ANT_PROP_ATTR_DEFAULT);
    assert(ant_shape_lookup_interned(prefix, keys[9]) == -1);
    ant_shape_release(child);
    ant_shape_release(prefix);
    ant_gc_shapes_begin();
    ant_gc_shapes_sweep();
    assert(ant_shape_total_bytes() == empty_bytes);
  }
  puts("PASS descriptor append allocation failures preserve prefixes and ownership");
}
