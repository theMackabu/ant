#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <uthash.h>

#include "ant.h"
#include "errors.h"
#include "arena.h"
#include "runtime.h"
#include "internal.h"

#include "modules/events.h"
#include "modules/symbol.h"

#define MAX_LISTENERS_PER_EVENT 32

typedef struct {
  jsval_t listener;
  bool once;
} __attribute__((packed)) EventListener;

typedef struct {
  EventListener listeners[MAX_LISTENERS_PER_EVENT];
  char *event_type;
  UT_hash_handle hh;
  int listener_count;
} EventType;

static EventType *global_events = NULL;

static inline void remove_listener_at(EventType *evt, int i) {
  int remaining = evt->listener_count - i - 1;
  if (remaining > 0) {
    memmove(&evt->listeners[i], &evt->listeners[i + 1], remaining * sizeof(EventListener));
  }
  evt->listener_count--;
}

static EventType **get_or_create_emitter_events(struct js *js, jsval_t this_obj) {
  jsval_t slot = js_get_slot(js, this_obj, SLOT_DATA);
  
  if (vtype(slot) == T_UNDEF) {
    EventType **events = ant_calloc(sizeof(EventType *));
    if (!events) return NULL;
    *events = NULL;
    js_set_slot(js, this_obj, SLOT_DATA, ANT_PTR(events));
    return events;
  }
  
  return (EventType **)(uintptr_t)js_getnum(slot);
}

static EventType *find_emitter_event_type(struct js *js, jsval_t this_obj, const char *event_type) {
  EventType **events = get_or_create_emitter_events(js, this_obj);
  if (events == NULL) return NULL;
  
  EventType *evt = NULL;
  HASH_FIND_STR(*events, event_type, evt);
  return evt;
}

static EventType *find_or_create_emitter_event_type(struct js *js, jsval_t this_obj, const char *event_type) {
  EventType **events = get_or_create_emitter_events(js, this_obj);
  if (events == NULL) return NULL;
  
  EventType *evt = NULL;
  HASH_FIND_STR(*events, event_type, evt);
  
  if (evt == NULL) {
    size_t etlen = strlen(event_type);
    evt = ant_calloc(sizeof(EventType) + etlen + 1);
    if (!evt) return NULL;
    evt->event_type = (char *)(evt + 1);
    memcpy(evt->event_type, event_type, etlen + 1);
    evt->listener_count = 0;
    HASH_ADD_KEYPTR(hh, *events, evt->event_type, etlen, evt);
  }
  
  return evt;
}

static EventType *find_or_create_global_event_type(const char *event_type) {
  EventType *evt = NULL;
  HASH_FIND_STR(global_events, event_type, evt);
  
  if (evt == NULL) {
    size_t etlen = strlen(event_type);
    evt = ant_calloc(sizeof(EventType) + etlen + 1);
    if (!evt) return NULL;
    evt->event_type = (char *)(evt + 1);
    memcpy(evt->event_type, event_type, etlen + 1);
    evt->listener_count = 0;
    HASH_ADD_KEYPTR(hh, global_events, evt->event_type, etlen, evt);
  }
  
  return evt;
}

static inline EventType *find_global_event_type(const char *event_type) {
  EventType *evt = NULL;
  HASH_FIND_STR(global_events, event_type, evt);
  return evt;
}

// addEventListener(eventType, listener, options)
static jsval_t js_add_event_listener_method(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "addEventListener requires at least 2 arguments (eventType, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) return js_mkerr(js, "eventType must be a string");
  if (vtype(args[1]) != T_FUNC) return js_mkerr(js, "listener must be a function");
  
  EventType *evt = find_or_create_emitter_event_type(js, this_obj, event_type);
  if (evt == NULL) return js_mkerr(js, "failed to create event type");
  
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum number of listeners for event type '%s' reached", event_type);
  }
  
  bool once = false;
  if (nargs >= 3 && vtype(args[2]) != T_UNDEF) {
    jsval_t once_val = js_get(js, args[2], "once");
    if (vtype(once_val) != T_UNDEF) once = js_truthy(js, once_val);
  }
  
  EventListener *listener = &evt->listeners[evt->listener_count++];
  listener->listener = args[1];
  listener->once = once;
  
  return js_mkundef();
}

// addEventListener(eventType, listener, options)
static jsval_t js_add_event_listener(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "addEventListener requires at least 2 arguments (eventType, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "eventType must be a string");
  }
  
  if (vtype(args[1]) != T_FUNC) {
    return js_mkerr(js, "listener must be a function");
  }
  
  EventType *evt = find_or_create_global_event_type(event_type);
  if (evt == NULL) {
    return js_mkerr(js, "failed to create event type");
  }
  
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum number of listeners for event type '%s' reached", event_type);
  }
  
  bool once = false;
  if (nargs >= 3 && vtype(args[2]) != T_UNDEF) {
    jsval_t once_val = js_get(js, args[2], "once");
    if (vtype(once_val) != T_UNDEF) once = js_truthy(js, once_val);
  }
  
  EventListener *listener = &evt->listeners[evt->listener_count++];
  listener->listener = args[1];
  listener->once = once;
  
  return js_mkundef();
}

// removeEventListener(eventType, listener)
static jsval_t js_remove_event_listener_method(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "removeEventListener requires 2 arguments (eventType, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) return js_mkerr(js, "eventType must be a string");
  
  EventType *evt = find_emitter_event_type(js, this_obj, event_type);
  if (evt == NULL) return js_mkundef();
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener == args[1]) {
      remove_listener_at(evt, i); break;
    }
  }
  
  return js_mkundef();
}

// removeEventListener(eventType, listener)
static jsval_t js_remove_event_listener(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "removeEventListener requires 2 arguments (eventType, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) return js_mkerr(js, "eventType must be a string");
  
  EventType *evt = find_global_event_type(event_type);
  if (evt == NULL) return js_mkundef();
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener == args[1]) {
      remove_listener_at(evt, i); break;
    }
  }
  
  return js_mkundef();
}

// dispatchEvent(eventType, eventData)
static jsval_t js_dispatch_event_method(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 1) {
    return js_mkerr(js, "dispatchEvent requires at least 1 argument (eventType)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) return js_mkerr(js, "eventType must be a string");
  
  EventType *evt = find_emitter_event_type(js, this_obj, event_type);
  if (evt == NULL || evt->listener_count == 0) return js_true;
  
  jsval_t event_obj = js_mkobj(js);
  js_set(js, event_obj, "type", args[0]);
  js_set(js, event_obj, "target", this_obj);
  js_set(js, event_obj, get_toStringTag_sym_key(), js_mkstr(js, "Event", 5));
  
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    js_set(js, event_obj, "detail", args[1]);
  }
  
  jsval_t listener_args[1] = {event_obj};
  int i = 0;
  while (i < evt->listener_count) {
    EventListener *listener = &evt->listeners[i];
    jsval_t result = js_call(js, listener->listener, listener_args, 1);
    
    if (vtype(result) == T_ERR) {
      fprintf(stderr, "Error in event listener for '%s': %s\n", event_type, js_str(js, result));
    }
    
    if (listener->once) {
      remove_listener_at(evt, i);
    } else i++;
  }
  
  return js_true;
}

// dispatchEvent(eventType, eventData)
static jsval_t js_dispatch_event(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "dispatchEvent requires at least 1 argument (eventType)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) return js_mkerr(js, "eventType must be a string");
  
  EventType *evt = find_global_event_type(event_type);
  if (evt == NULL || evt->listener_count == 0) {
    return js_true;
  }
  
  jsval_t event_obj = js_mkobj(js);
  js_set(js, event_obj, "type", args[0]);
  js_set(js, event_obj, get_toStringTag_sym_key(), js_mkstr(js, "Event", 5));
  
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    js_set(js, event_obj, "detail", args[1]);
  }
  
  jsval_t listener_args[1] = {event_obj};
  int i = 0;
  while (i < evt->listener_count) {
    EventListener *listener = &evt->listeners[i];
    jsval_t result = js_call(js, listener->listener, listener_args, 1);
    
    if (vtype(result) == T_ERR) {
      fprintf(stderr, "Error in event listener for '%s': %s\n", event_type, js_str(js, result));
    }
    
    if (listener->once) {
      remove_listener_at(evt, i);
    } else i++;
  }
  
  return js_true;
}

static jsval_t js_get_event_listeners(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  
  EventType *evt, *tmp;
  jsval_t result = js_mkobj(js);
  
  HASH_ITER(hh, global_events, evt, tmp) {
    jsval_t listeners_array = js_mkobj(js);
    
    for (int j = 0; j < evt->listener_count; j++) {
      char key[16];
      snprintf(key, sizeof(key), "%d", j);
      
      jsval_t listener_info = js_mkobj(js);
      js_set(js, listener_info, "listener", evt->listeners[j].listener);
      js_set(js, listener_info, "once", js_bool(evt->listeners[j].once));
      
      js_set(js, listeners_array, key, listener_info);
    }
    
    js_set(js, listeners_array, "length", js_mknum(evt->listener_count));
    js_set(js, result, evt->event_type, listeners_array);
  }
  
  return result;
}

// EventEmitter.prototype.on(event, listener)
static jsval_t js_eventemitter_on(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "on requires 2 arguments (event, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "event must be a string");
  }
  
  if (vtype(args[1]) != T_FUNC) {
    return js_mkerr(js, "listener must be a function");
  }
  
  EventType *evt = find_or_create_emitter_event_type(js, this_obj, event_type);
  if (evt == NULL) {
    return js_mkerr(js, "failed to create event type");
  }
  
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum number of listeners for event '%s' reached", event_type);
  }
  
  EventListener *listener = &evt->listeners[evt->listener_count++];
  listener->listener = args[1];
  listener->once = false;
  
  return this_obj;
}

// EventEmitter.prototype.once(event, listener)
static jsval_t js_eventemitter_once(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "once requires 2 arguments (event, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "event must be a string");
  }
  
  if (vtype(args[1]) != T_FUNC) {
    return js_mkerr(js, "listener must be a function");
  }
  
  EventType *evt = find_or_create_emitter_event_type(js, this_obj, event_type);
  if (evt == NULL) {
    return js_mkerr(js, "failed to create event type");
  }
  
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum number of listeners for event '%s' reached", event_type);
  }
  
  EventListener *listener = &evt->listeners[evt->listener_count++];
  listener->listener = args[1];
  listener->once = true;
  
  return this_obj;
}

// EventEmitter.prototype.off(event, listener)
static jsval_t js_eventemitter_off(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "off requires 2 arguments (event, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "event must be a string");
  }
  
  EventType *evt = find_emitter_event_type(js, this_obj, event_type);
  if (evt == NULL) {
    return this_obj;
  }
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener == args[1]) {
      remove_listener_at(evt, i); break;
    }
  }
  
  return this_obj;
}

// EventEmitter.prototype.emit(event, ...args)
static jsval_t js_eventemitter_emit(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 1) {
    return js_mkerr(js, "emit requires at least 1 argument (event)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "event must be a string");
  }
  
  EventType *evt = find_emitter_event_type(js, this_obj, event_type);
  if (evt == NULL || evt->listener_count == 0) {
    return js_false;
  }
  
  int listener_nargs = nargs - 1;
  jsval_t *listener_args = (listener_nargs > 0) ? &args[1] : NULL;
  
  int i = 0;
  while (i < evt->listener_count) {
    EventListener *listener = &evt->listeners[i];
    jsval_t result = js_call(js, listener->listener, listener_args, listener_nargs);
    
    if (vtype(result) == T_ERR) {
      fprintf(stderr, "Error in event listener for '%s': %s\n", event_type, js_str(js, result));
    }
    
    if (listener->once) {
      remove_listener_at(evt, i);
    } else i++;
  }
  
  return js_true;
}

// EventEmitter.prototype.removeAllListeners(event)
static jsval_t js_eventemitter_removeAllListeners(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 1) {
    return this_obj;
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return this_obj;
  }
  
  EventType *evt = find_emitter_event_type(js, this_obj, event_type);
  if (evt != NULL) {
    evt->listener_count = 0;
  }
  
  return this_obj;
}

// EventEmitter.prototype.listenerCount(event)
static jsval_t js_eventemitter_listenerCount(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 1) {
    return js_mknum(0);
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mknum(0);
  }
  
  EventType *evt = find_emitter_event_type(js, this_obj, event_type);
  if (evt == NULL) {
    return js_mknum(0);
  }
  
  return js_mknum(evt->listener_count);
}

// EventEmitter.prototype.eventNames()
static jsval_t js_eventemitter_eventNames(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  
  jsval_t this_obj = js_getthis(js);
  jsval_t result = js_mkarr(js);
  
  EventType **events = get_or_create_emitter_events(js, this_obj);
  
  if (events != NULL && *events != NULL) {
    EventType *evt, *tmp;
    HASH_ITER(hh, *events, evt, tmp) {
      if (evt->listener_count > 0) {
        jsval_t name = js_mkstr(js, evt->event_type, strlen(evt->event_type));
        js_arr_push(js, result, name);
      }
    }
  }
  
  return result;
}

// EventEmitter constructor
static jsval_t js_eventemitter_constructor(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  
  jsval_t proto = js_get_ctor_proto(js, "EventEmitter", 12);
  jsval_t obj = js_mkobj(js);
  js_set_proto(js, obj, proto);
  
  return obj;
}

static jsval_t js_eventtarget_constructor(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  
  jsval_t proto = js_get_ctor_proto(js, "EventTarget", 11);
  jsval_t obj = js_mkobj(js);
  js_set_proto(js, obj, proto);
  
  return obj;
}

jsval_t events_library(struct js *js) {
  jsval_t lib = js_mkobj(js);
  
  jsval_t eventemitter_ctor = js_mkobj(js);
  jsval_t eventemitter_proto = js_mkobj(js);
  
  js_set(js, eventemitter_proto, "on", js_mkfun(js_eventemitter_on));
  js_set(js, eventemitter_proto, "addListener", js_mkfun(js_eventemitter_on));
  js_set(js, eventemitter_proto, "once", js_mkfun(js_eventemitter_once));
  js_set(js, eventemitter_proto, "off", js_mkfun(js_eventemitter_off));
  js_set(js, eventemitter_proto, "removeListener", js_mkfun(js_eventemitter_off));
  js_set(js, eventemitter_proto, "emit", js_mkfun(js_eventemitter_emit));
  js_set(js, eventemitter_proto, "removeAllListeners", js_mkfun(js_eventemitter_removeAllListeners));
  js_set(js, eventemitter_proto, "listenerCount", js_mkfun(js_eventemitter_listenerCount));
  js_set(js, eventemitter_proto, "eventNames", js_mkfun(js_eventemitter_eventNames));
  js_set(js, eventemitter_proto, get_toStringTag_sym_key(), js_mkstr(js, "EventEmitter", 12));
  
  js_set_slot(js, eventemitter_ctor, SLOT_CFUNC, js_mkfun(js_eventemitter_constructor));
  js_mkprop_fast(js, eventemitter_ctor, "prototype", 9, eventemitter_proto);
  js_mkprop_fast(js, eventemitter_ctor, "name", 4, ANT_STRING("EventEmitter"));
  js_set_descriptor(js, eventemitter_ctor, "name", 4, 0);
  
  js_set(js, lib, "EventEmitter", js_obj_to_func(eventemitter_ctor));
  js_set(js, lib, get_toStringTag_sym_key(), js_mkstr(js, "events", 6));
  
  return lib;
}

void init_events_module() {
  struct js *js = rt->js;
  jsval_t global = js_glob(js);
  
  jsval_t eventtarget_ctor = js_mkobj(js);
  jsval_t eventtarget_proto = js_mkobj(js);
  
  js_set(js, eventtarget_proto, "addEventListener", js_mkfun(js_add_event_listener_method));
  js_set(js, eventtarget_proto, "removeEventListener", js_mkfun(js_remove_event_listener_method));
  js_set(js, eventtarget_proto, "dispatchEvent", js_mkfun(js_dispatch_event_method));
  js_set(js, eventtarget_proto, get_toStringTag_sym_key(), js_mkstr(js, "EventTarget", 11));
  
  js_set_slot(js, eventtarget_ctor, SLOT_CFUNC, js_mkfun(js_eventtarget_constructor));
  js_mkprop_fast(js, eventtarget_ctor, "prototype", 9, eventtarget_proto);
  js_mkprop_fast(js, eventtarget_ctor, "name", 4, ANT_STRING("EventTarget"));
  js_set_descriptor(js, eventtarget_ctor, "name", 4, 0);
  
  js_set(js, global, "addEventListener", js_mkfun(js_add_event_listener));
  js_set(js, global, "removeEventListener", js_mkfun(js_remove_event_listener));
  js_set(js, global, "dispatchEvent", js_mkfun(js_dispatch_event));
  js_set(js, global, "getEventListeners", js_mkfun(js_get_event_listeners));
  js_set(js, global, "EventTarget", js_obj_to_func(eventtarget_ctor));
}

void events_gc_update_roots(void (*op_val)(void *, jsval_t *), void *ctx) {
  EventType *evt, *tmp;
  HASH_ITER(hh, global_events, evt, tmp) {
    for (int i = 0; i < evt->listener_count; i++) op_val(ctx, &evt->listeners[i].listener);
  }
}
