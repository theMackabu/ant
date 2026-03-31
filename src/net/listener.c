#include <compat.h> // IWYU pragma: keep

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "net/connection.h"
#include "net/listener.h"

static int sockaddr_from_addrinfo(const struct addrinfo *ai, int port, struct sockaddr_storage *out) {
  if (ai->ai_family == AF_INET) {
    struct sockaddr_in sa;
    memcpy(&sa, ai->ai_addr, sizeof(sa));
    sa.sin_port = htons((uint16_t)port);
    memcpy(out, &sa, sizeof(sa));
    return 0;
  }
  
  if (ai->ai_family == AF_INET6) {
    struct sockaddr_in6 sa6;
    memcpy(&sa6, ai->ai_addr, sizeof(sa6));
    sa6.sin6_port = htons((uint16_t)port);
    memcpy(out, &sa6, sizeof(sa6));
    return 0;
  }
  
  return UV_ENOENT;
}

static int resolve_hostname(const char *hostname, int port, struct sockaddr_storage *out) {
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  
  if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || !res) return UV_ENOENT;
  int rc = sockaddr_from_addrinfo(res, port, out);
  freeaddrinfo(res);
  
  return rc;
}

static void ant_listener_close_cb(uv_handle_t *handle) {
  ant_listener_t *listener = (ant_listener_t *)handle->data;
  if (!listener) return;

#ifndef _WIN32
  if (listener->kind == ANT_LISTENER_KIND_PIPE && listener->path && listener->path[0] != '\0')
    unlink(listener->path);
#endif

  listener->closed = true;
  if (listener->callbacks.on_listener_close)
    listener->callbacks.on_listener_close(listener, listener->user_data);

  free(listener->path);
  listener->path = NULL;
}

static void ant_listener_accept_cb(uv_stream_t *server_stream, int status) {
  ant_listener_t *listener = (ant_listener_t *)server_stream->data;
  ant_conn_t *conn = NULL;

  if (status < 0 || !listener || listener->closing) return;

  conn = listener->kind == ANT_LISTENER_KIND_PIPE
    ? ant_conn_create_pipe(listener, listener->idle_timeout_ms)
    : ant_conn_create_tcp(listener, listener->idle_timeout_ms);
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
  int port, int backlog,
  uint64_t idle_timeout_ms,
  const ant_listener_callbacks_t *callbacks,
  void *user_data
) {
  struct sockaddr_storage addr;
  struct sockaddr *sockaddr = (struct sockaddr *)&addr;
  
  int rc = 0;
  int addr_len = sizeof(addr);

  if (!listener || !loop || !hostname) return UV_EINVAL;
  memset(listener, 0, sizeof(*listener));

  listener->loop = loop;
  listener->user_data = user_data;
  listener->idle_timeout_ms = idle_timeout_ms;
  listener->port = port;
  listener->backlog = backlog > 0 ? backlog : 128;
  listener->kind = ANT_LISTENER_KIND_TCP;
  if (callbacks) listener->callbacks = *callbacks;

  uv_tcp_init(loop, &listener->handle.tcp);
  listener->handle.tcp.data = listener;

  memset(&addr, 0, sizeof(addr));
  rc = uv_ip4_addr(hostname, port, (struct sockaddr_in *)&addr);
  
  if (rc != 0) rc = uv_ip6_addr(hostname, port, (struct sockaddr_in6 *)&addr);
  if (rc != 0) rc = resolve_hostname(hostname, port, &addr);
  if (rc != 0) return rc;

  rc = uv_tcp_bind(&listener->handle.tcp, sockaddr, 0);
  if (rc != 0) return rc;

  rc = uv_listen((uv_stream_t *)&listener->handle.tcp, listener->backlog, ant_listener_accept_cb);
  if (rc != 0) return rc;

  listener->started = true;
  if (port == 0 && uv_tcp_getsockname(&listener->handle.tcp, sockaddr, &addr_len) == 0) {
    if (addr.ss_family == AF_INET) listener->port = ntohs(((struct sockaddr_in *)sockaddr)->sin_port);
    else if (addr.ss_family == AF_INET6) listener->port = ntohs(((struct sockaddr_in6 *)sockaddr)->sin6_port);
  }

  return 0;
}

int ant_listener_listen_pipe(
  ant_listener_t *listener,
  uv_loop_t *loop,
  const char *path,
  int backlog,
  uint64_t idle_timeout_ms,
  const ant_listener_callbacks_t *callbacks,
  void *user_data
) {
  int rc = 0;

  if (!listener || !loop || !path || !*path) return UV_EINVAL;
  memset(listener, 0, sizeof(*listener));

  listener->loop = loop;
  listener->user_data = user_data;
  listener->idle_timeout_ms = idle_timeout_ms;
  listener->backlog = backlog > 0 ? backlog : 128;
  listener->kind = ANT_LISTENER_KIND_PIPE;
  listener->path = strdup(path);
  if (callbacks) listener->callbacks = *callbacks;

  if (!listener->path) return UV_ENOMEM;

  uv_pipe_init(loop, &listener->handle.pipe, 0);
  listener->handle.pipe.data = listener;

  rc = uv_pipe_bind(&listener->handle.pipe, path);
  if (rc != 0) {
    free(listener->path);
    listener->path = NULL;
    return rc;
  }

  rc = uv_listen((uv_stream_t *)&listener->handle.pipe, listener->backlog, ant_listener_accept_cb);
  if (rc != 0) {
    free(listener->path);
    listener->path = NULL;
    return rc;
  }

  listener->started = true;
  return 0;
}

void ant_listener_stop(ant_listener_t *listener, bool force) {
  ant_conn_t *conn = NULL;
  ant_conn_t *next = NULL;

  if (!listener) return;
  listener->closing = true;

  if (listener->started && !uv_is_closing((uv_handle_t *)ant_listener_stream(listener)))
    uv_close((uv_handle_t *)ant_listener_stream(listener), ant_listener_close_cb);
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

void ant_listener_ref(ant_listener_t *listener) {
  if (!listener) return;
  if (!listener->started) return;
  if (!uv_is_closing((uv_handle_t *)ant_listener_stream(listener))) uv_ref((uv_handle_t *)ant_listener_stream(listener));
}

void ant_listener_unref(ant_listener_t *listener) {
  if (!listener) return;
  if (!listener->started) return;
  if (!uv_is_closing((uv_handle_t *)ant_listener_stream(listener))) uv_unref((uv_handle_t *)ant_listener_stream(listener));
}

const char *ant_listener_path(const ant_listener_t *listener) {
  return listener ? listener->path : NULL;
}
