// meson test -C build isolate-timers
//
// Timers, microtasks, next ticks and immediates belong to their isolate
// (js->timers). With one process-wide timer state, the last isolate to start
// owned every timer callback, and draining one isolate's jobs ran the other's.
// The default libuv loop is still shared, so destroying an isolate must close
// its timers before the loop can fire them into freed memory.
#include "internal.h"
#include "modules/timer.h"
#include "silver/call.h"
#include <assert.h>
#include <stdio.h>
#include <uv.h>

static ant_t *a;
static ant_t *b;
static int a_calls;
static int b_calls;

static ant_value_t on_a(ant_params_t) {
  assert(js == a);
  a_calls++;
  return js_mkundef();
}

static ant_value_t on_b(ant_params_t) {
  assert(js == b);
  b_calls++;
  return js_mkundef();
}

static void schedule(ant_t *js, const char *name, ant_value_t callback, int delay) {
  ant_value_t args[] = { callback, js_mknum(delay) };
  ant_value_t fn = js_get(js, js_glob(js), name);
  ant_value_t result = sv_vm_call(js->vm, js, fn, js_mkundef(), args, 2, NULL, js_mkundef());
  assert(!is_err(result));
}

static void count_active_timer(uv_handle_t *handle, void *arg) {
  if (handle->type == UV_TIMER && uv_is_active(handle)) (*(int *)arg)++;
}

static int active_timers(void) {
  int count = 0;
  uv_walk(uv_default_loop(), count_active_timer, &count);
  return count;
}

static void run_loop_while(bool (*pending)(void)) {
  while (pending()) uv_run(uv_default_loop(), UV_RUN_ONCE);
}

static bool either_has_timers(void) { return has_pending_timers(a) || has_pending_timers(b); }
static bool b_has_timers(void) { return has_pending_timers(b); }

int main(void) {
  char stack_base;
  a = ant_create();
  b = ant_create();
  assert(a && b);
  js_setstackbase(a, &stack_base);
  js_setstackbase(b, &stack_base);
  init_timer_module(a);
  init_timer_module(b);

  // jobs drain only in their own isolate
  queue_microtask(a, js_mkfun(on_a));
  queue_next_tick(b, js_mkfun(on_b));
  assert(has_pending_microtasks(a) && has_pending_microtasks(b));
  process_microtasks(b);
  assert(b_calls == 1 && a_calls == 0 && has_pending_microtasks(a));
  process_microtasks(a);
  assert(a_calls == 1 && !has_pending_microtasks(a));

  // timers fire in the isolate that set them
  schedule(a, "setTimeout", js_mkfun(on_a), 1);
  schedule(b, "setTimeout", js_mkfun(on_b), 1);
  assert(has_pending_timers(a) && has_pending_timers(b));
  run_loop_while(either_has_timers);
  assert(a_calls == 2 && b_calls == 2);

  // destroying A closes its interval and drops its jobs; B's timer still
  // fires, and no timer of A's is left on the shared loop
  schedule(a, "setInterval", js_mkfun(on_a), 1);
  schedule(b, "setTimeout", js_mkfun(on_b), 5);
  queue_microtask(a, js_mkfun(on_a));
  assert(active_timers() == 2);
  js_destroy(a);
  run_loop_while(b_has_timers);
  uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  assert(a_calls == 2 && b_calls == 3);
  assert(active_timers() == 0);

  js_destroy(b);
  uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  puts("PASS timers and jobs belong to their isolate and close with it");
}
