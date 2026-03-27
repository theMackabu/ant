#include <stdlib.h>
#include <string.h>
#include <utarray.h>
#include <uv.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "descriptors.h"

#include "gc/modules.h"
#include "modules/symbol.h"
#include "modules/timer.h"
#include "modules/abort.h"
#include "modules/domexception.h"
#include "silver/engine.h"

typedef struct {
  ant_value_t callback;
  bool once;
} abort_listener_t;

typedef struct {
  bool aborted;
  bool fired;
  ant_value_t reason;
  UT_array *listeners;
  UT_array *followers;
} abort_signal_data_t;

typedef struct abort_timeout_entry {
  uv_timer_t handle;
  ant_t *js;
  ant_value_t signal;
  int closed;
  struct abort_timeout_entry *next;
} abort_timeout_entry_t;

static const UT_icd abort_listener_icd = { sizeof(abort_listener_t), NULL, NULL, NULL };
static const UT_icd abort_value_icd    = { sizeof(ant_value_t),      NULL, NULL, NULL };

static abort_timeout_entry_t *timeout_entries = NULL;
static ant_value_t g_signal_proto = 0;
static bool g_initialized = false;

static abort_signal_data_t *get_signal_data(ant_value_t obj) {
  ant_value_t slot = js_get_slot(obj, SLOT_DATA);
  if (vtype(slot) != T_NUM) return NULL;
  return (abort_signal_data_t *)(uintptr_t)js_getnum(slot);
}

static ant_value_t make_abort_error(ant_t *js) {
  return make_dom_exception(js, "signal is aborted without reason", "AbortError");
}

static ant_value_t make_timeout_error(ant_t *js) {
  return make_dom_exception(js, "signal timed out", "TimeoutError");
}

static void signal_mark_aborted(ant_t *js, ant_value_t signal_obj, ant_value_t reason) {
  abort_signal_data_t *data = get_signal_data(signal_obj);
  if (!data || data->aborted) return;
  
  data->aborted = true;
  data->fired = true;
  data->reason = reason;
  
  js_set(js, signal_obj, "aborted", js_true);
  js_set(js, signal_obj, "reason",  reason);
}

void signal_do_abort(ant_t *js, ant_value_t signal_obj, ant_value_t reason) {
  abort_signal_data_t *data = get_signal_data(signal_obj);
  if (!data || data->aborted) return;

  UT_array *queue;    utarray_new(queue,   &abort_value_icd);
  UT_array *to_fire;  utarray_new(to_fire, &abort_value_icd);

  utarray_push_back(queue, &signal_obj);

  for (unsigned int qi = 0; qi < utarray_len(queue); qi++) {
  ant_value_t *cur = (ant_value_t *)utarray_eltptr(queue, qi);
  abort_signal_data_t *d = get_signal_data(*cur);
  if (!d || d->aborted) continue;

  d->aborted = true;
  d->fired   = true;
  d->reason  = reason;
  js_set(js, *cur, "aborted", js_true);
  js_set(js, *cur, "reason",  reason);
  utarray_push_back(to_fire, cur);

  unsigned int nf = utarray_len(d->followers);
  for (unsigned int i = 0; i < nf; i++) {
    ant_value_t *sig = (ant_value_t *)utarray_eltptr(d->followers, i);
    utarray_push_back(queue, sig);
  }}
  utarray_free(queue);

  for (unsigned int qi = 0; qi < utarray_len(to_fire); qi++) {
  ant_value_t *cur = (ant_value_t *)utarray_eltptr(to_fire, qi);
  abort_signal_data_t *d = get_signal_data(*cur);
  if (!d) continue;

  ant_value_t event_obj = js_mkobj(js);
  js_set(js, event_obj, "type",   js_mkstr(js, "abort", 5));
  js_set(js, event_obj, "target", *cur);
  ant_value_t call_args[1] = { event_obj };

  ant_value_t onabort = js_get(js, *cur, "onabort");
  if (is_callable(onabort)) {
    sv_vm_call(js->vm, js, onabort, *cur, call_args, 1, NULL, false);
    process_microtasks(js);
  }

  unsigned int n = utarray_len(d->listeners);
  for (unsigned int i = 0; i < n;) {
    abort_listener_t *entry = (abort_listener_t *)utarray_eltptr(d->listeners, i);
    ant_value_t cb = entry->callback;
    bool once = entry->once;
    
    if (once) { utarray_erase(d->listeners, i, 1); n--; } else i++;
    if (!is_callable(cb)) continue;
    
    sv_vm_call(js->vm, js, cb, *cur, call_args, 1, NULL, false);
    process_microtasks(js);
  }}
  
  utarray_free(to_fire);
}

void abort_signal_remove_listener(ant_t *js, ant_value_t signal, ant_value_t callback) {
  abort_signal_data_t *data = get_signal_data(signal);
  if (!data) return;

  unsigned int n = utarray_len(data->listeners);
  for (unsigned int i = 0; i < n; i++) {
    abort_listener_t *entry = (abort_listener_t *)utarray_eltptr(data->listeners, i);
    if (entry->callback != callback) continue;
    utarray_erase(data->listeners, i, 1);
    return;
  }
}

static ant_value_t make_new_signal(ant_t *js) {
  abort_signal_data_t *data = ant_calloc(sizeof(abort_signal_data_t));
  if (!data) return js_mkerr(js, "AbortSignal: out of memory");

  data->aborted = false;
  data->fired = false;
  data->reason = js_mkundef();
  utarray_new(data->listeners, &abort_listener_icd);
  utarray_new(data->followers, &abort_value_icd);

  ant_value_t obj = js_mkobj(js);
  js_set_slot(obj, SLOT_DATA, ANT_PTR(data));
  if (g_initialized) js_set_slot_wb(js, obj, SLOT_PROTO, g_signal_proto);

  js_set(js, obj, "aborted", js_false);
  js_set(js, obj, "reason", js_mkundef());
  js_set(js, obj, "onabort", js_mkundef());
  js_set_sym(js, obj, get_toStringTag_sym(), js_mkstr(js, "AbortSignal", 11));

  return obj;
}

// signal.addEventListener(type, listener, options?)
static ant_value_t abort_signal_add_event_listener(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();

  const char *type = js_getstr(js, args[0], NULL);
  if (!type || strcmp(type, "abort") != 0) return js_mkundef();
  if (!is_callable(args[1])) return js_mkundef();

  abort_signal_data_t *data = get_signal_data(js_getthis(js));
  if (!data) return js_mkundef();
  if (data->aborted) return js_mkundef();

  bool once = false;
  if (nargs >= 3 && vtype(args[2]) == T_OBJ) {
    ant_value_t once_val = js_get(js, args[2], "once");
    if (vtype(once_val) != T_UNDEF) once = js_truthy(js, once_val);
  } else if (nargs >= 3 && vtype(args[2]) == T_BOOL) once = js_truthy(js, args[2]);

  unsigned int n = utarray_len(data->listeners);
  for (unsigned int i = 0; i < n; i++) {
    abort_listener_t *e = (abort_listener_t *)utarray_eltptr(data->listeners, i);
    if (e->callback == args[1] && e->once == once) return js_mkundef();
  }

  abort_listener_t entry = { args[1], once };
  utarray_push_back(data->listeners, &entry);
  
  return js_mkundef();
}

// signal.removeEventListener(type, listener)
static ant_value_t abort_signal_remove_event_listener(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkundef();

  const char *type = js_getstr(js, args[0], NULL);
  if (!type || strcmp(type, "abort") != 0) return js_mkundef();

  abort_signal_data_t *data = get_signal_data(js_getthis(js));
  if (!data) return js_mkundef();

  unsigned int n = utarray_len(data->listeners);
  for (unsigned int i = 0; i < n; i++) {
    abort_listener_t *e = (abort_listener_t *)utarray_eltptr(data->listeners, i);
    if (e->callback != args[1]) continue;
    utarray_erase(data->listeners, i, 1);
    return js_mkundef();
  }
  
  return js_mkundef();
}

// signal.dispatchEvent(event)
static ant_value_t abort_signal_dispatch_event(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;

  const char *type = NULL;
  if (vtype(args[0]) == T_OBJ) type = js_getstr(js, js_get(js, args[0], "type"), NULL);
  else type = js_getstr(js, args[0], NULL);

  if (!type || strcmp(type, "abort") != 0) return js_true;

  abort_signal_data_t *data = get_signal_data(js_getthis(js));
  if (!data || data->fired) return js_true;

  signal_do_abort(js, js_getthis(js), data->reason);
  return js_true;
}

// signal.throwIfAborted()
static ant_value_t abort_signal_throw_if_aborted(ant_t *js, ant_value_t *args, int nargs) {
  abort_signal_data_t *data = get_signal_data(js_getthis(js));
  if (!data || !data->aborted) return js_mkundef();
  return js_throw(js, data->reason);
}

// AbortSignal.abort(reason?)
static ant_value_t abort_signal_static_abort(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t reason = (nargs >= 1 && vtype(args[0]) != T_UNDEF)
    ? args[0]
    : make_abort_error(js);

  ant_value_t signal = make_new_signal(js);
  if (is_err(signal)) return signal;

  signal_mark_aborted(js, signal, reason);
  return signal;
}

// AbortSignal.any(signals)
static ant_value_t abort_signal_static_any(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_ARR)
    return js_mkerr(js, "AbortSignal.any: argument must be an array of AbortSignal objects");

  ant_value_t composite = make_new_signal(js);
  if (is_err(composite)) return composite;
  ant_offset_t len = js_arr_len(js, args[0]);

  for (ant_offset_t i = 0; i < len; i++) {
  ant_value_t sig = js_arr_get(js, args[0], i);
  abort_signal_data_t *d = get_signal_data(sig);
  if (d && d->aborted) {
    signal_mark_aborted(js, composite, d->reason);
    return composite;
  }}

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t sig = js_arr_get(js, args[0], i);
    abort_signal_data_t *d = get_signal_data(sig);
    if (!d) continue;
    utarray_push_back(d->followers, &composite);
  }

  return composite;
}

ant_value_t abort_signal_create_dependent(ant_t *js, ant_value_t source) {
  ant_value_t composite = make_new_signal(js);
  
  if (is_err(composite)) return composite;
  if (vtype(source) != T_OBJ && vtype(source) != T_ARR) return composite;

  abort_signal_data_t *d = get_signal_data(source);
  if (!d) return composite;

  if (d->aborted) signal_mark_aborted(js, composite, d->reason);
  else utarray_push_back(d->followers, &composite);

  return composite;
}

static void abort_timeout_close_cb(uv_handle_t *h) {
  abort_timeout_entry_t *entry = (abort_timeout_entry_t *)h->data;
  if (entry) entry->closed = 1;
}

static void abort_timeout_fire_cb(uv_timer_t *handle) {
  abort_timeout_entry_t *entry = (abort_timeout_entry_t *)handle->data;
  if (!entry || entry->closed) return;

  ant_t *js = entry->js;
  signal_do_abort(js, entry->signal, make_timeout_error(js));
  process_microtasks(js);

  if (!uv_is_closing((uv_handle_t *)handle))
    uv_close((uv_handle_t *)handle, abort_timeout_close_cb);
}

// AbortSignal.timeout(milliseconds)
static ant_value_t abort_signal_static_timeout(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "AbortSignal.timeout: milliseconds argument required");

  double ms = js_getnum(args[0]);
  if (ms < 0) return js_mkerr(js, "AbortSignal.timeout: milliseconds must be non-negative");

  ant_value_t signal = make_new_signal(js);
  if (is_err(signal)) return signal;

  abort_timeout_entry_t *entry = ant_calloc(sizeof(abort_timeout_entry_t));
  if (!entry) return js_mkerr(js, "AbortSignal.timeout: out of memory");

  entry->js = js;
  entry->signal = signal;
  entry->closed = 0;
  entry->next = timeout_entries;
  timeout_entries = entry;

  uv_timer_init(uv_default_loop(), &entry->handle);
  entry->handle.data = entry;
  uv_timer_start(&entry->handle, abort_timeout_fire_cb, (uint64_t)(ms > 0 ? ms : 0), 0);

  return signal;
}

// new AbortController()
static ant_value_t abort_controller_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);

  ant_value_t signal = make_new_signal(js);
  if (is_err(signal)) return signal;

  js_mkprop_fast(js, this_obj, "signal", 6, signal);
  js_set_descriptor(js, this_obj, "signal", 6, 0);
  js_set_sym(js, this_obj, get_toStringTag_sym(), js_mkstr(js, "AbortController", 15));

  return js_mkundef();
}

// controller.abort(reason?)
static ant_value_t abort_controller_abort(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t signal = js_get(js, js_getthis(js), "signal");

  abort_signal_data_t *data = get_signal_data(signal);
  if (!data || data->aborted) return js_mkundef();

  ant_value_t reason = (nargs >= 1 && vtype(args[0]) != T_UNDEF)
    ? args[0]
    : make_abort_error(js);

  signal_do_abort(js, signal, reason);
  return js_mkundef();
}

void init_abort_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);

  ant_value_t signal_proto = js_mkobj(js);
  g_signal_proto = signal_proto;
  g_initialized = true;

  js_set(js, signal_proto, "addEventListener",    js_mkfun(abort_signal_add_event_listener));
  js_set(js, signal_proto, "removeEventListener", js_mkfun(abort_signal_remove_event_listener));
  js_set(js, signal_proto, "dispatchEvent",       js_mkfun(abort_signal_dispatch_event));
  js_set(js, signal_proto, "throwIfAborted",      js_mkfun(abort_signal_throw_if_aborted));
  js_set_sym(js, signal_proto, get_toStringTag_sym(), js_mkstr(js, "AbortSignal", 11));

  ant_value_t signal_ctor = js_mkobj(js);
  js_mkprop_fast(js, signal_ctor, "prototype", 9, signal_proto);
  js_mkprop_fast(js, signal_ctor, "name", 4, ANT_STRING("AbortSignal"));
  js_set_descriptor(js, signal_ctor, "name", 4, 0);

  ant_value_t signal_fn = js_obj_to_func_ex(signal_ctor, SV_CALL_IS_DEFAULT_CTOR);
  js_set(js, signal_fn, "abort",   js_mkfun(abort_signal_static_abort));
  js_set(js, signal_fn, "timeout", js_mkfun(abort_signal_static_timeout));
  js_set(js, signal_fn, "any",     js_mkfun(abort_signal_static_any));

  js_set(js, signal_proto, "constructor", signal_fn);
  js_set_descriptor(js, signal_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  ant_value_t ctrl_proto = js_mkobj(js);
  js_set(js, ctrl_proto, "abort", js_mkfun(abort_controller_abort));
  js_set_sym(js, ctrl_proto, get_toStringTag_sym(), js_mkstr(js, "AbortController", 15));

  ant_value_t ctrl_ctor = js_mkobj(js);
  js_set_slot(ctrl_ctor, SLOT_CFUNC, js_mkfun(abort_controller_ctor));
  js_mkprop_fast(js, ctrl_ctor, "prototype", 9, ctrl_proto);
  js_mkprop_fast(js, ctrl_ctor, "name", 4, ANT_STRING("AbortController"));
  js_set_descriptor(js, ctrl_ctor, "name", 4, 0);

  ant_value_t ctrl_fn = js_obj_to_func(ctrl_ctor);
  js_set(js, ctrl_proto, "constructor", ctrl_fn);
  js_set_descriptor(js, ctrl_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "AbortController", ctrl_fn);
  js_set(js, global, "AbortSignal",     signal_fn);
}

void gc_mark_abort(ant_t *js, gc_mark_fn mark) {
  if (g_initialized) mark(js, g_signal_proto);
  for (abort_timeout_entry_t *e = timeout_entries; e; e = e->next)
    if (!e->closed) mark(js, e->signal);
}

bool abort_signal_is_aborted(ant_value_t signal) {
  abort_signal_data_t *data = get_signal_data(signal);
  return data && data->aborted;
}

bool abort_signal_is_signal(ant_value_t signal) {
  return get_signal_data(signal) != NULL;
}

ant_value_t abort_signal_get_reason(ant_value_t signal) {
  abort_signal_data_t *data = get_signal_data(signal);
  return data ? data->reason : js_mkundef();
}

void abort_signal_add_listener(ant_t *js, ant_value_t signal, ant_value_t callback) {
  abort_signal_data_t *data = get_signal_data(signal);
  if (!data) return;

  if (data->aborted) {
    ant_value_t event_obj = js_mkobj(js);
    js_set(js, event_obj, "type", js_mkstr(js, "abort", 5));
    js_set(js, event_obj, "target", signal);
    ant_value_t call_args[1] = { event_obj };
    sv_vm_call(js->vm, js, callback, signal, call_args, 1, NULL, false);
    return;
  }

  abort_listener_t entry = { callback, false };
  utarray_push_back(data->listeners, &entry);
}
