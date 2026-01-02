#ifndef FETCH_H
#define FETCH_H

#include "ant.h"

void init_fetch_module(void);
void fetch_poll_events(void);
void fetch_gc_update_roots(GC_FWD_ARGS);

int has_pending_fetches(void);

#endif
