#ifndef ANT_INSPECTOR_H
#define ANT_INSPECTOR_H

#include "types.h"
#include "modules/http.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool enabled;
  bool wait_for_session;
  char host[64];
  int port;
} ant_inspector_options_t;

bool ant_inspector_start(
  ant_t *js,
  const ant_inspector_options_t *options
);

void ant_inspector_register_script_source(
  const char *path,
  const char *source,
  size_t len,
  bool is_module
);

void ant_inspector_register_script_file(
  const char *path,
  bool is_module
);

void ant_inspector_console_api_called(
  ant_t *js, const char *level,
  ant_value_t *args, int nargs
);

uint64_t ant_inspector_network_request(
  const char *method,
  const char *url,
  const char *type,
  const char *initiator,
  bool has_post_data,
  const ant_http_header_t *headers
);

void ant_inspector_network_response(
  uint64_t request_id,
  const char *url,
  int status,
  const char *status_text,
  const char *mime_type,
  const char *type,
  const ant_http_header_t *headers
);

void ant_inspector_network_finish(
  uint64_t request_id,
  size_t encoded_data_length
);

void ant_inspector_network_fail(
  uint64_t request_id,
  const char *error_text,
  bool canceled,
  const char *type
);

void ant_inspector_network_set_request_body(
  uint64_t request_id,
  const uint8_t *data,
  size_t len
);

void ant_inspector_network_append_response_body(
  uint64_t request_id,
  const uint8_t *data,
  size_t len
);

void ant_inspector_websocket_request(
  uint64_t request_id,
  const ant_http_header_t *headers
);

void ant_inspector_websocket_response(
  uint64_t request_id,
  int status,
  const char *status_text,
  const ant_http_header_t *headers
);

void ant_inspector_websocket_frame_sent(
  uint64_t request_id,
  const uint8_t *data,
  size_t len,
  bool binary
);

void ant_inspector_websocket_frame_received(
  uint64_t request_id,
  const uint8_t *data,
  size_t len,
  bool binary
);

void ant_inspector_websocket_error(
  uint64_t request_id,
  const char *message
);

void ant_inspector_stop(void);
void ant_inspector_wait_for_session(void);

uint64_t ant_inspector_websocket_created(const char *url);
void ant_inspector_websocket_closed(uint64_t request_id);

#endif
