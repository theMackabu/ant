#ifndef ANT_HTTP1_PARSER_H
#define ANT_HTTP1_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <llhttp.h>

#include "modules/http.h"
#include "http/http1_writer.h"

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

typedef struct {
  ant_http1_parsed_request_t req;
  ant_http1_buffer_t method;
  ant_http1_buffer_t target;
  ant_http1_buffer_t header_field;
  ant_http1_buffer_t header_value;
  ant_http1_buffer_t body;
  ant_http_header_t **header_tail;
  bool message_complete;
} ant_http1_parser_ctx_t;

typedef struct {
  llhttp_t parser;
  ant_http1_parser_ctx_t ctx;
  size_t fed_len;
} ant_http1_conn_parser_t;

typedef enum {
  ANT_HTTP1_PARSE_INCOMPLETE = 0,
  ANT_HTTP1_PARSE_OK,
  ANT_HTTP1_PARSE_ERROR,
} ant_http1_parse_result_t;


void ant_http1_free_parsed_request(ant_http1_parsed_request_t *req);
void ant_http1_conn_parser_init(ant_http1_conn_parser_t *cp);
void ant_http1_conn_parser_reset(ant_http1_conn_parser_t *cp);
void ant_http1_conn_parser_free(ant_http1_conn_parser_t *cp);

ant_http1_parse_result_t ant_http1_parse_request(
  const char *data,
  size_t len,
  ant_http1_parsed_request_t *out,
  const char **error_reason,
  const char **error_code
);

ant_http1_parse_result_t ant_http1_conn_parser_execute(
  ant_http1_conn_parser_t *cp,
  const char *data,
  size_t len,
  ant_http1_parsed_request_t *out,
  size_t *consumed_out
);

#endif
