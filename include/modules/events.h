#ifndef EVENTS_H
#define EVENTS_H

#include "types.h"

ant_value_t events_library(ant_t *js);

void init_events_module(void);
void js_dispatch_global_event(ant_t *js, ant_value_t event_obj);

#endif