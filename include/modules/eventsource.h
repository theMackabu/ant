#ifndef MODULES_EVENTSOURCE_H
#define MODULES_EVENTSOURCE_H

#include "types.h"
#include "gc/modules.h"

void init_eventsource_module(ant_t *js);
void gc_mark_eventsource(ant_t *js, gc_mark_fn mark);

#endif
