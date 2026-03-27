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
#include "modules/buffer.h"
#include "modules/headers.h"
#include "modules/request.h"
#include "modules/response.h"
#include "modules/server.h"

typedef struct server_runtime_s server_runtime_t;
typedef struct server_request_s server_request_t;

static server_runtime_t *g_server = NULL;

typedef struct stop_waiter_s {
  ant_value_t promise;
  struct stop_waiter_s *next;
} stop_waiter_t;

typedef enum {
  SERVER_WRITE_NONE = 0,
  SERVER_WRITE_CLOSE_CLIENT,
  SERVER_WRITE_STREAM_READ,
} server_write_action_t;

typedef struct {
  server_request_t *request;
  ant_conn_t *conn;
  server_write_action_t action;
} server_write_req_t;

struct server_request_s {
  server_runtime_t *server;
  ant_conn_t *conn;
  ant_value_t request_obj;
  ant_value_t response_obj;
  ant_value_t response_promise;
  ant_value_t response_reader;
  ant_value_t response_read_promise;
  int refs;
  bool response_started;
  struct server_request_s *next;
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
  int port;
  int idle_timeout_secs;
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


static void server_request_release(server_request_t *req) {
  if (!req) return;
  if (--req->refs > 0) return;

  server_remove_request(req->server, req);
  free(req);
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

static char *server_build_request_url(server_runtime_t *server, const ant_http1_parsed_request_t *req) {
  ant_http1_buffer_t url;
  ant_http1_buffer_init(&url);

  if (req->absolute_target) return strdup(req->target);
  if (!ant_http1_buffer_append_cstr(&url, "http://")) goto oom;
  if (req->host && req->host[0]) {
    if (!ant_http1_buffer_append_cstr(&url, req->host)) goto oom;
  } else {
    if (!ant_http1_buffer_append_cstr(&url, server->hostname)) goto oom;
    if (server->port != 80 && server->port > 0) {
      if (!ant_http1_buffer_appendf(&url, ":%d", server->port)) goto oom;
    }
  }
  
  if (!ant_http1_buffer_append_cstr(&url, req->target)) goto oom;
  return ant_http1_buffer_take(&url, NULL);

oom:
  ant_http1_buffer_free(&url);
  return NULL;
}

static ant_value_t server_headers_from_parsed(ant_t *js, const ant_http1_parsed_request_t *parsed) {
  ant_value_t headers = headers_create_empty(js);
  const ant_http_header_t *hdr = NULL;

  if (is_err(headers)) return headers;
  for (hdr = parsed->headers; hdr; hdr = hdr->next) {
    ant_value_t step = headers_append_value(
      js,
      headers,
      js_mkstr(js, hdr->name, strlen(hdr->name)),
      js_mkstr(js, hdr->value, strlen(hdr->value))
    );
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
  if (vtype(server->fetch_fn) == T_CFUNC)
    result = ((ant_value_t (*)(ant_t *, ant_value_t *, int))vdata(server->fetch_fn))(js, args, 2);
  else result = sv_vm_call(js->vm, js, server->fetch_fn, server->export_obj, args, 2, NULL, false);
  js->this_val = saved_this;
  return result;
}

static bool server_response_chunk(server_request_t *req, ant_value_t value, const uint8_t **out, size_t *len) {
  ant_value_t slot = js_get_slot(value, SLOT_BUFFER);
  TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(slot);

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
static void server_write_cb(ant_conn_t *conn, int status, void *user_data);

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
  if (!ant_http1_write_response_head(&buf, resp->status, status_text, headers, body_is_stream, resp->body_size, false)) {
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
  if (!server_queue_write(req->conn, req, out, out_len, body_is_stream ? SERVER_WRITE_STREAM_READ : SERVER_WRITE_CLOSE_CLIENT))
    return;

  if (body_is_stream && head_only) ant_conn_close(req->conn);
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
    server_queue_write(req->conn, req, out, out_len, SERVER_WRITE_CLOSE_CLIENT);
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

static void server_process_client_request(ant_conn_t *conn, ant_http1_parsed_request_t *parsed) {
  server_runtime_t *server = (server_runtime_t *)ant_listener_get_user_data(ant_conn_listener(conn));
  
  ant_t *js = server->js;
  ant_value_t headers = 0;
  ant_value_t request_obj = 0;
  ant_value_t result = 0;
  
  char *url = NULL;
  server_request_t *req = NULL;

  url = server_build_request_url(server, parsed);
  if (!url) {
    ant_http1_free_parsed_request(parsed);
    server_send_internal_error(conn, NULL);
    return;
  }

  headers = server_headers_from_parsed(js, parsed);
  if (is_err(headers)) {
    free(url);
    ant_http1_free_parsed_request(parsed);
    server_send_internal_error(conn, NULL);
    return;
  }

  request_obj = request_create(js, parsed->method, url, headers, parsed->body, parsed->body_len, parsed->content_type);
  free(url);
  ant_http1_free_parsed_request(parsed);

  if (is_err(request_obj)) {
    server_send_internal_error(conn, NULL);
    return;
  }

  req = calloc(1, sizeof(*req));
  if (!req) {
    server_send_internal_error(conn, NULL);
    return;
  }

  req->server = server;
  req->conn = conn;
  req->request_obj = request_obj;
  req->response_obj = js_mkundef();
  req->response_promise = js_mkundef();
  req->response_reader = js_mkundef();
  req->response_read_promise = js_mkundef();
  req->refs = 1;
  req->next = server->requests;
  server->requests = req;
  ant_conn_set_user_data(conn, req);

  result = server_call_fetch(server, request_obj);
  server_handle_fetch_result(req, result);
}

static void server_on_read(ant_conn_t *conn, ssize_t nread, void *user_data) {
  ant_http1_parsed_request_t parsed = {0};
  ant_http1_parse_result_t parse_result = ANT_HTTP1_PARSE_INCOMPLETE;
  if (!conn) return;

  parse_result = ant_http1_parse_request(ant_conn_buffer(conn), ant_conn_buffer_len(conn), &parsed, NULL, NULL);
  if (parse_result == ANT_HTTP1_PARSE_ERROR) {
    ant_http1_free_parsed_request(&parsed);
    server_send_text_response(conn, 400, "Bad Request", "Bad Request");
    return;
  }

  if (parse_result == ANT_HTTP1_PARSE_OK) {
    ant_conn_pause_read(conn);
    server_process_client_request(conn, &parsed);
  }
}

static void server_on_end(ant_conn_t *conn, void *user_data) {
  if (conn) ant_conn_close(conn);
}

static void server_on_conn_close(ant_conn_t *conn, void *user_data) {
  server_runtime_t *server = (server_runtime_t *)user_data;
  server_request_t *req = (server_request_t *)ant_conn_get_user_data(conn);

  if (req) {
    req->conn = NULL;
    ant_conn_set_user_data(conn, NULL);
    server_request_release(req);
  }

  server_maybe_finish_stop(server);
}

static void server_on_listener_close(ant_listener_t *listener, void *user_data) {
  server_maybe_finish_stop((server_runtime_t *)user_data);
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
    ant_value_t unix_path = js_get(js, default_export, "unix");
    ant_value_t tls = js_get(js, default_export, "tls");

    *looks_like_config =
      vtype(fetch)        != T_UNDEF ||
      vtype(port)         != T_UNDEF ||
      vtype(hostname)     != T_UNDEF ||
      vtype(idle_timeout) != T_UNDEF ||
      vtype(unix_path)    != T_UNDEF ||
      vtype(tls)          != T_UNDEF;
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
  ant_value_t timeout_v = 0;
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
    .port = 3000,
    .idle_timeout_secs = 10,
    .loop = uv_default_loop(),
  };

  if (!server->hostname) {
    free(server);
    return js_mkerr(js, "out of memory");
  }

  unix_v = js_get(js, default_export, "unix");
  tls_v = js_get(js, default_export, "tls");
  if (vtype(unix_v) != T_UNDEF && vtype(unix_v) != T_NULL) {
    free(server->hostname);
    free(server);
    return js_mkerr_typed(js, JS_ERR_TYPE, "unix sockets are not implemented yet");
  }
  
  if (vtype(tls_v) != T_UNDEF && vtype(tls_v) != T_NULL) {
    free(server->hostname);
    free(server);
    return js_mkerr_typed(js, JS_ERR_TYPE, "tls server config is not implemented yet");
  }

  port_v = js_get(js, default_export, "port");
  hostname_v = js_get(js, default_export, "hostname");
  timeout_v = js_get(js, default_export, "idleTimeout");

  if (vtype(port_v) != T_UNDEF && vtype(port_v) != T_NULL) {
    if (vtype(port_v) != T_NUM) {
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_TYPE, "server port must be a number");
    }
    server->port = (int)js_getnum(port_v);
    if (server->port < 0 || server->port > 65535) {
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_RANGE, "server port must be between 0 and 65535");
    }
  }

  if (vtype(hostname_v) != T_UNDEF && vtype(hostname_v) != T_NULL) {
    char *next_hostname = NULL;
    if (vtype(hostname_v) != T_STR) {
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_TYPE, "server hostname must be a string");
    }
    next_hostname = strdup(js_getstr(js, hostname_v, NULL));
    if (!next_hostname) {
      free(server->hostname);
      free(server);
      return js_mkerr(js, "out of memory");
    }
    free(server->hostname);
    server->hostname = next_hostname;
  }

  if (vtype(timeout_v) != T_UNDEF && vtype(timeout_v) != T_NULL) {
    if (vtype(timeout_v) != T_NUM) {
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_TYPE, "server idleTimeout must be a number");
    }
    server->idle_timeout_secs = (int)js_getnum(timeout_v);
    if (server->idle_timeout_secs < 0) {
      free(server->hostname);
      free(server);
      return js_mkerr_typed(js, JS_ERR_RANGE, "server idleTimeout must be >= 0");
    }
  }

  uv_signal_init(server->loop, &server->sigint_handle);
  uv_signal_init(server->loop, &server->sigterm_handle);
  server->sigint_handle.data = server;
  server->sigterm_handle.data = server;

  callbacks.on_read = server_on_read;
  callbacks.on_end = server_on_end;
  callbacks.on_conn_close = server_on_conn_close;
  callbacks.on_listener_close = server_on_listener_close;

  rc = ant_listener_listen_tcp(
    &server->listener, server->loop,
    server->hostname, server->port,
    128, (uint64_t)server->idle_timeout_secs * 1000ULL, &callbacks, server
  );
  
  if (rc != 0) {
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
