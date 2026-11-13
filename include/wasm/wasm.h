#pragma once

#include "types.h"

__attribute__((cold)) 
void init_wasm_module(ant_t *js);
void wasm_isolate_destroy(ant_t *js);
