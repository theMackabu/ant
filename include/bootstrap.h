#pragma once

#include "types.h"

__attribute__((cold)) 
void ant_bootstrap_modules(ant_t *js);
void ant_bootstrap_realm_intrinsics(ant_t *js);
bool ant_bootstrap_bare(ant_t *js);
