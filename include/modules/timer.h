#ifndef TIMER_H
#define TIMER_H

#include "gc.h"
#include "types.h"

void init_timer_module(void);
void process_microtasks(ant_t *js);
void process_immediates(ant_t *js);
void queue_microtask(ant_t *js, ant_value_t callback);
void queue_promise_trigger(uint32_t promise_id);
void timer_gc_update_roots(GC_OP_VAL_ARGS);

int has_pending_timers(void);
int has_pending_microtasks(void);
int has_pending_immediates(void);

#endif
