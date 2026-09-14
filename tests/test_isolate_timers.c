// meson test -C build isolate-timers
#include "internal.h"
#include "gc/roots.h"
#include "gc/modules.h"
#include "modules/timer.h"
#include "silver/call.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

static ant_t *first;
static ant_t *second;
static int first_calls;
static int second_calls;
static ant_value_t first_arg;
static ant_value_t second_arg;
static int visited_arg;

static ant_value_t record_first(ant_params_t) {
  assert(js == first);
  if (nargs) assert(args[0] == first_arg);
  first_calls++;
  return js_mkundef();
}

static ant_value_t record_second(ant_params_t) {
  assert(js == second);
  if (nargs) assert(args[0] == second_arg);
  second_calls++;
  return js_mkundef();
}

static void visit_first(ant_t *js, ant_value_t value) {
  assert(js == first);
  assert(value != second_arg);
  if (value == first_arg) visited_arg++;
}

static ant_value_t drain_second(ant_params_t) {
  assert(js == first);
  process_microtasks(second);
  visited_arg = 0;
  gc_mark_timers(first, visit_first);
  assert(visited_arg > 0); // The unprocessed first-isolate batch stays rooted.
  return js_mkundef();
}

static ant_value_t call(ant_t *js, const char *name, ant_value_t *args, int argc) {
  ant_value_t fn = js_get(js, js_glob(js), name);
  ant_value_t result = sv_vm_call(js->vm, js, fn, js_mkundef(), args, argc, NULL, js_mkundef());
  assert(!is_err(result) && !js->thrown_exists);
  return result;
}

static ant_value_t method(ant_t *js, ant_value_t obj, const char *name) {
  ant_value_t fn = js_get(js, obj, name);
  ant_value_t result = sv_vm_call(js->vm, js, fn, obj, NULL, 0, NULL, js_mkundef());
  assert(!is_err(result) && !js->thrown_exists);
  return result;
}

static ant_value_t schedule(ant_t *js, bool interval, ant_value_t callback, ant_value_t value, int delay) {
  ant_value_t args[] = { callback, js_mknum(delay), value };
  return call(js, interval ? "setInterval" : "setTimeout", args, 3);
}

static void eval_deferred(ant_t *js, const char *source) {
  js->microtasks_draining = true;
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  js->microtasks_draining = false;
  if (is_err(result)) fprintf(stderr, "%s\n", js_str(js, result));
  assert(!is_err(result) && !js->thrown_exists);
}

static void check_jobs(void) {
  assert(!has_pending_microtasks(first));
  assert(!has_pending_microtasks(second));
  queue_microtask_with_args(first, js_mkfun(record_first), &first_arg, 1);
  queue_next_tick_with_args(first, js_mkfun(record_first), &first_arg, 1);
  queue_microtask_with_args(second, js_mkfun(record_second), &second_arg, 1);
  queue_next_tick_with_args(second, js_mkfun(record_second), &second_arg, 1);
  visited_arg = 0;
  gc_mark_timers(first, visit_first);
  assert(visited_arg == 2);
  process_microtasks(second);
  assert(second_calls == 2 && first_calls == 0);
  assert(has_pending_microtasks(first) && !has_pending_microtasks(second));
  process_microtasks(first);
  assert(first_calls == 2);

  queue_microtask(first, js_mkfun(drain_second));
  queue_microtask_with_args(first, js_mkfun(record_first), &first_arg, 1);
  queue_microtask(second, js_mkfun(record_second));
  process_microtasks(first);
  assert(first_calls == 3 && second_calls == 3);

  const char *async_source =
    "globalThis.jobResult = 0;"
    "Promise.resolve({then(resolve) { resolve(5); }}).then(x => { jobResult += x; });"
    "(async () => { await 1; jobResult += 7; })();";
  eval_deferred(first, async_source);
  eval_deferred(second, async_source);
  assert(has_pending_microtasks(first) && has_pending_microtasks(second));
  process_microtasks(second);
  assert(js_getnum(js_get(second, js_glob(second), "jobResult")) == 12);
  assert(js_getnum(js_get(first, js_glob(first), "jobResult")) == 0);
  process_microtasks(first);
  assert(js_getnum(js_get(first, js_glob(first), "jobResult")) == 12);
}

static void check_timers(void) {
  ant_value_t a = schedule(first, false, js_mkfun(record_first), first_arg, 0);
  ant_value_t b = schedule(second, false, js_mkfun(record_second), second_arg, 0);
  // Numeric timer IDs are local to the scheduling isolate.
  ant_value_t id = js_get_slot(a, SLOT_DATA);
  assert(id == js_get_slot(b, SLOT_DATA));
  assert(has_pending_timers(first) && has_pending_timers(second));
  call(first, "clearTimeout", &id, 1);
  assert(!has_pending_timers(first) && has_pending_timers(second));
  int a_before = first_calls;
  int b_before = second_calls;
  uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  assert(first_calls == a_before && second_calls == b_before + 1);
  assert(!has_pending_timers(second));

  a = schedule(first, true, js_mkfun(record_first), first_arg, 10000);
  b = schedule(second, true, js_mkfun(record_second), second_arg, 10000);
  assert(method(first, a, "unref") == a);
  assert(!has_pending_timers(first) && has_pending_timers(second));
  assert(method(first, a, "hasRef") == js_false);
  assert(method(first, a, "ref") == a);
  assert(has_pending_timers(first) && has_pending_timers(second));
  assert(method(first, a, "refresh") == a);
  // Visiting one timer registry never sees the other isolate's argument.
  visited_arg = 0;
  gc_mark_timers(first, visit_first);
  assert(visited_arg > 0);
  call(first, "clearInterval", &a, 1);
  call(second, "clearInterval", &b, 1);

  ant_value_t callback = js_mkfun(record_first);
  a = call(first, "setImmediate", &callback, 1);
  callback = js_mkfun(record_second);
  b = call(second, "setImmediate", &callback, 1);
  assert(has_pending_immediates(first) && has_pending_immediates(second));
  call(first, "clearImmediate", &a, 1);
  assert(!has_pending_immediates(first) && has_pending_immediates(second));
  process_immediates(first);
  assert(second_calls == b_before + 1);
  process_immediates(second);
  assert(second_calls == b_before + 2);
  assert(!has_pending_immediates(first) && !has_pending_immediates(second));
}

static void check_teardown(bool reverse) {
  ant_t *departing = reverse ? second : first;
  ant_t *survivor = reverse ? first : second;
  ant_value_t departing_callback = reverse ? js_mkfun(record_second) : js_mkfun(record_first);
  ant_value_t surviving_callback = reverse ? js_mkfun(record_first) : js_mkfun(record_second);
  ant_value_t departing_arg = reverse ? second_arg : first_arg;
  ant_value_t surviving_arg = reverse ? first_arg : second_arg;
  schedule(departing, true, departing_callback, departing_arg, 10000);
  schedule(survivor, false, surviving_callback, surviving_arg, 0);
  call(departing, "setImmediate", &departing_callback, 1);
  call(survivor, "setImmediate", &surviving_callback, 1);
  queue_microtask(departing, departing_callback);
  queue_next_tick(departing, departing_callback);
  queue_microtask(survivor, surviving_callback);
  queue_next_tick(survivor, surviving_callback);
  eval_deferred(departing, "(async () => { await 1; throw 'must not resume'; })();");
  int before = reverse ? first_calls : second_calls;
  js_destroy(departing);
  assert((reverse ? first_calls : second_calls) == before);
  assert(has_pending_timers(survivor));
  assert(has_pending_immediates(survivor));
  assert(has_pending_microtasks(survivor));
  process_microtasks(survivor);
  process_immediates(survivor);
  uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  assert((reverse ? first_calls : second_calls) == before + 4);

  // New work and GC still work after the sibling and its queued jobs are gone.
  gc_run(survivor);
  queue_microtask(survivor, surviving_callback);
  process_microtasks(survivor);
  assert((reverse ? first_calls : second_calls) == before + 5);
  js_destroy(survivor);
  uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  assert(!uv_loop_alive(uv_default_loop()));
}

static void check_discarded_await(void) {
  ant_t *js = ant_create();
  assert(js);
  coroutine_t *coro = calloc(1, sizeof(*coro));
  assert(coro);
  coro->js = js;
  coro->refcount = 1;
  coro->await_registered = true;
  coro->awaited_promise = js_mkundef();
  coroutine_hold(coro, CORO_HOLD_AWAIT);
  assert(queue_await_resume_job(coro, js_mknum(42)));
  assert(coro->refcount == 3);
  cleanup_timer_module(js);
  assert(coro->refcount == 1 && !coro->await_registered && !coro->hold_bits);
  assert(!has_pending_microtasks(js));
  assert(!queue_await_resume_job(coro, js_mknum(42)));
  queue_microtask(js, js_mkfun(record_first));
  queue_next_tick(js, js_mkfun(record_first));
  assert(!has_pending_microtasks(js));
  coroutine_release(coro);
  js_destroy(js);
}

int main(void) {
  char stack;
  for (int reverse = 0; reverse < 2; reverse++) {
    first = ant_create();
    second = ant_create();
    assert(first && second);
    js_setstackbase(first, &stack);
    js_setstackbase(second, &stack);
    // Bootstrap just timers: unrelated global module roots remain separate debt.
    init_timer_module(first);
    init_timer_module(second);
    first_arg = js_mkobj(first);
    second_arg = js_mkobj(second);
    js_set(first, js_glob(first), "argument", first_arg);
    js_set(second, js_glob(second), "argument", second_arg);
    first_calls = second_calls = 0;
    check_jobs();
    check_timers();
    check_teardown(reverse != 0);
  }
  check_discarded_await();
  puts("PASS isolate timer/job dispatch, roots, cancellation and both teardown orders");
}
