#include <stdlib.h>
#include <string.h>
#include <uthash.h>
#include <uv.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "list.h"
#include "ptr.h"
#include "sync.h"
#include "silver/call.h"

#include "gc/modules.h"
#include "modules/domexception.h"
#include "modules/events.h"
#include "modules/message_port.h"
#include "modules/timer.h"
#include "modules/wire.h"

typedef struct ant_port_message {
  struct ant_port_message *next;
  ant_wire_t *wire;
} ant_port_message_t;

typedef struct {
  ant_port_message_t *head;
  ant_port_message_t *tail;
  ant_port_binding_t *binding;
  bool close_pending;
  bool held;
} port_side_t;

typedef struct ant_channel {
  _Atomic int refs;
  ant_mutex_t lock;
  bool entangled;
  port_side_t side[2];
} ant_channel_t;

struct ant_port_end {
  ant_channel_t *ch;
  uint8_t side;
};

struct ant_port_binding {
  ant_t *js;
  ant_port_end_t *end;
  ant_value_t target;
  ant_port_message_fn on_message;
  uv_async_t async;
  bool async_open;
  bool started;
  bool refed;
  bool has_listeners;
  bool global_listeners;
  bool mirror_global;
  bool closed;
  bool draining;
  ant_port_binding_t *next;
  ant_port_binding_t **pprev;
};

typedef struct broadcast_group {
  char *name;
  broadcast_member_t *members;
  UT_hash_handle hh;
} broadcast_group_t;

struct broadcast_member {
  ant_t *js;
  ant_value_t obj;
  broadcast_group_t *group;
  ant_port_message_t *head;
  ant_port_message_t *tail;
  uv_async_t async;
  bool async_open;
  bool handle_closed;
  bool obj_alive;
  bool refed;
  bool closed;
  broadcast_member_t *group_next;
  broadcast_member_t *next;
  broadcast_member_t **pprev;
};

static constexpr uint32_t MESSAGE_PORT_NATIVE_TAG = Ant_NativeTag("MSGP");
static constexpr uint32_t BROADCAST_NATIVE_TAG = Ant_NativeTag("BCST");

static ant_mutex_t broadcast_lock = ANT_MUTEX_INIT;
static broadcast_group_t *broadcast_groups = NULL;

static void message_free(ant_port_message_t *msg) {
  ant_wire_release(msg->wire);
  free(msg);
}

static void queue_free(ant_port_message_t *head) {
  while (head) {
    ant_port_message_t *next = head->next;
    message_free(head);
    head = next;
  }
}

static void channel_release(ant_channel_t *ch) {
  if (atomic_fetch_sub_explicit(&ch->refs, 1, memory_order_acq_rel) != 1) return;
  queue_free(ch->side[0].head);
  queue_free(ch->side[1].head);
  free(ch);
}

static void binding_notify_locked(ant_port_binding_t *binding) {
  if (binding && binding->async_open) uv_async_send(&binding->async);
}

static void channel_disentangle_locked(ant_channel_t *ch) {
  if (!ch->entangled) return;
  ch->entangled = false;
  for (int i = 0; i < 2; i++) {
    ch->side[i].close_pending = true;
    binding_notify_locked(ch->side[i].binding);
  }
}

bool ant_port_end_pair(ant_port_end_t **a, ant_port_end_t **b) {
  ant_channel_t *ch = calloc(1, sizeof(*ch));
  ant_port_end_t *ea = calloc(1, sizeof(*ea));
  ant_port_end_t *eb = calloc(1, sizeof(*eb));
  
  if (!ch || !ea || !eb) {
    free(ch);
    free(ea);
    free(eb);
    return false;
  }

  ch->lock = (ant_mutex_t)ANT_MUTEX_INIT;
  ch->entangled = true;
  ch->side[0].held = ch->side[1].held = true;
  
  atomic_init(&ch->refs, 2);
  *ea = (ant_port_end_t){ ch, 0 };
  *eb = (ant_port_end_t){ ch, 1 };
  *a = ea;
  *b = eb;
  
  return true;
}

void ant_port_end_release(ant_port_end_t *end) {
  if (!end) return;
  ant_channel_t *ch = end->ch;
  ant_mutex_lock(&ch->lock);
  ch->side[end->side].held = false;
  channel_disentangle_locked(ch);
  ant_mutex_unlock(&ch->lock);
  channel_release(ch);
  free(end);
}

static bool end_push(ant_port_end_t *end, ant_wire_t *wire) {
  ant_port_message_t *msg = calloc(1, sizeof(*msg));
  if (!msg) return false;
  msg->wire = wire;

  ant_channel_t *ch = end->ch;
  ant_mutex_lock(&ch->lock);
  if (!ch->entangled) {
    ant_mutex_unlock(&ch->lock);
    message_free(msg);
    return true;
  }

  port_side_t *peer = &ch->side[end->side ^ 1];
  if (peer->tail) peer->tail->next = msg;
  else peer->head = msg;
  peer->tail = msg;
  
  binding_notify_locked(peer->binding);
  ant_mutex_unlock(&ch->lock);
  
  return true;
}

static ant_port_message_t *end_pop(ant_port_end_t *end) {
  ant_channel_t *ch = end->ch;
  ant_mutex_lock(&ch->lock);
  
  port_side_t *side = &ch->side[end->side];
  ant_port_message_t *msg = side->head;
  
  if (msg) {
    side->head = msg->next;
    if (!side->head) side->tail = NULL;
    msg->next = NULL;
  }
  
  ant_mutex_unlock(&ch->lock);
  
  return msg;
}

static bool end_take_close(ant_port_end_t *end) {
  ant_channel_t *ch = end->ch;
  ant_mutex_lock(&ch->lock);
  
  port_side_t *side = &ch->side[end->side];
  bool pending = side->close_pending && !side->head;
  
  if (pending) side->close_pending = false;
  ant_mutex_unlock(&ch->lock);
  
  return pending;
}

ant_value_t ant_port_end_post(ant_t *js, ant_port_end_t *end, ant_value_t value, ant_value_t transfer) {
  ant_wire_t *wire = NULL;
  ant_value_t r = ant_wire_serialize(js, value, transfer, js_mkundef(), &wire);
  if (is_err(r)) return r;
  if (!end_push(end, wire)) {
    ant_wire_release(wire);
    return Ant_Error_CreateOutOfMemory(js);
  }
  return js_mkundef();
}

static inline ant_port_binding_t *port_binding(ant_value_t value) {
  return js_get_native(value, MESSAGE_PORT_NATIVE_TAG);
}

bool ant_message_port_is(ant_value_t value) {
  return port_binding(value) != NULL;
}

static void binding_update_ref(ant_port_binding_t *binding) {
  if (!binding->async_open) return;
  uv_handle_t *handle = (uv_handle_t *)&binding->async;
  if (binding->refed && !binding->closed && !binding->on_message) uv_ref(handle);
  else uv_unref(handle);
}

static void binding_async_closed(uv_handle_t *handle) {
  free(handle->data);
}

static void binding_detach(ant_port_binding_t *binding, bool release_end) {
  if (binding->end) {
    ant_channel_t *ch = binding->end->ch;
    ant_mutex_lock(&ch->lock);
    port_side_t *side = &ch->side[binding->end->side];
    if (side->binding == binding) side->binding = NULL;
    ant_mutex_unlock(&ch->lock);
    if (release_end) ant_port_end_release(binding->end);
    binding->end = NULL;
  }
  Ant_List_Remove(binding);
}

static void binding_destroy(ant_port_binding_t *binding) {
  binding_detach(binding, true);
  if (binding->async_open) {
    binding->async_open = false;
    uv_close((uv_handle_t *)&binding->async, binding_async_closed);
  } else free(binding);
}

static ant_value_t message_event_fill(ant_t *js, ant_value_t event, ant_value_t data, ant_value_t ports) {
  if (!is_object_type(event)) return event;
  ant_value_t empty = js_mkstr(js, "", 0);
  
  Ant_Event_SetField(js, event, ANT_EVENT_FIELD_MESSAGE_data, data);
  Ant_Event_SetField(js, event, ANT_EVENT_FIELD_MESSAGE_origin, empty);
  Ant_Event_SetField(js, event, ANT_EVENT_FIELD_MESSAGE_lastEventId, empty);
  Ant_Event_SetField(js, event, ANT_EVENT_FIELD_MESSAGE_source, js_mknull());
  Ant_Event_SetField(js, event, ANT_EVENT_FIELD_MESSAGE_ports, vtype(ports) == kTypeArray ? ports : js_mkarr(js));
  
  return event;
}

static ant_value_t port_message_event(ant_t *js, ant_value_t target, const char *type, ant_value_t data, ant_value_t ports) {
  ant_value_t event = Ant_EventTarget_CreateEvent(js, target, js->builtins.message_event_proto, ANT_EVENT_MESSAGE, type);
  return message_event_fill(js, event, data, ports);
}

ant_value_t ant_message_event_create(ant_t *js, const char *type, ant_value_t data, ant_value_t ports) {
  ant_value_t event = Ant_Event_CreateTrusted(js, js->builtins.message_event_proto, ANT_EVENT_MESSAGE, type);
  return message_event_fill(js, event, data, ports);
}

static void port_dispatch(ant_t *js, ant_value_t target, const char *type, ant_value_t data, ant_value_t ports) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, data);
  GC_ROOT_PIN(js, ports);
  
  ant_value_t event = port_message_event(js, target, type, data, ports);
  if (is_object_type(event)) {
    GC_ROOT_PIN(js, event);
    Ant_EventTarget_Dispatch(js, target, event);
  }
  
  GC_ROOT_RESTORE(js, mark);
}

static void binding_dispatch(ant_port_binding_t *binding, const char *type, ant_value_t data, ant_value_t ports) {
  ant_t *js = binding->js;
  port_dispatch(js, binding->target, type, data, ports);
  if (!binding->mirror_global || Ant_Exception_Pending(js)) return;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, data);
  GC_ROOT_PIN(js, ports);
  ant_value_t event = ant_message_event_create(js, type, data, ports);
  if (is_object_type(event)) {
    GC_ROOT_PIN(js, event);
    js_dispatch_global_event(js, event);
  }
  GC_ROOT_RESTORE(js, mark);
}

static void port_dispatch_close(ant_t *js, ant_value_t target) {
  ant_value_t event = Ant_EventTarget_CreateEvent(js, target, js->builtins.event_proto, ANT_EVENT_BASE, "close");
  if (is_err(event)) return;
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, event);
  Ant_EventTarget_Dispatch(js, target, event);
  GC_ROOT_RESTORE(js, mark);
}

static void binding_deliver(ant_port_binding_t *binding, ant_port_message_t *msg) {
  ant_t *js = binding->js;
  ant_value_t target = binding->target;
  ant_value_t ports = js_mkundef();

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, target);
  ant_value_t value = ant_wire_deserialize(js, msg->wire, &ports);
  message_free(msg);
  GC_ROOT_PIN(js, ports);

  if (is_err(value)) {
    ant_value_t error = js_take_thrown(js, value);
    if (binding->on_message) binding->on_message(js, target, error, js_mknull());
    else binding_dispatch(binding, "messageerror", error, ports);
  } else if (binding->on_message) binding->on_message(js, target, value, ports);
  else binding_dispatch(binding, "message", value, ports);

  GC_ROOT_RESTORE(js, mark);
  process_microtasks(js);
}

static void binding_finish_close(ant_port_binding_t *binding) {
  if (binding->closed) return;
  binding->closed = true;
  binding_update_ref(binding);
  if (!binding->on_message && is_object_type(binding->target)) port_dispatch_close(binding->js, binding->target);
}

void ant_port_binding_drain(ant_port_binding_t *binding) {
  if (!binding || !binding->end || binding->draining) return;
  binding->draining = true;

  while (binding->end && binding->started && !binding->js->destroying) {
    ant_port_message_t *msg = end_pop(binding->end);
    if (!msg) break;
    binding_deliver(binding, msg);
  }

  if (binding->end && end_take_close(binding->end)) binding_finish_close(binding);
  binding->draining = false;
}

static void binding_async_cb(uv_async_t *handle) {
  ant_port_binding_drain((ant_port_binding_t *)handle->data);
}

ant_port_binding_t *ant_port_end_bind(ant_t *js, ant_port_end_t *end, ant_value_t target, ant_port_message_fn on_message) {
  ant_port_binding_t *binding = calloc(1, sizeof(*binding));
  if (!binding) return NULL;

  if (uv_async_init(js->loop, &binding->async, binding_async_cb) != 0) {
    free(binding);
    return NULL;
  }

  binding->js = js;
  binding->end = end;
  binding->target = target;
  binding->on_message = on_message;
  binding->async.data = binding;
  binding->async_open = true;
  
  binding->started = on_message != NULL;
  uv_unref((uv_handle_t *)&binding->async);
  Ant_List_Insert(&js->port_bindings, binding);

  ant_channel_t *ch = end->ch;
  ant_mutex_lock(&ch->lock);
  
  port_side_t *side = &ch->side[end->side];
  side->binding = binding;
  
  if (side->head || side->close_pending) uv_async_send(&binding->async);
  ant_mutex_unlock(&ch->lock);
  
  return binding;
}

void ant_port_binding_free(ant_port_binding_t *binding) {
  if (binding) binding_destroy(binding);
}

ant_value_t ant_port_binding_post(ant_t *js, ant_port_binding_t *binding, ant_value_t value, ant_value_t transfer) {
  if (!binding || !binding->end) return js_mkundef();
  return ant_port_end_post(js, binding->end, value, transfer);
}

void ant_port_binding_close(ant_port_binding_t *binding) {
  if (!binding) return;
  if (binding->end) {
    ant_channel_t *ch = binding->end->ch;
    ant_mutex_lock(&ch->lock);
    channel_disentangle_locked(ch);
    ant_mutex_unlock(&ch->lock);
  }
}

static void message_port_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_port_binding_t *binding = port_binding(value);
  if (!binding) return;
  js_clear_native(value, MESSAGE_PORT_NATIVE_TAG);
  binding->target = js_mkundef();
  binding_destroy(binding);
}

static bool binding_listener_change(ant_t *js, ant_port_binding_t *binding, ant_value_t key, bool *slot, ant_offset_t count) {
  if (!binding || vtype(key) != kTypeString) return false;
  size_t len = 0;
  const char *name = js_getstr(js, key, &len);
  if (len != 7 || memcmp(name, "message", 7) != 0) return false;

  bool had = binding->has_listeners || binding->global_listeners;
  *slot = count > 0;
  bool has = binding->has_listeners || binding->global_listeners;
  if (has == had) return true;

  binding->refed = has;
  if (has && !binding->started) {
    binding->started = true;
    if (binding->async_open) uv_async_send(&binding->async);
  }
  binding_update_ref(binding);
  return true;
}

static void message_port_listener_change(ant_t *js, ant_value_t target, ant_value_t key, ant_offset_t count, void *context) {
  ant_port_binding_t *binding = port_binding(target);
  if (binding) binding_listener_change(js, binding, key, &binding->has_listeners, count);
}

static void message_port_global_listener_change(ant_t *js, ant_value_t target, ant_value_t key, ant_offset_t count, void *context) {
  ant_port_binding_t *binding = context;
  binding_listener_change(js, binding, key, &binding->global_listeners, count);
}

static ant_value_t message_port_create(ant_t *js, ant_port_end_t *end) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) return obj;
  js_set_proto_init(obj, js->builtins.message_port_proto);

  ant_port_binding_t *binding = ant_port_end_bind(js, end, obj, NULL);
  if (!binding) return Ant_Error_CreateOutOfMemory(js);

  js_set_native(obj, binding, MESSAGE_PORT_NATIVE_TAG);
  js_set_finalizer(obj, message_port_finalize);
  eventemitter_set_listener_change_hook(js, obj, message_port_listener_change, NULL);
  return obj;
}

ant_value_t ant_message_port_proto(ant_t *js) {
  return js->builtins.message_port_proto;
}

ant_value_t ant_message_port_adopt(ant_t *js, ant_port_end_t *end) {
  ant_value_t port = message_port_create(js, end);
  if (is_err(port)) ant_port_end_release(end);
  return port;
}

bool ant_message_port_mirror_global(ant_t *js, ant_value_t port) {
  ant_port_binding_t *binding = port_binding(port);
  if (!binding) return false;
  binding->mirror_global = true;
  Ant_GlobalEvents_SetListenerChangeHook(js, message_port_global_listener_change, binding);
  return true;
}

ant_port_end_t *ant_message_port_take(ant_t *js, ant_value_t port) {
  ant_port_binding_t *binding = port_binding(port);
  if (!binding || !binding->end) return NULL;
  ant_port_end_t *end = binding->end;
  
  binding_detach(binding, false);
  binding->closed = true;
  binding_update_ref(binding);
  
  return end;
}

ant_value_t ant_message_port_post(ant_t *js, ant_value_t port, ant_value_t value, ant_value_t transfer) {
  ant_port_binding_t *binding = port_binding(port);
  if (!binding) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (!binding->end) return js_mkundef();

  ant_wire_t *wire = NULL;
  ant_value_t r = ant_wire_serialize(js, value, transfer, port, &wire);
  if (is_err(r)) return r;
  if (!binding->end || !end_push(binding->end, wire)) {
    ant_wire_release(wire);
    return binding->end ? Ant_Error_CreateOutOfMemory(js) : js_mkundef();
  }
  return js_mkundef();
}

ant_value_t ant_message_port_receive(ant_t *js, ant_value_t port) {
  ant_port_binding_t *binding = port_binding(port);
  if (!binding) return js_mkerr_typed(js, JS_ERR_TYPE, "The \"port\" argument must be a MessagePort instance");
  if (!binding->end) return js_mkundef();

  ant_port_message_t *msg = end_pop(binding->end);
  if (!msg) return js_mkundef();

  ant_value_t value = ant_wire_deserialize(js, msg->wire, NULL);
  message_free(msg);
  if (is_err(value)) return value;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, value);
  ant_value_t out = Ant_Object_Create(js);
  if (!is_err(out)) js_set(js, out, "message", value);
  GC_ROOT_RESTORE(js, mark);
  return out;
}

static ant_value_t js_message_port_post_message(ant_params_t) {
  ant_value_t transfer = js_mkundef();
  if (nargs > 1) {
    ant_value_t opt = args[1];
    if (is_object_type(opt) && vtype(opt) != kTypeArray) {
      ant_value_t list = js_get(js, opt, "transfer");
      if (vtype(list) == kTypeUndefined) list = js_get(js, opt, "transferList");
      transfer = list;
    } else transfer = opt;
  }
  return ant_message_port_post(js, js_getthis(js), nargs > 0 ? args[0] : js_mkundef(), transfer);
}

static ant_value_t js_message_port_start(ant_params_t) {
  ant_port_binding_t *binding = port_binding(js_getthis(js));
  if (!binding) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (!binding->started) {
    binding->started = true;
    if (binding->async_open) uv_async_send(&binding->async);
  }
  return js_mkundef();
}

static ant_value_t js_message_port_close(ant_params_t) {
  ant_port_binding_t *binding = port_binding(js_getthis(js));
  if (!binding) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  ant_port_binding_close(binding);
  return js_mkundef();
}

static ant_value_t js_message_port_ref(ant_params_t) {
  ant_port_binding_t *binding = port_binding(js_getthis(js));
  if (binding) {
    binding->refed = true;
    binding_update_ref(binding);
  }
  return js_getthis(js);
}

static ant_value_t js_message_port_unref(ant_params_t) {
  ant_port_binding_t *binding = port_binding(js_getthis(js));
  if (binding) {
    binding->refed = false;
    binding_update_ref(binding);
  }
  return js_getthis(js);
}

static ant_value_t js_message_port_has_ref(ant_params_t) {
  ant_port_binding_t *binding = port_binding(js_getthis(js));
  return js_bool(binding && binding->refed && !binding->closed);
}

static ant_value_t node_target_add(ant_t *js, ant_value_t *args, int nargs, bool once, bool prepend) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 2 || !is_callable(args[1]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The \"listener\" argument must be of type function");
  ant_value_t type = vtype(args[0]) == kTypeSymbol ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(type)) return type;
  Ant_EventTarget_AddDataListener(js, this_obj, type, args[1], once, prepend);
  return this_obj;
}

static ant_value_t js_node_target_on(ant_params_t) { return node_target_add(js, args, nargs, false, false); }
static ant_value_t js_node_target_once(ant_params_t) { return node_target_add(js, args, nargs, true, false); }
static ant_value_t js_node_target_prepend(ant_params_t) { return node_target_add(js, args, nargs, false, true); }
static ant_value_t js_node_target_prepend_once(ant_params_t) { return node_target_add(js, args, nargs, true, true); }

static ant_value_t js_node_target_off(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  ant_value_t type = vtype(args[0]) == kTypeSymbol ? args[0] : js_tostring_val(js, args[0]);
  if (is_err(type)) return type;
  
  Ant_EventTarget_RemoveListener(js, this_obj, type, args[1]);
  
  return this_obj;
}

static ant_value_t js_node_target_remove_all(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  ant_value_t remove = js_get(js, js->builtins.eventemitter_proto, "removeAllListeners");
  if (is_callable(remove)) {
    ant_value_t r = sv_vm_call(js->vm, js, remove, this_obj, args, nargs, NULL, js_mkundef());
    if (is_err(r)) return r;
  }
  return this_obj;
}

static ant_value_t js_node_target_emit(ant_params_t) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 1) return js_false;
  
  ant_value_t type = js_tostring_val(js, args[0]);
  if (is_err(type)) return type;
  
  size_t len = 0;
  const char *name = js_getstr(js, type, &len);
  char buf[64];
  
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, name, len);
  buf[len] = '\0';
  port_dispatch(js, this_obj, buf, nargs > 1 ? args[1] : js_mkundef(), js_mkundef());
  
  return js_true;
}

static ant_value_t js_node_target_listener_count(ant_params_t) {
  if (nargs < 1) return js_mknum(0);
  ant_value_t type = js_tostring_val(js, args[0]);
  if (is_err(type)) return type;
  
  size_t len = 0;
  const char *name = js_getstr(js, type, &len);
  char buf[64];
  
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, name, len);
  buf[len] = '\0';
  
  return js_mknum((double)eventemitter_listener_count(js, js_getthis(js), buf));
}

static ant_value_t js_node_target_max_listeners(ant_params_t) {
  return js_mknum(EVENTS_DEFAULT_MAX_LISTENERS);
}

static ant_value_t js_node_target_set_max_listeners(ant_params_t) {
  return js_getthis(js);
}

static ant_value_t js_message_port_onmessage_get(ant_params_t) {
  return Ant_EventTarget_GetHandler(js, js_getthis(js), "message", 7);
}

static ant_value_t js_message_port_onmessage_set(ant_params_t) {
  Ant_EventTarget_SetHandler(js, js_getthis(js), "message", 7, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t js_message_port_onmessageerror_get(ant_params_t) {
  return Ant_EventTarget_GetHandler(js, js_getthis(js), "messageerror", 12);
}

static ant_value_t js_message_port_onmessageerror_set(ant_params_t) {
  Ant_EventTarget_SetHandler(js, js_getthis(js), "messageerror", 12, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t js_message_port_ctor(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal constructor");
}

static ant_value_t js_message_channel_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor MessageChannel cannot be invoked without 'new'");

  ant_value_t this_obj = js_getthis(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.message_channel_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(this_obj, proto);

  ant_port_end_t *a = NULL, *b = NULL;
  if (!ant_port_end_pair(&a, &b)) return Ant_Error_CreateOutOfMemory(js);

  GC_ROOT_SAVE(mark, js);
  ant_value_t port1 = ant_message_port_adopt(js, a);
  if (is_err(port1)) {
    ant_port_end_release(b);
    GC_ROOT_RESTORE(js, mark);
    return port1;
  }
  GC_ROOT_PIN(js, port1);
  ant_value_t port2 = ant_message_port_adopt(js, b);
  if (is_err(port2)) {
    GC_ROOT_RESTORE(js, mark);
    return port2;
  }
  GC_ROOT_PIN(js, port2);

  js_set(js, this_obj, "port1", port1);
  js_set(js, this_obj, "port2", port2);
  GC_ROOT_RESTORE(js, mark);
  return this_obj;
}

static inline broadcast_member_t *broadcast_member(ant_value_t value) {
  return js_get_native(value, BROADCAST_NATIVE_TAG);
}

static void broadcast_update_ref(broadcast_member_t *m) {
  if (!m->async_open) return;
  if (m->refed && !m->closed) uv_ref((uv_handle_t *)&m->async);
  else uv_unref((uv_handle_t *)&m->async);
}

static void broadcast_leave_locked(broadcast_member_t *m) {
  broadcast_group_t *group = m->group;
  if (!group) return;
  for (broadcast_member_t **it = &group->members; *it; it = &(*it)->group_next) {
    if (*it != m) continue;
    *it = m->group_next;
    break;
  }
  m->group = NULL;
  m->group_next = NULL;
  if (!group->members) {
    HASH_DEL(broadcast_groups, group);
    free(group->name);
    free(group);
  }
}

static void broadcast_maybe_free(broadcast_member_t *m) {
  if (m->obj_alive || !m->handle_closed) return;
  queue_free(m->head);
  free(m);
}

static void broadcast_async_closed(uv_handle_t *handle) {
  broadcast_member_t *m = handle->data;
  m->handle_closed = true;
  broadcast_maybe_free(m);
}

static void broadcast_close(broadcast_member_t *m) {
  ant_mutex_lock(&broadcast_lock);
  broadcast_leave_locked(m);
  bool was_open = m->async_open;
  m->async_open = false;
  m->closed = true;
  ant_mutex_unlock(&broadcast_lock);
  Ant_List_Remove(m);
  if (was_open) uv_close((uv_handle_t *)&m->async, broadcast_async_closed);
}

static void broadcast_async_cb(uv_async_t *handle) {
  broadcast_member_t *m = handle->data;
  ant_t *js = m->js;

  while (!m->closed && !js->destroying) {
    ant_mutex_lock(&broadcast_lock);
    ant_port_message_t *msg = m->head;
    if (msg) {
      m->head = msg->next;
      if (!m->head) m->tail = NULL;
    }
    ant_mutex_unlock(&broadcast_lock);
    if (!msg) break;

    GC_ROOT_SAVE(mark, js);
    ant_value_t obj = m->obj;
    GC_ROOT_PIN(js, obj);
    ant_value_t value = ant_wire_deserialize(js, msg->wire, NULL);
    message_free(msg);
    if (is_err(value)) port_dispatch(js, obj, "messageerror", js_take_thrown(js, value), js_mkundef());
    else port_dispatch(js, obj, "message", value, js_mkundef());
    GC_ROOT_RESTORE(js, mark);
    process_microtasks(js);
  }
}

static void broadcast_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  broadcast_member_t *m = broadcast_member(value);
  if (!m) return;
  js_clear_native(value, BROADCAST_NATIVE_TAG);
  m->obj = js_mkundef();
  m->obj_alive = false;
  if (!m->closed) broadcast_close(m);
  else broadcast_maybe_free(m);
}

static ant_value_t js_broadcast_channel_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor BroadcastChannel cannot be invoked without 'new'");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "The \"name\" argument must be specified");

  ant_value_t name_val = js_tostring_val(js, args[0]);
  if (is_err(name_val)) return name_val;

  ant_value_t this_obj = js_getthis(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.broadcast_channel_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(this_obj, proto);

  broadcast_member_t *m = calloc(1, sizeof(*m));
  if (!m) return Ant_Error_CreateOutOfMemory(js);
  if (uv_async_init(js->loop, &m->async, broadcast_async_cb) != 0) {
    free(m);
    return js_mkerr(js, "failed to create BroadcastChannel");
  }
  m->js = js;
  m->obj = this_obj;
  m->async.data = m;
  m->async_open = true;
  m->obj_alive = true;
  m->refed = true;

  size_t name_len = 0;
  const char *name = js_getstr(js, name_val, &name_len);
  ant_mutex_lock(&broadcast_lock);
  broadcast_group_t *group = NULL;
  
  HASH_FIND(hh, broadcast_groups, name, name_len, group);
  if (!group && (group = calloc(1, sizeof(*group))) && (group->name = strndup(name, name_len)))
    HASH_ADD_KEYPTR(hh, broadcast_groups, group->name, name_len, group);
  if (group && group->name) {
    m->group = group;
    m->group_next = group->members;
    group->members = m;
  }
  ant_mutex_unlock(&broadcast_lock);

  Ant_List_Insert(&js->broadcast_members, m);
  js_set_native(this_obj, m, BROADCAST_NATIVE_TAG);
  js_set_finalizer(this_obj, broadcast_finalize);
  js_set(js, this_obj, "name", name_val);
  
  return this_obj;
}

static ant_value_t js_broadcast_channel_post_message(ant_params_t) {
  broadcast_member_t *m = broadcast_member(js_getthis(js));
  if (!m) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (m->closed) return js_throw(js, make_dom_exception(js, "BroadcastChannel is closed.", "InvalidStateError"));

  ant_wire_t *wire = NULL;
  ant_value_t r = ant_wire_serialize(js, nargs > 0 ? args[0] : js_mkundef(), js_mkundef(), js_mkundef(), &wire);
  if (is_err(r)) return r;

  ant_mutex_lock(&broadcast_lock);
  for (broadcast_member_t *peer = m->group ? m->group->members : NULL; peer; peer = peer->group_next) {
    if (peer == m || peer->closed || !peer->async_open) continue;
    ant_port_message_t *msg = calloc(1, sizeof(*msg));
    if (!msg) continue;
    msg->wire = ant_wire_retain(wire);
    if (peer->tail) peer->tail->next = msg;
    else peer->head = msg;
    peer->tail = msg;
    uv_async_send(&peer->async);
  }
  ant_mutex_unlock(&broadcast_lock);
  ant_wire_release(wire);
  return js_mkundef();
}

static ant_value_t js_broadcast_channel_close(ant_params_t) {
  broadcast_member_t *m = broadcast_member(js_getthis(js));
  if (!m) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (!m->closed) broadcast_close(m);
  return js_mkundef();
}

static ant_value_t js_broadcast_channel_ref(ant_params_t) {
  broadcast_member_t *m = broadcast_member(js_getthis(js));
  if (m) {
    m->refed = true;
    broadcast_update_ref(m);
  }
  return js_getthis(js);
}

static ant_value_t js_broadcast_channel_unref(ant_params_t) {
  broadcast_member_t *m = broadcast_member(js_getthis(js));
  if (m) {
    m->refed = false;
    broadcast_update_ref(m);
  }
  return js_getthis(js);
}

#define define_handler_accessor(js, proto, name, getter, setter) \
  js_set_accessor_desc(js, proto, name, sizeof(name) - 1, Ant_Function_Create(getter), Ant_Function_Create(setter), JS_DESC_C | JS_DESC_E)

void init_message_port_module(ant_t *js) {
  ant_value_t et_proto = js->builtins.eventtarget_proto;

  ant_value_t port_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(port_proto, et_proto);
  js->builtins.message_port_proto = port_proto;
  js_set(js, port_proto, "postMessage", Ant_Function_CreateWithArity(js_message_port_post_message, 1));
  js_set(js, port_proto, "start", Ant_Function_Create(js_message_port_start));
  js_set(js, port_proto, "close", Ant_Function_Create(js_message_port_close));
  js_set(js, port_proto, "ref", Ant_Function_Create(js_message_port_ref));
  js_set(js, port_proto, "unref", Ant_Function_Create(js_message_port_unref));
  js_set(js, port_proto, "hasRef", Ant_Function_Create(js_message_port_has_ref));
  js_set(js, port_proto, "on", Ant_Function_CreateWithArity(js_node_target_on, 2));
  js_set(js, port_proto, "addListener", Ant_Function_CreateWithArity(js_node_target_on, 2));
  js_set(js, port_proto, "once", Ant_Function_CreateWithArity(js_node_target_once, 2));
  js_set(js, port_proto, "prependListener", Ant_Function_CreateWithArity(js_node_target_prepend, 2));
  js_set(js, port_proto, "prependOnceListener", Ant_Function_CreateWithArity(js_node_target_prepend_once, 2));
  js_set(js, port_proto, "off", Ant_Function_CreateWithArity(js_node_target_off, 2));
  js_set(js, port_proto, "removeListener", Ant_Function_CreateWithArity(js_node_target_off, 2));
  js_set(js, port_proto, "removeAllListeners", Ant_Function_Create(js_node_target_remove_all));
  js_set(js, port_proto, "emit", Ant_Function_Create(js_node_target_emit));
  js_set(js, port_proto, "listenerCount", Ant_Function_CreateWithArity(js_node_target_listener_count, 1));
  js_set(js, port_proto, "getMaxListeners", Ant_Function_Create(js_node_target_max_listeners));
  js_set(js, port_proto, "setMaxListeners", Ant_Function_CreateWithArity(js_node_target_set_max_listeners, 1));
  define_handler_accessor(js, port_proto, "onmessage", js_message_port_onmessage_get, js_message_port_onmessage_set);
  define_handler_accessor(js, port_proto, "onmessageerror", js_message_port_onmessageerror_get, js_message_port_onmessageerror_set);
  js_set_tostringtag(js, port_proto, js_mkstr(js, "MessagePort", 11));
  ant_value_t port_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_message_port_ctor, port_proto, "MessagePort", 0);
  js_set_global_builtin(js, "MessagePort", port_ctor);

  ant_value_t channel_proto = Ant_Object_Create(js);
  js->builtins.message_channel_proto = channel_proto;
  js_set_tostringtag(js, channel_proto, js_mkstr(js, "MessageChannel", 14));
  ant_value_t channel_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_message_channel_ctor, channel_proto, "MessageChannel", 0);
  js_set_global_builtin(js, "MessageChannel", channel_ctor);

  ant_value_t bc_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(bc_proto, et_proto);
  js->builtins.broadcast_channel_proto = bc_proto;
  js_set(js, bc_proto, "postMessage", Ant_Function_CreateWithArity(js_broadcast_channel_post_message, 1));
  js_set(js, bc_proto, "close", Ant_Function_Create(js_broadcast_channel_close));
  js_set(js, bc_proto, "ref", Ant_Function_Create(js_broadcast_channel_ref));
  js_set(js, bc_proto, "unref", Ant_Function_Create(js_broadcast_channel_unref));
  define_handler_accessor(js, bc_proto, "onmessage", js_message_port_onmessage_get, js_message_port_onmessage_set);
  define_handler_accessor(js, bc_proto, "onmessageerror", js_message_port_onmessageerror_get, js_message_port_onmessageerror_set);
  js_set_tostringtag(js, bc_proto, js_mkstr(js, "BroadcastChannel", 16));
  ant_value_t bc_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_broadcast_channel_ctor, bc_proto, "BroadcastChannel", 1);
  js_set_global_builtin(js, "BroadcastChannel", bc_ctor);
}

void cleanup_message_port_module(ant_t *js) {
  while (js->port_bindings) {
    ant_port_binding_t *binding = js->port_bindings;
    if (is_object_type(binding->target) && !binding->on_message)
      js_clear_native(binding->target, MESSAGE_PORT_NATIVE_TAG);
    binding->target = js_mkundef();
    binding_destroy(binding);
  }

  while (js->broadcast_members) {
    broadcast_member_t *m = js->broadcast_members;
    if (is_object_type(m->obj)) js_clear_native(m->obj, BROADCAST_NATIVE_TAG);
    m->obj = js_mkundef();
    m->obj_alive = false;
    broadcast_close(m);
  }
}

void gc_mark_message_ports(ant_t *js, gc_mark_fn mark) {
  for (ant_port_binding_t *b = js->port_bindings; b; b = b->next)
    if (!b->on_message && !b->closed && (b->has_listeners || b->refed) && is_object_type(b->target)) mark(js, b->target);
  for (broadcast_member_t *m = js->broadcast_members; m; m = m->next)
    if (!m->closed && is_object_type(m->obj)) mark(js, m->obj);
}
