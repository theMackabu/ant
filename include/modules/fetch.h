#ifndef FETCH_H
#define FETCH_H

#include "types.h"

void init_fetch_module(void);
int has_pending_fetches(void);
ant_value_t ant_fetch(ant_params_t);

#endif
