#pragma once

#include "types.h"

typedef enum {
#define PRIMORDIAL_DEF(name, owner, property, uncurry) ANT_PRIMORDIAL_##name,
#include "primordial_list.h"
  ANT_PRIMORDIAL_EXPORT_COUNT,
  ANT_PRIMORDIAL_CALL = ANT_PRIMORDIAL_EXPORT_COUNT,
  ANT_PRIMORDIAL_COUNT
} ant_primordial_id_t;

__attribute__((cold)) 
ant_value_t primordial_library(ant_t *js);
ant_value_t ant_capture_primordials(ant_t *js);
