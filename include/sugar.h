#ifndef SUGAR_H
#define SUGAR_H

#include "types.h"

#include <stddef.h>
#include <stdlib.h>
#include <utarray.h>
#include <minicoro.h>

#define CORO_MALLOC(size) calloc(1, size)
#define CORO_FREE(ptr) free(ptr)

#define CORO_PER_TICK_LIMIT 100000

typedef enum {
  CORO_ASYNC_AWAIT,
  CORO_GENERATOR,
  CORO_ASYNC_GENERATOR
} coroutine_type_t;

typedef struct coroutine {
  struct js *js;
  coroutine_type_t type;
  jsval_t scope;
  jsval_t this_val;
  jsval_t super_val;
  jsval_t new_target;
  jsval_t awaited_promise;
  jsval_t result;
  jsval_t async_func;
  jsval_t *args;
  int nargs;
  bool is_settled;
  bool is_error;
  bool is_done;
  jsoff_t resume_point;
  jsval_t yield_value;
  struct coroutine *prev;
  struct coroutine *next;
  mco_coro* mco;
  bool mco_started;
  bool is_ready;
  struct for_let_ctx *for_let_stack;
  int for_let_stack_len;
  int for_let_stack_cap;
  UT_array *scope_stack;
  void *token_stream;
  int token_stream_pos;
  const char *token_stream_code;
} coroutine_t;

typedef struct {
  coroutine_t *head;
  coroutine_t *tail;
} coroutine_queue_t;

typedef struct {
  struct js *js;
  const char *code;
  size_t code_len;
  jsval_t closure_scope;
  jsval_t result;
  jsval_t promise;
  bool has_error;
  coroutine_t *coro;
} async_exec_context_t;

typedef struct {
  jsval_t scope;
  UT_array *scope_stack;
} coro_saved_state_t;

extern coroutine_queue_t pending_coroutines;
extern uint32_t coros_this_tick;

void enqueue_coroutine(coroutine_t *coro);
void remove_coroutine(coroutine_t *coro);
void free_coroutine(coroutine_t *coro);

coro_saved_state_t coro_enter(struct js *js, coroutine_t *coro);
void coro_leave(struct js *js, coroutine_t *coro, coro_saved_state_t saved);

jsval_t start_async_in_coroutine(struct js *js, const char *code, size_t code_len, jsval_t closure_scope, jsval_t *args, int nargs);
jsval_t resume_coroutine_wrapper(struct js *js, jsval_t *args, int nargs);
jsval_t reject_coroutine_wrapper(struct js *js, jsval_t *args, int nargs);

bool has_ready_coroutines(void);
bool has_pending_coroutines(void);

#endif
