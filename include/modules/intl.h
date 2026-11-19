#pragma once

#include "types.h"

__attribute__((cold)) 
void init_intl_module(ant_t *js);
ant_value_t Ant_Intl_ResolveLocale(ant_t *js, ant_value_t locales);
