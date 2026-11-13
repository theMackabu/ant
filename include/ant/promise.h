#pragma once

#include "ant/base.h"

typedef enum: uint8_t {
  JS_PROMISE_INVALID,
  JS_PROMISE_PENDING,
  JS_PROMISE_FULFILLED,
  JS_PROMISE_REJECTED,
} js_promise_settlement_t;

ant_value_t js_mkpromise(ant_t *js);
ant_value_t js_mkgenerator(ant_t *js);

bool js_mark_promise_trigger_queued(ant_t *js, ant_value_t promise);
void js_process_promise_handlers(ant_t *js, ant_value_t promise);
void js_mark_promise_trigger_dequeued(ant_t *js, ant_value_t promise);

void js_mark_promise_rejection_handled_chain(ant_t *js, ant_value_t promise);
void js_reject_promise(ant_t *js, ant_value_t promise, ant_value_t value);
void js_resolve_promise(ant_t *js, ant_value_t promise, ant_value_t value);
void js_process_promise_thenable_job(ant_t *js, ant_value_t promise, ant_value_t thenable, ant_value_t then_fn);
void Ant_Promise_Observe(ant_t *js, ant_value_t promise, ant_value_t on_fulfilled, ant_value_t on_rejected);

ant_value_t js_promise_assimilate_awaitable(ant_t *js, ant_value_t value);
ant_value_t js_promise_then(ant_t *js, ant_value_t promise, ant_value_t on_fulfilled, ant_value_t on_rejected);
js_promise_settlement_t js_promise_get_settlement(ant_t *js, ant_value_t promise,  ant_value_t *value_out);
