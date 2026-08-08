#ifndef ANT_GC_ROPES_H
#define ANT_GC_ROPES_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

bool gc_ropes_begin(ant_t *js, bool minor);
void gc_ropes_sweep(ant_t *js, bool minor);
bool gc_ropes_contains(ant_t *js, const void *ptr, size_t size, size_t align);
bool gc_ropes_mark(ant_t *js, const void *ptr);

#endif
