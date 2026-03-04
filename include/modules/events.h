#ifndef EVENTS_H
#define EVENTS_H

#include "types.h"

void init_events_module(void);
void events_gc_update_roots(void (*op_val)(void *, ant_value_t *), void *ctx);

ant_value_t events_library(ant_t *js);

#endif