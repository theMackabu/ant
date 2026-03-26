#ifndef STREAMS_QUEUING_H
#define STREAMS_QUEUING_H

#include "types.h"

void init_queuing_strategies_module(void);
void gc_mark_queuing_strategies(ant_t *js, void (*mark)(ant_t *, ant_value_t));

#endif
