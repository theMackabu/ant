#pragma once

#include "types.h"
#include "gc/modules.h"

__attribute__((cold)) 
void init_domexception_module(ant_t *js);
ant_value_t make_dom_exception(ant_t *js, const char *message, const char *name);
