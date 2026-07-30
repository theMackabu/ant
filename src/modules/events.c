#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <utarray.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
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
static ant_value_t g_eventemitter_ctor           = 0;
static ant_value_t g_eventemitter_proto          = 0;
static ant_value_t g_eventtarget_proto           = 0;
static ant_value_t g_event_proto                 = 0;
static ant_value_t g_customevent_proto           = 0;
static ant_value_t g_errorevent_proto            = 0;
static ant_value_t g_promiserejectionevent_proto = 0;

enum {
  EVENT_NATIVE_TAG = 0x45564e54u,        // EVNT
  EVENT_EMITTER_NATIVE_TAG = 0x45454d54u // EEMT
};

static event_data_t *get_event_data(ant_value_t obj) {
  return (event_data_t *)js_get_native(obj, EVENT_NATIVE_TAG);
}

static double get_timestamp_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct EventListenerCold {
  ant_value_t raw_callback;
  ant_value_t signal;
  uint32_t id;
  bool capture;
  bool pending_prepend;
  struct EventListenerCold *next;
} EventListenerCold;

typedef struct {
  ant_value_t callback;
  uint32_t dead_gen;
  uint32_t meta;
} EventListenerEntry;

static_assert(
  sizeof(EventListenerEntry) == 16,
  "EventListenerEntry must stay dispatch-cache dense"
);

enum {
  ENTRY_ONCE = 1u << 0,
  ENTRY_CONSUMED = 1u << 1,
  ENTRY_HAS_COLD = 1u << 2,
  ENTRY_FLAG_MASK = 7u,
  ENTRY_ID_SHIFT = 3,
};

static const UT_icd event_listener_icd = { 
  sizeof(EventListenerEntry),
  NULL, NULL, NULL
};

// TODO: the global EventTarget listener table holds isolate-heap values, so it
// lives on the isolate like process_state; allocated on first touch
// -- START ---
typedef struct EventTypeList EventTypeList;

typedef struct {
  UT_array *listeners;
  ant_value_t js_key;
  const char *key_bytes;
  size_t key_len;
  EventTypeList *owner;
  uint32_t generation;
  unsigned int dead_count;
  int emitting;
  EventListenerCold *cold_entries;
  uint32_t next_listener_id;
  bool needs_reorder;
  bool warned_max_listeners;
} EventType;

struct EventTypeList {
  EventType **types;
  unsigned int count;
  unsigned int cap;
  ant_value_t target;
  eventemitter_listener_change_fn listener_change_hook;
  void *listener_change_context;
};

struct ant_events_state {
  EventTypeList global_events;
};
// --- END: TO BE MIGRATED ---

static EventTypeList *global_events_list(ant_t *js) {
  if (!js->events_state) js->events_state = ant_calloc(sizeof(*js->events_state));
  return js->events_state ? &js->events_state->global_events : NULL;
}

static void evt_list_free(EventTypeList *list) {
  if (!list) return;

  for (unsigned int i = 0; i < list->count; i++) {
    EventListenerCold *cold = list->types[i]->cold_entries;
    while (cold) {
      EventListenerCold *next = cold->next;
      free(cold);
      cold = next;
    }
    if (list->types[i]->listeners) utarray_free(list->types[i]->listeners);
    free(list->types[i]);
  }

  free(list->types);
  list->types = NULL;
  list->count = 0;
  list->cap = 0;
}

static EventType *make_event_type(ant_t *js, ant_value_t js_key) {
  EventType *evt = ant_calloc(sizeof(EventType));
  if (!evt) return NULL;
  evt->js_key = js_key;
  if (vtype(js_key) == T_STR) evt->key_bytes = js_getstr(js, js_key, &evt->key_len);
  utarray_new(evt->listeners, &event_listener_icd);
  return evt;
}

static inline bool entry_live(const EventListenerEntry *e) {
  return e->dead_gen == 0;
}

static inline bool entry_once(const EventListenerEntry *entry) {
  return (entry->meta & ENTRY_ONCE) != 0;
}

static inline bool entry_consumed(const EventListenerEntry *entry) {
  return (entry->meta & ENTRY_CONSUMED) != 0;
}

static inline uint32_t entry_id(const EventListenerEntry *entry) {
  return entry->meta >> ENTRY_ID_SHIFT;
}

static EventListenerCold *entry_cold(
  const EventType *evt,
  const EventListenerEntry *entry
) {
  uint32_t id = entry_id(entry);
  if ((entry->meta & ENTRY_HAS_COLD) == 0 || id == 0) return NULL;
  for (EventListenerCold *cold = evt->cold_entries; cold; cold = cold->next)
    if (cold->id == id) return cold;
  return NULL;
}

static EventListenerCold *entry_ensure_cold(
  EventType *evt,
  EventListenerEntry *entry
) {
  EventListenerCold *cold = entry_cold(evt, entry);
  const uint32_t max_id = UINT32_MAX >> ENTRY_ID_SHIFT;
  if (cold) return cold;

  cold = ant_calloc(sizeof(*cold));
  if (!cold) return NULL;

  do {
    bool collision = false;
    evt->next_listener_id = evt->next_listener_id >= max_id
      ? 1
      : evt->next_listener_id + 1;
    for (EventListenerCold *it = evt->cold_entries; it; it = it->next) {
      if (it->id != evt->next_listener_id) continue;
      collision = true;
      break;
    }
    if (!collision) break;
  } while (true);

  cold->id = evt->next_listener_id;
  cold->raw_callback = js_mkundef();
  cold->signal = js_mkundef();
  cold->next = evt->cold_entries;
  evt->cold_entries = cold;
  entry->meta = (cold->id << ENTRY_ID_SHIFT)
    | (entry->meta & ENTRY_FLAG_MASK)
    | ENTRY_HAS_COLD;
  return cold;
}

static void entry_remove_cold(EventType *evt, EventListenerEntry *entry) {
  EventListenerCold **cursor = &evt->cold_entries;
  uint32_t id = entry_id(entry);
  if ((entry->meta & ENTRY_HAS_COLD) == 0 || id == 0) return;

  while (*cursor) {
    if ((*cursor)->id == id) {
      EventListenerCold *cold = *cursor;
      *cursor = cold->next;
      free(cold);
      break;
    }
    cursor = &(*cursor)->next;
  }

  entry->meta &= ENTRY_ONCE | ENTRY_CONSUMED;
}

static ant_value_t entry_raw_callback(
  const EventType *evt,
  const EventListenerEntry *entry
) {
  EventListenerCold *cold = entry_cold(evt, entry);
  return cold ? cold->raw_callback : js_mkundef();
}

static ant_value_t entry_signal(
  const EventType *evt,
  const EventListenerEntry *entry
) {
  EventListenerCold *cold = entry_cold(evt, entry);
  return cold ? cold->signal : js_mkundef();
}

static bool entry_capture(
  const EventType *evt,
  const EventListenerEntry *entry
) {
  EventListenerCold *cold = entry_cold(evt, entry);
  return cold && cold->capture;
}

static bool entry_pending_prepend(
  const EventType *evt,
  const EventListenerEntry *entry
) {
  EventListenerCold *cold = entry_cold(evt, entry);
  return cold && cold->pending_prepend;
}

static unsigned int evt_live_count(EventType *evt);

static void evt_notify_listener_change(ant_t *js, EventType *evt) {
  EventTypeList *list = evt ? evt->owner : NULL;
  if (!list || !list->listener_change_hook) return;
  list->listener_change_hook(
    js,
    list->target,
    evt->js_key,
    (ant_offset_t)evt_live_count(evt),
    list->listener_change_context
  );
}

static void evt_release_if_empty(EventType *evt) {
  EventTypeList *list = evt->owner;

  if (!list || evt->emitting != 0 || utarray_len(evt->listeners) != 0) return;

  for (unsigned int i = 0; i < list->count; i++) {
    if (list->types[i] != evt) continue;
    list->types[i] = list->types[--list->count];
    break;
  }

  utarray_free(evt->listeners);
  free(evt);
}

static void evt_retire(ant_t *js, EventType *evt, unsigned int index) {
  EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, index);
  if (!e || !entry_live(e)) return;

  if (evt->emitting == 0) {
    entry_remove_cold(evt, e);
    utarray_erase(evt->listeners, index, 1);
    evt_notify_listener_change(js, evt);
    evt_release_if_empty(evt);
    return;
  }

  e->dead_gen = evt->generation;
  evt->dead_count++;
  evt_notify_listener_change(js, evt);
}

static void evt_consume(ant_t *js, EventType *evt, unsigned int index) {
  EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, index);
  if (!e) return;
  e->meta |= ENTRY_CONSUMED;
  evt_retire(js, evt, index);
}

static void evt_retire_all(ant_t *js, EventType *evt) {
  if (evt->emitting == 0) {
    while (evt->cold_entries) {
      EventListenerCold *next = evt->cold_entries->next;
      free(evt->cold_entries);
      evt->cold_entries = next;
    }
    utarray_clear(evt->listeners);
    evt_notify_listener_change(js, evt);
    evt_release_if_empty(evt);
    return;
  }
  for (unsigned int i = utarray_len(evt->listeners); i-- > 0;)
    evt_retire(js, evt, i);
}

static void evt_sweep(EventType *evt) {
  if (evt->emitting != 0) return;

  if (evt->needs_reorder) {
    for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
      EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
      if (!entry_pending_prepend(evt, e)) continue;

      EventListenerEntry moved = *e;
      entry_cold(evt, &moved)->pending_prepend = false;
      utarray_erase(evt->listeners, i, 1);
      utarray_insert(evt->listeners, &moved, 0);
    }
    evt->needs_reorder = false;
  }

  evt->generation = 0;
  if (evt->dead_count == 0) {
    evt_release_if_empty(evt);
    return;
  }
  {
    unsigned int n = utarray_len(evt->listeners), w = 0;
    EventListenerEntry *a = (EventListenerEntry *)utarray_front(evt->listeners);

    for (unsigned int r = 0; r < n; r++) {
      if (!entry_live(&a[r])) {
        entry_remove_cold(evt, &a[r]);
        continue;
      }
      if (w != r) {
        a[w] = a[r];
      }
      w++;
    }
    utarray_resize(evt->listeners, (int)w);
  }

  evt->dead_count = 0;
  evt_release_if_empty(evt);
}

static unsigned int evt_live_count(EventType *evt) {
  unsigned int live = 0;
  if (!evt) return 0;
  if (evt->dead_count == 0) return utarray_len(evt->listeners);

  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++)
    if (entry_live((EventListenerEntry *)utarray_eltptr(evt->listeners, i))) live++;

  return live;
}

static EventType *evt_list_find_cstr(EventTypeList *list, const char *name, size_t len) {
  for (unsigned int i = 0; i < list->count; i++) {
    EventType *evt = list->types[i];
    if (evt->key_len != len || !evt->key_bytes) continue;
    if (memcmp(evt->key_bytes, name, len) == 0) return evt;
  }
  return NULL;
}

static EventType *evt_list_find(ant_t *js, EventTypeList *list, ant_value_t js_key) {
  const char *probe = NULL;
  size_t probe_len = 0;

  if (vtype(js_key) == T_STR) probe = js_getstr(js, js_key, &probe_len);

  for (unsigned int i = 0; i < list->count; i++) {
    EventType *evt = list->types[i];
    if (evt->js_key == js_key) return evt;
    if (!probe || !evt->key_bytes || evt->key_len != probe_len) continue;
    if (memcmp(evt->key_bytes, probe, probe_len) == 0) return evt;
  }
  return NULL;
}

static EventType *evt_list_find_or_create(ant_t *js, EventTypeList *list, ant_value_t js_key) {
  EventType *evt = evt_list_find(js, list, js_key);
  if (evt) return evt;

  if (list->count == list->cap) {
    unsigned int cap = list->cap ? list->cap * 2 : 4;
    EventType **next = realloc(list->types, cap * sizeof(*next));
    if (!next) return NULL;
    list->types = next;
    list->cap = cap;
  }

  evt = make_event_type(js, js_key);
  if (!evt) return NULL;

  evt->owner = list;
  list->types[list->count++] = evt;
  return evt;
}

static EventTypeList *find_emitter_events(ant_value_t this_obj) {
  return (EventTypeList *)js_get_native(this_obj, EVENT_EMITTER_NATIVE_TAG);
}

static EventTypeList *get_or_create_emitter_events(ant_t *js, ant_value_t this_obj) {
  EventTypeList *events = find_emitter_events(this_obj);
  if (!events) {
    events = ant_calloc(sizeof(*events));
    if (!events) return NULL;
    events->target = this_obj;
    js_set_native(this_obj, events, EVENT_EMITTER_NATIVE_TAG);
  }
  return events;
}

static EventType *find_or_create_global_event_type(ant_t *js, ant_value_t js_key) {
  if (vtype(js_key) != T_STR && vtype(js_key) != T_SYMBOL) return NULL;
  EventTypeList *list = global_events_list(js);
  return list ? evt_list_find_or_create(js, list, js_key) : NULL;
}

static EventType *find_global_event_type(ant_t *js, ant_value_t js_key) {
  EventTypeList *list = global_events_list(js);
  return list ? evt_list_find(js, list, js_key) : NULL;
}

static EventType *find_or_create_emitter_event_type(ant_t *js, ant_value_t this_obj, ant_value_t js_key) {
  EventTypeList *events = NULL;

  if (vtype(js_key) != T_STR && vtype(js_key) != T_SYMBOL) return NULL;
  events = get_or_create_emitter_events(js, this_obj);

  return events ? evt_list_find_or_create(js, events, js_key) : NULL;
}

static EventType *find_emitter_event_type_cstr(ant_t *js, ant_value_t this_obj, const char *name, size_t len) {
  EventTypeList *events = find_emitter_events(this_obj);
  if (!events) return NULL;
  return evt_list_find_cstr(events, name, len);
}

static EventType *find_emitter_event_type(ant_t *js, ant_value_t this_obj, ant_value_t js_key) {
  EventTypeList *events = find_emitter_events(this_obj);
  if (!events) return NULL;
  return evt_list_find(js, events, js_key);
}

static inline ant_value_t evt_key_from_arg(ant_value_t arg) {
  uint8_t t = vtype(arg);
  return (t == T_STR || t == T_SYMBOL) ? arg : 0;
}

static bool is_eventemitter_instance(ant_value_t target) {
  return js_check_brand(target, BRAND_EVENTEMITTER);
}

static bool is_eventtarget_instance(ant_value_t target) {
  return js_check_brand(target, BRAND_EVENTTARGET);
}

static int eventemitter_get_max_listeners_impl(ant_value_t target) {
  ant_value_t slot = js_get_slot(target, SLOT_EVENT_MAX_LISTENERS);
  if (vtype(slot) == T_NUM) {
    int n = (int)js_getnum(slot);
    return n >= 0 ? n : EVENTS_DEFAULT_MAX_LISTENERS;
  }
  return EVENTS_DEFAULT_MAX_LISTENERS;
}

static ant_value_t eventemitter_call_listener(
  ant_t *js,
  ant_value_t listener,
  ant_value_t this_val,
  ant_value_t *args,
  int nargs
) {
  if (!is_callable(listener)) return js_mkundef();
  if (sv_check_c_stack_overflow(js))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");

  sv_call_plan_t plan;
  ant_value_t err = sv_prepare_call(
    js->vm, js, listener, this_val, args, nargs, 
    NULL, SV_CALL_MODE_NORMAL, &plan
  );
  
  if (is_err(err)) return err;
  return sv_execute_call_plan(js->vm, js, &plan, NULL);
}

static ant_value_t js_eventemitter_once_wrapper(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t listener = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_callable(listener)) return js_mkundef();
  return eventemitter_call_listener(js, listener, js->this_val, args, nargs);
}

static ant_value_t event_type_get_listeners_array(ant_t *js, EventType *evt, bool raw) {
  ant_value_t result = js_mkarr(js);
  if (!evt) return result;

  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
    EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    if (!entry || !entry_live(entry)) continue;
    
    js_arr_push(js, result, raw 
      && entry_once(entry)
      && is_callable(entry_raw_callback(evt, entry))
      ? entry_raw_callback(evt, entry) : entry->callback
    );
  }

  return result;
}

static ant_value_t eventemitter_get_listeners_array(ant_t *js, ant_value_t target, ant_value_t key, bool raw) {
  if (!is_object_type(target) || !key) return js_mkarr(js);
  return event_type_get_listeners_array(js, find_emitter_event_type(js, target, key), raw);
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
  if (data) js_set_native(obj, data, EVENT_NATIVE_TAG);
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

static ant_value_t js_eventemitter_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  
  if (is_object_type(this_obj)) {
    js_set_slot(this_obj, SLOT_BRAND, js_mknum(BRAND_EVENTEMITTER));
    return this_obj;
  }

  if (vtype(js->new_target) != T_UNDEF) {
    ant_value_t obj = js_mkobj(js);
    ant_value_t proto = js_instance_proto_from_new_target(js, g_eventemitter_proto);
    if (is_object_type(proto)) js_set_proto_init(obj, proto);
    js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_EVENTEMITTER));
    return obj;
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "EventEmitter constructor requires an object receiver or 'new'");
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

static ant_value_t js_eventtarget_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "EventTarget constructor requires 'new'");

  ant_value_t this_obj = js_getthis(js);
  if (is_object_type(this_obj)) js_set_slot(this_obj, SLOT_BRAND, js_mknum(BRAND_EVENTTARGET));
  return js_mkundef();
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
    if (!abort_signal_is_signal(sig)) return false;
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
    if (entry_live(e) && e->callback == cb && entry_capture(evt, e) == capture)
      return js_mkundef();
  }

  EventListenerEntry entry = {
    .callback = cb,
    .dead_gen = 0,
    .meta = once ? ENTRY_ONCE : 0,
  };
  if (once || capture || vtype(signal) != T_UNDEF) {
    EventListenerCold *cold = entry_ensure_cold(evt, &entry);
    if (!cold) return js_mkerr(js, "out of memory");
    cold->signal = signal;
    cold->capture = capture;
  }
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
    if (entry_live(e) && e->callback == cb && entry_capture(evt, e) == capture) {
      evt_retire(js, evt, i);
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
  evt->generation++;

  evt->emitting++;
  for (unsigned int i = 0; i < n; i++) {
    EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    ant_value_t cb = entry->callback;
    ant_value_t signal = entry_signal(evt, entry);

    if (!entry_live(entry)) continue;
    if (entry_once(entry)) evt_consume(js, evt, i);

    if (vtype(signal) != T_UNDEF && abort_signal_is_aborted(signal)) {
      evt_retire(js, evt, i);
      continue;
    }

    uint8_t t = vtype(cb);
    if (t != T_FUNC && t != T_CFUNC) continue;

    eventemitter_call_listener(js, cb, js_mkundef(), call_args, 1);
    if (data && data->stop_immediate) break;
  }
  
  evt->emitting--;
  evt_sweep(evt);

  if (data) data->dispatching = false;
  js_set(js, event_obj, "currentTarget", js_mknull());
  js_set(js, event_obj, "eventPhase",    js_mknum(0));

  bool canceled = data && data->canceled;
  return js_bool(!canceled);
}

static ant_value_t js_add_event_listener_method(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkundef();
  return add_listener_to(js, args, nargs,
    find_or_create_emitter_event_type(js, js_getthis(js), key));
}

static ant_value_t js_add_event_listener(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkundef();
  return add_listener_to(js, args, nargs, find_or_create_global_event_type(js, key));
}

static ant_value_t js_remove_event_listener_method(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkundef();
  return remove_listener_from(js, args, nargs,
    find_emitter_event_type(js, js_getthis(js), key));
}

static ant_value_t js_remove_event_listener(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkundef();
  return remove_listener_from(js, args, nargs, find_global_event_type(js, key));
}

static ant_value_t js_dispatch_event_method(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  ant_value_t this_obj = js_getthis(js);
  ant_value_t key = js_get(js, args[0], "type");
  if (!evt_key_from_arg(key)) return js_false;
  return dispatch_event_to(js, args[0],
    find_emitter_event_type(js, this_obj, key), this_obj);
}

static ant_value_t js_dispatch_event(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  ant_value_t key = js_get(js, args[0], "type");
  if (!evt_key_from_arg(key)) return js_false;
  return dispatch_event_to(js, args[0],
    find_global_event_type(js, key), js_glob(js));
}

void js_dispatch_global_event(ant_t *js, ant_value_t event_obj) {
  ant_value_t key = js_get(js, event_obj, "type");
  if (!evt_key_from_arg(key)) return;
  EventType *evt = find_global_event_type(js, key);
  if (evt_live_count(evt) == 0) return;
  dispatch_event_to(js, event_obj, evt, js_glob(js));
}

static bool eventemitter_add_listener_impl(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t listener, bool once, bool prepend
) {
  EventType *evt = NULL;
  EventListenerEntry entry = {0};
  uint8_t t = 0;

  if (!is_object_type(target) || !key) return false;
  t = vtype(listener);
  if (t != T_FUNC && t != T_CFUNC) return false;

  evt = find_or_create_emitter_event_type(js, target, key);
  if (!evt) return false;

  entry.callback = listener;
  entry.dead_gen = 0;
  entry.meta = once ? ENTRY_ONCE : 0;

  if (once) {
    EventListenerCold *cold = entry_ensure_cold(evt, &entry);
    if (!cold) return false;
    cold->raw_callback = js_heavy_mkfun(js, js_eventemitter_once_wrapper, listener);
    if (is_callable(cold->raw_callback))
      js_set(js, cold->raw_callback, "listener", listener);
  }

  if (!prepend || utarray_len(evt->listeners) == 0) utarray_push_back(evt->listeners, &entry);
  else if (evt->emitting > 0) {
    EventListenerCold *cold = entry_ensure_cold(evt, &entry);
    if (!cold) {
      entry_remove_cold(evt, &entry);
      return false;
    }
    cold->pending_prepend = true;
    evt->needs_reorder = true;
    utarray_push_back(evt->listeners, &entry);
  } else utarray_insert(evt->listeners, &entry, 0);

  int max_listeners = eventemitter_get_max_listeners_impl(target);
  unsigned int live = evt_live_count(evt);
  if (
    max_listeners > 0 &&
    !evt->warned_max_listeners &&
    (int)live > max_listeners
  ) {
    evt->warned_max_listeners = true;
    if (vtype(key) == T_STR) {
      fprintf(
        stderr,
        "Warning: Possible EventEmitter memory leak detected. "
        "%u %s listeners added. Use emitter.setMaxListeners() to increase limit.\n",
        live,
        js_str(js, key)
      );
    } else {
      fprintf(
        stderr,
        "Warning: Possible EventEmitter memory leak detected. "
        "%u listeners added for a Symbol event. Use emitter.setMaxListeners() to increase limit.\n",
        live
      );
    }
  }

  evt_notify_listener_change(js, evt);
  return true;
}

static bool eventemitter_remove_listener_impl(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t listener
) {
  EventType *evt = NULL;
  uint8_t t = 0;

  if (!is_object_type(target) || !key) return false;
  t = vtype(listener);
  if (t != T_FUNC && t != T_CFUNC) return false;

  evt = find_emitter_event_type(js, target, key);
  if (!evt) return false;

  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
  EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
  if (
    entry_live(entry) &&
    (
      entry->callback == listener ||
      entry_raw_callback(evt, entry) == listener
    )
  ) {
    evt_retire(js, evt, i);
    return true;
  }}

  return false;
}

static ant_offset_t eventemitter_listener_count_impl(
  ant_t *js,
  ant_value_t target, ant_value_t key
) {
  EventType *evt = NULL;

  if (!is_object_type(target) || !key) return 0;
  evt = find_emitter_event_type(js, target, key);
  if (!evt) return 0;

  return (ant_offset_t)evt_live_count(evt);
}

static ant_value_t js_eventemitter_off(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "off requires 2 arguments (event, listener)");
  ant_value_t key = evt_key_from_arg(args[0]);
  
  if (!key) return js_mkerr(js, "event must be a string or Symbol");
  ant_value_t this_obj = js_getthis(js);
  
  remove_listener_from(
    js, args, nargs, 
    find_emitter_event_type(js, this_obj, key)
  );
  
  return this_obj;
}

static bool eventemitter_dispatch(
  ant_t *js,
  ant_value_t target, EventType *evt,
  ant_value_t *args, int nargs
) {
  unsigned int count = 0;
  uint32_t gen = 0;
  bool invoked = false;

  if (!evt || utarray_len(evt->listeners) == 0) return false;

  count = utarray_len(evt->listeners);
  gen = ++evt->generation;
  evt->emitting++;

  for (unsigned int i = 0; i < count; i++) {
    EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    ant_value_t cb = entry->callback;
    ant_value_t signal = entry_signal(evt, entry);

    if (entry_consumed(entry)) continue;
    if (entry->dead_gen != 0 && entry->dead_gen < gen) continue;
    if (entry_once(entry)) evt_consume(js, evt, i);

    if (vtype(signal) != T_UNDEF && abort_signal_is_aborted(signal)) {
      evt_retire(js, evt, i);
      continue;
    }

    if (vtype(cb) != T_FUNC && vtype(cb) != T_CFUNC) continue;
    ant_value_t result = eventemitter_call_listener(js, cb, target, args, nargs);
    invoked = true;

    if (vtype(result) == T_ERR) {
      if (vtype(evt->js_key) == T_STR) fprintf(stderr, "Error in event listener for %s: ", js_str(js, evt->js_key));
      else fprintf(stderr, "Error in event listener: ");
      fprintf(stderr, "%s\n", js_str(js, result));
    }
  }

  evt->emitting--;
  evt_sweep(evt);
  return invoked;
}

static bool eventemitter_emit_args_impl(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t *args, int nargs
) {
  if (!is_object_type(target) || !key) return false;
  return eventemitter_dispatch(js, target, find_emitter_event_type(js, target, key), args, nargs);
}

static ant_value_t js_eventemitter_emit(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "emit requires at least 1 argument (event)");
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkerr(js, "event must be a string or Symbol");
  
  return js_bool(eventemitter_emit_args_impl(
    js, js_getthis(js), key,
    nargs > 1 ? &args[1] : NULL, nargs - 1
  ));
}

bool eventemitter_emit_args_val(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t *args, int nargs
) {
  return eventemitter_emit_args_impl(js, target, key, args, nargs);
}

bool eventemitter_emit_args(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t *args, int nargs
) {
  if (!is_object_type(target) || !event_type) return false;
  return eventemitter_dispatch(
    js, target,
    find_emitter_event_type_cstr(js, target, event_type, strlen(event_type)),
    args, nargs
  );
}

bool eventemitter_add_listener_val(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t listener, bool once
) {
  return eventemitter_add_listener_impl(js, target, key, listener, once, false);
}

bool eventemitter_set_listener_change_hook(
  ant_t *js,
  ant_value_t target,
  eventemitter_listener_change_fn hook,
  void *context
) {
  EventTypeList *events = NULL;
  if (!is_object_type(target)) return false;
  events = get_or_create_emitter_events(js, target);
  if (!events) return false;
  events->listener_change_hook = hook;
  events->listener_change_context = context;
  return true;
}

bool eventemitter_add_listener(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t listener, bool once
) {
  return eventemitter_add_listener_val(js, target, js_mkstr(js, event_type, strlen(event_type)), listener, once);
}

bool eventemitter_remove_listener_val(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t listener
) {
  return eventemitter_remove_listener_impl(js, target, key, listener);
}

bool eventemitter_remove_listener(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t listener
) {
  EventTypeList *events = NULL;
  EventType *evt = NULL;

  if (!is_object_type(target) || !event_type) return false;
  events = find_emitter_events(target);
  if (!events) return false;

  evt = evt_list_find_cstr(events, event_type, strlen(event_type));
  if (!evt) return false;

  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
    EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    if (!entry_live(e)) continue;
    if (
      e->callback != listener &&
      entry_raw_callback(evt, e) != listener
    ) continue;
    evt_retire(js, evt, i);
    return true;
  }

  return false;
}

ant_offset_t eventemitter_listener_count_val(
  ant_t *js,
  ant_value_t target, ant_value_t key
) {
  return eventemitter_listener_count_impl(js, target, key);
}

ant_offset_t eventemitter_listener_count(
  ant_t *js,
  ant_value_t target, const char *event_type
) {
  EventTypeList *events = NULL;

  if (!is_object_type(target) || !event_type) return 0;
  events = find_emitter_events(target);
  if (!events) return 0;

  return (ant_offset_t)evt_live_count(evt_list_find_cstr(events, event_type, strlen(event_type)));
}

static ant_value_t js_eventemitter_add(ant_t *js, ant_value_t *args, int nargs, bool once, bool prepend) {
  if (nargs < 2) return js_mkerr(js, "requires 2 arguments (event, listener)");
  ant_value_t key = evt_key_from_arg(args[0]);
  
  if (!key) return js_mkerr(js, "event must be a string or Symbol");
  if (!eventemitter_add_listener_impl(js, js_getthis(js), key, args[1], once, prepend))
    return js_mkerr(js, "listener must be a function");
    
  return js_getthis(js);
}

static ant_value_t js_eventemitter_on(ant_t *js, ant_value_t *args, int nargs) {
  return js_eventemitter_add(js, args, nargs, false, false);
}

static ant_value_t js_eventemitter_once(ant_t *js, ant_value_t *args, int nargs) {
  return js_eventemitter_add(js, args, nargs, true, false);
}

static ant_value_t js_eventemitter_prepend_listener(ant_t *js, ant_value_t *args, int nargs) {
  return js_eventemitter_add(js, args, nargs, false, true);
}

static ant_value_t js_eventemitter_prepend_once_listener(ant_t *js, ant_value_t *args, int nargs) {
  return js_eventemitter_add(js, args, nargs, true, true);
}

static ant_value_t js_eventemitter_removeAllListeners(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  EventTypeList *events = NULL;

  if (nargs < 1 || vtype(args[0]) == T_UNDEF) {
    events = find_emitter_events(this_obj);
    if (!events) return this_obj;

    for (unsigned int i = events->count; i-- > 0;)
      evt_retire_all(js, events->types[i]);
    return this_obj;
  }

  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return this_obj;

  EventType *evt = find_emitter_event_type(js, this_obj, key);
  if (evt) evt_retire_all(js, evt);
  
  return this_obj;
}

static ant_value_t js_eventemitter_listenerCount(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mknum(0);
  ant_value_t key = evt_key_from_arg(args[0]);
  
  if (!key) return js_mknum(0);
  EventType *evt = find_emitter_event_type(js, js_getthis(js), key);
  
  if (!evt) return js_mknum(0);
  return js_mknum((double)evt_live_count(evt));
}

static ant_value_t js_eventemitter_setMaxListeners(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "setMaxListeners requires 1 argument");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "n must be a number");

  int n = (int)js_getnum(args[0]);
  if (n < 0) return js_mkerr(js, "n must be non-negative");

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "setMaxListeners requires an object receiver");
  js_set_slot(this_obj, SLOT_EVENT_MAX_LISTENERS, js_mknum(n));
  return this_obj;
}

static ant_value_t js_eventemitter_getMaxListeners(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) return js_mknum(EVENTS_DEFAULT_MAX_LISTENERS);
  return js_mknum(eventemitter_get_max_listeners_impl(this_obj));
}

static ant_value_t js_eventemitter_rawListeners(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkarr(js);
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkarr(js);
  return eventemitter_get_listeners_array(js, js_getthis(js), key, true);
}

static ant_value_t js_eventemitter_listeners(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkarr(js);
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkarr(js);
  return eventemitter_get_listeners_array(js, js_getthis(js), key, false);
}

static ant_value_t js_eventemitter_eventNames(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t result = js_mkarr(js);
  
  EventTypeList *events = find_emitter_events(this_obj);
  if (events) {
    for (unsigned int i = 0; i < events->count; i++)
      if (evt_live_count(events->types[i]) > 0) js_arr_push(js, result, events->types[i]->js_key);
  }
  
  return result;
}

static ant_value_t js_events_once_listener(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t promise = js_get_slot(state, SLOT_DATA);
  if (vtype(promise) != T_PROMISE) return js_mkundef();

  ant_value_t settled = js_get_slot(state, SLOT_SETTLED);
  if (vtype(settled) == T_BOOL && settled == js_true) return js_mkundef();
  js_set_slot(state, SLOT_SETTLED, js_true);

  ant_value_t signal = js_get(js, state, "signal");
  ant_value_t abort_listener = js_get(js, state, "abortListener");
  if (abort_signal_is_signal(signal) && is_callable(abort_listener))
    abort_signal_remove_listener(js, signal, abort_listener);
    
  ant_value_t values = js_mkarr(js);
  for (int i = 0; i < nargs; i++) js_arr_push(js, values, args[i]);
  js_resolve_promise(js, promise, values);
  
  return js_mkundef();
}

static void js_events_once_remove_listener_from_target(ant_t *js, ant_value_t state) {
  ant_value_t target = js_get(js, state, "target");
  ant_value_t key = js_get(js, state, "eventName");
  ant_value_t listener = js_get(js, state, "listener");
  if (!is_object_type(target) || !key || !is_callable(listener)) return;

  ant_value_t remove_method = js_getprop_fallback(js, target, "removeListener");
  if (!is_callable(remove_method)) remove_method = js_getprop_fallback(js, target, "removeEventListener");
  if (!is_callable(remove_method)) return;

  ant_value_t call_args[2] = { key, listener };
  eventemitter_call_listener(js, remove_method, target, call_args, 2);
}

static ant_value_t js_events_once_abort_listener(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t promise = js_get_slot(state, SLOT_DATA);
  if (vtype(promise) != T_PROMISE) return js_mkundef();

  ant_value_t settled = js_get_slot(state, SLOT_SETTLED);
  if (vtype(settled) == T_BOOL && settled == js_true) return js_mkundef();
  js_set_slot(state, SLOT_SETTLED, js_true);

  ant_value_t signal = js_get(js, state, "signal");
  ant_value_t abort_listener = js_get(js, state, "abortListener");
  if (abort_signal_is_signal(signal) && is_callable(abort_listener))
    abort_signal_remove_listener(js, signal, abort_listener);
  js_events_once_remove_listener_from_target(js, state);
    
  ant_value_t reason = abort_signal_get_reason(signal);
  if (vtype(reason) == T_UNDEF) reason = js_mkerr(js, "The operation was aborted");
  js_reject_promise(js, promise, reason);
  
  return js_mkundef();
}

static bool js_events_once_is_abort_key(ant_t *js, ant_value_t key) {
  size_t key_len = 0;
  const char *key_str = vtype(key) == T_STR ? js_getstr(js, key, &key_len) : NULL;
  return key_str && key_len == 5 && memcmp(key_str, "abort", 5) == 0;
}

static void js_events_once_reject_aborted(ant_t *js, ant_value_t promise, ant_value_t signal) {
  ant_value_t reason = abort_signal_get_reason(signal);
  if (vtype(reason) == T_UNDEF) reason = js_mkerr(js, "The operation was aborted");
  js_reject_promise(js, promise, reason);
}

static ant_value_t js_events_once_attach(
  ant_t *js,
  ant_value_t promise,
  ant_value_t target,
  ant_value_t key,
  ant_value_t listener,
  ant_value_t signal
) {
  if (abort_signal_is_signal(target)) {
    if (!js_events_once_is_abort_key(js, key)) {
      js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "AbortSignal only supports the abort event"));
      return promise;
    }
    abort_signal_add_listener(js, target, listener);
    return promise;
  }

  ant_value_t on_method = js_getprop_fallback(js, target, "on");
  ant_value_t once_method = is_callable(on_method)
    ? js_getprop_fallback(js, target, "once")
    : js_mkundef();
    
  if (is_callable(once_method)) {
    ant_value_t call_args[2] = { key, listener };
    ant_value_t result = eventemitter_call_listener(js, once_method, target, call_args, 2);
    if (is_err(result)) js_reject_promise(js, promise, result);
    return promise;
  }

  if (is_eventtarget_instance(target)) {
    ant_value_t listener_options = js_mkobj(js);
    js_set(js, listener_options, "once", js_true);
    if (abort_signal_is_signal(signal)) js_set(js, listener_options, "signal", signal);
    
    ant_value_t call_args[3] = { key, listener, listener_options };
    ant_value_t result = add_listener_to(js, call_args, 3, find_or_create_emitter_event_type(js, target, key));
    
    if (is_err(result)) js_reject_promise(js, promise, result);
    return promise;
  }

  js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "target is not an EventEmitter or EventTarget"));
  return promise;
}

static ant_value_t js_events_once(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "events.once requires an emitter and event name");

  ant_value_t key = evt_key_from_arg(args[1]);
  if (!key) return js_mkerr_typed(js, JS_ERR_TYPE, "event must be a string or Symbol");

  ant_value_t promise = js_mkpromise(js);
  if (is_err(promise)) return promise;

  ant_value_t state = js_mkobj(js);
  js_set_slot(state, SLOT_DATA, promise);
  js_set_slot(state, SLOT_SETTLED, js_false);

  ant_value_t listener = js_heavy_mkfun(js, js_events_once_listener, state);
  ant_value_t target = args[0];
  ant_value_t options = nargs >= 3 ? args[2] : js_mkundef();
  ant_value_t signal = js_mkundef();
  js_set(js, state, "target", target);
  js_set(js, state, "eventName", args[1]);
  js_set(js, state, "listener", listener);
  
  if (is_object_type(options)) signal = js_get(js, options, "signal");
  if (abort_signal_is_signal(signal)) {
    if (abort_signal_is_aborted(signal)) {
      js_events_once_reject_aborted(js, promise, signal);
      return promise;
    }
    ant_value_t abort_listener = js_heavy_mkfun(js, js_events_once_abort_listener, state);
    js_set(js, state, "signal", signal);
    js_set(js, state, "abortListener", abort_listener);
    abort_signal_add_listener(js, signal, abort_listener);
  }

  return js_events_once_attach(js, promise, target, key, listener, signal);
}

static ant_value_t js_events_disposable_dispose(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t disposed = js_get_slot(state, SLOT_SETTLED);
  if (vtype(disposed) == T_BOOL && disposed == js_true) return js_mkundef();
  js_set_slot(state, SLOT_SETTLED, js_true);

  ant_value_t signal = js_get(js, state, "signal");
  ant_value_t listener = js_get(js, state, "listener");
  if (abort_signal_is_signal(signal) && is_callable(listener))
    abort_signal_remove_listener(js, signal, listener);

  return js_mkundef();
}

static ant_value_t js_events_add_abort_listener(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || !abort_signal_is_signal(args[0]) || !is_callable(args[1]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "events.addAbortListener requires an AbortSignal and listener");

  bool already_aborted = abort_signal_is_aborted(args[0]);
  if (already_aborted) {
    ant_value_t event = js_mkobj(js);
    js_set(js, event, "type", js_mkstr(js, "abort", 5));
    eventemitter_call_listener(js, args[1], args[0], &event, 1);
  } else abort_signal_add_listener(js, args[0], args[1]);

  ant_value_t state = js_mkobj(js);
  js_set_slot(state, SLOT_SETTLED, js_bool(already_aborted));
  js_set(js, state, "signal", args[0]);
  js_set(js, state, "listener", args[1]);

  ant_value_t disposable = js_mkobj(js);
  js_set(js, disposable, "dispose", js_heavy_mkfun(js, js_events_disposable_dispose, state));
  
  return disposable;
}

static ant_value_t js_events_set_max_listeners(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "setMaxListeners requires at least 1 argument");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "n must be a number");

  int n = (int)js_getnum(args[0]);
  if (n < 0) return js_mkerr(js, "n must be non-negative");

  for (int i = 1; i < nargs; i++) {
    if (!is_object_type(args[i])) continue;
    js_set_slot(args[i], SLOT_EVENT_MAX_LISTENERS, js_mknum(n));
  }

  return js_mkundef();
}

static ant_value_t js_events_get_max_listeners(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mknum(EVENTS_DEFAULT_MAX_LISTENERS);
  if (!is_object_type(args[0])) return js_mknum(EVENTS_DEFAULT_MAX_LISTENERS);
  return js_mknum(eventemitter_get_max_listeners_impl(args[0]));
}

static ant_value_t js_events_get_event_listeners(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "target must be an EventEmitter or EventTarget");

  ant_value_t target = args[0];
  if (is_eventemitter_instance(target) || is_eventtarget_instance(target)) {
    ant_value_t key = nargs >= 2 ? evt_key_from_arg(args[1]) : 0;
    if (!key) return js_mkarr(js);
    return event_type_get_listeners_array(js, find_emitter_event_type(js, target, key), false);
  }

  ant_value_t listeners_method = js_getprop_fallback(js, target, "listeners");
  if (!is_callable(listeners_method))
    return js_mkerr_typed(js, JS_ERR_TYPE, "target must be an EventEmitter or EventTarget");

  ant_value_t key = nargs >= 2 ? args[1] : js_mkundef();
  return eventemitter_call_listener(js, listeners_method, target, &key, 1);
}

static ant_value_t events_on_iter_result(ant_t *js, ant_value_t value, bool done) {
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "value", value);
  js_set(js, result, "done", js_bool(done));
  return result;
}

static ant_value_t events_on_queue_shift(ant_t *js, ant_value_t state, const char *arr_name, const char *head_name) {
  ant_value_t arr = js_get(js, state, arr_name);
  ant_value_t head_val = js_get(js, state, head_name);
  ant_offset_t head = vtype(head_val) == T_NUM ? (ant_offset_t)js_getnum(head_val) : 0;

  if (vtype(arr) != T_ARR || head >= js_arr_len(js, arr)) return js_mkundef();
  js_set(js, state, head_name, js_mknum((double)(head + 1)));
  return js_arr_get(js, arr, head);
}

static void events_on_remove_one(ant_t *js, ant_value_t target, ant_value_t key, ant_value_t listener) {
  if (!is_object_type(target) || !key || !is_callable(listener)) return;

  ant_value_t remove_method = js_getprop_fallback(js, target, "removeListener");
  if (is_callable(remove_method)) {
    ant_value_t call_args[2] = { key, listener };
    eventemitter_call_listener(js, remove_method, target, call_args, 2);
    return;
  }

  if (is_eventtarget_instance(target)) 
    eventemitter_remove_listener_val(js, target, key, listener);
}

static void events_on_detach(ant_t *js, ant_value_t state) {
  ant_value_t target = js_get(js, state, "target");
  ant_value_t signal = js_get(js, state, "signal");
  ant_value_t abort_listener = js_get(js, state, "abortListener");

  if (js_truthy(js, js_get(js, state, "finished"))) return;
  js_set(js, state, "finished", js_true);

  events_on_remove_one(js, target, js_get(js, state, "eventName"), js_get(js, state, "listener"));
  events_on_remove_one(js, target, js_get(js, state, "errorKey"), js_get(js, state, "errorListener"));
  
  if (abort_signal_is_signal(signal) && is_callable(abort_listener))
    abort_signal_remove_listener(js, signal, abort_listener);
}

static void events_on_finish_parked(ant_t *js, ant_value_t state) {
  for (;;) {
    ant_value_t parked = events_on_queue_shift(js, state, "parked", "parkedHead");
    if (vtype(parked) != T_PROMISE) return;
    js_resolve_promise(js, parked, events_on_iter_result(js, js_mkundef(), true));
  }
}

static void events_on_fail(ant_t *js, ant_value_t state, ant_value_t reason) {
  events_on_detach(js, state);

  ant_value_t parked = events_on_queue_shift(js, state, "parked", "parkedHead");
  if (vtype(parked) == T_PROMISE) {
    js_reject_promise(js, parked, reason);
    events_on_finish_parked(js, state);
  } else js_set(js, state, "storedError", reason);
}

static ant_value_t js_events_on_event_cb(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t values = js_mkarr(js);
  for (int i = 0; i < nargs; i++) js_arr_push(js, values, args[i]);

  ant_value_t parked = events_on_queue_shift(js, state, "parked", "parkedHead");
  if (vtype(parked) == T_PROMISE)
    js_resolve_promise(js, parked, events_on_iter_result(js, values, false));
  else js_arr_push(js, js_get(js, state, "buffer"), values);

  return js_mkundef();
}

static ant_value_t js_events_on_error_cb(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();
  events_on_fail(js, state, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t js_events_on_abort_cb(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t reason = abort_signal_get_reason(js_get(js, state, "signal"));
  if (vtype(reason) == T_UNDEF) reason = js_mkerr(js, "The operation was aborted");
  events_on_fail(js, state, reason);
  return js_mkundef();
}

static ant_value_t js_events_on_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_mkpromise(js);
  if (!is_object_type(state)) return promise;

  // drain what arrived before the failure; the error surfaces after
  ant_value_t buffered = events_on_queue_shift(js, state, "buffer", "bufferHead");
  if (!is_undefined(buffered)) {
    js_resolve_promise(js, promise, events_on_iter_result(js, buffered, false));
    return promise;
  }

  ant_value_t stored = js_get(js, state, "storedError");
  if (!is_undefined(stored)) {
    js_set(js, state, "storedError", js_mkundef());
    js_reject_promise(js, promise, stored);
    return promise;
  }

  if (js_truthy(js, js_get(js, state, "finished"))) {
    js_resolve_promise(js, promise, events_on_iter_result(js, js_mkundef(), true));
    return promise;
  }

  js_arr_push(js, js_get(js, state, "parked"), promise);
  return promise;
}

static ant_value_t js_events_on_return(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_mkpromise(js);

  if (is_object_type(state)) {
    events_on_detach(js, state);
    events_on_finish_parked(js, state);
  }

  js_resolve_promise(js, promise, events_on_iter_result(js, nargs > 0 ? args[0] : js_mkundef(), true));
  return promise;
}

static ant_value_t js_events_on_self(ant_t *js, ant_value_t *args, int nargs) {
  return js_getthis(js);
}

static ant_value_t js_events_on(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "events.on requires an emitter and an event name");

  ant_value_t target = args[0];
  ant_value_t key = evt_key_from_arg(args[1]);
  if (!key) return js_mkerr_typed(js, JS_ERR_TYPE, "event name must be a string or Symbol");

  ant_value_t options = nargs >= 3 && is_object_type(args[2]) ? args[2] : js_mkundef();
  ant_value_t signal = is_object_type(options) ? js_get(js, options, "signal") : js_mkundef();
  if (abort_signal_is_signal(signal) && abort_signal_is_aborted(signal))
    return js_mkerr(js, "The operation was aborted");

  ant_value_t state = js_mkobj(js);
  ant_value_t listener = js_heavy_mkfun(js, js_events_on_event_cb, state);
  ant_value_t error_key = js_mkstr(js, "error", 5);
  ant_value_t error_listener = js_heavy_mkfun(js, js_events_on_error_cb, state);

  js_set(js, state, "target", target);
  js_set(js, state, "eventName", key);
  js_set(js, state, "listener", listener);
  js_set(js, state, "errorKey", error_key);
  js_set(js, state, "errorListener", error_listener);
  js_set(js, state, "signal", js_mkundef());
  js_set(js, state, "abortListener", js_mkundef());
  js_set(js, state, "buffer", js_mkarr(js));
  js_set(js, state, "bufferHead", js_mknum(0));
  js_set(js, state, "parked", js_mkarr(js));
  js_set(js, state, "parkedHead", js_mknum(0));
  js_set(js, state, "finished", js_false);
  js_set(js, state, "storedError", js_mkundef());

  ant_value_t on_method = js_getprop_fallback(js, target, "on");
  if (is_callable(on_method)) {
    ant_value_t on_args[2] = { key, listener };
    eventemitter_call_listener(js, on_method, target, on_args, 2);
    on_args[0] = error_key; on_args[1] = error_listener;
    eventemitter_call_listener(js, on_method, target, on_args, 2);
  } else if (is_eventtarget_instance(target)) {
    eventemitter_add_listener_val(js, target, key, listener, false);
  } else return js_mkerr_typed(js, JS_ERR_TYPE, "target is not an EventEmitter or EventTarget");

  if (abort_signal_is_signal(signal)) {
    ant_value_t abort_listener = js_heavy_mkfun(js, js_events_on_abort_cb, state);
    js_set(js, state, "signal", signal);
    js_set(js, state, "abortListener", abort_listener);
    abort_signal_add_listener(js, signal, abort_listener);
  }

  ant_value_t iter = js_mkobj(js);
  js_set(js, iter, "next", js_heavy_mkfun(js, js_events_on_next, state));
  js_set(js, iter, "return", js_heavy_mkfun(js, js_events_on_return, state));
  js_set_sym(js, iter, get_asyncIterator_sym(), js_mkfun(js_events_on_self));
  return iter;
}

ant_value_t events_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  
  eventemitter_prototype(js);
  js_set_module_default(js, lib, g_eventemitter_ctor, "EventEmitter");
  js_set(js, lib, "once", js_mkfun(js_events_once));
  js_set(js, lib, "on", js_mkfun(js_events_on));
  js_set(js, lib, "addAbortListener", js_mkfun(js_events_add_abort_listener));
  js_set(js, lib, "setMaxListeners", js_mkfun(js_events_set_max_listeners));
  js_set(js, lib, "getMaxListeners", js_mkfun(js_events_get_max_listeners));
  js_set(js, lib, "getEventListeners", js_mkfun(js_events_get_event_listeners));
  js_set(js, g_eventemitter_ctor, "once", js_get(js, lib, "once"));
  js_set(js, g_eventemitter_ctor, "on", js_get(js, lib, "on"));
  js_set(js, g_eventemitter_ctor, "addAbortListener", js_get(js, lib, "addAbortListener"));
  js_set(js, g_eventemitter_ctor, "setMaxListeners", js_get(js, lib, "setMaxListeners"));
  js_set(js, g_eventemitter_ctor, "getMaxListeners", js_get(js, lib, "getMaxListeners"));
  js_set(js, g_eventemitter_ctor, "getEventListeners", js_get(js, lib, "getEventListeners"));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "events", 6));
  
  return lib;
}

ant_value_t eventemitter_prototype(ant_t *js) {
  if (g_eventemitter_proto) return g_eventemitter_proto;

  ant_value_t object_proto = js->sym.object_proto;
  ant_value_t function_proto = js_get_slot(js_glob(js), SLOT_FUNC_PROTO);
  if (vtype(function_proto) == T_UNDEF) function_proto = js_get_ctor_proto(js, "Function", 8);

  ant_value_t eventemitter_ctor = js_mkobj(js);
  ant_value_t eventemitter_proto = js_mkobj(js);
  
  if (is_object_type(object_proto)) js_set_proto_init(eventemitter_proto, object_proto);
  if (is_object_type(function_proto)) js_set_proto_init(eventemitter_ctor, function_proto);

  js_set(js, eventemitter_proto, "on",                 js_mkfun(js_eventemitter_on));
  js_set_exact(js, eventemitter_proto, "addListener",  js_get(js, eventemitter_proto, "on"));
  js_set(js, eventemitter_proto, "once",               js_mkfun(js_eventemitter_once));
  js_set(js, eventemitter_proto, "prependListener",    js_mkfun(js_eventemitter_prepend_listener));
  js_set(js, eventemitter_proto, "prependOnceListener", js_mkfun(js_eventemitter_prepend_once_listener));
  js_set(js, eventemitter_proto, "off",                js_mkfun(js_eventemitter_off));
  js_set_exact(js, eventemitter_proto, "removeListener", js_get(js, eventemitter_proto, "off"));
  js_set(js, eventemitter_proto, "emit",               js_mkfun(js_eventemitter_emit));
  js_set(js, eventemitter_proto, "removeAllListeners", js_mkfun(js_eventemitter_removeAllListeners));
  js_set(js, eventemitter_proto, "listenerCount",      js_mkfun(js_eventemitter_listenerCount));
  js_set(js, eventemitter_proto, "setMaxListeners",    js_mkfun(js_eventemitter_setMaxListeners));
  js_set(js, eventemitter_proto, "getMaxListeners",    js_mkfun(js_eventemitter_getMaxListeners));
  js_set(js, eventemitter_proto, "listeners",          js_mkfun(js_eventemitter_listeners));
  js_set(js, eventemitter_proto, "rawListeners",       js_mkfun(js_eventemitter_rawListeners));
  js_set(js, eventemitter_proto, "eventNames",         js_mkfun(js_eventemitter_eventNames));
  js_set_sym(js, eventemitter_proto, get_toStringTag_sym(), js_mkstr(js, "EventEmitter", 12));

  js_set_slot(eventemitter_ctor, SLOT_CFUNC, js_mkfun(js_eventemitter_ctor));
  js_mkprop_fast(js, eventemitter_ctor, "prototype", 9, eventemitter_proto);
  js_mkprop_fast(js, eventemitter_ctor, "name", 4, ANT_STRING("EventEmitter"));
  js_set_descriptor(js, eventemitter_ctor, "name", 4, 0);

  g_eventemitter_proto = eventemitter_proto;
  g_eventemitter_ctor = js_obj_to_func(js, eventemitter_ctor);
  js_set(js, eventemitter_proto, "constructor", g_eventemitter_ctor);
  js_set_descriptor(js, eventemitter_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  return g_eventemitter_proto;
}

void init_events_module(ant_t *js) {
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

  ant_value_t event_fn = js_make_ctor(js, js_event_ctor, g_event_proto, "Event", 5);
  js_set(js, event_fn, "NONE",            js_mknum(0));
  js_set(js, event_fn, "CAPTURING_PHASE", js_mknum(1));
  js_set(js, event_fn, "AT_TARGET",       js_mknum(2));
  js_set(js, event_fn, "BUBBLING_PHASE",  js_mknum(3));
  js_set(js, global, "Event", event_fn);

  g_customevent_proto = js_mkobj(js);
  js_set_proto_init(g_customevent_proto, g_event_proto);
  js_set_sym(js, g_customevent_proto, get_toStringTag_sym(), js_mkstr(js, "CustomEvent", 11));

  ant_value_t customevent_fn = js_make_ctor(js, js_customevent_ctor, g_customevent_proto, "CustomEvent", 11);
  js_set(js, global, "CustomEvent", customevent_fn);

  g_errorevent_proto = js_mkobj(js);
  js_set_proto_init(g_errorevent_proto, g_event_proto);
  js_set_sym(js, g_errorevent_proto, get_toStringTag_sym(), js_mkstr(js, "ErrorEvent", 10));

  ant_value_t errorevent_fn = js_make_ctor(js, js_errorevent_ctor, g_errorevent_proto, "ErrorEvent", 10);
  js_set(js, global, "ErrorEvent", errorevent_fn);

  g_promiserejectionevent_proto = js_mkobj(js);
  js_set_proto_init(g_promiserejectionevent_proto, g_event_proto);
  js_set_sym(js, g_promiserejectionevent_proto, get_toStringTag_sym(), js_mkstr(js, "PromiseRejectionEvent", 21));

  ant_value_t pre_fn = js_make_ctor(js, js_promiserejectionevent_ctor, g_promiserejectionevent_proto, "PromiseRejectionEvent", 21);
  js_set(js, global, "PromiseRejectionEvent", pre_fn);

  ant_value_t object_proto = js->sym.object_proto;
  ant_value_t function_proto = js_get_slot(global, SLOT_FUNC_PROTO);
  if (vtype(function_proto) == T_UNDEF) function_proto = js_get_ctor_proto(js, "Function", 8);

  ant_value_t eventtarget_proto = js_mkobj(js);
  g_eventtarget_proto = eventtarget_proto;
  if (is_object_type(object_proto)) js_set_proto_init(eventtarget_proto, object_proto);
  js_set(js, eventtarget_proto, "addEventListener",    js_mkfun(js_add_event_listener_method));
  js_set(js, eventtarget_proto, "removeEventListener", js_mkfun(js_remove_event_listener_method));
  js_set(js, eventtarget_proto, "dispatchEvent",       js_mkfun(js_dispatch_event_method));
  js_set_sym(js, eventtarget_proto, get_toStringTag_sym(), js_mkstr(js, "EventTarget", 11));

  ant_value_t eventtarget_ctor = js_mkobj(js);
  if (is_object_type(function_proto)) js_set_proto_init(eventtarget_ctor, function_proto);
  js_set_slot(eventtarget_ctor, SLOT_CFUNC, js_mkfun(js_eventtarget_ctor));
  js_mkprop_fast(js, eventtarget_ctor, "prototype", 9, eventtarget_proto);
  js_mkprop_fast(js, eventtarget_ctor, "name", 4, ANT_STRING("EventTarget"));
  js_set_descriptor(js, eventtarget_ctor, "name", 4, 0);
  ant_value_t eventtarget_fn = js_obj_to_func(js, eventtarget_ctor);
  js_set(js, eventtarget_proto, "constructor", eventtarget_fn);
  js_set_descriptor(js, eventtarget_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "addEventListener",    js_mkfun(js_add_event_listener));
  js_set(js, global, "removeEventListener", js_mkfun(js_remove_event_listener));
  js_set(js, global, "dispatchEvent",       js_mkfun(js_dispatch_event));
  js_set(js, global, "EventTarget",         eventtarget_fn);
}

static void mark_event_type_listeners(ant_t *js, gc_mark_fn mark, EventTypeList *list) {
  if (!list) return;
  for (unsigned int t = 0; t < list->count; t++) {
  EventType *evt = list->types[t];
  if (vtype(evt->js_key) == T_STR || vtype(evt->js_key) == T_SYMBOL) mark(js, evt->js_key);
  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
    EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    mark(js, e->callback);
    ant_value_t raw_callback = entry_raw_callback(evt, e);
    ant_value_t signal = entry_signal(evt, e);
    if (vtype(raw_callback) != T_UNDEF) mark(js, raw_callback);
    if (vtype(signal) != T_UNDEF) mark(js, signal);
  }
}}

void gc_mark_events(ant_t *js, gc_mark_fn mark) {
  if (js->events_state) mark_event_type_listeners(js, mark, &js->events_state->global_events);
  
  if (g_isTrusted_getter)            mark(js, g_isTrusted_getter);
  if (g_eventemitter_ctor)           mark(js, g_eventemitter_ctor);
  if (g_eventemitter_proto)          mark(js, g_eventemitter_proto);
  if (g_eventtarget_proto)           mark(js, g_eventtarget_proto);
  if (g_event_proto)                 mark(js, g_event_proto);
  if (g_customevent_proto)           mark(js, g_customevent_proto);
  if (g_errorevent_proto)            mark(js, g_errorevent_proto);
  if (g_promiserejectionevent_proto) mark(js, g_promiserejectionevent_proto);
}

void gc_mark_eventemitter_object(ant_t *js, ant_value_t obj, gc_mark_fn mark) {
  EventTypeList *events = find_emitter_events(obj);
  if (events) mark_event_type_listeners(js, mark, events);
}

void gc_finalize_events_object(ant_t *js, ant_value_t obj) {
  event_data_t *data = (event_data_t *)js_get_native(obj, EVENT_NATIVE_TAG);
  EventTypeList *events = find_emitter_events(obj);

  if (data) {
    js_clear_native(obj, EVENT_NATIVE_TAG);
    free(data);
  }
  
  if (events) {
    js_clear_native(obj, EVENT_EMITTER_NATIVE_TAG);
    evt_list_free(events);
    free(events);
  }
}
