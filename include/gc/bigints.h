#ifndef ANT_GC_BIGINTS_H
#define ANT_GC_BIGINTS_H

#include "types.h"

void gc_bigints_begin(ant_t *js);
void gc_bigints_mark(ant_t *js, const void *ptr);
void gc_bigints_destroy(ant_t *js);
void gc_bigints_sweep(ant_t *js);

#endif
