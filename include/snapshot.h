#ifndef ANT_SNAPSHOT_LOADER_H
#define ANT_SNAPSHOT_LOADER_H

#include <stddef.h>
#include "types.h"

jsval_t ant_load_snapshot(ant_t *js);
const uint8_t *ant_get_snapshot_source(size_t *len);

#endif