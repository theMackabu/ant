#ifndef REQUEST_H
#define REQUEST_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "types.h"
#include "modules/url.h"

typedef struct {
  char *method;
  url_state_t url;
  char *referrer;
  char *referrer_policy;
  char *mode;
  char *credentials;
  char *cache;
  char *redirect;
  char *integrity;
  bool keepalive;
  bool reload_navigation;
  bool history_navigation;
  uint8_t *body_data;
  size_t body_size;
  char *body_type;
  bool body_is_stream;
  bool has_body;
  bool body_used;
} request_data_t;

extern ant_value_t g_request_proto;
void init_request_module(void);

request_data_t *request_get_data(ant_value_t obj);
ant_value_t request_get_headers(ant_value_t obj);
ant_value_t request_get_signal(ant_value_t obj);

ant_value_t request_create(
  ant_t *js, const char *method, const char *url, ant_value_t headers,
  const uint8_t *body, size_t body_len, const char *body_type
);

#endif
