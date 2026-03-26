#ifndef HEADERS_H
#define HEADERS_H

#include "types.h"
#include "modules/symbol.h"

extern ant_value_t g_headers_iter_proto;
extern ant_value_t g_headers_proto;

typedef enum {
  HEADERS_GUARD_NONE = 0,
  HEADERS_GUARD_REQUEST,
  HEADERS_GUARD_REQUEST_NO_CORS
} headers_guard_t;

headers_guard_t headers_get_guard(ant_value_t hdrs);

void init_headers_module(void);
void headers_set_guard(ant_value_t hdrs, headers_guard_t guard);
void headers_apply_guard(ant_value_t hdrs);
void headers_append_if_missing(ant_value_t hdrs, const char *name, const char *value);

bool headers_is_headers(ant_value_t obj);
bool headers_copy_from(ant_t *js, ant_value_t dst, ant_value_t src);
bool advance_headers(ant_t *js, struct js_iter_t *it, ant_value_t *out);

ant_value_t headers_create_empty(ant_t *js);
ant_value_t headers_get_value(ant_t *js, ant_value_t hdrs, const char *name);
ant_value_t headers_append_value(ant_t *js, ant_value_t hdrs, ant_value_t name_v, ant_value_t value_v);

#endif
