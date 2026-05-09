// stub: node:tls implementation
// just enough for tls routing

#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <tlsuv/tlsuv.h>
#include <tlsuv/tls_engine.h>

#include "ant.h"
#include "internal.h"
#include "ptr.h"
#include "errors.h"

#include "gc/modules.h"
#include "modules/tls.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/net.h"
#include "modules/symbol.h"
#include "modules/timer.h"
#include "silver/engine.h"

typedef struct
  ant_tls_socket_s 
  ant_tls_socket_t;

typedef struct ant_tls_context_wrap_s {
  ant_value_t obj;
  tls_context *ctx;
  tlsuv_private_key_t key;
  tlsuv_certificate_t cert;
  unsigned refs;
  bool closed;
} ant_tls_context_wrap_t;

typedef struct tls_read_chunk_s {
  struct tls_read_chunk_s *next;
  char *data;
  size_t len;
  size_t off;
} tls_read_chunk_t;

typedef struct tls_write_req_s {
  uv_write_t req;
  ant_tls_socket_t *socket;
  struct tls_write_req_s *next;
  ant_value_t callback;
  char *data;
  size_t len;
} tls_write_req_t;

typedef struct ant_tls_socket_s {
  ant_t *js;
  ant_value_t obj;
  ant_value_t encoding;
  tlsuv_stream_t stream;
  uv_connect_t connect_req;
  tls_context *ctx;
  ant_tls_context_wrap_t *ctx_wrap;
  ant_value_t secure_context;
  char *host;
  char *servername;
  int port;
  char **alpn_protocols;
  int alpn_count;
  tls_read_chunk_t *read_head;
  tls_read_chunk_t *read_tail;
  size_t read_len;
  tls_write_req_t *writes;
  struct ant_tls_socket_s *next_active;
  uint64_t timeout_ms;
  uint64_t bytes_read;
  uint64_t bytes_written;
  bool owns_ctx;
  bool active;
  bool connecting;
  bool destroyed;
  bool closing;
  bool had_error;
  bool ended;
  bool read_drain_scheduled;
} ant_tls_socket_t;

enum {
  TLS_CONTEXT_NATIVE_TAG = 0x544c5343u, // TLSC
  TLS_SOCKET_NATIVE_TAG = 0x544c534bu,  // TLSK
};

static ant_value_t g_tls_context_proto = 0;
static ant_value_t g_tls_context_ctor = 0;
static ant_value_t g_tls_socket_proto = 0;
static ant_value_t g_tls_socket_ctor = 0;
static ant_tls_socket_t *g_active_tls_sockets = NULL;

static void tls_context_dispose(ant_tls_context_wrap_t *wrap) {
  if (!wrap) return;

  if (wrap->cert && wrap->cert->free) wrap->cert->free(wrap->cert);
  if (wrap->key && wrap->key->free) wrap->key->free(wrap->key);
  if (wrap->ctx && wrap->ctx->free_ctx) wrap->ctx->free_ctx(wrap->ctx);

  wrap->cert = NULL;
  wrap->key = NULL;
  wrap->ctx = NULL;
}

static void tls_context_free(ant_tls_context_wrap_t *wrap) {
  if (!wrap || wrap->closed) return;
  wrap->closed = true;
  if (wrap->refs > 0) return;
  tls_context_dispose(wrap);
}

static void tls_context_retain(ant_tls_context_wrap_t *wrap) {
  if (wrap) wrap->refs++;
}

static void tls_context_release(ant_tls_context_wrap_t *wrap) {
  if (!wrap || wrap->refs == 0) return;
  wrap->refs--;
  if (wrap->refs == 0 && wrap->closed) tls_context_dispose(wrap);
}

static ant_tls_context_wrap_t *tls_context_data(ant_value_t value) {
  return (ant_tls_context_wrap_t *)js_get_native(value, TLS_CONTEXT_NATIVE_TAG);
}

static ant_tls_socket_t *tls_socket_data(ant_value_t value) {
  return (ant_tls_socket_t *)js_get_native(value, TLS_SOCKET_NATIVE_TAG);
}

static void tls_add_active_socket(ant_tls_socket_t *socket) {
  if (!socket || socket->active) return;
  socket->active = true;
  socket->next_active = g_active_tls_sockets;
  g_active_tls_sockets = socket;
}

static void tls_remove_active_socket(ant_tls_socket_t *socket) {
  ant_tls_socket_t **it = NULL;
  for (it = &g_active_tls_sockets; *it; it = &(*it)->next_active) {
  if (*it == socket) {
    *it = socket->next_active;
    socket->next_active = NULL;
    socket->active = false;
    return;
  }}
}

static ant_value_t tls_call_value(
  ant_t *js,
  ant_value_t fn,
  ant_value_t this_val,
  ant_value_t *args,
  int nargs
) {
  ant_value_t saved_this = js->this_val;
  ant_value_t result = js_mkundef();

  js->this_val = this_val;
  if (vtype(fn) == T_CFUNC) result = js_as_cfunc(fn)(js, args, nargs);
  else result = sv_vm_call(js->vm, js, fn, this_val, args, nargs, NULL, false);
  js->this_val = saved_this;
  return result;
}

static bool tls_emit(ant_t *js, ant_value_t target, const char *event, ant_value_t *args, int nargs) {
  return eventemitter_emit_args(js, target, event, args, nargs);
}

static ant_tls_socket_t *tls_require_socket(ant_t *js, ant_value_t this_val) {
  ant_tls_socket_t *socket = tls_socket_data(this_val);
  if (!socket) {
    js->thrown_exists = true;
    js->thrown_value = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TLS socket");
    return NULL;
  }
  return socket;
}

static ant_value_t tls_make_buffer_chunk(ant_t *js, const char *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  if (len > 0 && data) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Buffer");
}

static void tls_socket_sync_state(ant_tls_socket_t *socket) {
  ant_t *js = NULL;
  const char *ready_state = "open";

  if (!socket || !is_object_type(socket->obj)) return;
  js = socket->js;
  if (socket->destroyed) ready_state = "closed";
  else if (socket->connecting) ready_state = "opening";

  js_set(js, socket->obj, "encrypted", js_true);
  js_set(js, socket->obj, "authorized", js_true);
  js_set(js, socket->obj, "authorizationError", js_mknull());
  js_set(js, socket->obj, "secureConnecting", js_bool(socket->connecting));
  js_set(js, socket->obj, "connecting", js_bool(socket->connecting));
  js_set(js, socket->obj, "destroyed", js_bool(socket->destroyed));
  js_set(js, socket->obj, "readable", js_bool(!socket->destroyed));
  js_set(js, socket->obj, "writable", js_bool(!socket->destroyed));
  js_set(js, socket->obj, "writableNeedDrain", js_false);
  js_set(js, socket->obj, "readyState", js_mkstr(js, ready_state, strlen(ready_state)));
  js_set(js, socket->obj, "timeout", js_mknum((double)socket->timeout_ms));
  js_set(js, socket->obj, "bytesRead", js_mknum((double)socket->bytes_read));
  js_set(js, socket->obj, "bytesWritten", js_mknum((double)socket->bytes_written));
  js_set(js, socket->obj, "remoteAddress", socket->host ? js_mkstr(js, socket->host, strlen(socket->host)) : js_mkundef());
  js_set(js, socket->obj, "remotePort", socket->port > 0 ? js_mknum((double)socket->port) : js_mkundef());
  js_set(js, socket->obj, "remoteFamily", js_mkstr(js, "IPv4", 4));
}

static void tls_define_default_socket_state(ant_t *js, ant_value_t obj) {
  if (!is_object_type(obj)) return;
  if (vtype(js_get(js, obj, "encrypted")) == T_UNDEF) js_set(js, obj, "encrypted", js_true);
  if (vtype(js_get(js, obj, "authorized")) == T_UNDEF) js_set(js, obj, "authorized", js_true);
  if (vtype(js_get(js, obj, "authorizationError")) == T_UNDEF) js_set(js, obj, "authorizationError", js_mknull());
  if (vtype(js_get(js, obj, "secureConnecting")) == T_UNDEF) js_set(js, obj, "secureConnecting", js_false);
}

static bool tls_socket_push_read(
  ant_tls_socket_t *socket,
  const char *data,
  size_t len,
  bool front
) {
  tls_read_chunk_t *chunk = NULL;

  if (!socket || !data || len == 0) return true;
  chunk = calloc(1, sizeof(*chunk));
  if (!chunk) return false;
  chunk->data = malloc(len);
  if (!chunk->data) {
    free(chunk);
    return false;
  }
  memcpy(chunk->data, data, len);
  chunk->len = len;

  if (front) {
    chunk->next = socket->read_head;
    socket->read_head = chunk;
    if (!socket->read_tail) socket->read_tail = chunk;
  } else {
    if (socket->read_tail) socket->read_tail->next = chunk;
    else socket->read_head = chunk;
    socket->read_tail = chunk;
  }
  socket->read_len += len;
  return true;
}

static void tls_socket_free_read_queue(ant_tls_socket_t *socket) {
  tls_read_chunk_t *chunk = socket ? socket->read_head : NULL;
  while (chunk) {
    tls_read_chunk_t *next = chunk->next;
    free(chunk->data);
    free(chunk);
    chunk = next;
  }
  if (socket) {
    socket->read_head = NULL;
    socket->read_tail = NULL;
    socket->read_len = 0;
  }
}

static ant_value_t tls_make_error(ant_t *js, tls_context *ctx, long code, const char *fallback) {
  const char *message = fallback;
  if (ctx && ctx->strerror) {
    const char *detail = ctx->strerror(code);
    if (detail && *detail) message = detail;
  }
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s", message ? message : "TLS error");
}

static void tls_socket_free_alpn(ant_tls_socket_t *socket) {
  if (!socket || !socket->alpn_protocols) return;
  for (int i = 0; i < socket->alpn_count; i++) free(socket->alpn_protocols[i]);
  free(socket->alpn_protocols);
  socket->alpn_protocols = NULL;
  socket->alpn_count = 0;
}

static void tls_socket_free(ant_tls_socket_t *socket) {
  if (!socket) return;
  tls_remove_active_socket(socket);
  
  if (is_object_type(socket->obj)) 
    js_clear_native(socket->obj, TLS_SOCKET_NATIVE_TAG);
  tls_socket_free_read_queue(socket);
  tls_socket_free_alpn(socket);
  
  if (socket->ctx_wrap) tls_context_release(socket->ctx_wrap);
  if (socket->owns_ctx && socket->ctx && socket->ctx->free_ctx) 
    socket->ctx->free_ctx(socket->ctx);
  
  free(socket->host);
  free(socket->servername);
  free(socket);
}

static void tls_socket_close_cb(uv_handle_t *handle) {
  tlsuv_stream_t *tls_stream = (tlsuv_stream_t *)handle;
  ant_tls_socket_t *socket = tls_stream ? (ant_tls_socket_t *)tls_stream->data : NULL;
  
  ant_t *js = socket ? socket->js : NULL;
  ant_value_t had_error = 0;
  if (!socket || !js) return;

  socket->destroyed = true;
  socket->connecting = false;
  socket->closing = false;
  
  tls_socket_sync_state(socket);
  had_error = js_bool(socket->had_error);
  tls_emit(js, socket->obj, "close", &had_error, 1);
  tls_socket_free(socket);
}

static void tls_socket_close(ant_tls_socket_t *socket) {
  if (!socket || socket->closing || socket->destroyed) return;
  socket->closing = true;
  tlsuv_stream_close(&socket->stream, tls_socket_close_cb);
}

static void tls_socket_maybe_emit_end(ant_tls_socket_t *socket) {
  ant_t *js = socket ? socket->js : NULL;
  if (!socket || !js || !socket->ended || socket->read_len != 0 || socket->read_head) return;
  tls_emit(js, socket->obj, "end", NULL, 0);
  tls_socket_close(socket);
}

static ant_value_t tls_socket_drain_read_queue(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_tls_socket_t *socket = tls_socket_data(obj);
  
  if (!socket) return js_mkundef();
  socket->read_drain_scheduled = false;

  while (
    !socket->destroyed &&
    socket->read_head &&
    eventemitter_listener_count(js, obj, "data") > 0
  ) {
    tls_read_chunk_t *chunk = socket->read_head;
    size_t len = chunk->len - chunk->off;
    ant_value_t data = vtype(socket->encoding) == T_STR
      ? js_mkstr(js, chunk->data + chunk->off, len)
      : tls_make_buffer_chunk(js, chunk->data + chunk->off, len);
    
    if (is_err(data)) {
      socket->had_error = true;
      tls_emit(js, obj, "error", &data, 1);
      return data;
    }
    
    socket->read_head = chunk->next;
    if (socket->read_tail == chunk) socket->read_tail = NULL;
    socket->read_len -= len;
    free(chunk->data);
    free(chunk);
    
    tls_emit(js, obj, "data", &data, 1);
  }

  tls_socket_maybe_emit_end(socket);
  return js_mkundef();
}

static void tls_socket_schedule_read_drain(ant_tls_socket_t *socket) {
  if (!socket || socket->read_drain_scheduled || socket->destroyed) return;
  socket->read_drain_scheduled = true;
  queue_microtask(socket->js, js_heavy_mkfun(socket->js, tls_socket_drain_read_queue, socket->obj));
}

static bool tls_value_bytes(
  ant_t *js,
  ant_value_t value,
  const char **bytes_out,
  size_t *len_out,
  ant_value_t *error_out
) {
  ant_value_t str_value = 0;
  const uint8_t *buffer_bytes = NULL;

  if (error_out) *error_out = js_mkundef();
  if (bytes_out) *bytes_out = NULL;
  if (len_out) *len_out = 0;

  if (vtype(value) == T_UNDEF || vtype(value) == T_NULL) return true;
  if (buffer_source_get_bytes(js, value, &buffer_bytes, len_out)) {
    if (bytes_out) *bytes_out = (const char *)buffer_bytes;
    return true;
  }

  str_value = js_tostring_val(js, value);
  if (is_err(str_value)) {
    if (error_out) *error_out = str_value;
    return false;
  }

  if (!bytes_out || !len_out) return true;
  *bytes_out = js_getstr(js, str_value, len_out);
  
  if (!*bytes_out) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TLS input");
    return false;
  }

  return true;
}

static ant_value_t tls_stream_error(ant_tls_socket_t *socket, int status, const char *fallback) {
  const char *message = fallback;
  if (socket) {
    const char *detail = tlsuv_stream_get_error(&socket->stream);
    if (detail && *detail) message = detail;
    else if (status < 0) message = uv_strerror(status);
  }
  return js_mkerr_typed(socket->js, JS_ERR_TYPE, "%s", message ? message : "TLS error");
}

static bool tls_parse_write_args(
  ant_t *js,
  ant_value_t *args,
  int nargs,
  const uint8_t **bytes_out,
  size_t *len_out,
  ant_value_t *callback_out,
  ant_value_t *error_out
) {
  ant_value_t value = 0;

  if (bytes_out) *bytes_out = NULL;
  if (len_out) *len_out = 0;
  if (callback_out) *callback_out = js_mkundef();
  if (error_out) *error_out = js_mkundef();

  if (nargs < 1 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL) return true;
  value = args[0];

  if (!buffer_source_get_bytes(js, value, bytes_out, len_out)) {
    size_t slen = 0;
    ant_value_t str_val = js_tostring_val(js, value);
    const char *str = NULL;

    if (is_err(str_val)) {
      if (error_out) *error_out = str_val;
      return false;
    }

    str = js_getstr(js, str_val, &slen);
    if (!str) {
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid socket write data");
      return false;
    }

    if (bytes_out) *bytes_out = (const uint8_t *)str;
    if (len_out) *len_out = slen;
  }

  if (callback_out) {
    if (nargs > 1 && is_callable(args[1])) *callback_out = args[1];
    else if (nargs > 2 && is_callable(args[2])) *callback_out = args[2];
  }
  return true;
}

static ant_value_t js_tls_context_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_context_wrap_t *wrap = tls_context_data(js_getthis(js));
  if (!wrap) return js_getthis(js);
  tls_context_free(wrap);
  js_clear_native(wrap->obj, TLS_CONTEXT_NATIVE_TAG);
  return js_getthis(js);
}

static void tls_init_context_proto(ant_t *js) {
  if (g_tls_context_proto && g_tls_context_ctor) return;

  if (!g_tls_context_proto) {
    g_tls_context_proto = js_mkobj(js);
    js_set(js, g_tls_context_proto, "close", js_mkfun(js_tls_context_close));
    js_set_sym(js, g_tls_context_proto, get_toStringTag_sym(), js_mkstr(js, "SecureContext", 13));
  }
}

static void tls_write_remove(ant_tls_socket_t *socket, tls_write_req_t *write) {
  tls_write_req_t **it = NULL;
  if (!socket || !write) return;
  for (it = &socket->writes; *it; it = &(*it)->next) {
  if (*it == write) {
    *it = write->next;
    write->next = NULL;
    return;
  }}
}

static void tls_socket_maybe_close_after_writes(ant_tls_socket_t *socket) {
  if (socket && socket->ended && !socket->writes) tls_socket_close(socket);
}

static void tls_socket_write_cb(uv_write_t *req, int status) {
  tls_write_req_t *write = (tls_write_req_t *)req;
  ant_tls_socket_t *socket = write ? write->socket : NULL;
  ant_t *js = socket ? socket->js : NULL;

  if (!socket || !js) return;

  tls_write_remove(socket, write);
  if (status < 0) {
    ant_value_t err = tls_stream_error(socket, status, "TLS write failed");
    socket->had_error = true;
    tls_emit(js, socket->obj, "error", &err, 1);
    tls_socket_close(socket);
  } else {
    socket->bytes_written += write->len;
    tls_socket_sync_state(socket);
    if (is_callable(write->callback))
      tls_call_value(js, write->callback, js_mkundef(), NULL, 0);
  }

  free(write->data);
  free(write);
  tls_socket_maybe_close_after_writes(socket);
}

static void tls_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)handle;
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void tls_socket_on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  tlsuv_stream_t *tls_stream = (tlsuv_stream_t *)stream;
  ant_tls_socket_t *socket = tls_stream ? (ant_tls_socket_t *)tls_stream->data : NULL;
  ant_t *js = socket ? socket->js : NULL;

  if (!socket || !js) goto done;

  if (nread > 0) {
    ant_value_t chunk = 0;
    socket->bytes_read += (uint64_t)nread;
    tls_socket_sync_state(socket);
    if (eventemitter_listener_count(js, socket->obj, "data") > 0) {
      if (vtype(socket->encoding) == T_STR)
        chunk = js_mkstr(js, buf->base, (size_t)nread);
      else chunk = tls_make_buffer_chunk(js, buf->base, (size_t)nread);
      if (is_err(chunk)) {
        socket->had_error = true;
        tls_emit(js, socket->obj, "error", &chunk, 1);
        tls_socket_close(socket);
        goto done;
      }
      tls_emit(js, socket->obj, "data", &chunk, 1);
    } else {
      if (!tls_socket_push_read(socket, buf->base, (size_t)nread, false)) {
        ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
        socket->had_error = true;
        tls_emit(js, socket->obj, "error", &err, 1);
        tls_socket_close(socket);
        goto done;
      }
      tls_emit(js, socket->obj, "readable", NULL, 0);
    }
  } else if (nread == UV_EOF) {
    socket->ended = true;
    tls_socket_maybe_emit_end(socket);
  } else if (nread < 0) {
    ant_value_t err = tls_stream_error(socket, (int)nread, "TLS read failed");
    socket->had_error = true;
    tls_emit(js, socket->obj, "error", &err, 1);
    tls_socket_close(socket);
  }

done:
  free(buf ? buf->base : NULL);
}

static void tls_socket_on_connect(uv_connect_t *req, int status) {
  ant_tls_socket_t *socket = req ? (ant_tls_socket_t *)req->data : NULL;
  ant_t *js = socket ? socket->js : NULL;
  const char *protocol = NULL;

  if (!socket || !js) return;
  socket->connecting = false;

  if (status < 0) {
    ant_value_t err = tls_stream_error(socket, status, "TLS connect failed");
    socket->had_error = true;
    tls_socket_sync_state(socket);
    tls_emit(js, socket->obj, "error", &err, 1);
    tls_socket_close(socket);
    return;
  }

  protocol = tlsuv_stream_get_protocol(&socket->stream);
  if (protocol && *protocol)
    js_set(js, socket->obj, "alpnProtocol", js_mkstr(js, protocol, strlen(protocol)));
  else js_set(js, socket->obj, "alpnProtocol", js_false);

  tlsuv_stream_read_start(&socket->stream, tls_alloc_cb, tls_socket_on_read);
  tls_socket_sync_state(socket);
  tls_emit(js, socket->obj, "secureConnect", NULL, 0);
  tls_emit(js, socket->obj, "connect", NULL, 0);
}

static ant_value_t js_tls_socket_read(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  size_t wanted = 0;
  char *data = NULL;
  size_t copied = 0;
  ant_value_t out = 0;

  if (!socket) return js->thrown_value;
  if (!socket->read_head || socket->read_len == 0) return js_mknull();

  if (nargs > 0 && vtype(args[0]) == T_NUM && js_getnum(args[0]) > 0)
    wanted = (size_t)js_getnum(args[0]);
  if (wanted == 0 || wanted > socket->read_len) wanted = socket->read_len;

  data = malloc(wanted);
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");

  while (copied < wanted && socket->read_head) {
    tls_read_chunk_t *chunk = socket->read_head;
    size_t available = chunk->len - chunk->off;
    size_t take = wanted - copied;
    if (take > available) take = available;

    memcpy(data + copied, chunk->data + chunk->off, take);
    copied += take;
    chunk->off += take;
    socket->read_len -= take;

    if (chunk->off >= chunk->len) {
      socket->read_head = chunk->next;
      if (socket->read_tail == chunk) socket->read_tail = NULL;
      free(chunk->data);
      free(chunk);
    }
  }

  if (vtype(socket->encoding) == T_STR)
    out = js_mkstr(js, data, copied);
  else out = tls_make_buffer_chunk(js, data, copied);
  free(data);
  tls_socket_maybe_emit_end(socket);
  return out;
}

static ant_value_t js_tls_socket_unshift(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t err = js_mkundef();

  if (!socket) return js->thrown_value;
  if (!tls_parse_write_args(js, args, nargs, &bytes, &len, NULL, &err)) return err;
  if (len > 0) {
    if (!tls_socket_push_read(socket, (const char *)bytes, len, true))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
    tls_socket_schedule_read_drain(socket);
  }
  return js_getthis(js);
}

static ant_value_t js_tls_socket_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t callback = js_mkundef();
  ant_value_t err = js_mkundef();
  tls_write_req_t *write = NULL;
  uv_buf_t buf;
  int rc = 0;

  if (!socket) return js->thrown_value;
  if (socket->destroyed || socket->closing) return js_false;
  if (!tls_parse_write_args(js, args, nargs, &bytes, &len, &callback, &err)) return err;
  if (len == 0) {
    if (is_callable(callback)) tls_call_value(js, callback, js_mkundef(), NULL, 0);
    return js_true;
  }

  write = calloc(1, sizeof(*write));
  if (!write) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  write->data = malloc(len);
  if (!write->data) {
    free(write);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }
  memcpy(write->data, bytes, len);
  write->socket = socket;
  write->callback = callback;
  write->len = len;
  write->next = socket->writes;
  socket->writes = write;

  buf = uv_buf_init(write->data, (unsigned int)len);
  rc = tlsuv_stream_write(&write->req, &socket->stream, &buf, tls_socket_write_cb);
  if (rc != 0) {
    tls_write_remove(socket, write);
    free(write->data);
    free(write);
    return js_false;
  }

  return js_true;
}

static ant_value_t js_tls_socket_end(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  ant_value_t result = js_getthis(js);

  if (!socket) return js->thrown_value;
  if (nargs > 0 && vtype(args[0]) != T_UNDEF && vtype(args[0]) != T_NULL) {
    result = js_tls_socket_write(js, args, nargs);
    if (is_err(result)) return result;
  }
  socket->ended = true;
  tls_socket_maybe_close_after_writes(socket);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_destroy(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;

  if (nargs > 0 && vtype(args[0]) != T_UNDEF && vtype(args[0]) != T_NULL) {
    ant_value_t err = args[0];
    socket->had_error = true;
    tls_emit(js, socket->obj, "error", &err, 1);
  }

  tls_socket_close(socket);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_pause(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  tlsuv_stream_read_stop(&socket->stream);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_resume(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  if (!socket->destroyed) tlsuv_stream_read_start(&socket->stream, tls_alloc_cb, tls_socket_on_read);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_setEncoding(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  ant_value_t encoding = js_mkundef();

  if (!socket) return js->thrown_value;
  if (nargs > 0 && vtype(args[0]) != T_UNDEF) {
    encoding = js_tostring_val(js, args[0]);
    if (is_err(encoding)) return encoding;
  }
  socket->encoding = encoding;
  return js_getthis(js);
}

static ant_value_t js_tls_socket_setNoDelay(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  bool enable = nargs == 0 || js_truthy(js, args[0]);
  if (!socket) return js->thrown_value;
  tlsuv_stream_nodelay(&socket->stream, enable ? 1 : 0);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_setKeepAlive(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  bool enable = nargs > 0 && js_truthy(js, args[0]);
  unsigned int delay = nargs > 1 && vtype(args[1]) == T_NUM ? (unsigned int)(js_getnum(args[1]) / 1000.0) : 0;
  if (!socket) return js->thrown_value;
  tlsuv_stream_keepalive(&socket->stream, enable ? 1 : 0, delay);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_setTimeout(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  socket->timeout_ms = nargs > 0 && vtype(args[0]) == T_NUM && js_getnum(args[0]) > 0 ? (uint64_t)js_getnum(args[0]) : 0;
  tls_socket_sync_state(socket);
  if (nargs > 1 && is_callable(args[1]))
    eventemitter_add_listener(js, socket->obj, "timeout", args[1], true);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_address(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  ant_value_t out = js_mkobj(js);
  if (!socket) return js->thrown_value;
  js_set(js, out, "address", js_mkundef());
  js_set(js, out, "port", js_mkundef());
  js_set(js, out, "family", js_mkundef());
  return out;
}

static ant_value_t js_tls_socket_ref(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  if (
    uv_handle_get_type((uv_handle_t *)&socket->stream.watcher) != UV_UNKNOWN_HANDLE &&
    !uv_is_closing((uv_handle_t *)&socket->stream.watcher)
  ) uv_ref((uv_handle_t *)&socket->stream.watcher);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_unref(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return js->thrown_value;
  if (
    uv_handle_get_type((uv_handle_t *)&socket->stream.watcher) != UV_UNKNOWN_HANDLE &&
    !uv_is_closing((uv_handle_t *)&socket->stream.watcher)
  ) uv_unref((uv_handle_t *)&socket->stream.watcher);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_cork(ant_t *js, ant_value_t *args, int nargs) {
  return js_getthis(js);
}

static ant_value_t js_tls_socket_uncork(ant_t *js, ant_value_t *args, int nargs) {
  return js_getthis(js);
}

static ant_value_t js_tls_socket_getProtocol(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  const char *protocol = NULL;
  if (!socket) return js->thrown_value;
  protocol = tlsuv_stream_get_protocol(&socket->stream);
  return protocol && *protocol ? js_mkstr(js, protocol, strlen(protocol)) : js_mkundef();
}

static ant_value_t js_tls_socket_getCipher(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static ant_value_t js_tls_socket_getSession(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static ant_value_t js_tls_socket_getPeerCertificate(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkobj(js);
}

static ant_value_t js_tls_socket_renegotiate(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t callback = js_mkundef();
  if (nargs > 1 && is_callable(args[1])) callback = args[1];
  else if (nargs > 0 && is_callable(args[0])) callback = args[0];
  if (is_callable(callback)) {
    ant_value_t cb_args[] = {js_mknull()};
    tls_call_value(js, callback, js_getthis(js), cb_args, 1);
  }
  return js_true;
}

static ant_value_t js_tls_socket_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t existing = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t obj = 0;
  ant_value_t proto = 0;

  tls_init_socket_proto(js);
  if (tls_socket_data(existing)) {
    tls_define_default_socket_state(js, existing);
    js_set_proto_wb(js, existing, g_tls_socket_proto);
    return existing;
  }

  obj = js_mkobj(js);
  proto = js_instance_proto_from_new_target(js, g_tls_socket_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  tls_define_default_socket_state(js, obj);
  js_set(js, obj, "alpnProtocol", js_false);
  return obj;
}

void tls_init_socket_proto(ant_t *js) {
  ant_value_t events = 0;
  ant_value_t ee_ctor = 0;
  ant_value_t ee_proto = 0;
  
  ant_value_t net = 0;
  ant_value_t net_socket_ctor = 0;
  ant_value_t net_socket_proto = 0;

  if (g_tls_socket_proto && g_tls_socket_ctor) return;

  if (!g_tls_socket_proto) {
    net = net_library(js);
    net_socket_ctor = js_get(js, net, "Socket");
    net_socket_proto = js_get(js, net_socket_ctor, "prototype");

    if (!is_object_type(net_socket_proto)) {
      events = events_library(js);
      ee_ctor = js_get(js, events, "EventEmitter");
      ee_proto = js_get(js, ee_ctor, "prototype");
      net_socket_proto = ee_proto;
    }

    g_tls_socket_proto = js_mkobj(js);
    if (is_object_type(net_socket_proto)) js_set_proto_init(g_tls_socket_proto, net_socket_proto);
    js_set(js, g_tls_socket_proto, "read", js_mkfun(js_tls_socket_read));
    js_set(js, g_tls_socket_proto, "unshift", js_mkfun(js_tls_socket_unshift));
    js_set(js, g_tls_socket_proto, "write", js_mkfun(js_tls_socket_write));
    js_set(js, g_tls_socket_proto, "end", js_mkfun(js_tls_socket_end));
    js_set(js, g_tls_socket_proto, "destroy", js_mkfun(js_tls_socket_destroy));
    js_set(js, g_tls_socket_proto, "pause", js_mkfun(js_tls_socket_pause));
    js_set(js, g_tls_socket_proto, "resume", js_mkfun(js_tls_socket_resume));
    js_set(js, g_tls_socket_proto, "setEncoding", js_mkfun(js_tls_socket_setEncoding));
    js_set(js, g_tls_socket_proto, "setNoDelay", js_mkfun(js_tls_socket_setNoDelay));
    js_set(js, g_tls_socket_proto, "setKeepAlive", js_mkfun(js_tls_socket_setKeepAlive));
    js_set(js, g_tls_socket_proto, "setTimeout", js_mkfun(js_tls_socket_setTimeout));
    js_set(js, g_tls_socket_proto, "address", js_mkfun(js_tls_socket_address));
    js_set(js, g_tls_socket_proto, "ref", js_mkfun(js_tls_socket_ref));
    js_set(js, g_tls_socket_proto, "unref", js_mkfun(js_tls_socket_unref));
    js_set(js, g_tls_socket_proto, "cork", js_mkfun(js_tls_socket_cork));
    js_set(js, g_tls_socket_proto, "uncork", js_mkfun(js_tls_socket_uncork));
    js_set(js, g_tls_socket_proto, "getProtocol", js_mkfun(js_tls_socket_getProtocol));
    js_set(js, g_tls_socket_proto, "getCipher", js_mkfun(js_tls_socket_getCipher));
    js_set(js, g_tls_socket_proto, "getSession", js_mkfun(js_tls_socket_getSession));
    js_set(js, g_tls_socket_proto, "getPeerCertificate", js_mkfun(js_tls_socket_getPeerCertificate));
    js_set(js, g_tls_socket_proto, "renegotiate", js_mkfun(js_tls_socket_renegotiate));
    js_set_sym(js, g_tls_socket_proto, get_toStringTag_sym(), js_mkstr(js, "TLSSocket", 9));
  }

  if (!g_tls_socket_ctor)
    g_tls_socket_ctor = js_make_ctor(js, js_tls_socket_ctor, g_tls_socket_proto, "TLSSocket", 9);
}

static ant_value_t js_tls_create_context(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t obj = 0;
  ant_value_t error = js_mkundef();
  
  ant_tls_context_wrap_t *wrap = NULL;
  ant_value_t ca_v = js_mkundef();
  ant_value_t key_v = js_mkundef();
  ant_value_t cert_v = js_mkundef();
  ant_value_t partial_v = js_mkundef();
  
  const char *ca = NULL;
  const char *key_data = NULL;
  const char *cert_data = NULL;
  
  size_t ca_len = 0;
  size_t key_len = 0;
  size_t cert_len = 0;
  int rc = 0;

  if (vtype(options) != T_UNDEF && vtype(options) != T_NULL && vtype(options) != T_OBJ)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TLS context options must be an object");

  tls_init_context_proto(js);

  if (vtype(options) == T_OBJ) {
    ca_v = js_get(js, options, "ca");
    key_v = js_get(js, options, "key");
    cert_v = js_get(js, options, "cert");
    partial_v = js_get(js, options, "allowPartialChain");
  }

  if (!tls_value_bytes(js, ca_v, &ca, &ca_len, &error)) return error;
  if (!tls_value_bytes(js, key_v, &key_data, &key_len, &error)) return error;
  if (!tls_value_bytes(js, cert_v, &cert_data, &cert_len, &error)) return error;

  wrap = calloc(1, sizeof(*wrap));
  if (!wrap) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");

  wrap->ctx = default_tls_context(ca, ca_len);
  if (!wrap->ctx) {
    free(wrap);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to create TLS context");
  }

  if (wrap->ctx->allow_partial_chain && js_truthy(js, partial_v)) {
  rc = wrap->ctx->allow_partial_chain(wrap->ctx, 1);
  if (rc != 0) {
    ant_value_t err = tls_make_error(js, wrap->ctx, rc, "Failed to enable partial chain verification");
    tls_context_free(wrap);
    free(wrap);
    return err;
  }}

  if (key_data) {
    if (!wrap->ctx->load_key) {
      tls_context_free(wrap);
      free(wrap);
      return js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support loading private keys");
    }
    
    rc = wrap->ctx->load_key(&wrap->key, key_data, key_len);
    if (rc != 0) {
      ant_value_t err = tls_make_error(js, wrap->ctx, rc, "Failed to load TLS private key");
      tls_context_free(wrap);
      free(wrap);
      return err;
    }
    
    if (cert_data) {
    if (!wrap->ctx->load_cert) {
      tls_context_free(wrap);
      free(wrap);
      return js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support loading certificates");
    }
    
    rc = wrap->ctx->load_cert(&wrap->cert, cert_data, cert_len);
    if (rc != 0) {
      ant_value_t err = tls_make_error(js, wrap->ctx, rc, "Failed to load TLS certificate");
      tls_context_free(wrap);
      free(wrap);
      return err;
    }}

    if (!wrap->ctx->set_own_cert) {
      tls_context_free(wrap);
      free(wrap);
      return js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support own certificate configuration");
    }

    rc = wrap->ctx->set_own_cert(wrap->ctx, wrap->key, wrap->cert);
    if (rc != 0) {
      ant_value_t err = tls_make_error(js, wrap->ctx, rc, "Failed to configure TLS certificate");
      tls_context_free(wrap);
      free(wrap);
      return err;
    }
  } else if (cert_data) {
    tls_context_free(wrap);
    free(wrap);
    return js_mkerr_typed(js, JS_ERR_TYPE, "TLS certificate requires a private key");
  }

  obj = js_mkobj(js);
  js_set_proto_init(obj, g_tls_context_proto);
  wrap->obj = obj;
  
  js_set_native(obj, wrap, TLS_CONTEXT_NATIVE_TAG);
  
  return obj;
}

static ant_value_t js_tls_is_context(ant_t *js, ant_value_t *args, int nargs) {
  ant_tls_context_wrap_t *wrap = nargs > 0 ? tls_context_data(args[0]) : NULL;
  return js_bool(wrap && !wrap->closed && wrap->ctx);
}

static ant_value_t js_tls_secure_context_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return js_tls_create_context(js, args, nargs);
}

static ant_value_t js_tls_set_config_path(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str_value = js_mkundef();
  const char *path = NULL;
  int rc = 0;

  if (nargs < 1 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL) {
    rc = tlsuv_set_config_path(NULL);
    if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
    return js_mkundef();
  }

  str_value = js_tostring_val(js, args[0]);
  if (is_err(str_value)) return str_value;
  
  path = js_getstr(js, str_value, NULL);
  if (!path) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TLS config path");

  rc = tlsuv_set_config_path(path);
  if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
  
  return js_mkundef();
}

static bool tls_socket_set_alpn(ant_t *js, ant_tls_socket_t *socket, ant_value_t protocols) {
  ant_offset_t len = 0;

  if (!socket || vtype(protocols) != T_ARR) return true;
  len = js_arr_len(js, protocols);
  if (len <= 0) return true;

  socket->alpn_protocols = calloc((size_t)len, sizeof(char *));
  if (!socket->alpn_protocols) return false;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = js_arr_get(js, protocols, i);
    ant_value_t str_val = js_tostring_val(js, item);
    const char *str = NULL;
    if (is_err(str_val)) return false;
    str = js_getstr(js, str_val, NULL);
    if (!str) return false;
    socket->alpn_protocols[socket->alpn_count] = strdup(str);
    if (!socket->alpn_protocols[socket->alpn_count]) return false;
    socket->alpn_count++;
  }

  return true;
}

static void tls_copy_connect_option(ant_t *js, ant_value_t dst, ant_value_t src, const char *key) {
  ant_value_t value = js_get(js, src, key);
  if (vtype(value) != T_UNDEF) js_set(js, dst, key, value);
}

static void tls_copy_connect_options(ant_t *js, ant_value_t dst, ant_value_t src) {
  if (vtype(src) != T_OBJ) return;
  tls_copy_connect_option(js, dst, src, "host");
  tls_copy_connect_option(js, dst, src, "hostname");
  tls_copy_connect_option(js, dst, src, "port");
  tls_copy_connect_option(js, dst, src, "servername");
  tls_copy_connect_option(js, dst, src, "secureContext");
  tls_copy_connect_option(js, dst, src, "ALPNProtocols");
}

static ant_value_t tls_normalize_connect_options(
  ant_t *js,
  ant_value_t *args,
  int nargs,
  ant_value_t *callback_out
) {
  int argc = nargs;
  ant_value_t options = js_mkobj(js);

  if (callback_out) *callback_out = js_mkundef();
  if (argc > 0 && is_callable(args[argc - 1])) {
    if (callback_out) *callback_out = args[argc - 1];
    argc--;
  }

  if (argc == 1 && vtype(args[0]) == T_OBJ) return args[0];
  if (argc > 0 && vtype(args[0]) == T_NUM) js_set(js, options, "port", args[0]);
  if (argc > 1 && vtype(args[1]) == T_STR) js_set(js, options, "host", args[1]);
  if (argc > 1 && vtype(args[1]) == T_OBJ) tls_copy_connect_options(js, options, args[1]);
  if (argc > 2 && vtype(args[2]) == T_OBJ) tls_copy_connect_options(js, options, args[2]);
  return options;
}

static ant_value_t js_tls_connect_options(ant_t *js, ant_value_t options, ant_value_t callback) {
  ant_value_t obj = 0;
  ant_tls_socket_t *socket = NULL;
  ant_tls_context_wrap_t *ctx_wrap = NULL;
  ant_value_t value = 0;
  const char *host = "localhost";
  const char *servername = NULL;
  int port = 443;
  int rc = 0;

  if (vtype(options) != T_OBJ)
    return js_mkerr_typed(js, JS_ERR_TYPE, "tls.connect requires an options object");

  value = js_get(js, options, "host");
  if (vtype(value) == T_STR) host = js_getstr(js, value, NULL);
  value = js_get(js, options, "hostname");
  if (vtype(value) == T_STR) host = js_getstr(js, value, NULL);
  value = js_get(js, options, "port");
  if (vtype(value) == T_NUM) port = (int)js_getnum(value);
  value = js_get(js, options, "servername");
  if (vtype(value) == T_STR) servername = js_getstr(js, value, NULL);
  if (!servername || !*servername) servername = host;

  tls_init_socket_proto(js);

  socket = calloc(1, sizeof(*socket));
  if (!socket) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");

  socket->js = js;
  socket->encoding = js_mkundef();
  socket->secure_context = js_mkundef();
  socket->host = strdup(host ? host : "localhost");
  socket->servername = strdup(servername ? servername : (host ? host : "localhost"));
  socket->port = port > 0 ? port : 443;
  socket->connecting = true;
  if (!socket->host || !socket->servername) {
    tls_socket_free(socket);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }

  value = js_get(js, options, "secureContext");
  ctx_wrap = tls_context_data(value);
  if (ctx_wrap && !ctx_wrap->closed && ctx_wrap->ctx) {
    socket->ctx = ctx_wrap->ctx;
    socket->ctx_wrap = ctx_wrap;
    socket->secure_context = value;
    socket->owns_ctx = false;
    tls_context_retain(ctx_wrap);
  } else {
    socket->ctx = default_tls_context(NULL, 0);
    socket->owns_ctx = true;
    if (!socket->ctx) {
      tls_socket_free(socket);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to create TLS context");
    }
  }

  value = js_get(js, options, "ALPNProtocols");
  if (!tls_socket_set_alpn(js, socket, value)) {
    tls_socket_free(socket);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to configure TLS ALPN protocols");
  }

  obj = js_mkobj(js);
  js_set_proto_init(obj, g_tls_socket_proto);
  socket->obj = obj;
  js_set_native(obj, socket, TLS_SOCKET_NATIVE_TAG);
  tls_define_default_socket_state(js, obj);
  if (is_callable(callback)) eventemitter_add_listener(js, obj, "secureConnect", callback, true);
  tlsuv_stream_init(uv_default_loop(), &socket->stream, socket->ctx);
  socket->stream.data = socket;
  socket->connect_req.data = socket;
  if (socket->servername) tlsuv_stream_set_hostname(&socket->stream, socket->servername);
  if (socket->alpn_count > 0)
    tlsuv_stream_set_protocols(&socket->stream, socket->alpn_count, (const char **)socket->alpn_protocols);

  js_set(js, obj, "localAddress", js_mkundef());
  js_set(js, obj, "localPort", js_mkundef());
  js_set(js, obj, "localFamily", js_mkundef());
  js_set(js, obj, "alpnProtocol", js_false);
  tls_socket_sync_state(socket);
  tls_add_active_socket(socket);

  rc = tlsuv_stream_connect(&socket->connect_req, &socket->stream, socket->host, socket->port, tls_socket_on_connect);
  if (rc != 0) {
    ant_value_t err = tls_stream_error(socket, rc, "TLS connect failed");
    socket->connecting = false;
    socket->had_error = true;
    tls_socket_sync_state(socket);
    tls_emit(js, socket->obj, "error", &err, 1);
    tls_socket_close(socket);
  }

  return obj;
}

static ant_value_t js_tls_connect(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t callback = js_mkundef();
  ant_value_t options = tls_normalize_connect_options(js, args, nargs, &callback);
  return js_tls_connect_options(js, options, callback);
}

static ant_value_t js_tls_check_server_identity(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "tls.checkServerIdentity is not implemented");
}

static ant_value_t js_tls_get_ciphers(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkarr(js);
}

static ant_value_t tls_build_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t version = js_mkstr(js, "unknown", 7);
  ant_value_t connect = js_mkfun(js_tls_connect);
  ant_value_t create_secure_context = js_mkfun(js_tls_create_context);
  ant_value_t root_certificates = js_mkarr(js);
  
  tls_context *ctx = default_tls_context(NULL, 0);
  const char *version_str = NULL;

  if (ctx && ctx->version) {
    version_str = ctx->version();
    if (version_str) version = js_mkstr(js, version_str, strlen(version_str));
  }
  
  if (ctx && ctx->free_ctx) ctx->free_ctx(ctx);
  tls_init_context_proto(js);
  tls_init_socket_proto(js);
  
  if (!g_tls_context_ctor) g_tls_context_ctor = js_make_ctor(
    js, js_tls_secure_context_ctor,
    g_tls_context_proto, "SecureContext", 13
  );
  
  js_set(js, lib, "version", version);
  js_set(js, lib, "TLSSocket", g_tls_socket_ctor);
  js_set(js, lib, "SecureContext", g_tls_context_ctor);
  js_set(js, lib, "createSecureContext", create_secure_context);
  js_set(js, lib, "connect", connect);
  js_set(js, lib, "createConnection", connect);
  js_set(js, lib, "isSecureContext", js_mkfun(js_tls_is_context));
  js_set(js, lib, "setConfigPath", js_mkfun(js_tls_set_config_path));
  js_set(js, lib, "checkServerIdentity", js_mkfun(js_tls_check_server_identity));
  js_set(js, lib, "getCiphers", js_mkfun(js_tls_get_ciphers));
  
  builtin_object_freeze(js, &root_certificates, 1);
  js_set(js, lib, "rootCertificates", root_certificates);
  js_set(js, lib, "DEFAULT_ECDH_CURVE", js_mkstr(js, "auto", 4));
  js_set(js, lib, "DEFAULT_MIN_VERSION", js_mkstr(js, "TLSv1.2", 7));
  js_set(js, lib, "DEFAULT_MAX_VERSION", js_mkstr(js, "TLSv1.3", 7));
  js_set(js, lib, "CLIENT_RENEG_LIMIT", js_mknum(3));
  js_set(js, lib, "CLIENT_RENEG_WINDOW", js_mknum(600));
  js_set(js, lib, "default", lib);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "tls", 3));
  
  return lib;
}

ant_value_t tls_library(ant_t *js) {
  return tls_build_library(js);
}

void gc_mark_tls(ant_t *js, gc_mark_fn mark) {
  ant_tls_socket_t *socket = NULL;

  if (g_tls_context_proto) mark(js, g_tls_context_proto);
  if (g_tls_context_ctor) mark(js, g_tls_context_ctor);
  if (g_tls_socket_proto) mark(js, g_tls_socket_proto);
  if (g_tls_socket_ctor) mark(js, g_tls_socket_ctor);

  for (socket = g_active_tls_sockets; socket; socket = socket->next_active) {
    mark(js, socket->obj);
    if (vtype(socket->encoding) != T_UNDEF) mark(js, socket->encoding);
    if (vtype(socket->secure_context) != T_UNDEF) mark(js, socket->secure_context);
    for (tls_write_req_t *write = socket->writes; write; write = write->next) 
      if (vtype(write->callback) != T_UNDEF) mark(js, write->callback);
  }
}
