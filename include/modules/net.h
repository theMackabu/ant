#ifndef ANT_NET_MODULE_H
#define ANT_NET_MODULE_H

#include <stdbool.h>
#include "types.h"

ant_value_t net_library(ant_t *js);
void cleanup_net_module(ant_t *js);
int net_socket_fileno(ant_value_t value);

typedef struct net_listen_args_s {
  const char *host;
  const char *path;
  int port;
  int backlog;
  ant_value_t callback;
  ant_value_t error;
} net_listen_args_t;

bool net_value_is_port(ant_t *js, ant_value_t value);
bool net_parse_listen_args(ant_native_params_t, net_listen_args_t *out);
bool net_parse_port(ant_t *js, ant_value_t value, const char *range_name, int *out_port, ant_value_t *out_error);

#endif
