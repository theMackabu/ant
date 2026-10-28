#include <stdlib.h>
#include <string.h>
#include <utarray.h>
#include <uv.h>

#include "ant.h"
#include "cfunc.h"
#include "list.h"
#include "loop.h"
#include "common.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "native_getter.h"

#include "gc.h"
#include "gc/modules.h"
#include "modules/timer.h"
#include "modules/abort.h"
#include "modules/events.h"
#include "modules/domexception.h"
#include "silver/call.h"

typedef struct {
  ant_value_t aborted;
  ant_value_t reason;
  UT_array *followers;
} abort_signal_data_t;

struct abort_timeout_entry {
  uv_timer_t handle;
  ant_value_t signal;
  abort_timeout_entry_t *next;
  abort_timeout_entry_t **pprev;
};

static const UT_icd abort_value_icd = { 
  sizeof(ant_value_t), 
  NULL, NULL, NULL 
};

static constexpr uint32_t ABORT_SIGNAL_NATIVE_TAG = Ant_NativeTag("ASIG");
static constexpr uint32_t ABORT_CONTROLLER_NATIVE_TAG = Ant_NativeTag("ACTR");

static inline abort_signal_data_t *get_signal_data(ant_value_t obj) {
  return js_get_native(obj, ABORT_SIGNAL_NATIVE_TAG);
}

static inline bool signal_aborted(const abort_signal_data_t *data) {
  return data->aborted == js_true;
}

static void abort_signal_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  abort_signal_data_t *data = get_signal_data(value);
  js_clear_native(value, ABORT_SIGNAL_NATIVE_TAG);
  if (!data) return;
  if (data->followers) utarray_free(data->followers);
  free(data);
}

static ant_value_t make_abort_error(ant_t *js) {
  return make_dom_exception(js, "signal is aborted without reason", "AbortError");
}

static ant_value_t make_timeout_error(ant_t *js) {
  return make_dom_exception(js, "signal timed out", "TimeoutError");
}

static ant_value_t make_new_signal(ant_t *js) {
  abort_signal_data_t *data = malloc(sizeof(*data));
  if (!data) return js_mkerr(js, "AbortSignal: out of memory");
  *data = (abort_signal_data_t){ .aborted = js_false, .reason = js_mkundef() };

  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_native(obj, data, ABORT_SIGNAL_NATIVE_TAG);
  js_set_finalizer(obj, abort_signal_finalize);
  js_set_proto_init(obj, js->builtins.signal_proto);
  return obj;
}

static void signal_mark_aborted(ant_t *js, ant_value_t signal, abort_signal_data_t *data, ant_value_t reason) {
  data->aborted = js_true;
  data->reason = reason;
  gc_write_barrier(js, js_obj_ptr(signal), reason);
}

static void signal_add_follower(ant_t *js, ant_value_t source, abort_signal_data_t *data, ant_value_t follower) {
  if (!data->followers) utarray_new(data->followers, &abort_value_icd);
  utarray_push_back(data->followers, &follower);
  gc_write_barrier(js, js_obj_ptr(source), follower);
}

static void signal_fire(ant_t *js, ant_value_t signal, abort_signal_data_t *data) {
  ant_value_t event = Ant_EventTarget_CreateEvent(js, signal, js->builtins.event_proto, ANT_EVENT_BASE, "abort");
  if (is_object_type(event)) Ant_EventTarget_Dispatch(js, signal, event);
}

void signal_do_abort(ant_t *js, ant_value_t signal, ant_value_t reason) {
  abort_signal_data_t *data = get_signal_data(signal);
  if (!data || signal_aborted(data)) return;
  signal_mark_aborted(js, signal, data, reason);

  if (!data->followers) {
    signal_fire(js, signal, data);
    return;
  }

  UT_array *queue = NULL;
  utarray_new(queue, &abort_value_icd);
  utarray_push_back(queue, &signal);

  for (unsigned int qi = 0; qi < utarray_len(queue); qi++) {
    abort_signal_data_t *d = get_signal_data(*(ant_value_t *)utarray_eltptr(queue, qi));
    unsigned int nf = d && d->followers ? utarray_len(d->followers) : 0;
    for (unsigned int i = 0; i < nf; i++) {
      ant_value_t follower = *(ant_value_t *)utarray_eltptr(d->followers, i);
      abort_signal_data_t *fd = get_signal_data(follower);
      if (!fd || signal_aborted(fd)) continue;
      signal_mark_aborted(js, follower, fd, reason);
      utarray_push_back(queue, &follower);
    }
  }

  for (unsigned int qi = 0; qi < utarray_len(queue); qi++) {
    ant_value_t cur = *(ant_value_t *)utarray_eltptr(queue, qi);
    abort_signal_data_t *d = get_signal_data(cur);
    if (d) signal_fire(js, cur, d);
  }

  utarray_free(queue);
}

static __attribute__((cold, noinline)) ant_value_t abort_this_error(ant_t *js, const char *type) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Value of \"this\" must be of type %s", type);
}

static ant_value_t abort_signal_get_aborted(ant_params_t) {
  abort_signal_data_t *data = get_signal_data(js_getthis(js));
  return data ? data->aborted : abort_this_error(js, "AbortSignal");
}

static ant_value_t abort_signal_get_reason_fn(ant_params_t) {
  abort_signal_data_t *data = get_signal_data(js_getthis(js));
  return data ? data->reason : abort_this_error(js, "AbortSignal");
}

static ant_value_t abort_signal_invalid_this(ant_t *js, ant_value_t this_obj) {
  return get_signal_data(this_obj) ? 0 : abort_this_error(js, "AbortSignal");
}

ANT_EVENT_HANDLER_ACCESSORS(abort_signal, abort, abort_signal_invalid_this)

static ant_value_t abort_signal_inspect(ant_params_t) {
  abort_signal_data_t *data = get_signal_data(js_getthis(js));
  return data && signal_aborted(data) ? ANT_STRING("AbortSignal { aborted: true }") : ANT_STRING("AbortSignal { aborted: false }");
}

static ant_value_t abort_signal_throw_if_aborted(ant_params_t) {
  abort_signal_data_t *data = get_signal_data(js_getthis(js));
  if (!data) return abort_this_error(js, "AbortSignal");
  return signal_aborted(data) ? js_throw(js, data->reason) : js_mkundef();
}

// AbortSignal.abort(reason?)
static ant_value_t abort_signal_static_abort(ant_params_t) {
  ant_value_t reason = (nargs >= 1 && vtype(args[0]) != kTypeUndefined)
    ? args[0]
    : make_abort_error(js);

  ant_value_t signal = make_new_signal(js);
  if (is_err(signal)) return signal;

  signal_mark_aborted(js, signal, get_signal_data(signal), reason);
  return signal;
}

// AbortSignal.any(signals)
static ant_value_t abort_signal_static_any(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeArray)
    return js_mkerr(js, "AbortSignal.any: argument must be an array of AbortSignal objects");

  ant_value_t composite = make_new_signal(js);
  if (is_err(composite)) return composite;
  abort_signal_data_t *cd = get_signal_data(composite);
  ant_offset_t len = js_arr_len(js, args[0]);

  for (ant_offset_t i = 0; i < len; i++) {
    abort_signal_data_t *d = get_signal_data(js_arr_get(js, args[0], i));
    if (d && signal_aborted(d)) {
      signal_mark_aborted(js, composite, cd, d->reason);
      return composite;
    }
  }

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t sig = js_arr_get(js, args[0], i);
    abort_signal_data_t *d = get_signal_data(sig);
    if (d) signal_add_follower(js, sig, d, composite);
  }

  return composite;
}

ant_value_t abort_signal_create_dependent(ant_t *js, ant_value_t source) {
  ant_value_t composite = make_new_signal(js);
  if (is_err(composite)) return composite;

  abort_signal_data_t *d = get_signal_data(source);
  if (!d) return composite;

  if (signal_aborted(d)) signal_mark_aborted(js, composite, get_signal_data(composite), d->reason);
  else signal_add_follower(js, source, d, composite);

  return composite;
}

static void abort_timeout_close_cb(uv_handle_t *h) {
  abort_timeout_entry_t *entry = (abort_timeout_entry_t *)h->data;
  if (!entry) return;

  Ant_List_Remove(entry);
  free(entry);
}

static void abort_timeout_fire_cb(uv_timer_t *handle) {
  abort_timeout_entry_t *entry = (abort_timeout_entry_t *)handle->data;
  if (!entry || uv_is_closing((uv_handle_t *)handle)) return;

  ant_t *js = Ant_Loop_Isolate(handle->loop);
  signal_do_abort(js, entry->signal, make_timeout_error(js));
  process_microtasks(js);

  if (!uv_is_closing((uv_handle_t *)handle))
    uv_close((uv_handle_t *)handle, abort_timeout_close_cb);
}

// AbortSignal.timeout(milliseconds)
static ant_value_t abort_signal_static_timeout(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "AbortSignal.timeout: milliseconds argument required");

  double ms = js_getnum(args[0]);
  if (ms < 0) return js_mkerr(js, "AbortSignal.timeout: milliseconds must be non-negative");

  ant_value_t signal = make_new_signal(js);
  if (is_err(signal)) return signal;

  abort_timeout_entry_t *entry = calloc(1, sizeof(abort_timeout_entry_t));
  if (!entry) return js_mkerr(js, "AbortSignal.timeout: out of memory");

  entry->signal = signal;
  Ant_List_Insert(&js->abort_timeouts, entry);
  uv_timer_init(js->loop, &entry->handle);
  
  entry->handle.data = entry;
  uv_timer_start(&entry->handle, abort_timeout_fire_cb, (uint64_t)(ms > 0 ? ms : 0), 0);
  uv_unref((uv_handle_t *)&entry->handle);

  return signal;
}

static ant_value_t abort_signal_ctor(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal constructor");
}

// new AbortController()
static ant_value_t abort_controller_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "AbortController constructor requires 'new'");
  ant_value_t this_obj = js_getthis(js);

  ant_value_t signal = make_new_signal(js);
  if (is_err(signal)) return signal;

  js_set_native(this_obj, get_signal_data(signal), ABORT_CONTROLLER_NATIVE_TAG);
  js_set_slot_wb(js, this_obj, SLOT_DATA, signal);
  return js_mkundef();
}

static ant_value_t abort_controller_get_signal(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj) || !js_check_native_tag(this_obj, ABORT_CONTROLLER_NATIVE_TAG))
    return abort_this_error(js, "AbortController");
  return js_get_slot(this_obj, SLOT_DATA);
}

// controller.abort(reason?)
static ant_value_t abort_controller_abort(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  abort_signal_data_t *data = js_get_native(this_obj, ABORT_CONTROLLER_NATIVE_TAG);
  
  if (!data) return abort_this_error(js, "AbortController");
  if (signal_aborted(data)) return js_mkundef();

  ant_value_t reason = (nargs >= 1 && vtype(args[0]) != kTypeUndefined)
    ? args[0]
    : make_abort_error(js);

  signal_do_abort(js, js_get_slot(this_obj, SLOT_DATA), reason);
  return js_mkundef();
}

static ant_value_t abort_controller_inspect(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  abort_signal_data_t *data = js_get_native(this_obj, ABORT_CONTROLLER_NATIVE_TAG);
  return data && signal_aborted(data)
    ? ANT_STRING("AbortController { signal: AbortSignal { aborted: true } }")
    : ANT_STRING("AbortController { signal: AbortSignal { aborted: false } }");
}

#define ABORT_FIELD_GETTER(name, fn, field)                                              \
  static const ant_native_getter_t name = {                                              \
    .meta = { fn, NULL, 0, CFUNC_NATIVE_GETTER }, .native_tag = ABORT_SIGNAL_NATIVE_TAG, \
    .kind = ANT_NATIVE_GETTER_FIELD, .field_offset = offsetof(abort_signal_data_t, field) }

ABORT_FIELD_GETTER(signal_aborted_getter, abort_signal_get_aborted, aborted);
ABORT_FIELD_GETTER(signal_reason_getter, abort_signal_get_reason_fn, reason);

static const ant_native_getter_t controller_signal_getter = {
  .meta = { abort_controller_get_signal, NULL, 0, CFUNC_NATIVE_GETTER },
  .native_tag = ABORT_CONTROLLER_NATIVE_TAG, .kind = ANT_NATIVE_GETTER_SLOT_DATA,
};

void init_abort_module(ant_t *js) {
  ant_value_t signal_proto = Ant_Object_CreateBare(js);
  js->builtins.signal_proto = signal_proto;
  js_set_proto_init(signal_proto, js->builtins.eventtarget_proto);

  js_set_getter_desc(js, signal_proto, "aborted", 7, js_mkfun_meta(&signal_aborted_getter.meta), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, signal_proto, "reason", 6, js_mkfun_meta(&signal_reason_getter.meta), JS_DESC_E | JS_DESC_C);
  ANT_EVENT_HANDLER_DEFINE(signal_proto, abort_signal, abort);
  js_set(js, signal_proto, "throwIfAborted", Ant_Function_Create(abort_signal_throw_if_aborted));
  defmethod_sym(js, signal_proto, js->sym.inspect_sym, Ant_Function_Create(abort_signal_inspect));
  js_set_tostringtag(js, signal_proto, js_mkstr(js, "AbortSignal", 11));

  ant_value_t signal_fn = Ant_Function_CreateIntrinsicConstructor(js, abort_signal_ctor, signal_proto, "AbortSignal", 0);
  static const ant_cfunc_meta_t signal_statics[] = {
    { abort_signal_static_abort,   "abort",   0, 0 },
    { abort_signal_static_timeout, "timeout", 1, 0 },
    { abort_signal_static_any,     "any",     1, 0 },
  };
  js_define_methods(js, signal_fn, signal_statics, countof(signal_statics), ANT_PROP_ATTR_METHOD);
  js_set_proto(js, signal_fn, js->builtins.eventtarget_ctor);

  ant_value_t ctrl_proto = Ant_Object_CreateBare(js);
  js_set_getter_desc(js, ctrl_proto, "signal", 6, js_mkfun_meta(&controller_signal_getter.meta), JS_DESC_E | JS_DESC_C);
  js_set(js, ctrl_proto, "abort", Ant_Function_Create(abort_controller_abort));
  defmethod_sym(js, ctrl_proto, js->sym.inspect_sym, Ant_Function_Create(abort_controller_inspect));
  js_set_tostringtag(js, ctrl_proto, js_mkstr(js, "AbortController", 15));

  ant_value_t ctrl_fn = Ant_Function_CreateIntrinsicConstructor(js, abort_controller_ctor, ctrl_proto, "AbortController", 0);

  js_set_global_builtin(js, "AbortController", ctrl_fn);
  js_set_global_builtin(js, "AbortSignal", signal_fn);
}

void cleanup_abort_module(ant_t *js) {
  for (abort_timeout_entry_t *entry = js->abort_timeouts, *next; entry; entry = next) {
    next = entry->next;
    if (uv_is_closing((uv_handle_t *)&entry->handle)) continue;
    uv_timer_stop(&entry->handle);
    uv_close((uv_handle_t *)&entry->handle, abort_timeout_close_cb);
  }
}

void gc_mark_abort(ant_t *js, gc_mark_fn mark) {
  for (abort_timeout_entry_t *e = js->abort_timeouts; e; e = e->next) mark(js, e->signal);
}

void gc_mark_abort_signal_object(ant_t *js, ant_value_t signal, gc_mark_fn mark) {
  abort_signal_data_t *data = (abort_signal_data_t *)js_get_native(signal, ABORT_SIGNAL_NATIVE_TAG);
  if (!data) return;
  mark(js, data->reason);
  
  unsigned int follower_count = data->followers ? utarray_len(data->followers) : 0;
  for (unsigned int i = 0; i < follower_count; i++)
    mark(js, *(ant_value_t *)utarray_eltptr(data->followers, i));
}

bool abort_signal_is_aborted(ant_value_t signal) {
  abort_signal_data_t *data = get_signal_data(signal);
  return data && signal_aborted(data);
}

bool abort_signal_is_signal(ant_value_t signal) {
  return get_signal_data(signal) != NULL;
}

ant_value_t abort_signal_get_reason(ant_value_t signal) {
  abort_signal_data_t *data = get_signal_data(signal);
  return data ? data->reason : js_mkundef();
}

ant_value_t abort_signal_make_error(ant_t *js, ant_value_t signal) {
  ant_value_t error = Ant_Error_Create(js, JS_ERR_GENERIC, "The operation was aborted");
  if (!is_object_type(error)) return error;

  js_set(js, error, "name", ANT_STRING("AbortError"));
  js_set(js, error, "code", ANT_STRING("ABORT_ERR"));

  ant_value_t reason = abort_signal_get_reason(signal);
  if (vtype(reason) != kTypeUndefined) js_set(js, error, "cause", reason);
  return error;
}

void abort_signal_add_listener(ant_t *js, ant_value_t signal, ant_value_t callback) {
  abort_signal_data_t *data = get_signal_data(signal);
  if (!data) return;

  if (!signal_aborted(data)) {
    Ant_EventTarget_AddListener(js, signal, ANT_STRING("abort"), callback);
    return;
  }

  ant_value_t event = Ant_Event_CreateTrusted(js, js->builtins.event_proto, ANT_EVENT_BASE, "abort");
  if (is_err(event)) return;
  
  Ant_Event_BeginDispatch(js, event, signal);
  sv_vm_call(js->vm, js, callback, signal, &event, 1, NULL, js_mkundef());
  Ant_Event_EndDispatch(js, event);
}

void abort_signal_remove_listener(ant_t *js, ant_value_t signal, ant_value_t callback) {
  if (get_signal_data(signal)) Ant_EventTarget_RemoveListener(js, signal, ANT_STRING("abort"), callback);
}
