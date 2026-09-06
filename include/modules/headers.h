#ifndef HEADERS_H
#define HEADERS_H

#include "types.h"
#include "modules/symbol.h"

typedef void (*headers_foreach_cb)(
  const char *name,
  const char *value, void *ctx
);

struct ant_http_header_s;
const struct ant_http_header_s *headers_data_http_view(const headers_data_t *data);

size_t headers_data_find_literal(
  const headers_data_t *data, 
  const char *lower_name,
  const char **first_value
);

void init_headers_module(ant_t *js);
void headers_data_destroy(headers_data_t *data);
void headers_set_immutable(ant_value_t hdrs, bool immutable);
void headers_data_for_each(const headers_data_t *data, headers_foreach_cb cb, void *ctx);

bool headers_is_headers(ant_value_t obj);
bool headers_is_immutable(ant_value_t hdrs);
bool headers_copy_from(ant_value_t dst, ant_value_t src);
bool advance_headers(ant_t *js, js_iter_t *it, ant_value_t *out);
bool headers_set_literal(ant_value_t hdrs, const char *name, const char *value);
bool headers_data_append_if_missing(headers_data_t *data, const char *name, const char *value);
bool headers_data_set_literal(headers_data_t *data, const char *name, const char *value);

headers_data_t *headers_get_data(ant_value_t hdrs);
headers_data_t *headers_data_create(void);
headers_data_t *headers_data_copy(const headers_data_t *src);
headers_data_t *headers_data_take_http_headers(struct ant_http_header_s **headers);

ant_value_t headers_create_empty(ant_t *js);
ant_value_t headers_create_from_init(ant_t *js, ant_value_t init);
ant_value_t headers_create_from_data(ant_t *js, headers_data_t *data);
ant_value_t headers_get_value(ant_t *js, ant_value_t hdrs, const char *name);
ant_value_t headers_append_value(ant_t *js, ant_value_t hdrs, ant_value_t name_v, ant_value_t value_v);
ant_value_t headers_append_literal(ant_t *js, ant_value_t hdrs, const char *name, const char *value);
ant_value_t headers_data_init_from(ant_t *js, headers_data_t *data, ant_value_t init);

#endif
