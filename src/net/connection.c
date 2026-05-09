#include <compat.h> // IWYU pragma: keep

#ifndef _WIN32
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
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

typedef struct {
  uv_shutdown_t req;
  ant_conn_t *conn;
} ant_conn_shutdown_req_t;

typedef struct {
  uv_connect_t connect_req;
  uv_getaddrinfo_t resolver_req;
  ant_conn_t *conn;
  ant_conn_connect_cb cb;
  void *user_data;
  int port;
} ant_conn_connect_req_t;

static void ant_conn_restart_timer(ant_conn_t *conn);
static void ant_conn_close_cb(uv_handle_t *handle);
static void ant_conn_finish_close(ant_conn_t *conn);

static void ant_listener_remove_conn(ant_listener_t *listener, ant_conn_t *conn) {
  ant_conn_t **it = NULL;
  if (!listener || !conn) return;
  for (it = &listener->connections; *it; it = &(*it)->next) 
    if (*it == conn) { *it = conn->next; return; }
}

static bool ant_conn_store_addr(
  uv_tcp_t *handle,
  int (*get_name)(const uv_tcp_t *, struct sockaddr *, int *),
  char *out_addr,
  size_t out_addr_len,
  char *out_family,
  size_t out_family_len,
  int *out_port,
  bool *out_has_addr
) {
  struct sockaddr_storage addr;
  int len = sizeof(addr);

  if (get_name(handle, (struct sockaddr *)&addr, &len) != 0) return false;
  if (addr.ss_family == AF_INET) {
    struct sockaddr_in *in = (struct sockaddr_in *)&addr;
    uv_ip4_name(in, out_addr, out_addr_len);
    memcpy(out_family, "IPv4", out_family_len < 5 ? out_family_len : 5);
    *out_port = ntohs(in->sin_port);
    *out_has_addr = true;
    return true;
  }

  if (addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&addr;
    uv_ip6_name(in6, out_addr, out_addr_len);
    memcpy(out_family, "IPv6", out_family_len < 5 ? out_family_len : 5);
    *out_port = ntohs(in6->sin6_port);
    *out_has_addr = true;
    return true;
  }

  return false;
}

static bool ant_conn_store_peer_addr(ant_conn_t *conn) {
  if (!conn || conn->kind != ANT_CONN_KIND_TCP) return false;
  return ant_conn_store_addr(
    &conn->handle.tcp,
    (int (*)(const uv_tcp_t *, struct sockaddr *, int *))uv_tcp_getpeername,
    conn->remote_addr,
    sizeof(conn->remote_addr),
    conn->remote_family,
    sizeof(conn->remote_family),
    &conn->remote_port,
    &conn->has_remote_addr
  );
}

static bool ant_conn_store_local_addr(ant_conn_t *conn) {
  if (!conn || conn->kind != ANT_CONN_KIND_TCP) return false;
  return ant_conn_store_addr(
    &conn->handle.tcp,
    (int (*)(const uv_tcp_t *, struct sockaddr *, int *))uv_tcp_getsockname,
    conn->local_addr,
    sizeof(conn->local_addr),
    conn->local_family,
    sizeof(conn->local_family),
    &conn->local_port,
    &conn->has_local_addr
  );
}

static void ant_conn_timeout_cb(uv_timer_t *handle) {
  ant_conn_t *conn = (ant_conn_t *)handle->data;
  ant_listener_t *listener = conn ? conn->listener : NULL;

  if (!conn) return;
  if (listener && listener->callbacks.on_timeout) {
    listener->callbacks.on_timeout(conn, listener->user_data);
    return;
  }

  ant_conn_close(conn);
}

static void ant_conn_restart_timer(ant_conn_t *conn) {
  if (!conn || conn->closing) return;
  if (conn->timeout_ms == 0) { uv_timer_stop(&conn->timer); return; }
  uv_timer_start(&conn->timer, ant_conn_timeout_cb, conn->timeout_ms, 0);
}

static void ant_conn_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  ant_conn_t *conn = (ant_conn_t *)handle->data;
  size_t next_cap = 0;
  char *next = NULL;

  if (!conn) {
    buf->base = NULL;
    buf->len = 0;
    return;
  }

  if (conn->buffer_len + suggested_size > conn->buffer_cap - conn->buffer_offset) {
  if (conn->buffer_offset > 0 && conn->buffer_len > 0) memmove(conn->buffer, conn->buffer + conn->buffer_offset, conn->buffer_len);
  else if (conn->buffer_offset > 0) conn->buffer_len = 0;
  conn->buffer_offset = 0;

  if (conn->buffer_len + suggested_size > conn->buffer_cap) {
    next_cap = conn->buffer_cap ? conn->buffer_cap * 2 : ANT_CONN_READ_BUFFER_SIZE;
    while (next_cap < conn->buffer_len + suggested_size) next_cap *= 2;
    next = realloc(conn->buffer, next_cap);
    if (!next) {
      buf->base = NULL;
      buf->len = 0;
      return;
    }
    conn->buffer = next;
    conn->buffer_cap = next_cap;
  }}

  buf->base = conn->buffer + conn->buffer_offset + conn->buffer_len;
  buf->len = conn->buffer_cap - conn->buffer_offset - conn->buffer_len;
}

static void ant_conn_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  ant_conn_t *conn = (ant_conn_t *)stream->data;
  ant_listener_t *listener = conn ? conn->listener : NULL;
  if (!conn || !listener) return;

  if (nread < 0) {
    if (nread == UV_EOF) {
      conn->read_eof = true;
      uv_read_stop(ant_conn_stream(conn));
      if (listener->callbacks.on_end)
        listener->callbacks.on_end(conn, listener->user_data);
      return;
    }

    if (listener->callbacks.on_error) listener->callbacks.on_error(
      conn, (int)nread, listener->user_data
    );
    
    ant_conn_close(conn);
    return;
  }

  if (nread == 0) return;

  conn->bytes_read += (uint64_t)nread;
  conn->buffer_len += (size_t)nread;
  ant_conn_restart_timer(conn);

  if (listener->callbacks.on_read)
    listener->callbacks.on_read(conn, nread, listener->user_data);
}

static void ant_conn_write_cb_impl(uv_write_t *req, int status) {
  ant_conn_write_req_t *wr = (ant_conn_write_req_t *)req;

  if (status >= 0 && wr->conn)
    wr->conn->bytes_written += (uint64_t)wr->buf.len;
  if (wr->cb) wr->cb(wr->conn, status, wr->user_data);
  
  free(wr->buf.base);
  free(wr);
}

static void ant_conn_shutdown_cb(uv_shutdown_t *req, int status) {
  ant_conn_shutdown_req_t *shutdown_req = (ant_conn_shutdown_req_t *)req;
  ant_conn_t *conn = shutdown_req ? shutdown_req->conn : NULL;
  free(shutdown_req);
  ant_conn_close(conn);
}

static void ant_conn_finish_close(ant_conn_t *conn) {
  ant_listener_t *listener = conn ? conn->listener : NULL;
  
  if (!conn || !listener) return;

  ant_listener_remove_conn(listener, conn);
  if (listener->callbacks.on_conn_close)
    listener->callbacks.on_conn_close(conn, listener->user_data);

  free(conn->buffer);
  free(conn);
}

static void ant_conn_close_cb(uv_handle_t *handle) {
  ant_conn_t *conn = (ant_conn_t *)handle->data;
  if (!conn) return;
  if (--conn->close_handles > 0) return;
  ant_conn_finish_close(conn);
}

ant_conn_t *ant_conn_create_tcp(ant_listener_t *listener, uint64_t timeout_ms) {
  ant_conn_t *conn = NULL;

  if (!listener || !listener->loop) return NULL;

  conn = calloc(1, sizeof(*conn));
  if (!conn) return NULL;

  conn->kind = ANT_CONN_KIND_TCP;
  conn->listener = listener;
  conn->timeout_ms = timeout_ms;
  conn->buffer_cap = ANT_CONN_READ_BUFFER_SIZE;
  conn->buffer = malloc(conn->buffer_cap);
  
  if (!conn->buffer) {
    free(conn);
    return NULL;
  }

  uv_tcp_init(listener->loop, &conn->handle.tcp);
  uv_timer_init(listener->loop, &conn->timer);
  conn->handle.tcp.data = conn;
  conn->timer.data = conn;
  
  return conn;
}

ant_conn_t *ant_conn_create_pipe(ant_listener_t *listener, uint64_t timeout_ms) {
  ant_conn_t *conn = NULL;

  if (!listener || !listener->loop) return NULL;

  conn = calloc(1, sizeof(*conn));
  if (!conn) return NULL;

  conn->kind = ANT_CONN_KIND_PIPE;
  conn->listener = listener;
  conn->timeout_ms = timeout_ms;
  conn->buffer_cap = ANT_CONN_READ_BUFFER_SIZE;
  conn->buffer = malloc(conn->buffer_cap);
  
  if (!conn->buffer) {
    free(conn);
    return NULL;
  }

  uv_pipe_init(listener->loop, &conn->handle.pipe, 0);
  uv_timer_init(listener->loop, &conn->timer);
  conn->handle.pipe.data = conn;
  conn->timer.data = conn;
  
  return conn;
}

int ant_conn_accept(ant_conn_t *conn, uv_stream_t *server_stream) {
  if (!conn || !server_stream) return UV_EINVAL;

  if (uv_accept(server_stream, ant_conn_stream(conn)) != 0)
    return UV_ECONNABORTED;

  if (conn->kind == ANT_CONN_KIND_TCP) {
    ant_conn_store_peer_addr(conn);
    ant_conn_store_local_addr(conn);
  }
  
  conn->next = conn->listener->connections;
  conn->listener->connections = conn;
  
  return 0;
}

static void ant_conn_connect_cb_impl(uv_connect_t *req, int status) {
  ant_conn_connect_req_t *cr = req ? (ant_conn_connect_req_t *)req->data : NULL;
  ant_conn_t *conn = cr ? cr->conn : NULL;

  if (conn && status == 0 && conn->kind == ANT_CONN_KIND_TCP) {
    ant_conn_store_peer_addr(conn);
    ant_conn_store_local_addr(conn);
  }

  if (cr && cr->cb) cr->cb(conn, status, cr->user_data);
  free(cr);
}

static int sockaddr_from_addrinfo(const struct addrinfo *res, int port, struct sockaddr_storage *out) {
  if (!res || !out) return UV_EINVAL;

  memset(out, 0, sizeof(*out));
  if (res->ai_family == AF_INET) {
    struct sockaddr_in sa;
    memcpy(&sa, res->ai_addr, sizeof(sa));
    sa.sin_port = htons((uint16_t)port);
    memcpy(out, &sa, sizeof(sa));
    return 0;
  }

  if (res->ai_family == AF_INET6) {
    struct sockaddr_in6 sa6;
    memcpy(&sa6, res->ai_addr, sizeof(sa6));
    sa6.sin6_port = htons((uint16_t)port);
    memcpy(out, &sa6, sizeof(sa6));
    return 0;
  }
  
  return UV_ENOENT;
}

static int sockaddr_from_ip_literal(const char *hostname, int port, struct sockaddr_storage *out) {
  int rc = 0;

  if (!hostname || !out) return UV_EINVAL;
  memset(out, 0, sizeof(*out));

  rc = uv_ip4_addr(hostname, port, (struct sockaddr_in *)out);
  if (rc == 0) return 0;

  rc = uv_ip6_addr(hostname, port, (struct sockaddr_in6 *)out);
  return rc;
}

static void ant_conn_resolved_cb(uv_getaddrinfo_t *resolver, int status, struct addrinfo *res) {
  ant_conn_connect_req_t *cr = resolver ? (ant_conn_connect_req_t *)resolver->data : NULL;
  ant_conn_t *conn = cr ? cr->conn : NULL;
  struct sockaddr_storage addr;
  int rc = status;

  if (conn) conn->resolving = false;

  if (conn && conn->closing) {
    if (res) uv_freeaddrinfo(res);
    free(cr);
    if (--conn->close_handles == 0) ant_conn_finish_close(conn);
    return;
  }

  if (status == 0) {
    rc = sockaddr_from_addrinfo(res, cr ? cr->port : 0, &addr);
    if (rc == 0) {
      cr->connect_req.data = cr;
      rc = uv_tcp_connect(&cr->connect_req, &conn->handle.tcp, (const struct sockaddr *)&addr, ant_conn_connect_cb_impl);
    }
  }

  if (res) uv_freeaddrinfo(res);
  if (rc != 0) {
    if (cr && cr->cb) cr->cb(conn, rc, cr->user_data);
    free(cr);
  }
}

int ant_conn_connect_tcp(
  ant_conn_t *conn,
  const char *hostname,
  int port,
  ant_conn_connect_cb cb,
  void *user_data
) {
  ant_conn_connect_req_t *req = NULL;
  struct sockaddr_storage addr;
  struct addrinfo hints = {0};
  int rc = 0;

  if (!conn || conn->kind != ANT_CONN_KIND_TCP || !hostname || port <= 0 || port > 65535) return UV_EINVAL;

  req = calloc(1, sizeof(*req));
  if (!req) return UV_ENOMEM;

  req->conn = conn;
  req->cb = cb;
  req->user_data = user_data;
  req->port = port;

  rc = sockaddr_from_ip_literal(hostname, port, &addr);
  if (rc == 0) {
    req->connect_req.data = req;
    rc = uv_tcp_connect(&req->connect_req, &conn->handle.tcp, (const struct sockaddr *)&addr, ant_conn_connect_cb_impl);
    if (rc != 0) {
      free(req);
      return rc;
    }
    return 0;
  }

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  conn->resolving = true;
  req->resolver_req.data = req;
  rc = uv_getaddrinfo(conn->listener ? conn->listener->loop : uv_default_loop(), &req->resolver_req, ant_conn_resolved_cb, hostname, NULL, &hints);
  if (rc != 0) {
    conn->resolving = false;
    free(req);
    return rc;
  }

  return 0;
}

int ant_conn_connect_pipe(
  ant_conn_t *conn,
  const char *path,
  ant_conn_connect_cb cb,
  void *user_data
) {
  ant_conn_connect_req_t *req = NULL;
  int rc = 0;

  if (!conn || conn->kind != ANT_CONN_KIND_PIPE || !path || !*path) return UV_EINVAL;

  req = calloc(1, sizeof(*req));
  if (!req) return UV_ENOMEM;

  req->conn = conn;
  req->cb = cb;
  req->user_data = user_data;

  req->connect_req.data = req;
  uv_pipe_connect(&req->connect_req, &conn->handle.pipe, path, ant_conn_connect_cb_impl);
  return rc;
}

void ant_conn_start(ant_conn_t *conn) {
  if (!conn || conn->closing) return;
  conn->read_paused = false;
  ant_conn_restart_timer(conn);
  uv_read_start(ant_conn_stream(conn), ant_conn_alloc_cb, ant_conn_read_cb);
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
  return conn ? conn->buffer + conn->buffer_offset : NULL;
}

size_t ant_conn_buffer_len(const ant_conn_t *conn) {
  return conn ? conn->buffer_len : 0;
}

void ant_conn_set_timeout_ms(ant_conn_t *conn, uint64_t timeout_ms) {
  if (!conn) return;
  conn->timeout_ms = timeout_ms;
  ant_conn_restart_timer(conn);
}

uint64_t ant_conn_timeout_ms(const ant_conn_t *conn) {
  return conn ? conn->timeout_ms : 0;
}

bool ant_conn_is_closing(const ant_conn_t *conn) {
  return !conn || conn->closing;
}

bool ant_conn_has_local_addr(const ant_conn_t *conn) {
  return conn && conn->has_local_addr;
}

bool ant_conn_has_remote_addr(const ant_conn_t *conn) {
  return conn && conn->has_remote_addr;
}

const char *ant_conn_local_addr(const ant_conn_t *conn) {
  return conn ? conn->local_addr : NULL;
}

const char *ant_conn_remote_addr(const ant_conn_t *conn) {
  return conn ? conn->remote_addr : NULL;
}

const char *ant_conn_local_family(const ant_conn_t *conn) {
  return conn ? conn->local_family : NULL;
}

const char *ant_conn_remote_family(const ant_conn_t *conn) {
  return conn ? conn->remote_family : NULL;
}

int ant_conn_local_port(const ant_conn_t *conn) {
  return conn ? conn->local_port : 0;
}

int ant_conn_remote_port(const ant_conn_t *conn) {
  return conn ? conn->remote_port : 0;
}

void ant_conn_pause_read(ant_conn_t *conn) {
  if (!conn || conn->closing) return;
  conn->read_paused = true;
  uv_read_stop(ant_conn_stream(conn));
}

void ant_conn_resume_read(ant_conn_t *conn) {
  if (!conn || conn->closing || conn->read_eof || !conn->read_paused) return;
  conn->read_paused = false;
  ant_conn_restart_timer(conn);
  uv_read_start(ant_conn_stream(conn), ant_conn_alloc_cb, ant_conn_read_cb);
}

void ant_conn_shutdown(ant_conn_t *conn) {
  ant_conn_shutdown_req_t *req = NULL;
  int rc = 0;

  if (!conn || conn->closing) return;

  req = calloc(1, sizeof(*req));
  if (!req) {
    ant_conn_close(conn);
    return;
  }

  req->conn = conn;
  rc = uv_shutdown(&req->req, ant_conn_stream(conn), ant_conn_shutdown_cb);
  if (rc != 0) { free(req); ant_conn_close(conn); }
}

void ant_conn_ref(ant_conn_t *conn) {
  if (!conn) return;
  if (!uv_is_closing((uv_handle_t *)ant_conn_stream(conn))) uv_ref((uv_handle_t *)ant_conn_stream(conn));
  if (!uv_is_closing((uv_handle_t *)&conn->timer)) uv_ref((uv_handle_t *)&conn->timer);
}

void ant_conn_unref(ant_conn_t *conn) {
  if (!conn) return;
  if (!uv_is_closing((uv_handle_t *)ant_conn_stream(conn))) uv_unref((uv_handle_t *)ant_conn_stream(conn));
  if (!uv_is_closing((uv_handle_t *)&conn->timer)) uv_unref((uv_handle_t *)&conn->timer);
}

int ant_conn_set_no_delay(ant_conn_t *conn, bool enable) {
  if (!conn || conn->closing) return UV_EINVAL;
  if (conn->kind != ANT_CONN_KIND_TCP) return UV_ENOTSUP;
  return uv_tcp_nodelay(&conn->handle.tcp, enable ? 1 : 0);
}

int ant_conn_set_keep_alive(ant_conn_t *conn, bool enable, unsigned int delay_secs) {
  if (!conn || conn->closing) return UV_EINVAL;
  if (conn->kind != ANT_CONN_KIND_TCP) return UV_ENOTSUP;
  return uv_tcp_keepalive(&conn->handle.tcp, enable ? 1 : 0, delay_secs);
}

void ant_conn_consume(ant_conn_t *conn, size_t len) {
  if (!conn || conn->buffer_len == 0 || len == 0) return;
  if (len >= conn->buffer_len) {
    conn->buffer_offset = 0;
    conn->buffer_len = 0;
    return;
  }

  conn->buffer_offset += len;
  conn->buffer_len -= len;
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

  if (!uv_is_closing((uv_handle_t *)ant_conn_stream(conn))) {
    uv_close((uv_handle_t *)ant_conn_stream(conn), ant_conn_close_cb);
    conn->close_handles++;
  }

  if (conn->resolving) conn->close_handles++;
  if (conn->close_handles == 0) ant_conn_finish_close(conn);
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
  rc = uv_write(&wr->req, ant_conn_stream(conn), &wr->buf, 1, ant_conn_write_cb_impl);
  if (rc != 0) {
    free(wr->buf.base);
    free(wr);
    return rc;
  }

  return 0;
}

uint64_t ant_conn_bytes_read(const ant_conn_t *conn) {
  return conn ? conn->bytes_read : 0;
}

uint64_t ant_conn_bytes_written(const ant_conn_t *conn) {
  return conn ? conn->bytes_written : 0;
}
