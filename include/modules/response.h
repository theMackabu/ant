#ifndef RESPONSE_H
#define RESPONSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "types.h"
#include "modules/url.h"

typedef uint8_t response_body_storage_t;
enum response_body_storage {
  RESPONSE_BODY_STORAGE_NONE = 0,
  RESPONSE_BODY_STORAGE_OWNED,
  RESPONSE_BODY_STORAGE_BORROWED_STRING,
};

typedef struct {
  char *type;
  url_state_t url;
  char *status_text;
  uint8_t *body_data;
  size_t body_size;
  char *body_type;
  ant_value_t websocket;
  int url_list_size;
  int status;
  response_body_storage_t body_storage;
  bool has_url;
  bool body_is_stream;
  bool has_body;
  bool body_used;
} response_data_t;

response_data_t *response_get_data(ant_value_t obj);
ant_value_t response_get_headers(ant_value_t obj);
ant_value_t response_get_websocket(ant_value_t obj);

void init_response_module(ant_t *js);
void response_set_websocket(ant_value_t obj, ant_value_t websocket);

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
