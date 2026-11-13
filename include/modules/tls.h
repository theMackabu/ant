#pragma once

#include "types.h"

__attribute__((cold)) 
ant_value_t tls_library(ant_t *js);
void tls_init_socket_proto(ant_t *js);
void cleanup_tls_module(ant_t *js);
