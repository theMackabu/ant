#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include "types.h"

ant_value_t server_start_from_export(ant_t *js, ant_value_t default_export);
int server_maybe_start_from_export(ant_t *js, ant_value_t default_export);

#endif
