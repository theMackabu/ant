#ifndef SUGAR_H
#define SUGAR_H

#include "esm/loader.h"
#include "types.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

typedef enum {
  CORO_HOLD_ACTIVE    = 1u << 0,
  CORO_HOLD_GENERATOR = 1u << 2,
  CORO_HOLD_AWAIT     = 1u << 3,
} coroutine_hold_t;

struct coroutine {
  ant_t *js;

  ant_value_t this_val;
  ant_value_t super_val;
  ant_value_t new_target;
  ant_value_t async_func;
  ant_value_t owner_gen;
  ant_value_t *args;

  ant_value_t awaited_promise;
  ant_value_t async_promise;
  ant_module_t *module_eval_ctx;

  struct coroutine *active_parent;
  struct coroutine *active_prev;
  struct sv_activation *act;

  int nargs;
  uint8_t hold_bits;

  bool is_generator: 1;
  bool await_registered: 1;

  uint64_t gc_epoch;
  uint32_t refcount;
  uint32_t remember_index;

  microtask_entry_t *await_resume_job;
};

typedef enum {
  JS_AWAIT_PENDING = 0,
  JS_AWAIT_ERROR,
} js_await_state_t;

typedef struct {
  js_await_state_t state;
  ant_value_t value;
} js_await_result_t;

bool coroutine_cancel(coroutine_t *coro);

void coroutine_retain(coroutine_t *coro);
void coroutine_release(coroutine_t *coro);
void coroutine_clear_await_registration(coroutine_t *coro);

void coroutine_hold(coroutine_t *coro, uint8_t hold);
void coroutine_unhold(coroutine_t *coro, uint8_t hold);

ant_value_t start_async_in_coroutine(
  ant_t *js, const char *code, size_t code_len,
  ant_value_t closure_scope, ant_value_t *args, int nargs
);

ant_value_t resume_coroutine_wrapper(ant_params_t);
ant_value_t reject_coroutine_wrapper(ant_params_t);

// TODO: move to promise.c
js_await_result_t js_promise_await_coroutine(ant_t *js, ant_value_t promise, coroutine_t *coro);

void Ant_Promise_ClearAwaitCoroutine(ant_t *js, ant_value_t promise, coroutine_t *coro);
void Ant_Coroutine_ResumeAwaitJob(ant_t *js, coroutine_t *coro, ant_value_t value);
void Ant_Coroutine_SettleAndResume(ant_t *js, coroutine_t *coro, ant_value_t value, bool is_error);

#endif
