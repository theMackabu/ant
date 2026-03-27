#include <compat.h> // IWYU pragma: keep

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include "net/connection.h"
#include "net/listener.h"

#define ANT_CONN_READ_BUFFER_SIZE (16 * 1024)

typedef struct {
  uv_write_t req;
  uv_buf_t buf;
  ant_conn_t *conn;
  ant_conn_write_cb cb;
  void *user_data;
} ant_conn_write_req_t;

static void ant_conn_restart_timer(ant_conn_t *conn);
static void ant_conn_close_cb(uv_handle_t *handle);

static void ant_listener_remove_conn(ant_listener_t *listener, ant_conn_t *conn) {
  ant_conn_t **it = NULL;
  if (!listener || !conn) return;

  for (it = &listener->connections; *it; it = &(*it)->next) {
  if (*it == conn) {
    *it = conn->next;
    return;
  }}
}

static bool ant_conn_store_peer_addr(ant_conn_t *conn) {
  struct sockaddr_storage addr;
  int len = sizeof(addr);

  if (uv_tcp_getpeername(&conn->handle, (struct sockaddr *)&addr, &len) != 0) return false;
  if (addr.ss_family == AF_INET) {
    struct sockaddr_in *in = (struct sockaddr_in *)&addr;
    uv_ip4_name(in, conn->remote_addr, sizeof(conn->remote_addr));
    conn->remote_port = ntohs(in->sin_port);
    conn->has_remote_addr = true;
    return true;
  }
  if (addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&addr;
    uv_ip6_name(in6, conn->remote_addr, sizeof(conn->remote_addr));
    conn->remote_port = ntohs(in6->sin6_port);
    conn->has_remote_addr = true;
    return true;
  }
  return false;
}

static void ant_conn_timeout_cb(uv_timer_t *handle) {
  ant_conn_t *conn = (ant_conn_t *)handle->data;
  ant_conn_close(conn);
}

static void ant_conn_restart_timer(ant_conn_t *conn) {
  uint64_t timeout_ms = 0;

  if (!conn || conn->closing) return;
  uv_timer_stop(&conn->timer);
  if (conn->timeout_secs <= 0) return;

  timeout_ms = (uint64_t)conn->timeout_secs * 1000ULL;
  uv_timer_start(&conn->timer, ant_conn_timeout_cb, timeout_ms, 0);
}

static void ant_conn_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  ant_conn_t *conn = (ant_conn_t *)handle->data;
  size_t need = 0;
  size_t next_cap = 0;
  char *next = NULL;

  if (!conn) {
    buf->base = NULL;
    buf->len = 0;
    return;
  }

  need = conn->buffer_len + suggested_size;
  if (need > conn->buffer_cap) {
    next_cap = conn->buffer_cap ? conn->buffer_cap * 2 : ANT_CONN_READ_BUFFER_SIZE;
    while (next_cap < need) next_cap *= 2;
    next = realloc(conn->buffer, next_cap);
    if (!next) {
      buf->base = NULL;
      buf->len = 0;
      return;
    }
    conn->buffer = next;
    conn->buffer_cap = next_cap;
  }

  buf->base = conn->buffer + conn->buffer_len;
  buf->len = conn->buffer_cap - conn->buffer_len;
}

static void ant_conn_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  ant_conn_t *conn = (ant_conn_t *)stream->data;
  ant_listener_t *listener = conn ? conn->listener : NULL;
  (void)buf;

  if (!conn || !listener) return;
  if (nread < 0) {
    ant_conn_close(conn);
    return;
  }
  if (nread == 0) return;

  conn->buffer_len += (size_t)nread;
  ant_conn_restart_timer(conn);

  if (listener->callbacks.on_read)
    listener->callbacks.on_read(conn, nread, listener->user_data);
}

static void ant_conn_write_cb_impl(uv_write_t *req, int status) {
  ant_conn_write_req_t *wr = (ant_conn_write_req_t *)req;

  if (wr->cb) wr->cb(wr->conn, status, wr->user_data);
  free(wr->buf.base);
  free(wr);
}

static void ant_conn_close_cb(uv_handle_t *handle) {
  ant_conn_t *conn = (ant_conn_t *)handle->data;
  ant_listener_t *listener = conn ? conn->listener : NULL;
  if (!conn || !listener) return;

  if (--conn->close_handles > 0) return;

  ant_listener_remove_conn(listener, conn);
  if (listener->callbacks.on_conn_close)
    listener->callbacks.on_conn_close(conn, listener->user_data);

  free(conn->buffer);
  free(conn);
}

ant_conn_t *ant_conn_create_tcp(ant_listener_t *listener, int timeout_secs) {
  ant_conn_t *conn = NULL;

  if (!listener || !listener->loop) return NULL;

  conn = calloc(1, sizeof(*conn));
  if (!conn) return NULL;

  conn->listener = listener;
  conn->timeout_secs = timeout_secs;
  conn->buffer_cap = ANT_CONN_READ_BUFFER_SIZE;
  conn->buffer = malloc(conn->buffer_cap);
  if (!conn->buffer) {
    free(conn);
    return NULL;
  }

  uv_tcp_init(listener->loop, &conn->handle);
  uv_timer_init(listener->loop, &conn->timer);
  conn->handle.data = conn;
  conn->timer.data = conn;
  return conn;
}

int ant_conn_accept(ant_conn_t *conn, uv_stream_t *server_stream) {
  if (!conn || !server_stream) return UV_EINVAL;

  if (uv_accept(server_stream, (uv_stream_t *)&conn->handle) != 0)
    return UV_ECONNABORTED;

  ant_conn_store_peer_addr(conn);
  conn->next = conn->listener->connections;
  conn->listener->connections = conn;
  return 0;
}

void ant_conn_start(ant_conn_t *conn) {
  if (!conn || conn->closing) return;
  ant_conn_restart_timer(conn);
  uv_read_start((uv_stream_t *)&conn->handle, ant_conn_alloc_cb, ant_conn_read_cb);
}

void ant_conn_set_user_data(ant_conn_t *conn, void *user_data) {
  if (conn) conn->user_data = user_data;
}

void *ant_conn_get_user_data(const ant_conn_t *conn) {
  return conn ? conn->user_data : NULL;
}

ant_listener_t *ant_conn_listener(const ant_conn_t *conn) {
  return conn ? conn->listener : NULL;
}

const char *ant_conn_buffer(const ant_conn_t *conn) {
  return conn ? conn->buffer : NULL;
}

size_t ant_conn_buffer_len(const ant_conn_t *conn) {
  return conn ? conn->buffer_len : 0;
}

void ant_conn_set_timeout(ant_conn_t *conn, int timeout_secs) {
  if (!conn) return;
  conn->timeout_secs = timeout_secs < 0 ? 0 : timeout_secs;
  ant_conn_restart_timer(conn);
}

int ant_conn_timeout(const ant_conn_t *conn) {
  return conn ? conn->timeout_secs : 0;
}

bool ant_conn_is_closing(const ant_conn_t *conn) {
  return !conn || conn->closing;
}

bool ant_conn_has_remote_addr(const ant_conn_t *conn) {
  return conn && conn->has_remote_addr;
}

const char *ant_conn_remote_addr(const ant_conn_t *conn) {
  return conn ? conn->remote_addr : NULL;
}

int ant_conn_remote_port(const ant_conn_t *conn) {
  return conn ? conn->remote_port : 0;
}

void ant_conn_pause_read(ant_conn_t *conn) {
  if (!conn || conn->closing) return;
  uv_read_stop((uv_stream_t *)&conn->handle);
}

void ant_conn_close(ant_conn_t *conn) {
  if (!conn || conn->closing) return;
  conn->closing = true;

  conn->close_handles = 0;
  if (!uv_is_closing((uv_handle_t *)&conn->timer)) {
    uv_timer_stop(&conn->timer);
    uv_close((uv_handle_t *)&conn->timer, ant_conn_close_cb);
    conn->close_handles++;
  }

  if (!uv_is_closing((uv_handle_t *)&conn->handle)) {
    uv_close((uv_handle_t *)&conn->handle, ant_conn_close_cb);
    conn->close_handles++;
  }

  if (conn->close_handles == 0) ant_conn_close_cb((uv_handle_t *)&conn->handle);
}

int ant_conn_write(ant_conn_t *conn, char *data, size_t len, ant_conn_write_cb cb, void *user_data) {
  ant_conn_write_req_t *wr = NULL;
  int rc = 0;

  if (!conn || conn->closing) {
    free(data);
    return UV_EPIPE;
  }

  wr = calloc(1, sizeof(*wr));
  if (!wr) {
    free(data);
    return UV_ENOMEM;
  }

  wr->buf = uv_buf_init(data, (unsigned int)len);
  wr->conn = conn;
  wr->cb = cb;
  wr->user_data = user_data;

  ant_conn_restart_timer(conn);
  rc = uv_write(&wr->req, (uv_stream_t *)&conn->handle, &wr->buf, 1, ant_conn_write_cb_impl);
  if (rc != 0) {
    free(wr->buf.base);
    free(wr);
    return rc;
  }

  return 0;
}
