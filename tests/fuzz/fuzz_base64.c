// Base64 codecs (src/base64.c).
//
// Input: a control byte, then bytes used both as data to encode and as text
// to decode.
// Properties:
//   - standard and URL-safe encodings decode back to the input through every
//     decoder (strict, loose, and decode_into in each last-chunk mode);
//   - decode_into never reports reading or writing more than it was given,
//     and decoding in pieces with a small output buffer, resuming at the
//     reported read offset, matches decoding into one large buffer;
//   - arbitrary text never crashes a decoder, and text the libbase64 decoder
//     (Buffer, data: URLs) accepts decodes the same through decode_into.
#include "fuzz.h"
#include "base64.h"

static void check_decode_into(
  const char *src, size_t len, bool url, ant_base64_last_chunk_t mode,
  const uint8_t *expect, size_t expect_len
) {
  size_t cap = len / 4 * 3 + 3;
  uint8_t *dst = malloc(cap);
  if (!dst) abort();
  size_t read = 0, written = 0;
  const char *error = NULL;
  bool ok = ant_base64_decode_into(src, len, url, mode, dst, cap, &read, &written, &error);
  if (expect) {
    FUZZ_CHECK(ok && !error);
    FUZZ_CHECK(read == len && written == expect_len);
    FUZZ_CHECK(written == 0 || memcmp(dst, expect, written) == 0);
  } else if (ok) {
    FUZZ_CHECK(read <= len && written <= cap);
  }
  free(dst);
}

static void check_roundtrip(const uint8_t *data, size_t len) {
  size_t enc_len = 0, dec_len = 0;
  char *enc = ant_base64_encode(data, len, &enc_len);
  FUZZ_CHECK(enc && enc_len == 4 * ((len + 2) / 3) && strlen(enc) == enc_len);

  uint8_t *dec = ant_base64_decode(enc, enc_len, &dec_len);
  FUZZ_CHECK(dec && dec_len == len && (len == 0 || memcmp(dec, data, len) == 0));
  free(dec);

  dec = ant_base64_decode_loose(enc, enc_len, &dec_len);
  FUZZ_CHECK(dec && dec_len == len && (len == 0 || memcmp(dec, data, len) == 0));
  free(dec);

  check_decode_into(enc, enc_len, false, ANT_BASE64_STRICT, data, len);
  check_decode_into(enc, enc_len, false, ANT_BASE64_LOOSE, data, len);
  check_decode_into(enc, enc_len, false, ANT_BASE64_STOP_BEFORE_PARTIAL, data, len);
  free(enc);

  size_t url_len = 0;
  char *url = ant_base64url_encode(data, len, &url_len);
  FUZZ_CHECK(url && strlen(url) == url_len);
  FUZZ_CHECK(!memchr(url, '+', url_len) && !memchr(url, '/', url_len) && !memchr(url, '=', url_len));

  dec = ant_base64_decode_loose(url, url_len, &dec_len);
  FUZZ_CHECK(dec && dec_len == len && (len == 0 || memcmp(dec, data, len) == 0));
  free(dec);

  // unpadded input is only complete in loose mode
  check_decode_into(url, url_len, true, ANT_BASE64_LOOSE, data, len);
  free(url);
}

// Decodes with an output buffer of max_len bytes at a time, resuming at the
// reported read offset, the way Uint8Array.prototype.setFromBase64 callers
// stitch results together.
static void check_resumable(const char *src, size_t len, bool url, ant_base64_last_chunk_t mode, size_t step) {
  size_t cap = len / 4 * 3 + 3;
  uint8_t *whole = malloc(cap);
  if (!whole) abort();
  size_t whole_read = 0, whole_written = 0;
  const char *error = NULL;
  bool whole_ok = ant_base64_decode_into(src, len, url, mode, whole, cap, &whole_read, &whole_written, &error);

  uint8_t *pieces = malloc(cap);
  if (!pieces) abort();
  size_t off = 0, out = 0;
  bool ok = true;
  for (int guard = 0; guard < 4096 && ok; guard++) {
    uint8_t *chunk = malloc(step);
    if (!chunk) abort();
    size_t read = 0, written = 0;
    ok = ant_base64_decode_into(src + off, len - off, url, mode, chunk, step, &read, &written, &error);
    if (ok) {
      FUZZ_CHECK(read <= len - off && written <= step && out + written <= cap);
      memcpy(pieces + out, chunk, written);
      out += written;
      off += read;
    }
    free(chunk);
    if (!ok || written < step || read == 0) break;
  }

  // a decode that succeeds whole succeeds in pieces with the same bytes
  if (whole_ok && ok && off == whole_read) {
    FUZZ_CHECK(out == whole_written);
    FUZZ_CHECK(out == 0 || memcmp(pieces, whole, out) == 0);
  }
  free(whole);
  free(pieces);
}

static void check_arbitrary(const char *text, size_t len, uint8_t control) {
  size_t strict_len = 0, loose_len = 0;
  uint8_t *strict = ant_base64_decode(text, len, &strict_len);
  uint8_t *loose = ant_base64_decode_loose(text, len, &loose_len);
  FUZZ_CHECK(loose && loose_len <= len / 4 * 3 + 3);
  // what libbase64 accepts, the spec decoder (Uint8Array.fromBase64)
  // accepts in loose mode with the same bytes
  if (strict) check_decode_into(text, len, false, ANT_BASE64_LOOSE, strict, strict_len);
  free(strict);
  free(loose);

  bool url = control & 1;
  ant_base64_last_chunk_t mode = (ant_base64_last_chunk_t)((control >> 1) % 3);
  check_decode_into(text, len, url, mode, NULL, 0);
  check_resumable(text, len, url, mode, 1 + (control >> 3) % 8);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz_input_t in = { data, size };
  uint8_t control = fuzz_u8(&in);
  check_roundtrip(in.data, in.size);

  char *text = fuzz_dup(in.data, in.size);
  check_arbitrary(text, in.size, control);
  free(text);
  return 0;
}
