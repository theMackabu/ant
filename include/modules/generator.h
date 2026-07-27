#ifndef GENERATOR_H
#define GENERATOR_H

#include "sugar.h"
#include "types.h"

void init_generator_module(void);
void generator_mark_for_gc(ant_t *js, ant_value_t gen);

bool generator_resume_pending_request(ant_t *js, coroutine_t *coro, ant_value_t result);
coroutine_t *generator_get_coro_for_gc(ant_value_t gen);

#endif
