#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#include "errors.h"
#include "runtime.h"
#include "internal.h"

#include "silver/engine.h"
#include "gc/roots.h"
#include "gc/modules.h"
#include "modules/abort.h"
#include "modules/timer.h"
#include "modules/symbol.h"

typedef struct timer_entry {
  uv_timer_t handle;
  ant_value_t callback;
  ant_value_t *args;
  int nargs;
  int timer_id;
  int active;
  int closed;
  int is_interval;
  uint64_t timeout_ms;
  struct timer_entry *next;
  struct timer_entry *prev;
} timer_entry_t;

typedef struct microtask_entry {
  ant_value_t callback;
  ant_value_t promise;
  struct microtask_entry *next;
  uint8_t argc;
  ant_value_t argv[];
} microtask_entry_t;

typedef struct immediate_entry {
  ant_value_t callback;
  int immediate_id;
  int active;
  struct immediate_entry *next;
} immediate_entry_t;

static struct {
  ant_t *js;
  timer_entry_t *timers;
  
  microtask_entry_t *next_ticks;
  microtask_entry_t *next_ticks_tail;
  microtask_entry_t *next_ticks_processing;
  microtask_entry_t *microtasks;
  microtask_entry_t *microtasks_tail;
  microtask_entry_t *microtasks_processing;
  immediate_entry_t *immediates;
  immediate_entry_t *immediates_tail;
  
  int next_timer_id;
  int next_immediate_id;
  int active_timer_count;
} timer_state = {
  .js = NULL,
  .timers = NULL,
  .next_ticks = NULL,
  .next_ticks_tail = NULL,
  .next_ticks_processing = NULL,
  .microtasks = NULL,
  .microtasks_tail = NULL,
  .microtasks_processing = NULL,
  .immediates = NULL,
  .immediates_tail = NULL,
  .next_timer_id = 1,
  .next_immediate_id = 1,
  .active_timer_count = 0,
};

static ant_value_t g_timeout_proto = 0;
static ant_value_t g_interval_proto = 0;

static void add_timer_entry(timer_entry_t *entry) {
  entry->next = timer_state.timers;
  entry->prev = NULL;
  if (timer_state.timers) timer_state.timers->prev = entry;
  timer_state.timers = entry;
}

static void remove_timer_entry(timer_entry_t *entry) {
  if (entry->prev) entry->prev->next = entry->next;
  else timer_state.timers = entry->next;
  if (entry->next) entry->next->prev = entry->prev;
}

static int timer_entry_is_registered(timer_entry_t *entry) {
  for (timer_entry_t *it = timer_state.timers; it != NULL; it = it->next)
    if (it == entry) return 1;
  return 0;
}

static timer_entry_t *find_timer_entry_by_id(int timer_id) {
  for (timer_entry_t *entry = timer_state.timers; entry != NULL; entry = entry->next)
    if (entry->timer_id == timer_id) return entry;
  return NULL;
}

static int timer_copy_args(timer_entry_t *entry, ant_value_t *args, int nargs) {
  entry->nargs = nargs > 2 ? nargs - 2 : 0;
  if (entry->nargs > 0) {
    entry->args = ant_calloc(sizeof(ant_value_t) * entry->nargs);
    if (!entry->args) return -1;
    memcpy(entry->args, args + 2, sizeof(ant_value_t) * entry->nargs);
  } else entry->args = NULL;
  return 0;
}

static void timer_release_args(timer_entry_t *entry) {
  if (!entry) return;
  if (entry->args) {
    free(entry->args);
    entry->args = NULL;
  }
  entry->nargs = 0;
}

static void timer_release_callback_args(timer_entry_t *entry) {
  if (!entry) return;
  entry->callback = js_mkundef();
  timer_release_args(entry);
}

static int timer_copy_args_from_object(ant_t *js, timer_entry_t *entry, ant_value_t obj) {
  ant_value_t args_arr = js_get_slot(obj, SLOT_AUX);
  ant_offset_t len = vtype(args_arr) == T_ARR ? js_arr_len(js, args_arr) : 0;

  timer_release_args(entry);
  if (len == 0) return 0;

  entry->args = ant_calloc(sizeof(ant_value_t) * (size_t)len);
  if (!entry->args) return -1;
  entry->nargs = (int)len;
  for (ant_offset_t i = 0; i < len; i++) entry->args[i] = js_arr_get(js, args_arr, i);
  return 0;
}

static ant_value_t timer_make_args_array(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js_mkundef();
  int arg_count = nargs > 2 ? nargs - 2 : 0;

  if (arg_count <= 0) return arr;
  arr = js_mkarr(js);
  for (int i = 0; i < arg_count; i++) js_arr_push(js, arr, args[i + 2]);
  return arr;
}

static ant_value_t timer_to_primitive(ant_t *js, ant_value_t *args, int nargs) {
  return js_get_slot(js_getthis(js), SLOT_DATA);
}

static ant_value_t timer_inspect(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t id_val = js_get_slot(this_obj, SLOT_DATA);
  int timer_id = vtype(id_val) == T_NUM ? (int)js_getnum(id_val) : 0;

  ant_value_t tag_val = js_get_sym(js, this_obj, get_toStringTag_sym());
  const char *tag = vtype(tag_val) == T_STR ? js_getstr(js, tag_val, NULL) : "Timeout";

  js_inspect_builder_t builder;
  if (!js_inspect_builder_init_dynamic(&builder, js, 128)) {
    return js_mkerr(js, "out of memory");
  }

  bool ok = js_inspect_header(&builder, "%s (%d)", tag, timer_id);
  if (ok) ok = js_inspect_object_body(&builder, this_obj);
  if (ok) ok = js_inspect_close(&builder);
  
  if (!ok) {
    js_inspect_builder_dispose(&builder);
    return js_mkerr(js, "out of memory");
  }

  return js_inspect_builder_result(&builder);
}

static ant_value_t js_timer_ref(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  timer_entry_t *entry = find_timer_entry_by_id((int)js_getnum(js_get_slot(this_obj, SLOT_DATA)));
  if (entry && !entry->closed && !uv_is_closing((uv_handle_t *)&entry->handle))
    uv_ref((uv_handle_t *)&entry->handle);
  return this_obj;
}

static ant_value_t js_timer_unref(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  timer_entry_t *entry = find_timer_entry_by_id((int)js_getnum(js_get_slot(this_obj, SLOT_DATA)));
  if (entry && !entry->closed && !uv_is_closing((uv_handle_t *)&entry->handle))
    uv_unref((uv_handle_t *)&entry->handle);
  return this_obj;
}

static ant_value_t js_timer_has_ref(ant_t *js, ant_value_t *args, int nargs) {
  timer_entry_t *entry = find_timer_entry_by_id((int)js_getnum(js_get_slot(js_getthis(js), SLOT_DATA)));
  if (!entry || entry->closed || uv_is_closing((uv_handle_t *)&entry->handle)) return js_false;
  return js_bool(uv_has_ref((const uv_handle_t *)&entry->handle) != 0);
}

static ant_value_t timer_make_object(
  ant_t *js,
  int id,
  double delay_ms,
  int is_interval,
  ant_value_t callback,
  ant_value_t timer_args
) {
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = is_interval ? g_interval_proto : g_timeout_proto;

  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  js_set(js, obj, "delay", js_mknum(delay_ms));
  js_set(js, obj, "repeat", is_interval ? js_mknum(delay_ms) : js_mknull());
  
  js_set(js, obj, "callback", callback);
  js_set_descriptor(js, obj, "callback", 8, JS_DESC_W | JS_DESC_C);
  
  js_set_slot(obj, SLOT_DATA, js_mknum((double)id));
  js_set_slot_wb(js, obj, SLOT_AUX, timer_args);
  js_set_sym(js, obj, get_toPrimitive_sym(), js_mkfun(timer_to_primitive));

  return obj;
}

static int timer_id_from_arg(ant_t *js, ant_value_t arg) {
  if (vtype(arg) == T_NUM) return (int)js_getnum(arg);
  return (int)js_getnum(js_get_slot(arg, SLOT_DATA));
}

static void timer_close_cb(uv_handle_t *h) {
  timer_entry_t *entry = (timer_entry_t *)h->data;
  if (!entry) return;
  if (entry->closed) return;
  if (timer_entry_is_registered(entry)) remove_timer_entry(entry);
  entry->closed = 1;
  entry->active = 0;
  timer_release_callback_args(entry);
  entry->next = NULL;
  entry->prev = NULL;
}

static void timer_callback(uv_timer_t *handle) {
  timer_entry_t *entry = (timer_entry_t *)handle->data;
  if (!entry || entry->closed || !timer_entry_is_registered(entry) || !entry->active) return;
  
  ant_t *js = timer_state.js;
  ant_value_t callback = entry->callback;
  if (!entry->is_interval) {
    entry->active = 0;
    timer_state.active_timer_count--;
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, callback);
  for (int i = 0; i < entry->nargs; i++) GC_ROOT_PIN(js, entry->args[i]);
  sv_vm_call(js->vm, js, callback, js_mkundef(), entry->args, entry->nargs, NULL, false);
  GC_ROOT_RESTORE(js, root_mark);
  if (!entry->is_interval && !entry->active) timer_release_callback_args(entry);
  process_microtasks(js);
}

static ant_value_t js_timer_refresh(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  
  timer_entry_t *entry = find_timer_entry_by_id((int)js_getnum(js_get_slot(this_obj, SLOT_DATA)));
  if (!entry || entry->closed || uv_is_closing((uv_handle_t *)&entry->handle)) return this_obj;

  if (!entry->active) {
    if (vtype(entry->callback) == T_UNDEF) {
      entry->callback = js_get(js, this_obj, "callback");
      if (!is_callable(entry->callback)) return this_obj;
      if (timer_copy_args_from_object(js, entry, this_obj) != 0)
        return js_mkerr(js, "failed to allocate timer args");
    }
    entry->active = 1;
    timer_state.active_timer_count++;
  }

  uv_timer_start(
    &entry->handle,
    timer_callback,
    entry->timeout_ms,
    entry->is_interval ? entry->timeout_ms : 0
  );
  
  return this_obj;
}

// setTimeout(callback, delay, ...args)
static ant_value_t js_set_timeout(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "setTimeout requires at least 1 argument (callback)");
  }
  
  ant_value_t callback = args[0];
  double delay_ms = nargs > 1 ? js_getnum(args[1]) : 0;
  uint64_t ms = delay_ms >= 1 ? (uint64_t)delay_ms : 0;
  ant_value_t timer_args = timer_make_args_array(js, args, nargs);
  
  timer_entry_t *entry = ant_calloc(sizeof(timer_entry_t));
  if (entry == NULL) return js_mkerr(js, "failed to allocate timer");
  
  if (timer_copy_args(entry, args, nargs) < 0) {
    free(entry);
    return js_mkerr(js, "failed to allocate timer args");
  }
  
  uv_timer_init(uv_default_loop(), &entry->handle);
  entry->handle.data = entry;
  entry->callback = callback;
  entry->timer_id = timer_state.next_timer_id++;
  entry->active = 1;
  entry->closed = 0;
  entry->is_interval = 0;
  entry->timeout_ms = ms;
  
  add_timer_entry(entry);
  timer_state.active_timer_count++;
  uv_timer_start(&entry->handle, timer_callback, ms, 0);

  return timer_make_object(js, entry->timer_id, delay_ms, 0, callback, timer_args);
}

// setInterval(callback, delay, ...args)
static ant_value_t js_set_interval(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "setInterval requires at least 1 argument (callback)");
  }
  
  ant_value_t callback = args[0];
  double delay_ms = nargs > 1 ? js_getnum(args[1]) : 0;
  uint64_t ms = delay_ms >= 1 ? (uint64_t)delay_ms : 1;
  ant_value_t timer_args = timer_make_args_array(js, args, nargs);
  
  timer_entry_t *entry = ant_calloc(sizeof(timer_entry_t));
  if (entry == NULL) return js_mkerr(js, "failed to allocate timer");
  
  if (timer_copy_args(entry, args, nargs) < 0) {
    free(entry);
    return js_mkerr(js, "failed to allocate timer args");
  }
  
  uv_timer_init(uv_default_loop(), &entry->handle);
  entry->handle.data = entry;
  entry->callback = callback;
  entry->timer_id = timer_state.next_timer_id++;
  entry->active = 1;
  entry->closed = 0;
  entry->is_interval = 1;
  entry->timeout_ms = ms;
  
  add_timer_entry(entry);
  timer_state.active_timer_count++;
  uv_timer_start(&entry->handle, timer_callback, ms, ms);

  return timer_make_object(js, entry->timer_id, delay_ms, 1, callback, timer_args);
}

// clearTimeout(timerId | timerObject)
static ant_value_t js_clear_timeout(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  int timer_id = timer_id_from_arg(js, args[0]);
  
  for (timer_entry_t *entry = timer_state.timers; entry != NULL; entry = entry->next) {
  if (entry->timer_id == timer_id && !entry->closed) {
    if (entry->active) {
      entry->active = 0;
      timer_state.active_timer_count--;
    }
    if (!uv_is_closing((uv_handle_t *)&entry->handle)) uv_close((uv_handle_t *)&entry->handle, timer_close_cb);
    break;
  }}
  
  return js_mkundef();
}

// setImmediate(callback)
static ant_value_t js_set_immediate(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "setImmediate requires 1 argument (callback)");
  }
  
  ant_value_t callback = args[0];
  
  immediate_entry_t *entry = ant_calloc(sizeof(immediate_entry_t));
  if (entry == NULL) {
    return js_mkerr(js, "failed to allocate immediate");
  }
  
  entry->callback = callback;
  entry->immediate_id = timer_state.next_immediate_id++;
  entry->active = 1;
  entry->next = NULL;
  
  if (timer_state.immediates_tail == NULL) {
    timer_state.immediates = entry;
    timer_state.immediates_tail = entry;
  } else {
    timer_state.immediates_tail->next = entry;
    timer_state.immediates_tail = entry;
  }

  ant_value_t obj = js_mkobj(js);
  js_set(js, obj, "id", js_mknum((double)entry->immediate_id));
  js_set(js, obj, "callback", callback);
  
  return obj;
}

// clearImmediate(immediateId | immediateObject)
static ant_value_t js_clear_immediate(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  int immediate_id = timer_id_from_arg(js, args[0]);
  
  for (immediate_entry_t *entry = timer_state.immediates; entry != NULL; entry = entry->next) {
    if (entry->immediate_id == immediate_id) { entry->active = 0; break; }
  }
  
  return js_mkundef();
}

// queueMicrotask(callback)
static ant_value_t js_queue_microtask(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "queueMicrotask requires 1 argument (callback)");
  }
  
  queue_microtask(js, args[0]);
  return js_mkundef();
}

static ant_value_t timers_promises_get_state(ant_t *js) {
  return js_get_slot(js->current_func, SLOT_DATA);
}

static ant_value_t timers_promises_abort_reason(ant_t *js, ant_value_t signal) {
  ant_value_t reason = abort_signal_get_reason(signal);
  if (vtype(reason) != T_UNDEF && vtype(reason) != T_NULL) return reason;
  return js_mkerr_typed(js, JS_ERR_TYPE, "The operation was aborted");
}

static void timers_promises_remove_abort_listener(ant_t *js, ant_value_t state) {
  ant_value_t signal = 0;
  ant_value_t listener = 0;

  if (!is_object_type(state)) return;

  signal = js_get(js, state, "signal");
  listener = js_get(js, state, "abortListener");

  if (abort_signal_is_signal(signal) && is_callable(listener))
    abort_signal_remove_listener(js, signal, listener);

  js_set(js, state, "abortListener", js_mkundef());
}

static void timers_promises_settle(ant_t *js, ant_value_t state, bool reject, ant_value_t value) {
  ant_value_t settled = 0;
  ant_value_t promise = 0;

  if (!is_object_type(state)) return;

  settled = js_get(js, state, "settled");
  if (js_truthy(js, settled)) return;

  js_set(js, state, "settled", js_true);
  timers_promises_remove_abort_listener(js, state);
  js_set(js, state, "handle", js_mkundef());

  promise = js_get(js, state, "promise");
  if (reject) js_reject_promise(js, promise, value);
  else js_resolve_promise(js, promise, value);
}

static ant_value_t timers_promises_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = timers_promises_get_state(js);
  ant_value_t value = js_mkundef();
  if (!is_object_type(state)) return js_mkundef();
  value = js_get(js, state, "value");
  timers_promises_settle(js, state, false, value);
  return js_mkundef();
}

static ant_value_t timers_promises_on_abort(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = timers_promises_get_state(js);
  ant_value_t signal = 0;
  ant_value_t handle = 0;
  ant_value_t is_immediate = 0;
  ant_value_t reason = js_mkundef();
  ant_value_t clear_args[1];

  if (!is_object_type(state)) return js_mkundef();

  signal = js_get(js, state, "signal");
  handle = js_get(js, state, "handle");
  is_immediate = js_get(js, state, "isImmediate");

  if (vtype(handle) != T_UNDEF && vtype(handle) != T_NULL) {
    clear_args[0] = handle;
    if (js_truthy(js, is_immediate)) js_clear_immediate(js, clear_args, 1);
    else js_clear_timeout(js, clear_args, 1);
  }

  if (abort_signal_is_signal(signal)) reason = timers_promises_abort_reason(js, signal);
  else reason = js_mkerr_typed(js, JS_ERR_TYPE, "The operation was aborted");

  timers_promises_settle(js, state, true, reason);
  return js_mkundef();
}

static bool timers_promises_parse_options(
  ant_t *js,
  ant_value_t value,
  ant_value_t *signal_out,
  ant_value_t *error_out
) {
  ant_value_t signal = js_mkundef();

  if (signal_out) *signal_out = js_mkundef();
  if (error_out) *error_out = js_mkundef();

  if (vtype(value) == T_UNDEF || vtype(value) == T_NULL) return true;
  if (vtype(value) != T_OBJ) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Timer options must be an object");
    return false;
  }

  signal = js_get(js, value, "signal");
  if (vtype(signal) != T_UNDEF && vtype(signal) != T_NULL && !abort_signal_is_signal(signal)) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "options.signal must be an AbortSignal");
    return false;
  }

  if (signal_out) *signal_out = signal;
  return true;
}

static ant_value_t timers_promises_schedule(
  ant_t *js,
  double delay_ms,
  ant_value_t value,
  ant_value_t signal,
  bool is_immediate
) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t state = js_mkobj(js);
  ant_value_t callback = 0;
  ant_value_t handle = 0;
  ant_value_t args[2];

  if (abort_signal_is_signal(signal) && abort_signal_is_aborted(signal)) {
    js_reject_promise(js, promise, timers_promises_abort_reason(js, signal));
    return promise;
  }

  js_set(js, state, "promise", promise);
  js_set(js, state, "value", value);
  js_set(js, state, "signal", signal);
  js_set(js, state, "abortListener", js_mkundef());
  js_set(js, state, "handle", js_mkundef());
  js_set(js, state, "settled", js_false);
  js_set(js, state, "isImmediate", js_bool(is_immediate));

  callback = js_heavy_mkfun(js, timers_promises_resolve, state);
  if (is_immediate) handle = js_set_immediate(js, &callback, 1);
  else {
    args[0] = callback;
    args[1] = js_mknum(delay_ms);
    handle = js_set_timeout(js, args, 2);
  }

  if (is_err(handle)) {
    js_reject_promise(js, promise, handle);
    return promise;
  }

  js_set(js, state, "handle", handle);

  if (abort_signal_is_signal(signal)) {
    ant_value_t listener = js_heavy_mkfun(js, timers_promises_on_abort, state);
    js_set(js, state, "abortListener", listener);
    abort_signal_add_listener(js, signal, listener);
  }

  return promise;
}

static ant_value_t js_timers_promises_setTimeout(ant_t *js, ant_value_t *args, int nargs) {
  double delay_ms = nargs > 0 ? js_getnum(args[0]) : 0;
  ant_value_t value = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t options = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t signal = js_mkundef();
  ant_value_t error = js_mkundef();

  if (!timers_promises_parse_options(js, options, &signal, &error)) return error;
  return timers_promises_schedule(js, delay_ms, value, signal, false);
}

static ant_value_t js_timers_promises_setImmediate(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t signal = js_mkundef();
  ant_value_t error = js_mkundef();

  if (!timers_promises_parse_options(js, options, &signal, &error)) return error;
  return timers_promises_schedule(js, 0, value, signal, true);
}

static ant_value_t js_timers_promises_setInterval(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "node:timers/promises setInterval() is not implemented yet");
}

static ant_value_t js_timers_promises_scheduler_wait(ant_t *js, ant_value_t *args, int nargs) {
  return js_timers_promises_setTimeout(js, args, nargs);
}

static ant_value_t js_timers_promises_scheduler_yield(ant_t *js, ant_value_t *args, int nargs) {
  return js_timers_promises_setImmediate(js, args, nargs);
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

void queue_microtask(ant_t *js, ant_value_t callback) {
  microtask_entry_t *entry = ant_calloc(sizeof(microtask_entry_t));
  if (entry == NULL) return;
  
  entry->callback = callback;
  entry->promise = js_mkundef();
  entry->next = NULL;
  entry->argc = 0;
  
  queue_microtask_entry(&timer_state.microtasks, &timer_state.microtasks_tail, entry);
}

void queue_microtask_with_args(ant_t *js, ant_value_t callback, ant_value_t *args, int nargs) {
  if (nargs <= 0) { queue_microtask(js, callback); return; }
  
  microtask_entry_t *entry = ant_calloc(sizeof(microtask_entry_t) + (size_t)nargs * sizeof(ant_value_t));
  if (entry == NULL) return;
  
  entry->callback = callback;
  entry->promise = js_mkundef();
  entry->next = NULL;
  entry->argc = (uint8_t)nargs;
  
  for (int i = 0; i < nargs; i++) entry->argv[i] = args[i];
  queue_microtask_entry(&timer_state.microtasks, &timer_state.microtasks_tail, entry);
}

void queue_next_tick(ant_t *js, ant_value_t callback) {
  microtask_entry_t *entry = ant_calloc(sizeof(microtask_entry_t));
  if (entry == NULL) return;

  entry->callback = callback;
  entry->promise = js_mkundef();
  entry->next = NULL;
  entry->argc = 0;

  queue_microtask_entry(&timer_state.next_ticks, &timer_state.next_ticks_tail, entry);
}

void queue_next_tick_with_args(ant_t *js, ant_value_t callback, ant_value_t *args, int nargs) {
  if (nargs <= 0) { queue_next_tick(js, callback); return; }

  microtask_entry_t *entry = ant_calloc(sizeof(microtask_entry_t) + (size_t)nargs * sizeof(ant_value_t));
  if (entry == NULL) return;

  entry->callback = callback;
  entry->promise = js_mkundef();
  entry->next = NULL;
  entry->argc = (uint8_t)nargs;

  for (int i = 0; i < nargs; i++) entry->argv[i] = args[i];
  queue_microtask_entry(&timer_state.next_ticks, &timer_state.next_ticks_tail, entry);
}

void queue_promise_trigger(ant_t *js, ant_value_t promise) {
  if (!js_mark_promise_trigger_queued(js, promise)) return;

  microtask_entry_t *entry = ant_calloc(sizeof(microtask_entry_t));
  if (entry == NULL) {
    js_mark_promise_trigger_dequeued(js, promise);
    return;
  }
  
  entry->callback = js_mkundef();
  entry->promise = promise;
  entry->next = NULL;
  
  queue_microtask_entry(&timer_state.microtasks, &timer_state.microtasks_tail, entry);
}

static inline void process_microtask_entry(ant_t *js, microtask_entry_t *entry) {
  if (!entry) return;

  if (vtype(entry->promise) == T_PROMISE) {
    GC_ROOT_SAVE(root_mark, js);
    ant_value_t promise = entry->promise;
    
    GC_ROOT_PIN(js, promise);
    js_mark_promise_trigger_dequeued(js, promise);
    js_process_promise_handlers(js, promise);
    GC_ROOT_RESTORE(js, root_mark);
    
    return;
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t callback = entry->callback;
  GC_ROOT_PIN(js, callback);
  
  for (uint8_t i = 0; i < entry->argc; i++) GC_ROOT_PIN(js, entry->argv[i]);
  sv_vm_call(js->vm, js, callback, js_mkundef(), entry->argv, entry->argc, NULL, false);
  GC_ROOT_RESTORE(js, root_mark);
}

static inline microtask_entry_t *take_microtask_batch(void) {
  microtask_entry_t *batch = timer_state.microtasks;

  timer_state.microtasks = NULL;
  timer_state.microtasks_tail = NULL;
  timer_state.microtasks_processing = batch;
  
  return batch;
}

static inline microtask_entry_t *take_next_tick_batch(void) {
  microtask_entry_t *batch = timer_state.next_ticks;

  timer_state.next_ticks = NULL;
  timer_state.next_ticks_tail = NULL;
  timer_state.next_ticks_processing = batch;
  
  return batch;
}

static inline void process_microtask_batch(ant_t *js, microtask_entry_t *batch) {
while (batch != NULL) {
  microtask_entry_t *entry = batch;
  batch = entry->next;
  timer_state.microtasks_processing = batch;
  process_microtask_entry(js, entry);
  free(entry);
}}

static inline void process_next_tick_batch(ant_t *js, microtask_entry_t *batch) {
while (batch != NULL) {
  microtask_entry_t *entry = batch;
  batch = entry->next;
  timer_state.next_ticks_processing = batch;
  process_microtask_entry(js, entry);
  free(entry);
}}

static void process_microtasks_internal(ant_t *js, bool check_unhandled_rejections) {
  microtask_entry_t *batch = NULL;

  if (!js || js->microtasks_draining) return;
  js->microtasks_draining = true;

  while (timer_state.next_ticks != NULL || timer_state.microtasks != NULL) {
  while ((batch = timer_state.next_ticks) != NULL) {
    batch = take_next_tick_batch();
    process_next_tick_batch(js, batch);
  }
  while ((batch = timer_state.microtasks) != NULL) {
    batch = take_microtask_batch();
    process_microtask_batch(js, batch);
  }}

  timer_state.next_ticks_processing = NULL;
  timer_state.microtasks_processing = NULL;
  if (check_unhandled_rejections) js_check_unhandled_rejections(js);
  js->microtasks_draining = false;
  reap_retired_coroutines();
}

void process_microtasks(ant_t *js) {
  process_microtasks_internal(js, true);
}

bool js_maybe_drain_microtasks(ant_t *js) {
  if (!js) return false;
  if (js->microtasks_draining) return false;
  if (js->vm_exec_depth != 0) return false;
  if (!has_pending_microtasks()) return false;
  process_microtasks_internal(js, true);
  return true;
}

bool js_maybe_drain_microtasks_after_async_settle(ant_t *js) {
  if (!js) return false;

  if (js->microtasks_draining) return false;
  if (!has_pending_microtasks()) return false;

  process_microtasks_internal(js, false);
  return true;
}

void process_immediates(ant_t *js) {
while (timer_state.immediates != NULL) {
  immediate_entry_t *entry = timer_state.immediates;
  timer_state.immediates = entry->next;
  
  if (timer_state.immediates == NULL) {
    timer_state.immediates_tail = NULL;
  }
  
  if (entry->active) {
    ant_value_t args[0];
    sv_vm_call(js->vm, js, entry->callback, js_mkundef(), args, 0, NULL, false);
    process_microtasks(js);
  }
  
  free(entry);
}}

int has_pending_immediates(void) {
  for (
    immediate_entry_t *entry = timer_state.immediates;
    entry != NULL; entry = entry->next
  ) if (entry->active) return 1;
  return 0;
}

int has_pending_timers(void) {
  return timer_state.active_timer_count > 0;
}

int has_pending_microtasks(void) {
  return (timer_state.next_ticks != NULL || timer_state.microtasks != NULL) ? 1 : 0;
}

static void timers_define_common(ant_t *js, ant_value_t obj) {
  js_set(js, obj, "setTimeout", js_mkfun_flags(js_set_timeout, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "clearTimeout", js_mkfun_flags(js_clear_timeout, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "setInterval", js_mkfun_flags(js_set_interval, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "clearInterval", js_mkfun_flags(js_clear_timeout, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "setImmediate", js_mkfun_flags(js_set_immediate, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "clearImmediate", js_mkfun_flags(js_clear_immediate, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "queueMicrotask", js_mkfun(js_queue_microtask));
}

void init_timer_module() {  
  ant_t *js = rt->js;
  timer_state.js = js;

  g_timeout_proto = js_mkobj(js);
  g_interval_proto = js_mkobj(js);
  gc_register_root(&g_timeout_proto);
  gc_register_root(&g_interval_proto);

  js_set_proto_init(g_timeout_proto, js->sym.object_proto);
  js_set(js, g_timeout_proto, "ref", js_mkfun(js_timer_ref));
  js_set(js, g_timeout_proto, "unref", js_mkfun(js_timer_unref));
  js_set(js, g_timeout_proto, "hasRef", js_mkfun(js_timer_has_ref));
  js_set(js, g_timeout_proto, "refresh", js_mkfun(js_timer_refresh));
  js_set_sym(js, g_timeout_proto, get_toStringTag_sym(), js_mkstr(js, "Timeout", 7));
  js_set_sym(js, g_timeout_proto, get_inspect_sym(), js_mkfun(timer_inspect));

  js_set_proto_init(g_interval_proto, js->sym.object_proto);
  js_set(js, g_interval_proto, "ref", js_mkfun(js_timer_ref));
  js_set(js, g_interval_proto, "unref", js_mkfun(js_timer_unref));
  js_set(js, g_interval_proto, "hasRef", js_mkfun(js_timer_has_ref));
  js_set(js, g_interval_proto, "refresh", js_mkfun(js_timer_refresh));
  js_set_sym(js, g_interval_proto, get_toStringTag_sym(), js_mkstr(js, "Interval", 8));
  js_set_sym(js, g_interval_proto, get_inspect_sym(), js_mkfun(timer_inspect));

  timers_define_common(js, js_glob(js));
}

ant_value_t timers_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  timers_define_common(js, lib);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "timers", 6));

  return lib;
}

// TODO: mostly stubbed
ant_value_t timers_promises_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t scheduler = js_mkobj(js);

  js_set(js, lib, "scheduler", scheduler);
  js_set(js, lib, "setTimeout", js_mkfun(js_timers_promises_setTimeout));
  js_set(js, lib, "setImmediate", js_mkfun(js_timers_promises_setImmediate));
  js_set(js, lib, "setInterval", js_mkfun(js_timers_promises_setInterval));
  js_set(js, scheduler, "wait", js_mkfun(js_timers_promises_scheduler_wait));
  js_set(js, scheduler, "yield", js_mkfun(js_timers_promises_scheduler_yield));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "timers/promises", 16));

  return lib;
}

void gc_mark_timers(ant_t *js, gc_mark_fn mark) {
  if (is_object_type(g_timeout_proto)) mark(js, g_timeout_proto);
  if (is_object_type(g_interval_proto)) mark(js, g_interval_proto);
  for (timer_entry_t *t = timer_state.timers; t; t = t->next) {
    if (!t->active) continue;
    mark(js, t->callback);
    for (int i = 0; i < t->nargs; i++) mark(js, t->args[i]);
  }
  for (microtask_entry_t *m = timer_state.microtasks; m; m = m->next) {
    mark(js, m->callback);
    mark(js, m->promise);
    for (uint8_t i = 0; i < m->argc; i++) mark(js, m->argv[i]);
  }
  for (microtask_entry_t *m = timer_state.microtasks_processing; m; m = m->next) {
    mark(js, m->callback);
    mark(js, m->promise);
    for (uint8_t i = 0; i < m->argc; i++) mark(js, m->argv[i]);
  }
  for (microtask_entry_t *m = timer_state.next_ticks; m; m = m->next) {
    mark(js, m->callback);
    mark(js, m->promise);
    for (uint8_t i = 0; i < m->argc; i++) mark(js, m->argv[i]);
  }
  for (microtask_entry_t *m = timer_state.next_ticks_processing; m; m = m->next) {
    mark(js, m->callback);
    mark(js, m->promise);
    for (uint8_t i = 0; i < m->argc; i++) mark(js, m->argv[i]);
  }
  for (immediate_entry_t *i = timer_state.immediates; i; i = i->next) {
    mark(js, i->callback);
  }
}
