#ifndef ANT_OBJECT_H
#define ANT_OBJECT_H

#include "types.h"
#include "common.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct ant_object {
  struct ant_object *gc_next;
  uint8_t gc_mark;
  uint8_t type_tag;

  ant_value_t proto;
  ant_shape_t *shape;
  ant_value_t *prop;
  uint32_t prop_count;

  uint8_t extensible : 1;
  uint8_t frozen : 1;
  uint8_t sealed : 1;
  uint8_t is_exotic : 1;
  uint8_t is_constructor : 1;

  ant_value_t slots[SLOT_MAX + 1];
} ant_object_t;

typedef struct ant_prop_ref {
  ant_object_t *obj;
  uint32_t slot;
  bool valid;
} ant_prop_ref_t;

#endif
