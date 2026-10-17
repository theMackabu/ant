#ifndef ANT_WASM_MODULE_H
#define ANT_WASM_MODULE_H

#include "types.h"

void init_wasm_module(ant_t *js);
void wasm_isolate_destroy(ant_t *js);

#endif
