#ifndef ANT_GC_ROPES_H
#define ANT_GC_ROPES_H

#include "types.h"
#include <stdbool.h>

void gc_ropes_begin(ant_t *js);
void gc_ropes_sweep(ant_t *js);
bool gc_ropes_mark(const void *ptr);

#endif
