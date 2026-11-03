#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "sugar.h"
#include "silver/call.h"

#include "gc/roots.h"
#include "gc/weak.h"
#include "gc/modules.h"

#include "modules/microtasks.h"
#include "modules/process.h"
#include "modules/generator.h"

#ifdef ANT_WASM_EMBED
#include "wasm_embed.h"
#endif

enum {
  MT_CALLBACK = 0,
  MT_PROMISE_TRIGGER,
  MT_THENABLE_JOB,
  MT_AWAIT_RESUME,
  MT_GENERATOR_YIELD,
  MT_PROMISE_RESOLVE,
  MT_AWAIT_RESUME_THROW,
  MT_AWAIT_DEFER,
};

static inline bool microtask_holds_coro(uint8_t kind) {
  return 
    kind == MT_AWAIT_RESUME       || 
    kind == MT_AWAIT_RESUME_THROW || 
    kind == MT_AWAIT_DEFER;
}

typedef struct microtask_entry {
  ant_value_t callback;
  union {
    ant_value_t promise;
    ant_value_t this_val;
    coroutine_t *coro;
  } u;
  struct microtask_entry *next;
  uint8_t argc;
  uint8_t kind;
  ant_value_t argv[];
} microtask_entry_t;

static inline bool microtasks_interrupted(ant_t *js) {
#ifdef ANT_WASM_EMBED
  if (!ant_wasm_should_interrupt(js)) return false;
  ant_microtasks_discard(js);
  return true;
#else
  (void)js;
  return false;
#endif
}

static constexpr int MICROTASK_POOLED_ARGS = 1;
static constexpr uint32_t MICROTASK_FREE_JOBS_MAX = 256;
static constexpr size_t MICROTASK_POOLED_BYTES = sizeof(microtask_entry_t) + MICROTASK_POOLED_ARGS * sizeof(ant_value_t);

static microtask_entry_t *microtask_entry_new(ant_t *js, int nargs) {
  if (nargs > MICROTASK_POOLED_ARGS)
    return calloc(1, sizeof(microtask_entry_t) + (size_t)nargs * sizeof(ant_value_t));

  microtask_entry_t *entry = js->timers.free_jobs;
  if (!entry) return calloc(1, MICROTASK_POOLED_BYTES);

  js->timers.free_jobs = entry->next;
  js->timers.free_job_count--;
  memset(entry, 0, MICROTASK_POOLED_BYTES);
  
  return entry;
}

static void microtask_entry_free(ant_t *js, microtask_entry_t *entry) {
  if (entry->argc > MICROTASK_POOLED_ARGS || js->timers.free_job_count >= MICROTASK_FREE_JOBS_MAX) {
    free(entry);
    return;
  }
  
  entry->next = js->timers.free_jobs;
  js->timers.free_jobs = entry;
  js->timers.free_job_count++;
}

static void queue_microtask_entry(
  microtask_entry_t **head,
  microtask_entry_t **tail,
  microtask_entry_t *entry
) {
  if (*tail == NULL) goto empty;

  (*tail)->next = entry;
  *tail = entry;
  return;

empty:
  *head = entry;
  *tail = entry;
}

static inline void queue_job(ant_t *js, microtask_entry_t *entry) {
  ant_realm_t *own = js->microtask_realm;
  if (__builtin_expect(own != NULL, 0)) queue_microtask_entry(&own->microtasks, &own->microtasks_tail, entry);
  else queue_microtask_entry(&js->timers.microtasks, &js->timers.microtasks_tail, entry);
}

void queue_microtask(ant_t *js, ant_value_t callback) {
  microtask_entry_t *entry = microtask_entry_new(js, 0);
  if (entry == NULL) return;
  
  entry->callback = callback;
  entry->u.promise = Ant_Value_Undefined;
  entry->next = NULL;
  entry->argc = 0;
  
  queue_job(js, entry);
}

ant_value_t js_queue_microtask(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "queueMicrotask requires 1 argument (callback)");
  queue_microtask(js, args[0]);
  return Ant_Value_Undefined;
}

void queue_microtask_with_args(ant_t *js, ant_value_t callback, ant_args_t) {
  if (nargs <= 0) { queue_microtask(js, callback); return; }
  
  microtask_entry_t *entry = microtask_entry_new(js, nargs);
  if (entry == NULL) return;
  
  entry->callback = callback;
  entry->u.promise = Ant_Value_Undefined;
  entry->next = NULL;
  entry->argc = (uint8_t)nargs;
  
  for (int i = 0; i < nargs; i++) entry->argv[i] = args[i];
  queue_job(js, entry);
}

bool queue_promise_thenable_job(ant_t *js, ant_value_t promise, ant_value_t thenable, ant_value_t then_fn) {
  microtask_entry_t *entry = microtask_entry_new(js, 1);
  if (entry == NULL) return false;

  entry->callback = then_fn;
  entry->u.this_val = thenable;
  entry->next = NULL;
  entry->argc = 1;
  entry->kind = MT_THENABLE_JOB;
  entry->argv[0] = promise;

  queue_job(js, entry);
  
  return true;
}

bool queue_generator_yield_job(ant_t *js, ant_value_t gen, ant_value_t value) {
  microtask_entry_t *entry = microtask_entry_new(js, 0);
  if (entry == NULL) return false;

  entry->callback = value;
  entry->u.this_val = gen;
  entry->kind = MT_GENERATOR_YIELD;
  queue_microtask_entry(&js->timers.microtasks, &js->timers.microtasks_tail, entry);
  
  return true;
}

bool queue_promise_resolve_job(ant_t *js, ant_value_t promise, ant_value_t value) {
  microtask_entry_t *entry = microtask_entry_new(js, 0);
  if (entry == NULL) return false;

  entry->callback = value;
  entry->u.promise = promise;
  entry->kind = MT_PROMISE_RESOLVE;
  queue_job(js, entry);
  
  return true;
}

static bool queue_coro_job(coroutine_t *coro, ant_value_t value, uint8_t kind) {
  ant_t *js = coro->js;
  microtask_entry_t *entry = microtask_entry_new(js, 0);
  if (entry == NULL) return false;

  entry->callback = value;
  entry->u.coro = coro;
  entry->next = NULL;
  entry->kind = kind;

  coroutine_retain(coro);
  coro->await_resume_job = entry;
  queue_job(js, entry);
  
  return true;
}

bool queue_await_resume_job(coroutine_t *coro, ant_value_t value) {
  return queue_coro_job(coro, value, MT_AWAIT_RESUME);
}

bool queue_await_settle_job(coroutine_t *coro, ant_value_t value, bool is_error) {
  return queue_coro_job(coro, value, is_error ? MT_AWAIT_RESUME_THROW : MT_AWAIT_RESUME);
}

bool queue_await_defer_job(coroutine_t *coro, ant_value_t value) {
  return queue_coro_job(coro, value, MT_AWAIT_DEFER);
}

void queue_next_tick(ant_t *js, ant_value_t callback) {
  microtask_entry_t *entry = microtask_entry_new(js, 0);
  if (entry == NULL) return;

  entry->callback = callback;
  entry->u.promise = Ant_Value_Undefined;
  entry->next = NULL;
  entry->argc = 0;

  queue_microtask_entry(&js->timers.next_ticks, &js->timers.next_ticks_tail, entry);
}

void queue_next_tick_with_args(ant_t *js, ant_value_t callback, ant_args_t) {
  if (nargs <= 0) { queue_next_tick(js, callback); return; }

  microtask_entry_t *entry = microtask_entry_new(js, nargs);
  if (entry == NULL) return;

  entry->callback = callback;
  entry->u.promise = Ant_Value_Undefined;
  entry->next = NULL;
  entry->argc = (uint8_t)nargs;

  for (int i = 0; i < nargs; i++) entry->argv[i] = args[i];
  queue_microtask_entry(&js->timers.next_ticks, &js->timers.next_ticks_tail, entry);
}

void queue_promise_trigger(ant_t *js, ant_value_t promise) {
  if (!js_mark_promise_trigger_queued(js, promise)) return;

  microtask_entry_t *entry = microtask_entry_new(js, 0);
  if (entry == NULL) {
    js_mark_promise_trigger_dequeued(js, promise);
    return;
  }
  
  entry->callback = Ant_Value_Undefined;
  entry->u.promise = promise;
  entry->next = NULL;
  entry->kind = MT_PROMISE_TRIGGER;

  queue_job(js, entry);
}

static inline void process_microtask_entry(ant_t *js, microtask_entry_t *entry) {
  if (!entry) return;

  if (entry->kind == MT_PROMISE_TRIGGER) {
    GC_ROOT_SAVE(root_mark, js);
    ant_value_t promise = entry->u.promise;

    GC_ROOT_PIN(js, promise);
    js_mark_promise_trigger_dequeued(js, promise);
    js_process_promise_handlers(js, promise);
    GC_ROOT_RESTORE(js, root_mark);

    return;
  }

  if (entry->kind == MT_GENERATOR_YIELD) {
    Ant_Generator_SettleYield(js, entry->u.this_val, entry->callback);
    return;
  }

  if (entry->kind == MT_THENABLE_JOB) {
    js_process_promise_thenable_job(
      js, entry->argv[0], 
      entry->u.this_val, entry->callback
    );
    return;
  }

  if (entry->kind == MT_PROMISE_RESOLVE) {
    GC_ROOT_SAVE(root_mark, js);
    ant_value_t promise = entry->u.promise;
    ant_value_t value = entry->callback;
    
    GC_ROOT_PIN(js, promise);
    GC_ROOT_PIN(js, value);
    
    js_resolve_promise(js, promise, value);
    GC_ROOT_RESTORE(js, root_mark);
    
    return;
  }

  if (entry->kind == MT_AWAIT_DEFER) {
    GC_ROOT_SAVE(root_mark, js);
    
    ant_value_t value = entry->callback;
    coroutine_t *coro = entry->u.coro;
    
    GC_ROOT_PIN(js, value);

    if (
      coro->await_registered && coro->await_resume_job == entry && 
      !queue_await_resume_job(coro, value)
    ) Ant_Coroutine_ResumeAwaitJob(js, coro, value, false);

    coroutine_release(coro);
    GC_ROOT_RESTORE(js, root_mark);
    
    return;
  }

  if (entry->kind == MT_AWAIT_RESUME || entry->kind == MT_AWAIT_RESUME_THROW) {
    GC_ROOT_SAVE(root_mark, js);
    
    ant_value_t value = entry->callback;
    coroutine_t *coro = entry->u.coro;
    
    GC_ROOT_PIN(js, value);

    if (coro->await_registered && coro->await_resume_job == entry)
      Ant_Coroutine_ResumeAwaitJob(js, coro, value, entry->kind == MT_AWAIT_RESUME_THROW);

    coroutine_release(coro);
    GC_ROOT_RESTORE(js, root_mark);
    
    return;
  }

  ant_value_t this_val = Ant_Value_Undefined;
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t callback = entry->callback;
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, this_val);

  for (uint8_t i = 0; i < entry->argc; i++) GC_ROOT_PIN(js, entry->argv[i]);
  sv_vm_call(js->vm, js, callback, this_val, entry->argv, entry->argc, NULL, Ant_Value_Undefined);

  GC_ROOT_RESTORE(js, root_mark);
  process_report_uncaught_exception_if_pending(js);
}

static inline microtask_entry_t *take_microtask_batch(ant_t *js) {
  microtask_entry_t *batch = js->timers.microtasks;

  js->timers.microtasks = NULL;
  js->timers.microtasks_tail = NULL;
  js->timers.microtasks_processing = batch;
  
  return batch;
}

static inline microtask_entry_t *take_next_tick_batch(ant_t *js) {
  microtask_entry_t *batch = js->timers.next_ticks;

  js->timers.next_ticks = NULL;
  js->timers.next_ticks_tail = NULL;
  js->timers.next_ticks_processing = batch;
  
  return batch;
}

static inline bool process_microtask_batch(ant_t *js, microtask_entry_t *batch) {
  while (batch != NULL) {
    microtask_entry_t *entry = batch;
    
    batch = entry->next;
    js->timers.microtasks_processing = batch;
    
    process_microtask_entry(js, entry);
    microtask_entry_free(js, entry);
    if (microtasks_interrupted(js)) return false;
  }
  return true;
}

static inline bool process_next_tick_batch(ant_t *js, microtask_entry_t *batch) {
  while (batch != NULL) {
    microtask_entry_t *entry = batch;
    
    batch = entry->next;
    js->timers.next_ticks_processing = batch;
    
    process_microtask_entry(js, entry);
    microtask_entry_free(js, entry);
    if (microtasks_interrupted(js)) return false;
  }
  return true;
}

static void process_microtasks_internal(ant_t *js, bool check_unhandled_rejections) {
  microtask_entry_t *batch = NULL;

  if (!js || js->microtasks_draining || js->destroying) return;
  bool at_job_boundary = js->vm_exec_depth == 0;
  bool completed = true;
  js->microtasks_draining = true;

  while (completed && (js->timers.next_ticks != NULL || js->timers.microtasks != NULL)) {
    while (completed && (batch = js->timers.next_ticks) != NULL) {
      batch = take_next_tick_batch(js);
      completed = process_next_tick_batch(js, batch);
    }
    
    while (completed && (batch = js->timers.microtasks) != NULL) {
      batch = take_microtask_batch(js);
      completed = process_microtask_batch(js, batch);
    }
  }

  js->timers.next_ticks_processing = NULL;
  js->timers.microtasks_processing = NULL;
  
  if (check_unhandled_rejections && completed) js_check_unhandled_rejections(js);
  
  js->microtasks_draining = false;
  if (at_job_boundary) gc_weak_clear_kept_alive(js);
}

void ant_realm_run_microtasks(ant_t *js, ant_realm_t *realm) {
while (realm->microtasks) {
  microtask_entry_t *batch = realm->microtasks;
  realm->microtasks = realm->microtasks_tail = NULL;
  
  while (batch) {
    microtask_entry_t *entry = batch;
    
    batch = entry->next;
    realm->microtasks_processing = batch;
    
    process_microtask_entry(js, entry);
    microtask_entry_free(js, entry);
  }
}}

void process_microtasks(ant_t *js) {
  process_microtasks_internal(js, true);
}

bool js_maybe_drain_microtasks(ant_t *js) {
  if (!js) return false;
  if (js->microtasks_draining) return false;
  if (js->vm_exec_depth != 0 || js->jit_active_depth != 0) return false;
  
  if (!has_pending_microtasks(js)) return false;
  process_microtasks_internal(js, true);
  
  return true;
}

bool js_maybe_drain_microtasks_after_async_settle(ant_t *js) {
  if (!js) return false;

  if (js->microtasks_draining) return false;
  if (!has_pending_microtasks(js)) return false;

  process_microtasks_internal(js, false);
  return true;
}

int has_pending_microtasks(ant_t *js) {
  return (js->timers.next_ticks != NULL || js->timers.microtasks != NULL) ? 1 : 0;
}

static void discard_jobs(ant_t *js, microtask_entry_t *entry) {
  while (entry) {
    microtask_entry_t *next = entry->next;
    
    if (microtask_holds_coro(entry->kind)) {
      coroutine_clear_await_registration(entry->u.coro);
      coroutine_release(entry->u.coro);
    } else if (entry->kind == MT_PROMISE_TRIGGER) 
      js_mark_promise_trigger_dequeued(js, entry->u.promise);
    
    free(entry);
    entry = next;
  }
}

void ant_microtasks_discard(ant_t *js) {
  ant_timer_state_t *t = &js->timers;

  discard_jobs(js, t->microtasks_processing);
  discard_jobs(js, t->next_ticks_processing);
  discard_jobs(js, t->microtasks);
  discard_jobs(js, t->next_ticks);
  
  for (ant_realm_t *realm = js->realms; realm; realm = realm->next) {
    discard_jobs(js, realm->microtasks);
    realm->microtasks = realm->microtasks_tail = NULL;
  }
  
  t->microtasks_processing = NULL;
  t->next_ticks_processing = NULL;
  t->microtasks = t->microtasks_tail = NULL;
  t->next_ticks = t->next_ticks_tail = NULL;

  while (t->free_jobs) {
    microtask_entry_t *next = t->free_jobs->next;
    free(t->free_jobs);
    t->free_jobs = next;
  }
  
  t->free_job_count = 0;
}

static void gc_mark_jobs(ant_t *js, microtask_entry_t *m, gc_mark_fn mark) {
  for (; m; m = m->next) {
    mark(js, m->callback);
    if (microtask_holds_coro(m->kind)) gc_mark_coroutine(js, m->u.coro);
    else mark(js, m->u.promise);
    for (uint8_t i = 0; i < m->argc; i++) mark(js, m->argv[i]);
  }
}

void gc_mark_microtasks(ant_t *js, gc_mark_fn mark) {
  gc_mark_jobs(js, js->timers.microtasks, mark);
  gc_mark_jobs(js, js->timers.microtasks_processing, mark);

  for (ant_realm_t *realm = js->realms; realm; realm = realm->next) {
    gc_mark_jobs(js, realm->microtasks, mark);
    gc_mark_jobs(js, realm->microtasks_processing, mark);
  }

  for (microtask_entry_t *m = js->timers.next_ticks; m; m = m->next) {
    mark(js, m->callback);
    mark(js, m->u.promise);
    for (uint8_t i = 0; i < m->argc; i++) mark(js, m->argv[i]);
  }

  for (microtask_entry_t *m = js->timers.next_ticks_processing; m; m = m->next) {
    mark(js, m->callback);
    mark(js, m->u.promise);
    for (uint8_t i = 0; i < m->argc; i++) mark(js, m->argv[i]);
  }
}
