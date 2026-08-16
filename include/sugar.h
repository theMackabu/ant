#ifndef SUGAR_H
#define SUGAR_H

#include "esm/loader.h"
#include "types.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

typedef enum {
  CORO_ASYNC_AWAIT,
  CORO_GENERATOR,
  CORO_ASYNC_GENERATOR
} coroutine_type_t;

typedef enum {
  CORO_HOLD_ACTIVE    = 1u << 0,
  CORO_HOLD_GENERATOR = 1u << 2,
  CORO_HOLD_AWAIT     = 1u << 3,
} coroutine_hold_t;

typedef struct coroutine {
  ant_t *js;

  ant_value_t this_val;
  ant_value_t super_val;
  ant_value_t new_target;
  ant_value_t result;
  ant_value_t async_func;
  ant_value_t owner_gen;
  ant_value_t *args;

  ant_value_t awaited_promise;
  ant_value_t async_promise;
  ant_module_t *module_eval_ctx;

  union {
    struct coroutine *active_parent;
    struct coroutine *retired_next;
  };
  
  struct coroutine *active_prev;
  struct sv_activation *act;
  coroutine_type_t type;

  int nargs;
  uint64_t gc_epoch;
  uint32_t refcount;
  uint8_t hold_bits;

  bool is_error;
  bool await_registered;
} coroutine_t;

typedef enum {
  JS_AWAIT_PENDING = 0,
  JS_AWAIT_ERROR,
} js_await_state_t;

typedef struct {
  js_await_state_t state;
  ant_value_t value;
} js_await_result_t;

void coroutine_retain(coroutine_t *coro);
void coroutine_release(coroutine_t *coro);

void coroutine_hold(coroutine_t *coro, uint8_t hold);
void coroutine_unhold(coroutine_t *coro, uint8_t hold);

void reap_retired_coroutines(ant_t *js);
void free_coroutine(coroutine_t *coro);
void coroutine_clear_await_registration(coroutine_t *coro);

ant_value_t start_async_in_coroutine(
  ant_t *js, const char *code, size_t code_len,
  ant_value_t closure_scope, ant_value_t *args, int nargs
);

ant_value_t resume_coroutine_wrapper(ant_t *js, ant_value_t *args, int nargs);
ant_value_t reject_coroutine_wrapper(ant_t *js, ant_value_t *args, int nargs);

js_async_entry_t *js_eval_async_entry_create(coroutine_t *coro);
js_await_result_t js_promise_await_coroutine(ant_t *js, ant_value_t promise, coroutine_t *coro);

void js_promise_clear_await_coroutine(ant_t *js, ant_value_t promise, coroutine_t *coro);
void settle_and_resume_coroutine(ant_t *js, coroutine_t *coro, ant_value_t value, bool is_error);

#endif
