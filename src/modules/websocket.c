#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlsuv/websocket.h>
#include <uv.h>

#include "ant.h"
#include "common.h"
#include "errors.h"
#include "internal.h"
#include "ptr.h"
#include "runtime.h"
#include "silver/engine.h"

#include "http/websocket.h"
#include "modules/buffer.h"
#include "modules/symbol.h"
#include "modules/websocket.h"
#include "net/listener.h"

// TODO: optimize padding
typedef struct websocket_state_s {
  ant_t *js;
  ant_value_t obj;
  ant_value_t request_obj;
  tlsuv_websocket_t client;
  uv_connect_t connect_req;
  ant_conn_t *server_conn;
  struct websocket_state_s *next;
  uint8_t *fragment_buf;
  size_t fragment_len;
  size_t fragment_cap;
  ant_ws_opcode_t fragment_opcode;
  int ready_state;
  bool is_client;
  bool close_emitted;
  bool active;
  bool fragmenting;
} websocket_state_t;

enum {
  WS_NATIVE_TAG = 0x57534f42u, // WSOB
  WS_CONNECTING = 0,
  WS_OPEN = 1,
  WS_CLOSING = 2,
  WS_CLOSED = 3,
};

static ant_value_t g_websocket_proto = 0;
static ant_value_t g_websocket_ctor = 0;
static ant_value_t g_message_event_proto = 0;
static ant_value_t g_close_event_proto = 0;
static websocket_state_t *g_active_websockets = NULL;

static websocket_state_t *websocket_data(ant_value_t value) {
  return (websocket_state_t *)js_get_native(value, WS_NATIVE_TAG);
}

static websocket_state_t *websocket_from_client(tlsuv_websocket_t *client) {
  return client ? (websocket_state_t *)((char *)client - offsetof(websocket_state_t, client)) : NULL;
}

static void websocket_add_active(websocket_state_t *ws) {
  if (!ws || ws->active) return;
  ws->next = g_active_websockets;
  g_active_websockets = ws;
  ws->active = true;
}

static void websocket_remove_active(websocket_state_t *ws) {
  websocket_state_t **it = &g_active_websockets;
  if (!ws || !ws->active) return;
  while (*it) {
    if (*it == ws) {
      *it = ws->next;
      ws->next = NULL;
      ws->active = false;
      return;
    }
    it = &(*it)->next;
  }
  ws->active = false;
}

static void websocket_fragment_clear(websocket_state_t *ws) {
  if (!ws) return;
  free(ws->fragment_buf);
  ws->fragment_buf = NULL;
  ws->fragment_len = 0;
  ws->fragment_cap = 0;
  ws->fragment_opcode = 0;
  ws->fragmenting = false;
}

static bool websocket_fragment_append(websocket_state_t *ws, const uint8_t *data, size_t len) {
  if (!ws) return false;
  if (len > SIZE_MAX - ws->fragment_len) return false;
  size_t need = ws->fragment_len + len;
  if (need > ws->fragment_cap) {
    size_t cap = ws->fragment_cap ? ws->fragment_cap : 1024;
    while (cap < need) {
      if (cap > SIZE_MAX / 2) { cap = need; break; }
      cap *= 2;
    }
    uint8_t *next = realloc(ws->fragment_buf, cap);
    if (!next) return false;
    ws->fragment_buf = next;
    ws->fragment_cap = cap;
  }
  if (len > 0) memcpy(ws->fragment_buf + ws->fragment_len, data, len);
  ws->fragment_len = need;
  return true;
}

static void websocket_free_state(websocket_state_t *ws) {
  if (!ws) return;
  websocket_remove_active(ws);
  websocket_fragment_clear(ws);
  free(ws);
}

static ant_value_t websocket_call(ant_t *js, ant_value_t fn, ant_value_t this_val, ant_value_t *args, int nargs) {
  if (!is_callable(fn)) return js_mkundef();
  ant_value_t saved_this = js->this_val;
  ant_value_t result = js_mkundef();
  js->this_val = this_val;
  if (vtype(fn) == T_CFUNC) result = js_as_cfunc(fn)(js, args, nargs);
  else result = sv_vm_call(js->vm, js, fn, this_val, args, nargs, NULL, false);
  js->this_val = saved_this;
  return result;
}

static void websocket_sync_state(websocket_state_t *ws) {
  if (!ws || !is_object_type(ws->obj)) return;
  js_set(ws->js, ws->obj, "readyState", js_mknum(ws->ready_state));
  js_set(ws->js, ws->obj, "bufferedAmount", js_mknum(0));
}

static ant_value_t websocket_make_event(ant_t *js, ant_value_t proto, const char *type) {
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

static void websocket_emit(websocket_state_t *ws, const char *type, ant_value_t event) {
  ant_t *js = ws->js;
  ant_value_t dispatch = js_get(js, ws->obj, "dispatchEvent");
  ant_value_t args[1] = { event };
  char handler_name[32];

  if (!is_callable(dispatch)) {
    ant_value_t eventtarget_proto = js_get_ctor_proto(js, "EventTarget", 11);
    if (is_object_type(eventtarget_proto)) dispatch = js_get(js, eventtarget_proto, "dispatchEvent");
  }
  websocket_call(js, dispatch, ws->obj, args, 1);
  snprintf(handler_name, sizeof(handler_name), "on%s", type);
  ant_value_t handler = js_get(js, ws->obj, handler_name);
  websocket_call(js, handler, ws->obj, args, 1);
}

static void websocket_emit_simple(websocket_state_t *ws, const char *type) {
  ant_value_t proto = js_get_ctor_proto(ws->js, "Event", 5);
  websocket_emit(ws, type, websocket_make_event(ws->js, proto, type));
}

static void websocket_emit_message(websocket_state_t *ws, const uint8_t *data, size_t len, bool binary) {
  ant_t *js = ws->js;
  ant_value_t event = websocket_make_event(js, g_message_event_proto, "message");
  ant_value_t data_val = 0;

  if (binary) {
    ArrayBufferData *ab = create_array_buffer_data(len);
    if (!ab) return;
    if (len > 0) memcpy(ab->data, data, len);
    data_val = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Uint8Array");
  } else {
    data_val = js_mkstr(js, data, len);
  }
  js_set(js, event, "data", data_val);
  js_set(js, event, "origin", js_mkstr(js, "", 0));
  js_set(js, event, "lastEventId", js_mkstr(js, "", 0));
  websocket_emit(ws, "message", event);
}

static void websocket_emit_close(websocket_state_t *ws, uint16_t code, const char *reason, bool was_clean) {
  if (!ws || ws->close_emitted) return;
  ws->close_emitted = true;
  ws->ready_state = WS_CLOSED;
  websocket_fragment_clear(ws);
  websocket_remove_active(ws);
  websocket_sync_state(ws);

  ant_t *js = ws->js;
  ant_value_t event = websocket_make_event(js, g_close_event_proto, "close");
  js_set(js, event, "code", js_mknum(code));
  js_set(js, event, "reason", js_mkstr(js, reason ? reason : "", reason ? strlen(reason) : 0));
  js_set(js, event, "wasClean", js_bool(was_clean));
  websocket_emit(ws, "close", event);
}

static void websocket_client_close_cb(uv_handle_t *handle) {
  websocket_state_t *ws = websocket_from_client((tlsuv_websocket_t *)handle);
  websocket_emit_close(ws, 1000, "", true);
}

static void websocket_client_connect_cb(uv_connect_t *req, int status) {
  websocket_state_t *ws = websocket_from_client((tlsuv_websocket_t *)req->handle);
  if (!ws) return;

  if (status == 0) {
    ws->ready_state = WS_OPEN;
    websocket_sync_state(ws);
    websocket_emit_simple(ws, "open");
    return;
  }

  ws->ready_state = WS_CLOSED;
  websocket_sync_state(ws);
  websocket_emit_simple(ws, "error");
  websocket_emit_close(ws, 1006, "", false);
}

static void websocket_client_read_cb(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf) {
  websocket_state_t *ws = websocket_from_client((tlsuv_websocket_t *)handle);
  if (!ws) return;

  if (nread > 0) {
    websocket_emit_message(ws, (const uint8_t *)buf->base, (size_t)nread, false);
    return;
  }

  if (nread < 0) {
    if (nread != UV_EOF) websocket_emit_simple(ws, "error");
    websocket_emit_close(ws, nread == UV_EOF ? 1000 : 1006, "", nread == UV_EOF);
  }
}

static void websocket_write_cb(uv_write_t *req, int status) {
  (void)status;
  free(req);
}

static void websocket_finalize(ant_t *js, ant_object_t *obj) {
  (void)js;
  ant_value_t value = js_obj_from_ptr(obj);
  websocket_state_t *ws = websocket_data(value);
  if (!ws) return;
  js_clear_native(value, WS_NATIVE_TAG);
  websocket_free_state(ws);
}

static websocket_state_t *websocket_state_new(ant_t *js, ant_value_t obj) {
  websocket_state_t *ws = calloc(1, sizeof(*ws));
  if (!ws) return NULL;
  ws->js = js;
  ws->obj = obj;
  ws->ready_state = WS_CONNECTING;
  websocket_add_active(ws);
  return ws;
}

static ant_value_t websocket_create_object(ant_t *js) {
  ant_value_t obj = js_mkobj(js);
  if (is_object_type(g_websocket_proto)) js_set_proto_init(obj, g_websocket_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_EVENTTARGET));
  js_set(js, obj, "binaryType", js_mkstr(js, "arraybuffer", 11));
  js_set(js, obj, "bufferedAmount", js_mknum(0));
  js_set(js, obj, "extensions", js_mkstr(js, "", 0));
  js_set(js, obj, "protocol", js_mkstr(js, "", 0));
  js_set(js, obj, "onopen", js_mknull());
  js_set(js, obj, "onmessage", js_mknull());
  js_set(js, obj, "onerror", js_mknull());
  js_set(js, obj, "onclose", js_mknull());
  js_set_finalizer(obj, websocket_finalize);
  return obj;
}

static ant_value_t js_websocket_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WebSocket constructor requires 'new'");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "WebSocket URL is required");

  ant_value_t url_val = js_tostring_val(js, args[0]);
  if (is_err(url_val)) return url_val;
  const char *url = js_getstr(js, url_val, NULL);
  if (!url || (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0))
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "WebSocket URL must use ws: or wss:");

  ant_value_t obj = websocket_create_object(js);
  websocket_state_t *ws = websocket_state_new(js, obj);
  if (!ws) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  ws->is_client = true;
  js_set_native(obj, ws, WS_NATIVE_TAG);
  js_set(js, obj, "url", url_val);
  websocket_sync_state(ws);

  if (tlsuv_websocket_init(uv_default_loop(), &ws->client) != 0) {
    ws->ready_state = WS_CLOSED;
    websocket_sync_state(ws);
    return obj;
  }

  int rc = tlsuv_websocket_connect(&ws->connect_req, &ws->client, url, websocket_client_connect_cb, websocket_client_read_cb);
  if (rc != 0) {
    ws->ready_state = WS_CLOSED;
    websocket_sync_state(ws);
    websocket_emit_simple(ws, "error");
    websocket_emit_close(ws, 1006, "", false);
  }

  return obj;
}

static bool websocket_bytes_from_value(ant_t *js, ant_value_t value, const uint8_t **bytes, size_t *len, ant_value_t *owned_str) {
  if (buffer_source_get_bytes(js, value, bytes, len)) {
    if (owned_str) *owned_str = js_mkundef();
    return true;
  }

  ant_value_t str = js_tostring_val(js, value);
  if (is_err(str)) return false;
  if (owned_str) *owned_str = str;
  *bytes = (const uint8_t *)js_getstr(js, str, len);
  return *bytes != NULL;
}

static ant_value_t js_websocket_send(ant_t *js, ant_value_t *args, int nargs) {
  websocket_state_t *ws = websocket_data(js_getthis(js));
  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t keepalive = js_mkundef();
  bool binary = false;

  if (!ws) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WebSocket");
  if (ws->ready_state != WS_OPEN) return js_mkerr_typed(js, JS_ERR_TYPE, "WebSocket is not open");
  if (nargs < 1) return js_mkundef();
  binary = buffer_source_get_bytes(js, args[0], &bytes, &len);
  if (!binary && !websocket_bytes_from_value(js, args[0], &bytes, &len, &keepalive))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WebSocket message");

  if (ws->is_client) {
    uv_write_t *wr = calloc(1, sizeof(*wr));
    if (!wr) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
    uv_buf_t buf = uv_buf_init((char *)bytes, (unsigned int)len);
    int rc = tlsuv_websocket_write(wr, &ws->client, &buf, websocket_write_cb);
    if (rc != 0) {
      free(wr);
      return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
    }
    return js_mkundef();
  }

  if (ws->server_conn) {
    size_t frame_len = 0;
    uint8_t *frame = ant_ws_encode_frame(binary ? ANT_WS_OPCODE_BINARY : ANT_WS_OPCODE_TEXT, bytes, len, false, &frame_len);
    if (!frame) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
    int rc = ant_conn_write(ws->server_conn, (char *)frame, frame_len, NULL, NULL);
    if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
  }
  return js_mkundef();
}

static ant_value_t js_websocket_close(ant_t *js, ant_value_t *args, int nargs) {
  websocket_state_t *ws = websocket_data(js_getthis(js));
  uint16_t code = nargs > 0 && vtype(args[0]) == T_NUM ? (uint16_t)js_getnum(args[0]) : 1000;
  const char *reason = NULL;

  if (!ws) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WebSocket");
  if (ws->ready_state == WS_CLOSED || ws->ready_state == WS_CLOSING) return js_mkundef();
  if (nargs > 1 && vtype(args[1]) != T_UNDEF && vtype(args[1]) != T_NULL) {
    ant_value_t reason_val = js_tostring_val(js, args[1]);
    if (is_err(reason_val)) return reason_val;
    reason = js_getstr(js, reason_val, NULL);
  }

  ws->ready_state = WS_CLOSING;
  websocket_sync_state(ws);
  if (ws->is_client) {
    tlsuv_websocket_close(&ws->client, websocket_client_close_cb);
  } else if (ws->server_conn) {
    size_t frame_len = 0;
    uint8_t *frame = ant_ws_encode_close_frame(code, reason, false, &frame_len);
    if (frame) ant_conn_write(ws->server_conn, (char *)frame, frame_len, NULL, NULL);
    ant_conn_shutdown(ws->server_conn);
  }
  return js_mkundef();
}

static ant_value_t js_message_event_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "MessageEvent constructor requires 'new'");
  ant_value_t type_val = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "message", 7);
  if (is_err(type_val)) return type_val;
  ant_value_t event = websocket_make_event(js, g_message_event_proto, js_str(js, type_val));
  ant_value_t init = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t data = js_mknull();
  if (is_object_type(init)) {
    ant_value_t d = js_get(js, init, "data");
    if (vtype(d) != T_UNDEF) data = d;
  }
  js_set(js, event, "data", data);
  return event;
}

static ant_value_t js_close_event_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "CloseEvent constructor requires 'new'");
  ant_value_t type_val = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "close", 5);
  if (is_err(type_val)) return type_val;
  ant_value_t event = websocket_make_event(js, g_close_event_proto, js_str(js, type_val));
  ant_value_t init = nargs > 1 ? args[1] : js_mkundef();
  js_set(js, event, "wasClean", js_false);
  js_set(js, event, "code", js_mknum(0));
  js_set(js, event, "reason", js_mkstr(js, "", 0));
  if (is_object_type(init)) {
    ant_value_t was_clean = js_get(js, init, "wasClean");
    ant_value_t code = js_get(js, init, "code");
    ant_value_t reason = js_get(js, init, "reason");
    if (vtype(was_clean) != T_UNDEF) js_set(js, event, "wasClean", js_bool(js_truthy(js, was_clean)));
    if (vtype(code) != T_UNDEF) js_set(js, event, "code", js_mknum(js_to_number(js, code)));
    if (vtype(reason) != T_UNDEF) {
      ant_value_t reason_str = js_tostring_val(js, reason);
      if (!is_err(reason_str)) js_set(js, event, "reason", reason_str);
    }
  }
  return event;
}

ant_value_t ant_websocket_accept_server(
  ant_t *js,
  ant_conn_t *conn,
  ant_value_t request_obj,
  const char *protocol
) {
  ant_value_t obj = websocket_create_object(js);
  websocket_state_t *ws = websocket_state_new(js, obj);
  if (!ws) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  ws->server_conn = conn;
  ws->request_obj = request_obj;
  ws->ready_state = WS_CONNECTING;
  js_set_native(obj, ws, WS_NATIVE_TAG);
  js_set(js, obj, "url", js_mkstr(js, "", 0));
  if (protocol) js_set(js, obj, "protocol", js_mkstr(js, protocol, strlen(protocol)));
  websocket_sync_state(ws);
  return obj;
}

void ant_websocket_server_open(ant_t *js, ant_value_t socket_obj) {
  (void)js;
  websocket_state_t *ws = websocket_data(socket_obj);
  if (!ws) return;
  ws->ready_state = WS_OPEN;
  websocket_sync_state(ws);
  websocket_emit_simple(ws, "open");
}

void ant_websocket_server_on_read(ant_t *js, ant_value_t socket_obj, ant_conn_t *conn) {
  websocket_state_t *ws = websocket_data(socket_obj);
  if (!ws || !conn) return;

  while (ant_conn_buffer_len(conn) > 0) {
    ant_ws_frame_t frame = {0};
    ant_ws_frame_result_t result = ant_ws_parse_frame(
      (const uint8_t *)ant_conn_buffer(conn),
      ant_conn_buffer_len(conn),
      true,
      &frame
    );
    if (result == ANT_WS_FRAME_INCOMPLETE) return;
    if (result == ANT_WS_FRAME_PROTOCOL_ERROR) {
      websocket_emit_simple(ws, "error");
      ant_conn_close(conn);
      return;
    }

    ant_conn_consume(conn, frame.consumed_len);
    switch (frame.opcode) {
      case ANT_WS_OPCODE_TEXT:
      case ANT_WS_OPCODE_BINARY:
        if (ws->fragmenting) {
          websocket_emit_simple(ws, "error");
          ant_ws_frame_clear(&frame);
          ant_conn_close(conn);
          return;
        }
        if (frame.fin) {
          websocket_emit_message(ws, frame.payload, frame.payload_len, frame.opcode == ANT_WS_OPCODE_BINARY);
        } else {
          ws->fragmenting = true;
          ws->fragment_opcode = frame.opcode;
          if (!websocket_fragment_append(ws, frame.payload, frame.payload_len)) {
            websocket_emit_simple(ws, "error");
            ant_ws_frame_clear(&frame);
            ant_conn_close(conn);
            return;
          }
        }
        break;
      case ANT_WS_OPCODE_CONTINUATION:
        if (!ws->fragmenting) {
          websocket_emit_simple(ws, "error");
          ant_ws_frame_clear(&frame);
          ant_conn_close(conn);
          return;
        }
        if (!websocket_fragment_append(ws, frame.payload, frame.payload_len)) {
          websocket_emit_simple(ws, "error");
          ant_ws_frame_clear(&frame);
          ant_conn_close(conn);
          return;
        }
        if (frame.fin) {
          websocket_emit_message(ws, ws->fragment_buf, ws->fragment_len, ws->fragment_opcode == ANT_WS_OPCODE_BINARY);
          websocket_fragment_clear(ws);
        }
        break;
      case ANT_WS_OPCODE_PING: {
        size_t out_len = 0;
        uint8_t *out = ant_ws_encode_frame(ANT_WS_OPCODE_PONG, frame.payload, frame.payload_len, false, &out_len);
        if (out) ant_conn_write(conn, (char *)out, out_len, NULL, NULL);
        break;
      }
      case ANT_WS_OPCODE_CLOSE: {
        uint16_t code = 1000;
        const char *reason = "";
        char reason_buf[124];
        if (frame.payload_len >= 2) {
          code = (uint16_t)(((uint16_t)frame.payload[0] << 8) | frame.payload[1]);
          size_t reason_len = frame.payload_len - 2;
          if (reason_len > sizeof(reason_buf) - 1) reason_len = sizeof(reason_buf) - 1;
          if (reason_len > 0) memcpy(reason_buf, frame.payload + 2, reason_len);
          reason_buf[reason_len] = '\0';
          reason = reason_buf;
        }
        size_t out_len = 0;
        uint8_t *out = ant_ws_encode_frame(ANT_WS_OPCODE_CLOSE, frame.payload, frame.payload_len, false, &out_len);
        if (out) ant_conn_write(conn, (char *)out, out_len, NULL, NULL);
        websocket_emit_close(ws, code, reason, true);
        ant_conn_shutdown(conn);
        break;
      }
      default:
        break;
    }
    ant_ws_frame_clear(&frame);
  }
}

void ant_websocket_server_on_close(ant_t *js, ant_value_t socket_obj) {
  websocket_state_t *ws = websocket_data(socket_obj);
  if (!ws) return;
  ws->server_conn = NULL;
  websocket_emit_close(ws, 1000, "", true);
}

void init_websocket_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);
  ant_value_t event_proto = js_get_ctor_proto(js, "Event", 5);
  ant_value_t eventtarget_proto = js_get_ctor_proto(js, "EventTarget", 11);

  g_websocket_proto = js_mkobj(js);
  if (is_object_type(eventtarget_proto)) js_set_proto_init(g_websocket_proto, eventtarget_proto);
  js_set(js, g_websocket_proto, "send", js_mkfun(js_websocket_send));
  js_set(js, g_websocket_proto, "close", js_mkfun(js_websocket_close));
  js_set(js, g_websocket_proto, "CONNECTING", js_mknum(WS_CONNECTING));
  js_set(js, g_websocket_proto, "OPEN", js_mknum(WS_OPEN));
  js_set(js, g_websocket_proto, "CLOSING", js_mknum(WS_CLOSING));
  js_set(js, g_websocket_proto, "CLOSED", js_mknum(WS_CLOSED));
  js_set_sym(js, g_websocket_proto, get_toStringTag_sym(), js_mkstr(js, "WebSocket", 9));
  g_websocket_ctor = js_make_ctor(js, js_websocket_ctor, g_websocket_proto, "WebSocket", 9);
  js_set(js, g_websocket_ctor, "CONNECTING", js_mknum(WS_CONNECTING));
  js_set(js, g_websocket_ctor, "OPEN", js_mknum(WS_OPEN));
  js_set(js, g_websocket_ctor, "CLOSING", js_mknum(WS_CLOSING));
  js_set(js, g_websocket_ctor, "CLOSED", js_mknum(WS_CLOSED));
  js_set(js, global, "WebSocket", g_websocket_ctor);

  g_message_event_proto = js_mkobj(js);
  if (is_object_type(event_proto)) js_set_proto_init(g_message_event_proto, event_proto);
  js_set_sym(js, g_message_event_proto, get_toStringTag_sym(), js_mkstr(js, "MessageEvent", 12));
  ant_value_t message_ctor = js_make_ctor(js, js_message_event_ctor, g_message_event_proto, "MessageEvent", 12);
  js_set(js, global, "MessageEvent", message_ctor);

  g_close_event_proto = js_mkobj(js);
  if (is_object_type(event_proto)) js_set_proto_init(g_close_event_proto, event_proto);
  js_set_sym(js, g_close_event_proto, get_toStringTag_sym(), js_mkstr(js, "CloseEvent", 10));
  ant_value_t close_ctor = js_make_ctor(js, js_close_event_ctor, g_close_event_proto, "CloseEvent", 10);
  js_set(js, global, "CloseEvent", close_ctor);
}

void gc_mark_websocket(ant_t *js, gc_mark_fn mark) {
  if (g_websocket_proto) mark(js, g_websocket_proto);
  if (g_websocket_ctor) mark(js, g_websocket_ctor);
  if (g_message_event_proto) mark(js, g_message_event_proto);
  if (g_close_event_proto) mark(js, g_close_event_proto);
  
  for (websocket_state_t *ws = g_active_websockets; ws; ws = ws->next) {
    mark(js, ws->obj);
    if (is_object_type(ws->request_obj)) mark(js, ws->request_obj);
  }
}
