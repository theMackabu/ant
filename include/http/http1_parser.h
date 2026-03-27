#ifndef ANT_HTTP1_PARSER_H
#define ANT_HTTP1_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "modules/http.h"

typedef struct {
  char *method;
  char *target;
  char *host;
  char *content_type;
  ant_http_header_t *headers;
  uint8_t *body;
  size_t body_len;
  size_t content_length;
  size_t consumed_len;
  uint8_t http_major;
  uint8_t http_minor;
  bool absolute_target;
  bool keep_alive;
} ant_http1_parsed_request_t;

typedef enum {
  ANT_HTTP1_PARSE_INCOMPLETE = 0,
  ANT_HTTP1_PARSE_OK,
  ANT_HTTP1_PARSE_ERROR,
} ant_http1_parse_result_t;

ant_http1_parse_result_t ant_http1_parse_request(
  const char *data,
  size_t len,
  ant_http1_parsed_request_t *out,
  const char **error_reason,
  const char **error_code
);

void ant_http1_free_parsed_request(ant_http1_parsed_request_t *req);

#endif
