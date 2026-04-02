#ifndef TIMER_H
#define TIMER_H

#include "types.h"

ant_value_t timers_library(ant_t *js);
ant_value_t timers_promises_library(ant_t *js);

void init_timer_module(void);
void process_microtasks(ant_t *js);
void process_immediates(ant_t *js);
void queue_promise_trigger(ant_t *js, ant_value_t promise);

void queue_microtask(ant_t *js, ant_value_t callback);
void queue_microtask_with_args(ant_t *js, ant_value_t callback, ant_value_t *args, int nargs);

bool js_maybe_drain_microtasks(ant_t *js);
bool js_maybe_drain_microtasks_after_async_settle(ant_t *js);

int has_pending_timers(void);
int has_pending_microtasks(void);
int has_pending_immediates(void);

#endif
