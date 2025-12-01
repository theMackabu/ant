#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "ant.h"

void init_timer_module(struct js *js, jsval_t ant_obj);
void process_timers(struct js *js);
void process_microtasks(struct js *js);
void queue_microtask(struct js *js, jsval_t callback);
int has_pending_timers(void);
int has_pending_microtasks(void);
int64_t get_next_timer_timeout(void);

#endif
