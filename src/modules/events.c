#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <utarray.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "builder.h"
#include "native_getter.h"
#include "silver/call.h"

#include "gc.h"
#include "gc/modules.h"
#include "modules/abort.h"
#include "modules/events.h"
#include "modules/iterator.h"
#include "modules/timer.h"

typedef struct {
  ant_value_t type;
  ant_value_t target;
  ant_value_t current_target;
  ant_value_t phase;
  ant_value_t time_stamp;
  ant_event_kind_t kind;
  uint8_t field_count;
  bool bubbles;
  bool cancelable;
  bool canceled;
  bool stop_immediate;
  bool stop_propagation;
  bool dispatching;
  bool trusted;
  ant_value_t fields[];
} event_data_t;

static constexpr uint32_t EVENT_NATIVE_TAG = Ant_NativeTag("EVNT");
static constexpr uint32_t EVENT_EMITTER_NATIVE_TAG = Ant_NativeTag("EEMT");

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

typedef struct {
  UT_array *listeners;
  ant_value_t js_key;
  ant_value_t handler;
  const char *key_bytes;
  size_t key_len;
  ant_event_list_t *owner;
  uint32_t generation;
  unsigned int dead_count;
  int emitting;
  EventListenerCold *cold_entries;
  uint32_t next_listener_id;
  bool needs_reorder;
  bool warned_max_listeners;
  uint32_t key_matches;
} EventType;

struct ant_event_list {
  EventType **types;
  unsigned int count;
  unsigned int cap;
  ant_value_t target;
  eventemitter_listener_change_fn listener_change_hook;
  void *listener_change_context;
};

static void emitter_write_barrier(ant_t *js, ant_value_t owner, ant_value_t stored) {
  if (!is_object_type(owner)) return;
  gc_write_barrier(js, js_obj_ptr(owner), stored);
}

static ant_event_list_t *global_events_list(ant_t *js) {
  if (!js->global_events) js->global_events = calloc(1, sizeof(*js->global_events));
  return js->global_events;
}

static void evt_list_free(ant_event_list_t *list) {
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
  EventType *evt = calloc(1, sizeof(EventType));
  if (!evt) return NULL;
  evt->js_key = js_key;
  evt->handler = js_mknull();
  if (vtype(js_key) == kTypeString) evt->key_bytes = js_getstr(js, js_key, &evt->key_len);
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

  cold = calloc(1, sizeof(*cold));
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
  ant_event_list_t *list = evt ? evt->owner : NULL;
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
  ant_event_list_t *list = evt->owner;

  if (!list || evt->emitting != 0 || utarray_len(evt->listeners) != 0) return;
  
  if (list->count <= 16 && !evt->cold_entries) {
    evt->warned_max_listeners = false;
    return;
  }

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

static EventType *evt_list_find_cstr(ant_event_list_t *list, const char *name, size_t len) {
  for (unsigned int i = 0; i < list->count; i++) {
    EventType *evt = list->types[i];
    if (evt->key_len != len || !evt->key_bytes) continue;
    if (memcmp(evt->key_bytes, name, len) == 0) return evt;
  }
  return NULL;
}

static __attribute__((noinline, cold)) void evt_cache_key(
  ant_t *js, ant_event_list_t *list, EventType *evt, ant_value_t js_key, const char *bytes
) {
  uint32_t matches = ++evt->key_matches;
  if (matches & (matches - 1)) return;
  evt->js_key = js_key;
  evt->key_bytes = bytes;
  emitter_write_barrier(js, list->target, js_key);
}

static EventType *evt_list_find_slow(ant_t *js, ant_event_list_t *list, ant_value_t js_key) {
  if (vtype(js_key) != kTypeString) return NULL;

  size_t probe_len = 0;
  const char *probe = js_getstr(js, js_key, &probe_len);
  EventType *evt = probe ? evt_list_find_cstr(list, probe, probe_len) : NULL;
  if (evt) evt_cache_key(js, list, evt, js_key, probe);
  return evt;
}

static inline __attribute__((always_inline)) EventType *evt_list_find(
  ant_t *js, ant_event_list_t *list, ant_value_t js_key
) {
  for (unsigned int i = 0; i < list->count; i++)
    if (list->types[i]->js_key == js_key) return list->types[i];
  return evt_list_find_slow(js, list, js_key);
}

static EventType *evt_list_find_or_create(ant_t *js, ant_event_list_t *list, ant_value_t js_key) {
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

static inline ant_event_list_t *find_emitter_events(ant_value_t this_obj) {
  return (ant_event_list_t *)js_get_native(this_obj, EVENT_EMITTER_NATIVE_TAG);
}

static ant_event_list_t *get_or_create_emitter_events(ant_t *js, ant_value_t this_obj) {
  ant_event_list_t *events = find_emitter_events(this_obj);
  if (!events) {
    events = calloc(1, sizeof(*events));
    if (!events) return NULL;
    events->target = this_obj;
    js_set_native(this_obj, events, EVENT_EMITTER_NATIVE_TAG);
  }
  return events;
}

static EventType *find_or_create_global_event_type(ant_t *js, ant_value_t js_key) {
  if (vtype(js_key) != kTypeString && vtype(js_key) != kTypeSymbol) return NULL;
  ant_event_list_t *list = global_events_list(js);
  return list ? evt_list_find_or_create(js, list, js_key) : NULL;
}

static EventType *find_global_event_type(ant_t *js, ant_value_t js_key) {
  ant_event_list_t *list = global_events_list(js);
  return list ? evt_list_find(js, list, js_key) : NULL;
}

static EventType *find_or_create_emitter_event_type(ant_t *js, ant_value_t this_obj, ant_value_t js_key) {
  ant_event_list_t *events = NULL;

  if (vtype(js_key) != kTypeString && vtype(js_key) != kTypeSymbol) return NULL;
  events = get_or_create_emitter_events(js, this_obj);

  return events ? evt_list_find_or_create(js, events, js_key) : NULL;
}

static EventType *find_emitter_event_type_cstr(ant_t *js, ant_value_t this_obj, const char *name, size_t len) {
  ant_event_list_t *events = find_emitter_events(this_obj);
  if (!events) return NULL;
  return evt_list_find_cstr(events, name, len);
}

static EventType *find_emitter_event_type(ant_t *js, ant_value_t this_obj, ant_value_t js_key) {
  ant_event_list_t *events = find_emitter_events(this_obj);
  if (!events) return NULL;
  return evt_list_find(js, events, js_key);
}

static inline ant_value_t evt_key_from_arg(ant_value_t arg) {
  uint8_t t = vtype(arg);
  return (t == kTypeString || t == kTypeSymbol) ? arg : 0;
}

static bool is_eventemitter_instance(ant_value_t target) {
  return is_object_type(target) && find_emitter_events(target) != NULL;
}

static bool is_eventtarget_instance(ant_value_t target) {
  return js_check_brand(target, BRAND_EVENTTARGET);
}

static int eventemitter_get_max_listeners_impl(ant_value_t target) {
  ant_value_t slot = js_get_slot(target, SLOT_EVENT_MAX_LISTENERS);
  if (vtype(slot) == kTypeNumber) {
    int n = (int)js_getnum(slot);
    return n >= 0 ? n : EVENTS_DEFAULT_MAX_LISTENERS;
  }
  return EVENTS_DEFAULT_MAX_LISTENERS;
}

static inline __attribute__((always_inline)) ant_value_t eventemitter_call_listener_inline(
  ant_t *js,
  ant_value_t listener,
  ant_value_t this_val,
  ant_value_t *args,
  int nargs
) {
  if (!is_callable(listener)) return js_mkundef();
  if (sv_check_c_stack_overflow(js))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");

  if (vtype(listener) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(listener);
    
    if (sv_closure_is_plain_sync(closure)) {
      sv_call_ctx_t ctx = {
        .this_val = this_val,
        .super_val = js_mkundef(),
        .new_target = js_mkundef(),
        .args = args,
        .argc = nargs,
        .alloc = NULL,
      };
      
      sv_func_t *fn = closure->func;
      if (fn->jit_code) {
        ant_value_t result = sv_jit_invoke(js, SV_JIT_FROM_C, fn->jit_code, js->vm, &ctx, closure);
        if (!sv_is_jit_bailout(result)) return result;
        sv_jit_on_bailout(fn);
      }
      
      return sv_call_closure_after_jit(js->vm, js, closure, listener, &ctx, NULL);
    }
  }

  return sv_vm_call_mode(
    js->vm, js, listener, this_val, args, nargs,
    NULL, SV_CALL_MODE_NORMAL, js_mkundef()
  );
}

static ant_value_t eventemitter_call_listener(
  ant_t *js,
  ant_value_t listener,
  ant_value_t this_val,
  ant_value_t *args, int nargs
) {
  return eventemitter_call_listener_inline(js, listener, this_val, args, nargs);
}

static ant_value_t js_eventemitter_once_wrapper(ant_params_t) {
  ant_value_t listener = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_callable(listener)) return js_mkundef();
  return eventemitter_call_listener(js, listener, js->this_val, args, nargs);
}

static ant_value_t entry_once_wrapper(ant_t *js, EventType *evt, EventListenerEntry *entry) {
  EventListenerCold *cold = entry_ensure_cold(evt, entry);
  if (!cold) return entry->callback;
  if (is_callable(cold->raw_callback)) return cold->raw_callback;

  ant_value_t wrapper = js_heavy_mkfun(js, js_eventemitter_once_wrapper, entry->callback);
  if (!is_callable(wrapper)) return entry->callback;
  
  cold->raw_callback = wrapper;
  if (evt->owner) emitter_write_barrier(js, evt->owner->target, wrapper);
  js_set(js, wrapper, "listener", entry->callback);
  
  return wrapper;
}

static ant_value_t event_type_get_listeners_array(ant_t *js, EventType *evt, bool raw) {
  if (!evt || utarray_len(evt->listeners) == 0) return js_mkarr(js);

  unsigned int len = utarray_len(evt->listeners);
  if (raw) for (unsigned int i = 0; i < len; i++) {
    EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    if (entry_live(entry) && entry_once(entry)) entry_once_wrapper(js, evt, entry);
  }

  ant_value_t *data = NULL;
  ant_value_t result = js_mkarr_dense_uninit(js, evt_live_count(evt), &data);
  if (is_err(result) || !data) return result;

  unsigned int count = 0;
  for (unsigned int i = 0; i < len; i++) {
    EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    if (!entry_live(entry)) continue;
    ant_value_t raw_cb = raw && entry_once(entry) ? entry_raw_callback(evt, entry) : js_mkundef();
    data[count++] = is_callable(raw_cb) ? raw_cb : entry->callback;
  }

  return result;
}

static ant_value_t eventemitter_get_listeners_array(ant_t *js, ant_value_t target, ant_value_t key, bool raw) {
  if (!is_object_type(target) || !key) return js_mkarr(js);
  return event_type_get_listeners_array(js, find_emitter_event_type(js, target, key), raw);
}

static inline uint8_t event_field_count(ant_event_kind_t kind) {
  uint8_t count = 0;
#define ANT_EVENT_FIELD(k, name, index) if (kind == ANT_EVENT_##k) count++;
#include "events/fields.h"
  return count;
}

static event_data_t *event_init(
  ant_t *js, ant_value_t obj, ant_event_kind_t kind,
  ant_value_t type, bool bubbles, bool cancelable
) {
  uint8_t count = event_field_count(kind);
  event_data_t *data = malloc(sizeof(*data) + count * sizeof(ant_value_t));
  if (!data) return NULL;
  
  *data = (event_data_t){
    .type = type, .target = js_mknull(), .current_target = js_mknull(),
    .phase = js_mknum(0), .time_stamp = js_mknum(get_timestamp_ms()),
    .kind = kind, .field_count = count, .bubbles = bubbles, .cancelable = cancelable,
  };
  
  for (uint8_t i = 0; i < count; i++) data->fields[i] = js_mkundef();
  js_set_native(obj, data, EVENT_NATIVE_TAG);
  gc_write_barrier(js, js_obj_ptr(obj), type);
  
  return data;
}

static __attribute__((cold, noinline)) ant_value_t event_this_error(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Value of \"this\" must be of type Event");
}

static void event_set_value(ant_t *js, ant_value_t event, ant_value_t *slot, ant_value_t value) {
  *slot = value;
  gc_write_barrier(js, js_obj_ptr(event), value);
}

static ant_value_t event_construct(
  ant_t *js, ant_value_t *args, int nargs, ant_value_t new_target,
  ant_event_kind_t kind, const char *name, ant_value_t *init_out
) {
  if (vtype(new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s constructor requires 'new'", name);
  if (nargs < 1 || vtype(args[0]) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s constructor: type argument is required", name);

  ant_value_t type = vtype(args[0]) == kTypeString ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(type)) return type;

  ant_value_t init = nargs >= 2 && vtype(args[1]) == kTypeObject ? args[1] : js_mkundef();
  bool bubbles = false, cancelable = false;
  if (is_object_type(init)) {
    ant_value_t b = js_get(js, init, "bubbles");
    if (is_err(b)) return b;
    ant_value_t c = js_get(js, init, "cancelable");
    if (is_err(c)) return c;
    bubbles = js_truthy(js, b);
    cancelable = js_truthy(js, c);
  }

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj) || !event_init(js, this_obj, kind, type, bubbles, cancelable))
    return js_mkerr(js, "out of memory");
  
  *init_out = init;
  return this_obj;
}

static ant_value_t event_init_field(
  ant_t *js, ant_value_t event, ant_value_t init,
  const char *key, unsigned int index, ant_value_t fallback, bool to_string
) {
  ant_value_t value = is_object_type(init) ? js_get(js, init, key) : js_mkundef();
  if (is_err(value)) return value;
  if (vtype(value) == kTypeUndefined) value = fallback;
  else if (to_string) value = js_tostring_val(js, value);
  if (is_err(value)) return value;
  Ant_Event_SetField(js, event, index, value);
  return js_mkundef();
}

static ant_value_t js_event_ctor(ant_params_t) {
  ant_value_t init;
  ant_value_t event = event_construct(js, args, nargs, call_new_target, ANT_EVENT_BASE, "Event", &init);
  return is_err(event) ? event : js_mkundef();
}

static ant_value_t js_customevent_ctor(ant_params_t) {
  ant_value_t init;
  ant_value_t event = event_construct(js, args, nargs, call_new_target, ANT_EVENT_CUSTOM, "CustomEvent", &init);
  if (is_err(event)) return event;
  ant_value_t r = event_init_field(js, event, init, "detail", ANT_EVENT_FIELD_CUSTOM_detail, js_mknull(), false);
  return is_err(r) ? r : js_mkundef();
}

static ant_value_t js_errorevent_ctor(ant_params_t) {
  ant_value_t init;
  ant_value_t event = event_construct(js, args, nargs, call_new_target, ANT_EVENT_ERROR, "ErrorEvent", &init);
  if (is_err(event)) return event;
  
  ant_value_t empty = js_mkstr(js, "", 0);
  ant_value_t r;
  if (is_err(r = event_init_field(js, event, init, "message",  ANT_EVENT_FIELD_ERROR_message,  empty, true)))  return r;
  if (is_err(r = event_init_field(js, event, init, "filename", ANT_EVENT_FIELD_ERROR_filename, empty, true)))  return r;
  if (is_err(r = event_init_field(js, event, init, "lineno",   ANT_EVENT_FIELD_ERROR_lineno,   js_mknum(0), false))) return r;
  if (is_err(r = event_init_field(js, event, init, "colno",    ANT_EVENT_FIELD_ERROR_colno,    js_mknum(0), false))) return r;
  if (is_err(r = event_init_field(js, event, init, "error",    ANT_EVENT_FIELD_ERROR_error,    js_mkundef(), false))) return r;
  return js_mkundef();
}

static ant_value_t js_promiserejectionevent_ctor(ant_params_t) {
  ant_value_t init;
  ant_value_t event = event_construct(js, args, nargs, call_new_target, ANT_EVENT_PROMISE_REJECTION, "PromiseRejectionEvent", &init);
  if (is_err(event)) return event;
  
  ant_value_t r;
  if (is_err(r = event_init_field(js, event, init, "promise", ANT_EVENT_FIELD_PROMISE_REJECTION_promise, js_mkundef(), false))) return r;
  if (is_err(r = event_init_field(js, event, init, "reason",  ANT_EVENT_FIELD_PROMISE_REJECTION_reason,  js_mkundef(), false))) return r;
  return js_mkundef();
}

#define EVENT_VALUE_GETTER(field) \
  static ant_value_t event_get_##field(ant_params_t) { \
    event_data_t *data = get_event_data(js_getthis(js)); \
    return data ? data->field : event_this_error(js); \
  } \
  static const ant_native_getter_t event_##field##_getter = { \
    .meta = { event_get_##field, NULL, 0, CFUNC_NATIVE_GETTER }, .native_tag = EVENT_NATIVE_TAG, \
    .kind = ANT_NATIVE_GETTER_FIELD, .field_offset = offsetof(event_data_t, field) };

EVENT_VALUE_GETTER(type)
EVENT_VALUE_GETTER(target)
EVENT_VALUE_GETTER(current_target)
EVENT_VALUE_GETTER(phase)
EVENT_VALUE_GETTER(time_stamp)

#define EVENT_FLAG_GETTER(name, expr) \
  static ant_value_t event_get_##name(ant_params_t) { \
    event_data_t *data = get_event_data(js_getthis(js)); \
    return data ? js_bool(expr) : event_this_error(js); \
  }

EVENT_FLAG_GETTER(bubbles, data->bubbles)
EVENT_FLAG_GETTER(cancelable, data->cancelable)
EVENT_FLAG_GETTER(default_prevented, data->canceled)
EVENT_FLAG_GETTER(return_value, !data->canceled)
EVENT_FLAG_GETTER(cancel_bubble, data->stop_propagation)
EVENT_FLAG_GETTER(composed, false)

static ant_value_t js_event_get_isTrusted(ant_params_t) {
  event_data_t *data = get_event_data(js_getthis(js));
  return data ? js_bool(data->trusted) : event_this_error(js);
}

static ant_value_t event_set_return_value(ant_params_t) {
  event_data_t *data = get_event_data(js_getthis(js));
  if (!data) return event_this_error(js);
  if (nargs > 0 && !js_truthy(js, args[0]) && data->cancelable) data->canceled = true;
  return js_mkundef();
}

static ant_value_t event_set_cancel_bubble(ant_params_t) {
  event_data_t *data = get_event_data(js_getthis(js));
  if (!data) return event_this_error(js);
  if (nargs > 0 && js_truthy(js, args[0])) data->stop_propagation = true;
  return js_mkundef();
}

static ant_value_t js_event_preventDefault(ant_params_t) {
  event_data_t *data = get_event_data(js_getthis(js));
  if (!data) return event_this_error(js);
  if (data->cancelable) data->canceled = true;
  return js_mkundef();
}

static ant_value_t js_event_stopPropagation(ant_params_t) {
  event_data_t *data = get_event_data(js_getthis(js));
  if (!data) return event_this_error(js);
  data->stop_propagation = true;
  return js_mkundef();
}

static ant_value_t js_event_stopImmediatePropagation(ant_params_t) {
  event_data_t *data = get_event_data(js_getthis(js));
  if (!data) return event_this_error(js);
  data->stop_immediate = true;
  data->stop_propagation = true;
  return js_mkundef();
}

static ant_value_t js_event_composedPath(ant_params_t) {
  event_data_t *data = get_event_data(js_getthis(js));
  if (!data) return event_this_error(js);
  ant_value_t result = js_mkarr(js);
  if (data->dispatching && is_object_type(data->current_target)) js_arr_push(js, result, data->current_target);
  return result;
}

static ant_value_t js_event_initEvent(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  event_data_t *data = get_event_data(this_obj);
  if (!data) return event_this_error(js);
  if (data->dispatching) return js_mkundef();
  
  if (nargs >= 1) {
    ant_value_t type = vtype(args[0]) == kTypeString ? args[0] : js_tostring_val(js, args[0]);
    if (is_err(type)) return type;
    event_set_value(js, this_obj, &data->type, type);
  }
  
  data->bubbles = nargs >= 2 && js_truthy(js, args[1]);
  data->cancelable = nargs >= 3 && js_truthy(js, args[2]);
  data->canceled = data->stop_immediate = data->stop_propagation = false;
  return js_mkundef();
}

static ant_value_t js_event_inspect(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  event_data_t *data = get_event_data(this_obj);
  if (!data) return js_mkundef();
  
  ant_value_t tag_val = js_get_sym(js, this_obj, js->sym.toStringTag_sym);
  const char *tag = vtype(tag_val) == kTypeString ? js_getstr(js, tag_val, NULL) : "Event";
  
  js_inspect_builder_t builder;
  if (!js_inspect_builder_init_dynamic(&builder, js, 128)) return js_mkerr(js, "out of memory");
  
  bool ok = js_inspect_header(&builder, "%s", tag);
  if (ok) ok = js_inspect_field(&builder, "type", data->type);
  if (ok) ok = js_inspect_field(&builder, "defaultPrevented", js_bool(data->canceled));
  if (ok) ok = js_inspect_field(&builder, "cancelable", js_bool(data->cancelable));
  if (ok) ok = js_inspect_field(&builder, "timeStamp", data->time_stamp);
  if (ok) ok = js_inspect_close(&builder);
  
  if (!ok) {
    js_inspect_builder_dispose(&builder);
    return js_mkerr(js, "out of memory");
  }
  
  return js_inspect_builder_result(&builder);
}

#define ANT_EVENT_FIELD(k, name, index) \
  static ant_value_t event_get_##k##_##name(ant_params_t) { \
    event_data_t *data = get_event_data(js_getthis(js)); \
    if (!data || data->kind != ANT_EVENT_##k) return event_this_error(js); \
    return data->fields[index]; \
  }
#include "events/fields.h"

typedef struct {
  ant_cfunc_meta_t getter;
  ant_event_kind_t kind;
} event_field_getter_t;

static const event_field_getter_t event_field_getters[] = {
#define ANT_EVENT_FIELD(k, name, index) { { event_get_##k##_##name, "get " #name, 0, 0 }, ANT_EVENT_##k },
#include "events/fields.h"
};

ant_value_t Ant_Event_Create(
  ant_t *js, ant_value_t proto, ant_event_kind_t kind,
  ant_value_t type, unsigned int flags
) {
  ant_value_t event = Ant_Object_CreateBare(js);
  if (is_err(event)) return event;
  if (is_object_type(proto)) js_set_proto_init(event, proto);
  event_data_t *data = event_init(js, event, kind, type, flags & ANT_EVENT_BUBBLES, flags & ANT_EVENT_CANCELABLE);
  if (!data) return js_mkerr(js, "out of memory");
  data->trusted = (flags & ANT_EVENT_TRUSTED) != 0;
  return event;
}

ant_value_t Ant_Event_CreateTrusted(ant_t *js, ant_value_t proto, ant_event_kind_t kind, const char *type) {
  return Ant_Event_Create(js, proto, kind, js_mkstr(js, type, strlen(type)), ANT_EVENT_TRUSTED);
}

void Ant_Event_SetField(ant_t *js, ant_value_t event, unsigned int index, ant_value_t value) {
  event_data_t *data = get_event_data(event);
  if (data && index < data->field_count) event_set_value(js, event, &data->fields[index], value);
}

void Ant_Event_BeginDispatch(ant_t *js, ant_value_t event, ant_value_t target) {
  event_data_t *data = get_event_data(event);
  if (!data) {
    js_set(js, event, "target", target);
    js_set(js, event, "currentTarget", target);
    js_set(js, event, "eventPhase", js_mknum(2));
    return;
  }
  
  data->target = data->current_target = target;
  data->phase = js_mknum(2);
  data->dispatching = true;
  data->stop_immediate = false;
  gc_write_barrier(js, js_obj_ptr(event), target);
}

void Ant_Event_EndDispatch(ant_t *js, ant_value_t event) {
  event_data_t *data = get_event_data(event);
  if (!data) {
    js_set(js, event, "currentTarget", js_mknull());
    js_set(js, event, "eventPhase", js_mknum(0));
    return;
  }
  
  data->current_target = js_mknull();
  data->phase = js_mknum(0);
  data->dispatching = false;
}

void Ant_Event_DefineFields(ant_t *js, ant_value_t proto, ant_event_kind_t kind) {
  for (size_t i = 0; i < countof(event_field_getters); i++) {
    const event_field_getter_t *field = &event_field_getters[i];
    if (field->kind != kind) continue;
    const char *name = field->getter.name + 4;
    js_set_getter_desc(js, proto, name, strlen(name), js_mkfun_meta(&field->getter), JS_DESC_E | JS_DESC_C);
  }
}

static void event_define_prototype(ant_t *js, ant_value_t proto, ant_value_t ctor) {
  static const struct { const char *name; const ant_native_getter_t *getter; } values[] = {
    { "type",          &event_type_getter },
    { "target",        &event_target_getter },
    { "srcElement",    &event_target_getter },
    { "currentTarget", &event_current_target_getter },
    { "eventPhase",    &event_phase_getter },
    { "timeStamp",     &event_time_stamp_getter },
  };
  
  static const struct { const char *name; ant_cfunc_meta_t getter; ant_cfunc_meta_t setter; } flags[] = {
    { "bubbles",          { event_get_bubbles,           "get bubbles",          0, 0 }, { 0 } },
    { "cancelable",       { event_get_cancelable,        "get cancelable",       0, 0 }, { 0 } },
    { "defaultPrevented", { event_get_default_prevented, "get defaultPrevented", 0, 0 }, { 0 } },
    { "composed",         { event_get_composed,          "get composed",         0, 0 }, { 0 } },
    { "returnValue",      { event_get_return_value,      "get returnValue",      0, 0 }, { event_set_return_value,  "set returnValue",  1, 0 } },
    { "cancelBubble",     { event_get_cancel_bubble,     "get cancelBubble",     0, 0 }, { event_set_cancel_bubble, "set cancelBubble", 1, 0 } },
  };
  
  static const ant_cfunc_meta_t methods[] = {
    { js_event_preventDefault,           "preventDefault",           0, 0 },
    { js_event_stopPropagation,          "stopPropagation",          0, 0 },
    { js_event_stopImmediatePropagation, "stopImmediatePropagation", 0, 0 },
    { js_event_composedPath,             "composedPath",             0, 0 },
    { js_event_initEvent,                "initEvent",                1, 0 },
  };
  
  static const char *const phases[] = { "NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE" };
  
  for (size_t i = 0; i < countof(values); i++)
    js_set_getter_desc(js, proto, values[i].name, strlen(values[i].name), js_mkfun_meta(&values[i].getter->meta), JS_DESC_E | JS_DESC_C);
  
  for (size_t i = 0; i < countof(flags); i++) js_set_accessor_desc(
    js, proto, flags[i].name, strlen(flags[i].name), js_mkfun_meta(&flags[i].getter),
    flags[i].setter.fn ? js_mkfun_meta(&flags[i].setter) : js_mkundef(), JS_DESC_E | JS_DESC_C
  );
  
  js_set_accessor_desc(js, proto, "isTrusted", 9, Ant_Function_CreateWithMeta(js_event_get_isTrusted, "get isTrusted", 0, 0), js_mkundef(), JS_DESC_E);
  js_define_methods(js, proto, methods, countof(methods), ANT_PROP_ATTR_DEFAULT);
  defmethod_sym(js, proto, js->sym.inspect_sym, Ant_Function_Create(js_event_inspect));
  js_set_tostringtag(js, proto, js_mkstr(js, "Event", 5));
  
  for (size_t i = 0; i < countof(phases); i++) {
    ant_value_t key = js_mkstr(js, phases[i], strlen(phases[i]));
    mkprop_exact_attrs(js, proto, key, js_mknum((double)i), ANT_PROP_ATTR_ENUMERABLE);
    mkprop_exact_attrs(js, ctor, key, js_mknum((double)i), ANT_PROP_ATTR_ENUMERABLE);
  }
}

static ant_value_t js_eventemitter_ctor(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  if (is_object_type(this_obj)) return this_obj;

  if (vtype(call_new_target) != kTypeUndefined) {
    ant_value_t obj = Ant_Object_CreateBare(js);
    ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.eventemitter_proto, call_new_target);
    if (is_object_type(proto)) js_set_proto_init(obj, proto);
    return obj;
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "EventEmitter constructor requires an object receiver or 'new'");
}

static ant_value_t js_eventtarget_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "EventTarget constructor requires 'new'");

  ant_value_t this_obj = js_getthis(js);
  if (is_object_type(this_obj)) js_set_slot(this_obj, SLOT_BRAND, js_mknum(BRAND_EVENTTARGET));
  return js_mkundef();
}

static bool parse_addEventListener_options(ant_native_params_t, bool *once, bool *capture, ant_value_t *signal) {
  *once = false; *capture = false; *signal = js_mkundef();
  if (nargs < 3) return true;
  ant_value_t opts = args[2];
  if (vtype(opts) == kTypeBool) {
    *capture = js_truthy(js, opts);
    return true;
  }
  if (vtype(opts) != kTypeObject) return true;

  ant_value_t sig = js_get(js, opts, "signal");
  if (vtype(sig) == kTypeNull) return false;
  if (vtype(sig) != kTypeUndefined) {
    if (!abort_signal_is_signal(sig)) return false;
    *signal = sig;
  }

  ant_value_t o = js_get(js, opts, "once");
  ant_value_t ca = js_get(js, opts, "capture");
  if (vtype(o) != kTypeUndefined)  *once    = js_truthy(js, o);
  if (vtype(ca) != kTypeUndefined) *capture = js_truthy(js, ca);
  return true;
}

static int eventtarget_find_entry(EventType *evt, ant_value_t cb, bool capture) {
  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
    EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    if (entry_live(e) && e->callback == cb && entry_capture(evt, e) == capture) return (int)i;
  }
  return -1;
}

static bool eventtarget_remove_entry(ant_t *js, EventType *evt, ant_value_t cb, bool capture) {
  int index = evt ? eventtarget_find_entry(evt, cb, capture) : -1;
  if (index < 0) return false;
  evt_retire(js, evt, (unsigned int)index);
  return true;
}

static ant_value_t eventtarget_add_entry(
  ant_t *js, EventType *evt, ant_value_t cb,
  bool once, bool capture, ant_value_t signal
) {
  if (eventtarget_find_entry(evt, cb, capture) >= 0) return js_mkundef();

  EventListenerEntry entry = {
    .callback = cb,
    .dead_gen = 0,
    .meta = once ? ENTRY_ONCE : 0,
  };
  if (once || capture || vtype(signal) != kTypeUndefined) {
    EventListenerCold *cold = entry_ensure_cold(evt, &entry);
    if (!cold) return js_mkerr(js, "out of memory");
    cold->signal = signal;
    cold->capture = capture;
    emitter_write_barrier(js, evt->owner->target, signal);
  }
  utarray_push_back(evt->listeners, &entry);
  emitter_write_barrier(js, evt->owner->target, cb);
  return js_mkundef();
}

static ant_value_t add_listener_to(ant_native_params_t, EventType *evt) {
  if (!evt) return js_mkerr(js, "failed to create event type");

  bool once, capture;
  ant_value_t signal;
  if (!parse_addEventListener_options(js, args, nargs, &once, &capture, &signal))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to execute 'addEventListener': signal is not an AbortSignal");

  if (vtype(signal) != kTypeUndefined && abort_signal_is_aborted(signal)) return js_mkundef();

  ant_value_t cb = args[1];
  if (!is_callable(cb) && !is_object_type(cb)) return js_mkundef();
  return eventtarget_add_entry(js, evt, cb, once, capture, signal);
}

static ant_value_t remove_listener_from(ant_native_params_t, EventType *evt) {
  if (!evt) return js_mkundef();

  bool capture = false;
  if (nargs >= 3) {
  ant_value_t opts = args[2];
  if (vtype(opts) == kTypeBool) capture = js_truthy(js, opts);
  else if (vtype(opts) == kTypeObject) {
    ant_value_t ca = js_get(js, opts, "capture");
    if (vtype(ca) != kTypeUndefined) capture = js_truthy(js, ca);
  }}

  ant_value_t cb = (nargs >= 2) ? args[1] : js_mkundef();
  uint8_t cbt = vtype(cb);
  if (cbt == kTypeNull || cbt == kTypeUndefined) return js_mkundef();

  eventtarget_remove_entry(js, evt, cb, capture);
  return js_mkundef();
}

static ant_value_t eventtarget_rethrow(ant_params_t) {
  return js_throw(js, js_get_slot(js_getcurrentfunc(js), SLOT_DATA));
}

static void eventtarget_report_error(ant_t *js, ant_value_t result) {
  ant_value_t error = js_take_thrown(js, result);
  queue_next_tick(js, js_heavy_mkfun(js, eventtarget_rethrow, error));
}

static void eventtarget_invoke(ant_t *js, ant_value_t listener, ant_value_t target, ant_value_t *args) {
  ant_value_t fn = listener, this_val = target;
  
  if (!is_callable(listener)) {
    fn = js_get(js, listener, "handleEvent");
    this_val = listener;
    
    if (!is_err(fn) && !is_callable(fn))
       fn = js_mkerr_typed(js, JS_ERR_TYPE, "'handleEvent' property of event listener is not callable");
    
    if (is_err(fn)) {
      eventtarget_report_error(js, fn);
      return;
    }
  }
  
  ant_value_t result = eventemitter_call_listener(js, fn, this_val, args, 1);
  if (is_err(result) || Ant_Exception_Pending(js)) eventtarget_report_error(js, result);
}

static ant_value_t eventtarget_handler_listener(ant_params_t) {
  ant_value_t target = js_getthis(js);
  event_data_t *data = nargs > 0 ? get_event_data(args[0]) : NULL;
  ant_value_t type = data ? data->type : nargs > 0 ? js_get(js, args[0], "type") : js_mkundef();
  
  EventType *evt = evt_key_from_arg(type) ? find_emitter_event_type(js, target, type) : NULL;
  if (!evt || !is_callable(evt->handler)) return js_mkundef();
  return eventemitter_call_listener(js, evt->handler, target, args, nargs);
}

static ant_value_t eventtarget_handler_trampoline(void) {
  return Ant_Function_Create(eventtarget_handler_listener);
}

static ant_value_t dispatch_event_to(ant_t *js, ant_value_t event_obj, EventType *evt, ant_value_t target) {
  if (!evt) return js_true;

  event_data_t *data = get_event_data(event_obj);
  Ant_Event_BeginDispatch(js, event_obj, target);

  ant_value_t call_args[1] = { event_obj };
  ant_value_t trampoline = eventtarget_handler_trampoline();
  unsigned int n = utarray_len(evt->listeners);
  evt->generation++;

  evt->emitting++;
  for (unsigned int i = 0; i < n; i++) {
    EventListenerEntry *entry = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    ant_value_t cb = entry->callback;
    ant_value_t signal = entry_signal(evt, entry);

    if (!entry_live(entry)) continue;
    if (entry_once(entry)) evt_consume(js, evt, i);

    if (vtype(signal) != kTypeUndefined && abort_signal_is_aborted(signal)) {
      evt_retire(js, evt, i);
      continue;
    }

    if (cb == trampoline) {
      if (!is_callable(evt->handler)) continue;
      cb = evt->handler;
    }
    if (!is_object_type(cb) && !is_callable(cb)) continue;
    eventtarget_invoke(js, cb, target, call_args);
    if (data && data->stop_immediate) break;
  }
  
  evt->emitting--;
  evt_sweep(evt);
  Ant_Event_EndDispatch(js, event_obj);

  bool canceled = data && data->canceled;
  return js_bool(!canceled);
}

static ant_value_t js_add_event_listener_method(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkundef();
  return add_listener_to(js, args, nargs,
    find_or_create_emitter_event_type(js, js_getthis(js), key));
}

static ant_value_t js_add_event_listener(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkundef();
  return add_listener_to(js, args, nargs, find_or_create_global_event_type(js, key));
}

static ant_value_t js_remove_event_listener_method(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkundef();
  return remove_listener_from(js, args, nargs,
    find_emitter_event_type(js, js_getthis(js), key));
}

static ant_value_t js_remove_event_listener(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkundef();
  return remove_listener_from(js, args, nargs, find_global_event_type(js, key));
}

static ant_value_t js_dispatch_event_method(ant_params_t) {
  if (nargs < 1 || !is_object_type(args[0])) return js_false;
  ant_value_t this_obj = js_getthis(js);
  ant_value_t key = js_get(js, args[0], "type");
  if (!evt_key_from_arg(key)) return js_false;
  return dispatch_event_to(js, args[0],
    find_emitter_event_type(js, this_obj, key), this_obj);
}

static ant_value_t js_dispatch_event(ant_params_t) {
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
  if (t != kTypeFunction && t != kTypeBuiltin) return false;

  evt = find_or_create_emitter_event_type(js, target, key);
  if (!evt) return false;

  entry.callback = listener;
  entry.dead_gen = 0;
  entry.meta = once ? ENTRY_ONCE : 0;

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

  emitter_write_barrier(js, target, listener);
  int max_listeners = eventemitter_get_max_listeners_impl(target);
  
  unsigned int live = evt_live_count(evt);
  if (
    max_listeners > 0 &&
    !evt->warned_max_listeners &&
    (int)live > max_listeners
  ) {
    evt->warned_max_listeners = true;
    if (vtype(key) == kTypeString) fprintf(
      stderr,
      "Warning: Possible EventEmitter memory leak detected. "
      "%u %s listeners added. Use emitter.setMaxListeners() to increase limit.\n",
      live, js_str(js, key)
    );
    else fprintf(
      stderr,
      "Warning: Possible EventEmitter memory leak detected. "
      "%u listeners added for a Symbol event. Use emitter.setMaxListeners() to increase limit.\n",
      live
    );
    
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
  if (t != kTypeFunction && t != kTypeBuiltin) return false;

  evt = find_emitter_event_type(js, target, key);
  if (!evt) return false;

  for (unsigned int i = utarray_len(evt->listeners); i-- > 0;) {
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

static ant_value_t js_eventemitter_off(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "off requires 2 arguments (event, listener)");
  ant_value_t key = evt_key_from_arg(args[0]);
  
  if (!key) return js_mkerr(js, "event must be a string or Symbol");
  ant_value_t this_obj = js_getthis(js);
  eventemitter_remove_listener_impl(js, this_obj, key, args[1]);
  
  return this_obj;
}

static inline __attribute__((always_inline)) bool eventemitter_dispatch(
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

    if (entry->meta != 0 || entry->dead_gen != 0) {
      if (entry_consumed(entry)) continue;
      if (entry->dead_gen != 0 && entry->dead_gen < gen) continue;
      
      ant_value_t signal = entry_signal(evt, entry);
      if (entry_once(entry)) evt_consume(js, evt, i);

      if (vtype(signal) != kTypeUndefined && abort_signal_is_aborted(signal)) {
        evt_retire(js, evt, i);
        continue;
      }
    }

    if (vtype(cb) != kTypeFunction && vtype(cb) != kTypeBuiltin) continue;
    ant_value_t result = eventemitter_call_listener_inline(js, cb, target, args, nargs);
    invoked = true;

    if (is_err(result) || Ant_Exception_Pending(js)) break;
  }

  if (--evt->emitting == 0 && evt->dead_count == 0 && !evt->needs_reorder) evt->generation = 0;
  else evt_sweep(evt);

  return invoked;
}

static ant_value_t js_eventemitter_emit(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "emit requires at least 1 argument (event)");

  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkerr(js, "event must be a string or Symbol");
  
  ant_value_t target = js_getthis(js);
  if (!is_object_type(target)) return js_false;
  
  ant_event_list_t *events = find_emitter_events(target);
  if (!events) return js_false;
  
  bool invoked = eventemitter_dispatch(
    js, target, evt_list_find(js, events, key),
    nargs > 1 ? &args[1] : NULL, nargs - 1
  );

  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_bool(invoked);
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
  ant_event_list_t *events = NULL;
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
  ant_event_list_t *events = NULL;
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

ant_offset_t eventemitter_listener_count(
  ant_t *js,
  ant_value_t target, const char *event_type
) {
  ant_event_list_t *events = NULL;

  if (!is_object_type(target) || !event_type) return 0;
  events = find_emitter_events(target);
  if (!events) return 0;

  return (ant_offset_t)evt_live_count(evt_list_find_cstr(events, event_type, strlen(event_type)));
}

static ant_value_t js_eventemitter_add(ant_native_params_t, bool once, bool prepend) {
  if (nargs < 2) return js_mkerr(js, "requires 2 arguments (event, listener)");
  ant_value_t key = evt_key_from_arg(args[0]);
  
  if (!key) return js_mkerr(js, "event must be a string or Symbol");
  if (!eventemitter_add_listener_impl(js, js_getthis(js), key, args[1], once, prepend))
    return js_mkerr(js, "listener must be a function");
    
  return js_getthis(js);
}

static ant_value_t js_eventemitter_on(ant_params_t) {
  return js_eventemitter_add(js, args, nargs, false, false);
}

static ant_value_t js_eventemitter_once(ant_params_t) {
  return js_eventemitter_add(js, args, nargs, true, false);
}

static ant_value_t js_eventemitter_prepend_listener(ant_params_t) {
  return js_eventemitter_add(js, args, nargs, false, true);
}

static ant_value_t js_eventemitter_prepend_once_listener(ant_params_t) {
  return js_eventemitter_add(js, args, nargs, true, true);
}

static ant_value_t js_eventemitter_removeAllListeners(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  ant_event_list_t *events = NULL;

  if (nargs < 1 || vtype(args[0]) == kTypeUndefined) {
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

static ant_value_t js_eventemitter_listenerCount(ant_params_t) {
  if (nargs < 1) return js_mknum(0);
  ant_value_t key = evt_key_from_arg(args[0]);
  
  if (!key) return js_mknum(0);
  EventType *evt = find_emitter_event_type(js, js_getthis(js), key);
  
  if (!evt) return js_mknum(0);
  return js_mknum((double)evt_live_count(evt));
}

static ant_value_t js_eventemitter_setMaxListeners(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "setMaxListeners requires 1 argument");
  if (vtype(args[0]) != kTypeNumber) return js_mkerr(js, "n must be a number");

  int n = (int)js_getnum(args[0]);
  if (n < 0) return js_mkerr(js, "n must be non-negative");

  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "setMaxListeners requires an object receiver");
  js_set_slot(this_obj, SLOT_EVENT_MAX_LISTENERS, js_mknum(n));
  return this_obj;
}

static ant_value_t js_eventemitter_getMaxListeners(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  if (!is_object_type(this_obj)) return js_mknum(EVENTS_DEFAULT_MAX_LISTENERS);
  return js_mknum(eventemitter_get_max_listeners_impl(this_obj));
}

static ant_value_t js_eventemitter_rawListeners(ant_params_t) {
  if (nargs < 1) return js_mkarr(js);
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkarr(js);
  return eventemitter_get_listeners_array(js, js_getthis(js), key, true);
}

static ant_value_t js_eventemitter_listeners(ant_params_t) {
  if (nargs < 1) return js_mkarr(js);
  ant_value_t key = evt_key_from_arg(args[0]);
  if (!key) return js_mkarr(js);
  return eventemitter_get_listeners_array(js, js_getthis(js), key, false);
}

static ant_value_t js_eventemitter_eventNames(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t result = js_mkarr(js);
  
  ant_event_list_t *events = find_emitter_events(this_obj);
  if (events) {
    for (unsigned int i = 0; i < events->count; i++)
      if (evt_live_count(events->types[i]) > 0) js_arr_push(js, result, events->types[i]->js_key);
  }
  
  return result;
}

static ant_value_t Ant_Events_SettleOnce(ant_t *js, ant_value_t state) {
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t promise = js_get_slot(state, SLOT_DATA);
  if (vtype(promise) != kTypePromise || js_get_slot(state, SLOT_SETTLED) == js_true)
    return js_mkundef();

  js_set_slot(state, SLOT_SETTLED, js_true);
  ant_value_t signal, abort_listener;
  
  js_try_get_own_data_prop(js, state, "signal", 6, &signal);
  js_try_get_own_data_prop(js, state, "abortListener", 13, &abort_listener);
  
  if (abort_signal_is_signal(signal) && is_callable(abort_listener))
    abort_signal_remove_listener(js, signal, abort_listener);
  
  return promise;
}

static ant_value_t js_events_once_listener(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = Ant_Events_SettleOnce(js, state);
  if (vtype(promise) != kTypePromise) return js_mkundef();

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

static ant_value_t js_events_once_abort_listener(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = Ant_Events_SettleOnce(js, state);
  if (vtype(promise) != kTypePromise) return js_mkundef();

  ant_value_t signal = js_get(js, state, "signal");
  js_events_once_remove_listener_from_target(js, state);
    
  ant_value_t reason = abort_signal_get_reason(signal);
  if (vtype(reason) == kTypeUndefined) reason = js_mkerr(js, "The operation was aborted");
  js_reject_promise(js, promise, reason);
  
  return js_mkundef();
}

static bool js_events_once_is_abort_key(ant_t *js, ant_value_t key) {
  size_t key_len = 0;
  const char *key_str = vtype(key) == kTypeString ? js_getstr(js, key, &key_len) : NULL;
  return key_str && key_len == 5 && memcmp(key_str, "abort", 5) == 0;
}

static void js_events_once_reject_aborted(ant_t *js, ant_value_t promise, ant_value_t signal) {
  ant_value_t reason = abort_signal_get_reason(signal);
  if (vtype(reason) == kTypeUndefined) reason = js_mkerr(js, "The operation was aborted");
  js_reject_promise(js, promise, reason);
}

static ant_value_t js_events_once_attach(
  ant_t *js, ant_value_t target, ant_value_t key, 
  ant_value_t listener, ant_value_t signal
) {
  if (abort_signal_is_signal(target)) {
    if (!js_events_once_is_abort_key(js, key)) return js_mkerr_typed(
      js, JS_ERR_TYPE, "AbortSignal only supports the abort event");
    abort_signal_add_listener(js, target, listener);
    return js_mkundef();
  }

  ant_value_t on_method = js_getprop_fallback(js, target, "on");
  if (is_err(on_method)) return on_method;

  ant_value_t once_method = is_callable(on_method)
    ? js_getprop_fallback(js, target, "once")
    : js_mkundef();

  if (is_err(once_method)) return once_method;
    
  if (is_callable(once_method)) {
    ant_value_t call_args[2] = { key, listener };
    return eventemitter_call_listener(js, once_method, target, call_args, 2);
  }

  if (is_eventtarget_instance(target)) {
    ant_value_t listener_options = Ant_Object_CreateBare(js);
    js_set(js, listener_options, "once", js_true);
    if (abort_signal_is_signal(signal)) js_set(js, listener_options, "signal", signal);
    
    ant_value_t call_args[3] = { key, listener, listener_options };
    return add_listener_to(js, call_args, 3, find_or_create_emitter_event_type(js, target, key));
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "target is not an EventEmitter or EventTarget");
}

static ant_value_t js_events_once(ant_params_t) {
  if (nargs < 2 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "events.once requires an emitter and event name");

  ant_value_t key = evt_key_from_arg(args[1]);
  if (!key) return js_mkerr_typed(js, JS_ERR_TYPE, "event must be a string or Symbol");

  ant_value_t promise = js_mkpromise(js);
  if (is_err(promise)) return promise;

  ant_value_t state = Ant_Object_CreateBare(js);
  if (is_err(state)) { 
    js_reject_promise(js, promise, state);
    return promise;
  }
  
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

  ant_value_t result = js_events_once_attach(js, target, key, listener, signal);
  if (is_err(result)) {
    Ant_Events_SettleOnce(js, state);
    js_reject_promise(js, promise, result);
  }
  
  return promise;
}

static ant_value_t js_events_disposable_dispose(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t disposed = js_get_slot(state, SLOT_SETTLED);
  if (vtype(disposed) == kTypeBool && disposed == js_true) return js_mkundef();
  js_set_slot(state, SLOT_SETTLED, js_true);

  ant_value_t signal = js_get(js, state, "signal");
  ant_value_t listener = js_get(js, state, "listener");
  if (abort_signal_is_signal(signal) && is_callable(listener))
    abort_signal_remove_listener(js, signal, listener);

  return js_mkundef();
}

static ant_value_t js_events_add_abort_listener(ant_params_t) {
  if (nargs < 2 || !abort_signal_is_signal(args[0]) || !is_callable(args[1]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "events.addAbortListener requires an AbortSignal and listener");

  bool already_aborted = abort_signal_is_aborted(args[0]);
  if (already_aborted) {
    ant_value_t event = Ant_Event_CreateTrusted(js, js->builtins.event_proto, ANT_EVENT_BASE, "abort");
    if (is_err(event)) return event;
    Ant_Event_BeginDispatch(js, event, args[0]);
    eventemitter_call_listener(js, args[1], args[0], &event, 1);
    Ant_Event_EndDispatch(js, event);
  } else abort_signal_add_listener(js, args[0], args[1]);

  ant_value_t state = Ant_Object_CreateBare(js);
  js_set_slot(state, SLOT_SETTLED, js_bool(already_aborted));
  js_set(js, state, "signal", args[0]);
  js_set(js, state, "listener", args[1]);

  ant_value_t disposable = Ant_Object_CreateBare(js);
  js_set(js, disposable, "dispose", js_heavy_mkfun(js, js_events_disposable_dispose, state));
  
  return disposable;
}

static ant_value_t js_events_set_max_listeners(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "setMaxListeners requires at least 1 argument");
  if (vtype(args[0]) != kTypeNumber) return js_mkerr(js, "n must be a number");

  int n = (int)js_getnum(args[0]);
  if (n < 0) return js_mkerr(js, "n must be non-negative");

  for (int i = 1; i < nargs; i++) {
    if (!is_object_type(args[i])) continue;
    js_set_slot(args[i], SLOT_EVENT_MAX_LISTENERS, js_mknum(n));
  }

  return js_mkundef();
}

static ant_value_t js_events_get_max_listeners(ant_params_t) {
  if (nargs < 1) return js_mknum(EVENTS_DEFAULT_MAX_LISTENERS);
  if (!is_object_type(args[0])) return js_mknum(EVENTS_DEFAULT_MAX_LISTENERS);
  return js_mknum(eventemitter_get_max_listeners_impl(args[0]));
}

static ant_value_t js_events_get_event_listeners(ant_params_t) {
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

static void events_on_queue_compact(ant_t *js, ant_value_t state, const char *arr_name, const char *head_name) {
  ant_value_t arr = js_get(js, state, arr_name);
  ant_value_t head_val = js_get(js, state, head_name);
  
  ant_offset_t head = vtype(head_val) == kTypeNumber ? (ant_offset_t)js_getnum(head_val) : 0;
  ant_offset_t len = vtype(arr) == kTypeArray ? js_arr_len(js, arr) : 0;
  ant_value_t compact = 0;

  if (vtype(arr) != kTypeArray || head == 0) return;
  if (head < len && (head < 32 || head * 2 < len)) return;

  compact = js_mkarr(js);
  for (ant_offset_t i = head; i < len; i++)
    js_arr_push(js, compact, js_arr_get(js, arr, i));

  js_set(js, state, arr_name, compact);
  js_set(js, state, head_name, js_mknum(0));
}

static ant_value_t events_on_queue_shift(ant_t *js, ant_value_t state, const char *arr_name, const char *head_name) {
  ant_value_t arr = js_get(js, state, arr_name);
  ant_value_t head_val = js_get(js, state, head_name);
  
  ant_offset_t head = vtype(head_val) == kTypeNumber ? (ant_offset_t)js_getnum(head_val) : 0;
  ant_value_t value = 0;

  if (vtype(arr) != kTypeArray || head >= js_arr_len(js, arr)) return js_mkundef();
  js_set(js, state, head_name, js_mknum((double)(head + 1)));
  
  value = js_arr_get(js, arr, head);
  events_on_queue_compact(js, state, arr_name, head_name);

  return value;
}

static ant_value_t events_on_remover(ant_t *js, ant_value_t target) {
  static const char *names[] = { "removeListener", "off", "removeEventListener" };

  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    ant_value_t remove_method = js_getprop_fallback(js, target, names[i]);
    if (is_callable(remove_method)) return remove_method;
  }

  return js_mkundef();
}

static void events_on_remove_one(ant_t *js, ant_value_t target, ant_value_t key, ant_value_t listener) {
  if (!is_object_type(target) || !key || !is_callable(listener)) return;

  ant_value_t remove_method = events_on_remover(js, target);
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
    if (vtype(parked) != kTypePromise) return;
    js_resolve_promise(js, parked, Ant_Iterator_CreateResult(js, js_mkundef(), true));
  }
}

static void events_on_fail(ant_t *js, ant_value_t state, ant_value_t reason) {
  events_on_detach(js, state);

  ant_value_t parked = events_on_queue_shift(js, state, "parked", "parkedHead");
  if (vtype(parked) == kTypePromise) {
    js_reject_promise(js, parked, reason);
    events_on_finish_parked(js, state);
  } else js_set(js, state, "storedError", reason);
}

static ant_value_t js_events_on_event_cb(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();

  ant_value_t values = js_mkarr(js);
  for (int i = 0; i < nargs; i++) js_arr_push(js, values, args[i]);

  ant_value_t parked = events_on_queue_shift(js, state, "parked", "parkedHead");
  if (vtype(parked) == kTypePromise)
    js_resolve_promise(js, parked, Ant_Iterator_CreateResult(js, values, false));
  else js_arr_push(js, js_get(js, state, "buffer"), values);

  return js_mkundef();
}

static ant_value_t js_events_on_error_cb(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();
  events_on_fail(js, state, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t js_events_on_abort_cb(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (!is_object_type(state)) return js_mkundef();
  events_on_fail(js, state, abort_signal_make_error(js, js_get(js, state, "signal")));
  return js_mkundef();
}

static ant_value_t js_events_on_next(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_mkpromise(js);
  if (!is_object_type(state)) return promise;

  // drain what arrived before the failure; the error surfaces after
  ant_value_t buffered = events_on_queue_shift(js, state, "buffer", "bufferHead");
  if (!is_undefined(buffered)) {
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, buffered, false));
    return promise;
  }

  ant_value_t stored = js_get(js, state, "storedError");
  if (!is_undefined(stored)) {
    js_set(js, state, "storedError", js_mkundef());
    js_reject_promise(js, promise, stored);
    return promise;
  }

  if (js_truthy(js, js_get(js, state, "finished"))) {
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, js_mkundef(), true));
    return promise;
  }

  js_arr_push(js, js_get(js, state, "parked"), promise);
  return promise;
}

static ant_value_t js_events_on_return(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_mkpromise(js);

  if (is_object_type(state)) {
    events_on_detach(js, state);
    events_on_finish_parked(js, state);
  }

  js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, nargs > 0 ? args[0] : js_mkundef(), true));
  return promise;
}

static ant_value_t js_events_on_self(ant_params_t) {
  return js_getthis(js);
}

static ant_value_t js_events_on(ant_params_t) {
  if (nargs < 2 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "events.on requires an emitter and an event name");

  ant_value_t target = args[0];
  ant_value_t key = evt_key_from_arg(args[1]);
  if (!key) return js_mkerr_typed(js, JS_ERR_TYPE, "event name must be a string or Symbol");

  ant_value_t options = nargs >= 3 && is_object_type(args[2]) ? args[2] : js_mkundef();
  ant_value_t signal = is_object_type(options) ? js_get(js, options, "signal") : js_mkundef();
  if (abort_signal_is_signal(signal) && abort_signal_is_aborted(signal))
    return js_throw(js, abort_signal_make_error(js, signal));

  ant_value_t state = Ant_Object_CreateBare(js);
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
    ant_value_t registered = 0;

    if (!is_callable(events_on_remover(js, target)) && !is_eventtarget_instance(target))
      return js_mkerr_typed(
        js, JS_ERR_TYPE,
        "target must provide removeListener(), off() or removeEventListener()"
      );

    registered = eventemitter_call_listener(js, on_method, target, on_args, 2);
    if (is_err(registered)) return registered;

    size_t key_len = 0;
    const char *key_str = vtype(key) == kTypeString ? js_getstr(js, key, &key_len) : NULL;
    bool key_is_error = key_str && key_len == 5 && memcmp(key_str, "error", 5) == 0;

    if (!key_is_error) {
      on_args[0] = error_key; on_args[1] = error_listener;
      registered = eventemitter_call_listener(js, on_method, target, on_args, 2);

      if (is_err(registered)) {
        events_on_remove_one(js, target, key, listener);
        return registered;
      }
    }
  } else if (is_eventtarget_instance(target)) eventemitter_add_listener_val(js, target, key, listener, false);
  else return js_mkerr_typed(js, JS_ERR_TYPE, "target is not an EventEmitter or EventTarget");

  if (abort_signal_is_signal(signal)) {
    ant_value_t abort_listener = js_heavy_mkfun(js, js_events_on_abort_cb, state);
    js_set(js, state, "signal", signal);
    js_set(js, state, "abortListener", abort_listener);
    abort_signal_add_listener(js, signal, abort_listener);
  }

  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set(js, iter, "next", js_heavy_mkfun(js, js_events_on_next, state));
  js_set(js, iter, "return", js_heavy_mkfun(js, js_events_on_return, state));
  js_set_sym(js, iter, js->sym.asyncIterator_sym, Ant_Function_Create(js_events_on_self));
  return iter;
}

ant_value_t events_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  
  eventemitter_prototype(js);
  js_set_module_default(js, lib, js->builtins.eventemitter_ctor, "EventEmitter");
  js_set(js, lib, "once", Ant_Function_CreateWithArity(js_events_once, 2));
  js_set(js, lib, "on", Ant_Function_CreateWithArity(js_events_on, 2));
  js_set(js, lib, "addAbortListener", Ant_Function_CreateWithArity(js_events_add_abort_listener, 2));
  js_set(js, lib, "setMaxListeners", Ant_Function_Create(js_events_set_max_listeners));
  js_set(js, lib, "getMaxListeners", Ant_Function_CreateWithArity(js_events_get_max_listeners, 1));
  js_set(js, lib, "getEventListeners", Ant_Function_CreateWithArity(js_events_get_event_listeners, 2));
  js_set(js, js->builtins.eventemitter_ctor, "once", js_get(js, lib, "once"));
  js_set(js, js->builtins.eventemitter_ctor, "on", js_get(js, lib, "on"));
  js_set(js, js->builtins.eventemitter_ctor, "addAbortListener", js_get(js, lib, "addAbortListener"));
  js_set(js, js->builtins.eventemitter_ctor, "setMaxListeners", js_get(js, lib, "setMaxListeners"));
  js_set(js, js->builtins.eventemitter_ctor, "getMaxListeners", js_get(js, lib, "getMaxListeners"));
  js_set(js, js->builtins.eventemitter_ctor, "getEventListeners", js_get(js, lib, "getEventListeners"));
  js_set_tostringtag(js, lib, js_mkstr(js, "events", 6));
  
  return lib;
}

ant_value_t eventemitter_prototype(ant_t *js) {
  if (js->builtins.eventemitter_proto) return js->builtins.eventemitter_proto;

  ant_value_t object_proto = js->sym.object_proto;
  ant_value_t eventemitter_proto = Ant_Object_CreateBare(js);
  if (is_object_type(object_proto)) js_set_proto_init(eventemitter_proto, object_proto);

  js_set(js, eventemitter_proto, "on",                 Ant_Function_CreateWithArity(js_eventemitter_on, 2));
  js_set_exact(js, eventemitter_proto, "addListener",  js_get(js, eventemitter_proto, "on"));
  js_set(js, eventemitter_proto, "once",               Ant_Function_CreateWithArity(js_eventemitter_once, 2));
  js_set(js, eventemitter_proto, "prependListener",    Ant_Function_CreateWithArity(js_eventemitter_prepend_listener, 2));
  js_set(js, eventemitter_proto, "prependOnceListener", Ant_Function_CreateWithArity(js_eventemitter_prepend_once_listener, 2));
  js_set(js, eventemitter_proto, "off",                Ant_Function_CreateWithArity(js_eventemitter_off, 2));
  js_set_exact(js, eventemitter_proto, "removeListener", js_get(js, eventemitter_proto, "off"));
  js_set(js, eventemitter_proto, "emit",               Ant_Function_CreateWithArity(js_eventemitter_emit, 1));
  js_set(js, eventemitter_proto, "removeAllListeners", Ant_Function_CreateWithArity(js_eventemitter_removeAllListeners, 1));
  js_set(js, eventemitter_proto, "listenerCount",      Ant_Function_CreateWithArity(js_eventemitter_listenerCount, 2));
  js_set(js, eventemitter_proto, "setMaxListeners",    Ant_Function_CreateWithArity(js_eventemitter_setMaxListeners, 1));
  js_set(js, eventemitter_proto, "getMaxListeners",    Ant_Function_Create(js_eventemitter_getMaxListeners));
  js_set(js, eventemitter_proto, "listeners",          Ant_Function_CreateWithArity(js_eventemitter_listeners, 1));
  js_set(js, eventemitter_proto, "rawListeners",       Ant_Function_CreateWithArity(js_eventemitter_rawListeners, 1));
  js_set(js, eventemitter_proto, "eventNames",         Ant_Function_Create(js_eventemitter_eventNames));
  js_set_tostringtag(js, eventemitter_proto, js_mkstr(js, "EventEmitter", 12));

  js->builtins.eventemitter_proto = eventemitter_proto;
  js->builtins.eventemitter_ctor = Ant_Function_CreateConstructor(js, js_eventemitter_ctor, eventemitter_proto, "EventEmitter", 1);
  
  return js->builtins.eventemitter_proto;
}

void init_events_module(ant_t *js) {
  ant_value_t global = js_glob(js);
  
  js->builtins.event_proto = Ant_Object_CreateBare(js);
  ant_value_t event_fn = Ant_Function_CreateIntrinsicConstructor(js, js_event_ctor, js->builtins.event_proto, "Event", 1);
  event_define_prototype(js, js->builtins.event_proto, event_fn);
  js_set_global_builtin(js, "Event", event_fn);

  js->builtins.customevent_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.customevent_proto, js->builtins.event_proto);
  js_set_tostringtag(js, js->builtins.customevent_proto, js_mkstr(js, "CustomEvent", 11));
  Ant_Event_DefineFields(js, js->builtins.customevent_proto, ANT_EVENT_CUSTOM);

  ant_value_t customevent_fn = Ant_Function_CreateIntrinsicConstructor(js, js_customevent_ctor, js->builtins.customevent_proto, "CustomEvent", 1);
  js_set_global_builtin(js, "CustomEvent", customevent_fn);

  js->builtins.errorevent_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.errorevent_proto, js->builtins.event_proto);
  js_set_tostringtag(js, js->builtins.errorevent_proto, js_mkstr(js, "ErrorEvent", 10));
  Ant_Event_DefineFields(js, js->builtins.errorevent_proto, ANT_EVENT_ERROR);

  ant_value_t errorevent_fn = Ant_Function_CreateIntrinsicConstructor(js, js_errorevent_ctor, js->builtins.errorevent_proto, "ErrorEvent", 2);
  js_set_global_builtin(js, "ErrorEvent", errorevent_fn);

  js->builtins.promiserejectionevent_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.promiserejectionevent_proto, js->builtins.event_proto);
  js_set_tostringtag(js, js->builtins.promiserejectionevent_proto, js_mkstr(js, "PromiseRejectionEvent", 21));
  Ant_Event_DefineFields(js, js->builtins.promiserejectionevent_proto, ANT_EVENT_PROMISE_REJECTION);

  ant_value_t pre_fn = Ant_Function_CreateIntrinsicConstructor(js, js_promiserejectionevent_ctor, js->builtins.promiserejectionevent_proto, "PromiseRejectionEvent", 2);
  js_set_global_builtin(js, "PromiseRejectionEvent", pre_fn);

  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t eventtarget_proto = Ant_Object_CreateBare(js);
  js->builtins.eventtarget_proto = eventtarget_proto;
  if (is_object_type(object_proto)) js_set_proto_init(eventtarget_proto, object_proto);
  js_set(js, eventtarget_proto, "addEventListener",    Ant_Function_CreateWithArity(js_add_event_listener_method, 2));
  js_set(js, eventtarget_proto, "removeEventListener", Ant_Function_CreateWithArity(js_remove_event_listener_method, 2));
  js_set(js, eventtarget_proto, "dispatchEvent",       Ant_Function_CreateWithArity(js_dispatch_event_method, 1));
  js_set_tostringtag(js, eventtarget_proto, js_mkstr(js, "EventTarget", 11));

  ant_value_t eventtarget_fn = Ant_Function_CreateIntrinsicConstructor(js, js_eventtarget_ctor, eventtarget_proto, "EventTarget", 0);
  js->builtins.eventtarget_ctor = eventtarget_fn;

  js_set(js, global, "addEventListener",    Ant_Function_Create(js_add_event_listener));
  js_set(js, global, "removeEventListener", Ant_Function_Create(js_remove_event_listener));
  js_set(js, global, "dispatchEvent",       Ant_Function_Create(js_dispatch_event));
  js_set_global_builtin(js, "EventTarget", eventtarget_fn);
}

bool Ant_EventTarget_AddListener(ant_t *js, ant_value_t target, ant_value_t type, ant_value_t listener) {
  if (!is_object_type(target) || !is_callable(listener)) return false;
  EventType *evt = find_or_create_emitter_event_type(js, target, type);
  return evt && !is_err(eventtarget_add_entry(js, evt, listener, false, false, js_mkundef()));
}

bool Ant_EventTarget_RemoveListener(ant_t *js, ant_value_t target, ant_value_t type, ant_value_t listener) {
  EventType *evt = is_object_type(target) ? find_emitter_event_type(js, target, type) : NULL;
  return eventtarget_remove_entry(js, evt, listener, false);
}

ant_value_t Ant_EventTarget_CreateEvent(ant_t *js, ant_value_t target, ant_value_t proto, ant_event_kind_t kind, const char *type) {
  EventType *evt = is_object_type(target) ? find_emitter_event_type_cstr(js, target, type, strlen(type)) : NULL;
  if (evt_live_count(evt) == 0) return js_mkundef();
  return Ant_Event_Create(js, proto, kind, evt->js_key, ANT_EVENT_TRUSTED);
}

ant_value_t Ant_EventTarget_GetHandler(ant_t *js, ant_value_t target, const char *type, size_t len) {
  EventType *evt = is_object_type(target) ? find_emitter_event_type_cstr(js, target, type, len) : NULL;
  return evt ? evt->handler : js_mknull();
}

void Ant_EventTarget_SetHandler(ant_t *js, ant_value_t target, const char *type, size_t len, ant_value_t handler) {
  if (!is_object_type(target)) return;
  EventType *evt = find_emitter_event_type_cstr(js, target, type, len);
  ant_value_t trampoline = eventtarget_handler_trampoline();

  if (!is_object_type(handler)) {
    if (!evt) return;
    evt->handler = js_mknull();
    eventtarget_remove_entry(js, evt, trampoline, false);
    return;
  }

  if (!evt) evt = find_or_create_emitter_event_type(js, target, js_mkstr(js, type, len));
  if (!evt) return;
  evt->handler = handler;
  emitter_write_barrier(js, target, handler);
  eventtarget_add_entry(js, evt, trampoline, false, false, js_mkundef());
}

void Ant_EventTarget_Dispatch(ant_t *js, ant_value_t target, ant_value_t event) {
  event_data_t *data = get_event_data(event);
  ant_value_t type = data ? data->type : js_get(js, event, "type");
  EventType *evt = evt_key_from_arg(type) ? find_emitter_event_type(js, target, type) : NULL;
  dispatch_event_to(js, event, evt, target);
}

static void mark_event_type_listeners(ant_t *js, gc_mark_fn mark, ant_event_list_t *list) {
  if (!list) return;
  for (unsigned int t = 0; t < list->count; t++) {
  EventType *evt = list->types[t];
  if (vtype(evt->js_key) == kTypeString || vtype(evt->js_key) == kTypeSymbol) mark(js, evt->js_key);
  mark(js, evt->handler);
  for (unsigned int i = 0; i < utarray_len(evt->listeners); i++) {
    EventListenerEntry *e = (EventListenerEntry *)utarray_eltptr(evt->listeners, i);
    mark(js, e->callback);
    ant_value_t raw_callback = entry_raw_callback(evt, e);
    ant_value_t signal = entry_signal(evt, e);
    if (vtype(raw_callback) != kTypeUndefined) mark(js, raw_callback);
    if (vtype(signal) != kTypeUndefined) mark(js, signal);
  }
}}

void cleanup_events_module(ant_t *js) {
  if (!js || !js->global_events) return;
  evt_list_free(js->global_events);
  free(js->global_events);
  js->global_events = NULL;
}

void gc_mark_events(ant_t *js, gc_mark_fn mark) {
  if (js->global_events) mark_event_type_listeners(js, mark, js->global_events);
}

void gc_mark_eventemitter_object(ant_t *js, ant_value_t obj, gc_mark_fn mark) {
  ant_event_list_t *events = find_emitter_events(obj);
  if (events) mark_event_type_listeners(js, mark, events);
  
  event_data_t *data = get_event_data(obj);
  if (!data) return;
  
  mark(js, data->type);
  mark(js, data->target);
  mark(js, data->current_target);
  for (uint8_t i = 0; i < data->field_count; i++) mark(js, data->fields[i]);
}

void gc_finalize_events_object(ant_t *js, ant_value_t obj) {
  event_data_t *data = (event_data_t *)js_get_native(obj, EVENT_NATIVE_TAG);
  ant_event_list_t *events = find_emitter_events(obj);

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
