#include <compat.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "isolate.h"
#include "reactor.h"
#include "modules/atomics.h"
#include "isolate_internal.h"
#include "net/listen_policy.h"

static constexpr size_t GUEST_STACK_BYTES = 8u * 1024u * 1024u;

static void own_push_done(ant_guest_t *g, guest_call_t *call) {
  call->next = NULL;
  if (g->done_tail) g->done_tail->next = call;
  else g->done_head = call;
  g->done_tail = call;
}

static void own_complete(ant_guest_t *g, guest_call_t *call) {
  uv_mutex_lock(&g->lock);
  call->done = true;
  if (call->sync) uv_cond_broadcast(&g->cond);
  else {
    own_push_done(g, call);
    if (g->host_async_open) uv_async_send(&g->host_async);
  }
  uv_mutex_unlock(&g->lock);
}

static void own_unlink_host(ant_guest_t *g, guest_host_call_t *call) {
  guest_host_call_t *prev = NULL;
  for (guest_host_call_t *it = g->host_head; it; prev = it, it = it->next) if (it == call) {
    if (prev) prev->next = call->next;
    else g->host_head = call->next;
    if (g->host_tail == call) g->host_tail = prev;
    return;
  }
}

static guest_host_call_t *own_take_host(ant_guest_t *g) {
  guest_host_call_t *call = g->host_head;
  if (!call) return NULL;
  g->host_head = call->next;
  if (!g->host_head) g->host_tail = NULL;
  call->taken = true;
  return call;
}

static bool own_call_host(ant_guest_t *g, guest_host_call_t *call) {
  uv_mutex_lock(&g->lock);
  if (g->disposed || !g->host_async_open) {
    call->status = GUEST_STATUS_DISPOSED;
    uv_mutex_unlock(&g->lock);
    return true;
  }

  call->next = NULL;
  if (g->host_tail) g->host_tail->next = call;
  else g->host_head = call;
  g->host_tail = call;

  uv_async_send(&g->host_async);
  uv_cond_broadcast(&g->cond);
  while (!call->done && !g->disposed) uv_cond_wait(&g->cond, &g->lock);

  bool owned = true;
  if (!call->done) {
    if (call->taken) {
      call->abandoned = true;
      owned = false;
    } else {
      own_unlink_host(g, call);
      call->status = GUEST_STATUS_DISPOSED;
    }
  }

  uv_mutex_unlock(&g->lock);
  return owned;
}

static guest_call_t *own_take_calls(ant_guest_t *g) {
  uv_mutex_lock(&g->lock);
  guest_call_t *calls = g->calls_head;
  g->calls_head = g->calls_tail = NULL;
  uv_mutex_unlock(&g->lock);
  return calls;
}

static void own_child_wake(uv_async_t *handle) {
  ant_guest_t *g = handle->data;

  uv_mutex_lock(&g->lock);
  bool disposed = g->disposed;
  uv_mutex_unlock(&g->lock);

  if (disposed || !g->child) {
    if (!uv_is_closing((uv_handle_t *)handle)) uv_close((uv_handle_t *)handle, NULL);
    uv_stop(handle->loop);
    return;
  }

  for (guest_call_t *call = own_take_calls(g), *next; call; call = next) {
    next = call->next;
    call->next = NULL;
    guest_child_execute(g, call);
  }
}

static void own_fail_queued(ant_guest_t *g) {
  for (guest_call_t *call = own_take_calls(g), *next; call; call = next) {
    next = call->next;
    call->status = GUEST_STATUS_DISPOSED;
    own_complete(g, call);
  }
}

static void own_thread_main(void *arg) {
  ant_guest_t *g = arg;
  volatile char stack_base;

  ant_t *child = Ant_Create(&(ant_isolate_options_t){ .object_arena_bytes = g->arena_bytes });
  bool ok = child != NULL;

  if (ok) {
    child->interrupts_armed = true;
    Ant_Net_ListenPolicy_Inherit(child, g->host);
    js_setstackbase(child, (void *)&stack_base);
    js_setstacklimit(child, GUEST_STACK_BYTES * 3 / 4);
    ok = guest_child_boot(g, child) && uv_async_init(child->loop, &g->child_async, own_child_wake) == 0;
  }

  uv_mutex_lock(&g->lock);
  g->started = true;
  g->failed = !ok;
  g->child_async_open = ok;
  if (ok) g->child_async.data = g;
  uv_cond_broadcast(&g->cond);
  uv_mutex_unlock(&g->lock);

  if (ok) {
    uv_async_send(&g->child_async);
    js_run_event_loop(child);
  }

  uv_mutex_lock(&g->lock);
  g->child_async_open = false;
  g->child = NULL;
  uv_mutex_unlock(&g->lock);
  own_fail_queued(g);

  if (child) {
    if (ok && !uv_is_closing((uv_handle_t *)&g->child_async)) uv_close((uv_handle_t *)&g->child_async, NULL);
    guest_child_teardown(g);
    child->guest = NULL;
    child->terminating = false;
    atomic_store_explicit(&child->interrupt_pending, 0, memory_order_relaxed);
    Ant_Destroy(child);
    ant_thread_cleanup();
  }
  
  guest_release(g);
}

static void own_host_drain(ant_t *js, ant_guest_t *g) {
  for (;;) {
    uv_mutex_lock(&g->lock);
    guest_call_t *call = g->done_head;
    guest_host_call_t *host = NULL;

    if (call) {
      g->done_head = call->next;
      if (!g->done_head) g->done_tail = NULL;
    } else host = own_take_host(g);

    uv_mutex_unlock(&g->lock);
    if (!call && !host) return;

    if (call) guest_finish_host(js, g, call);
    else guest_host_run_callback(js, g, host);
  }
}

static void own_host_wake(uv_async_t *handle) {
  ant_guest_t *g = handle->data;
  own_host_drain(g->host, g);
  process_microtasks(g->host);
}

ant_value_t guest_own_host_call(ant_t *js, ant_guest_t *g, guest_host_call_t *call) {
  uv_mutex_lock(&g->lock);
  bool abandoned = call->abandoned;
  call->done = true;
  uv_cond_broadcast(&g->cond);
  uv_mutex_unlock(&g->lock);
  if (abandoned) guest_host_call_free(call);
  return Ant_Value_Undefined;
}

ant_value_t guest_own_start(ant_t *js, ant_guest_t *g) {
  if (uv_async_init(js->loop, &g->host_async, own_host_wake) != 0) return js_mkerr(js, "Failed to create an isolate");
  g->host_async.data = g;
  g->host_async_open = true;
  uv_unref((uv_handle_t *)&g->host_async);

  g->complete = own_complete;
  g->call_host = own_call_host;

  uv_thread_options_t opts = { .flags = UV_THREAD_HAS_STACK_SIZE, .stack_size = GUEST_STACK_BYTES };
  guest_retain(g);
  if (uv_thread_create_ex(&g->thread, &opts, own_thread_main, g) != 0) {
    guest_release(g);
    return js_mkerr(js, "Failed to start the isolate thread");
  }
  g->thread_started = true;

  uv_mutex_lock(&g->lock);
  while (!g->started) uv_cond_wait(&g->cond, &g->lock);
  bool failed = g->failed;
  uv_mutex_unlock(&g->lock);

  return failed ? js_mkerr_code(js, JS_ERR_GENERIC, "ERR_ISOLATE_INIT_FAILED", "Isolate initialization failed") : Ant_Value_Undefined;
}

static bool own_enqueue(ant_guest_t *g, guest_call_t *call) {
  uv_mutex_lock(&g->lock);
  bool open = !g->disposed && g->child_async_open;
  if (open) {
    call->next = NULL;
    if (g->calls_tail) g->calls_tail->next = call;
    else g->calls_head = call;
    g->calls_tail = call;
    uv_async_send(&g->child_async);
  }
  uv_mutex_unlock(&g->lock);
  return open;
}

static ant_value_t own_wait(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  uv_mutex_lock(&g->lock);
  for (;;) {
    if (call->done) break;
    guest_host_call_t *host = own_take_host(g);
    if (host) {
      uv_mutex_unlock(&g->lock);
      guest_host_run_callback(js, g, host);
      uv_mutex_lock(&g->lock);
      continue;
    }
    uv_cond_wait(&g->cond, &g->lock);
  }
  uv_mutex_unlock(&g->lock);
  return guest_finish_host(js, g, call);
}

ant_value_t guest_own_submit(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  if (!own_enqueue(g, call)) {
    call->status = GUEST_STATUS_DISPOSED;
    call->done = true;
    return guest_finish_host(js, g, call);
  }

  return call->sync ? own_wait(js, g, call) : call->promise;
}

void guest_own_set_busy(ant_guest_t *g, bool busy) {
  if (!g->host_async_open) return;
  if (busy) uv_ref((uv_handle_t *)&g->host_async);
  else uv_unref((uv_handle_t *)&g->host_async);
}

static void own_host_async_closed(uv_handle_t *handle) {
  guest_release(handle->data);
}

void guest_own_dispose(ant_guest_t *g) {
  uv_mutex_lock(&g->lock);
  bool already = g->disposed;
  g->disposed = true;
  if (g->child) {
    atomic_store_explicit(&g->child->interrupt_pending, 1, memory_order_relaxed);
    atomics_wake_isolate(g->child);
  }
  if (g->child_async_open) uv_async_send(&g->child_async);
  uv_cond_broadcast(&g->cond);
  uv_mutex_unlock(&g->lock);
  if (already) return;

  if (g->thread_started) {
    uv_thread_join(&g->thread);
    g->thread_started = false;
  }

  own_host_drain(g->host, g);
  if (g->host_async_open) {
    g->host_async_open = false;
    guest_retain(g);
    uv_close((uv_handle_t *)&g->host_async, own_host_async_closed);
  }
}
