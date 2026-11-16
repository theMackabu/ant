#pragma once

#ifdef ANT_HAVE_UNSAFE_C
#include "types.h"

void init_cjit_module(ant_t *js, ant_value_t unsafe_obj);

#endif