#ifndef GENERATOR_H
#define GENERATOR_H

#include "sugar.h"
#include "types.h"

void init_generator_module(void);
coroutine_t *generator_get_coro_for_gc(ant_value_t gen);

#endif
