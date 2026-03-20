#ifndef ANT_GC_STRINGS_H
#define ANT_GC_STRINGS_H

#include "types.h"
#include <stdbool.h>

void gc_strings_begin(ant_t *js);
void gc_strings_mark(ant_t *js, const void *ptr);
void gc_strings_sweep(ant_t *js);

#endif
