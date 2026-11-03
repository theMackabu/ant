#ifndef ANT_MODULES_MICROTASKS_H
#define ANT_MODULES_MICROTASKS_H

#include "types.h"
#include "gc/modules.h"

int has_pending_microtasks(ant_t *js);

void process_microtasks(ant_t *js);
void ant_realm_run_microtasks(ant_t *js, ant_realm_t *realm);
void ant_microtasks_discard(ant_t *js);
void gc_mark_microtasks(ant_t *js, gc_mark_fn mark);

ant_value_t js_queue_microtask(ant_params_t);
void queue_microtask(ant_t *js, ant_value_t callback);
void queue_microtask_with_args(ant_t *js, ant_value_t callback, ant_args_t);

void queue_next_tick(ant_t *js, ant_value_t callback);
void queue_next_tick_with_args(ant_t *js, ant_value_t callback, ant_args_t);

void queue_promise_trigger(ant_t *js, ant_value_t promise);
bool queue_promise_thenable_job(ant_t *js, ant_value_t promise, ant_value_t thenable, ant_value_t then_fn);
bool queue_promise_resolve_job(ant_t *js, ant_value_t promise, ant_value_t value);
bool queue_generator_yield_job(ant_t *js, ant_value_t gen, ant_value_t value);
bool queue_await_resume_job(coroutine_t *coro, ant_value_t value);
bool queue_await_settle_job(coroutine_t *coro, ant_value_t value, bool is_error);
bool queue_await_defer_job(coroutine_t *coro, ant_value_t value);

bool js_maybe_drain_microtasks(ant_t *js);
bool js_maybe_drain_microtasks_after_async_settle(ant_t *js);

#endif
