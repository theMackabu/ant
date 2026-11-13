#pragma once

#include "types.h"

__attribute__((cold)) 
void init_structured_clone_module(ant_t *js);
ant_value_t js_structured_clone(ant_params_t);
