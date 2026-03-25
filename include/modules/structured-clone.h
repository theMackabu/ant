#ifndef STRUCTURED_CLONE_H
#define STRUCTURED_CLONE_H

#include "types.h"

void init_structured_clone_module(void);
ant_value_t js_structured_clone(ant_t *js, ant_value_t *args, int nargs);

#endif
