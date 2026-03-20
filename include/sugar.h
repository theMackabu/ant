#ifndef SUGAR_H
#define SUGAR_H

#include "types.h"

#include <stddef.h>
#include <stdlib.h>
#include <minicoro.h>

#define CORO_MALLOC(size) calloc(1, size)
#define CORO_FREE(ptr) free(ptr)

#define MCO_RESUME_SAVE(js_, mco_, res_) do {    \
  void *_saved_cstk = (js_)->cstk.base;          \
  size_t _saved_limit = (js_)->cstk.limit;       \
  volatile char _stk_mark;                       \
  if (!mco_running())                            \
    (js_)->cstk.main_lo = (void *)&_stk_mark;    \
  (res_) = mco_resume((mco_));                   \
  (js_)->cstk.base = _saved_cstk;                \
  (js_)->cstk.limit = _saved_limit;              \
} while (0)

#define MCO_CORO_STACK_ENTER(js_, mco_) do {     \
  volatile char _coro_marker;                    \
  (js_)->cstk.base = (void *)&_coro_marker;      \
  (js_)->cstk.limit = (mco_)->stack_size;        \
} while (0)

#define CORO_PER_TICK_LIMIT 100000

typedef enum {
  CORO_ASYNC_AWAIT,
  CORO_GENERATOR,
  CORO_ASYNC_GENERATOR
} coroutine_type_t;

typedef struct coroutine {
  ant_t *js;
  
  ant_value_t this_val;
  ant_value_t super_val;
  ant_value_t new_target;
  ant_value_t result;
  ant_value_t async_func;
  ant_value_t yield_value;
  ant_value_t *args;
  
  ant_value_t awaited_promise;
  ant_value_t async_promise;
  
  struct coroutine *prev;
  struct coroutine *next;
  
  mco_coro *mco;
  struct sv_vm *sv_vm;
  
  ant_offset_t resume_point;
  coroutine_type_t type;
  
  int nargs;
  bool is_settled;
  bool is_error;
  bool is_done;
  bool mco_started;
  bool is_ready;
} coroutine_t;

typedef struct {
  coroutine_t *head;
  coroutine_t *tail;
} coroutine_queue_t;

extern coroutine_queue_t pending_coroutines;
extern uint32_t coros_this_tick;

void enqueue_coroutine(coroutine_t *coro);
void remove_coroutine(coroutine_t *coro);
void free_coroutine(coroutine_t *coro);

ant_value_t start_async_in_coroutine(ant_t *js, const char *code, size_t code_len, ant_value_t closure_scope, ant_value_t *args, int nargs);
ant_value_t resume_coroutine_wrapper(ant_t *js, ant_value_t *args, int nargs);
ant_value_t reject_coroutine_wrapper(ant_t *js, ant_value_t *args, int nargs);

bool has_ready_coroutines(void);
bool has_pending_coroutines(void);

#endif
