#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>

#include "ant.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "reactor.h"
#include "isolate_internal.h"
#include "net/listen_policy.h"

typedef struct {
  ant_realm_t *cfunc_realm;
} current_saved_t;

typedef struct {
  ant_guest_t *g;
  uv_poll_t poll;
  uv_timer_t timer;
  bool poll_open;
  bool polling;
  bool timer_open;
  uint32_t depth;
  bool destroy_pending;
} current_state_t;

static current_state_t *current_state(ant_guest_t *g) {
  return g->transport;
}

static inline __attribute__((always_inline)) void current_enter(ant_guest_t *g, current_saved_t *saved) {
  current_state_t *st = current_state(g);
  ant_t *host = g->host, *child = g->child;
  
  saved->cfunc_realm = ant_cfunc_intern_realm;
  if (st->depth++ == 0) {
    void *frame = __builtin_frame_address(0);
    uintptr_t sp = (uintptr_t)frame;
    uintptr_t base = (uintptr_t)host->cstk.base;
    size_t used = base > sp ? base - sp : 0;
    size_t room = host->cstk.limit > used ? host->cstk.limit - used : 0;
    js_setstackbase(child, frame);
    js_setstacklimit(child, room);
  }
  ant_cfunc_intern_realm = child->builtin_realm;
}

static void current_destroy(ant_guest_t *g);

static void current_exit(ant_guest_t *g, current_saved_t *saved) {
  current_state_t *st = current_state(g);
  ant_cfunc_intern_realm = saved->cfunc_realm;
  if (--st->depth == 0 && st->destroy_pending) current_destroy(g);
}

static void current_complete(ant_guest_t *g, guest_call_t *call) {
  call->done = true;
  if (!call->sync && g->host) guest_finish_host(g->host, g, call);
}

static bool current_call_host(ant_guest_t *g, guest_host_call_t *call) {
  if (g->disposed) {
    call->status = GUEST_STATUS_DISPOSED;
    return true;
  }
  ant_realm_t *inner = ant_cfunc_intern_realm;
  ant_cfunc_intern_realm = g->host->builtin_realm;
  guest_host_run_callback(g->host, g, call);
  ant_cfunc_intern_realm = inner;
  return true;
}

ant_value_t guest_current_host_call(ant_t *js, ant_guest_t *g, guest_host_call_t *call) {
  call->done = true;
  return Ant_Value_Undefined;
}

static void current_rearm(ant_guest_t *g);

static void current_drive(ant_guest_t *g) {
  if (g->disposed || !g->child) return;
  current_saved_t saved;
  current_enter(g, &saved);
  ant_uv_run(g->child->loop, UV_RUN_NOWAIT);
  process_microtasks(g->child);
  current_exit(g, &saved);
  current_rearm(g);
}

static void current_poll_cb(uv_poll_t *handle, int status, int events) {
  current_drive(((current_state_t *)handle->data)->g);
}

static void current_timer_cb(uv_timer_t *handle) {
  current_drive(((current_state_t *)handle->data)->g);
}

static void current_rearm(ant_guest_t *g) {
  current_state_t *st = current_state(g);
  if (!st || g->disposed || !g->child) return;

  bool alive = uv_loop_alive(g->child->loop);
  if (st->poll_open) {
    if (alive && !st->polling) st->polling = uv_poll_start(&st->poll, UV_READABLE, current_poll_cb) == 0;
    else if (!alive && st->polling) {
      uv_poll_stop(&st->poll);
      st->polling = false;
    }
  }

  if (!st->timer_open) return;
  int timeout = alive ? uv_backend_timeout(g->child->loop) : -1;
  if (timeout >= 0) uv_timer_start(&st->timer, current_timer_cb, (uint64_t)timeout, 0);
  else uv_timer_stop(&st->timer);
}

static void current_handle_closed(uv_handle_t *handle) {
  current_state_t *st = handle->data;
  if (handle == (uv_handle_t *)&st->poll) st->poll_open = false;
  else st->timer_open = false;
  if (st->poll_open || st->timer_open) return;
  guest_release(st->g);
  free(st);
}

static void current_destroy(ant_guest_t *g) {
  current_state_t *st = current_state(g);
  ant_t *child = g->child;
  if (!child) return;

  current_saved_t saved;
  current_enter(g, &saved);
  guest_child_teardown(g);
  st->depth--;
  ant_cfunc_intern_realm = saved.cfunc_realm;

  child->terminating = false;
  atomic_store_explicit(&child->interrupt_pending, 0, memory_order_relaxed);
  g->child = NULL;
  Ant_Destroy(child);

  g->transport = NULL;
  bool closing = false;
  if (st->poll_open) {
    uv_close((uv_handle_t *)&st->poll, current_handle_closed);
    closing = true;
  }
  if (st->timer_open) {
    uv_close((uv_handle_t *)&st->timer, current_handle_closed);
    closing = true;
  }
  if (!closing) {
    guest_release(g);
    free(st);
  }
}

ant_value_t guest_current_start(ant_t *js, ant_guest_t *g) {
  current_state_t *st = calloc(1, sizeof(*st));
  if (!st) return Ant_Error_CreateOutOfMemory(js);
  st->g = guest_retain(g);
  g->transport = st;
  g->complete = current_complete;
  g->call_host = current_call_host;

  ant_t *child = Ant_Create(&(ant_isolate_options_t){ .object_arena_bytes = g->arena_bytes });
  if (!child) {
    g->transport = NULL;
    guest_release(g);
    free(st);
    return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_ISOLATE_INIT_FAILED", "Isolate initialization failed");
  }

  g->child = child;
  child->interrupts_armed = true;
  Ant_Net_ListenPolicy_Inherit(child, js);
  current_saved_t saved;
  current_enter(g, &saved);
  bool ok = guest_child_boot(g, child);
  current_exit(g, &saved);

  st->timer_open = uv_timer_init(js->loop, &st->timer) == 0;
  if (st->timer_open) st->timer.data = st;
  int fd = uv_backend_fd(child->loop);
  st->poll_open = fd >= 0 && uv_poll_init(js->loop, &st->poll, fd) == 0;
  if (st->poll_open) st->poll.data = st;

  if (!ok) {
    g->disposed = true;
    current_destroy(g);
    return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_ISOLATE_INIT_FAILED", "Isolate initialization failed");
  }

  current_rearm(g);
  return Ant_Value_Undefined;
}

ant_value_t guest_current_submit(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  if (g->disposed || !g->child) {
    call->status = GUEST_STATUS_DISPOSED;
    call->done = true;
    return guest_finish_host(js, g, call);
  }

  bool sync = call->sync;
  ant_value_t promise = call->promise;
  current_saved_t saved;
  current_enter(g, &saved);
  guest_child_execute(g, call);
  while (sync && !call->done && !g->disposed && g->child && uv_loop_alive(g->child->loop)) {
    ant_uv_run(g->child->loop, UV_RUN_ONCE);
    process_microtasks(g->child);
  }
  current_exit(g, &saved);
  current_rearm(g);

  if (!sync) return promise;
  if (!call->done) call->status = g->disposed ? GUEST_STATUS_DISPOSED : GUEST_STATUS_THROW;
  return guest_finish_host(js, g, call);
}

void guest_current_dispose(ant_guest_t *g) {
  current_state_t *st = current_state(g);
  g->disposed = true;
  if (!st) return;

  if (st->depth > 0) {
    st->destroy_pending = true;
    if (g->child) {
      g->child->terminating = true;
      atomic_store_explicit(&g->child->interrupt_pending, 1, memory_order_relaxed);
    }
    return;
  }

  current_destroy(g);
}
