#pragma once

#include "types.h"
#include "gc/modules.h"

__attribute__((cold)) 
void init_eventsource_module(ant_t *js);
void cleanup_eventsource_module(ant_t *js);
void gc_mark_eventsource(ant_t *js, gc_mark_fn mark);
