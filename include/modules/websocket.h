#ifndef MODULES_WEBSOCKET_H
#define MODULES_WEBSOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"
#include "gc/modules.h"
#include "net/connection.h"

typedef struct {
  size_t max_payload_len;
  bool per_message_deflate;
} ant_websocket_server_options_t;

void init_websocket_module(ant_t *js);
void gc_mark_websocket(ant_t *js, gc_mark_fn mark);

ant_value_t ant_websocket_accept_server(
  ant_t *js, ant_conn_t *conn,
  ant_value_t request_obj,
  const char *protocol,
  const ant_websocket_server_options_t *options
);

void ant_websocket_server_open(ant_t *js, ant_value_t socket_obj);
void ant_websocket_server_on_close(ant_t *js, ant_value_t socket_obj);
void ant_websocket_server_on_read(ant_t *js, ant_value_t socket_obj, ant_conn_t *conn);

#endif
