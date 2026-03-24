#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <uthash.h>
#include <utarray.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"
#include "silver/engine.h"

#include "gc/modules.h"
#include "modules/abort.h"
#include "modules/events.h"
#include "modules/symbol.h"

typedef struct {
  bool canceled;
  bool stop_immediate;
  bool stop_propagation;
  bool dispatching;
} event_data_t;

static ant_value_t g_isTrusted_getter            = 0;
static ant_value_t g_event_proto                 = 0;
static ant_value_t g_customevent_proto           = 0;
static ant_value_t g_errorevent_proto            = 0;
static ant_value_t g_promiserejectionevent_proto = 0;

static event_data_t *get_event_data(ant_value_t obj) {
  ant_value_t slot = js_get_slot(obj, SLOT_DATA);
  if (vtype(slot) != T_NUM) return NULL;
  return (event_data_t *)(uintptr_t)js_getnum(slot);
}

static double get_timestamp_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
  ant_value_t callback;
  ant_value_t signal;
  bool once;
  bool capture;
} EventListenerEntry;

static const UT_icd event_listener_icd = { 
  sizeof(EventListenerEntry),
  NULL, NULL, NULL 
};

typedef struct {
  UT_array *listeners;
  char *event_type;
  UT_hash_handle hh;
} EventType;

typedef struct emitter_reg {
  EventType **events;
  struct emitter_reg *next;
} emitter_reg_t;

static EventType *global_events     = NULL;
static emitter_reg_t *emitter_registry = NULL;

static EventType *make_event_type(const char *event_type) {
  size_t etlen = strlen(event_type);
  EventType *evt = ant_calloc(sizeof(EventType) + etlen + 1);
  if (!evt) return NULL;
  evt->event_type = (char *)(evt + 1);
  memcpy(evt->event_type, event_type, etlen + 1);
  utarray_new(evt->listeners, &event_listener_icd);
  return evt;
}

static EventType **get_or_create_emitter_events(ant_t *js, ant_value_t this_obj) {
  ant_value_t slot = js_get_slot(this_obj, SLOT_DATA);
  if (vtype(slot) == T_UNDEF) {
    EventType **events = ant_calloc(sizeof(EventType *));
    if (!events) return NULL;
    *events = NULL;

    emitter_reg_t *reg = ant_calloc(sizeof(emitter_reg_t));
    if (!reg) { free(events); return NULL; }
    reg->events = events;
    reg->next = emitter_registry;
    emitter_registry = reg;

    js_set_slot(this_obj, SLOT_DATA, ANT_PTR(events));
    return events;
  }
  return (EventType **)(uintptr_t)js_getnum(slot);
}

static EventType *find_or_create_global_event_type(const char *event_type) {
  EventType *evt = NULL;
  HASH_FIND_STR(global_events, event_type, evt);
  if (!evt) {
    evt = make_event_type(event_type);
    if (!evt) return NULL;
    HASH_ADD_KEYPTR(hh, global_events, evt->event_type, strlen(evt->event_type), evt);
  }
  return evt;
}

static EventType *find_global_event_type(const char *event_type) {
  EventType *evt = NULL;
  HASH_FIND_STR(global_events, event_type, evt);
  return evt;
}

static EventType *find_or_create_emitter_event_type(ant_t *js, ant_value_t this_obj, const char *event_type) {
  EventType **events = get_or_create_emitter_events(js, this_obj);
  if (!events) return NULL;
  EventType *evt = NULL;
  HASH_FIND_STR(*events, event_type, evt);
  if (!evt) {
    evt = make_event_type(event_type);
    if (!evt) return NULL;
    HASH_ADD_KEYPTR(hh, *events, evt->event_type, strlen(evt->event_type), evt);
  }
  return evt;
}

static EventType *find_emitter_event_type(ant_t *js, ant_value_t this_obj, const char *event_type) {
  EventType **events = get_or_create_emitter_events(js, this_obj);
  if (!events) return NULL;
  EventType *evt = NULL;
  HASH_FIND_STR(*events, event_type, evt);
  return evt;
}

static void js_init_event_obj(ant_t *js, ant_value_t obj, ant_value_t type_val, bool bubbles, bool cancelable) {
  js_set(js, obj, "type",             type_val);
  js_set(js, obj, "target",           js_mknull());
  js_set(js, obj, "srcElement",       js_mknull());
  js_set(js, obj, "currentTarget",    js_mknull());
  js_set(js, obj, "eventPhase",       js_mknum(0));
  js_set(js, obj, "bubbles",          js_bool(bubbles));
  js_set(js, obj, "cancelable",       js_bool(cancelable));
  js_set(js, obj, "defaultPrevented", js_false);
  js_set(js, obj, "returnValue",      js_true);
  js_set(js, obj, "cancelBubble",     js_false);
  js_set(js, obj, "timeStamp",        js_mknum(get_timestamp_ms()));

  if (g_isTrusted_getter)
    js_set_accessor_desc(js, obj, "isTrusted", 9, g_isTrusted_getter, js_mkundef(), 0);

  event_data_t *data = ant_calloc(sizeof(event_data_t));
  if (data) js_set_slot(obj, SLOT_DATA, ANT_PTR(data));
}

static ant_value_t js_event_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Event constructor requires 'new'");
  if (nargs < 1 || vtype(args[0]) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Event constructor: type argument is required");

  ant_value_t type_val = args[0];
  if (vtype(type_val) != T_STR) {
    type_val = js_tostring_val(js, type_val);
    if (is_err(type_val)) return type_val;
  }

  bool bubbles = false, cancelable = false;
  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t b = js_get(js, args[1], "bubbles");
    ant_value_t c = js_get(js, args[1], "cancelable");
    if (is_err(b)) return b;
    if (is_err(c)) return c;
    bubbles    = js_truthy(js, b);
    cancelable = js_truthy(js, c);
  }

  ant_value_t this_obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_event_proto);
  if (is_object_type(proto)) js_set_proto_init(this_obj, proto);

  js_init_event_obj(js, this_obj, type_val, bubbles, cancelable);
  return this_obj;
}

static ant_value_t js_event_get_isTrusted(ant_t *js, ant_value_t *args, int nargs) {
  return js_false;
}

static ant_value_t js_event_preventDefault(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t cancelable = js_get(js, this_obj, "cancelable");
  if (!js_truthy(js, cancelable)) return js_mkundef();
  event_data_t *data = get_event_data(this_obj);
  if (data) data->canceled = true;
  js_set(js, this_obj, "defaultPrevented", js_true);
  js_set(js, this_obj, "returnValue", js_false);
  return js_mkundef();
}

static ant_value_t js_event_stopPropagation(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  event_data_t *data = get_event_data(this_obj);
  if (data) data->stop_propagation = true;
  js_set(js, this_obj, "cancelBubble", js_true);
  return js_mkundef();
}

static ant_value_t js_event_stopImmediatePropagation(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  event_data_t *data = get_event_data(this_obj);
  if (data) { data->stop_immediate = true; data->stop_propagation = true; }
  js_set(js, this_obj, "cancelBubble", js_true);
  return js_mkundef();
}

static ant_value_t js_event_composedPath(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  event_data_t *data = get_event_data(this_obj);
  ant_value_t result = js_mkarr(js);
  if (data && data->dispatching) {
    ant_value_t ct = js_get(js, this_obj, "currentTarget");
    if (is_object_type(ct)) js_arr_push(js, result, ct);
  }
  return result;
}

static ant_value_t js_event_initEvent(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs >= 1) {
    ant_value_t type_val = vtype(args[0]) == T_STR ? args[0] : js_tostring_val(js, args[0]);
    if (!is_err(type_val)) js_set(js, this_obj, "type", type_val);
  }
  if (nargs >= 2) js_set(js, this_obj, "bubbles",   js_bool(js_truthy(js, args[1])));
  if (nargs >= 3) js_set(js, this_obj, "cancelable", js_bool(js_truthy(js, args[2])));
  js_set(js, this_obj, "defaultPrevented", js_false);
  js_set(js, this_obj, "returnValue", js_true);
  event_data_t *data = get_event_data(this_obj);
  if (data) { data->canceled = false; data->stop_immediate = false; data->stop_propagation = false; }
  return js_mkundef();
}

static ant_value_t js_customevent_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "CustomEvent constructor requires 'new'");
  if (nargs < 1 || vtype(args[0]) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "CustomEvent constructor: type argument is required");

  ant_value_t type_val = args[0];
  if (vtype(type_val) != T_STR) {
    type_val = js_tostring_val(js, type_val);
    if (is_err(type_val)) return type_val;
  }

  bool bubbles = false, cancelable = false;
  ant_value_t detail = js_mknull();
  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t b = js_get(js, args[1], "bubbles");
    ant_value_t c = js_get(js, args[1], "cancelable");
    ant_value_t d = js_get(js, args[1], "detail");
    if (is_err(b)) return b;
    if (is_err(c)) return c;
    bubbles    = js_truthy(js, b);
    cancelable = js_truthy(js, c);
    if (vtype(d) != T_UNDEF) detail = d;
  }

  ant_value_t this_obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_customevent_proto);
  if (is_object_type(proto)) js_set_proto_init(this_obj, proto);

  js_init_event_obj(js, this_obj, type_val, bubbles, cancelable);
  js_set(js, this_obj, "detail", detail);
  return this_obj;
}

static ant_value_t js_errorevent_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "ErrorEvent constructor requires 'new'");
  if (nargs < 1 || vtype(args[0]) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "ErrorEvent constructor: type argument is required");

  ant_value_t type_val = args[0];
  if (vtype(type_val) != T_STR) {
    type_val = js_tostring_val(js, type_val);
    if (is_err(type_val)) return type_val;
  }

  bool bubbles = false, cancelable = false;
  ant_value_t message  = js_mkstr(js, "", 0);
  ant_value_t filename = js_mkstr(js, "", 0);
  ant_value_t lineno   = js_mknum(0);
  ant_value_t colno    = js_mknum(0);
  ant_value_t error    = js_mknull();

  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t b  = js_get(js, args[1], "bubbles");
    ant_value_t c  = js_get(js, args[1], "cancelable");
    ant_value_t m  = js_get(js, args[1], "message");
    ant_value_t f  = js_get(js, args[1], "filename");
    ant_value_t l  = js_get(js, args[1], "lineno");
    ant_value_t co = js_get(js, args[1], "colno");
    ant_value_t e  = js_get(js, args[1], "error");
    if (is_err(b)) return b;
    if (is_err(c)) return c;
    bubbles    = js_truthy(js, b);
    cancelable = js_truthy(js, c);
    if (vtype(m) != T_UNDEF) { message  = js_tostring_val(js, m); if (is_err(message))  return message;  }
    if (vtype(f) != T_UNDEF) { filename = js_tostring_val(js, f); if (is_err(filename)) return filename; }
    if (vtype(l) != T_UNDEF) lineno = l;
    if (vtype(co) != T_UNDEF) colno = co;
    if (vtype(e) != T_UNDEF) error = e;
  }

  ant_value_t this_obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_errorevent_proto);
  if (is_object_type(proto)) js_set_proto_init(this_obj, proto);

  js_init_event_obj(js, this_obj, type_val, bubbles, cancelable);
  js_set(js, this_obj, "message",  message);
  js_set(js, this_obj, "filename", filename);
  js_set(js, this_obj, "lineno",   lineno);
  js_set(js, this_obj, "colno",    colno);
  js_set(js, this_obj, "error",    error);
  return this_obj;
}

static ant_value_t js_promiserejectionevent_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "PromiseRejectionEvent constructor requires 'new'");
  if (nargs < 1 || vtype(args[0]) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "PromiseRejectionEvent constructor: type argument is required");

  ant_value_t type_val = args[0];
  if (vtype(type_val) != T_STR) {
    type_val = js_tostring_val(js, type_val);
    if (is_err(type_val)) return type_val;
  }

  bool bubbles = false, cancelable = false;
  ant_value_t promise = js_mkundef();
  ant_value_t reason  = js_mkundef();

  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t b = js_get(js, args[1], "bubbles");
    ant_value_t c = js_get(js, args[1], "cancelable");
    ant_value_t p = js_get(js, args[1], "promise");
    ant_value_t r = js_get(js, args[1], "reason");
    if (is_err(b)) return b;
    if (is_err(c)) return c;
    bubbles    = js_truthy(js, b);
    cancelable = js_truthy(js, c);
    if (vtype(p) != T_UNDEF) promise = p;
    if (vtype(r) != T_UNDEF) reason  = r;
  }

  ant_value_t this_obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_promiserejectionevent_proto);
  if (is_object_type(proto)) js_set_proto_init(this_obj, proto);

  js_init_event_obj(js, this_obj, type_val, bubbles, cancelable);
  js_set(js, this_obj, "promise", promise);
  js_set(js, this_obj, "reason",  reason);
  return this_obj;
}

static bool parse_addEventListener_options(ant_t *js, ant_value_t *args, int nargs, bool *once, bool *capture, ant_value_t *signal) {
  *once = false; *capture = false; *signal = js_mkundef();
  if (nargs < 3) return true;
  ant_value_t opts = args[2];
  if (vtype(opts) == T_BOOL) {
    *capture = js_truthy(js, opts);
    return true;
  }
  if (vtype(opts) != T_OBJ) return true;

  ant_value_t sig = js_get(js, opts, "signal");
  if (vtype(sig) == T_NULL) return false;
  if (vtype(sig) != T_UNDEF) {
    if (!is_object_type(sig)) return false;
    *signal = sig;
  }

  ant_value_t o = js_get(js, opts, "once");
  ant_value_t ca = js_get(js, opts, "capture");
  if (vtype(o) != T_UNDEF)  *once    = js_truthy(js, o);
  if (vtype(ca) != T_UNDEF) *capture = js_truthy(js, ca);
  return true;
}

static ant_value_t add_listener_to(ant_t *js, ant_value_t *args, int nargs, EventType *evt) {
  if (!evt) return js_mkerr(js, "failed to create event type");

  bool once, capture;
  ant_value_t signal;
  if (!parse_addEventListener_options(js, args, nargs, &once, &capture, &signal))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to execute 'addEventListener': signal is not an AbortSignal");

  if (vtype(signal) != T_UNDEF && abort_signal_is_aborted(signal)) return js_mkundef();

  ant_value_t cb = args[1];
  uint8_t cbt = vtype(cb);
  if (cbt == T_NULL || cbt == T_UNDEF) return js_mkundef();
  if (cbt != T_FUNC && cbt != T_CFUNC) return js_mkundef();

  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
    EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    if (e->callback == cb && e->capture == capture) return js_mkundef();
  }

  EventListenerEntry entry = { cb, signal, once, capture };
  utarray_push_back(evt->listeners, &entry);
  return js_mkundef();
}

static ant_value_t remove_listener_from(ant_t *js, ant_value_t *args, int nargs, EventType *evt) {
  if (!evt) return js_mkundef();

  bool capture = false;
  if (nargs >= 3) {
  ant_value_t opts = args[2];
  if (vtype(opts) == T_BOOL) capture = js_truthy(js, opts);
  else if (vtype(opts) == T_OBJ) {
    ant_value_t ca = js_get(js, opts, "capture");
    if (vtype(ca) != T_UNDEF) capture = js_truthy(js, ca);
  }}

  ant_value_t cb = (nargs >= 2) ? args[1] : js_mkundef();
  uint8_t cbt = vtype(cb);
  if (cbt == T_NULL || cbt == T_UNDEF) return js_mkundef();

  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
    EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    if (e->callback == cb && e->capture == capture) {
      utarray_erase(evt->listeners, i, 1);
      return js_mkundef();
    }
  }
  return js_mkundef();
}

static ant_value_t dispatch_event_to(ant_t *js, ant_value_t event_obj, EventType *evt, ant_value_t target) {
  if (!evt) return js_true;

  event_data_t *data = get_event_data(event_obj);
  if (data) { data->dispatching = true; data->stop_immediate = false; }
  js_set(js, event_obj, "target",        target);
  js_set(js, event_obj, "currentTarget", target);
  js_set(js, event_obj, "eventPhase",    js_mknum(2));

  ant_value_t call_args[1] = { event_obj };
  unsigned int n = utarray_len(evt->listeners);

  for (unsigned int i = 0; i < n;) {
    EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);

    if (vtype(entry->signal) != T_UNDEF && abort_signal_is_aborted(entry->signal)) {
      utarray_erase(evt->listeners, i, 1);
      n--; continue;
    }

    ant_value_t cb   = entry->callback;
    bool        once = entry->once;
    if (once) { utarray_erase(evt->listeners, i, 1); n--; } else i++;
    
    uint8_t t = vtype(cb);
    if (t != T_FUNC && t != T_CFUNC) continue;

    sv_vm_call(js->vm, js, cb, js_mkundef(), call_args, 1, NULL, false);
    if (data && data->stop_immediate) break;
  }

  if (data) data->dispatching = false;
  js_set(js, event_obj, "currentTarget", js_mknull());
  js_set(js, event_obj, "eventPhase",    js_mknum(0));

  bool canceled = data && data->canceled;
  return js_bool(!canceled);
}

static ant_value_t js_add_event_listener_method(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return js_mkundef();
  return add_listener_to(js, args, nargs,
    find_or_create_emitter_event_type(js, js_getthis(js), event_type));
}

static ant_value_t js_add_event_listener(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return js_mkundef();
  return add_listener_to(js, args, nargs, find_or_create_global_event_type(event_type));
}

static ant_value_t js_remove_event_listener_method(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return js_mkundef();
  return remove_listener_from(js, args, nargs,
    find_emitter_event_type(js, js_getthis(js), event_type));
}

static ant_value_t js_remove_event_listener(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return js_mkundef();
  return remove_listener_from(js, args, nargs, find_global_event_type(event_type));
}

static ant_value_t js_dispatch_event_method(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  ant_value_t this_obj = js_getthis(js);
  ant_value_t type_val = js_get(js, args[0], "type");
  const char *event_type = js_getstr(js, type_val, NULL);
  if (!event_type) return js_false;
  return dispatch_event_to(js, args[0],
    find_emitter_event_type(js, this_obj, event_type), this_obj);
}

static ant_value_t js_dispatch_event(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  ant_value_t type_val = js_get(js, args[0], "type");
  const char *event_type = js_getstr(js, type_val, NULL);
  if (!event_type) return js_false;
  return dispatch_event_to(js, args[0],
    find_global_event_type(event_type), js_glob(js));
}

void js_dispatch_global_event(ant_t *js, ant_value_t event_obj) {
  ant_value_t type_val = js_get(js, event_obj, "type");
  const char *event_type = js_getstr(js, type_val, NULL);
  if (!event_type) return;
  EventType *evt = find_global_event_type(event_type);
  if (!evt || utarray_len(evt->listeners) == 0) return;
  dispatch_event_to(js, event_obj, evt, js_glob(js));
}

static ant_value_t js_eventemitter_add(ant_t *js, ant_value_t *args, int nargs, bool once) {
  if (nargs < 2) return js_mkerr(js, "requires 2 arguments (event, listener)");
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return js_mkerr(js, "event must be a string");
  uint8_t t = vtype(args[1]);
  if (t != T_FUNC && t != T_CFUNC) return js_mkerr(js, "listener must be a function");

  ant_value_t this_obj = js_getthis(js);
  EventType *evt = find_or_create_emitter_event_type(js, this_obj, event_type);
  if (!evt) return js_mkerr(js, "failed to create event type");

  EventListenerEntry entry = { args[1], js_mkundef(), once, false };
  utarray_push_back(evt->listeners, &entry);
  return this_obj;
}

static ant_value_t js_eventemitter_on(ant_t *js, ant_value_t *args, int nargs) {
  return js_eventemitter_add(js, args, nargs, false);
}

static ant_value_t js_eventemitter_once(ant_t *js, ant_value_t *args, int nargs) {
  return js_eventemitter_add(js, args, nargs, true);
}

static ant_value_t js_eventemitter_off(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "off requires 2 arguments (event, listener)");
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return js_mkerr(js, "event must be a string");
  ant_value_t this_obj = js_getthis(js);
  remove_listener_from(js, args, nargs, find_emitter_event_type(js, this_obj, event_type));
  return this_obj;
}

static ant_value_t js_eventemitter_emit(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "emit requires at least 1 argument (event)");
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return js_mkerr(js, "event must be a string");

  ant_value_t this_obj = js_getthis(js);
  EventType *evt = find_emitter_event_type(js, this_obj, event_type);
  if (!evt || utarray_len(evt->listeners) == 0) return js_false;

  int listener_nargs = nargs - 1;
  ant_value_t *listener_args = listener_nargs > 0 ? &args[1] : NULL;

  for (unsigned int i = 0; i < utarray_len(evt->listeners);) {
    EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    ant_value_t cb = entry->callback;
    bool once = entry->once;
    if (once) { utarray_erase(evt->listeners, i, 1); } else i++;
    uint8_t t = vtype(cb);
    if (t != T_FUNC && t != T_CFUNC) continue;
    ant_value_t result = sv_vm_call(js->vm, js, cb, js_mkundef(), listener_args, listener_nargs, NULL, false);
    if (vtype(result) == T_ERR)
      fprintf(stderr, "Error in event listener for '%s': %s\n", event_type, js_str(js, result));
  }
  return js_true;
}

static ant_value_t js_eventemitter_removeAllListeners(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 1) return this_obj;
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return this_obj;
  EventType *evt = find_emitter_event_type(js, this_obj, event_type);
  if (evt) utarray_clear(evt->listeners);
  return this_obj;
}

static ant_value_t js_eventemitter_listenerCount(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mknum(0);
  const char *event_type = js_getstr(js, args[0], NULL);
  if (!event_type) return js_mknum(0);
  EventType *evt = find_emitter_event_type(js, js_getthis(js), event_type);
  if (!evt) return js_mknum(0);
  return js_mknum((double)utarray_len(evt->listeners));
}

static ant_value_t js_eventemitter_eventNames(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t result = js_mkarr(js);
  EventType **events = get_or_create_emitter_events(js, this_obj);
  if (events && *events) {
    EventType *evt, *tmp;
    HASH_ITER(hh, *events, evt, tmp) {
      if (utarray_len(evt->listeners) > 0)
        js_arr_push(js, result, js_mkstr(js, evt->event_type, strlen(evt->event_type)));
    }
  }
  return result;
}

static ant_value_t make_event_ctor(ant_t *js, ant_cfunc_t fn, ant_value_t proto, const char *name, size_t nlen) {
  ant_value_t ctor_obj = js_mkobj(js);
  js_set_slot(ctor_obj, SLOT_CFUNC, js_mkfun(fn));
  
  js_mkprop_fast(js, ctor_obj, "prototype", 9, proto);
  js_mkprop_fast(js, ctor_obj, "name", 4, js_mkstr(js, name, nlen));
  js_set_descriptor(js, ctor_obj, "name", 4, 0);
  
  ant_value_t fn_val = js_obj_to_func(ctor_obj);
  js_set(js, proto, "constructor", fn_val);
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  return fn_val;
}

ant_value_t events_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t eventemitter_ctor  = js_mkobj(js);
  ant_value_t eventemitter_proto = js_mkobj(js);

  js_set(js, eventemitter_proto, "on",                 js_mkfun(js_eventemitter_on));
  js_set(js, eventemitter_proto, "addListener",        js_mkfun(js_eventemitter_on));
  js_set(js, eventemitter_proto, "once",               js_mkfun(js_eventemitter_once));
  js_set(js, eventemitter_proto, "off",                js_mkfun(js_eventemitter_off));
  js_set(js, eventemitter_proto, "removeListener",     js_mkfun(js_eventemitter_off));
  js_set(js, eventemitter_proto, "emit",               js_mkfun(js_eventemitter_emit));
  js_set(js, eventemitter_proto, "removeAllListeners", js_mkfun(js_eventemitter_removeAllListeners));
  js_set(js, eventemitter_proto, "listenerCount",      js_mkfun(js_eventemitter_listenerCount));
  js_set(js, eventemitter_proto, "eventNames",         js_mkfun(js_eventemitter_eventNames));
  js_set_sym(js, eventemitter_proto, get_toStringTag_sym(), js_mkstr(js, "EventEmitter", 12));

  js_mkprop_fast(js, eventemitter_ctor, "prototype", 9, eventemitter_proto);
  js_mkprop_fast(js, eventemitter_ctor, "name", 4, ANT_STRING("EventEmitter"));
  js_set_descriptor(js, eventemitter_ctor, "name", 4, 0);

  ant_value_t ctor_fn = js_obj_to_func_ex(eventemitter_ctor, SV_CALL_IS_DEFAULT_CTOR);
  js_set(js, lib, "EventEmitter", ctor_fn);
  js_set(js, lib, "default", ctor_fn);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "events", 6));
  
  return lib;
}

void init_events_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);
  g_isTrusted_getter = js_mkfun(js_event_get_isTrusted);

  g_event_proto = js_mkobj(js);
  js_set_sym(js, g_event_proto, get_toStringTag_sym(), js_mkstr(js, "Event", 5));
  js_set(js, g_event_proto, "preventDefault",          js_mkfun(js_event_preventDefault));
  js_set(js, g_event_proto, "stopPropagation",         js_mkfun(js_event_stopPropagation));
  js_set(js, g_event_proto, "stopImmediatePropagation", js_mkfun(js_event_stopImmediatePropagation));
  js_set(js, g_event_proto, "composedPath",            js_mkfun(js_event_composedPath));
  js_set(js, g_event_proto, "initEvent",               js_mkfun(js_event_initEvent));
  js_set(js, g_event_proto, "NONE",             js_mknum(0));
  js_set(js, g_event_proto, "CAPTURING_PHASE",  js_mknum(1));
  js_set(js, g_event_proto, "AT_TARGET",        js_mknum(2));
  js_set(js, g_event_proto, "BUBBLING_PHASE",   js_mknum(3));

  ant_value_t event_fn = make_event_ctor(js, js_event_ctor, g_event_proto, "Event", 5);
  js_set(js, event_fn, "NONE",            js_mknum(0));
  js_set(js, event_fn, "CAPTURING_PHASE", js_mknum(1));
  js_set(js, event_fn, "AT_TARGET",       js_mknum(2));
  js_set(js, event_fn, "BUBBLING_PHASE",  js_mknum(3));
  js_set(js, global, "Event", event_fn);

  g_customevent_proto = js_mkobj(js);
  js_set_proto_init(g_customevent_proto, g_event_proto);
  js_set_sym(js, g_customevent_proto, get_toStringTag_sym(), js_mkstr(js, "CustomEvent", 11));

  ant_value_t customevent_fn = make_event_ctor(js, js_customevent_ctor, g_customevent_proto, "CustomEvent", 11);
  js_set(js, global, "CustomEvent", customevent_fn);

  g_errorevent_proto = js_mkobj(js);
  js_set_proto_init(g_errorevent_proto, g_event_proto);
  js_set_sym(js, g_errorevent_proto, get_toStringTag_sym(), js_mkstr(js, "ErrorEvent", 10));

  ant_value_t errorevent_fn = make_event_ctor(js, js_errorevent_ctor, g_errorevent_proto, "ErrorEvent", 10);
  js_set(js, global, "ErrorEvent", errorevent_fn);

  g_promiserejectionevent_proto = js_mkobj(js);
  js_set_proto_init(g_promiserejectionevent_proto, g_event_proto);
  js_set_sym(js, g_promiserejectionevent_proto, get_toStringTag_sym(), js_mkstr(js, "PromiseRejectionEvent", 21));

  ant_value_t pre_fn = make_event_ctor(js, js_promiserejectionevent_ctor, g_promiserejectionevent_proto, "PromiseRejectionEvent", 21);
  js_set(js, global, "PromiseRejectionEvent", pre_fn);

  ant_value_t eventtarget_proto = js_mkobj(js);
  js_set(js, eventtarget_proto, "addEventListener",    js_mkfun(js_add_event_listener_method));
  js_set(js, eventtarget_proto, "removeEventListener", js_mkfun(js_remove_event_listener_method));
  js_set(js, eventtarget_proto, "dispatchEvent",       js_mkfun(js_dispatch_event_method));
  js_set_sym(js, eventtarget_proto, get_toStringTag_sym(), js_mkstr(js, "EventTarget", 11));

  ant_value_t eventtarget_ctor = js_mkobj(js);
  js_mkprop_fast(js, eventtarget_ctor, "prototype", 9, eventtarget_proto);
  js_mkprop_fast(js, eventtarget_ctor, "name", 4, ANT_STRING("EventTarget"));
  js_set_descriptor(js, eventtarget_ctor, "name", 4, 0);
  ant_value_t eventtarget_fn = js_obj_to_func_ex(eventtarget_ctor, SV_CALL_IS_DEFAULT_CTOR);
  js_set(js, eventtarget_proto, "constructor", eventtarget_fn);
  js_set_descriptor(js, eventtarget_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "addEventListener",    js_mkfun(js_add_event_listener));
  js_set(js, global, "removeEventListener", js_mkfun(js_remove_event_listener));
  js_set(js, global, "dispatchEvent",       js_mkfun(js_dispatch_event));
  js_set(js, global, "EventTarget",         eventtarget_fn);
}

static void mark_event_type_listeners(ant_t *js, gc_mark_fn mark, EventType *events) {
  EventType *evt, *tmp;
  HASH_ITER(hh, events, evt, tmp) {
  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
    EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    mark(js, e->callback);
    if (vtype(e->signal) != T_UNDEF) mark(js, e->signal);
  }
}}

void gc_mark_events(ant_t *js, gc_mark_fn mark) {
  mark_event_type_listeners(js, mark, global_events);
  for (emitter_reg_t *reg = emitter_registry; reg; reg = reg->next) {
    if (*reg->events) mark_event_type_listeners(js, mark, *reg->events);
  }
  if (g_isTrusted_getter)            mark(js, g_isTrusted_getter);
  if (g_event_proto)                 mark(js, g_event_proto);
  if (g_customevent_proto)           mark(js, g_customevent_proto);
  if (g_errorevent_proto)            mark(js, g_errorevent_proto);
  if (g_promiserejectionevent_proto) mark(js, g_promiserejectionevent_proto);
}
