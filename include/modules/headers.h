#ifndef HEADERS_H
#define HEADERS_H

#include "types.h"
#include "modules/symbol.h"

extern ant_value_t g_headers_iter_proto;
extern ant_value_t g_headers_proto;

typedef struct headers_data headers_data_t;

typedef void (*headers_foreach_cb)(
  const char *name,
  const char *value,
  void *ctx
);

void init_headers_module(void);
void headers_set_immutable(ant_value_t hdrs, bool immutable);
void headers_append_if_missing(ant_value_t hdrs, const char *name, const char *value);
void headers_for_each(ant_value_t hdrs, headers_foreach_cb cb, void *ctx);

bool headers_is_headers(ant_value_t obj);
bool headers_is_immutable(ant_value_t hdrs);
bool headers_copy_from(ant_t *js, ant_value_t dst, ant_value_t src);
bool advance_headers(ant_t *js, js_iter_t *it, ant_value_t *out);
bool headers_init_has_name(ant_t *js, ant_value_t init, const char *name);
bool headers_set_literal(ant_t *js, ant_value_t hdrs, const char *name, const char *value);

headers_data_t *headers_data_create(void);
headers_data_t *headers_data_copy(const headers_data_t *src);
void headers_data_destroy(headers_data_t *data);
void headers_data_append_if_missing(headers_data_t *data, const char *name, const char *value);
ant_value_t headers_data_init_from(ant_t *js, headers_data_t *data, ant_value_t init);

ant_value_t headers_create_empty(ant_t *js);
ant_value_t headers_create_from_init(ant_t *js, ant_value_t init);
ant_value_t headers_create_from_data(ant_t *js, headers_data_t *data);
ant_value_t headers_init_from(ant_t *js, ant_value_t hdrs, ant_value_t init);
ant_value_t headers_get_value(ant_t *js, ant_value_t hdrs, const char *name);
ant_value_t headers_append_value(ant_t *js, ant_value_t hdrs, ant_value_t name_v, ant_value_t value_v);
ant_value_t headers_append_literal(ant_t *js, ant_value_t hdrs, const char *name, const char *value);

size_t headers_find_literal(ant_value_t hdrs, const char *lower_name, const char **first_value);

#endif
