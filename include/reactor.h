#ifndef REACTOR_H
#define REACTOR_H

#include "types.h"
#include <uv.h>

typedef enum: uint8_t {
  WORK_MICROTASKS  = 1u << 0,
  WORK_TIMERS      = 1u << 1,
  WORK_IMMEDIATES  = 1u << 2,
  WORK_FETCHES     = 1u << 3,
  WORK_FS_OPS      = 1u << 4,
  WORK_CHILD_PROCS = 1u << 5,
  WORK_READLINE    = 1u << 6,
  WORK_STDIN       = 1u << 7,

  WORK_BLOCKING = (WORK_MICROTASKS | WORK_IMMEDIATES),
  WORK_TASKS    = (WORK_MICROTASKS | WORK_TIMERS | WORK_IMMEDIATES | WORK_FETCHES),
  WORK_PENDING  = (WORK_TASKS | WORK_FS_OPS | WORK_CHILD_PROCS | WORK_READLINE | WORK_STDIN),
  WORK_ASYNC    = (WORK_READLINE | WORK_STDIN | WORK_TIMERS | WORK_FETCHES | WORK_FS_OPS | WORK_CHILD_PROCS),
} work_flags_t;

typedef enum: uint8_t {
  JS_REACTOR_AWAIT_FULFILLED,
  JS_REACTOR_AWAIT_REJECTED,
  JS_REACTOR_AWAIT_INTERRUPTED,
  JS_REACTOR_AWAIT_INVALID,
} js_reactor_await_status_t;

void js_poll_events(ant_t *js);
void js_run_event_loop(ant_t *js);
void js_reactor_pump_repl_nowait(ant_t *js);

typedef bool (*js_reactor_interrupt_fn)(void *ctx);
js_reactor_await_status_t js_reactor_await_promise(
  ant_t *js, ant_value_t promise, ant_value_t *value_out,
  js_reactor_interrupt_fn interrupted, void *interrupt_ctx
);

#endif
