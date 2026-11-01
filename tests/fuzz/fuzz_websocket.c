// WebSocket frame codec (src/http/websocket.c).
//
// Input: a flags byte, a max-payload byte, then frame bytes.
// Properties:
//   - parsing is prefix-stable: a prefix of the input never yields a result
//     other than INCOMPLETE or the full input's result, and every prefix of
//     an accepted frame is INCOMPLETE;
//   - in-place and copying parses agree, and accepted frames obey RFC 6455
//     (known opcodes; control frames have FIN set and payload <= 125);
//   - ant_ws_unmask matches a per-byte XOR at any alignment and undoes itself;
//   - ant_ws_encode_frame output parses back to the same frame.
#include "fuzz.h"
#include "http/websocket.h"

static ant_ws_frame_result_t parse_copy(
  const uint8_t *data, size_t len, bool require_mask, bool allow_rsv1,
  bool inplace, size_t max_payload, ant_ws_frame_t *frame, uint8_t **buf_out
) {
  // in-place parsing unmasks the caller's bytes, so give it its own copy
  uint8_t *buf = fuzz_dup(data, len);
  ant_ws_frame_result_t r = ant_ws_parse_frame(buf, len, require_mask, allow_rsv1, inplace, max_payload, frame);
  if (buf_out) *buf_out = buf;
  else free(buf);
  return r;
}

static void check_parse(const uint8_t *data, size_t len, uint8_t flags, size_t max_payload) {
  bool require_mask = flags & 1, allow_rsv1 = flags & 2;
  ant_ws_frame_t copied, inplace;
  uint8_t *inplace_buf = NULL;

  ant_ws_frame_result_t r = parse_copy(data, len, require_mask, allow_rsv1, false, max_payload, &copied, NULL);
  ant_ws_frame_result_t ri = parse_copy(data, len, require_mask, allow_rsv1, true, max_payload, &inplace, &inplace_buf);
  FUZZ_CHECK(r == ri);

  if (r == ANT_WS_FRAME_OK) {
    FUZZ_CHECK(copied.owned && !inplace.owned);
    FUZZ_CHECK(copied.consumed_len <= len && copied.consumed_len == inplace.consumed_len);
    FUZZ_CHECK(copied.payload_len <= max_payload && copied.payload_len == inplace.payload_len);
    FUZZ_CHECK(copied.consumed_len >= copied.payload_len + 2);
    FUZZ_CHECK(copied.opcode == inplace.opcode && copied.fin == inplace.fin);
    FUZZ_CHECK(copied.rsv1 == inplace.rsv1 && copied.masked == inplace.masked);
    FUZZ_CHECK(!require_mask || copied.masked);
    FUZZ_CHECK(allow_rsv1 || !copied.rsv1);
    FUZZ_CHECK(copied.payload_len == 0 || memcmp(copied.payload, inplace.payload, copied.payload_len) == 0);
    FUZZ_CHECK(inplace.payload >= inplace_buf && inplace.payload + inplace.payload_len <= inplace_buf + len);
    if (copied.opcode >= 0x8) FUZZ_CHECK(copied.fin && copied.payload_len <= 125 && !copied.rsv1);
    FUZZ_CHECK(copied.opcode <= ANT_WS_OPCODE_BINARY || (copied.opcode >= ANT_WS_OPCODE_CLOSE && copied.opcode <= ANT_WS_OPCODE_PONG));
  }

  // every result is decided by bytes the frame has already delivered
  size_t limit = r == ANT_WS_FRAME_OK ? copied.consumed_len : len;
  for (size_t k = 0; k < limit; k++) {
    if (k > 16 && k + 1 < limit) k = limit - 1;
    ant_ws_frame_t prefix;
    ant_ws_frame_result_t rp = parse_copy(data, k, require_mask, allow_rsv1, false, max_payload, &prefix, NULL);
    FUZZ_CHECK(rp == ANT_WS_FRAME_INCOMPLETE || (r != ANT_WS_FRAME_OK && rp == r));
    ant_ws_frame_clear(&prefix);
  }

  ant_ws_frame_clear(&copied);
  ant_ws_frame_clear(&inplace);
  free(inplace_buf);
}

static void check_unmask(const uint8_t *data, size_t len, const uint8_t mask[4]) {
  if (len > 64) len = 64;
  for (size_t off = 0; off < 8 && off <= len; off++) {
    size_t n = len - off;
    uint8_t *buf = fuzz_dup(data + off, n);
    ant_ws_unmask(buf, n, mask);
    for (size_t i = 0; i < n; i++) FUZZ_CHECK(buf[i] == (uint8_t)(data[off + i] ^ mask[i & 3]));
    ant_ws_unmask(buf, n, mask);
    FUZZ_CHECK(n == 0 || memcmp(buf, data + off, n) == 0);
    free(buf);
  }
}

static void check_roundtrip(const uint8_t *payload, size_t len, uint8_t flags) {
  static const ant_ws_opcode_t opcodes[] = {
    ANT_WS_OPCODE_CONTINUATION, ANT_WS_OPCODE_TEXT, ANT_WS_OPCODE_BINARY,
    ANT_WS_OPCODE_CLOSE, ANT_WS_OPCODE_PING, ANT_WS_OPCODE_PONG,
  };
  ant_ws_opcode_t opcode = opcodes[(flags >> 4) % (sizeof(opcodes) / sizeof(*opcodes))];
  bool mask = flags & 4, rsv1 = (flags & 8) && opcode < 0x8;
  if (opcode >= 0x8 && len > 125) len = 125;

  size_t frame_len = 0;
  uint8_t *frame = ant_ws_encode_frame(opcode, payload, len, mask, rsv1, &frame_len);
  FUZZ_CHECK(frame != NULL);

  ant_ws_frame_t parsed;
  FUZZ_CHECK(ant_ws_parse_frame(frame, frame_len, mask, rsv1, false, SIZE_MAX, &parsed) == ANT_WS_FRAME_OK);
  FUZZ_CHECK(parsed.consumed_len == frame_len);
  FUZZ_CHECK(parsed.opcode == opcode && parsed.fin && parsed.masked == mask && parsed.rsv1 == rsv1);
  FUZZ_CHECK(parsed.payload_len == len && (len == 0 || memcmp(parsed.payload, payload, len) == 0));

  ant_ws_frame_clear(&parsed);
  free(frame);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz_input_t in = { data, size };
  uint8_t flags = fuzz_u8(&in);
  uint8_t max_byte = fuzz_u8(&in);
  // small limits exercise TOO_BIG; 0xff means unlimited
  size_t max_payload = max_byte == 0xff ? SIZE_MAX : (size_t)max_byte * 64;

  check_parse(in.data, in.size, flags, max_payload);
  uint8_t mask[4] = { flags, max_byte, (uint8_t)size, 0xa5 };
  check_unmask(in.data, in.size, mask);
  check_roundtrip(in.data, in.size, flags);
  return 0;
}
