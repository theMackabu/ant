#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "ant.h"

void init_timer_module(struct js *js, jsval_t ant_obj);
void process_timers(struct js *js);
void process_microtasks(struct js *js);
int has_pending_timers(void);
int64_t get_next_timer_timeout(void);

#endif
