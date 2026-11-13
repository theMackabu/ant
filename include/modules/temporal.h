#pragma once

#include "types.h"

#ifdef ANT_HAVE_TEMPORAL
__attribute__((cold)) 
void init_temporal_module(ant_t *js);
#endif
