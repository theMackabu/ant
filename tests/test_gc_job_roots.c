#include "internal.h"
#include "cfunc.h"
#include "gc.h"
#include "gc/objects.h"
#include "modules/microtasks.h"
#include "silver/engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ant_value_t held[21];
static ant_value_t noop;
static size_t visited;
static int ran;

static void count_objects(ant_t *js, ant_value_t value) {
  (void)js;
  if (is_object_type(value)) visited++;
}

static size_t minor_job_visits(ant_t *js) {
  visited = 0;
  js->gc.minor = true;
  gc_mark_microtasks(js, count_objects);
  js->gc.minor = false;
  gc_run(js);
  return visited;
}

static ant_value_t payload(ant_t *js, int id) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set(js, obj, "id", Ant_Value_FromNumber(id));
  return obj;
}

static void churn(ant_t *js, int count) {
  for (int i = 0; i < count; i++) payload(js, -1);
}

static void check(ant_t *js, ant_value_t obj, int id) {
  assert(js_obj_ptr(obj)->mark_epoch != ANT_GC_DEAD);
  assert(Ant_Value_AsNumberUnchecked(js_get(js, obj, "id")) == id);
}

static void hold_job(ant_t *js, int id, ant_value_t callback, bool tick) {
  held[id] = payload(js, id);
  if (tick) queue_next_tick_with_args(js, callback, &held[id], 1);
  else queue_microtask_with_args(js, callback, &held[id], 1);
}

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  assert(!is_err(result));
}

static bool reaction_visited(ant_t *js, unsigned index) {
  ant_value_t gate = js_get(js, js->global, "gate");
  ant_promise_state_t *state = ant_object_promise_state(js_obj_ptr(js_as_obj(gate)));
  assert(state && state->handlers && index < utarray_len(state->handlers));
  promise_handler_t *handler = (promise_handler_t *)utarray_eltptr(state->handlers, index);
  return js_func_closure(handler->onFulfilled)->gc_epoch == gc_get_epoch(js);
}

static ant_value_t on_noop(ant_params_t) {
  ran++;
  return Ant_Value_Undefined;
}

static ant_value_t on_first_job(ant_params_t) {
  ran++;
  churn(js, 2000);
  gc_run_minor(js);
  assert(gc_obj_is_marked(js, js_obj_ptr(held[10])));
  assert(gc_obj_is_marked(js, js_obj_ptr(held[11])));
  check(js, held[9], 9);

  hold_job(js, 12, noop, false);
  gc_run_minor(js);
  assert(gc_obj_is_marked(js, js_obj_ptr(held[12])));

  hold_job(js, 13, noop, false);
  churn(js, 2000);
  gc_run_minor(js);
  gc_run_minor(js);
  for (int i = 9; i <= 13; i++) check(js, held[i], i);
  assert(minor_job_visits(js) == 0);
  return Ant_Value_Undefined;
}

static ant_value_t on_first_tick(ant_params_t) {
  ran++;
  assert(minor_job_visits(js) == 1);
  check(js, held[15], 15);
  return Ant_Value_Undefined;
}

static ant_value_t on_requeueing_tick(ant_params_t) {
  ran++;
  hold_job(js, 20, noop, true);
  gc_run_minor(js);
  assert(gc_obj_is_marked(js, js_obj_ptr(held[19])));
  assert(gc_obj_is_marked(js, js_obj_ptr(held[20])));
  return Ant_Value_Undefined;
}

static ant_value_t on_first_microtask(ant_params_t) {
  ran++;
  assert(minor_job_visits(js) == 1);
  check(js, held[17], 17);
  return Ant_Value_Undefined;
}

int main(void) {
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, NULL);
  noop = Ant_Function_Create(on_noop);

  for (int i = 0; i < 4; i++) hold_job(js, i, noop, (i & 1) != 0);
  gc_run_minor(js);
  for (int i = 0; i < 4; i++) assert(gc_obj_is_marked(js, js_obj_ptr(held[i])));
  assert(minor_job_visits(js) == 0);

  for (int i = 4; i < 8; i++) hold_job(js, i, noop, (i & 1) != 0);
  churn(js, 2000);
  gc_run_minor(js);
  for (int i = 4; i < 8; i++) assert(gc_obj_is_marked(js, js_obj_ptr(held[i])));
  for (int round = 0; round < 3; round++) {
    churn(js, 2000);
    gc_run_minor(js);
  }
  for (int i = 0; i < 8; i++) check(js, held[i], i);

  gc_run(js);
  for (int i = 0; i < 8; i++) assert(gc_obj_is_marked(js, js_obj_ptr(held[i])));
  process_microtasks(js);
  assert(ran == 8);

  hold_job(js, 8, Ant_Function_Create(on_first_job), false);
  hold_job(js, 9, noop, false);
  gc_run_minor(js);
  hold_job(js, 10, noop, false);
  hold_job(js, 11, noop, false);
  process_microtasks(js);
  assert(ran == 14);
  assert(minor_job_visits(js) == 0);

  hold_job(js, 14, Ant_Function_Create(on_first_tick), true);
  hold_job(js, 15, noop, true);
  process_microtasks(js);
  assert(ran == 16);

  hold_job(js, 16, Ant_Function_Create(on_first_microtask), false);
  hold_job(js, 17, noop, false);
  process_microtasks(js);
  assert(ran == 18);

  hold_job(js, 18, Ant_Function_Create(on_requeueing_tick), true);
  gc_run_minor(js);
  hold_job(js, 19, noop, true);
  process_microtasks(js);
  assert(ran == 21);

  run(js, "globalThis.gate = new Promise(() => {}); gate.then(() => 1); gate.then(() => 2);");
  gc_run(js);
  assert(reaction_visited(js, 0) && reaction_visited(js, 1));

  run(js, "gate.then(() => 3);");
  gc_run_minor(js);
  assert(reaction_visited(js, 2));
  assert(!reaction_visited(js, 0) && !reaction_visited(js, 1));

  run(js, "gate.then(() => 4);");
  gc_run_minor(js);
  assert(reaction_visited(js, 3));
  assert(!reaction_visited(js, 2));
  gc_run(js);
  assert(reaction_visited(js, 0) && reaction_visited(js, 3));

  Ant_Destroy(js);
  puts("PASS pending jobs and reactions: minor collections scan what was queued since the last collection");
}
