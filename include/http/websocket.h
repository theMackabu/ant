#ifndef ANT_HTTP_WEBSOCKET_H
#define ANT_HTTP_WEBSOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "modules/http.h"

#define ANT_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef uint8_t ant_ws_opcode_t;
enum {
  ANT_WS_OPCODE_CONTINUATION = 0x0,
  ANT_WS_OPCODE_TEXT         = 0x1,
  ANT_WS_OPCODE_BINARY       = 0x2,
  ANT_WS_OPCODE_CLOSE        = 0x8,
  ANT_WS_OPCODE_PING         = 0x9,
  ANT_WS_OPCODE_PONG         = 0xA,
};

typedef enum {
  ANT_WS_FRAME_INCOMPLETE = 0,
  ANT_WS_FRAME_OK,
  ANT_WS_FRAME_PROTOCOL_ERROR,
} ant_ws_frame_result_t;

typedef struct {
  uint8_t *payload;
  size_t payload_len;
  size_t consumed_len;
  ant_ws_opcode_t opcode;
  uint8_t mask[4];
  bool fin;
  bool masked;
} ant_ws_frame_t;

bool ant_ws_header_contains_token(const char *value, const char *token);
bool ant_ws_validate_client_handshake(const ant_http_header_t *headers, const char **key_out);
const char *ant_ws_find_header(const ant_http_header_t *headers, const char *name);

char *ant_ws_accept_key(const char *client_key);
void ant_ws_frame_clear(ant_ws_frame_t *frame);

ant_ws_frame_result_t ant_ws_parse_frame(
  const uint8_t *data,
  size_t len,
  bool require_mask,
  ant_ws_frame_t *out
);

uint8_t *ant_ws_encode_frame(
  ant_ws_opcode_t opcode,
  const uint8_t *payload,
  size_t payload_len,
  bool mask,
  size_t *out_len
);

uint8_t *ant_ws_encode_close_frame(
  uint16_t code,
  const char *reason,
  bool mask,
  size_t *out_len
);

#endif
