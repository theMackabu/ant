#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

#if defined(__APPLE__)
#include <dlfcn.h>
#elif defined(_WIN32)
#include <wincrypt.h>
#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#else
#include <unistd.h>
#endif

#include <openssl/base64.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <tlsuv/tlsuv.h>
#include <tlsuv/listener.h>
#include <tlsuv/tls_engine.h>

#include "ant.h"
#include "cfunc.h"
#include "list.h"
#include "internal.h"
#include "ptr.h"
#include "errors.h"
#include "descriptors.h"

#include "gc/modules.h"
#include "sandbox/policy.h"
#include "modules/tls.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/net.h"
#include "modules/stream.h"
#include "modules/timer.h"
#include "silver/call.h"

typedef struct ant_tls_context_wrap_s {
  ant_value_t obj;
  tls_context *ctx;
  tlsuv_private_key_t key;
  tlsuv_certificate_t cert;
  unsigned refs;
  bool closed;
} ant_tls_context_wrap_t;

typedef struct tls_write_req_s {
  uv_write_t req;
  ant_tls_socket_t *socket;
  struct tls_write_req_s *next;
  ant_value_t callback;
  char *data;
  size_t len;
} tls_write_req_t;

struct ant_tls_socket_s {
  ant_t *js;
  ant_value_t obj;
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
  tls_write_req_t *writes;
  ant_tls_server_t *server;
  const char *family;
  ant_tls_socket_t *next;
  ant_tls_socket_t **pprev;
  uint64_t timeout_ms;
  uint64_t bytes_read;
  uint64_t bytes_written;
  bool owns_ctx;
  bool authorized;
  bool connecting;
  bool destroyed;
  bool closing;
  bool had_error;
  bool read_eof;
  bool read_done;
  bool read_paused;
  bool write_ended;
  bool shutting_down;
};

struct ant_tls_server_s {
  ant_t *js;
  ant_value_t obj;
  ant_value_t secure_context;
  ant_tls_context_wrap_t *ctx_wrap;
  tlsuv_listener_t listener;
  char **alpn_protocols;
  int alpn_count;
  int connections;
  ant_tls_server_t *next;
  ant_tls_server_t **pprev;
  bool request_cert;
  bool listening;
  bool closing;
  bool listener_open;
  bool unref;
};

static constexpr uint32_t TLS_CONTEXT_NATIVE_TAG = Ant_NativeTag("TLSC");
static constexpr uint32_t TLS_SOCKET_NATIVE_TAG = Ant_NativeTag("TLSK");
static constexpr uint32_t TLS_SERVER_NATIVE_TAG = Ant_NativeTag("TLSS");

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
  if (socket) Ant_List_Insert(&socket->js->tls_sockets, socket);
}

static void tls_remove_active_socket(ant_tls_socket_t *socket) {
  Ant_List_Remove(socket);
}

static bool tls_emit(ant_t *js, ant_value_t target, const char *event, ant_value_t *args, int nargs) {
  return eventemitter_emit_args(js, target, event, args, nargs);
}

static ant_tls_socket_t *tls_require_socket(ant_t *js, ant_value_t this_val) {
  ant_tls_socket_t *socket = tls_socket_data(this_val);
  if (!socket) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TLS socket");
    return NULL;
  }
  return socket;
}

static ant_value_t tls_make_buffer_chunk(ant_t *js, const char *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(js, len);
  if (!ab) return Ant_Error_CreateOutOfMemory(js);
  if (len > 0 && data) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, js->builtins.buffer_proto);
}

static void tls_socket_sync_state(ant_tls_socket_t *socket) {
  ant_t *js = NULL;
  const char *ready_state = "open";

  if (!socket || !is_object_type(socket->obj)) return;
  js = socket->js;
  if (socket->destroyed) ready_state = "closed";
  else if (socket->connecting) ready_state = "opening";

  js_set(js, socket->obj, "encrypted", js_true);
  js_set(js, socket->obj, "authorized", js_bool(socket->authorized));
  js_set(js, socket->obj, "authorizationError", js_mknull());
  js_set(js, socket->obj, "secureConnecting", js_bool(socket->connecting));
  js_set(js, socket->obj, "connecting", js_bool(socket->connecting));
  
  stream_set_destroyed(js, socket->obj, socket->destroyed);
  stream_set_readable(js, socket->obj, !socket->destroyed);
  
  js_set(js, socket->obj, "writable", js_bool(!socket->destroyed));
  js_set(js, socket->obj, "writableNeedDrain", js_false);
  js_set(js, socket->obj, "readyState", js_mkstr(js, ready_state, strlen(ready_state)));
  js_set(js, socket->obj, "timeout", js_mknum((double)socket->timeout_ms));
  js_set(js, socket->obj, "bytesRead", js_mknum((double)socket->bytes_read));
  js_set(js, socket->obj, "bytesWritten", js_mknum((double)socket->bytes_written));
  js_set(js, socket->obj, "remoteAddress", socket->host ? js_mkstr(js, socket->host, strlen(socket->host)) : js_mkundef());
  js_set(js, socket->obj, "remotePort", socket->port > 0 ? js_mknum((double)socket->port) : js_mkundef());
  js_set(js, socket->obj, "remoteFamily", js_mkstr(js, socket->family, strlen(socket->family)));
}

static void tls_define_default_socket_state(ant_t *js, ant_value_t obj) {
  if (!is_object_type(obj)) return;
  if (vtype(js_get(js, obj, "encrypted")) == kTypeUndefined) js_set(js, obj, "encrypted", js_true);
  if (vtype(js_get(js, obj, "authorized")) == kTypeUndefined) js_set(js, obj, "authorized", js_true);
  if (vtype(js_get(js, obj, "authorizationError")) == kTypeUndefined) js_set(js, obj, "authorizationError", js_mknull());
  if (vtype(js_get(js, obj, "secureConnecting")) == kTypeUndefined) js_set(js, obj, "secureConnecting", js_false);
  if (vtype(js_get(js, obj, "allowHalfOpen")) == kTypeUndefined) js_set(js, obj, "allowHalfOpen", js_false);
}

static void tls_free_alpn(char ***protocols, int *count) {
  if (!*protocols) return;
  for (int i = 0; i < *count; i++) free((*protocols)[i]);
  free(*protocols);
  *protocols = NULL;
  *count = 0;
}

static void tls_server_maybe_release(ant_tls_server_t *server) {
  bool emit_close = server->closing;
  ant_t *js = server->js;

  if (server->listener_open || server->connections > 0) return;
  server->closing = false;
  
  Ant_List_Remove(server);
  if (!emit_close) return;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, server->obj);
  tls_emit(js, server->obj, "close", NULL, 0);
  GC_ROOT_RESTORE(js, root_mark);
}

static void tls_server_socket_closed(ant_tls_server_t *server) {
  server->connections--;
  tls_server_maybe_release(server);
}

static void tls_socket_release(ant_tls_socket_t *socket) {
  ant_tls_server_t *server = socket->server;

  tls_remove_active_socket(socket);
  tls_free_alpn(&socket->alpn_protocols, &socket->alpn_count);

  if (socket->ctx_wrap) tls_context_release(socket->ctx_wrap);
  if (socket->owns_ctx && socket->ctx && socket->ctx->free_ctx)
    socket->ctx->free_ctx(socket->ctx);

  socket->ctx_wrap = NULL;
  socket->ctx = NULL;
  socket->owns_ctx = false;
  socket->secure_context = js_mkundef();
  socket->server = NULL;
  
  if (server) tls_server_socket_closed(server);
}

static void tls_socket_free(ant_tls_socket_t *socket) {
  if (!socket) return;
  tls_socket_release(socket);
  
  if (is_object_type(socket->obj))
    js_clear_native(socket->obj, TLS_SOCKET_NATIVE_TAG);
  
  free(socket->host);
  free(socket->servername);
  free(socket);
}

static void tls_socket_finalize(ant_t *js, ant_object_t *obj) {
  ant_tls_socket_t *socket = tls_socket_data(js_obj_from_ptr(obj));
  if (socket && !Ant_List_IsLinked(socket)) tls_socket_free(socket);
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
  tls_socket_release(socket);
}

static void tls_socket_close(ant_tls_socket_t *socket) {
  if (!socket || socket->closing || socket->destroyed) return;
  socket->closing = true;
  tlsuv_stream_close(&socket->stream, tls_socket_close_cb);
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

  if (vtype(value) == kTypeUndefined || vtype(value) == kTypeNull) return true;
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
  return Ant_Error_Create(socket->js, JS_ERR_TYPE, message ? message : "TLS error");
}

static bool tls_parse_write_args(
  ant_native_params_t,
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

  if (nargs < 1 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) return true;
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

static ant_value_t js_tls_context_close(ant_params_t) {
  ant_tls_context_wrap_t *wrap = tls_context_data(js_getthis(js));
  if (!wrap) return js_getthis(js);
  tls_context_free(wrap);
  js_clear_native(wrap->obj, TLS_CONTEXT_NATIVE_TAG);
  return js_getthis(js);
}

static void tls_init_context_proto(ant_t *js) {
  if (js->builtins.tls_context_proto && js->builtins.tls_context_ctor) return;

  if (!js->builtins.tls_context_proto) {
    js->builtins.tls_context_proto = Ant_Object_CreateBare(js);
    js_set(js, js->builtins.tls_context_proto, "close", Ant_Function_Create(js_tls_context_close));
    js_set_tostringtag(js, js->builtins.tls_context_proto, js_mkstr(js, "SecureContext", 13));
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

static void tls_socket_finish_writes(ant_tls_socket_t *socket) {
  if (!socket || !socket->write_ended || socket->writes) return;
  
  if (!socket->shutting_down) {
    socket->shutting_down = true;
    if (tlsuv_stream_shutdown(&socket->stream) != 0) { tls_socket_close(socket); return; }
  }
  
  if (socket->read_done) tls_socket_close(socket);
}

static void tls_socket_on_read_end(ant_t *js, ant_value_t stream_obj) {
  ant_tls_socket_t *socket = tls_socket_data(stream_obj);
  if (!socket) return;
  
  socket->read_done = true;
  if (socket->shutting_down) tls_socket_close(socket);
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
    if (is_callable(write->callback)) sv_call_value(js, write->callback, js_mkundef(), NULL, 0);
  }

  free(write->data);
  free(write);
  tls_socket_finish_writes(socket);
}

static void tls_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void tls_socket_pause_read(ant_tls_socket_t *socket) {
  if (socket->read_paused || socket->destroyed || socket->closing) return;
  socket->read_paused = true;
  tlsuv_stream_read_stop(&socket->stream);
}

static void tls_socket_on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  tlsuv_stream_t *tls_stream = (tlsuv_stream_t *)stream;
  ant_tls_socket_t *socket = tls_stream ? (ant_tls_socket_t *)tls_stream->data : NULL;
  
  ant_t *js = socket ? socket->js : NULL;
  if (!socket || !js) goto done;

  if (nread > 0) {
    socket->bytes_read += (uint64_t)nread;
    tls_socket_sync_state(socket);
    
    ant_value_t chunk = tls_make_buffer_chunk(js, buf->base, (size_t)nread);
    if (!is_err(chunk)) chunk = stream_readable_push(js, socket->obj, chunk, js_mkundef());
    if (chunk == js_false) tls_socket_pause_read(socket);
    
    if (is_err(chunk)) {
      chunk = js_take_thrown(js, chunk);
      socket->had_error = true;
      tls_emit(js, socket->obj, "error", &chunk, 1);
      tls_socket_close(socket);
    }
  } else if (nread == UV_EOF && !socket->read_eof) {
    socket->read_eof = true;
    stream_readable_end(js, socket->obj);
  } else if (nread < 0 && nread != UV_EOF) {
    ant_value_t err = tls_stream_error(socket, (int)nread, "TLS read failed");
    socket->had_error = true;
    tls_emit(js, socket->obj, "error", &err, 1);
    tls_socket_close(socket);
  }

done:
  free(buf ? buf->base : NULL);
}

static void tls_socket_resume_read(ant_tls_socket_t *socket) {
  if (!socket->read_paused || socket->read_eof || socket->destroyed || socket->closing) return;
  socket->read_paused = false;
  tlsuv_stream_read_start(&socket->stream, tls_alloc_cb, tls_socket_on_read);
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

static ant_value_t js_tls_socket_write(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  const uint8_t *bytes = NULL;
  
  size_t len = 0;
  ant_value_t callback = js_mkundef();
  ant_value_t err = js_mkundef();
  
  tls_write_req_t *write = NULL;
  uv_buf_t buf;
  int rc = 0;

  if (!socket) return Ant_Exception_Current(js);
  if (socket->destroyed || socket->closing) return js_false;
  if (!tls_parse_write_args(js, args, nargs, &bytes, &len, &callback, &err)) return err;
  
  if (len == 0) {
    if (is_callable(callback)) sv_call_value(js, callback, js_mkundef(), NULL, 0);
    return js_true;
  }

  write = calloc(1, sizeof(*write));
  if (!write) return Ant_Error_CreateOutOfMemory(js);
  write->data = malloc(len);
  
  if (!write->data) {
    free(write);
    return Ant_Error_CreateOutOfMemory(js);
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

static ant_value_t js_tls_socket_end(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  ant_value_t result = js_getthis(js);

  if (!socket) return Ant_Exception_Current(js);
  if (nargs > 0 && vtype(args[0]) != kTypeUndefined && vtype(args[0]) != kTypeNull) {
    result = js_tls_socket_write(js, args, nargs, js_mkundef());
    if (is_err(result)) return result;
  }
  
  socket->write_ended = true;
  tls_socket_finish_writes(socket);
  
  return js_getthis(js);
}

static ant_value_t js_tls_socket_destroy(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  
  if (!socket) return Ant_Exception_Current(js);
  if (socket->destroyed || socket->closing) return js_getthis(js);

  if (nargs > 0 && vtype(args[0]) != kTypeUndefined && vtype(args[0]) != kTypeNull) {
    ant_value_t err = args[0];
    socket->had_error = true;
    tls_emit(js, socket->obj, "error", &err, 1);
  }

  tls_socket_close(socket);
  socket->destroyed = true;
  socket->connecting = false;
  tls_socket_sync_state(socket);
  
  return js_getthis(js);
}

static ant_value_t js_tls_socket_pause(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return Ant_Exception_Current(js);
  return stream_readable_pause(js, socket->obj);
}

static ant_value_t js_tls_socket_resume(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return Ant_Exception_Current(js);
  tls_socket_resume_read(socket);
  return stream_readable_resume(js, socket->obj);
}

static ant_value_t js_tls_socket__read(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return Ant_Exception_Current(js);
  tls_socket_resume_read(socket);
  return js_mkundef();
}

static ant_value_t js_tls_socket_setNoDelay(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  bool enable = nargs == 0 || js_truthy(js, args[0]);
  if (!socket) return Ant_Exception_Current(js);
  if (!socket->destroyed && !socket->closing) tlsuv_stream_nodelay(&socket->stream, enable ? 1 : 0);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_setKeepAlive(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  bool enable = nargs > 0 && js_truthy(js, args[0]);
  unsigned int delay = nargs > 1 && vtype(args[1]) == kTypeNumber ? (unsigned int)(js_getnum(args[1]) / 1000.0) : 0;
  if (!socket) return Ant_Exception_Current(js);
  if (!socket->destroyed && !socket->closing) tlsuv_stream_keepalive(&socket->stream, enable ? 1 : 0, delay);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_setTimeout(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return Ant_Exception_Current(js);
  socket->timeout_ms = nargs > 0 && vtype(args[0]) == kTypeNumber && js_getnum(args[0]) > 0 ? (uint64_t)js_getnum(args[0]) : 0;
  tls_socket_sync_state(socket);
  if (nargs > 1 && is_callable(args[1]))
    eventemitter_add_listener(js, socket->obj, "timeout", args[1], true);
  return js_getthis(js);
}

static ant_value_t tls_sockaddr_object(ant_t *js, const struct sockaddr *addr) {
  char host[INET6_ADDRSTRLEN] = {0};
  ant_value_t out = Ant_Object_CreateBare(js);
  bool v6 = addr->sa_family == AF_INET6;
  int port = v6
    ? ntohs(((const struct sockaddr_in6 *)addr)->sin6_port)
    : ntohs(((const struct sockaddr_in *)addr)->sin_port);

  uv_ip_name(addr, host, sizeof(host));
  js_set(js, out, "address", js_mkstr(js, host, strlen(host)));
  js_set(js, out, "family", js_mkstr(js, v6 ? "IPv6" : "IPv4", 4));
  js_set(js, out, "port", js_mknum((double)port));
  return out;
}

static ant_value_t js_tls_socket_address(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  struct sockaddr_storage addr;
  int len = sizeof(addr);

  if (!socket) return Ant_Exception_Current(js);
  if (socket->destroyed || socket->closing || socket->connecting) return Ant_Object_CreateBare(js);
  if (tlsuv_stream_sockname(&socket->stream, (struct sockaddr *)&addr, &len) != 0) return Ant_Object_CreateBare(js);
  return tls_sockaddr_object(js, (struct sockaddr *)&addr);
}

static ant_value_t js_tls_socket_ref(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return Ant_Exception_Current(js);
  if (
    uv_handle_get_type((uv_handle_t *)&socket->stream.watcher) != UV_UNKNOWN_HANDLE &&
    !uv_is_closing((uv_handle_t *)&socket->stream.watcher)
  ) uv_ref((uv_handle_t *)&socket->stream.watcher);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_unref(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  if (!socket) return Ant_Exception_Current(js);
  if (
    uv_handle_get_type((uv_handle_t *)&socket->stream.watcher) != UV_UNKNOWN_HANDLE &&
    !uv_is_closing((uv_handle_t *)&socket->stream.watcher)
  ) uv_unref((uv_handle_t *)&socket->stream.watcher);
  return js_getthis(js);
}

static ant_value_t js_tls_socket_cork(ant_params_t) {
  return js_getthis(js);
}

static ant_value_t js_tls_socket_uncork(ant_params_t) {
  return js_getthis(js);
}

static ant_value_t js_tls_socket_getProtocol(ant_params_t) {
  ant_tls_socket_t *socket = tls_require_socket(js, js_getthis(js));
  const char *protocol = NULL;
  if (!socket) return Ant_Exception_Current(js);
  if (socket->destroyed || socket->closing) return js_mknull();
  protocol = tlsuv_stream_get_protocol(&socket->stream);
  return protocol && *protocol ? js_mkstr(js, protocol, strlen(protocol)) : js_mkundef();
}

static ant_value_t js_tls_socket_getCipher(ant_params_t) {
  return js_mkundef();
}

static ant_value_t js_tls_socket_getSession(ant_params_t) {
  return js_mkundef();
}

static ant_value_t js_tls_socket_getPeerCertificate(ant_params_t) {
  return Ant_Object_CreateBare(js);
}

static ant_value_t js_tls_socket_renegotiate(ant_params_t) {
  ant_value_t callback = js_mkundef();
  if (nargs > 1 && is_callable(args[1])) callback = args[1];
  else if (nargs > 0 && is_callable(args[0])) callback = args[0];
  if (is_callable(callback)) {
    ant_value_t cb_args[] = {js_mknull()};
    sv_call_value(js, callback, js_getthis(js), cb_args, 1);
  }
  return js_true;
}

static ant_value_t js_tls_socket_ctor(ant_params_t) {
  ant_value_t existing = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t obj = 0;
  ant_value_t proto = 0;

  tls_init_socket_proto(js);
  if (tls_socket_data(existing)) {
    tls_define_default_socket_state(js, existing);
    stream_init_duplex_object(js, existing, js_mkundef());
    js_set_proto_wb(js, existing, js->builtins.tls_socket_proto);
    return existing;
  }

  obj = Ant_Object_CreateBare(js);
  proto = js_instance_proto_from_new_target(js, js->builtins.tls_socket_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  stream_init_duplex_object(js, obj, js_mkundef());
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

  if (js->builtins.tls_socket_proto && js->builtins.tls_socket_ctor) return;

  if (!js->builtins.tls_socket_proto) {
    net = net_library(js);
    net_socket_ctor = js_get(js, net, "Socket");
    net_socket_proto = js_get(js, net_socket_ctor, "prototype");

    if (!is_object_type(net_socket_proto)) {
      events = events_library(js);
      ee_ctor = js_get(js, events, "EventEmitter");
      ee_proto = js_get(js, ee_ctor, "prototype");
      net_socket_proto = ee_proto;
    }

    js->builtins.tls_socket_proto = Ant_Object_CreateBare(js);
    if (is_object_type(net_socket_proto)) js_set_proto_init(js->builtins.tls_socket_proto, net_socket_proto);
    js_set(js, js->builtins.tls_socket_proto, "_read", Ant_Function_CreateWithArity(js_tls_socket__read, 1));
    js_set(js, js->builtins.tls_socket_proto, "write", Ant_Function_Create(js_tls_socket_write));
    js_set(js, js->builtins.tls_socket_proto, "end", Ant_Function_Create(js_tls_socket_end));
    js_set(js, js->builtins.tls_socket_proto, "destroy", Ant_Function_Create(js_tls_socket_destroy));
    js_set(js, js->builtins.tls_socket_proto, "pause", Ant_Function_Create(js_tls_socket_pause));
    js_set(js, js->builtins.tls_socket_proto, "resume", Ant_Function_Create(js_tls_socket_resume));
    js_set(js, js->builtins.tls_socket_proto, "setNoDelay", Ant_Function_Create(js_tls_socket_setNoDelay));
    js_set(js, js->builtins.tls_socket_proto, "setKeepAlive", Ant_Function_Create(js_tls_socket_setKeepAlive));
    js_set(js, js->builtins.tls_socket_proto, "setTimeout", Ant_Function_Create(js_tls_socket_setTimeout));
    js_set(js, js->builtins.tls_socket_proto, "address", Ant_Function_Create(js_tls_socket_address));
    js_set(js, js->builtins.tls_socket_proto, "ref", Ant_Function_Create(js_tls_socket_ref));
    js_set(js, js->builtins.tls_socket_proto, "unref", Ant_Function_Create(js_tls_socket_unref));
    js_set(js, js->builtins.tls_socket_proto, "cork", Ant_Function_Create(js_tls_socket_cork));
    js_set(js, js->builtins.tls_socket_proto, "uncork", Ant_Function_Create(js_tls_socket_uncork));
    js_set(js, js->builtins.tls_socket_proto, "getProtocol", Ant_Function_Create(js_tls_socket_getProtocol));
    js_set(js, js->builtins.tls_socket_proto, "getCipher", Ant_Function_Create(js_tls_socket_getCipher));
    js_set(js, js->builtins.tls_socket_proto, "getSession", Ant_Function_Create(js_tls_socket_getSession));
    js_set(js, js->builtins.tls_socket_proto, "getPeerCertificate", Ant_Function_Create(js_tls_socket_getPeerCertificate));
    js_set(js, js->builtins.tls_socket_proto, "renegotiate", Ant_Function_Create(js_tls_socket_renegotiate));
    js_set_tostringtag(js, js->builtins.tls_socket_proto, js_mkstr(js, "TLSSocket", 9));
  }

  if (!js->builtins.tls_socket_ctor)
    js->builtins.tls_socket_ctor = Ant_Function_CreateConstructor(js, js_tls_socket_ctor, js->builtins.tls_socket_proto, "TLSSocket", 2);
}

static ant_value_t js_tls_create_context(ant_params_t) {
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

  if (vtype(options) != kTypeUndefined && vtype(options) != kTypeNull && vtype(options) != kTypeObject)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TLS context options must be an object");

  tls_init_context_proto(js);

  if (vtype(options) == kTypeObject) {
    ca_v = js_get(js, options, "ca");
    key_v = js_get(js, options, "key");
    cert_v = js_get(js, options, "cert");
    partial_v = js_get(js, options, "allowPartialChain");
  }

  if (!tls_value_bytes(js, ca_v, &ca, &ca_len, &error)) return error;
  if (!tls_value_bytes(js, key_v, &key_data, &key_len, &error)) return error;
  if (!tls_value_bytes(js, cert_v, &cert_data, &cert_len, &error)) return error;

  if (cert_data && !key_data)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TLS certificate requires a private key");
  if (key_data && !cert_data)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TLS private key requires a certificate");

  wrap = calloc(1, sizeof(*wrap));
  if (!wrap) return Ant_Error_CreateOutOfMemory(js);

  wrap->ctx = default_tls_context();
  if (!wrap->ctx) {
    free(wrap);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to create TLS context");
  }

  if (ca && wrap->ctx->set_ca_bundle(wrap->ctx, ca, ca_len) != 0) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "Failed to load TLS CA bundle");
    goto fail;
  }

  if (wrap->ctx->allow_partial_chain && js_truthy(js, partial_v) &&
      wrap->ctx->allow_partial_chain(wrap->ctx, 1) != 0) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "Failed to enable partial chain verification");
    goto fail;
  }

  if (!key_data) goto done;

  if (!wrap->ctx->load_key) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support loading private keys");
    goto fail;
  }

  if (wrap->ctx->load_key(&wrap->key, key_data, key_len) != 0) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "Failed to load TLS private key: expected a PEM-encoded key");
    goto fail;
  }

  if (!wrap->ctx->load_cert) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support loading certificates");
    goto fail;
  }

  if (wrap->ctx->load_cert(&wrap->cert, cert_data, cert_len) != 0) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "Failed to load TLS certificate: expected a PEM-encoded certificate");
    goto fail;
  }

  if (!wrap->ctx->set_own_cert) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support own certificate configuration");
    goto fail;
  }

  if (wrap->ctx->set_own_cert(wrap->ctx, wrap->key, wrap->cert) != 0) {
    error = js_mkerr_typed(js, JS_ERR_TYPE, "TLS certificate does not match the private key");
    goto fail;
  }

done:
  obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, js->builtins.tls_context_proto);
  wrap->obj = obj;
  js_set_native(obj, wrap, TLS_CONTEXT_NATIVE_TAG);

  return obj;

fail:
  tls_context_free(wrap);
  free(wrap);
  return error;
}

static ant_value_t js_tls_is_context(ant_params_t) {
  ant_tls_context_wrap_t *wrap = nargs > 0 ? tls_context_data(args[0]) : NULL;
  return js_bool(wrap && !wrap->closed && wrap->ctx);
}

static ant_value_t js_tls_secure_context_ctor(ant_params_t) {
  return js_tls_create_context(js, args, nargs, js_mkundef());
}

static ant_value_t js_tls_set_config_path(ant_params_t) {
  ant_value_t str_value = js_mkundef();
  const char *path = NULL;
  int rc = 0;

  if (nargs < 1 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull) {
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

static bool tls_parse_alpn(ant_t *js, ant_value_t protocols, char ***out, int *count) {
  ant_offset_t len = 0;

  if (vtype(protocols) != kTypeArray) return true;
  len = js_arr_len(js, protocols);
  if (len <= 0) return true;

  *out = calloc((size_t)len, sizeof(char *));
  if (!*out) return false;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = js_arr_get(js, protocols, i);
    ant_value_t str_val = js_tostring_val(js, item);
    const char *str = NULL;
    if (is_err(str_val)) return false;
    str = js_getstr(js, str_val, NULL);
    if (!str) return false;
    (*out)[*count] = strdup(str);
    if (!(*out)[*count]) return false;
    (*count)++;
  }

  return true;
}

static void tls_socket_attach_object(ant_t *js, ant_tls_socket_t *socket) {
  ant_value_t obj = Ant_Object_CreateBare(js);

  js_set_proto_init(obj, js->builtins.tls_socket_proto);
  socket->obj = obj;

  js_set_native(obj, socket, TLS_SOCKET_NATIVE_TAG);
  js_set_finalizer(obj, tls_socket_finalize);
  stream_init_duplex_object(js, obj, js_mkundef());
  stream_set_end_hook(obj, tls_socket_on_read_end);
  tls_define_default_socket_state(js, obj);

  js_set(js, obj, "localAddress", js_mkundef());
  js_set(js, obj, "localPort", js_mkundef());
  js_set(js, obj, "localFamily", js_mkundef());
  js_set(js, obj, "alpnProtocol", js_false);

  tls_socket_sync_state(socket);
  tls_add_active_socket(socket);
}

static void tls_copy_connect_option(ant_t *js, ant_value_t dst, ant_value_t src, const char *key) {
  ant_value_t value = js_get(js, src, key);
  if (vtype(value) != kTypeUndefined) js_set(js, dst, key, value);
}

static void tls_copy_connect_options(ant_t *js, ant_value_t dst, ant_value_t src) {
  if (vtype(src) != kTypeObject) return;
  tls_copy_connect_option(js, dst, src, "host");
  tls_copy_connect_option(js, dst, src, "hostname");
  tls_copy_connect_option(js, dst, src, "port");
  tls_copy_connect_option(js, dst, src, "servername");
  tls_copy_connect_option(js, dst, src, "secureContext");
  tls_copy_connect_option(js, dst, src, "ca");
  tls_copy_connect_option(js, dst, src, "key");
  tls_copy_connect_option(js, dst, src, "cert");
  tls_copy_connect_option(js, dst, src, "allowPartialChain");
  tls_copy_connect_option(js, dst, src, "ALPNProtocols");
}

static ant_value_t tls_normalize_connect_options(
  ant_native_params_t,
  ant_value_t *callback_out
) {
  int argc = nargs;
  ant_value_t options = Ant_Object_CreateBare(js);

  if (callback_out) *callback_out = js_mkundef();
  if (argc > 0 && is_callable(args[argc - 1])) {
    if (callback_out) *callback_out = args[argc - 1];
    argc--;
  }

  if (argc == 1 && vtype(args[0]) == kTypeObject) return args[0];
  if (argc > 0 && net_value_is_port(js, args[0])) js_set(js, options, "port", args[0]);
  if (argc > 1 && vtype(args[1]) == kTypeString) js_set(js, options, "host", args[1]);
  if (argc > 1 && vtype(args[1]) == kTypeObject) tls_copy_connect_options(js, options, args[1]);
  if (argc > 2 && vtype(args[2]) == kTypeObject) tls_copy_connect_options(js, options, args[2]);
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

  if (vtype(options) != kTypeObject)
    return js_mkerr_typed(js, JS_ERR_TYPE, "tls.connect requires an options object");

  static const char *const string_options[] = { "host", "hostname", "servername" };
  for (size_t i = 0; i < sizeof(string_options) / sizeof(string_options[0]); i++) {
    ant_value_t nul = Ant_Error_CheckNoNullBytes(js, string_options[i], js_get(js, options, string_options[i]));
    if (is_err(nul)) return nul;
  }

  value = js_get(js, options, "host");
  if (vtype(value) == kTypeString) host = js_getstr(js, value, NULL);
  
  value = js_get(js, options, "hostname");
  if (vtype(value) == kTypeString) host = js_getstr(js, value, NULL);
  
  value = js_get(js, options, "port");
  if (vtype(value) != kTypeUndefined) {
    ant_value_t port_error = js_mkundef();
    if (!net_parse_port(js, value, "Port", &port, &port_error)) return port_error;
  }
  
  value = js_get(js, options, "servername");
  if (vtype(value) == kTypeString) servername = js_getstr(js, value, NULL);
  
  if (!servername || !*servername) servername = host;
  tls_init_socket_proto(js);

  socket = calloc(1, sizeof(*socket));
  if (!socket) return Ant_Error_CreateOutOfMemory(js);

  socket->js = js;
  socket->secure_context = js_mkundef();
  socket->host = strdup(host ? host : "localhost");
  socket->servername = strdup(servername ? servername : (host ? host : "localhost"));
  socket->port = port > 0 ? port : 443;
  socket->family = "IPv4";
  socket->authorized = true;
  socket->connecting = true;
  
  if (!socket->host || !socket->servername) {
    tls_socket_free(socket);
    return Ant_Error_CreateOutOfMemory(js);
  }

  value = js_get(js, options, "secureContext");
  ctx_wrap = tls_context_data(value);
  
  if (!ctx_wrap && (
    vtype(js_get(js, options, "ca")) != kTypeUndefined  ||
    vtype(js_get(js, options, "key")) != kTypeUndefined ||
    vtype(js_get(js, options, "cert")) != kTypeUndefined)) {
    value = js_tls_create_context(js, &options, 1, js_mkundef());
    
    if (is_err(value)) {
      tls_socket_free(socket);
      return value;
    }
    
    ctx_wrap = tls_context_data(value);
  }
  
  if (ctx_wrap && !ctx_wrap->closed && ctx_wrap->ctx) {
    socket->ctx = ctx_wrap->ctx;
    socket->ctx_wrap = ctx_wrap;
    socket->secure_context = value;
    socket->owns_ctx = false;
    tls_context_retain(ctx_wrap);
  } else {
    socket->ctx = default_tls_context();
    socket->owns_ctx = true;
    
    if (!socket->ctx) {
      tls_socket_free(socket);
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to create TLS context");
    }
  }

  value = js_get(js, options, "ALPNProtocols");
  if (!tls_parse_alpn(js, value, &socket->alpn_protocols, &socket->alpn_count)) {
    tls_socket_free(socket);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to configure TLS ALPN protocols");
  }

  tls_socket_attach_object(js, socket);
  obj = socket->obj;

  if (is_callable(callback)) eventemitter_add_listener(js, obj, "secureConnect", callback, true);
  tlsuv_stream_init(js->loop, &socket->stream, socket->ctx);
  socket->stream.data = socket;
  socket->connect_req.data = socket;
  
  if (socket->alpn_count > 0)
    tlsuv_stream_set_protocols(&socket->stream, socket->alpn_count, (const char **)socket->alpn_protocols);

  rc = tlsuv_stream_connect(&socket->connect_req, &socket->stream, socket->host, socket->port, tls_socket_on_connect);
  if (rc == 0) tlsuv_stream_set_hostname(&socket->stream, socket->servername);
  
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

static ant_value_t js_tls_connect(ant_params_t) {
  ant_value_t callback = js_mkundef();
  ant_value_t options = tls_normalize_connect_options(js, args, nargs, &callback);
  return js_tls_connect_options(js, options, callback);
}

static ant_tls_server_t *tls_server_data(ant_value_t value) {
  return (ant_tls_server_t *)js_get_native(value, TLS_SERVER_NATIVE_TAG);
}

static ant_tls_server_t *tls_require_server(ant_t *js, ant_value_t this_val) {
  ant_tls_server_t *server = tls_server_data(this_val);
  if (!server) js_mkerr_typed(js, JS_ERR_TYPE, "Invalid tls.Server");
  return server;
}

static void tls_server_sync_state(ant_tls_server_t *server) {
  if (is_object_type(server->obj))
    js_set(server->js, server->obj, "listening", js_bool(server->listening));
}

static void tls_server_listener_closed(uv_handle_t *handle) {
  ant_tls_server_t *server = tlsuv_listener_get_data((tlsuv_listener_t *)handle);
  server->listener_open = false;
  tls_server_maybe_release(server);
}

static void tls_server_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_tls_server_t *server = tls_server_data(value);

  if (!server || Ant_List_IsLinked(server)) return;
  js_clear_native(value, TLS_SERVER_NATIVE_TAG);
  tls_free_alpn(&server->alpn_protocols, &server->alpn_count);
  tls_context_release(server->ctx_wrap);
  free(server);
}

static tlsuv_stream_t *tls_server_on_accept(tlsuv_listener_t *l, const struct sockaddr *peer, int status) {
  ant_tls_server_t *server = tlsuv_listener_get_data(l);
  ant_t *js = server->js;
  
  ant_tls_socket_t *socket = NULL;
  char host[INET6_ADDRSTRLEN] = {0};

  if (status != 0) {
    if (status != UV_EMFILE && status != UV_ENFILE) {
      ant_value_t err = Ant_Error_Create(js, JS_ERR_TYPE, uv_strerror(status));
      tls_emit(js, server->obj, "error", &err, 1);
    }
    
    return NULL;
  }

  socket = calloc(1, sizeof(*socket));
  if (!socket) return NULL;

  uv_ip_name(peer, host, sizeof(host));
  socket->js = js;
  socket->secure_context = js_mkundef();
  socket->host = strdup(host);
  socket->family = peer->sa_family == AF_INET6 ? "IPv6" : "IPv4";
  
  socket->port = peer->sa_family == AF_INET6
    ? ntohs(((const struct sockaddr_in6 *)peer)->sin6_port)
    : ntohs(((const struct sockaddr_in *)peer)->sin_port);
  
  socket->ctx = server->ctx_wrap->ctx;
  socket->ctx_wrap = server->ctx_wrap;
  socket->authorized = server->request_cert;
  socket->connecting = true;
  socket->server = server;
  
  tls_context_retain(server->ctx_wrap);
  server->connections++;

  if (!socket->host) {
    tls_socket_free(socket);
    return NULL;
  }

  tls_socket_attach_object(js, socket);
  socket->stream.data = socket;
  return &socket->stream;
}

static void tls_server_on_handshake(tlsuv_stream_t *clt, int status) {
  ant_tls_socket_t *socket = (ant_tls_socket_t *)clt->data;
  ant_tls_server_t *server = socket->server;
  
  ant_t *js = socket->js;
  const char *protocol = NULL;

  socket->connecting = false;
  if (status != 0) {
    ant_value_t argv[2] = { tls_stream_error(socket, status, "TLS handshake failed"), socket->obj };
    socket->had_error = true;
    tls_socket_sync_state(socket);
    if (server) tls_emit(js, server->obj, "tlsClientError", argv, 2);
    tls_socket_close(socket);
    return;
  }

  protocol = tlsuv_stream_get_protocol(&socket->stream);
  if (protocol && *protocol)
    js_set(js, socket->obj, "alpnProtocol", js_mkstr(js, protocol, strlen(protocol)));

  tlsuv_stream_read_start(&socket->stream, tls_alloc_cb, tls_socket_on_read);
  tls_socket_sync_state(socket);
  tls_emit(js, socket->obj, "secure", NULL, 0);
  if (server) tls_emit(js, server->obj, "secureConnection", &socket->obj, 1);
}

static ant_value_t tls_server_create_context(ant_t *js, ant_value_t options, bool *request_cert) {
  ant_value_t value = 0;
  ant_value_t ctx_options = 0;

  *request_cert = false;
  if (vtype(options) != kTypeObject) return js_tls_create_context(js, NULL, 0, js_mkundef());

  *request_cert = js_truthy(js, js_get(js, options, "requestCert"));
  value = js_get(js, options, "secureContext");
  if (tls_context_data(value)) return value;

  if (*request_cert) {
    value = js_get(js, options, "rejectUnauthorized");
    if (vtype(value) != kTypeUndefined && !js_truthy(js, value)) return js_mkerr_typed(
      js, JS_ERR_TYPE, "tls.Server does not support requestCert with rejectUnauthorized: false"
    );
    if (vtype(js_get(js, options, "ca")) == kTypeUndefined)
      return js_mkerr_typed(js, JS_ERR_TYPE, "tls.Server requestCert requires a ca option");
  }

  ctx_options = Ant_Object_CreateBare(js);
  tls_copy_connect_option(js, ctx_options, options, "key");
  tls_copy_connect_option(js, ctx_options, options, "cert");
  tls_copy_connect_option(js, ctx_options, options, "allowPartialChain");
  
  if (*request_cert) tls_copy_connect_option(js, ctx_options, options, "ca");
  return js_tls_create_context(js, &ctx_options, 1, js_mkundef());
}

static ant_value_t tls_server_create(ant_t *js, ant_value_t options, ant_value_t listener, ant_value_t new_target) {
  ant_tls_server_t *server = NULL;
  ant_tls_context_wrap_t *wrap = NULL;
  
  ant_value_t context = 0;
  ant_value_t proto = 0;
  ant_value_t obj = 0;
  bool request_cert = false;

  if (vtype(options) != kTypeUndefined && vtype(options) != kTypeNull && vtype(options) != kTypeObject)
    return js_mkerr_typed(js, JS_ERR_TYPE, "tls.Server options must be an object");

  context = tls_server_create_context(js, options, &request_cert);
  if (is_err(context)) return context;
  wrap = tls_context_data(context);
  if (!wrap || wrap->closed || !wrap->ctx) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TLS secure context");

  server = calloc(1, sizeof(*server));
  if (!server) return Ant_Error_CreateOutOfMemory(js);

  if (vtype(options) == kTypeObject &&
      !tls_parse_alpn(js, js_get(js, options, "ALPNProtocols"), &server->alpn_protocols, &server->alpn_count)) {
    tls_free_alpn(&server->alpn_protocols, &server->alpn_count);
    free(server);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to configure TLS ALPN protocols");
  }

  tls_context_retain(wrap);
  if (!tls_context_data(vtype(options) == kTypeObject ? js_get(js, options, "secureContext") : js_mkundef()))
    tls_context_free(wrap);

  obj = Ant_Object_CreateBare(js);
  proto = js_instance_proto_from_new_target(js, js->builtins.tls_server_proto, new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  server->js = js;
  server->obj = obj;
  server->ctx_wrap = wrap;
  server->request_cert = request_cert;
  
  js_set_native(obj, server, TLS_SERVER_NATIVE_TAG);
  js_set_finalizer(obj, tls_server_finalize);
  
  js_set(js, obj, "maxConnections", js_mknum(0));
  tls_server_sync_state(server);

  if (is_callable(listener)) eventemitter_add_listener(js, obj, "secureConnection", listener, false);
  return obj;
}

static ant_value_t js_tls_server_ctor(ant_params_t) {
  ant_value_t options = nargs > 0 && !is_callable(args[0]) ? args[0] : js_mkundef();
  ant_value_t listener = nargs > 0 && is_callable(args[0]) ? args[0] : (nargs > 1 ? args[1] : js_mkundef());
  return tls_server_create(js, options, listener, call_new_target);
}

static ant_value_t js_tls_create_server(ant_params_t) {
  return js_tls_server_ctor(js, args, nargs, js_mkundef());
}

static int tls_server_bind(ant_tls_server_t *server, const char *host, int port) {
  struct sockaddr_storage addr;
  struct addrinfo hints = {0};
  struct addrinfo *res = NULL;
  int rc = 0;

  memset(&addr, 0, sizeof(addr));
  if (!host || !*host) {
    uv_ip6_addr("::", port, (struct sockaddr_in6 *)&addr);
    if (tlsuv_listener_bind(&server->listener, (struct sockaddr *)&addr, 0) == 0) return 0;
    
    uv_ip4_addr("0.0.0.0", port, (struct sockaddr_in *)&addr);
    return tlsuv_listener_bind(&server->listener, (struct sockaddr *)&addr, 0);
  }

  if (uv_ip4_addr(host, port, (struct sockaddr_in *)&addr) == 0 ||
      uv_ip6_addr(host, port, (struct sockaddr_in6 *)&addr) == 0)
    return tlsuv_listener_bind(&server->listener, (struct sockaddr *)&addr, 0);

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return UV_EAI_NONAME;

  memcpy(&addr, res->ai_addr, res->ai_addrlen);
  if (addr.ss_family == AF_INET6) ((struct sockaddr_in6 *)&addr)->sin6_port = htons((uint16_t)port);
  else ((struct sockaddr_in *)&addr)->sin_port = htons((uint16_t)port);
  freeaddrinfo(res);

  rc = tlsuv_listener_bind(&server->listener, (struct sockaddr *)&addr, 0);
  return rc;
}

static ant_value_t tls_server_emit_listening(ant_params_t) {
  ant_value_t obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_tls_server_t *server = tls_server_data(obj);
  if (server && server->listening) tls_emit(js, obj, "listening", NULL, 0);
  return js_mkundef();
}

static ant_value_t js_tls_server_listen(ant_params_t) {
  ant_tls_server_t *server = tls_require_server(js, js_getthis(js));
  net_listen_args_t parsed;
  int rc = 0;

  if (!server) return Ant_Exception_Current(js);
  if (server->listening || server->listener_open)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Server is already listening");

  if (!net_parse_listen_args(js, args, nargs, &parsed)) return parsed.error;
  if (parsed.path) return js_mkerr_typed(js, JS_ERR_TYPE, "tls.Server cannot listen on a path");

  ant_value_t callback = parsed.callback;
  const char *host = parsed.host;
  
  int port = parsed.port;
  int backlog = parsed.backlog > 0 ? parsed.backlog : 511;

  if (port == 0 && ant_sandbox_policy_forward_restricted()) return js_mkerr_typed(
    js, JS_ERR_TYPE | JS_ERR_NO_STACK,
    "sandbox listen on port 0 is not supported; use a fixed port with --forward <port>"
  );

  if (!ant_sandbox_policy_port_forwarded(port)) return js_mkerr_typed(
    js, JS_ERR_TYPE | JS_ERR_NO_STACK,
    "sandbox listen on port %d requires --forward %d or --forward <host>:%d",
    port, port, port
  );

  if (!server->ctx_wrap->ctx)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TLS secure context is closed");

  tlsuv_listener_set_data(&server->listener, server);
  rc = tlsuv_listener_init(js->loop, &server->listener, server->ctx_wrap->ctx);
  if (rc == UV_ENOTSUP) return js_mkerr_typed(js, JS_ERR_TYPE, "TLS engine does not support servers");
  if (rc != 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));

  server->listener_open = true;
  Ant_List_Insert(&js->tls_servers, server);

  if (server->alpn_count > 0)
    rc = tlsuv_listener_set_protocols(&server->listener, server->alpn_count, (const char **)server->alpn_protocols);
  if (rc == 0) rc = tls_server_bind(server, host, port);
  if (rc == 0) rc = tlsuv_listener_start_listen(&server->listener, backlog, tls_server_on_accept, tls_server_on_handshake);

  if (rc != 0) {
    tlsuv_listener_close(&server->listener, tls_server_listener_closed);
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s", uv_strerror(rc));
  }

  if (server->unref) uv_unref((uv_handle_t *)&server->listener);
  server->listening = true;
  server->closing = false;
  tls_server_sync_state(server);

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, callback);
  
  if (is_callable(callback)) eventemitter_add_listener(js, server->obj, "listening", callback, true);
  ant_value_t emit = js_make_data_cfunc(js, server->obj, tls_server_emit_listening);
  
  if (!is_err(emit)) queue_next_tick(js, emit);
  GC_ROOT_RESTORE(js, root_mark);

  return js_getthis(js);
}

static ant_value_t js_tls_server_close(ant_params_t) {
  ant_tls_server_t *server = tls_require_server(js, js_getthis(js));
  ant_value_t callback = nargs > 0 && is_callable(args[0]) ? args[0] : js_mkundef();

  if (!server) return Ant_Exception_Current(js);
  if (!server->listening) {
    if (is_callable(callback)) {
      ant_value_t err = Ant_Error_Create(js, JS_ERR_TYPE, "Server is not running.");
      js_set(js, err, "code", js_mkstr(js, "ERR_SERVER_NOT_RUNNING", 22));
      sv_call_value(js, callback, js_mkundef(), &err, 1);
    }
    return js_getthis(js);
  }

  if (is_callable(callback)) eventemitter_add_listener(js, server->obj, "close", callback, true);
  server->listening = false;
  server->closing = true;
  
  tls_server_sync_state(server);
  tlsuv_listener_close(&server->listener, tls_server_listener_closed);
  
  return js_getthis(js);
}

static ant_value_t js_tls_server_address(ant_params_t) {
  ant_tls_server_t *server = tls_require_server(js, js_getthis(js));
  struct sockaddr_storage addr;
  int len = sizeof(addr);

  if (!server) return Ant_Exception_Current(js);
  if (!server->listening) return js_mknull();
  if (tlsuv_listener_getsockname(&server->listener, (struct sockaddr *)&addr, &len) != 0) return js_mknull();
  
  return tls_sockaddr_object(js, (struct sockaddr *)&addr);
}

static ant_value_t js_tls_server_getConnections(ant_params_t) {
  ant_tls_server_t *server = tls_require_server(js, js_getthis(js));
  ant_value_t argv[2] = { js_mknull(), js_mknum(0) };

  if (!server) return Ant_Exception_Current(js);
  argv[1] = js_mknum((double)server->connections);
  if (nargs > 0 && is_callable(args[0])) sv_call_value(js, args[0], js_mkundef(), argv, 2);
  return js_getthis(js);
}

static ant_value_t tls_server_set_ref(ant_t *js, bool ref) {
  ant_tls_server_t *server = tls_require_server(js, js_getthis(js));

  if (!server) return Ant_Exception_Current(js);
  server->unref = !ref;
  
  if (server->listening && server->listener.bound) {
    if (ref) uv_ref((uv_handle_t *)&server->listener);
    else uv_unref((uv_handle_t *)&server->listener);
  }
  
  return js_getthis(js);
}

static ant_value_t js_tls_server_ref(ant_params_t) {
  return tls_server_set_ref(js, true);
}

static ant_value_t js_tls_server_unref(ant_params_t) {
  return tls_server_set_ref(js, false);
}

static void tls_init_server_proto(ant_t *js) {
  ant_value_t net = 0;
  ant_value_t net_server_proto = 0;
  ant_value_t proto = 0;

  if (js->builtins.tls_server_proto && js->builtins.tls_server_ctor) return;

  net = net_library(js);
  net_server_proto = js_get(js, js_get(js, net, "Server"), "prototype");

  proto = Ant_Object_CreateBare(js);
  if (is_object_type(net_server_proto)) js_set_proto_init(proto, net_server_proto);
  js_set(js, proto, "listen", Ant_Function_Create(js_tls_server_listen));
  js_set(js, proto, "close", Ant_Function_CreateWithArity(js_tls_server_close, 1));
  js_set(js, proto, "address", Ant_Function_Create(js_tls_server_address));
  js_set(js, proto, "getConnections", Ant_Function_CreateWithArity(js_tls_server_getConnections, 1));
  js_set(js, proto, "ref", Ant_Function_Create(js_tls_server_ref));
  js_set(js, proto, "unref", Ant_Function_Create(js_tls_server_unref));
  js_set_tostringtag(js, proto, js_mkstr(js, "Server", 6));

  js->builtins.tls_server_proto = proto;
  js->builtins.tls_server_ctor = Ant_Function_CreateConstructor(js, js_tls_server_ctor, proto, "Server", 2);
}

void cleanup_tls_module(ant_t *js) {
  for (ant_tls_server_t *server = js->tls_servers; server; server = server->next) {
    if (!server->listener_open || server->listener.closing) continue;
    server->listening = false;
    tlsuv_listener_close(&server->listener, tls_server_listener_closed);
  }

  for (ant_tls_socket_t *socket = js->tls_sockets; socket; socket = socket->next)
    tls_socket_close(socket);
}

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} tls_strbuf_t;

static void tls_strbuf_append(tls_strbuf_t *buf, const char *data, size_t len) {
  if (buf->len + len + 1 > buf->cap) {
    size_t cap = buf->cap ? buf->cap * 2 : 128;
    char *next = NULL;
    while (cap < buf->len + len + 1) cap *= 2;
    next = realloc(buf->data, cap);
    if (!next) return;
    buf->data = next;
    buf->cap = cap;
  }
  
  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
}

static void tls_strbuf_puts(tls_strbuf_t *buf, const char *str) {
  tls_strbuf_append(buf, str, strlen(str));
}

static bool tls_canonical_ip(const char *ip, char *out, size_t out_len) {
  unsigned char addr[sizeof(struct in6_addr)];
  if (inet_pton(AF_INET, ip, addr) == 1) return inet_ntop(AF_INET, addr, out, (socklen_t)out_len) != NULL;
  if (inet_pton(AF_INET6, ip, addr) == 1) return inet_ntop(AF_INET6, addr, out, (socklen_t)out_len) != NULL;
  return false;
}

static bool tls_host_ipv4_strict(const char *host) {
  int parts = 0;
  const char *p = host;

  while (parts < 4) {
    unsigned value = 0;
    const char *start = p;
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + (unsigned)(*p - '0');
      if (value > 255) return false;
      p++;
    }
    if (p == start || (p - start > 1 && *start == '0')) return false;
    parts++;
    if (parts < 4 && *p++ != '.') return false;
  }
  
  return *p == '\0';
}

static bool tls_host_ip(const char *host, char *out, size_t out_len) {
  unsigned char addr[sizeof(struct in6_addr)];
  if (strchr(host, ':'))
    return inet_pton(AF_INET6, host, addr) == 1 && inet_ntop(AF_INET6, addr, out, (socklen_t)out_len) != NULL;
  return tls_host_ipv4_strict(host) &&
    inet_pton(AF_INET, host, addr) == 1 &&
    inet_ntop(AF_INET, addr, out, (socklen_t)out_len) != NULL;
}

static bool tls_host_loose_ipv4(const char *host) {
  uint64_t parts[4];
  int count = 0;
  const char *p = host;

  for (;;) {
    uint64_t value = 0;
    int base = 10;
    const char *digits = p;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
      base = 16;
      p += 2;
      digits = p;
    } else if (p[0] == '0' && p[1] >= '0' && p[1] <= '9') base = 8;

    for (;; p++) {
      int digit;
      if (*p >= '0' && *p <= '9') digit = *p - '0';
      else if (base == 16 && *p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
      else if (base == 16 && *p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
      else break;
      if (digit >= base) return false;
      value = value * (uint64_t)base + (uint64_t)digit;
      if (value > 0xffffffffULL) return false;
    }

    if (p == digits) return false;
    if (count == 4) return false;
    parts[count++] = value;
    if (*p == '\0') break;
    if (*p++ != '.') return false;
  }

  for (int i = 0; i < count - 1; i++) if (parts[i] > 255) return false;
  return parts[count - 1] <= (0xffffffffULL >> (8 * (count - 1)));
}

static int tls_split_host(char *host, char **parts, int max_parts) {
  size_t len = strlen(host);
  int count = 0;

  if (len > 0 && host[len - 1] == '.') host[--len] = '\0';
  for (size_t i = 0; i < len; i++) if (host[i] >= 'A' && host[i] <= 'Z') host[i] = (char)(host[i] - 'A' + 'a');

  parts[count++] = host;
  for (char *p = host; *p; p++) if (*p == '.') {
    if (count == max_parts) return -1;
    *p = '\0';
    parts[count++] = p + 1;
  }
  return count;
}

static bool tls_host_matches(char **host_parts, int host_count, const char *pattern_in, bool wildcards) {
  char pattern[256];
  char *parts[128];
  int count = 0;
  
  const char *star = NULL;
  const char *prefix = NULL;
  const char *host_sub = host_parts[0];
  
  size_t prefix_len = 0;
  size_t suffix_len = 0;
  size_t host_len = strlen(host_sub);

  if (!pattern_in || !*pattern_in || strlen(pattern_in) >= sizeof(pattern)) return false;
  strcpy(pattern, pattern_in);
  count = tls_split_host(pattern, parts, 128);
  if (count != host_count) return false;

  for (int i = 0; i < count; i++) {
    if (!*parts[i]) return false;
    for (const unsigned char *c = (const unsigned char *)parts[i]; *c; c++)
      if (*c < 0x21 || *c > 0x7f) return false;
  }

  for (int i = count - 1; i > 0; i--) if (strcmp(host_parts[i], parts[i]) != 0) return false;

  prefix = parts[0];
  star = strchr(prefix, '*');
  if (!star || strstr(prefix, "xn--")) return strcmp(host_sub, prefix) == 0;
  if (!wildcards || strchr(star + 1, '*') || count <= 2) return false;

  prefix_len = (size_t)(star - prefix);
  suffix_len = strlen(star + 1);
  if (prefix_len + suffix_len > host_len) return false;
  return strncmp(host_sub, prefix, prefix_len) == 0 &&
    strcmp(host_sub + host_len - suffix_len, star + 1) == 0;
}

static char *tls_value_cstr(ant_t *js, ant_value_t value) {
  ant_value_t str = js_tostring_val(js, value);
  size_t len = 0;
  
  const char *data = is_err(str) ? NULL : js_getstr(js, str, &len);
  char *out = data ? malloc(len + 1) : NULL;
  
  if (!out) return NULL;
  memcpy(out, data, len);
  out[len] = '\0';
  
  return out;
}

static ant_value_t js_tls_check_server_identity(ant_params_t) {
  ant_value_t cert = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t subject = 0;
  ant_value_t cn = js_mkundef();
  ant_value_t err = 0;
  
  char *hostname = NULL;
  char *altnames = NULL;
  char *host_copy = NULL;
  char *host_parts[128];
  
  int host_count = 0;
  char canonical[INET6_ADDRSTRLEN];
  
  bool valid = false;
  bool has_dns = false;
  
  tls_strbuf_t ips = {0};
  tls_strbuf_t reason = {0};
  tls_strbuf_t message = {0};

  if (!is_object_type(cert))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The \"cert\" argument must be of type object");

  hostname = tls_value_cstr(js, nargs > 0 ? args[0] : js_mkundef());
  if (!hostname) return Ant_Error_CreateOutOfMemory(js);
  if (*hostname && hostname[strlen(hostname) - 1] == '.') hostname[strlen(hostname) - 1] = '\0';

  subject = js_get(js, cert, "subject");
  if (is_object_type(subject)) cn = js_get(js, subject, "CN");
  if (vtype(js_get(js, cert, "subjectaltname")) == kTypeString)
    altnames = tls_value_cstr(js, js_get(js, cert, "subjectaltname"));

  host_copy = strdup(hostname);
  host_count = host_copy ? tls_split_host(host_copy, host_parts, 128) : -1;

  for (char *entry = altnames; entry && *entry;) {
    char *end = entry;
    bool quoted = false;
    for (; *end; end++) {
      if (*end == '"' && (end == entry || end[-1] != '\\')) quoted = !quoted;
      else if (!quoted && end[0] == ',' && end[1] == ' ') break;
    }
    char saved = *end;
    *end = '\0';

    if (strncmp(entry, "DNS:", 4) == 0) {
      has_dns = true;
      if (host_count > 0 && tls_host_matches(host_parts, host_count, entry + 4, true)) valid = true;
    } else if (strncmp(entry, "IP Address:", 11) == 0) {
      const char *ip = tls_canonical_ip(entry + 11, canonical, sizeof(canonical)) ? canonical : entry + 11;
      if (ips.len > 0) tls_strbuf_puts(&ips, ", ");
      tls_strbuf_puts(&ips, ip);
    }

    *end = saved;
    entry = saved ? end + 2 : end;
  }

  bool host_is_ip = tls_host_ip(hostname, canonical, sizeof(canonical));
  if (!host_is_ip && tls_host_loose_ipv4(hostname)) {
    valid = false;
    tls_strbuf_puts(&reason, "Host is a non-canonical IP address");
  } else if (host_is_ip) {
    valid = false;
    for (char *ip = ips.data; ip && *ip;) {
      char *sep = strstr(ip, ", ");
      size_t len = sep ? (size_t)(sep - ip) : strlen(ip);
      if (len == strlen(canonical) && strncmp(ip, canonical, len) == 0) valid = true;
      ip = sep ? sep + 2 : ip + len;
    }
    if (!valid) {
      tls_strbuf_puts(&reason, "IP: ");
      tls_strbuf_puts(&reason, hostname);
      tls_strbuf_puts(&reason, " is not in the cert's list: ");
      tls_strbuf_puts(&reason, ips.data ? ips.data : "");
    }
  } else if (has_dns) {
    if (!valid) {
      tls_strbuf_puts(&reason, "Host: ");
      tls_strbuf_puts(&reason, hostname);
      tls_strbuf_puts(&reason, ". is not in the cert's altnames: ");
      tls_strbuf_puts(&reason, altnames);
    }
  } else if (js_truthy(js, cn)) {
    char *cn_text = tls_value_cstr(js, cn);
    if (vtype(cn) == kTypeArray) {
      ant_offset_t len = js_arr_len(js, cn);
      for (ant_offset_t i = 0; i < len && !valid; i++) {
        char *item = tls_value_cstr(js, js_arr_get(js, cn, i));
        valid = item && host_count > 0 && tls_host_matches(host_parts, host_count, item, true);
        free(item);
      }
    } else valid = cn_text && host_count > 0 && tls_host_matches(host_parts, host_count, cn_text, true);

    if (!valid) {
      tls_strbuf_puts(&reason, "Host: ");
      tls_strbuf_puts(&reason, hostname);
      tls_strbuf_puts(&reason, ". is not cert's CN: ");
      tls_strbuf_puts(&reason, cn_text ? cn_text : "");
    }
    free(cn_text);
  } else {
    tls_strbuf_puts(&reason, "Cert does not contain a DNS name");
  }

  if (valid) err = js_mkundef(); else {
    tls_strbuf_puts(&message, "Hostname/IP does not match certificate's altnames: ");
    tls_strbuf_puts(&message, reason.data ? reason.data : "");
    err = Ant_Error_Create(js, JS_ERR_TYPE, message.data ? message.data : "Hostname/IP mismatch");
    js_set(js, err, "name", js_mkstr(js, "Error", 5));
    js_set(js, err, "code", js_mkstr(js, "ERR_TLS_CERT_ALTNAME_INVALID", 28));
    js_set(js, err, "reason", js_mkstr(js, reason.data ? reason.data : "", reason.len));
    js_set(js, err, "host", js_mkstr(js, hostname, strlen(hostname)));
    js_set(js, err, "cert", cert);
  }

  free(hostname);
  free(altnames);
  free(host_copy);
  free(ips.data);
  free(reason.data);
  free(message.data);
  
  return err;
}

static int tls_strcmp_ptr(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static ant_value_t js_tls_get_ciphers(ant_params_t) {
  static const uint16_t tls13_suites[] = { 0x1301, 0x1302, 0x1303 };
  SSL_CTX *ctx = SSL_CTX_new(TLS_method());
  STACK_OF(SSL_CIPHER) *ciphers = ctx ? SSL_CTX_get_ciphers(ctx) : NULL;
  
  size_t count = ciphers ? sk_SSL_CIPHER_num(ciphers) : 0;
  size_t total = count + sizeof(tls13_suites) / sizeof(tls13_suites[0]);
  
  char **names = calloc(total, sizeof(char *));
  ant_value_t out = js_mkarr(js);
  size_t n = 0;

  for (size_t i = 0; names && i < count; i++)
    names[n++] = strdup(SSL_CIPHER_get_name(sk_SSL_CIPHER_value(ciphers, i)));
  for (size_t i = 0; names && i < sizeof(tls13_suites) / sizeof(tls13_suites[0]); i++) {
    const SSL_CIPHER *cipher = SSL_get_cipher_by_value(tls13_suites[i]);
    if (cipher) names[n++] = strdup(SSL_CIPHER_standard_name(cipher));
  }

  for (size_t i = 0; i < n; i++)
    for (char *c = names[i]; c && *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
  if (names) qsort(names, n, sizeof(char *), tls_strcmp_ptr);

  for (size_t i = 0; i < n; i++) {
    if (names[i] && (i == 0 || !names[i - 1] || strcmp(names[i], names[i - 1]) != 0))
      js_arr_push(js, out, js_mkstr(js, names[i], strlen(names[i])));
  }

  for (size_t i = 0; i < n; i++) free(names[i]);
  free(names);
  if (ctx) SSL_CTX_free(ctx);
  return out;
}

static void tls_push_der_as_pem(ant_t *js, ant_value_t out, const uint8_t *der, size_t der_len) {
  static const char header[] = "-----BEGIN CERTIFICATE-----\n";
  static const char footer[] = "-----END CERTIFICATE-----";
  
  size_t b64_len = 4 * ((der_len + 2) / 3);
  uint8_t *b64 = malloc(b64_len + 1);
  tls_strbuf_t pem = {0};

  if (!b64) return;
  EVP_EncodeBlock(b64, der, der_len);
  tls_strbuf_puts(&pem, header);
  
  for (size_t i = 0; i < b64_len; i += 64) {
    tls_strbuf_append(&pem, (const char *)b64 + i, b64_len - i < 64 ? b64_len - i : 64);
    tls_strbuf_puts(&pem, "\n");
  }
  
  tls_strbuf_puts(&pem, footer);
  if (pem.data) js_arr_push(js, out, js_mkstr(js, pem.data, pem.len));
  free(pem.data);
  free(b64);
}

#if defined(__APPLE__)
static void tls_load_root_certificates(ant_t *js, ant_value_t out) {
  void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_LOCAL);
  void *sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY | RTLD_LOCAL);
  
  int32_t (*copy_anchors)(const void **) = sec ? dlsym(sec, "SecTrustCopyAnchorCertificates") : NULL;
  const void *(*copy_data)(const void *) = sec ? dlsym(sec, "SecCertificateCopyData") : NULL;
  
  long (*array_count)(const void *) = cf ? dlsym(cf, "CFArrayGetCount") : NULL;
  const void *(*array_get)(const void *, long) = cf ? dlsym(cf, "CFArrayGetValueAtIndex") : NULL;
  const uint8_t *(*data_bytes)(const void *) = cf ? dlsym(cf, "CFDataGetBytePtr") : NULL;
  
  long (*data_len)(const void *) = cf ? dlsym(cf, "CFDataGetLength") : NULL;
  void (*release)(const void *) = cf ? dlsym(cf, "CFRelease") : NULL;
  const void *anchors = NULL;

  if (!copy_anchors || !copy_data || !array_count || !array_get || !data_bytes || !data_len || !release) return;
  if (copy_anchors(&anchors) != 0 || !anchors) return;

  for (long i = 0; i < array_count(anchors); i++) {
    const void *data = copy_data(array_get(anchors, i));
    if (!data) continue;
    tls_push_der_as_pem(js, out, data_bytes(data), (size_t)data_len(data));
    release(data);
  }
  
  release(anchors);
}
#elif defined(_WIN32)
static void tls_load_root_certificates(ant_t *js, ant_value_t out) {
  HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
  PCCERT_CONTEXT cert = NULL;

  if (!store) return;
  while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL)
    tls_push_der_as_pem(js, out, cert->pbCertEncoded, cert->cbCertEncoded);
  CertCloseStore(store, 0);
}
#else
static void tls_load_root_certificates(ant_t *js, ant_value_t out) {
  static const char *const bundles[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/ca-bundle.pem",
    "/etc/pki/tls/cacert.pem",
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    "/etc/ssl/certs/ca-bundle.crt",
  };
  
  const char *path = getenv(X509_get_default_cert_file_env());
  BIO *bio = NULL;
  X509 *cert = NULL;

  if (!path || access(path, R_OK) != 0) path = X509_get_default_cert_file();
  for (size_t i = 0; access(path, R_OK) != 0 && i < sizeof(bundles) / sizeof(bundles[0]); i++) path = bundles[i];

  bio = BIO_new_file(path, "r");
  if (!bio) return;

  while ((cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
    uint8_t *der = NULL;
    int der_len = i2d_X509(cert, &der);
    if (der_len > 0) tls_push_der_as_pem(js, out, der, (size_t)der_len);
    OPENSSL_free(der);
    X509_free(cert);
  }
  
  ERR_clear_error();
  BIO_free(bio);
}
#endif

static ant_value_t js_tls_root_certificates(ant_params_t) {
  ant_value_t getter = js_getcurrentfunc(js);
  ant_value_t cached = js_get_slot(getter, SLOT_DATA);
  ant_value_t out = 0;

  if (vtype(cached) == kTypeArray) return cached;

  out = js_mkarr(js);
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  
  tls_load_root_certificates(js, out);
  builtin_object_freeze(js, &out, 1, js_mkundef());
  
  js_set_slot_wb(js, getter, SLOT_DATA, out);
  GC_ROOT_RESTORE(js, root_mark);
  
  return out;
}

ant_value_t tls_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  ant_value_t version = js_mkstr(js, "unknown", 7);
  ant_value_t connect = Ant_Function_Create(js_tls_connect);
  ant_value_t create_secure_context = Ant_Function_Create(js_tls_create_context);
  
  tls_context *ctx = default_tls_context();
  const char *version_str = NULL;

  if (ctx && ctx->version) {
    version_str = ctx->version();
    if (version_str) version = js_mkstr(js, version_str, strlen(version_str));
  }
  
  if (ctx && ctx->free_ctx) ctx->free_ctx(ctx);
  tls_init_context_proto(js);
  tls_init_socket_proto(js);
  tls_init_server_proto(js);
  
  if (!js->builtins.tls_context_ctor) js->builtins.tls_context_ctor = Ant_Function_CreateConstructor(
    js, js_tls_secure_context_ctor, 
    js->builtins.tls_context_proto, "SecureContext", 4
  );
  
  js_set(js, lib, "version", version);
  js_set(js, lib, "TLSSocket", js->builtins.tls_socket_ctor);
  js_set(js, lib, "Server", js->builtins.tls_server_ctor);
  js_set(js, lib, "createServer", Ant_Function_Create(js_tls_create_server));
  js_set(js, lib, "SecureContext", js->builtins.tls_context_ctor);
  js_set(js, lib, "createSecureContext", create_secure_context);
  js_set(js, lib, "connect", connect);
  js_set(js, lib, "createConnection", connect);
  js_set(js, lib, "isSecureContext", Ant_Function_Create(js_tls_is_context));
  js_set(js, lib, "setConfigPath", Ant_Function_Create(js_tls_set_config_path));
  js_set(js, lib, "checkServerIdentity", Ant_Function_Create(js_tls_check_server_identity));
  js_set(js, lib, "getCiphers", Ant_Function_Create(js_tls_get_ciphers));
  
  js_set_getter_desc(
    js, lib, "rootCertificates", 16,
    js_heavy_mkfun(js, js_tls_root_certificates, js_mkundef()), JS_DESC_E
  );
  
  js_set(js, lib, "DEFAULT_ECDH_CURVE", js_mkstr(js, "auto", 4));
  js_set(js, lib, "DEFAULT_MIN_VERSION", js_mkstr(js, "TLSv1.2", 7));
  js_set(js, lib, "DEFAULT_MAX_VERSION", js_mkstr(js, "TLSv1.3", 7));
  js_set(js, lib, "CLIENT_RENEG_LIMIT", js_mknum(3));
  js_set(js, lib, "CLIENT_RENEG_WINDOW", js_mknum(600));
  js_set(js, lib, "default", lib);
  js_set_tostringtag(js, lib, js_mkstr(js, "tls", 3));
  
  return lib;
}

void gc_mark_tls(ant_t *js, gc_mark_fn mark) {
  ant_tls_socket_t *socket = NULL;
  for (socket = js->tls_sockets; socket; socket = socket->next) {
    mark(js, socket->obj);
    if (vtype(socket->secure_context) != kTypeUndefined) mark(js, socket->secure_context);
    for (tls_write_req_t *write = socket->writes; write; write = write->next) 
      if (vtype(write->callback) != kTypeUndefined) mark(js, write->callback);
  }
}
