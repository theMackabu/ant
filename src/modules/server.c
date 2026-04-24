#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <uv.h>

#include "ant.h"
#include "internal.h"
#include "ptr.h"

#include "gc/modules.h"
#include "net/listener.h"
#include "streams/readable.h"

#include "http/http1_parser.h"
#include "http/http1_writer.h"

#include "modules/assert.h"
#include "modules/abort.h"
#include "modules/buffer.h"
#include "modules/headers.h"
#include "modules/request.h"
#include "modules/response.h"
#include "modules/server.h"
#include "modules/timer.h"
#include "modules/domexception.h"

typedef struct server_runtime_s    server_runtime_t;
typedef struct server_request_s    server_request_t;
typedef struct server_conn_state_s server_conn_state_t;

static server_runtime_t *g_server = NULL;

typedef struct stop_waiter_s {
  ant_value_t promise;
  struct stop_waiter_s *next;
} stop_waiter_t;

typedef enum {
  SERVER_WRITE_NONE = 0,
  SERVER_WRITE_CLOSE_CLIENT,
  SERVER_WRITE_STREAM_READ,
  SERVER_WRITE_KEEP_ALIVE,
} server_write_action_t;

typedef struct {
  server_request_t *request;
  ant_conn_t *conn;
  server_write_action_t action;
} server_write_req_t;

struct server_request_s {
  server_runtime_t *server;
  server_conn_state_t *conn_state;
  ant_conn_t *conn;
  
  ant_value_t request_obj;
  ant_value_t response_obj;
  ant_value_t response_promise;
  ant_value_t response_reader;
  ant_value_t response_read_promise;
  
  struct server_request_s *next;
  size_t consumed_len;
  
  int refs;
  bool keep_alive;
  bool response_started;
};

struct server_conn_state_s {
  server_runtime_t *server;
  ant_conn_t *conn;
  
  ant_http1_conn_parser_t parser;
  uv_timer_t drain_timer;
  server_request_t request;
  server_request_t *active_req;
  
  bool drain_timer_closed;
  bool drain_scheduled;
};

struct server_runtime_s {
  ant_t *js;
  
  ant_value_t export_obj;
  ant_value_t fetch_fn;
  ant_value_t server_ctx;
  uv_loop_t *loop;
  
  ant_listener_t listener;
  uv_signal_t sigint_handle;
  uv_signal_t sigterm_handle;
  stop_waiter_t *stop_waiters;
  server_request_t *requests;
  
  char *hostname;
  char *unix_path;
  
  uint64_t request_timeout_ms;
  uint64_t idle_timeout_ms;
  
  int port;
  bool sigint_closed;
  bool sigterm_closed;
  bool stopping;
  bool force_stop;
};

static inline void server_request_retain(server_request_t *req) {
  if (req) req->refs++;
}

static inline server_request_t *server_current_request(ant_t *js) {
  return (server_request_t *)js_get_native_ptr(js->current_func);
}

static inline server_runtime_t *server_current_runtime(ant_t *js) {
  return (server_runtime_t *)js_get_native_ptr(js->current_func);
}

static ant_value_t server_mkreqfun(
  ant_t *js,
  ant_value_t (*fn)(ant_t *, ant_value_t *, int),
  server_request_t *req
) {
  ant_value_t func = js_heavy_mkfun(js, fn, js_mkundef());
  js_set_native_ptr(func, req);
  return func;
}

static ant_value_t server_mkruntimefun(
  ant_t *js,
  ant_value_t (*fn)(ant_t *, ant_value_t *, int),
  server_runtime_t *server
) {
  ant_value_t func = js_heavy_mkfun(js, fn, js_mkundef());
  js_set_native_ptr(func, server);
  return func;
}

static ant_value_t server_exception_reason(ant_t *js, ant_value_t value) {
  if (!is_err(value)) return value;
  if (!js->thrown_exists) return value;

  value = js->thrown_value;
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
  js->thrown_stack = js_mkundef();
  return value;
}

static void server_remove_request(server_runtime_t *server, server_request_t *req) {
  server_request_t **it = NULL;
  if (!server || !req) return;

  for (it = &server->requests; *it; it = &(*it)->next) {
  if (*it == req) {
    *it = req->next;
    return;
  }}
}

static void server_request_reset(server_request_t *req) {
  server_runtime_t *server = NULL;
  server_conn_state_t *conn_state = NULL;

  if (!req) return;
  server = req->server;
  conn_state = req->conn_state;
  
  *req = (server_request_t){
    .server = server,
    .conn_state = conn_state,
    .conn = conn_state ? conn_state->conn : NULL,
    .request_obj = js_mkundef(),
    .response_obj = js_mkundef(),
    .response_promise = js_mkundef(),
    .response_reader = js_mkundef(),
    .response_read_promise = js_mkundef(),
  };
}

static void server_conn_state_maybe_free(server_conn_state_t *cs) {
  if (!cs) return;
  if (cs->conn) return;
  if (cs->active_req) return;
  if (cs->request.refs > 0) return;
  if (!cs->drain_timer_closed) return;
  free(cs);
}

static void server_request_release(server_request_t *req) {
  if (!req) return;
  if (--req->refs > 0) return;

  server_remove_request(req->server, req);
  server_request_reset(req);
  server_conn_state_maybe_free(req->conn_state);
}

static server_request_t *server_find_request(server_runtime_t *server, ant_value_t request_obj) {
  server_request_t *req = NULL;
  if (!server || !is_object_type(request_obj)) return NULL;

  for (req = server->requests; req; req = req->next) {
    if (req->request_obj == request_obj) return req;
  }
  return NULL;
}
static void stop_waiters_resolve(server_runtime_t *server) {
  stop_waiter_t *waiter = server->stop_waiters;
  server->stop_waiters = NULL;

  while (waiter) {
    stop_waiter_t *next = waiter->next;
    js_resolve_promise(server->js, waiter->promise, js_mkundef());
    free(waiter);
    waiter = next;
  }
}

static void server_maybe_finish_stop(server_runtime_t *server) {
  if (!server || !server->stopping) return;
  if (ant_listener_has_connections(&server->listener)) return;
  if (!ant_listener_is_closed(&server->listener) || !server->sigint_closed || !server->sigterm_closed) return;

  stop_waiters_resolve(server);
}

static void server_signal_close_cb(uv_handle_t *handle) {
  server_runtime_t *server = (server_runtime_t *)handle->data;
  if (!server) return;

  if (handle == (uv_handle_t *)&server->sigint_handle) server->sigint_closed = true;
  if (handle == (uv_handle_t *)&server->sigterm_handle) server->sigterm_closed = true;
  server_maybe_finish_stop(server);
}

static void server_begin_stop(server_runtime_t *server, bool force) {
  if (!server) return;
  if (force) server->force_stop = true;
  
  server->stopping = true;
  ant_listener_stop(&server->listener, server->force_stop);

  if (!uv_is_closing((uv_handle_t *)&server->sigint_handle))
    uv_close((uv_handle_t *)&server->sigint_handle, server_signal_close_cb);
  else server->sigint_closed = true;

  if (!uv_is_closing((uv_handle_t *)&server->sigterm_handle))
    uv_close((uv_handle_t *)&server->sigterm_handle, server_signal_close_cb);
  else server->sigterm_closed = true;

  server_maybe_finish_stop(server);
}

static void server_signal_cb(uv_signal_t *handle, int signum) {
  server_runtime_t *server = (server_runtime_t *)handle->data;
  server_begin_stop(server, false);
}

static ant_value_t server_headers_from_parsed(ant_t *js, const ant_http1_parsed_request_t *parsed) {
  ant_value_t headers = headers_create_empty(js);
  const ant_http_header_t *hdr = NULL;

  if (is_err(headers)) return headers;
  for (hdr = parsed->headers; hdr; hdr = hdr->next) {
    ant_value_t step = headers_append_literal(js, headers, hdr->name, hdr->value);
    if (is_err(step)) return step;
  }
  return headers;
}

static ant_value_t server_call_fetch(server_runtime_t *server, ant_value_t request_obj) {
  ant_t *js = server->js;
  ant_value_t args[2] = { request_obj, server->server_ctx };
  ant_value_t saved_this = js->this_val;
  ant_value_t result = js_mkundef();

  js->this_val = server->export_obj;
  if (vtype(server->fetch_fn) == T_CFUNC) result = js_as_cfunc(server->fetch_fn)(js, args, 2);
  else result = sv_vm_call(js->vm, js, server->fetch_fn, server->export_obj, args, 2, NULL, false);
  js->this_val = saved_this;
  
  return result;
}

static ant_value_t server_abort_signal_task(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t payload = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t signal = 0; ant_value_t reason = 0;
  if (vtype(payload) != T_OBJ) return js_mkundef();

  signal = js_get(js, payload, "signal");
  reason = js_get(js, payload, "reason");

  if (abort_signal_is_signal(signal) && !abort_signal_is_aborted(signal))
    signal_do_abort(js, signal, reason);

  return js_mkundef();
}

static void server_queue_abort_signal(ant_t *js, ant_value_t signal, ant_value_t reason) {
  ant_value_t callback = 0;
  ant_value_t payload = 0;

  if (!abort_signal_is_signal(signal) || abort_signal_is_aborted(signal)) return;
  payload = js_mkobj(js);
  if (is_err(payload)) return;

  js_set(js, payload, "signal", signal);
  js_set(js, payload, "reason", reason);
  callback = js_heavy_mkfun(js, server_abort_signal_task, payload);
  
  queue_microtask(js, callback);
}

static void server_abort_request(server_request_t *req, const char *message) {
  ant_t *js = NULL;
  
  ant_value_t signal = 0; ant_value_t reason = 0; ant_value_t abort_reason = 0;
  if (!req || !req->server || !is_object_type(req->request_obj)) return;

  js = req->server->js;
  abort_reason = js_get_slot(req->request_obj, SLOT_REQUEST_ABORT_REASON);
  if (vtype(abort_reason) != T_UNDEF) return;

  reason = make_dom_exception(js, 
    message ? message : "The request was aborted", "AbortError"
  );
  
  js_set_slot_wb(js, req->request_obj, SLOT_REQUEST_ABORT_REASON, reason);
  signal = js_get_slot(req->request_obj, SLOT_REQUEST_SIGNAL);
  
  if (!abort_signal_is_signal(signal) || abort_signal_is_aborted(signal)) return;
  server_queue_abort_signal(js, signal, reason);
}

static bool server_response_chunk(server_request_t *req, ant_value_t value, const uint8_t **out, size_t *len) {
  TypedArrayData *ta = buffer_get_typedarray_data(value);

  if (!ta || ta->type != TYPED_ARRAY_UINT8) return false;
  if (!ta->buffer || ta->buffer->is_detached) {
    *out = NULL;
    *len = 0;
    return true;
  }

  *out = ta->buffer->data + ta->byte_offset;
  *len = ta->byte_length;
  return true;
}

static void server_start_stream_read(server_request_t *req);
static void server_on_read(ant_conn_t *conn, ssize_t nread, void *user_data);
static void server_write_cb(ant_conn_t *conn, int status, void *user_data);

static void server_on_deferred_drain(uv_timer_t *handle) {
  server_conn_state_t *cs = handle ? (server_conn_state_t *)handle->data : NULL;

  if (!cs) return;
  cs->drain_scheduled = false;

  if (!cs->conn || cs->active_req) return;
  if (ant_conn_is_closing(cs->conn)) return;
  if (ant_conn_buffer_len(cs->conn) == 0) return;

  server_on_read(cs->conn, (ssize_t)ant_conn_buffer_len(cs->conn), cs->server);
}

static void server_on_drain_timer_close(uv_handle_t *handle) {
  server_conn_state_t *cs = handle ? (server_conn_state_t *)handle->data : NULL;

  if (!cs) return;
  cs->drain_timer_closed = true;
  cs->drain_scheduled = false;
  server_conn_state_maybe_free(cs);
}

static void server_schedule_drain(server_conn_state_t *cs) {
  if (!cs || !cs->conn) return;
  if (cs->drain_scheduled) return;

  cs->drain_scheduled = true;
  if (uv_timer_start(&cs->drain_timer, server_on_deferred_drain, 0, 0) != 0)
    cs->drain_scheduled = false;
}

static bool server_queue_write(ant_conn_t *conn, server_request_t *req, char *data, size_t len, server_write_action_t action) {
  server_write_req_t *wr = calloc(1, sizeof(*wr));
  int rc = 0;

  if (!conn || ant_conn_is_closing(conn)) {
    free(data);
    return false;
  }

  if (!wr) {
    free(data);
    return false;
  }

  wr->request = req;
  wr->conn = conn;
  wr->action = action;
  if (req) server_request_retain(req);

  rc = ant_conn_write(conn, data, len, server_write_cb, wr);
  if (rc != 0) {
    if (req) server_request_release(req);
    free(wr);
    ant_conn_close(conn);
    return false;
  }

  return true;
}

static void server_send_basic_response(
  ant_conn_t *conn, int status, const char *status_text, 
  const char *content_type, const uint8_t *body, size_t body_len
) {
  ant_http1_buffer_t buf;
  char *out = NULL;
  size_t out_len = 0;

  ant_http1_buffer_init(&buf);
  if (!ant_http1_write_basic_response(&buf, status, status_text, content_type, body, body_len, false)) {
    ant_http1_buffer_free(&buf);
    ant_conn_close(conn);
    return;
  }

  out = ant_http1_buffer_take(&buf, &out_len);
  server_queue_write(conn, NULL, out, out_len, SERVER_WRITE_CLOSE_CLIENT);
}

static inline void server_send_text_response(
  ant_conn_t *conn,
  int status,
  const char *status_text,
  const char *body
) {
  if (!body) body = "";
  server_send_basic_response(
    conn, status,
    status_text,
    "text/plain;charset=UTF-8",
    (const uint8_t *)body,
    strlen(body)
  );
}

static inline void server_send_internal_error(ant_conn_t *conn, const char *body) {
  server_send_text_response(
    conn, 500,
    "Internal Server Error",
    body ? body : "Internal Server Error"
  );
}

static void server_finish_with_response(server_request_t *req, ant_value_t response_obj) {
  response_data_t *resp = response_get_data(response_obj);
  ant_value_t headers = response_get_headers(response_obj);
  
  ant_value_t stream = js_get_slot(response_obj, SLOT_RESPONSE_BODY_STREAM);
  bool body_is_stream = resp && resp->body_is_stream && rs_is_stream(stream);
  bool head_only = false;
  
  const char *status_text = NULL;
  ant_http1_buffer_t buf;
  
  char *out = NULL;
  size_t out_len = 0;

  if (!req->conn || ant_conn_is_closing(req->conn)) return;
  if (!resp) {
    server_send_internal_error(req->conn, "Invalid Response");
    return;
  }

  req->response_obj = response_obj;
  req->response_started = true;
  head_only = strcasecmp(request_get_data(req->request_obj)->method, "HEAD") == 0;
  status_text = (resp->status_text && resp->status_text[0]) ? resp->status_text : ant_http1_default_status_text(resp->status);

  ant_http1_buffer_init(&buf);
  if (!ant_http1_write_response_head(&buf, resp->status, status_text, headers, body_is_stream, resp->body_size, req->keep_alive)) {
    ant_http1_buffer_free(&buf);
    ant_conn_close(req->conn);
    return;
  }
  if (!body_is_stream && !head_only && resp->body_size > 0) ant_http1_buffer_append(&buf, resp->body_data, resp->body_size);
  if (buf.failed) {
    ant_http1_buffer_free(&buf);
    ant_conn_close(req->conn);
    return;
  }

  out = ant_http1_buffer_take(&buf, &out_len);
  ant_conn_set_timeout_ms(req->conn, req->server->idle_timeout_ms);
  
  if (body_is_stream) {
    if (!server_queue_write(req->conn, req, out, out_len, SERVER_WRITE_STREAM_READ)) return;
    if (head_only) ant_conn_close(req->conn);
    return;
  }

  server_queue_write(
    req->conn,
    req, out, out_len,
    req->keep_alive ? SERVER_WRITE_KEEP_ALIVE : SERVER_WRITE_CLOSE_CLIENT
  );
}

static ant_value_t server_on_response_reject(ant_t *js, ant_value_t *args, int nargs) {
  server_request_t *req = server_current_request(js);
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  const char *msg = NULL;

  if (!req) return js_mkundef();
  req->response_promise = js_mkundef();

  if (req->conn && !ant_conn_is_closing(req->conn)) {
    msg = js_str(js, reason);
    server_send_internal_error(req->conn, msg);
  }

  server_request_release(req);
  return js_mkundef();
}

static ant_value_t server_on_response_fulfill(ant_t *js, ant_value_t *args, int nargs) {
  server_request_t *req = server_current_request(js);
  ant_value_t value = (nargs > 0) ? args[0] : js_mkundef();

  if (!req) return js_mkundef();
  req->response_promise = js_mkundef();

  if (!response_get_data(value)) {
    if (req->conn && !ant_conn_is_closing(req->conn))
      server_send_internal_error(req->conn, "fetch handler must return a Response");
  } else if (req->conn && !ant_conn_is_closing(req->conn)) {
    server_finish_with_response(req, value);
  }

  server_request_release(req);
  return js_mkundef();
}

static void server_handle_fetch_result(server_request_t *req, ant_value_t result) {
  ant_t *js = req->server->js;

  if (is_err(result)) {
    ant_value_t reason = server_exception_reason(js, result);
    const char *msg = js_str(js, reason);
    server_send_internal_error(req->conn, msg);
    return;
  }

  if (vtype(result) == T_PROMISE) {
    ant_value_t fulfill = server_mkreqfun(js, server_on_response_fulfill, req);
    ant_value_t reject = server_mkreqfun(js, server_on_response_reject, req);
    ant_value_t then_result = 0;

    req->response_promise = result;
    server_request_retain(req);
    then_result = js_promise_then(js, result, fulfill, reject);
    promise_mark_handled(then_result);
    return;
  }

  if (!response_get_data(result)) {
    server_send_internal_error(req->conn, "fetch handler must return a Response");
    return;
  }

  server_finish_with_response(req, result);
}

static ant_value_t server_stream_read_reject(ant_t *js, ant_value_t *args, int nargs) {
  server_request_t *req = server_current_request(js);
  if (!req) return js_mkundef();
  
  req->response_read_promise = js_mkundef();
  if (req->conn && !ant_conn_is_closing(req->conn)) ant_conn_close(req->conn);
  server_request_release(req);
  
  return js_mkundef();
}

static ant_value_t server_stream_read_fulfill(ant_t *js, ant_value_t *args, int nargs) {
  server_request_t *req = server_current_request(js);
  ant_value_t result = (nargs > 0) ? args[0] : js_mkundef();
  
  ant_value_t done = 0;
  ant_value_t value = 0;
  
  const uint8_t *chunk = NULL;
  size_t chunk_len = 0;
  
  ant_http1_buffer_t buf;
  char *out = NULL;
  size_t out_len = 0;

  if (!req) return js_mkundef();
  req->response_read_promise = js_mkundef();

  if (!req->conn || ant_conn_is_closing(req->conn)) {
    server_request_release(req);
    return js_mkundef();
  }

  done = js_get(js, result, "done");
  if (done == js_true) {
    ant_http1_buffer_init(&buf);
    if (!ant_http1_write_final_chunk(&buf)) {
      ant_http1_buffer_free(&buf);
      ant_conn_close(req->conn);
      server_request_release(req);
      return js_mkundef();
    }
    
    out = ant_http1_buffer_take(&buf, &out_len);
    ant_conn_set_timeout_ms(req->conn, req->server->idle_timeout_ms);
    
    server_queue_write(
      req->conn,
      req, out, out_len,
      req->keep_alive ? SERVER_WRITE_KEEP_ALIVE : SERVER_WRITE_CLOSE_CLIENT
    );
    
    server_request_release(req);
    return js_mkundef();
  }

  value = js_get(js, result, "value");
  if (!server_response_chunk(req, value, &chunk, &chunk_len)) {
    ant_conn_close(req->conn);
    server_request_release(req);
    return js_mkundef();
  }

  ant_http1_buffer_init(&buf);
  if (!ant_http1_write_chunk(&buf, chunk, chunk_len)) {
    ant_http1_buffer_free(&buf);
    ant_conn_close(req->conn);
    server_request_release(req);
    return js_mkundef();
  }

  out = ant_http1_buffer_take(&buf, &out_len);
  ant_conn_set_timeout_ms(req->conn, req->server->idle_timeout_ms);
  
  server_queue_write(req->conn, req, out, out_len, SERVER_WRITE_STREAM_READ);
  server_request_release(req);
  
  return js_mkundef();
}

static void server_start_stream_read(server_request_t *req) {
  ant_t *js = req->server->js;
  
  ant_value_t next_p = 0;
  ant_value_t fulfill = 0;
  ant_value_t reject = 0;
  ant_value_t then_result = 0;

  if (!req->conn || ant_conn_is_closing(req->conn)) return;
  if (!is_object_type(req->response_reader)) return;

  next_p = rs_default_reader_read(js, req->response_reader);
  req->response_read_promise = next_p;
  fulfill = server_mkreqfun(js, server_stream_read_fulfill, req);
  reject = server_mkreqfun(js, server_stream_read_reject, req);

  server_request_retain(req);
  then_result = js_promise_then(js, next_p, fulfill, reject);
  promise_mark_handled(then_result);
}

static bool server_request_ensure_reader(server_request_t *req) {
  ant_t *js;
  ant_value_t reader_args[1];
  ant_value_t saved;

  if (!req || !is_object_type(req->response_obj)) return false;
  if (vtype(req->response_reader) != T_UNDEF) return true;

  js = req->server->js;
  reader_args[0] = js_get_slot(req->response_obj, SLOT_RESPONSE_BODY_STREAM);

  saved = js->new_target;
  js->new_target = g_reader_proto;
  req->response_reader = js_rs_reader_ctor(js, reader_args, 1);
  js->new_target = saved;

  return !is_err(req->response_reader);
}


static void server_write_cb(ant_conn_t *conn, int status, void *user_data) {
  server_write_req_t *wr = (server_write_req_t *)user_data;
  server_request_t *req = wr->request;
  server_conn_state_t *cs = conn ? (server_conn_state_t *)ant_conn_get_user_data(conn) : NULL;

  if (status < 0 && conn && !ant_conn_is_closing(conn))
    ant_conn_close(conn);

  if (status == 0 && conn && !ant_conn_is_closing(conn)) {
  switch (wr->action) {
  case SERVER_WRITE_STREAM_READ:
    if (!server_request_ensure_reader(req)) {
      ant_conn_close(conn);
      break;
    }
    server_start_stream_read(req);
    break;

  case SERVER_WRITE_KEEP_ALIVE:
    if (cs && req) {
      ant_conn_consume(conn, req->consumed_len);
      ant_http1_conn_parser_reset(&cs->parser);
      cs->active_req = NULL;
      ant_conn_set_timeout_ms(conn, cs->server->idle_timeout_ms);
      ant_conn_resume_read(conn);
      server_request_release(req);
      if (ant_conn_buffer_len(conn) > 0) server_schedule_drain(cs);
    }
    break;
    
  case SERVER_WRITE_CLOSE_CLIENT:
    ant_conn_close(conn);
    break;
    
  default: break;
  }}

  if (req) server_request_release(req);
  free(wr);
}

static ant_value_t server_request_ip(ant_t *js, ant_value_t *args, int nargs) {
  server_runtime_t *server = server_current_runtime(js);
  server_request_t *req = NULL;
  ant_value_t out = 0;

  if (!server || nargs < 1) return js_mknull();
  req = server_find_request(server, args[0]);
  if (!req || !req->conn || !ant_conn_has_remote_addr(req->conn)) return js_mknull();

  out = js_mkobj(js);
  js_set(js, out, "address", js_mkstr(js, ant_conn_remote_addr(req->conn), strlen(ant_conn_remote_addr(req->conn))));
  js_set(js, out, "port", js_mknum(ant_conn_remote_port(req->conn)));
  return out;
}

static ant_value_t server_timeout(ant_t *js, ant_value_t *args, int nargs) {
  server_runtime_t *server = server_current_runtime(js);
  server_request_t *req = NULL;
  int timeout = 0;

  if (!server || nargs < 2) return js_mkundef();
  req = server_find_request(server, args[0]);
  if (!req || !req->conn) return js_mkundef();

  timeout = (int)js_getnum(args[1]);
  ant_conn_set_timeout_ms(req->conn, (uint64_t)timeout * 1000ULL);
  
  return js_mkundef();
}

static ant_value_t server_stop(ant_t *js, ant_value_t *args, int nargs) {
  server_runtime_t *server = server_current_runtime(js);
  stop_waiter_t *waiter = NULL;
  ant_value_t promise = js_mkpromise(js);
  bool force = (nargs > 0 && js_truthy(js, args[0]));

  if (!server) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  waiter = calloc(1, sizeof(*waiter));
  if (!waiter) {
    js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
    return promise;
  }

  waiter->promise = promise;
  waiter->next = server->stop_waiters;
  server->stop_waiters = waiter;
  server_begin_stop(server, force);
  return promise;
}

static void server_process_client_request(
  ant_conn_t *conn,
  ant_http1_parsed_request_t *parsed,
  size_t consumed_len
) {
  server_conn_state_t *cs = (server_conn_state_t *)ant_conn_get_user_data(conn);
  server_runtime_t *server = cs ? cs->server : NULL;
  server_request_t *req = NULL;
  
  ant_t *js = NULL;
  ant_value_t headers = 0;
  ant_value_t request_obj = 0;
  ant_value_t result = 0;
  bool keep_alive = false;

  if (!server || !cs) {
    ant_http1_free_parsed_request(parsed);
    ant_conn_close(conn);
    return;
  }

  js = server->js;
  req = &cs->request;
  keep_alive = parsed->keep_alive;
  headers = server_headers_from_parsed(js, parsed);
  
  if (is_err(headers)) {
    ant_http1_free_parsed_request(parsed);
    server_send_internal_error(conn, NULL);
    return;
  }

  request_obj = request_create_server(
    js,
    parsed->method,
    parsed->target,
    parsed->absolute_target,
    parsed->host,
    server->hostname,
    server->port,
    headers,
    parsed->body,
    parsed->body_len,
    parsed->content_type
  );
  ant_http1_free_parsed_request(parsed);

  if (is_err(request_obj)) {
    server_send_internal_error(conn, NULL);
    return;
  }

  server_request_reset(req);
  req->server = server;
  req->conn_state = cs;
  req->conn = conn;
  req->request_obj = request_obj;
  req->consumed_len = consumed_len;
  req->keep_alive = keep_alive;
  req->refs = 1;
  req->next = server->requests;
  server->requests = req;
  cs->active_req = req;
  ant_conn_pause_read(conn);
  ant_conn_set_timeout_ms(conn, server->request_timeout_ms);

  result = server_call_fetch(server, request_obj);
  server_handle_fetch_result(req, result);
}

static void server_on_read(ant_conn_t *conn, ssize_t nread, void *user_data) {
  server_conn_state_t *cs = (server_conn_state_t *)ant_conn_get_user_data(conn);
  ant_http1_parsed_request_t parsed = {0};
  ant_http1_parse_result_t parse_result = ANT_HTTP1_PARSE_INCOMPLETE;
  size_t consumed = 0;

  if (!conn || !cs) return;
  if (cs->active_req) return;
  if (ant_conn_buffer_len(conn) == 0) return;

  ant_conn_set_timeout_ms(conn, cs->server->request_timeout_ms);
  parse_result = ant_http1_conn_parser_execute(
    &cs->parser,
    ant_conn_buffer(conn),
    ant_conn_buffer_len(conn),
    &parsed,
    &consumed
  );
  
  if (parse_result == ANT_HTTP1_PARSE_ERROR) {
    ant_http1_free_parsed_request(&parsed);
    server_send_text_response(conn, 400, "Bad Request", "Bad Request");
    return;
  }

  if (parse_result != ANT_HTTP1_PARSE_OK) return;
  server_process_client_request(conn, &parsed, consumed);
}

static void server_on_end(ant_conn_t *conn, void *user_data) {
  if (conn) ant_conn_close(conn);
}

static void server_on_conn_close(ant_conn_t *conn, void *user_data) {
  server_runtime_t *server = (server_runtime_t *)user_data;
  server_conn_state_t *cs = (server_conn_state_t *)ant_conn_get_user_data(conn);

  if (cs) {
    ant_conn_set_user_data(conn, NULL);
    cs->conn = NULL;
    ant_http1_conn_parser_free(&cs->parser);
    if (!uv_is_closing((uv_handle_t *)&cs->drain_timer))
      uv_close((uv_handle_t *)&cs->drain_timer, server_on_drain_timer_close);
    if (cs->active_req) {
      server_abort_request(cs->active_req, "Client disconnected");
      cs->active_req->conn = NULL;
      cs->active_req = NULL;
      server_request_release(&cs->request);
      cs = NULL;
    }
    if (cs) server_conn_state_maybe_free(cs);
  }

  server_maybe_finish_stop(server);
}

static void server_on_listener_close(ant_listener_t *listener, void *user_data) {
  server_maybe_finish_stop((server_runtime_t *)user_data);
}

static void server_on_accept(ant_listener_t *listener, ant_conn_t *conn, void *user_data) {
  server_runtime_t *server = (server_runtime_t *)user_data;
  server_conn_state_t *cs = NULL;

  (void)listener;
  if (!conn || !server) return;

  cs = calloc(1, sizeof(*cs));
  if (!cs) {
    ant_conn_close(conn);
    return;
  }

  cs->server = server;
  cs->conn = conn;
  cs->drain_timer_closed = true;
  cs->request.server = server;
  cs->request.conn_state = cs;
  server_request_reset(&cs->request);
  
  if (uv_timer_init(server->loop, &cs->drain_timer) != 0) {
    free(cs);
    ant_conn_close(conn);
    return;
  }
  
  cs->drain_timer.data = cs;
  cs->drain_timer_closed = false;
  ant_http1_conn_parser_init(&cs->parser);
  ant_conn_set_user_data(conn, cs);
  ant_conn_set_no_delay(conn, true);
}

static bool server_export_has_fetch_handler(ant_t *js, ant_value_t default_export, bool *looks_like_config) {
  ant_value_t fetch = 0;
  
  if (looks_like_config) *looks_like_config = false;
  if (!is_object_type(default_export)) return false;

  fetch = js_get(js, default_export, "fetch");
  if (is_callable(fetch)) return true;

  if (looks_like_config) {
    ant_value_t port = js_get(js, default_export, "port");
    ant_value_t hostname = js_get(js, default_export, "hostname");
    ant_value_t idle_timeout = js_get(js, default_export, "idleTimeout");
    ant_value_t request_timeout = js_get(js, default_export, "requestTimeout");
    ant_value_t unix_path = js_get(js, default_export, "unix");
    ant_value_t tls = js_get(js, default_export, "tls");

    *looks_like_config =
      vtype(fetch)           != T_UNDEF ||
      vtype(port)            != T_UNDEF ||
      vtype(hostname)        != T_UNDEF ||
      vtype(idle_timeout)    != T_UNDEF ||
      vtype(request_timeout) != T_UNDEF ||
      vtype(unix_path)       != T_UNDEF ||
      vtype(tls)             != T_UNDEF;
  }

  return false;
}

int server_maybe_start_from_export(ant_t *js, ant_value_t default_export) {
  bool looks_like_server = false;
  ant_value_t server_result = 0;
  const char *error = NULL;

  if (!server_export_has_fetch_handler(js, default_export, &looks_like_server)) {
    if (!looks_like_server) return EXIT_SUCCESS;
    error = "Module does not export a fetch handler";
    goto fail;
  }

  server_result = server_start_from_export(js, default_export);
  if (is_err(server_result)) {
    error = js_str(js, server_result);
    goto fail;
  }

  return EXIT_SUCCESS;

fail:
  fprintf(stderr, "%s\n", error);
  return EXIT_FAILURE;
}

ant_value_t server_start_from_export(ant_t *js, ant_value_t default_export) {
  server_runtime_t *server = NULL;
  
  ant_value_t port_v = 0;
  ant_value_t hostname_v = 0;
  ant_value_t idle_timeout_v = 0;
  ant_value_t request_timeout_v = 0;
  ant_value_t unix_v = 0;
  ant_value_t tls_v = 0;
  
  ant_listener_callbacks_t callbacks = {0};
  int rc = 0;

  if (g_server) return js_mkerr(js, "server is already running");
  if (!is_object_type(default_export)) return js_mkerr_typed(js, JS_ERR_TYPE, "Module does not export a fetch handler");

  server = malloc(sizeof(*server));
  if (!server) return js_mkerr(js, "out of memory");

  *server = (server_runtime_t){
    .js = js,
    .export_obj = default_export,
    .fetch_fn = js_get(js, default_export, "fetch"),
    .hostname = strdup("0.0.0.0"),
    .unix_path = NULL,
    .port = 3000,
    .request_timeout_ms = 30000,
    .idle_timeout_ms = 30000,
    .loop = uv_default_loop(),
  };

  if (!server->hostname) {
    free(server);
    return js_mkerr(js, "out of memory");
  }

  unix_v = js_get(js, default_export, "unix");
  tls_v = js_get(js, default_export, "tls");
  
  if (vtype(unix_v) != T_UNDEF && vtype(unix_v) != T_NULL) {
    if (vtype(unix_v) != T_STR) {
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_TYPE, "server unix must be a string");
    }

    server->unix_path = strdup(js_getstr(js, unix_v, NULL));
    if (!server->unix_path) {
      free(server->hostname);
      free(server);
      return js_mkerr(js, "out of memory");
    }
  }
  
  if (vtype(tls_v) != T_UNDEF && vtype(tls_v) != T_NULL) {
    free(server->unix_path);
    free(server->hostname);
    free(server);
    return js_mkerr_typed(js, JS_ERR_TYPE, "tls server config is not implemented yet");
  }

  port_v = js_get(js, default_export, "port");
  hostname_v = js_get(js, default_export, "hostname");
  idle_timeout_v = js_get(js, default_export, "idleTimeout");
  request_timeout_v = js_get(js, default_export, "requestTimeout");

  if (vtype(port_v) != T_UNDEF && vtype(port_v) != T_NULL) {
    if (vtype(port_v) != T_NUM) {
      free(server->unix_path);
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_TYPE, "server port must be a number");
    }
    server->port = (int)js_getnum(port_v);
    if (server->port < 0 || server->port > 65535) {
      free(server->unix_path);
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_RANGE, "server port must be between 0 and 65535");
    }
  }

  if (vtype(hostname_v) != T_UNDEF && vtype(hostname_v) != T_NULL) {
    char *next_hostname = NULL;
    if (vtype(hostname_v) != T_STR) {
      free(server->unix_path);
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_TYPE, "server hostname must be a string");
    }
    next_hostname = strdup(js_getstr(js, hostname_v, NULL));
    if (!next_hostname) {
      free(server->unix_path);
      free(server->hostname);
      free(server);
      return js_mkerr(js, "out of memory");
    }
    free(server->hostname);
    server->hostname = next_hostname;
  }

  if (vtype(idle_timeout_v) != T_UNDEF && vtype(idle_timeout_v) != T_NULL) {
    double timeout = 0;
    if (vtype(idle_timeout_v) != T_NUM) {
      free(server->unix_path);
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_TYPE, "server idleTimeout must be a number");
    }
    timeout = js_getnum(idle_timeout_v);
    if (timeout < 0) {
      free(server->unix_path);
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_RANGE, "server idleTimeout must be >= 0");
    }
    server->idle_timeout_ms = (uint64_t)(timeout * 1000.0);
  }

  if (vtype(request_timeout_v) != T_UNDEF && vtype(request_timeout_v) != T_NULL) {
    double timeout = 0;
    if (vtype(request_timeout_v) != T_NUM) {
      free(server->unix_path);
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_TYPE, "server requestTimeout must be a number");
    }
    
    timeout = js_getnum(request_timeout_v);
    if (timeout < 0) {
      free(server->unix_path);
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_RANGE, "server requestTimeout must be >= 0");
    }
    
    server->request_timeout_ms = (uint64_t)(timeout * 1000.0);
  }

  uv_signal_init(server->loop, &server->sigint_handle);
  uv_signal_init(server->loop, &server->sigterm_handle);
  server->sigint_handle.data = server;
  server->sigterm_handle.data = server;

  callbacks.on_accept = server_on_accept;
  callbacks.on_read = server_on_read;
  callbacks.on_end = server_on_end;
  callbacks.on_conn_close = server_on_conn_close;
  callbacks.on_listener_close = server_on_listener_close;

  if (server->unix_path) {
    rc = ant_listener_listen_pipe(
      &server->listener, server->loop,
      server->unix_path,
      128, server->idle_timeout_ms, &callbacks, server
    );
  } else {
    rc = ant_listener_listen_tcp(
      &server->listener, server->loop,
      server->hostname, server->port,
      128, server->idle_timeout_ms, &callbacks, server
    );
  }
  
  if (rc != 0) {
    free(server->unix_path);
    free(server->hostname);
    free(server);
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
  }

  server->port = ant_listener_port(&server->listener);
  uv_signal_start(&server->sigint_handle, server_signal_cb, SIGINT);
  uv_signal_start(&server->sigterm_handle, server_signal_cb, SIGTERM);

  server->server_ctx = js_mkobj(js);
  js_set(js, server->server_ctx, "requestIP", server_mkruntimefun(js, server_request_ip, server));
  js_set(js, server->server_ctx, "timeout", server_mkruntimefun(js, server_timeout, server));
  js_set(js, server->server_ctx, "stop", server_mkruntimefun(js, server_stop, server));

  g_server = server;
  return js_mkundef();
}

void gc_mark_server(ant_t *js, gc_mark_fn mark) {
  server_request_t *req = NULL;
  stop_waiter_t *waiter = NULL;

  if (!g_server) return;
  mark(js, g_server->export_obj);
  mark(js, g_server->fetch_fn);
  mark(js, g_server->server_ctx);

  for (req = g_server->requests; req; req = req->next) {
    mark(js, req->request_obj);
    mark(js, req->response_obj);
    mark(js, req->response_promise);
    mark(js, req->response_reader);
    mark(js, req->response_read_promise);
  }

  for (waiter = g_server->stop_waiters; waiter; waiter = waiter->next)
    mark(js, waiter->promise);
}
