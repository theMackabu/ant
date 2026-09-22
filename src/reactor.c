#include "gc.h"
#include "gc/roots.h"
#include "reactor.h"
#include "readline.h"

#include <string.h>

#include "modules/fs.h"
#include "modules/timer.h"
#include "modules/fetch.h"
#include "modules/child_process.h"
#include "modules/readline.h"
#include "modules/process.h"

static inline work_flags_t get_pending_work(ant_t *js) {
  work_flags_t flags = 0;
  if (has_pending_microtasks())         flags |= WORK_MICROTASKS;
  if (has_pending_timers())             flags |= WORK_TIMERS;
  if (has_pending_immediates())         flags |= WORK_IMMEDIATES;
  if (has_pending_fetches())            flags |= WORK_FETCHES;
  if (has_pending_fs_ops())             flags |= WORK_FS_OPS;
  if (has_pending_child_processes())    flags |= WORK_CHILD_PROCS;
  if (has_active_readline_interfaces()) flags |= WORK_READLINE;
  if (has_active_stdin(js))             flags |= WORK_STDIN;
  return flags;
}

static inline bool event_loop_alive(ant_t *js) {
  if (get_pending_work(js) & WORK_PENDING) return true;
  return uv_loop_alive(uv_default_loop());
}

void js_poll_events(ant_t *js) {
  gc_maybe(js);

  process_immediates(js);
  process_microtasks(js);
}

/*
 * uv_run()'s poll step keeps a large event buffer on the stack that the
 * kernel only partly fills, leaving the rest holding whatever was there
 * before. The conservative stack scan sees that stale data for as long as
 * uv_run() is active, and one stale pointer to a dead object can keep a chain
 * of later garbage alive. So the region uv_run() uses is zeroed once, and JS
 * work between uv_run() calls runs below it.
 *
 * libuv doesn't export its buffer sizes. The element counts below mirror the
 * vendored backends and must be updated if a libuv bump changes them:
 *
 *   kqueue.c    uv__io_poll()          struct kevent events[1024]
 *   linux.c     uv__io_poll()          struct epoll_event events[1024]
 *                                      struct epoll_event prep[256]
 *               uv__epoll_ctl_flush()  struct epoll_event oldevents[256]
 *   win/core.c  uv__poll()             OVERLAPPED_ENTRY overlappeds[128]
 *
 * The slack covers the rest of uv_run()'s frames.
 */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/event.h>
static constexpr size_t REACTOR_UV_POLL_BUFFER = 1024u * sizeof(struct kevent);
#elif defined(__linux__)
#include <sys/epoll.h>
static constexpr size_t REACTOR_UV_POLL_BUFFER = (1024u + 256u + 256u) * sizeof(struct epoll_event);
#elif defined(_WIN32)
static constexpr size_t REACTOR_UV_POLL_BUFFER = 128u * sizeof(OVERLAPPED_ENTRY);
#else
static constexpr size_t REACTOR_UV_POLL_BUFFER = 64u * 1024u;
#endif

static constexpr size_t REACTOR_UV_STACK_SLACK   = 8u * 1024u;
static constexpr size_t REACTOR_UV_STACK_RESERVE = REACTOR_UV_POLL_BUFFER + REACTOR_UV_STACK_SLACK;

__attribute__((noinline))
static void reactor_scrub_uv_stack(void) {
  char region[REACTOR_UV_STACK_RESERVE];
  memset(region, 0, sizeof(region));
  __asm__ volatile("" :: "r"(region) : "memory");
}

__attribute__((noinline))
static void reactor_run_below_uv_stack(ant_t *js, void (*work)(ant_t *js)) {
  void *reserve = __builtin_alloca(REACTOR_UV_STACK_RESERVE);
  __asm__ volatile("" :: "r"(reserve) : "memory");
  work(js);
}

static void reactor_poll(ant_t *js) {
  process_report_uncaught_exception_if_pending(js);
  js_poll_events(js);
}

static void reactor_before_exit(ant_t *js) {
  process_report_uncaught_exception_if_pending(js);
  js_poll_events(js);
  ant_value_t code = js_mknum(0);
  emit_process_event(js, "beforeExit", &code, 1);
}

void js_run_event_loop(ant_t *js) {
  reactor_scrub_uv_stack();
drain:
  while (event_loop_alive(js)) {
    reactor_run_below_uv_stack(js, reactor_poll);
    work_flags_t work = get_pending_work(js);
  
    if (work & WORK_BLOCKING) 
      uv_run(uv_default_loop(), UV_RUN_NOWAIT);
    else if ((work & WORK_ASYNC) || uv_loop_alive(uv_default_loop()))
      uv_run(uv_default_loop(), UV_RUN_ONCE);
    else break;
  
    reactor_run_below_uv_stack(js, process_report_uncaught_exception_if_pending);
  }
  
  reactor_run_below_uv_stack(js, reactor_before_exit);
  if (event_loop_alive(js)) goto drain;
}

void js_reactor_pump_repl_nowait(ant_t *js) {
  js_poll_events(js);
  uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  js_poll_events(js);
}

static void reactor_blocking_await_fallback_wake_cb(uv_timer_t *timer) {
  (void)timer;
}

static void reactor_blocking_await_signal_cb(
  uv_poll_t *poll, int status, int events
) {
  (void)poll;
  (void)status;
  (void)events;
  ant_readline_drain_interrupt_wake();
}

static void reactor_await_close_cb(uv_handle_t *handle) {
  bool *closed = (bool *)handle->data;
  if (closed) *closed = true;
}

js_reactor_await_status_t js_reactor_blocking_await_promise(
  ant_t *js, ant_value_t promise, ant_value_t *value_out,
  js_reactor_interrupt_fn interrupted, void *interrupt_ctx
) {
  if (value_out) *value_out = js_mkundef();
  ant_value_t settled = js_mkundef();
  
  js_promise_settlement_t promise_state = js_promise_get_settlement(js, promise, &settled);
  if (promise_state == JS_PROMISE_INVALID) return JS_REACTOR_AWAIT_INVALID;
  
  if (promise_state != JS_PROMISE_PENDING) {
    js_mark_promise_rejection_handled_chain(js, promise);
    if (value_out) *value_out = settled;
    return promise_state == JS_PROMISE_FULFILLED
      ? JS_REACTOR_AWAIT_FULFILLED
      : JS_REACTOR_AWAIT_REJECTED;
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, settled);
  js_mark_promise_rejection_handled_chain(js, promise);

  uv_loop_t *loop = uv_default_loop();
  uv_poll_t signal_poll;
  
  int signal_fd = ant_readline_interrupt_fd();
  bool signal_poll_initialized = signal_fd >= 0 &&
    uv_poll_init(loop, &signal_poll, signal_fd) == 0;
    
  bool signal_poll_started = signal_poll_initialized &&
    uv_poll_start(&signal_poll, UV_READABLE, reactor_blocking_await_signal_cb) == 0;
  
  uv_timer_t wake_timer;
  bool wake_timer_initialized = uv_timer_init(loop, &wake_timer) == 0;
  
  bool wake_timer_started = wake_timer_initialized &&
    uv_timer_start(&wake_timer, reactor_blocking_await_fallback_wake_cb, 16, 16) == 0;

  js_reactor_await_status_t status = JS_REACTOR_AWAIT_INVALID;
  reactor_scrub_uv_stack();
  
  for (;;) {
    reactor_run_below_uv_stack(js, js_poll_events);

    promise_state = js_promise_get_settlement(js, promise, &settled);
    if (promise_state == JS_PROMISE_FULFILLED) {
      status = JS_REACTOR_AWAIT_FULFILLED;
      break;
    }
    
    if (promise_state == JS_PROMISE_REJECTED) {
      status = JS_REACTOR_AWAIT_REJECTED;
      break;
    }
    
    if (promise_state == JS_PROMISE_INVALID) break;
    if (interrupted && interrupted(interrupt_ctx)) {
      status = JS_REACTOR_AWAIT_INTERRUPTED;
      break;
    }

    uv_run(loop, UV_RUN_ONCE);
  }

  if (wake_timer_initialized) {
    if (wake_timer_started) uv_timer_stop(&wake_timer);
    bool wake_timer_closed = false;
    wake_timer.data = &wake_timer_closed;
    uv_close((uv_handle_t *)&wake_timer, reactor_await_close_cb);
    while (!wake_timer_closed) uv_run(loop, UV_RUN_ONCE);
  }
  
  if (signal_poll_initialized) {
    if (signal_poll_started) uv_poll_stop(&signal_poll);
    bool signal_poll_closed = false;
    signal_poll.data = &signal_poll_closed;
    uv_close((uv_handle_t *)&signal_poll, reactor_await_close_cb);
    while (!signal_poll_closed) uv_run(loop, UV_RUN_ONCE);
  }

  if (
    value_out &&
    (status == JS_REACTOR_AWAIT_FULFILLED || status == JS_REACTOR_AWAIT_REJECTED)
  ) *value_out = settled;

  GC_ROOT_RESTORE(js, root_mark);
  return status;
}
