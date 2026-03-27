#include <compat.h> // IWYU pragma: keep

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include "net/connection.h"
#include "net/listener.h"

static void ant_listener_close_cb(uv_handle_t *handle) {
  ant_listener_t *listener = (ant_listener_t *)handle->data;
  if (!listener) return;
  listener->closed = true;
  if (listener->callbacks.on_listener_close)
    listener->callbacks.on_listener_close(listener, listener->user_data);
}

static void ant_listener_accept_cb(uv_stream_t *server_stream, int status) {
  ant_listener_t *listener = (ant_listener_t *)server_stream->data;
  ant_conn_t *conn = NULL;

  if (status < 0 || !listener || listener->closing) return;

  conn = ant_conn_create_tcp(listener, listener->idle_timeout_ms);
  if (!conn) return;

  if (ant_conn_accept(conn, server_stream) != 0) {
    ant_conn_close(conn);
    return;
  }

  if (listener->callbacks.on_accept)
    listener->callbacks.on_accept(listener, conn, listener->user_data);

  ant_conn_start(conn);
}

int ant_listener_listen_tcp(
  ant_listener_t *listener,
  uv_loop_t *loop,
  const char *hostname,
  int port,
  int backlog,
  uint64_t idle_timeout_ms,
  const ant_listener_callbacks_t *callbacks,
  void *user_data
) {
  struct sockaddr_in addr;
  int rc = 0;
  int addr_len = sizeof(addr);

  if (!listener || !loop || !hostname) return UV_EINVAL;
  memset(listener, 0, sizeof(*listener));

  listener->loop = loop;
  listener->user_data = user_data;
  listener->idle_timeout_ms = idle_timeout_ms;
  listener->port = port;
  listener->backlog = backlog > 0 ? backlog : 128;
  if (callbacks) listener->callbacks = *callbacks;

  uv_tcp_init(loop, &listener->handle);
  listener->handle.data = listener;

  rc = uv_ip4_addr(hostname, port, &addr);
  if (rc != 0) return rc;

  rc = uv_tcp_bind(&listener->handle, (const struct sockaddr *)&addr, 0);
  if (rc != 0) return rc;

  rc = uv_listen((uv_stream_t *)&listener->handle, listener->backlog, ant_listener_accept_cb);
  if (rc != 0) return rc;

  listener->started = true;
  if (port == 0 && uv_tcp_getsockname(&listener->handle, (struct sockaddr *)&addr, &addr_len) == 0)
    listener->port = ntohs(addr.sin_port);

  return 0;
}

void ant_listener_stop(ant_listener_t *listener, bool force) {
  ant_conn_t *conn = NULL;
  ant_conn_t *next = NULL;

  if (!listener) return;
  listener->closing = true;

  if (listener->started && !uv_is_closing((uv_handle_t *)&listener->handle))
    uv_close((uv_handle_t *)&listener->handle, ant_listener_close_cb);
  else if (!listener->started) listener->closed = true;

  if (force) {
  for (conn = listener->connections; conn; conn = next) {
    next = conn->next;
    ant_conn_close(conn);
  }}
}

bool ant_listener_has_connections(const ant_listener_t *listener) {
  return listener && listener->connections != NULL;
}

bool ant_listener_is_closed(const ant_listener_t *listener) {
  return !listener || listener->closed;
}

int ant_listener_port(const ant_listener_t *listener) {
  return listener ? listener->port : 0;
}

void *ant_listener_get_user_data(const ant_listener_t *listener) {
  return listener ? listener->user_data : NULL;
}
