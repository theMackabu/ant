#pragma once

#include "types.h"

__attribute__((cold)) 
ant_value_t isolate_library(ant_t *js);
void cleanup_isolate_module(ant_t *js);
