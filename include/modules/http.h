#ifndef ANT_HTTP_H
#define ANT_HTTP_H

#include <uv.h>
#include <types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct ant_http_header_s {
  char *name;
  char *value;
  struct ant_http_header_s *next;
} ant_http_header_t;

typedef enum {
  ANT_HTTP_RESULT_OK = 0,
  ANT_HTTP_RESULT_NETWORK_ERROR,
  ANT_HTTP_RESULT_ABORTED,
  ANT_HTTP_RESULT_PROTOCOL_ERROR,
} ant_http_result_t;

typedef struct {
  const char *method;
  const char *url;
  const ant_http_header_t *headers;
  const uint8_t *body;
  size_t body_len;
  bool chunked_body;
} ant_http_request_options_t;

typedef struct {
  int status;
  const char *status_text;
  const ant_http_header_t *headers;
} ant_http_response_t;

typedef void (*ant_http_response_cb)(
  ant_http_request_t *req,
  const ant_http_response_t *resp,
  void *user_data
);

typedef void (*ant_http_body_cb)(
  ant_http_request_t *req,
  const uint8_t *chunk,
  size_t len,
  void *user_data
);

typedef void (*ant_http_complete_cb)(
  ant_http_request_t *req,
  ant_http_result_t result,
  int error_code,
  const char *error_message,
  void *user_data
);

int ant_http_request_start(
  uv_loop_t *loop,
  const ant_http_request_options_t *options,
  ant_http_response_cb on_response,
  ant_http_body_cb on_body,
  ant_http_complete_cb on_complete,
  void *user_data,
  ant_http_request_t **out_req
);

int ant_http_request_cancel(ant_http_request_t *req);
int ant_http_request_write(ant_http_request_t *req, const uint8_t *chunk, size_t len);

void ant_http_request_end(ant_http_request_t *req);
void ant_http_headers_free(ant_http_header_t *headers);

const ant_http_response_t *ant_http_request_response(ant_http_request_t *req);

#endif
