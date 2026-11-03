#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>

#include "errors.h"
#include "loop.h"
#include "cfunc.h"
#include "builder.h"
#include "ptr.h"
#include "internal.h"
#include "silver/call.h"

#include "gc/uv.h"
#include "gc/roots.h"
#include "gc/modules.h"

#include "modules/abort.h"
#include "modules/timer.h"
#include "modules/microtasks.h"
#include "modules/process.h"
#include "modules/iterator.h"

typedef struct timer_entry {
  struct timer_entry *next;
  ant_value_t obj;
  ant_value_t callback;
  ant_value_t *args;
  int nargs;
  int active;
  bool young;
  struct timer_entry *prev;
  int timer_id;
  int closed;
  int is_interval;
  bool handle_closed;
  bool detached;
  uint64_t timeout_ms;
  uv_timer_t handle;
} timer_entry_t;

typedef struct immediate_entry {
  ant_value_t callback;
  int immediate_id;
  int active;
  bool refed;
  ant_value_t handle;
  struct immediate_entry *next;
  int argc;
  ant_value_t argv[];
} immediate_entry_t;

static void add_timer_entry(ant_t *js, timer_entry_t *entry) {
  entry->young = true;
  entry->next = js->timers.timers;
  entry->prev = NULL;
  if (js->timers.timers) js->timers.timers->prev = entry;
  js->timers.timers = entry;
}

static void remove_timer_entry(ant_t *js, timer_entry_t *entry) {
  if (entry->prev) entry->prev->next = entry->next;
  else js->timers.timers = entry->next;
  if (entry->next) entry->next->prev = entry->prev;
  entry->next = NULL;
  entry->prev = NULL;
}

static void timer_entry_hold_young(ant_t *js, timer_entry_t *entry) {
  if (entry->young) return;
  remove_timer_entry(js, entry);
  add_timer_entry(js, entry);
}

static timer_entry_t *find_timer_entry_by_id(ant_t *js, int timer_id) {
  for (timer_entry_t *entry = js->timers.timers; entry != NULL; entry = entry->next)
    if (entry->timer_id == timer_id) return entry;
  return NULL;
}

static int timer_copy_args(timer_entry_t *entry, ant_args_t) {
  entry->nargs = nargs > 2 ? nargs - 2 : 0;
  if (entry->nargs > 0) {
    entry->args = calloc(1, sizeof(ant_value_t) * entry->nargs);
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
  entry->callback = Ant_Value_Undefined;
  timer_release_args(entry);
}

static int timer_copy_args_from_object(ant_t *js, timer_entry_t *entry, ant_value_t obj) {
  ant_value_t args_arr = js_get_slot(obj, SLOT_AUX);
  ant_offset_t len = vtype(args_arr) == kTypeArray ? js_arr_len(js, args_arr) : 0;

  timer_release_args(entry);
  if (len == 0) return 0;

  entry->args = calloc(1, sizeof(ant_value_t) * (size_t)len);
  if (!entry->args) return -1;
  entry->nargs = (int)len;
  for (ant_offset_t i = 0; i < len; i++) entry->args[i] = js_arr_get(js, args_arr, i);
  return 0;
}

static ant_value_t timer_make_args_array(ant_native_params_t) {
  ant_value_t arr = Ant_Value_Undefined;
  int arg_count = nargs > 2 ? nargs - 2 : 0;

  if (arg_count <= 0) return arr;
  arr = Ant_Array_Create(js);
  for (int i = 0; i < arg_count; i++) js_arr_push(js, arr, args[i + 2]);
  return arr;
}

static constexpr uint32_t TIMER_NATIVE_TAG = Ant_NativeTag("TIMR");
static constexpr uint32_t IMMEDIATE_NATIVE_TAG = Ant_NativeTag("IMMD");

static inline timer_entry_t *timer_entry_of(ant_value_t handle) {
  return js_get_native(handle, TIMER_NATIVE_TAG);
}

static timer_entry_t *timer_open_entry_of(ant_value_t handle) {
  timer_entry_t *entry = timer_entry_of(handle);
  return entry && !entry->closed ? entry : NULL;
}

static ant_value_t timer_to_primitive(ant_params_t) {
  timer_entry_t *entry = timer_entry_of(call_this);
  return entry ? Ant_Value_FromNumber((double)entry->timer_id) : Ant_Value_Undefined;
}

static inline immediate_entry_t *immediate_entry_of(ant_value_t handle) {
  return js_get_native(handle, IMMEDIATE_NATIVE_TAG);
}

static ant_value_t timer_inspect(ant_params_t) {
  timer_entry_t *entry = timer_entry_of(call_this);
  immediate_entry_t *immediate = immediate_entry_of(call_this);
  
  ant_value_t refed = Ant_Value_Null;
  bool destroyed = true;
  
  if (entry) {
    refed = Ant_Value_FromBool(uv_has_ref((const uv_handle_t *)&entry->handle));
    destroyed = entry->closed || !entry->active;
  } else if (immediate && immediate->active) {
    refed = Ant_Value_FromBool(immediate->refed);
    destroyed = false;
  }

  ant_value_t tag_val = js_get_sym(js, call_this, js->sym.toStringTag_sym);
  const char *tag = vtype(tag_val) == kTypeString ? js_getstr(js, tag_val, NULL) : "Timeout";

  js_inspect_builder_t builder;
  if (!js_inspect_builder_init_dynamic(&builder, js, 128))
    return Ant_Error_CreateOutOfMemory(js);

  bool ok = entry
    ? js_inspect_header(&builder, "%s (%d)", tag, entry->timer_id)
    : js_inspect_header(&builder, "%s", tag);
  
  if (ok) ok = js_inspect_object_body(&builder, call_this);
  if (ok) ok = js_inspect_field(&builder, "Symbol(refed)", refed);
  if (ok) ok = js_inspect_field(&builder, "Symbol(destroyed)", Ant_Value_FromBool(destroyed));
  if (ok) ok = js_inspect_close(&builder);
  
  if (!ok) {
    js_inspect_builder_dispose(&builder);
    return Ant_Error_CreateOutOfMemory(js);
  }

  return js_inspect_builder_result(&builder);
}

static ant_value_t js_timer_ref(ant_params_t) {
  timer_entry_t *entry = timer_open_entry_of(call_this);
  
  if (entry) {
    int was_refed = uv_has_ref((const uv_handle_t *)&entry->handle);
    uv_ref((uv_handle_t *)&entry->handle);
    if (entry->active && !was_refed) js->timers.active_refed_timer_count++;
  }
  
  return call_this;
}

static void timer_entry_unref(ant_t *js, timer_entry_t *entry) {
  int was_refed = uv_has_ref((const uv_handle_t *)&entry->handle);
  uv_unref((uv_handle_t *)&entry->handle);
  if (entry->active && was_refed) js->timers.active_refed_timer_count--;
}

static ant_value_t js_timer_unref(ant_params_t) {
  timer_entry_t *entry = timer_open_entry_of(call_this);
  if (entry) timer_entry_unref(js, entry);
  return call_this;
}

static ant_value_t js_timer_has_ref(ant_params_t) {
  timer_entry_t *entry = timer_open_entry_of(call_this);
  if (!entry) return Ant_Value_False;
  return Ant_Value_FromBool(uv_has_ref((const uv_handle_t *)&entry->handle) != 0);
}

static void immediate_detach_handle(immediate_entry_t *entry) {
  if (is_object_type(entry->handle)) js_clear_native(entry->handle, IMMEDIATE_NATIVE_TAG);
  entry->handle = Ant_Value_Undefined;
}

static ant_value_t js_immediate_ref(ant_params_t) {
  immediate_entry_t *entry = immediate_entry_of(call_this);
  if (entry) entry->refed = true;
  return call_this;
}

static ant_value_t js_immediate_unref(ant_params_t) {
  immediate_entry_t *entry = immediate_entry_of(call_this);
  if (entry) entry->refed = false;
  return call_this;
}

static ant_value_t js_immediate_has_ref(ant_params_t) {
  immediate_entry_t *entry = immediate_entry_of(call_this);
  return Ant_Value_FromBool(entry && entry->active && entry->refed);
}

static ant_value_t js_immediate_dispose(ant_params_t) {
  immediate_entry_t *entry = immediate_entry_of(call_this);
  if (entry) entry->active = 0;
  return Ant_Value_Undefined;
}

static ant_value_t immediate_make_object(ant_t *js, immediate_entry_t *entry) {
  ant_value_t obj = js_mkobj_from_template(js, js->builtins.immediate_template);
  if (is_err(obj)) return obj;
  
  ant_object_t *ptr = js_obj_ptr(obj);
  ant_object_prop_set_unchecked(ptr, 0, entry->callback);
  gc_write_barrier(js, ptr, entry->callback);
  js_set_native(obj, entry, IMMEDIATE_NATIVE_TAG);
  entry->handle = obj;
  
  return obj;
}

// uv_close completes on a later loop turn, possibly after the isolate is gone,
// so the entry leaves its isolate's list when it is closed, not here
static void timer_close_cb(uv_handle_t *h) {
  timer_entry_t *entry = (timer_entry_t *)h->data;
  if (!entry) return;
  timer_release_callback_args(entry);
  entry->handle_closed = true;
  if (entry->detached) free(entry);
}

static void timer_close_entry(ant_t *js, timer_entry_t *entry) {
  if (!entry || entry->closed) return;
  
  if (entry->active) {
    entry->active = 0;
    js->timers.active_timer_count--;
    if (uv_has_ref((const uv_handle_t *)&entry->handle))
      js->timers.active_refed_timer_count--;
  }
  
  remove_timer_entry(js, entry);
  entry->closed = 1;
  entry->obj = Ant_Value_Undefined;
  if (!uv_is_closing((uv_handle_t *)&entry->handle))
    uv_close((uv_handle_t *)&entry->handle, timer_close_cb);
}

// the entry outlives its uv handle while the timer object is alive, and is
// freed by whichever of uv_close and the object's finalizer finishes last
static void timer_object_finalize(ant_t *js, ant_object_t *obj) {
  timer_entry_t *entry = timer_entry_of(js_obj_from_ptr(obj));
  if (!entry) return;
  
  entry->detached = true;
  if (!entry->closed) timer_close_entry(js, entry);
  else if (entry->handle_closed) free(entry);
}

static ant_value_t timer_make_object(ant_t *js, timer_entry_t *entry, double delay_ms, ant_value_t timer_args) {
  ant_value_t obj = js_mkobj_from_template(js, entry->is_interval 
    ? js->builtins.interval_template 
    : js->builtins.timeout_template
  );
  
  if (is_err(obj)) {
    entry->detached = true;
    timer_close_entry(js, entry);
    return obj;
  }

  ant_object_t *ptr = js_obj_ptr(obj);
  ant_value_t delay = Ant_Value_FromNumber(delay_ms);
  
  ant_object_prop_set_unchecked(ptr, 0, delay);
  ant_object_prop_set_unchecked(ptr, 1, entry->is_interval ? delay : Ant_Value_Null);
  ant_object_prop_set_unchecked(ptr, 2, entry->callback);
  gc_write_barrier(js, ptr, entry->callback);
  
  if (vtype(timer_args) != kTypeUndefined) js_set_slot_wb(js, obj, SLOT_AUX, timer_args);
  js_set_native(obj, entry, TIMER_NATIVE_TAG);
  js_set_finalizer(obj, timer_object_finalize);
  entry->obj = obj;
  timer_entry_hold_young(js, entry);

  return obj;
}

static void timer_callback(uv_timer_t *handle) {
  GC_UV_CALLBACK();
  timer_entry_t *entry = (timer_entry_t *)handle->data;
  if (!entry || entry->closed || !entry->active) return;
  
  ant_t *js = Ant_Loop_Isolate(handle->loop);
  ant_value_t callback = entry->callback;
  
  if (!entry->is_interval) {
    entry->active = 0;
    js->timers.active_timer_count--;
    if (uv_has_ref((const uv_handle_t *)&entry->handle))
      js->timers.active_refed_timer_count--;
  }

  ant_value_t self = entry->obj;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, self);
  
  for (int i = 0; i < entry->nargs; i++) GC_ROOT_PIN(js, entry->args[i]);
  sv_vm_call(js->vm, js, callback, self, entry->args, entry->nargs, NULL, Ant_Value_Undefined);
  GC_ROOT_RESTORE(js, root_mark);
  
  if (!entry->is_interval && !entry->active) timer_release_callback_args(entry);
  process_report_uncaught_exception_if_pending(js);
  process_microtasks(js);
}

static void timer_start(ant_t *js, timer_entry_t *entry, uint64_t timeout, uint64_t repeat) {
  uv_update_time(js->loop);
  uv_timer_start(&entry->handle, timer_callback, timeout, repeat);
}

static ant_value_t js_timer_refresh(ant_params_t) {
  
  timer_entry_t *entry = timer_open_entry_of(call_this);
  if (!entry) return call_this;

  if (!entry->active) {
    if (vtype(entry->callback) == kTypeUndefined) {
      entry->callback = js_get(js, call_this, "callback");
      if (!is_callable(entry->callback)) return call_this;
      if (timer_copy_args_from_object(js, entry, call_this) != 0)
        return js_mkerr(js, "failed to allocate timer args");
      timer_entry_hold_young(js, entry);
    }
    
    entry->active = 1;
    js->timers.active_timer_count++;
    
    if (uv_has_ref((const uv_handle_t *)&entry->handle)) js->timers.active_refed_timer_count++;
  }

  timer_start(js, entry, entry->timeout_ms, entry->is_interval ? entry->timeout_ms : 0);
  return call_this;
}

// setTimeout(callback, delay, ...args)
static ant_value_t js_set_timeout(ant_params_t) {
  if (nargs < 1)
    return js_mkerr(js, "setTimeout requires at least 1 argument (callback)");
  
  ant_value_t callback = args[0];
  double delay_ms = nargs > 1 ? Ant_Value_AsNumberUnchecked(args[1]) : 0;
  uint64_t ms = delay_ms >= 1 ? (uint64_t)delay_ms : 0;
  ant_value_t timer_args = timer_make_args_array(js, args, nargs);
  
  timer_entry_t *entry = calloc(1, sizeof(timer_entry_t));
  if (entry == NULL) return js_mkerr(js, "failed to allocate timer");
  
  if (timer_copy_args(entry, args, nargs) < 0) {
    free(entry);
    return js_mkerr(js, "failed to allocate timer args");
  }
  
  uv_timer_init(js->loop, &entry->handle);
  entry->handle.data = entry;
  entry->callback = callback;
  entry->timer_id = js->timers.next_timer_id++;
  entry->active = 1;
  entry->closed = 0;
  entry->is_interval = 0;
  entry->timeout_ms = ms;
  
  add_timer_entry(js, entry);
  js->timers.active_timer_count++;
  js->timers.active_refed_timer_count++;
  timer_start(js, entry, ms, 0);

  return timer_make_object(js, entry, delay_ms, timer_args);
}

// setInterval(callback, delay, ...args)
static ant_value_t js_set_interval(ant_params_t) {
  if (nargs < 1)
    return js_mkerr(js, "setInterval requires at least 1 argument (callback)");
  
  ant_value_t callback = args[0];
  double delay_ms = nargs > 1 ? Ant_Value_AsNumberUnchecked(args[1]) : 0;
  uint64_t ms = delay_ms >= 1 ? (uint64_t)delay_ms : 1;
  ant_value_t timer_args = timer_make_args_array(js, args, nargs);
  
  timer_entry_t *entry = calloc(1, sizeof(timer_entry_t));
  if (entry == NULL) return js_mkerr(js, "failed to allocate timer");
  
  if (timer_copy_args(entry, args, nargs) < 0) {
    free(entry);
    return js_mkerr(js, "failed to allocate timer args");
  }
  
  uv_timer_init(js->loop, &entry->handle);
  entry->handle.data = entry;
  entry->callback = callback;
  entry->timer_id = js->timers.next_timer_id++;
  entry->active = 1;
  entry->closed = 0;
  entry->is_interval = 1;
  entry->timeout_ms = ms;
  
  add_timer_entry(js, entry);
  js->timers.active_timer_count++;
  js->timers.active_refed_timer_count++;
  timer_start(js, entry, ms, ms);

  return timer_make_object(js, entry, delay_ms, timer_args);
}

static ant_value_t js_timer_dispose(ant_params_t) {
  timer_entry_t *entry = timer_open_entry_of(call_this);
  if (entry) timer_close_entry(js, entry);
  return Ant_Value_Undefined;
}

// clearTimeout(timerId | timerObject)
static ant_value_t js_clear_timeout(ant_params_t) {
  if (nargs < 1) return Ant_Value_Undefined;
  
  timer_entry_t *entry = timer_open_entry_of(args[0]);
  if (!entry && vtype(args[0]) == kTypeNumber) entry = find_timer_entry_by_id(js, (int)Ant_Value_AsNumberUnchecked(args[0]));
  if (entry) timer_close_entry(js, entry);
  
  return Ant_Value_Undefined;
}

// setImmediate(callback, ...args)
static ant_value_t js_set_immediate(ant_params_t) {
  if (nargs < 1) {
    return js_mkerr(js, "setImmediate requires 1 argument (callback)");
  }
  
  ant_value_t callback = args[0];
  int argc = nargs - 1;
  
  immediate_entry_t *entry = calloc(1, sizeof(immediate_entry_t) + (size_t)argc * sizeof(ant_value_t));
  if (entry == NULL) {
    return js_mkerr(js, "failed to allocate immediate");
  }
  
  entry->callback = callback;
  entry->argc = argc;
  for (int i = 0; i < argc; i++) entry->argv[i] = args[i + 1];
  entry->immediate_id = js->timers.next_immediate_id++;
  entry->active = 1;
  entry->refed = true;
  entry->handle = Ant_Value_Undefined;
  entry->next = NULL;
  
  if (js->timers.immediates_tail == NULL) {
    js->timers.immediates = entry;
    js->timers.immediates_tail = entry;
  } else {
    js->timers.immediates_tail->next = entry;
    js->timers.immediates_tail = entry;
  }

  return immediate_make_object(js, entry);
}

// clearImmediate(immediateId | immediateObject)
static ant_value_t js_clear_immediate(ant_params_t) {
  if (nargs < 1) return Ant_Value_Undefined;
  
  immediate_entry_t *entry = immediate_entry_of(args[0]);
  if (entry) entry->active = 0;
  else if (vtype(args[0]) == kTypeNumber) {
    int immediate_id = (int)Ant_Value_AsNumberUnchecked(args[0]);
    immediate_entry_t *lists[2] = { js->timers.immediates_processing, js->timers.immediates };
    for (int l = 0; l < 2 && !entry; l++)
      for (immediate_entry_t *it = lists[l]; it; it = it->next)
        if (it->immediate_id == immediate_id) { entry = it; break; }
    if (entry) entry->active = 0;
  }
  
  return Ant_Value_Undefined;
}

static void timers_promises_remove_abort_listener(ant_t *js, ant_value_t state) {
  ant_value_t signal = 0;
  ant_value_t listener = 0;

  if (!is_object_type(state)) return;

  signal = js_get(js, state, "signal");
  listener = js_get(js, state, "abortListener");

  if (abort_signal_is_signal(signal) && is_callable(listener))
    abort_signal_remove_listener(js, signal, listener);

  js_set(js, state, "abortListener", Ant_Value_Undefined);
}

static void timers_promises_settle(ant_t *js, ant_value_t state, bool reject, ant_value_t value) {
  ant_value_t settled = 0;
  ant_value_t promise = 0;

  if (!is_object_type(state)) return;

  settled = js_get(js, state, "settled");
  if (js_truthy(js, settled)) return;

  js_set(js, state, "settled", Ant_Value_True);
  timers_promises_remove_abort_listener(js, state);
  js_set(js, state, "handle", Ant_Value_Undefined);

  promise = js_get(js, state, "promise");
  if (reject) js_reject_promise(js, promise, value);
  else js_resolve_promise(js, promise, value);
}

static ant_value_t timers_promises_resolve(ant_params_t) {
  ant_value_t state = js_get_slot(call_func, SLOT_DATA);
  ant_value_t value = Ant_Value_Undefined;
  if (!is_object_type(state)) return Ant_Value_Undefined;
  value = js_get(js, state, "value");
  timers_promises_settle(js, state, false, value);
  return Ant_Value_Undefined;
}

static ant_value_t timers_promises_on_abort(ant_params_t) {
  ant_value_t state = js_get_slot(call_func, SLOT_DATA);
  ant_value_t signal = 0;
  ant_value_t handle = 0;
  ant_value_t is_immediate = 0;
  ant_value_t reason = Ant_Value_Undefined;
  ant_value_t clear_args[1];

  if (!is_object_type(state)) return Ant_Value_Undefined;

  signal = js_get(js, state, "signal");
  handle = js_get(js, state, "handle");
  is_immediate = js_get(js, state, "isImmediate");

  if (vtype(handle) != kTypeUndefined && vtype(handle) != kTypeNull) {
    clear_args[0] = handle;
    if (js_truthy(js, is_immediate)) js_clear_immediate(js, clear_args, 1, Ant_Value_Undefined, call_this, call_func);
    else js_clear_timeout(js, clear_args, 1, Ant_Value_Undefined, call_this, call_func);
  }

  if (abort_signal_is_signal(signal)) reason = abort_signal_make_error(js, signal);
  else reason = Ant_Error_Create(js, JS_ERR_TYPE, "The operation was aborted");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, reason);

  timers_promises_settle(js, state, true, reason);
  GC_ROOT_RESTORE(js, root_mark);

  return Ant_Value_Undefined;
}

static ant_value_t timers_invalid_arg(ant_t *js, const char *message) {
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", message);
}

static ant_value_t timers_promises_parse_options(
  ant_t *js, ant_value_t value, ant_value_t *signal_out, ant_value_t *ref_out
) {
  if (vtype(value) == kTypeUndefined || vtype(value) == kTypeNull) return Ant_Value_Undefined;
  
  if (vtype(value) != kTypeObject)
    return timers_invalid_arg(js, "The \"options\" argument must be of type object");

  ant_value_t signal = js_get(js, value, "signal");
  if (is_err(signal)) return signal;
  
  if (vtype(signal) != kTypeUndefined && vtype(signal) != kTypeNull && !abort_signal_is_signal(signal))
    return timers_invalid_arg(js, "The \"options.signal\" property must be an instance of AbortSignal");
  
  *signal_out = signal;
  if (!ref_out) return Ant_Value_Undefined;
  
  ant_value_t ref = js_get(js, value, "ref");
  if (is_err(ref)) return ref;
  if (vtype(ref) == kTypeUndefined) return Ant_Value_Undefined;
  
  if (vtype(ref) != kTypeBool)
    return timers_invalid_arg(js, "The \"options.ref\" property must be of type boolean");
    
  *ref_out = ref;
  return Ant_Value_Undefined;
}

static ant_value_t timers_promises_schedule(
  ant_t *js, double delay_ms, ant_value_t value, ant_value_t signal, bool is_immediate
) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t state = Ant_Object_CreateBare(js);
  
  ant_value_t callback = 0;
  ant_value_t handle = 0;
  ant_value_t args[2];

  if (abort_signal_is_signal(signal) && abort_signal_is_aborted(signal)) {
    js_reject_promise(js, promise, abort_signal_make_error(js, signal));
    return promise;
  }

  js_set(js, state, "promise", promise);
  js_set(js, state, "value", value);
  js_set(js, state, "signal", signal);
  js_set(js, state, "abortListener", Ant_Value_Undefined);
  js_set(js, state, "handle", Ant_Value_Undefined);
  js_set(js, state, "settled", Ant_Value_False);
  js_set(js, state, "isImmediate", Ant_Value_FromBool(is_immediate));

  callback = js_heavy_mkfun(js, timers_promises_resolve, state);
  if (is_immediate) handle = js_set_immediate(js, &callback, 1, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  else {
    args[0] = callback;
    args[1] = Ant_Value_FromNumber(delay_ms);
    handle = js_set_timeout(js, args, 2, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
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

static ant_value_t js_timers_promises_setTimeout(ant_params_t) {
  double delay_ms = nargs > 0 ? Ant_Value_AsNumberUnchecked(args[0]) : 0;
  ant_value_t value = nargs > 1 ? args[1] : Ant_Value_Undefined;
  ant_value_t options = nargs > 2 ? args[2] : Ant_Value_Undefined;
  
  ant_value_t signal = Ant_Value_Undefined;
  ant_value_t error = timers_promises_parse_options(js, options, &signal, NULL);
  if (is_err(error)) return error;
  
  return timers_promises_schedule(js, delay_ms, value, signal, false);
}

static ant_value_t js_timers_promises_setImmediate(ant_params_t) {
  ant_value_t value = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t options = nargs > 1 ? args[1] : Ant_Value_Undefined;
  
  ant_value_t signal = Ant_Value_Undefined;
  ant_value_t error = timers_promises_parse_options(js, options, &signal, NULL);
  if (is_err(error)) return error;
  
  return timers_promises_schedule(js, 0, value, signal, true);
}

static ant_value_t timers_interval_shift_parked(ant_t *js, ant_value_t state) {
  ant_value_t parked = js_get(js, state, "parked");
  ant_offset_t head = (ant_offset_t)Ant_Value_AsNumberUnchecked(js_get(js, state, "parkedHead"));
  
  ant_offset_t len = js_arr_len(js, parked);
  if (head >= len) return Ant_Value_Undefined;

  ant_value_t promise = js_arr_get(js, parked, head);
  if (head + 1 == len) {
    js_set(js, state, "parked", Ant_Array_Create(js));
    js_set(js, state, "parkedHead", Ant_Value_FromNumber(0));
  } else js_set(js, state, "parkedHead", Ant_Value_FromNumber((double)(head + 1)));

  return promise;
}

static void timers_interval_stop(ant_t *js, ant_value_t state) {
  ant_value_t handle = js_get(js, state, "handle");
  if (vtype(handle) != kTypeUndefined) js_clear_timeout(js, &handle, 1, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  js_set(js, state, "handle", Ant_Value_Undefined);
  timers_promises_remove_abort_listener(js, state);
  js_set(js, state, "finished", Ant_Value_True);
}

static void timers_interval_finish_parked(ant_t *js, ant_value_t state) {
  for (;;) {
    ant_value_t parked = timers_interval_shift_parked(js, state);
    if (vtype(parked) != kTypePromise) return;
    js_resolve_promise(js, parked, Ant_Iterator_CreateResult(js, Ant_Value_Undefined, true));
  }
}

static ant_value_t timers_interval_tick(ant_params_t) {
  ant_value_t state = js_get_slot(call_func, SLOT_DATA);
  if (!is_object_type(state) || js_truthy(js, js_get(js, state, "finished"))) return Ant_Value_Undefined;

  ant_value_t parked = timers_interval_shift_parked(js, state);
  if (vtype(parked) == kTypePromise)
    js_resolve_promise(js, parked, Ant_Iterator_CreateResult(js, js_get(js, state, "value"), false));
  else js_set(js, state, "notYielded", Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(js_get(js, state, "notYielded")) + 1));

  return Ant_Value_Undefined;
}

static ant_value_t timers_interval_on_abort(ant_params_t) {
  ant_value_t state = js_get_slot(call_func, SLOT_DATA);
  if (!is_object_type(state) || js_truthy(js, js_get(js, state, "finished"))) return Ant_Value_Undefined;

  ant_value_t reason = abort_signal_make_error(js, js_get(js, state, "signal"));
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, reason);

  timers_interval_stop(js, state);
  ant_value_t parked = timers_interval_shift_parked(js, state);
  if (vtype(parked) == kTypePromise) {
    js_reject_promise(js, parked, reason);
    timers_interval_finish_parked(js, state);
  } else js_set(js, state, "storedError", reason);

  GC_ROOT_RESTORE(js, root_mark);
  return Ant_Value_Undefined;
}

static ant_value_t timers_interval_start(ant_t *js, ant_value_t state) {
  ant_value_t signal = js_get(js, state, "signal");
  if (abort_signal_is_signal(signal) && abort_signal_is_aborted(signal)) {
    js_set(js, state, "finished", Ant_Value_True);
    return abort_signal_make_error(js, signal);
  }

  ant_value_t args[2] = {
    js_heavy_mkfun(js, timers_interval_tick, state),
    js_get(js, state, "delay"),
  };
  
  ant_value_t handle = js_set_interval(js, args, 2, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  if (is_err(handle)) {
    js_set(js, state, "finished", Ant_Value_True);
    return js_take_thrown(js, handle);
  }

  js_set(js, state, "handle", handle);
  if (!js_truthy(js, js_get(js, state, "ref"))) timer_entry_unref(js, timer_open_entry_of(handle));

  if (abort_signal_is_signal(signal)) {
    ant_value_t listener = js_heavy_mkfun(js, timers_interval_on_abort, state);
    js_set(js, state, "abortListener", listener);
    abort_signal_add_listener(js, signal, listener);
  }

  return Ant_Value_Undefined;
}

static ant_value_t timers_interval_next(ant_params_t) {
  ant_value_t state = js_get_slot(call_func, SLOT_DATA);
  ant_value_t promise = js_mkpromise(js);
  if (!is_object_type(state)) return promise;

  if (!js_truthy(js, js_get(js, state, "started"))) {
    js_set(js, state, "started", Ant_Value_True);
    if (!js_truthy(js, js_get(js, state, "finished"))) {
      ant_value_t error = timers_interval_start(js, state);
      if (!is_undefined(error)) js_set(js, state, "storedError", error);
    }
  }

  double not_yielded = Ant_Value_AsNumberUnchecked(js_get(js, state, "notYielded"));
  if (not_yielded > 0) {
    js_set(js, state, "notYielded", Ant_Value_FromNumber(not_yielded - 1));
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, js_get(js, state, "value"), false));
    return promise;
  }

  ant_value_t stored = js_get(js, state, "storedError");
  if (!is_undefined(stored)) {
    js_set(js, state, "storedError", Ant_Value_Undefined);
    js_reject_promise(js, promise, stored);
    return promise;
  }

  if (js_truthy(js, js_get(js, state, "finished"))) {
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, Ant_Value_Undefined, true));
    return promise;
  }

  js_arr_push(js, js_get(js, state, "parked"), promise);
  return promise;
}

static ant_value_t timers_interval_finish(ant_t *js, ant_value_t state) {
  if (!is_object_type(state)) return Ant_Value_Undefined;
  js_set(js, state, "started", Ant_Value_True);
  js_set(js, state, "storedError", Ant_Value_Undefined);
  js_set(js, state, "notYielded", Ant_Value_FromNumber(0));
  
  timers_interval_stop(js, state);
  timers_interval_finish_parked(js, state);
  
  return Ant_Value_Undefined;
}

static ant_value_t timers_interval_return(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  timers_interval_finish(js, js_get_slot(call_func, SLOT_DATA));
  js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, nargs > 0 ? args[0] : Ant_Value_Undefined, true));
  return promise;
}

static ant_value_t timers_interval_throw(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  timers_interval_finish(js, js_get_slot(call_func, SLOT_DATA));
  js_reject_promise(js, promise, nargs > 0 ? args[0] : Ant_Value_Undefined);
  return promise;
}

static ant_value_t js_timers_promises_setInterval(ant_params_t) {
  ant_value_t options = nargs > 2 ? args[2] : Ant_Value_Undefined;
  ant_value_t signal = Ant_Value_Undefined;
  ant_value_t ref = Ant_Value_True;
  ant_value_t error = timers_promises_parse_options(js, options, &signal, &ref);
  if (is_err(error)) error = js_take_thrown(js, error);

  ant_value_t state = Ant_Object_CreateBare(js);
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, state);

  js_set(js, state, "delay", Ant_Value_FromNumber(nargs > 0 ? Ant_Value_AsNumberUnchecked(args[0]) : 1));
  js_set(js, state, "value", nargs > 1 ? args[1] : Ant_Value_Undefined);
  js_set(js, state, "signal", signal);
  js_set(js, state, "ref", ref);
  js_set(js, state, "abortListener", Ant_Value_Undefined);
  js_set(js, state, "handle", Ant_Value_Undefined);
  js_set(js, state, "notYielded", Ant_Value_FromNumber(0));
  js_set(js, state, "parked", Ant_Array_Create(js));
  js_set(js, state, "parkedHead", Ant_Value_FromNumber(0));
  js_set(js, state, "started", Ant_Value_False);
  js_set(js, state, "finished", Ant_Value_FromBool(!is_undefined(error)));
  js_set(js, state, "storedError", error);

  ant_value_t iter = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, iter);
  js_set_proto_init(iter, js->sym.async_iterator_proto);
  js_set(js, iter, "next", js_heavy_mkfun(js, timers_interval_next, state));
  js_set(js, iter, "return", js_heavy_mkfun(js, timers_interval_return, state));
  js_set(js, iter, "throw", js_heavy_mkfun(js, timers_interval_throw, state));

  GC_ROOT_RESTORE(js, root_mark);
  return iter;
}

static ant_value_t js_timers_promises_scheduler_wait(ant_params_t) {
  return js_timers_promises_setTimeout(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
}

static ant_value_t js_timers_promises_scheduler_yield(ant_params_t) {
  return js_timers_promises_setImmediate(js, args, nargs, Ant_Value_Undefined, call_this, call_func);
}

void process_immediates(ant_t *js) {
  ant_timer_state_t *t = &js->timers;
  if (t->immediates_processing) return;
  
  t->immediates_processing = t->immediates;
  t->immediates = t->immediates_tail = NULL;
  
  while (t->immediates_processing) {
    immediate_entry_t *entry = t->immediates_processing;
    t->immediates_processing = entry->next;
    ant_value_t self = entry->handle;
    immediate_detach_handle(entry);
    
    if (entry->active) {
      GC_ROOT_SAVE(root_mark, js);
      GC_ROOT_PIN(js, entry->callback);
      GC_ROOT_PIN(js, self);
      for (int i = 0; i < entry->argc; i++) GC_ROOT_PIN(js, entry->argv[i]);
      sv_vm_call(js->vm, js, entry->callback, self, entry->argv, entry->argc, NULL, Ant_Value_Undefined);
      GC_ROOT_RESTORE(js, root_mark);
      process_report_uncaught_exception_if_pending(js);
      process_microtasks(js);
    }
    
    free(entry);
  }
}

int has_pending_immediates(ant_t *js) {
  for (
    immediate_entry_t *entry = js->timers.immediates;
    entry != NULL; entry = entry->next
  ) if (entry->active && entry->refed) return 1;
  return 0;
}

int has_active_immediates(ant_t *js) {
  for (
    immediate_entry_t *entry = js->timers.immediates;
    entry != NULL; entry = entry->next
  ) if (entry->active) return 1;
  return 0;
}

int has_pending_timers(ant_t *js) {
  return js->timers.active_refed_timer_count > 0;
}

static void timers_define_common(ant_t *js, ant_value_t obj) {
  js_set(js, obj, "setTimeout", Ant_Function_CreateEx(js_set_timeout, 2, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "clearTimeout", Ant_Function_CreateEx(js_clear_timeout, 1, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "setInterval", Ant_Function_CreateEx(js_set_interval, 2, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "clearInterval", Ant_Function_CreateEx(js_clear_timeout, 1, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "setImmediate", Ant_Function_CreateEx(js_set_immediate, 1, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "clearImmediate", Ant_Function_CreateEx(js_clear_immediate, 1, CFUNC_HAS_PROTOTYPE));
  js_set(js, obj, "queueMicrotask", Ant_Function_CreateWithArity(js_queue_microtask, 1));
}

static ant_value_t timer_make_template(ant_t *js, ant_value_t proto, ant_value_t repeat) {
  ant_value_t template = Ant_Object_CreateBare(js);
  js_set_proto_init(template, proto);
  js_set(js, template, "delay", Ant_Value_FromNumber(0));
  js_set(js, template, "repeat", repeat);
  js_set(js, template, "callback", Ant_Value_Undefined);
  js_set_descriptor(js, template, "callback", 8, JS_DESC_W | JS_DESC_C);
  return template;
}

void init_timer_module(ant_t *js) {
  js->timers.next_timer_id = 1;
  js->timers.next_immediate_id = 1;

  js->builtins.timeout_proto = Ant_Object_CreateBare(js);
  js->builtins.interval_proto = Ant_Object_CreateBare(js);

  js_set_proto_init(js->builtins.timeout_proto, js->sym.object_proto);
  js_set(js, js->builtins.timeout_proto, "ref", Ant_Function_Create(js_timer_ref));
  js_set(js, js->builtins.timeout_proto, "unref", Ant_Function_Create(js_timer_unref));
  js_set(js, js->builtins.timeout_proto, "hasRef", Ant_Function_Create(js_timer_has_ref));
  js_set(js, js->builtins.timeout_proto, "refresh", Ant_Function_Create(js_timer_refresh));
  js_set_sym(js, js->builtins.timeout_proto, js->sym.dispose_sym, Ant_Function_Create(js_timer_dispose));
  js_set_tostringtag(js, js->builtins.timeout_proto, js_mkstr(js, "Timeout", 7));
  js_set_sym(js, js->builtins.timeout_proto, js->sym.inspect_sym, Ant_Function_Create(timer_inspect));
  js_set_sym(js, js->builtins.timeout_proto, js->sym.toPrimitive_sym, Ant_Function_Create(timer_to_primitive));

  js->builtins.immediate_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.immediate_proto, js->sym.object_proto);
  js_set(js, js->builtins.immediate_proto, "ref", Ant_Function_Create(js_immediate_ref));
  js_set(js, js->builtins.immediate_proto, "unref", Ant_Function_Create(js_immediate_unref));
  js_set(js, js->builtins.immediate_proto, "hasRef", Ant_Function_Create(js_immediate_has_ref));
  js_set_sym(js, js->builtins.immediate_proto, js->sym.dispose_sym, Ant_Function_Create(js_immediate_dispose));
  js_set_tostringtag(js, js->builtins.immediate_proto, js_mkstr(js, "Immediate", 9));
  js_set_sym(js, js->builtins.immediate_proto, js->sym.inspect_sym, Ant_Function_Create(timer_inspect));

  js->builtins.immediate_template = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.immediate_template, js->builtins.immediate_proto);
  js_set(js, js->builtins.immediate_template, "callback", Ant_Value_Undefined);
  js_set_descriptor(js, js->builtins.immediate_template, "callback", 8, JS_DESC_W | JS_DESC_C);

  js_set_proto_init(js->builtins.interval_proto, js->sym.object_proto);
  js_set(js, js->builtins.interval_proto, "ref", Ant_Function_Create(js_timer_ref));
  js_set(js, js->builtins.interval_proto, "unref", Ant_Function_Create(js_timer_unref));
  js_set(js, js->builtins.interval_proto, "hasRef", Ant_Function_Create(js_timer_has_ref));
  js_set(js, js->builtins.interval_proto, "refresh", Ant_Function_Create(js_timer_refresh));
  js_set_sym(js, js->builtins.interval_proto, js->sym.dispose_sym, Ant_Function_Create(js_timer_dispose));
  js_set_tostringtag(js, js->builtins.interval_proto, js_mkstr(js, "Interval", 8));
  js_set_sym(js, js->builtins.interval_proto, js->sym.inspect_sym, Ant_Function_Create(timer_inspect));
  js_set_sym(js, js->builtins.interval_proto, js->sym.toPrimitive_sym, Ant_Function_Create(timer_to_primitive));
  
  js->builtins.timeout_template = timer_make_template(js, js->builtins.timeout_proto, Ant_Value_Null);
  js->builtins.interval_template = timer_make_template(js, js->builtins.interval_proto, Ant_Value_FromNumber(0));

  timers_define_common(js, js_glob(js));
}

ant_value_t timers_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);

  timers_define_common(js, lib);
  js_set_tostringtag(js, lib, js_mkstr(js, "timers", 6));

  return lib;
}

// TODO: mostly stubbed
ant_value_t timers_promises_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  ant_value_t scheduler = Ant_Object_CreateBare(js);

  js_set(js, lib, "scheduler", scheduler);
  js_set(js, lib, "setTimeout", Ant_Function_CreateWithArity(js_timers_promises_setTimeout, 2));
  js_set(js, lib, "setImmediate", Ant_Function_CreateWithArity(js_timers_promises_setImmediate, 1));
  js_set(js, lib, "setInterval", Ant_Function_CreateWithArity(js_timers_promises_setInterval, 2));
  js_set(js, scheduler, "wait", Ant_Function_Create(js_timers_promises_scheduler_wait));
  js_set(js, scheduler, "yield", Ant_Function_Create(js_timers_promises_scheduler_yield));
  js_set_tostringtag(js, lib, js_mkstr(js, "timers/promises", 16));

  return lib;
}

void cleanup_timer_module(ant_t *js) {
  ant_timer_state_t *t = &js->timers;
  
  Ant_Assert(
    !t->microtasks_processing && !t->next_ticks_processing && !t->immediates_processing,
    "cannot destroy an isolate while it runs a job batch"
  );

  while (t->timers) timer_close_entry(js, t->timers);
  ant_microtasks_discard(js);

  for (immediate_entry_t *entry = t->immediates, *next; entry; entry = next) {
    next = entry->next;
    free(entry);
  }
  
  t->immediates = t->immediates_tail = NULL;
}


void gc_mark_timers(ant_t *js, gc_mark_fn mark) {
  bool minor = js->gc.minor;
  for (timer_entry_t *t = js->timers.timers; t; t = t->next) {
    if (minor && !t->young) break;
    if (minor) t->young = false;
    if (!t->active) continue;
    if (is_object_type(t->obj)) mark(js, t->obj);
    mark(js, t->callback);
    for (int i = 0; i < t->nargs; i++) mark(js, t->args[i]);
  }
  
  immediate_entry_t *immediate_lists[2] = { js->timers.immediates, js->timers.immediates_processing };
  for (int l = 0; l < 2; l++) for (immediate_entry_t *i = immediate_lists[l]; i; i = i->next) {
    mark(js, i->callback);
    mark(js, i->handle);
    for (int a = 0; a < i->argc; a++) mark(js, i->argv[a]);
  }
}
