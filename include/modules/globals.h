#ifndef ANT_GLOBALS_MODULE_H
#define ANT_GLOBALS_MODULE_H

#include "types.h"

void init_globals_module(void);

ant_value_t js_structured_clone(ant_t *js, ant_value_t *args, int nargs);
ant_value_t js_report_error(ant_t *js, ant_value_t *args, int nargs);

bool js_fire_unhandled_rejection(ant_t *js, ant_value_t promise_val, ant_value_t reason);
void js_fire_rejection_handled(ant_t *js, ant_value_t promise_val, ant_value_t reason);

#endif
