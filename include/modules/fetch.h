#pragma once

#include "types.h"

void init_fetch_module(ant_t *js);
void cleanup_fetch_module(ant_t *js);
void fetch_start_http(fetch_request_t *req);

int has_pending_fetches(ant_t *js);
ant_value_t ant_fetch(ant_params_t);
