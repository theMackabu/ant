#include "ant.h"
#include "errors.h"
#include "esm/loader.h"
#include "gc/modules.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "gc/weak.h"
#include "highlight.h"
#include "modules/blob.h"
#include "modules/buffer.h"
#include "modules/date.h"
#include "sugar.h"
#include "silver/engine.h"
#include "wasm_embed.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool io_no_color = true;

#define ANT_WASM_GC_STUB(name) \
  void name(ant_t *js, gc_mark_fn mark) { (void)js; (void)mark; }

ANT_WASM_GC_STUB(gc_mark_cron)
ANT_WASM_GC_STUB(gc_mark_atomics)
ANT_WASM_GC_STUB(gc_mark_fetch)
ANT_WASM_GC_STUB(gc_mark_fs)
ANT_WASM_GC_STUB(gc_mark_child_process)
ANT_WASM_GC_STUB(gc_mark_readline)
ANT_WASM_GC_STUB(gc_mark_process)
ANT_WASM_GC_STUB(gc_mark_navigator)
ANT_WASM_GC_STUB(gc_mark_net)
ANT_WASM_GC_STUB(gc_mark_tls)
ANT_WASM_GC_STUB(gc_mark_server)
ANT_WASM_GC_STUB(gc_mark_websocket)
ANT_WASM_GC_STUB(gc_mark_eventsource)
ANT_WASM_GC_STUB(gc_mark_events)
ANT_WASM_GC_STUB(gc_mark_lmdb)
ANT_WASM_GC_STUB(gc_mark_esm)
ANT_WASM_GC_STUB(gc_mark_worker_threads)
ANT_WASM_GC_STUB(gc_mark_abort)
ANT_WASM_GC_STUB(gc_mark_zlib)
ANT_WASM_GC_STUB(gc_mark_wasm)
ANT_WASM_GC_STUB(gc_mark_napi)
ANT_WASM_GC_STUB(gc_mark_rpc)
ANT_WASM_GC_STUB(gc_mark_sandbox)

void gc_clear_napi_weak_refs(ant_t *js, bool minor) {
  (void)js;
  (void)minor;
}

void gc_mark_abort_signal_object(
  ant_t *js, ant_value_t signal, gc_mark_fn mark
) {
  (void)js;
  (void)signal;
  (void)mark;
}

void gc_mark_eventemitter_object(
  ant_t *js, ant_value_t obj, gc_mark_fn mark
) {
  (void)js;
  (void)obj;
  (void)mark;
}

void gc_finalize_events_object(ant_t *js, ant_value_t obj) {
  (void)js;
  (void)obj;
}

void cleanup_cron_module(ant_t *js) { (void)js; }
void cleanup_rpc_module(void) {}
void cleanup_lmdb_module(void) {}
void cleanup_buffer_module(void) {}
void cleanup_atomics_module(ant_t *js) { (void)js; }
void cleanup_events_module(ant_t *js) { (void)js; }

bool is_date_instance(ant_value_t value) {
  (void)value;
  return false;
}

ant_value_t get_date_string(
  ant_t *js, ant_value_t this_val, date_string_spec_t spec
) {
  (void)this_val;
  (void)spec;
  return js_mkundef();
}

TypedArrayData *buffer_get_typedarray_data(ant_value_t value) {
  (void)value;
  return NULL;
}

ArrayBufferData *buffer_get_arraybuffer_data(ant_value_t value) {
  (void)value;
  return NULL;
}

DataViewData *buffer_get_dataview_data(ant_value_t value) {
  (void)value;
  return NULL;
}

const char *buffer_typedarray_type_name(TypedArrayType type) {
  (void)type;
  return "TypedArray";
}

bool buffer_typedarray_data_read_index(
  ant_t *js, const TypedArrayData *ta_data, size_t index, ant_value_t *out
) {
  (void)js;
  (void)ta_data;
  (void)index;
  (void)out;
  return false;
}

blob_data_t *blob_get_data(ant_value_t obj) {
  (void)obj;
  return NULL;
}

enum {
  ANT_WASM_MICROTASK_CALLBACK,
  ANT_WASM_MICROTASK_PROMISE_TRIGGER,
  ANT_WASM_MICROTASK_THENABLE,
  ANT_WASM_MICROTASK_AWAIT,
};

typedef struct ant_wasm_microtask {
  ant_value_t callback;
  union {
    ant_value_t promise;
    ant_value_t this_value;
    coroutine_t *coroutine;
  } target;
  struct ant_wasm_microtask *next;
  uint8_t argument_count;
  uint8_t kind;
  ant_value_t arguments[];
} ant_wasm_microtask_t;

static struct {
  ant_t *js;
  ant_wasm_microtask_t *head;
  ant_wasm_microtask_t *tail;
  ant_wasm_microtask_t *processing;
} ant_wasm_microtasks;

static void ant_wasm_microtask_discard(
  ant_t *js, ant_wasm_microtask_t *task
) {
  if (task->kind == ANT_WASM_MICROTASK_PROMISE_TRIGGER)
    js_mark_promise_trigger_dequeued(js, task->target.promise);
  else if (task->kind == ANT_WASM_MICROTASK_AWAIT) {
    coroutine_clear_await_registration(task->target.coroutine);
    coroutine_release(task->target.coroutine);
  }
  free(task);
}

void ant_wasm_microtasks_reset(ant_t *js) {
  ant_wasm_microtask_t *task = ant_wasm_microtasks.processing;
  while (task) {
    ant_wasm_microtask_t *next = task->next;
    ant_wasm_microtask_discard(js, task);
    task = next;
  }
  task = ant_wasm_microtasks.head;
  while (task) {
    ant_wasm_microtask_t *next = task->next;
    ant_wasm_microtask_discard(js, task);
    task = next;
  }
  memset(&ant_wasm_microtasks, 0, sizeof(ant_wasm_microtasks));
}

static void ant_wasm_microtask_append(
  ant_t *js, ant_wasm_microtask_t *task
) {
  if (ant_wasm_microtasks.js && ant_wasm_microtasks.js != js)
    ant_wasm_microtasks_reset(ant_wasm_microtasks.js);
  ant_wasm_microtasks.js = js;
  if (ant_wasm_microtasks.tail)
    ant_wasm_microtasks.tail->next = task;
  else
    ant_wasm_microtasks.head = task;
  ant_wasm_microtasks.tail = task;
}

void queue_microtask(ant_t *js, ant_value_t callback) {
  ant_wasm_microtask_t *task = calloc(1, sizeof(*task));
  if (!task) return;
  task->callback = callback;
  task->target.promise = js_mkundef();
  ant_wasm_microtask_append(js, task);
}

void queue_microtask_with_args(
  ant_t *js, ant_value_t callback, ant_value_t *args, int nargs
) {
  if (nargs <= 0) {
    queue_microtask(js, callback);
    return;
  }
  if (nargs > UINT8_MAX) return;
  ant_wasm_microtask_t *task = calloc(
    1, sizeof(*task) + (size_t)nargs * sizeof(*task->arguments)
  );
  if (!task) return;
  task->callback = callback;
  task->target.promise = js_mkundef();
  task->argument_count = (uint8_t)nargs;
  memcpy(task->arguments, args, (size_t)nargs * sizeof(*args));
  ant_wasm_microtask_append(js, task);
}

void queue_promise_trigger(ant_t *js, ant_value_t promise) {
  if (!js_mark_promise_trigger_queued(js, promise)) return;
  ant_wasm_microtask_t *task = calloc(1, sizeof(*task));
  if (!task) {
    js_mark_promise_trigger_dequeued(js, promise);
    return;
  }
  task->callback = js_mkundef();
  task->target.promise = promise;
  task->kind = ANT_WASM_MICROTASK_PROMISE_TRIGGER;
  ant_wasm_microtask_append(js, task);
}

bool queue_promise_thenable_job(
  ant_t *js, ant_value_t promise, ant_value_t thenable, ant_value_t then_fn
) {
  ant_wasm_microtask_t *task = calloc(
    1, sizeof(*task) + sizeof(*task->arguments)
  );
  if (!task) return false;
  task->callback = then_fn;
  task->target.this_value = thenable;
  task->argument_count = 1;
  task->kind = ANT_WASM_MICROTASK_THENABLE;
  task->arguments[0] = promise;
  ant_wasm_microtask_append(js, task);
  return true;
}

bool queue_await_resume_job(coroutine_t *coro, ant_value_t value) {
  if (!coro || !coro->js) return false;
  ant_wasm_microtask_t *task = calloc(1, sizeof(*task));
  if (!task) return false;
  task->callback = value;
  task->target.coroutine = coro;
  task->kind = ANT_WASM_MICROTASK_AWAIT;
  coroutine_retain(coro);
  ant_wasm_microtask_append(coro->js, task);
  return true;
}

static void ant_wasm_process_microtask(
  ant_t *js, ant_wasm_microtask_t *task
) {
  if (task->kind == ANT_WASM_MICROTASK_PROMISE_TRIGGER) {
    GC_ROOT_SAVE(root_mark, js);
    ant_value_t promise = task->target.promise;
    GC_ROOT_PIN(js, promise);
    js_mark_promise_trigger_dequeued(js, promise);
    js_process_promise_handlers(js, promise);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  if (task->kind == ANT_WASM_MICROTASK_THENABLE) {
    js_process_promise_thenable_job(
      js, task->arguments[0], task->target.this_value, task->callback
    );
    return;
  }

  if (task->kind == ANT_WASM_MICROTASK_AWAIT) {
    GC_ROOT_SAVE(root_mark, js);
    ant_value_t value = task->callback;
    coroutine_t *coro = task->target.coroutine;
    GC_ROOT_PIN(js, value);
    if (coro->await_registered)
      settle_and_resume_coroutine(js, coro, value, false);
    coroutine_release(coro);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t callback = task->callback;
  GC_ROOT_PIN(js, callback);
  for (uint8_t index = 0; index < task->argument_count; index++)
    GC_ROOT_PIN(js, task->arguments[index]);
  sv_vm_call(
    js->vm, js, callback, js_mkundef(), task->arguments,
    task->argument_count, NULL, false
  );
  GC_ROOT_RESTORE(js, root_mark);
}

static bool ant_wasm_drain_microtasks(
  ant_t *js, bool check_rejections, bool require_job_boundary
) {
  if (!js || js->microtasks_draining ||
      (require_job_boundary && js->vm_exec_depth != 0) ||
      ant_wasm_microtasks.head == NULL)
    return false;

  bool at_job_boundary = js->vm_exec_depth == 0;
  bool interrupted = false;
  js->microtasks_draining = true;
  while (ant_wasm_microtasks.head) {
    ant_wasm_microtask_t *batch = ant_wasm_microtasks.head;
    ant_wasm_microtasks.head = NULL;
    ant_wasm_microtasks.tail = NULL;
    ant_wasm_microtasks.processing = batch;

    while (batch) {
      ant_wasm_microtask_t *task = batch;
      batch = task->next;
      ant_wasm_microtasks.processing = batch;
      ant_wasm_process_microtask(js, task);
      free(task);
      if (ant_wasm_should_interrupt(js)) {
        interrupted = true;
        ant_wasm_microtasks_reset(js);
        break;
      }
    }
    if (interrupted) break;
  }

  ant_wasm_microtasks.processing = NULL;
  if (check_rejections && !interrupted) js_check_unhandled_rejections(js);
  js->microtasks_draining = false;
  if (at_job_boundary) gc_weak_clear_kept_alive(js);
  reap_retired_coroutines(js);
  return true;
}

void process_microtasks(ant_t *js) {
  ant_wasm_drain_microtasks(js, true, false);
}

bool js_maybe_drain_microtasks(ant_t *js) {
  return ant_wasm_drain_microtasks(js, true, true);
}

bool js_maybe_drain_microtasks_after_async_settle(ant_t *js) {
  return ant_wasm_drain_microtasks(js, false, false);
}

int has_pending_microtasks(void) {
  return ant_wasm_microtasks.head != NULL;
}

void gc_mark_timers(ant_t *js, gc_mark_fn mark) {
  ant_wasm_microtask_t *lists[] = {
    ant_wasm_microtasks.head,
    ant_wasm_microtasks.processing,
  };
  for (size_t list = 0; list < sizeof(lists) / sizeof(lists[0]); list++) {
    for (ant_wasm_microtask_t *task = lists[list]; task; task = task->next) {
      mark(js, task->callback);
      if (task->kind == ANT_WASM_MICROTASK_AWAIT)
        gc_mark_coroutine(js, task->target.coroutine);
      else if (task->kind == ANT_WASM_MICROTASK_THENABLE)
        mark(js, task->target.this_value);
      else
        mark(js, task->target.promise);
      for (uint8_t index = 0; index < task->argument_count; index++)
        mark(js, task->arguments[index]);
    }
  }
}

bool js_fire_unhandled_rejection(
  ant_t *js, ant_value_t promise_val, ant_value_t reason
) {
  (void)js;
  (void)promise_val;
  (void)reason;
  return false;
}

void js_fire_rejection_handled(
  ant_t *js, ant_value_t promise_val, ant_value_t reason
) {
  (void)js;
  (void)promise_val;
  (void)reason;
}

ant_value_t js_esm_import_sync(ant_t *js, ant_value_t specifier) {
  (void)specifier;
  return js_mkerr_typed(
    js, JS_ERR_TYPE, "module imports are not available in @antjs.org/wasm"
  );
}

ant_value_t js_esm_import_dynamic_ex(
  ant_t *js, ant_value_t specifier, const char *base_path,
  ant_value_t attrs, ant_value_t *out_tla_promise
) {
  (void)specifier;
  (void)base_path;
  (void)attrs;
  *out_tla_promise = js_mkundef();
  return js_mkerr_typed(
    js, JS_ERR_TYPE, "module imports are not available in @antjs.org/wasm"
  );
}

void js_esm_cleanup_module_cache(ant_t *js) { (void)js; }

int highlight_js_line_clipped(
  const char *line, size_t line_len, size_t max_cols,
  char *out, size_t out_size, highlight_state *state
) {
  (void)state;
  size_t len = line_len < max_cols ? line_len : max_cols;
  if (len >= out_size) len = out_size ? out_size - 1 : 0;
  if (len) memcpy(out, line, len);
  if (out_size) out[len] = '\0';
  return (int)len;
}

int crypto_fill_random(void *buf, size_t len) {
  if (len > UINT32_MAX) return -1;
  return ant_wasm_random_fill(buf, (uint32_t)len);
}

void init_async_iterator_helpers(ant_t *js) { (void)js; }
