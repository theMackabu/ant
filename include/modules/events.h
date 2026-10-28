#ifndef EVENTS_H
#define EVENTS_H

#include <stdbool.h>
#include "types.h"
#include "cfunc.h"

#define EVENTS_DEFAULT_MAX_LISTENERS 10

typedef void (*eventemitter_listener_change_fn)(
  ant_t *js,
  ant_value_t target,
  ant_value_t key,
  ant_offset_t listener_count,
  void *context
);

ant_value_t events_library(ant_t *js);
ant_value_t eventemitter_prototype(ant_t *js);
ant_offset_t eventemitter_listener_count(ant_t *js, ant_value_t target, const char *event_type);
ant_value_t eventemitter_sole_listener(ant_value_t target, const char *event_type);
bool eventemitter_emit_args_counted(
  ant_t *js, ant_value_t target, const char *event_type,
  ant_value_t *args, int nargs, ant_offset_t *before, ant_offset_t *after
);

typedef struct {
  const void *list;
  void *evt;
  const char *name;
  uint32_t gen;
} eventemitter_ref_t;

bool eventemitter_emit_ref(ant_t *js, ant_value_t target, eventemitter_ref_t *ref, ant_value_t *args, int nargs);
bool eventemitter_emit_ref_counted(ant_t *js, ant_value_t target, eventemitter_ref_t *ref, ant_value_t *args, int nargs, ant_offset_t *before, ant_offset_t *after);
ant_value_t eventemitter_sole_listener_ref(ant_value_t target, eventemitter_ref_t *ref);
ant_offset_t eventemitter_listener_count_ref(ant_value_t target, eventemitter_ref_t *ref);

void init_events_module(ant_t *js);
void cleanup_events_module(ant_t *js);
void js_dispatch_global_event(ant_t *js, ant_value_t event_obj);

bool eventemitter_add_listener(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t listener, bool once
);

bool eventemitter_add_listener_val(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t listener, bool once
);

bool eventemitter_set_listener_change_hook(
  ant_t *js,
  ant_value_t target,
  eventemitter_listener_change_fn hook,
  void *context
);

bool eventemitter_emit_args(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t *args, int nargs
);

bool eventemitter_remove_listener(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t listener
);

bool eventemitter_remove_listener_val(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t listener
);


typedef enum: uint8_t {
  ANT_EVENT_BASE,
  ANT_EVENT_CUSTOM,
  ANT_EVENT_ERROR,
  ANT_EVENT_PROMISE_REJECTION,
  ANT_EVENT_MESSAGE,
  ANT_EVENT_CLOSE,
} ant_event_kind_t;

enum {
#define ANT_EVENT_FIELD(k, name, index) ANT_EVENT_FIELD_##k##_##name = index,
#include "events/fields.h"
};

enum {
  ANT_EVENT_BUBBLES = 1 << 0,
  ANT_EVENT_CANCELABLE = 1 << 1,
  ANT_EVENT_TRUSTED = 1 << 2,
};

ant_value_t Ant_Event_Create(
  ant_t *js, ant_value_t proto, ant_event_kind_t kind,
  ant_value_t type, unsigned int flags
);

ant_value_t Ant_GlobalEvents_GetHandler(ant_t *js, const char *type, size_t len);
ant_value_t Ant_Event_CreateTrusted(ant_t *js, ant_value_t proto, ant_event_kind_t kind, const char *type);
ant_value_t Ant_EventTarget_CreateEvent(ant_t *js, ant_value_t target, ant_value_t proto, ant_event_kind_t kind, const char *type);
ant_value_t Ant_EventTarget_GetHandler(ant_t *js, ant_value_t target, const char *type, size_t len);

void Ant_Event_BeginDispatch(ant_t *js, ant_value_t event, ant_value_t target);
void Ant_Event_EndDispatch(ant_t *js, ant_value_t event);
void Ant_Event_SetField(ant_t *js, ant_value_t event, unsigned int index, ant_value_t value);
void Ant_Event_DefineFields(ant_t *js, ant_value_t proto, ant_event_kind_t kind);
void Ant_EventTarget_Dispatch(ant_t *js, ant_value_t target, ant_value_t event);
void Ant_EventTarget_SetHandler(ant_t *js, ant_value_t target, const char *type, size_t len, ant_value_t handler);
void Ant_GlobalEvents_SetHandler(ant_t *js, const char *type, size_t len, ant_value_t handler);
void Ant_GlobalEvents_SetListenerChangeHook(ant_t *js, eventemitter_listener_change_fn hook, void *context);

bool Ant_Event_Canceled(ant_value_t event);
bool Ant_EventTarget_AddListener(ant_t *js, ant_value_t target, ant_value_t type, ant_value_t listener);
bool Ant_EventTarget_RemoveListener(ant_t *js, ant_value_t target, ant_value_t type, ant_value_t listener);
bool Ant_EventTarget_AddDataListener(ant_t *js, ant_value_t target, ant_value_t type, ant_value_t listener, bool once, bool prepend);

#define ANT_EVENT_HANDLER_ACCESSORS(prefix, type, invalid_this)              \
  static ant_value_t prefix##_get_on##type(ant_params_t) {                   \
    ant_value_t self = js_getthis(js);                                       \
    ant_value_t invalid = invalid_this(js, self);                            \
    if (invalid) return invalid;                                             \
    return Ant_EventTarget_GetHandler(js, self, #type, sizeof(#type) - 1);   \
  }                                                                          \
  static ant_value_t prefix##_set_on##type(ant_params_t) {                   \
    ant_value_t self = js_getthis(js);                                       \
    ant_value_t invalid = invalid_this(js, self);                            \
    if (invalid) return invalid;                                             \
    ant_value_t handler = nargs > 0 ? args[0] : js_mknull();                 \
    Ant_EventTarget_SetHandler(js, self, #type, sizeof(#type) - 1, handler); \
    return js_mkundef();                                                     \
  }

#define ANT_EVENT_HANDLER_DEFINE(proto, prefix, type)                        \
  js_set_accessor_desc(js, proto, "on" #type, sizeof("on" #type) - 1,        \
    Ant_Function_Create(prefix##_get_on##type),                              \
    Ant_Function_CreateWithArity(prefix##_set_on##type, 1),                  \
    JS_DESC_E | JS_DESC_C)

#endif
