#ifndef PROMISE_H
#define PROMISE_H

#include "types.h"
#include "object.h"
#include "sugar.h"

void js_promise_init_prototype(ant_t *js, ant_value_t promise_proto);
void js_promise_init_constructor(ant_t *js, ant_value_t ctor_obj);

uint32_t js_promise_id(ant_t *js, ant_value_t promise);
bool js_is_promise_ctor_cfunc(ant_cfunc_t fn);

ant_promise_state_t *js_promise_data(ant_t *js, ant_value_t promise, bool create);
ant_value_t js_promise_reject_with_ctor(ant_t *js, ant_value_t ctor, ant_value_t reason);

void Ant_Promise_ClearAwaitCoroutine(ant_t *js, ant_value_t promise, coroutine_t *coro);
js_await_result_t js_promise_await_coroutine(ant_t *js, ant_value_t promise, coroutine_t *coro, bool deferred);
ant_value_t js_promise_tail_await(ant_t *js, ant_value_t value);

#endif
