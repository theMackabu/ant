#ifndef ANT_NET_CONNECTION_H
#define ANT_NET_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

typedef struct ant_listener_s ant_listener_t;
typedef struct ant_conn_s ant_conn_t;

struct ant_conn_s {
  uv_tcp_t handle;
  uv_timer_t timer;
  ant_listener_t *listener;
  void *user_data;
  char *buffer;
  size_t buffer_len;
  size_t buffer_cap;
  uint64_t timeout_ms;
  uint64_t bytes_read;
  uint64_t bytes_written;
  int close_handles;
  bool closing;
  bool read_paused;
  bool read_eof;
  bool has_local_addr;
  bool has_remote_addr;
  char local_addr[64];
  char remote_addr[64];
  char local_family[8];
  char remote_family[8];
  int local_port;
  int remote_port;
  struct ant_conn_s *next;
};

void ant_conn_start(ant_conn_t *conn);
int ant_conn_accept(ant_conn_t *conn, uv_stream_t *server_stream);
ant_conn_t *ant_conn_create_tcp(ant_listener_t *listener, uint64_t timeout_ms);

#endif
