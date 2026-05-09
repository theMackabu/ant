#ifndef ANT_INTERNAL_TLS_MODULE_H
#define ANT_INTERNAL_TLS_MODULE_H

#include "types.h"

ant_value_t tls_library(ant_t *js);
void tls_init_socket_proto(ant_t *js);

#endif
