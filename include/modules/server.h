#pragma once

#include <stdbool.h>
#include "types.h"

ant_value_t server_start_from_export(ant_t *js, ant_value_t default_export);
int server_maybe_start_from_export(ant_t *js, ant_value_t default_export);
void cleanup_server_module(ant_t *js);
