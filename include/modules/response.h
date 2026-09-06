#ifndef RESPONSE_H
#define RESPONSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"
#include "modules/url.h"

typedef enum response_body_storage: uint8_t {
  RESPONSE_BODY_STORAGE_NONE = 0,
  RESPONSE_BODY_STORAGE_OWNED,
  RESPONSE_BODY_STORAGE_BORROWED_STRING,
} response_body_storage_t;

typedef struct {
  char *type;
  url_state_t url;
  char *status_text;
  uint8_t *body_data;
  size_t body_size;
  char *body_type;
  headers_data_t *pending_headers;
  ant_value_t websocket;
  int url_list_size;
  int status;
  response_body_storage_t body_storage;
  bool has_url;
  bool body_is_stream;
  bool has_body;
  bool body_used;
  bool headers_immutable;
} response_data_t;

void init_response_module(ant_t *js);
void response_set_websocket(ant_value_t obj, ant_value_t websocket);

response_data_t *response_get_data(ant_value_t obj);
const headers_data_t *response_get_header_data(ant_value_t obj);

ant_value_t response_get_websocket(ant_value_t obj);
ant_value_t response_materialize_headers(ant_t *js, ant_value_t obj);

ant_value_t response_create(
  ant_t *js,
  const char *type,
  int status,
  const char *status_text,
  ant_value_t headers_obj,
  const uint8_t *body,
  size_t body_len,
  const char *body_type,
  bool immutable_headers
);

ant_value_t response_create_fetched(
  ant_t *js,
  int status,
  const char *status_text,
  const char *url,
  int url_list_size,
  ant_value_t headers_obj,
  const uint8_t *body,
  size_t body_len,
  ant_value_t body_stream,
  const char *body_type
);

#endif
