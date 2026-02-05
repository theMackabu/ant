#ifndef EVENTS_H
#define EVENTS_H

#include "types.h"

void init_events_module(void);
jsval_t events_library(struct js *js);
void events_gc_update_roots(void (*op_val)(void *, jsval_t *), void *ctx);

#endif
