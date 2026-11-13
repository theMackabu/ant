#pragma once

#include "types.h"
#include "object.h"

typedef void (*gc_parallel_scan_fn)(ant_t *js, ant_object_t *obj);

void gc_parallel_set_helpers(int count);
bool gc_parallel_available(void);
bool gc_parallel_drain(ant_t *js, uint32_t other_refs, gc_parallel_scan_fn scan);
