#ifndef TIMER_H
#define TIMER_H

#include "ant.h"

void init_timer_module(void);
void process_timers(struct js *js);
void process_microtasks(struct js *js);
void process_immediates(struct js *js);
void queue_microtask(struct js *js, jsval_t callback);
void timer_gc_update_roots(GC_FWD_ARGS);

int has_pending_timers(void);
int has_pending_microtasks(void);
int has_pending_immediates(void);
int64_t get_next_timer_timeout(void);

#endif
