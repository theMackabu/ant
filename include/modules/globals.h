#pragma once

#include "types.h"

__attribute__((cold)) void init_globals_module(ant_t *js);
void js_fire_rejection_handled(ant_t *js, ant_value_t promise_val, ant_value_t reason);

ant_value_t js_report_error(ant_params_t);
bool js_fire_unhandled_rejection(ant_t *js, ant_value_t promise_val, ant_value_t reason);
