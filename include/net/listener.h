#ifndef ANT_NET_LISTENER_H
#define ANT_NET_LISTENER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

typedef struct ant_listener_s ant_listener_t;
typedef struct ant_conn_s ant_conn_t;

typedef enum {
  ANT_LISTENER_KIND_TCP = 0,
  ANT_LISTENER_KIND_PIPE,
} ant_listener_kind_t;

typedef void (*ant_conn_write_cb)(
  ant_conn_t *conn,
  int status,
  void *user_data
);

typedef struct {
  void (*on_accept)(ant_listener_t *listener, ant_conn_t *conn, void *user_data);
  void (*on_read)(ant_conn_t *conn, ssize_t nread, void *user_data);
  void (*on_end)(ant_conn_t *conn, void *user_data);
  void (*on_error)(ant_conn_t *conn, int status, void *user_data);
  void (*on_timeout)(ant_conn_t *conn, void *user_data);
  void (*on_conn_close)(ant_conn_t *conn, void *user_data);
  void (*on_listener_close)(ant_listener_t *listener, void *user_data);
} ant_listener_callbacks_t;

struct ant_listener_s {
  uv_loop_t *loop;
  ant_listener_kind_t kind;
  
  union {
    uv_tcp_t tcp;
    uv_pipe_t pipe;
  } handle;
  
  ant_conn_t *connections;
  ant_listener_callbacks_t callbacks;
  
  void *user_data;
  uint64_t idle_timeout_ms;
  
  int port;
  int backlog;
  
  char *path;
  bool started;
  bool closing;
  bool closed;
};

static inline uv_stream_t *ant_listener_stream(ant_listener_t *listener) {
  if (!listener) return NULL;
  switch (listener->kind) {
    case ANT_LISTENER_KIND_PIPE: return (uv_stream_t *)&listener->handle.pipe;
    case ANT_LISTENER_KIND_TCP:
    default: return (uv_stream_t *)&listener->handle.tcp;
  }
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
);

int ant_listener_listen_pipe(
  ant_listener_t *listener,
  uv_loop_t *loop,
  const char *path,
  int backlog,
  uint64_t idle_timeout_ms,
  const ant_listener_callbacks_t *callbacks,
  void *user_data
);

void ant_listener_stop(ant_listener_t *listener, bool force);
void *ant_listener_get_user_data(const ant_listener_t *listener);
void ant_listener_ref(ant_listener_t *listener);
void ant_listener_unref(ant_listener_t *listener);

int ant_conn_local_port(const ant_conn_t *conn);
int ant_listener_port(const ant_listener_t *listener);
int ant_conn_remote_port(const ant_conn_t *conn);
int ant_conn_set_no_delay(ant_conn_t *conn, bool enable);
int ant_conn_set_keep_alive(ant_conn_t *conn, bool enable, unsigned int delay_secs);
int ant_conn_write(ant_conn_t *conn, char *data, size_t len, ant_conn_write_cb cb, void *user_data);

bool ant_listener_has_connections(const ant_listener_t *listener);
bool ant_listener_is_closed(const ant_listener_t *listener);
bool ant_conn_is_closing(const ant_conn_t *conn);
bool ant_conn_has_local_addr(const ant_conn_t *conn);
bool ant_conn_has_remote_addr(const ant_conn_t *conn);

void ant_conn_set_user_data(ant_conn_t *conn, void *user_data);
void *ant_conn_get_user_data(const ant_conn_t *conn);
void ant_conn_pause_read(ant_conn_t *conn);
void ant_conn_resume_read(ant_conn_t *conn);
void ant_conn_close(ant_conn_t *conn);
void ant_conn_shutdown(ant_conn_t *conn);
void ant_conn_ref(ant_conn_t *conn);
void ant_conn_unref(ant_conn_t *conn);
void ant_conn_set_timeout_ms(ant_conn_t *conn, uint64_t timeout_ms);
void ant_conn_consume(ant_conn_t *conn, size_t len);

const char *ant_conn_buffer(const ant_conn_t *conn);
const char *ant_conn_local_addr(const ant_conn_t *conn);
const char *ant_conn_remote_addr(const ant_conn_t *conn);
const char *ant_conn_local_family(const ant_conn_t *conn);
const char *ant_conn_remote_family(const ant_conn_t *conn);
const char *ant_listener_path(const ant_listener_t *listener);

size_t ant_conn_buffer_len(const ant_conn_t *conn);
ant_listener_t *ant_conn_listener(const ant_conn_t *conn);

uint64_t ant_conn_timeout_ms(const ant_conn_t *conn);
uint64_t ant_conn_bytes_read(const ant_conn_t *conn);
uint64_t ant_conn_bytes_written(const ant_conn_t *conn);

#endif
