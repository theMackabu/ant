#ifndef RESPONSE_H
#define RESPONSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"
#include "modules/headers.h"
#include "modules/url.h"

typedef struct {
  char *type;
  url_state_t url;
  bool has_url;
  int url_list_size;
  int status;
  char *status_text;
  uint8_t *body_data;
  size_t body_size;
  char *body_type;
  bool body_is_stream;
  bool has_body;
  bool body_used;
} response_data_t;

extern ant_value_t g_response_proto;

void init_response_module(void);

response_data_t *response_get_data(ant_value_t obj);
ant_value_t response_get_headers(ant_value_t obj);

ant_value_t response_create(
  ant_t *js,
  const char *type,
  int status,
  const char *status_text,
  ant_value_t headers_obj,
  const uint8_t *body,
  size_t body_len,
  const char *body_type,
  headers_guard_t guard
);

#endif
