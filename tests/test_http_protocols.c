#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http/eventsource.h"
#include "http/websocket.h"

static int failures = 0;

static void check_bool(const char *name, bool actual, bool expected) {
  if (actual == expected) return;
  fprintf(stderr, "FAIL %s: expected %s, got %s\n", name, expected ? "true" : "false", actual ? "true" : "false");
  failures++;
}

static void check_size(const char *name, size_t actual, size_t expected) {
  if (actual == expected) return;
  fprintf(stderr, "FAIL %s: expected %zu, got %zu\n", name, expected, actual);
  failures++;
}

static void check_str(const char *name, const char *actual, const char *expected) {
  if (actual && expected && strcmp(actual, expected) == 0) return;
  fprintf(stderr, "FAIL %s: expected [%s], got [%s]\n", name, expected ? expected : "(null)", actual ? actual : "(null)");
  failures++;
}

static void test_websocket_handshake(void) {
  ant_http_header_t h4 = { "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==", NULL };
  ant_http_header_t h3 = { "Sec-WebSocket-Version", "13", &h4 };
  ant_http_header_t h2 = { "Connection", "keep-alive, Upgrade", &h3 };
  ant_http_header_t h1 = { "Upgrade", "websocket", &h2 };
  const char *key = NULL;

  char *accept = ant_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
  check_str("websocket accept key", accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  free(accept);

  check_bool("websocket validates client handshake", ant_ws_validate_client_handshake(&h1, &key), true);
  check_str("websocket returns client key", key, "dGhlIHNhbXBsZSBub25jZQ==");

  h4.value = "not-base64";
  check_bool("websocket rejects invalid key", ant_ws_validate_client_handshake(&h1, NULL), false);
}

static void test_websocket_frames(void) {
  const uint8_t hello[] = "hello";
  ant_ws_frame_t frame = {0};
  size_t frame_len = 0;
  uint8_t *encoded = ant_ws_encode_frame(ANT_WS_OPCODE_TEXT, hello, 5, true, &frame_len);
  check_bool("websocket masked encode", encoded != NULL, true);
  check_bool("websocket masked parse", ant_ws_parse_frame(encoded, frame_len, true, &frame) == ANT_WS_FRAME_OK, true);
  check_bool("websocket parsed masked flag", frame.masked, true);
  check_bool("websocket parsed text opcode", frame.opcode == ANT_WS_OPCODE_TEXT, true);
  check_str("websocket parsed text payload", (const char *)frame.payload, "hello");
  ant_ws_frame_clear(&frame);
  free(encoded);

  encoded = ant_ws_encode_close_frame(1001, "bye", false, &frame_len);
  check_bool("websocket close encode", encoded != NULL, true);
  check_bool("websocket close parse", ant_ws_parse_frame(encoded, frame_len, false, &frame) == ANT_WS_FRAME_OK, true);
  check_bool("websocket close opcode", frame.opcode == ANT_WS_OPCODE_CLOSE, true);
  check_size("websocket close payload length", frame.payload_len, 5);
  check_bool("websocket close code", frame.payload[0] == 0x03 && frame.payload[1] == 0xe9, true);
  check_str("websocket close reason", (const char *)(frame.payload + 2), "bye");
  ant_ws_frame_clear(&frame);
  free(encoded);

  const uint8_t unmasked[] = { 0x89, 0x01, 'x' };
  check_bool("websocket unmasked client frame rejected", ant_ws_parse_frame(unmasked, sizeof(unmasked), true, &frame) == ANT_WS_FRAME_PROTOCOL_ERROR, true);

  const uint8_t first[] = { 0x01, 0x02, 'h', 'i' };
  const uint8_t continuation[] = { 0x80, 0x01, '!' };
  check_bool("websocket fragmented first frame", ant_ws_parse_frame(first, sizeof(first), false, &frame) == ANT_WS_FRAME_OK, true);
  check_bool("websocket fragmented first fin", frame.fin, false);
  check_bool("websocket fragmented first opcode", frame.opcode == ANT_WS_OPCODE_TEXT, true);
  ant_ws_frame_clear(&frame);
  check_bool("websocket continuation frame", ant_ws_parse_frame(continuation, sizeof(continuation), false, &frame) == ANT_WS_FRAME_OK, true);
  check_bool("websocket continuation opcode", frame.opcode == ANT_WS_OPCODE_CONTINUATION, true);
  ant_ws_frame_clear(&frame);
}

typedef struct {
  int count;
} sse_capture_t;

static bool capture_sse_message(const ant_sse_message_t *message, void *user_data) {
  sse_capture_t *capture = user_data;
  capture->count++;
  check_str("sse parser data", message->data, "hello\nworld");
  check_str("sse parser event", message->event, "greeting");
  check_str("sse parser id", message->id, "7");
  check_bool("sse parser retry flag", message->has_retry, true);
  check_size("sse parser retry", message->retry, 1500);
  return true;
}

static void test_eventsource_format_and_parse(void) {
  size_t len = 0;
  char *event = ant_sse_format_event("hello\nworld", "greeting", "7", "1500", &len);
  check_str("sse event formatting", event, "event: greeting\nid: 7\nretry: 1500\ndata: hello\ndata: world\n\n");
  check_size("sse event length", len, strlen(event));
  free(event);

  char *comment = ant_sse_format_comment("a\nb", &len);
  check_str("sse comment formatting", comment, ": a\n: b\n\n");
  check_size("sse comment length", len, strlen(comment));
  free(comment);

  ant_sse_parser_t parser;
  sse_capture_t capture = {0};
  const char first[] = "event: greeting\nid: 7\nretry: 1500\ndata: hello\n";
  ant_sse_parser_init(&parser);
  check_bool("sse parser feed first chunk", ant_sse_parser_feed(&parser, first, strlen(first), capture_sse_message, &capture), true);
  const char second[] = "data: world\n\n";
  check_bool("sse parser feed second chunk", ant_sse_parser_feed(&parser, second, strlen(second), capture_sse_message, &capture), true);
  check_size("sse parser dispatch count", capture.count, 1);
  check_str("sse parser last event id", parser.last_event_id, "7");
  check_bool("sse parser reconnect retry flag", parser.has_retry, true);
  check_size("sse parser reconnect retry", parser.retry, 1500);
  ant_sse_parser_free(&parser);
}

int main(void) {
  test_websocket_handshake();
  test_websocket_frames();
  test_eventsource_format_and_parse();

  if (failures) {
    fprintf(stderr, "%d protocol test failure(s)\n", failures);
    return 1;
  }
  puts("http protocol tests passed");
  return 0;
}
