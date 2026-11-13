#pragma once

#include "types.h"

__attribute__((cold)) 
void init_navigator_module(ant_t *js);
void cleanup_navigator_module(ant_t *js);
