#ifndef REACTOR_H
#define REACTOR_H

#include "types.h"
#include <uv.h>

#define UV_CHECK_ALIVE uv_loop_alive(uv_default_loop())

typedef enum {
  WORK_MICROTASKS  = 1 << 0,
  WORK_TIMERS      = 1 << 1,
  WORK_IMMEDIATES  = 1 << 2,
  WORK_FETCHES     = 1 << 3,
  WORK_FS_OPS      = 1 << 4,
  WORK_CHILD_PROCS = 1 << 5,
  WORK_READLINE    = 1 << 6,
  WORK_STDIN       = 1 << 7,
} work_flags_t;

enum {
  WORK_TASKS    = (WORK_MICROTASKS | WORK_TIMERS | WORK_IMMEDIATES | WORK_FETCHES),
  WORK_PENDING  = (WORK_TASKS | WORK_FS_OPS | WORK_CHILD_PROCS | WORK_READLINE | WORK_STDIN),
  WORK_BLOCKING = (WORK_MICROTASKS | WORK_IMMEDIATES),
  WORK_ASYNC    = (WORK_READLINE | WORK_STDIN | WORK_TIMERS | WORK_FETCHES | WORK_FS_OPS | WORK_CHILD_PROCS),
};

void js_poll_events(ant_t *js);
void js_run_event_loop(ant_t *js);
void js_reactor_pump_repl_nowait(ant_t *js);

#endif
