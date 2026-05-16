#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uv.h>

#include "ant.h"
#include "ptr.h"
#include "common.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "inspector.h"
#include "silver/engine.h"

#include "http/eventsource.h"
#include "modules/eventsource.h"
#include "modules/http.h"
#include "modules/symbol.h"
#include "modules/timer.h"

typedef struct eventsource_timer_s {
  uv_timer_t handle;
  struct eventsource_state_s *source;
} eventsource_timer_t;

typedef struct eventsource_state_s {
  ant_t *js;
  ant_value_t obj;
  ant_http_request_t *request;
  eventsource_timer_t *timer;
  uint64_t network_request_id;
  size_t network_encoded_length;
  ant_sse_parser_t parser;
  char *url;
  char *last_event_id;
  struct eventsource_state_s *next;
  uint32_t retry_ms;
  uint8_t ready_state;
  bool with_credentials : 1;
  bool active : 1;
  bool closed : 1;
  bool opened_request : 1;
  bool error_emitted_for_request : 1;
} eventsource_state_t;

enum {
  ES_NATIVE_TAG = 0x45565352u, // EVSR
  ES_CONNECTING = 0,
  ES_OPEN = 1,
  ES_CLOSED = 2,
};

static eventsource_state_t *g_active_eventsources = NULL;

static eventsource_state_t *eventsource_data(ant_value_t value) {
  return (eventsource_state_t *)js_get_native(value, ES_NATIVE_TAG);
}

static void eventsource_add_active(eventsource_state_t *es) {
  if (!es || es->active) return;
  es->next = g_active_eventsources;
  g_active_eventsources = es;
  es->active = true;
}

static void eventsource_remove_active(eventsource_state_t *es) {
  eventsource_state_t **it = &g_active_eventsources;
  if (!es || !es->active) return;
  while (*it) {
    if (*it == es) {
      *it = es->next;
      es->next = NULL;
      es->active = false;
      return;
    }
    it = &(*it)->next;
  }
  es->active = false;
}

static void eventsource_maybe_unroot(eventsource_state_t *es) {
  if (!es || !es->closed) return;
  if (es->request || es->timer) return;
  eventsource_remove_active(es);
}

static ant_value_t eventsource_call(ant_t *js, ant_value_t fn, ant_value_t this_val, ant_value_t *args, int nargs) {
  if (!is_callable(fn)) return js_mkundef();
  ant_value_t saved_this = js->this_val;
  ant_value_t result = js_mkundef();
  js->this_val = this_val;
  if (vtype(fn) == T_CFUNC) result = js_as_cfunc(fn)(js, args, nargs);
  else result = sv_vm_call(js->vm, js, fn, this_val, args, nargs, NULL, false);
  js->this_val = saved_this;
  return result;
}

static void eventsource_sync_state(eventsource_state_t *es) {
  if (!es || !is_object_type(es->obj)) return;
  js_set(es->js, es->obj, "readyState", js_mknum(es->ready_state));
}

static ant_value_t eventsource_make_event(ant_t *js, const char *type, ant_value_t proto) {
  ant_value_t event = js_mkobj(js);
  if (is_object_type(proto)) js_set_proto_init(event, proto);
  js_set(js, event, "type", js_mkstr(js, type, strlen(type)));
  js_set(js, event, "target", js_mknull());
  js_set(js, event, "currentTarget", js_mknull());
  js_set(js, event, "eventPhase", js_mknum(0));
  js_set(js, event, "bubbles", js_false);
  js_set(js, event, "cancelable", js_false);
  js_set(js, event, "defaultPrevented", js_false);
  return event;
}

static void eventsource_emit(eventsource_state_t *es, const char *type, ant_value_t event) {
  ant_t *js = es->js;
  ant_value_t dispatch = js_get(js, es->obj, "dispatchEvent");
  ant_value_t args[1] = { event };
  char handler_name[32];

  if (!is_callable(dispatch)) {
    ant_value_t eventtarget_proto = js_get_ctor_proto(js, "EventTarget", 11);
    if (is_object_type(eventtarget_proto)) dispatch = js_get(js, eventtarget_proto, "dispatchEvent");
  }
  eventsource_call(js, dispatch, es->obj, args, 1);
  snprintf(handler_name, sizeof(handler_name), "on%s", type);
  ant_value_t handler = js_get(js, es->obj, handler_name);
  eventsource_call(js, handler, es->obj, args, 1);
  process_microtasks(js);
}

static void eventsource_emit_simple(eventsource_state_t *es, const char *type) {
  ant_value_t proto = js_get_ctor_proto(es->js, "Event", 5);
  eventsource_emit(es, type, eventsource_make_event(es->js, type, proto));
}

static void eventsource_emit_error_once(eventsource_state_t *es) {
  if (!es || es->closed || es->error_emitted_for_request) return;
  es->error_emitted_for_request = true;
  eventsource_emit_simple(es, "error");
}

static bool eventsource_content_type_ok(const char *value) {
  static const char expected[] = "text/event-stream";
  size_t expected_len = sizeof(expected) - 1;
  if (!value) return false;
  while (*value == ' ' || *value == '\t') value++;
  return strncasecmp(value, expected, expected_len) == 0;
}

static bool eventsource_is_http_url(const char *url) {
  return url && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static const char *eventsource_find_header(const ant_http_header_t *headers, const char *name) {
  for (const ant_http_header_t *entry = headers; entry; entry = entry->next) {
    if (entry->name && name && strcasecmp(entry->name, name) == 0) return entry->value;
  }
  return NULL;
}

static void eventsource_start_request(eventsource_state_t *es);

static void eventsource_timer_close_cb(uv_handle_t *handle) {
  free(handle);
}

static void eventsource_reconnect_cb(uv_timer_t *handle) {
  eventsource_timer_t *timer = (eventsource_timer_t *)handle;
  eventsource_state_t *es = timer->source;
  if (es && es->timer == timer) es->timer = NULL;
  uv_timer_stop(handle);
  uv_close((uv_handle_t *)handle, eventsource_timer_close_cb);

  if (!es || es->closed) {
    eventsource_maybe_unroot(es);
    return;
  }
  eventsource_start_request(es);
}

static void eventsource_cancel_timer(eventsource_state_t *es) {
  eventsource_timer_t *timer = es ? es->timer : NULL;
  if (!timer) return;
  es->timer = NULL;
  uv_timer_stop(&timer->handle);
  uv_close((uv_handle_t *)&timer->handle, eventsource_timer_close_cb);
}

static void eventsource_schedule_reconnect(eventsource_state_t *es) {
  if (!es || es->closed || es->timer) return;
  eventsource_timer_t *timer = calloc(1, sizeof(*timer));
  if (!timer) {
    es->closed = true;
    es->ready_state = ES_CLOSED;
    eventsource_sync_state(es);
    eventsource_maybe_unroot(es);
    return;
  }
  timer->source = es;
  uv_timer_init(uv_default_loop(), &timer->handle);
  es->timer = timer;
  es->ready_state = ES_CONNECTING;
  eventsource_sync_state(es);
  uv_timer_start(&timer->handle, eventsource_reconnect_cb, es->retry_ms, 0);
}

static bool eventsource_on_message(const ant_sse_message_t *message, void *user_data) {
  eventsource_state_t *es = (eventsource_state_t *)user_data;
  ant_t *js = es->js;
  const char *type = (message->event && message->event[0]) ? message->event : "message";
  ant_value_t proto = js_get_ctor_proto(js, "MessageEvent", 12);
  ant_value_t event = eventsource_make_event(js, type, proto);

  js_set(js, event, "data", js_mkstr(js, message->data ? message->data : "", message->data ? strlen(message->data) : 0));
  if (message->id) {
    free(es->last_event_id);
    es->last_event_id = strdup(message->id);
  } else if (es->parser.last_event_id) {
    free(es->last_event_id);
    es->last_event_id = strdup(es->parser.last_event_id);
  }
  if (!es->last_event_id) es->last_event_id = strdup("");
  js_set(js, event, "lastEventId", js_mkstr(js, es->last_event_id ? es->last_event_id : "", es->last_event_id ? strlen(es->last_event_id) : 0));
  js_set(js, event, "origin", js_mkstr(js, "", 0));
  eventsource_emit(es, type, event);
  return !es->closed;
}

static void eventsource_http_on_response(ant_http_request_t *req, const ant_http_response_t *resp, void *user_data) {
  eventsource_state_t *es = (eventsource_state_t *)user_data;
  const char *content_type = eventsource_find_header(resp->headers, "content-type");

  if (!es || es->closed) return;
  
  ant_inspector_network_response(
    es->network_request_id, es->url,
    resp->status, resp->status_text,
    content_type ? content_type : "text/event-stream",
    "EventSource", resp->headers
  );
  
  if (resp->status != 200 || !eventsource_content_type_ok(content_type)) {
    eventsource_emit_error_once(es);
    ant_http_request_cancel(req);
    return;
  }

  es->opened_request = true;
  es->ready_state = ES_OPEN;
  eventsource_sync_state(es);
  eventsource_emit_simple(es, "open");
}

static void eventsource_http_on_body(ant_http_request_t *req, const uint8_t *chunk, size_t len, void *user_data) {
  eventsource_state_t *es = (eventsource_state_t *)user_data;
  if (!es || es->closed || !es->opened_request) return;
  
  es->network_encoded_length += len;
  ant_inspector_network_append_response_body(es->network_request_id, chunk, len);
  
  if (!ant_sse_parser_feed(&es->parser, (const char *)chunk, len, eventsource_on_message, es)) {
    eventsource_emit_error_once(es);
    ant_http_request_cancel(req);
    return;
  }
  
  if (es->parser.has_retry) es->retry_ms = es->parser.retry;
}

static void eventsource_http_on_complete(
  ant_http_request_t *req,
  ant_http_result_t result,
  int error_code,
  const char *error_message,
  void *user_data
) {
  eventsource_state_t *es = (eventsource_state_t *)user_data;
  (void)req;
  (void)error_code;
  (void)error_message;

  if (!es) return;
  es->request = NULL;
  es->opened_request = false;
  if (es->network_request_id) {
    if (result == ANT_HTTP_RESULT_OK && error_code == 0) ant_inspector_network_finish(
      es->network_request_id, 
      es->network_encoded_length
    ); else ant_inspector_network_fail(
      es->network_request_id,
      error_message ? error_message : (es->closed ? "EventSource closed" : "EventSource connection failed"),
      es->closed || result == ANT_HTTP_RESULT_ABORTED,
      "EventSource"
    );
    es->network_request_id = 0;
    es->network_encoded_length = 0;
  }

  if (!es->closed) {
    if (result != ANT_HTTP_RESULT_ABORTED) eventsource_emit_error_once(es);
    es->error_emitted_for_request = false;
    ant_sse_parser_free(&es->parser);
    ant_sse_parser_init(&es->parser);
    eventsource_schedule_reconnect(es);
  }
  eventsource_maybe_unroot(es);
}

static void eventsource_start_request(eventsource_state_t *es) {
  ant_http_header_t last_id = {0};
  ant_http_header_t accept = { "Accept", "text/event-stream", NULL };
  ant_http_request_options_t options = {0};
  int rc = 0;

  if (!es || es->closed || es->request) return;
  es->ready_state = ES_CONNECTING;
  es->error_emitted_for_request = false;
  eventsource_sync_state(es);

  if (es->last_event_id && es->last_event_id[0]) {
    last_id.name = "Last-Event-ID";
    last_id.value = es->last_event_id;
    accept.next = &last_id;
  }

  es->network_encoded_length = 0;
  es->network_request_id = ant_inspector_network_request(
    "GET", es->url,
    "EventSource", "EventSource",
    false, &accept
  );

  options.method = "GET";
  options.url = es->url;
  options.headers = &accept;

  rc = ant_http_request_start(
    uv_default_loop(), &options,
    eventsource_http_on_response,
    eventsource_http_on_body,
    eventsource_http_on_complete,
    es, &es->request
  );
  
  if (rc != 0) {
    ant_inspector_network_fail(es->network_request_id, uv_strerror(rc), false, "EventSource");
    es->network_request_id = 0;
    es->request = NULL;
    eventsource_emit_error_once(es);
    es->error_emitted_for_request = false;
    eventsource_schedule_reconnect(es);
  }
}

static void eventsource_close(eventsource_state_t *es) {
  if (!es || es->closed) return;
  es->closed = true;
  es->ready_state = ES_CLOSED;
  eventsource_sync_state(es);
  eventsource_cancel_timer(es);
  if (es->request) ant_http_request_cancel(es->request);
  eventsource_maybe_unroot(es);
}

static void eventsource_finalize(ant_t *js, ant_object_t *obj) {
  (void)js;
  ant_value_t value = js_obj_from_ptr(obj);
  eventsource_state_t *es = eventsource_data(value);
  if (!es) return;
  js_clear_native(value, ES_NATIVE_TAG);
  if (es->request) ant_http_request_cancel(es->request);
  eventsource_cancel_timer(es);
  eventsource_remove_active(es);
  ant_sse_parser_free(&es->parser);
  free(es->url);
  free(es->last_event_id);
  free(es);
}

static ant_value_t eventsource_create_object(ant_t *js) {
  ant_value_t obj = js_mkobj(js);
  if (is_object_type(js->sym.eventsource_proto)) js_set_proto_init(obj, js->sym.eventsource_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_EVENTTARGET));
  js_set(js, obj, "onopen", js_mknull());
  js_set(js, obj, "onmessage", js_mknull());
  js_set(js, obj, "onerror", js_mknull());
  js_set_finalizer(obj, eventsource_finalize);
  return obj;
}

static ant_value_t js_eventsource_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "EventSource constructor requires 'new'");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "EventSource URL is required");

  ant_value_t url_val = js_tostring_val(js, args[0]);
  if (is_err(url_val)) return url_val;
  const char *url = js_getstr(js, url_val, NULL);
  if (!eventsource_is_http_url(url))
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "EventSource URL must use http: or https:");

  ant_value_t obj = eventsource_create_object(js);
  eventsource_state_t *es = calloc(1, sizeof(*es));
  if (!es) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  es->js = js;
  es->obj = obj;
  es->url = strdup(url);
  es->last_event_id = strdup("");
  es->retry_ms = 3000;
  es->ready_state = ES_CONNECTING;
  
  if (nargs > 1 && is_object_type(args[1])) {
    ant_value_t with_credentials = js_get(js, args[1], "withCredentials");
    if (vtype(with_credentials) != T_UNDEF) es->with_credentials = js_truthy(js, with_credentials);
  }
  
  if (!es->url || !es->last_event_id) {
    free(es->url);
    free(es->last_event_id);
    free(es);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }

  ant_sse_parser_init(&es->parser);
  js_set_native(obj, es, ES_NATIVE_TAG);
  js_set(js, obj, "url", url_val);
  js_set(js, obj, "withCredentials", js_bool(es->with_credentials));
  
  eventsource_sync_state(es);
  eventsource_add_active(es);
  eventsource_start_request(es);
  
  return obj;
}

static ant_value_t js_eventsource_close(ant_t *js, ant_value_t *args, int nargs) {
  eventsource_state_t *es = eventsource_data(js_getthis(js));
  if (!es) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid EventSource");
  eventsource_close(es);
  return js_mkundef();
}

void init_eventsource_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);
  ant_value_t eventtarget_proto = js_get_ctor_proto(js, "EventTarget", 11);

  js->sym.eventsource_proto = js_mkobj(js);
  if (is_object_type(eventtarget_proto)) js_set_proto_init(js->sym.eventsource_proto, eventtarget_proto);
  js_set(js, js->sym.eventsource_proto, "close", js_mkfun(js_eventsource_close));
  js_set(js, js->sym.eventsource_proto, "CONNECTING", js_mknum(ES_CONNECTING));
  js_set(js, js->sym.eventsource_proto, "OPEN", js_mknum(ES_OPEN));
  js_set(js, js->sym.eventsource_proto, "CLOSED", js_mknum(ES_CLOSED));
  js_set_sym(js, js->sym.eventsource_proto, get_toStringTag_sym(), js_mkstr(js, "EventSource", 11));

  js->sym.eventsource_ctor = js_make_ctor(js, js_eventsource_ctor, js->sym.eventsource_proto, "EventSource", 11);
  js_set(js, js->sym.eventsource_ctor, "CONNECTING", js_mknum(ES_CONNECTING));
  js_set(js, js->sym.eventsource_ctor, "OPEN", js_mknum(ES_OPEN));
  js_set(js, js->sym.eventsource_ctor, "CLOSED", js_mknum(ES_CLOSED));
  js_set(js, global, "EventSource", js->sym.eventsource_ctor);
}

void gc_mark_eventsource(ant_t *js, gc_mark_fn mark) {
  if (js->sym.eventsource_proto) mark(js, js->sym.eventsource_proto);
  if (js->sym.eventsource_ctor) mark(js, js->sym.eventsource_ctor);
  for (eventsource_state_t *es = g_active_eventsources; es; es = es->next) mark(js, es->obj);
}
