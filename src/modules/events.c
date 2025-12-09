#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "ant.h"
#include "runtime.h"
#include "modules/events.h"
#include "uthash.h"

#define MAX_LISTENERS_PER_EVENT 32

typedef struct {
  jsval_t listener;
  bool once;
} EventListener;

typedef struct {
  char *event_type;
  EventListener listeners[MAX_LISTENERS_PER_EVENT];
  int listener_count;
  UT_hash_handle hh;
} EventType;

typedef struct {
  uint64_t target_id;
  EventType *events;
  UT_hash_handle hh;
} TargetEvents;

static TargetEvents *target_events_map = NULL;

static TargetEvents *get_or_create_target_events(jsval_t target) {
  uint64_t target_id = target;
  TargetEvents *te = NULL;
  
  HASH_FIND(hh, target_events_map, &target_id, sizeof(uint64_t), te);
  
  if (te == NULL) {
    te = malloc(sizeof(TargetEvents));
    te->target_id = target_id;
    te->events = NULL;
    HASH_ADD(hh, target_events_map, target_id, sizeof(uint64_t), te);
  }
  
  return te;
}

static EventType *find_or_create_event_type(jsval_t target, const char *event_type) {
  TargetEvents *te = get_or_create_target_events(target);
  EventType *evt = NULL;
  
  HASH_FIND_STR(te->events, event_type, evt);
  
  if (evt == NULL) {
    evt = malloc(sizeof(EventType));
    evt->event_type = strdup(event_type);
    evt->listener_count = 0;
    HASH_ADD_KEYPTR(hh, te->events, evt->event_type, strlen(evt->event_type), evt);
  }
  
  return evt;
}

static EventType *find_event_type(jsval_t target, const char *event_type) {
  uint64_t target_id = target;
  TargetEvents *te = NULL;
  
  HASH_FIND(hh, target_events_map, &target_id, sizeof(uint64_t), te);
  
  if (te == NULL) {
    return NULL;
  }
  
  EventType *evt = NULL;
  HASH_FIND_STR(te->events, event_type, evt);
  
  return evt;
}

// addEventListener(eventType, listener, options)
static jsval_t js_add_event_listener_method(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "addEventListener requires at least 2 arguments (eventType, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "eventType must be a string");
  }
  
  if (js_type(args[1]) != JS_PRIV) {
    return js_mkerr(js, "listener must be a function");
  }
  
  EventType *evt = find_or_create_event_type(this_obj, event_type);
  if (evt == NULL) {
    return js_mkerr(js, "failed to create event type");
  }
  
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum number of listeners for event type '%s' reached", event_type);
  }
  
  bool once = false;
  if (nargs >= 3 && js_type(args[2]) != JS_UNDEF) {
    jsval_t once_val = js_get(js, args[2], "once");
    if (js_type(once_val) != JS_UNDEF) once = js_truthy(js, once_val);
  }
  
  EventListener *listener = &evt->listeners[evt->listener_count++];
  listener->listener = args[1];
  listener->once = once;
  
  return js_mkundef();
}

// addEventListener(eventType, listener, options)
static jsval_t js_add_event_listener(struct js *js, jsval_t *args, int nargs) {
  jsval_t global = js_glob(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "addEventListener requires at least 2 arguments (eventType, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "eventType must be a string");
  }
  
  if (js_type(args[1]) != JS_PRIV) {
    return js_mkerr(js, "listener must be a function");
  }
  
  EventType *evt = find_or_create_event_type(global, event_type);
  if (evt == NULL) {
    return js_mkerr(js, "failed to create event type");
  }
  
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum number of listeners for event type '%s' reached", event_type);
  }
  
  bool once = false;
  if (nargs >= 3 && js_type(args[2]) != JS_UNDEF) {
    jsval_t once_val = js_get(js, args[2], "once");
    if (js_type(once_val) != JS_UNDEF) once = js_truthy(js, once_val);
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
  if (event_type == NULL) {
    return js_mkerr(js, "eventType must be a string");
  }
  
  EventType *evt = find_event_type(this_obj, event_type);
  if (evt == NULL) {
    return js_mkundef();
  }
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener == args[1]) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      }
      evt->listener_count--;
      break;
    }
  }
  
  return js_mkundef();
}

// removeEventListener(eventType, listener)
static jsval_t js_remove_event_listener(struct js *js, jsval_t *args, int nargs) {
  jsval_t global = js_glob(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "removeEventListener requires 2 arguments (eventType, listener)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "eventType must be a string");
  }
  
  EventType *evt = find_event_type(global, event_type);
  if (evt == NULL) {
    return js_mkundef();
  }
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener == args[1]) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      }
      evt->listener_count--;
      break;
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
  if (event_type == NULL) {
    return js_mkerr(js, "eventType must be a string");
  }
  
  EventType *evt = find_event_type(this_obj, event_type);
  if (evt == NULL || evt->listener_count == 0) {
    return js_mktrue();
  }
  
  jsval_t event_obj = js_mkobj(js);
  js_set(js, event_obj, "type", args[0]);
  js_set(js, event_obj, "target", this_obj);
  js_set(js, event_obj, "@@toStringTag", js_mkstr(js, "Event", 5));
  
  if (nargs >= 2 && js_type(args[1]) != JS_UNDEF) {
    js_set(js, event_obj, "detail", args[1]);
  }
  
  jsval_t listener_args[1] = {event_obj};
  int i = 0;
  while (i < evt->listener_count) {
    EventListener *listener = &evt->listeners[i];
    jsval_t result = js_call(js, listener->listener, listener_args, 1);
    
    if (js_type(result) == JS_ERR) {
      fprintf(stderr, "Error in event listener for '%s': %s\n", event_type, js_str(js, result));
    }
    
    if (listener->once) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      }
      evt->listener_count--;
    } else {
      i++;
    }
  }
  
  return js_mktrue();
}

// dispatchEvent(eventType, eventData)
static jsval_t js_dispatch_event(struct js *js, jsval_t *args, int nargs) {
  jsval_t global = js_glob(js);
  
  if (nargs < 1) {
    return js_mkerr(js, "dispatchEvent requires at least 1 argument (eventType)");
  }
  
  char *event_type = js_getstr(js, args[0], NULL);
  if (event_type == NULL) {
    return js_mkerr(js, "eventType must be a string");
  }
  
  EventType *evt = find_event_type(global, event_type);
  if (evt == NULL || evt->listener_count == 0) {
    return js_mktrue();
  }
  
  jsval_t event_obj = js_mkobj(js);
  js_set(js, event_obj, "type", args[0]);
  js_set(js, event_obj, "@@toStringTag", js_mkstr(js, "Event", 5));
  
  if (nargs >= 2 && js_type(args[1]) != JS_UNDEF) {
    js_set(js, event_obj, "detail", args[1]);
  }
  
  jsval_t listener_args[1] = {event_obj};
  int i = 0;
  while (i < evt->listener_count) {
    EventListener *listener = &evt->listeners[i];
    jsval_t result = js_call(js, listener->listener, listener_args, 1);
    
    if (js_type(result) == JS_ERR) {
      fprintf(stderr, "Error in event listener for '%s': %s\n", event_type, js_str(js, result));
    }
    
    if (listener->once) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      }
      evt->listener_count--;
    } else {
      i++;
    }
  }
  
  return js_mktrue();
}

static jsval_t js_get_event_listeners(struct js *js, jsval_t *args, int nargs) {
  jsval_t target = (nargs > 0) ? args[0] : js_glob(js);
  
  EventType *evt, *tmp;
  jsval_t result = js_mkobj(js);
  
  uint64_t target_id = target;
  TargetEvents *te = NULL;
  
  HASH_FIND(hh, target_events_map, &target_id, sizeof(uint64_t), te);
  
  if (te == NULL) {
    return result;
  }
  
  HASH_ITER(hh, te->events, evt, tmp) {
    jsval_t listeners_array = js_mkobj(js);
    
    for (int j = 0; j < evt->listener_count; j++) {
      char key[16];
      snprintf(key, sizeof(key), "%d", j);
      
      jsval_t listener_info = js_mkobj(js);
      js_set(js, listener_info, "listener", evt->listeners[j].listener);
      js_set(js, listener_info, "once", evt->listeners[j].once ? js_mktrue() : js_mkfalse());
      
      js_set(js, listeners_array, key, listener_info);
    }
    
    js_set(js, listeners_array, "length", js_mknum(evt->listener_count));
    js_set(js, result, evt->event_type, listeners_array);
  }
  
  return result;
}

void init_events_module() {
  struct js *js = rt->js;
  jsval_t global = js_glob(js);
  
  js_set(js, global, "addEventListener", js_mkfun(js_add_event_listener));
  js_set(js, global, "removeEventListener", js_mkfun(js_remove_event_listener));
  js_set(js, global, "dispatchEvent", js_mkfun(js_dispatch_event));
  js_set(js, global, "getEventListeners", js_mkfun(js_get_event_listeners));
  
  jsval_t event_target_proto = js_mkobj(js);
  js_set(js, event_target_proto, "addEventListener", js_mkfun(js_add_event_listener_method));
  js_set(js, event_target_proto, "removeEventListener", js_mkfun(js_remove_event_listener_method));
  js_set(js, event_target_proto, "dispatchEvent", js_mkfun(js_dispatch_event_method));
  js_set(js, event_target_proto, "@@toStringTag", js_mkstr(js, "EventTarget", 11));
  
  js_set(js, global, "EventTargetPrototype", event_target_proto);
}


