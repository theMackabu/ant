#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esm/remote.h"
#include "http/eventsource.h"
#include "http/http1_parser.h"
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
  uint8_t *encoded = ant_ws_encode_frame(ANT_WS_OPCODE_TEXT, hello, 5, true, false, &frame_len);
  check_bool("websocket masked encode", encoded != NULL, true);
  check_bool("websocket masked parse", ant_ws_parse_frame(encoded, frame_len, true, false, false, SIZE_MAX, &frame) == ANT_WS_FRAME_OK, true);
  check_bool("websocket parsed masked flag", frame.masked, true);
  check_bool("websocket parsed text opcode", frame.opcode == ANT_WS_OPCODE_TEXT, true);
  check_str("websocket parsed text payload", (const char *)frame.payload, "hello");
  ant_ws_frame_clear(&frame);
  free(encoded);

  encoded = ant_ws_encode_close_frame(1001, "bye", false, &frame_len);
  check_bool("websocket close encode", encoded != NULL, true);
  check_bool("websocket close parse", ant_ws_parse_frame(encoded, frame_len, false, false, false, SIZE_MAX, &frame) == ANT_WS_FRAME_OK, true);
  check_bool("websocket close opcode", frame.opcode == ANT_WS_OPCODE_CLOSE, true);
  check_size("websocket close payload length", frame.payload_len, 5);
  check_bool("websocket close code", frame.payload[0] == 0x03 && frame.payload[1] == 0xe9, true);
  check_str("websocket close reason", (const char *)(frame.payload + 2), "bye");
  ant_ws_frame_clear(&frame);
  free(encoded);

  uint8_t unmasked[] = { 0x89, 0x01, 'x' };
  check_bool("websocket unmasked client frame rejected", ant_ws_parse_frame(unmasked, sizeof(unmasked), true, false, false, SIZE_MAX, &frame) == ANT_WS_FRAME_PROTOCOL_ERROR, true);

  uint8_t first[] = { 0x01, 0x02, 'h', 'i' };
  uint8_t continuation[] = { 0x80, 0x01, '!' };
  check_bool("websocket fragmented first frame", ant_ws_parse_frame(first, sizeof(first), false, false, false, SIZE_MAX, &frame) == ANT_WS_FRAME_OK, true);
  check_bool("websocket fragmented first fin", frame.fin, false);
  check_bool("websocket fragmented first opcode", frame.opcode == ANT_WS_OPCODE_TEXT, true);
  ant_ws_frame_clear(&frame);
  check_bool("websocket continuation frame", ant_ws_parse_frame(continuation, sizeof(continuation), false, false, false, SIZE_MAX, &frame) == ANT_WS_FRAME_OK, true);
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

static void test_websocket_reserved_opcodes(void) {
  for (uint8_t opcode = 0; opcode < 16; opcode++) {
    uint8_t frame[2] = { (uint8_t)(0x80u | opcode), 0 };
    ant_ws_frame_t parsed;
    bool known = opcode <= ANT_WS_OPCODE_BINARY || (opcode >= ANT_WS_OPCODE_CLOSE && opcode <= ANT_WS_OPCODE_PONG);
    check_bool("websocket accepts only known opcodes",
               ant_ws_parse_frame(frame, sizeof(frame), false, false, true, 1024, &parsed) == ANT_WS_FRAME_OK, known);
  }
}

static void test_websocket_frame_limit(void) {
  uint8_t header[] = { 0x82, 0xff, 0, 0, 0, 0, 0, 0x10, 0, 0, 0x01, 0x02, 0x03, 0x04 };
  ant_ws_frame_t frame = {0};
  check_bool("websocket oversize frame rejected from its header",
             ant_ws_parse_frame(header, 10, true, false, true, 1024, &frame) == ANT_WS_FRAME_TOO_BIG, true);
  check_bool("websocket frame within limit waits for payload",
             ant_ws_parse_frame(header, sizeof(header), true, false, true, 2u * 1024u * 1024u, &frame) == ANT_WS_FRAME_INCOMPLETE, true);
}

static ant_http1_parse_result_t parse_with_body_limit(const char *raw, size_t limit, bool *overflow) {
  ant_http1_conn_parser_t cp;
  ant_http1_parsed_request_t req = {0};
  size_t consumed = 0;
  ant_http1_conn_parser_init(&cp);
  cp.ctx.body_limit = limit;
  ant_http1_parse_result_t result = ant_http1_conn_parser_execute(&cp, raw, strlen(raw), &req, &consumed);
  *overflow = cp.ctx.body_overflow;
  ant_http1_free_parsed_request(&req);
  ant_http1_conn_parser_free(&cp);
  return result;
}

static void test_http1_body_limit(void) {
  bool overflow = false;
  const char *declared = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 100000000000\r\n\r\n";
  check_bool("http1 oversize content-length rejected before body",
             parse_with_body_limit(declared, 1024, &overflow) == ANT_HTTP1_PARSE_ERROR && overflow, true);

  const char *chunked = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n8\r\n12345678\r\n8\r\n12345678\r\n";
  check_bool("http1 oversize chunked body rejected",
             parse_with_body_limit(chunked, 12, &overflow) == ANT_HTTP1_PARSE_ERROR && overflow, true);

  const char *small = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nabcd";
  check_bool("http1 body within limit parses",
             parse_with_body_limit(small, 4, &overflow) == ANT_HTTP1_PARSE_OK && !overflow, true);
  check_bool("http1 zero body limit is unlimited",
             parse_with_body_limit(declared, 0, &overflow) == ANT_HTTP1_PARSE_INCOMPLETE && !overflow, true);
}

static void test_http1_trailers_ignored(void) {
  const char *raw =
    "POST / HTTP/1.1\r\nHost: good.example\r\nTransfer-Encoding: chunked\r\n\r\n"
    "1\r\na\r\n0\r\nHost: evil.example\r\nX-Late: 1\r\n\r\n";
  ant_http1_parsed_request_t req;
  check_bool("http1 chunked request with trailers parses",
             ant_http1_parse_request(raw, strlen(raw), 0, &req, NULL, NULL) == ANT_HTTP1_PARSE_OK, true);
  check_str("http1 trailer cannot replace host", req.host, "good.example");
  size_t count = 0;
  for (const ant_http_header_t *h = req.headers; h; h = h->next) count++;
  check_size("http1 trailers stay out of headers", count, 2);
  check_size("http1 chunked body kept beside trailers", req.body_len, 1);
  ant_http1_free_parsed_request(&req);
}

static void test_remote_esm_downgrade(void) {
  char *same = esm_resolve(NULL, "./dep.js", "https://example.com/mod.js", NULL);
  check_str("https module resolves https dependency", same, "https://example.com/dep.js");
  free(same);
  check_bool("https module cannot import http",
             esm_resolve(NULL, "http://example.com/dep.js", "https://example.com/mod.js", NULL) == NULL, true);
  char *plain = esm_resolve(NULL, "http://example.com/dep.js", "http://example.com/mod.js", NULL);
  check_str("http module may import http", plain, "http://example.com/dep.js");
  free(plain);
}

int main(void) {
  test_websocket_handshake();
  test_websocket_frames();
  test_websocket_frame_limit();
  test_websocket_reserved_opcodes();
  test_http1_body_limit();
  test_http1_trailers_ignored();
  test_remote_esm_downgrade();
  test_eventsource_format_and_parse();

  if (failures) {
    fprintf(stderr, "%d protocol test failure(s)\n", failures);
    return 1;
  }
  puts("http protocol tests passed");
  return 0;
}
