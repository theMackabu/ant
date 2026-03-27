#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include "types.h"

ant_value_t server_start_from_export(ant_t *js, ant_value_t default_export);
bool server_export_has_fetch_handler(ant_t *js, ant_value_t default_export, bool *looks_like_config);

#endif
