#ifndef REACTOR_H
#define REACTOR_H

#include "types.h"

typedef enum {
  WORK_MICROTASKS       = 1 << 0,
  WORK_TIMERS           = 1 << 1,
  WORK_IMMEDIATES       = 1 << 2,
  WORK_COROUTINES       = 1 << 3,
  WORK_COROUTINES_READY = 1 << 4,
  WORK_FETCHES          = 1 << 5,
  WORK_FS_OPS           = 1 << 6,
  WORK_CHILD_PROCS      = 1 << 7,
  WORK_READLINE         = 1 << 8,
  WORK_STDIN            = 1 << 9,
} work_flags_t;

#define WORK_TASKS    (WORK_MICROTASKS | WORK_TIMERS | WORK_IMMEDIATES | WORK_COROUTINES | WORK_FETCHES)
#define WORK_PENDING  (WORK_TASKS | WORK_FS_OPS | WORK_CHILD_PROCS | WORK_READLINE | WORK_STDIN)
#define WORK_BLOCKING (WORK_MICROTASKS | WORK_IMMEDIATES | WORK_COROUTINES_READY)
#define WORK_ASYNC    (WORK_READLINE | WORK_STDIN | WORK_TIMERS | WORK_FETCHES | WORK_FS_OPS | WORK_CHILD_PROCS)

#define UV_CHECK_ALIVE uv_loop_alive(uv_default_loop())

typedef void (*reactor_poll_hook_t)(void *data);
void js_reactor_set_poll_hook(reactor_poll_hook_t hook, void *data);

void js_poll_events(ant_t *js);
void js_run_event_loop(ant_t *js);

#endif