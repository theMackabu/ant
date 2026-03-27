#ifndef ANT_NET_LISTENER_H
#define ANT_NET_LISTENER_H

#include <stdbool.h>
#include <stddef.h>
#include <uv.h>

typedef struct ant_listener_s ant_listener_t;
typedef struct ant_conn_s ant_conn_t;

typedef void (*ant_conn_write_cb)(
  ant_conn_t *conn,
  int status,
  void *user_data
);

typedef struct {
  void (*on_accept)(ant_listener_t *listener, ant_conn_t *conn, void *user_data);
  void (*on_read)(ant_conn_t *conn, ssize_t nread, void *user_data);
  void (*on_conn_close)(ant_conn_t *conn, void *user_data);
  void (*on_listener_close)(ant_listener_t *listener, void *user_data);
} ant_listener_callbacks_t;

struct ant_listener_s {
  uv_loop_t *loop;
  uv_tcp_t handle;
  ant_conn_t *connections;
  ant_listener_callbacks_t callbacks;
  void *user_data;
  int idle_timeout_secs;
  int port;
  int backlog;
  bool started;
  bool closing;
  bool closed;
};

int ant_listener_listen_tcp(
  ant_listener_t *listener,
  uv_loop_t *loop,
  const char *hostname,
  int port,
  int backlog,
  int idle_timeout_secs,
  const ant_listener_callbacks_t *callbacks,
  void *user_data
);

void ant_listener_stop(ant_listener_t *listener, bool force);
void *ant_listener_get_user_data(const ant_listener_t *listener);

int ant_listener_port(const ant_listener_t *listener);
int ant_conn_timeout(const ant_conn_t *conn);
int ant_conn_remote_port(const ant_conn_t *conn);
int ant_conn_write(ant_conn_t *conn, char *data, size_t len, ant_conn_write_cb cb, void *user_data);

bool ant_listener_has_connections(const ant_listener_t *listener);
bool ant_listener_is_closed(const ant_listener_t *listener);
bool ant_conn_is_closing(const ant_conn_t *conn);
bool ant_conn_has_remote_addr(const ant_conn_t *conn);

void ant_conn_set_user_data(ant_conn_t *conn, void *user_data);
void *ant_conn_get_user_data(const ant_conn_t *conn);
void ant_conn_pause_read(ant_conn_t *conn);
void ant_conn_close(ant_conn_t *conn);
void ant_conn_set_timeout(ant_conn_t *conn, int timeout_secs);

const char *ant_conn_buffer(const ant_conn_t *conn);
const char *ant_conn_remote_addr(const ant_conn_t *conn);

size_t ant_conn_buffer_len(const ant_conn_t *conn);
ant_listener_t *ant_conn_listener(const ant_conn_t *conn);

#endif
