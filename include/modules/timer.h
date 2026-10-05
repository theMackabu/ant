#ifndef TIMER_H
#define TIMER_H

#include "types.h"

struct coroutine;
struct timer_entry;
struct microtask_entry;
struct immediate_entry;

typedef struct {
  struct timer_entry *timers;
  struct microtask_entry *next_ticks;
  struct microtask_entry *next_ticks_tail;
  struct microtask_entry *next_ticks_processing;
  struct microtask_entry *microtasks;
  struct microtask_entry *microtasks_tail;
  struct microtask_entry *microtasks_processing;
  struct immediate_entry *immediates;
  struct immediate_entry *immediates_tail;
  int next_timer_id;
  int next_immediate_id;
  int active_timer_count;
  int active_refed_timer_count;
} ant_timer_state_t;

ant_value_t timers_library(ant_t *js);
ant_value_t timers_promises_library(ant_t *js);

void init_timer_module(ant_t *js);
void cleanup_timer_module(ant_t *js);
void process_microtasks(ant_t *js);
void process_immediates(ant_t *js);
void queue_promise_trigger(ant_t *js, ant_value_t promise);

void queue_microtask(ant_t *js, ant_value_t callback);
void queue_microtask_with_args(ant_t *js, ant_value_t callback, ant_value_t *args, int nargs);

void queue_next_tick(ant_t *js, ant_value_t callback);
void queue_next_tick_with_args(ant_t *js, ant_value_t callback, ant_value_t *args, int nargs);

bool js_maybe_drain_microtasks(ant_t *js);
bool js_maybe_drain_microtasks_after_async_settle(ant_t *js);

bool queue_promise_thenable_job(ant_t *js, ant_value_t promise, ant_value_t thenable, ant_value_t then_fn);
bool queue_await_resume_job(struct coroutine *coro, ant_value_t value);

int has_pending_timers(ant_t *js);
int has_pending_microtasks(ant_t *js);
int has_pending_immediates(ant_t *js);

#endif
