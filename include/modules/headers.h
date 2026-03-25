#ifndef HEADERS_H
#define HEADERS_H

#include "types.h"
#include "modules/symbol.h"

extern ant_value_t g_headers_iter_proto;

void init_headers_module(void);
bool advance_headers(ant_t *js, struct js_iter_t *it, ant_value_t *out);

#endif
