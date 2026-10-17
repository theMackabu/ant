#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uv.h>

#include "ant.h"
#include "cfunc.h"
#include "list.h"
#include "loop.h"
#include "ptr.h"
#include "common.h"
#include "errors.h"
#include "internal.h"
#include "inspector.h"

#include "http/eventsource.h"
#include "modules/events.h"
#include "modules/eventsource.h"
#include "modules/http.h"
#include "modules/timer.h"
#include "modules/url.h"

typedef struct eventsource_timer_s {
  uv_timer_t handle;
  struct eventsource_state_s *source;
} eventsource_timer_t;

struct eventsource_state_s {
  ant_value_t obj;
  ant_http_request_t *request;
  eventsource_timer_t *timer;
  uint64_t network_request_id;
  size_t network_encoded_length;
  ant_sse_parser_t parser;
  char *url;
  char *last_event_id;
  eventsource_state_t *next;
  eventsource_state_t **pprev;
  uint32_t retry_ms;
  uint8_t ready_state;
  bool with_credentials : 1;
  bool closed : 1;
  bool opened_request : 1;
  bool error_emitted_for_request : 1;
};

static constexpr uint32_t ES_NATIVE_TAG = Ant_NativeTag("EVSR");
enum { ES_CONNECTING = 0, ES_OPEN = 1, ES_CLOSED = 2 };

static eventsource_state_t *eventsource_data(ant_value_t value) {
  return (eventsource_state_t *)js_get_native(value, ES_NATIVE_TAG);
}

static void eventsource_maybe_unroot(eventsource_state_t *es) {
  if (!es || !es->closed) return;
  if (es->request || es->timer) return;
  Ant_List_Remove(es);
}

static void eventsource_sync_state(ant_t *js, eventsource_state_t *es) {
  if (!es || !is_object_type(es->obj)) return;
  js_set(js, es->obj, "readyState", js_mknum(es->ready_state));
}

static void eventsource_emit(ant_t *js, eventsource_state_t *es, ant_value_t event) {
  Ant_EventTarget_Dispatch(js, es->obj, event);
  process_microtasks(js);
}

static void eventsource_emit_simple(ant_t *js, eventsource_state_t *es, const char *type) {
  ant_value_t event = Ant_EventTarget_CreateEvent(js, es->obj, js->builtins.event_proto, ANT_EVENT_BASE, type);
  if (is_object_type(event)) eventsource_emit(js, es, event);
}

static void eventsource_emit_error_once(ant_t *js, eventsource_state_t *es) {
  if (!es || es->closed || es->error_emitted_for_request) return;
  es->error_emitted_for_request = true;
  eventsource_emit_simple(js, es, "error");
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

static void eventsource_start_request(ant_t *js, eventsource_state_t *es);

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
  eventsource_start_request(Ant_Loop_Isolate(handle->loop), es);
}

static void eventsource_cancel_timer(eventsource_state_t *es) {
  eventsource_timer_t *timer = es ? es->timer : NULL;
  if (!timer) return;
  es->timer = NULL;
  uv_timer_stop(&timer->handle);
  uv_close((uv_handle_t *)&timer->handle, eventsource_timer_close_cb);
}

static void eventsource_schedule_reconnect(ant_t *js, eventsource_state_t *es) {
  if (!es || es->closed || es->timer) return;
  eventsource_timer_t *timer = calloc(1, sizeof(*timer));
  if (!timer) {
    es->closed = true;
    es->ready_state = ES_CLOSED;
    eventsource_sync_state(js, es);
    eventsource_maybe_unroot(es);
    return;
  }
  timer->source = es;
  uv_timer_init(js->loop, &timer->handle);
  es->timer = timer;
  es->ready_state = ES_CONNECTING;
  eventsource_sync_state(js, es);
  uv_timer_start(&timer->handle, eventsource_reconnect_cb, es->retry_ms, 0);
}

typedef struct {
  ant_t *js;
  eventsource_state_t *es;
} eventsource_feed_t;

static bool eventsource_on_message(const ant_sse_message_t *message, void *user_data) {
  eventsource_feed_t *feed = (eventsource_feed_t *)user_data;
  eventsource_state_t *es = feed->es;
  ant_t *js = feed->js;
  
  const char *type = (message->event && message->event[0]) ? message->event : "message";

  if (message->id) {
    free(es->last_event_id);
    es->last_event_id = strdup(message->id);
  } else if (es->parser.last_event_id) {
    free(es->last_event_id);
    es->last_event_id = strdup(es->parser.last_event_id);
  }

  if (!es->last_event_id) es->last_event_id = strdup("");
  ant_value_t event = Ant_EventTarget_CreateEvent(js, es->obj, js->builtins.message_event_proto, ANT_EVENT_MESSAGE, type);
  if (!is_object_type(event)) return !es->closed;

  Ant_Event_SetField(
    js, event, ANT_EVENT_FIELD_MESSAGE_data, 
    js_mkstr(js, message->data ? message->data : "", message->data ? strlen(message->data) : 0)
  );

  Ant_Event_SetField(
    js, event, ANT_EVENT_FIELD_MESSAGE_lastEventId, 
    js_mkstr(js, es->last_event_id ? es->last_event_id : "", es->last_event_id ? strlen(es->last_event_id) : 0)
  );
  
  Ant_Event_SetField(js, event, ANT_EVENT_FIELD_MESSAGE_origin, js_mkstr(js, "", 0));
  eventsource_emit(js, es, event);
  
  return !es->closed;
}

static void eventsource_http_on_response(ant_http_request_t *req, const ant_http_response_t *resp, void *user_data) {
  eventsource_state_t *es = (eventsource_state_t *)user_data;
  ant_t *js = Ant_Loop_Isolate(Ant_Http_RequestLoop(req));
  const char *content_type = eventsource_find_header(resp->headers, "content-type");

  if (!es || es->closed) return;
  
  ant_inspector_network_response(
    es->network_request_id, es->url,
    resp->status, resp->status_text,
    content_type ? content_type : "text/event-stream",
    "EventSource", resp->headers
  );
  
  if (resp->status != 200 || !eventsource_content_type_ok(content_type)) {
    eventsource_emit_error_once(js, es);
    ant_http_request_cancel(req);
    return;
  }

  es->opened_request = true;
  es->ready_state = ES_OPEN;
  eventsource_sync_state(js, es);
  eventsource_emit_simple(js, es, "open");
}

static void eventsource_http_on_body(ant_http_request_t *req, const uint8_t *chunk, size_t len, void *user_data) {
  eventsource_state_t *es = (eventsource_state_t *)user_data;
  if (!es || es->closed || !es->opened_request) return;
  
  es->network_encoded_length += len;
  ant_inspector_network_append_response_body(es->network_request_id, chunk, len);
  
  eventsource_feed_t feed = { Ant_Loop_Isolate(Ant_Http_RequestLoop(req)), es };
  if (!ant_sse_parser_feed(&es->parser, (const char *)chunk, len, eventsource_on_message, &feed)) {
    eventsource_emit_error_once(feed.js, es);
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
  ant_t *js = Ant_Loop_Isolate(Ant_Http_RequestLoop(req));

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
    if (result != ANT_HTTP_RESULT_ABORTED) eventsource_emit_error_once(js, es);
    es->error_emitted_for_request = false;
    ant_sse_parser_free(&es->parser);
    ant_sse_parser_init(&es->parser);
    eventsource_schedule_reconnect(js, es);
  }
  eventsource_maybe_unroot(es);
}

static void eventsource_start_request(ant_t *js, eventsource_state_t *es) {
  ant_http_header_t last_id = {0};
  ant_http_header_t accept = { "Accept", "text/event-stream", NULL };
  ant_http_request_options_t options = {0};
  int rc = 0;

  if (!es || es->closed || es->request) return;
  es->ready_state = ES_CONNECTING;
  es->error_emitted_for_request = false;
  eventsource_sync_state(js, es);

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
    js->loop, &options,
    eventsource_http_on_response,
    eventsource_http_on_body,
    eventsource_http_on_complete,
    es, &es->request
  );
  
  if (rc != 0) {
    ant_inspector_network_fail(es->network_request_id, uv_strerror(rc), false, "EventSource");
    es->network_request_id = 0;
    es->request = NULL;
    eventsource_emit_error_once(js, es);
    es->error_emitted_for_request = false;
    eventsource_schedule_reconnect(js, es);
  }
}

static void eventsource_close(ant_t *js, eventsource_state_t *es) {
  if (!es || es->closed) return;
  es->closed = true;
  es->ready_state = ES_CLOSED;
  eventsource_sync_state(js, es);
  eventsource_cancel_timer(es);
  if (es->request) ant_http_request_cancel(es->request);
  eventsource_maybe_unroot(es);
}

static void eventsource_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  eventsource_state_t *es = eventsource_data(value);
  if (!es) return;
  
  js_clear_native(value, ES_NATIVE_TAG);
  if (es->request) ant_http_request_cancel(es->request);
  
  eventsource_cancel_timer(es);
  Ant_List_Remove(es);
  ant_sse_parser_free(&es->parser);
  
  free(es->url);
  free(es->last_event_id);
  free(es);
}

static ant_value_t eventsource_create_object(ant_t *js) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_object_type(js->builtins.eventsource_proto)) js_set_proto_init(obj, js->builtins.eventsource_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_EVENTTARGET));
  js_set_finalizer(obj, eventsource_finalize);
  return obj;
}

static ant_value_t js_eventsource_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "EventSource constructor requires 'new'");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "EventSource URL is required");

  ant_value_t url_val = js_tostring_val(js, args[0]);
  if (is_err(url_val)) return url_val;
  
  const char *input = js_getstr(js, url_val, NULL);
  url_state_t parsed = {0};
  
  if (!input || Ant_URL_ParseDocumentRelative(js, input, &parsed) != 0) {
    url_state_clear(&parsed);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "Failed to construct 'EventSource': Cannot open an EventSource to '%s'. The URL is invalid.", input ? input : "");
  }
  
  char *href = build_href(&parsed);
  url_state_clear(&parsed);
  if (!href) return js_mkerr(js, "oom");
  
  if (!eventsource_is_http_url(href)) {
    free(href);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "EventSource URL must use http: or https:");
  }
  
  url_val = js_mkstr(js, href, strlen(href));
  free(href);
  const char *url = js_getstr(js, url_val, NULL);

  ant_value_t obj = eventsource_create_object(js);
  eventsource_state_t *es = calloc(1, sizeof(*es));
  if (!es) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  es->obj = obj;
  es->url = strdup(url);
  es->last_event_id = strdup("");
  es->retry_ms = 3000;
  es->ready_state = ES_CONNECTING;
  
  if (nargs > 1 && is_object_type(args[1])) {
    ant_value_t with_credentials = js_get(js, args[1], "withCredentials");
    if (vtype(with_credentials) != kTypeUndefined) es->with_credentials = js_truthy(js, with_credentials);
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
  
  eventsource_sync_state(js, es);
  Ant_List_Insert(&js->active_eventsources, es);
  eventsource_start_request(js, es);
  
  return obj;
}

static ant_value_t js_eventsource_close(ant_params_t) {
  eventsource_state_t *es = eventsource_data(js_getthis(js));
  if (!es) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid EventSource");
  eventsource_close(js, es);
  return js_mkundef();
}

static ant_value_t eventsource_invalid_this(ant_t *js, ant_value_t this_obj) {
  return eventsource_data(this_obj) ? 0 : js_mkerr_typed(js, JS_ERR_TYPE, "Invalid EventSource");
}

ANT_EVENT_HANDLER_ACCESSORS(eventsource, open, eventsource_invalid_this)
ANT_EVENT_HANDLER_ACCESSORS(eventsource, message, eventsource_invalid_this)
ANT_EVENT_HANDLER_ACCESSORS(eventsource, error, eventsource_invalid_this)

void init_eventsource_module(ant_t *js) {
  ant_value_t eventtarget_proto = js->builtins.eventtarget_proto;
  js->builtins.eventsource_proto = Ant_Object_CreateBare(js);
  
  if (is_object_type(eventtarget_proto)) js_set_proto_init(js->builtins.eventsource_proto, eventtarget_proto);
  js_set(js, js->builtins.eventsource_proto, "close", Ant_Function_Create(js_eventsource_close));
  
  ANT_EVENT_HANDLER_DEFINE(js->builtins.eventsource_proto, eventsource, open);
  ANT_EVENT_HANDLER_DEFINE(js->builtins.eventsource_proto, eventsource, message);
  ANT_EVENT_HANDLER_DEFINE(js->builtins.eventsource_proto, eventsource, error);
  
  js_set(js, js->builtins.eventsource_proto, "CONNECTING", js_mknum(ES_CONNECTING));
  js_set(js, js->builtins.eventsource_proto, "OPEN", js_mknum(ES_OPEN));
  js_set(js, js->builtins.eventsource_proto, "CLOSED", js_mknum(ES_CLOSED));
  js_set_tostringtag(js, js->builtins.eventsource_proto, js_mkstr(js, "EventSource", 11));

  js->builtins.eventsource_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_eventsource_ctor, js->builtins.eventsource_proto, "EventSource", 1);
  js_set(js, js->builtins.eventsource_ctor, "CONNECTING", js_mknum(ES_CONNECTING));
  js_set(js, js->builtins.eventsource_ctor, "OPEN", js_mknum(ES_OPEN));
  js_set(js, js->builtins.eventsource_ctor, "CLOSED", js_mknum(ES_CLOSED));
  js_set_global_builtin(js, "EventSource", js->builtins.eventsource_ctor);
}

void cleanup_eventsource_module(ant_t *js) {
  for (eventsource_state_t *es = js->active_eventsources, *next; es; es = next) {
    next = es->next;
    eventsource_close(js, es);
  }
}

void gc_mark_eventsource(ant_t *js, gc_mark_fn mark) {
  for (eventsource_state_t *es = js->active_eventsources; es; es = es->next) mark(js, es->obj);
}
