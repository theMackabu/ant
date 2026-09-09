#include "ant.h"
#include "object.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
  assert(ant_object_flag_masks_match_layout());
  ant_object_t prototype = {.shape = ant_shape_new()};
  assert(prototype.shape);
  uint32_t epoch = ant_ic_epoch_counter;

  // Re-probing an empty intermediate prototype must not poison the shared root
  // shape when unrelated objects take either new or cached property transitions.
  for (int i = 0; i < 1000; i++) {
    ant_object_guard_absence(&prototype);
    ant_object_t sibling = {.shape = ant_shape_new()};
    assert(sibling.shape == prototype.shape);
    assert(ant_shape_add_interned_tr(&sibling.shape, "x", ANT_PROP_ATTR_DEFAULT, NULL));
    ant_object_invalidate_guarded_absence(&sibling);
    assert(ant_ic_epoch_counter == epoch);
    assert(prototype.flags.guards_absence);
    ant_shape_release(sibling.shape);
  }

  prototype.flags.generation = 1;
  prototype.flags.in_remember_set = 1;
  assert(ant_shape_add_interned_tr(&prototype.shape, "x", ANT_PROP_ATTR_DEFAULT, NULL));
  ant_object_invalidate_guarded_absence(&prototype);
  assert(ant_ic_epoch_counter == epoch + 1);
  assert(!prototype.flags.guards_absence);
  assert(prototype.flags.generation && prototype.flags.in_remember_set);
  ant_object_invalidate_guarded_absence(&prototype);
  assert(ant_ic_epoch_counter == epoch + 1);

  ant_object_guard_absence(&prototype);
  ant_object_invalidate_guarded_absence(&prototype);
  assert(ant_ic_epoch_counter == epoch + 2);
  ant_ic_epoch_counter = UINT32_MAX;
  ant_object_guard_absence(&prototype);
  ant_object_invalidate_guarded_absence(&prototype);
  assert(ant_ic_epoch_counter == 1);
  ant_shape_release(prototype.shape);
  puts("PASS object-precise absence guard");
  return 0;
}
