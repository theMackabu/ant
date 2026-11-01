// Text codecs: string escapes (src/escape.c), UTF-8/WTF-8 helpers
// (src/utf8.c), and URL form encoding (src/modules/url/state.c).
//
// Input: a control byte, then text.
// Properties:
//   - decoding escapes the way the lexer does stays inside the literal and
//     never writes more bytes than it reads;
//   - repairing invalid UTF-8 (or WTF-8) yields valid UTF-8 (or WTF-8), and
//     valid input is left alone;
//   - TextDecoder's one-shot decode yields valid UTF-8, and fatal mode fails
//     exactly when the input is not valid UTF-8;
//   - Latin-1 -> UTF-8 -> Latin-1 and JSON quoting both round-trip;
//   - form URL decoding inverts form URL encoding.
#include "fuzz.h"
#include "escape.h"
#include "utf8.h"
#include "modules/url.h"

static void check_escapes(const uint8_t *in, size_t len, uint8_t quote) {
  uint8_t *out = malloc(len ? len : 1);
  if (!out) abort();
  size_t di = 0, i = 0;
  uint8_t flags = 0;

  while (i < len) {
    if (in[i] == '\\' && i + 1 < len) {
      size_t before = di;
      size_t adv = decode_escape(in, i, len, out, &di, quote, &flags);
      FUZZ_CHECK(i + 2 + adv <= len);
      FUZZ_CHECK(di - before <= 2 + adv);
      i += 2 + adv;
      continue;
    }
    out[di++] = in[i++];
  }

  FUZZ_CHECK(di <= len);
  escape_error_message(flags, len ? in[len - 1] : 0, (escape_context_t)(quote % 3));
  free(out);
}

static void check_repair(const char *s, size_t len) {
  bool ascii = false;
  size_t valid = utf8_valid_prefix(s, len, &ascii);
  FUZZ_CHECK(valid <= len);
  FUZZ_CHECK(utf8_validate_bytes(s, valid));
  FUZZ_CHECK(utf8_validate_bytes(s, len) == (valid == len));
  if (ascii) for (size_t i = 0; i < valid; i++) FUZZ_CHECK((unsigned char)s[i] < 0x80);

  if (valid < len) {
    size_t out_len = 0;
    char *fixed = utf8_replace_invalid(s, len, valid, &out_len);
    FUZZ_CHECK(fixed && utf8_validate_bytes(fixed, out_len));
    FUZZ_CHECK(memcmp(fixed, s, valid) == 0);
    free(fixed);
  }

  size_t wtf_len = 0;
  char *wtf = wtf8_replace_invalid(s, len, &wtf_len);
  if (wtf) {
    FUZZ_CHECK(!wtf8_validate_bytes(s, len));
    FUZZ_CHECK(wtf8_validate_bytes(wtf, wtf_len));
    free(wtf);
  } else FUZZ_CHECK(wtf8_validate_bytes(s, len));

  // every valid UTF-8 string is valid WTF-8
  if (valid == len) FUZZ_CHECK(wtf8_validate_bytes(s, len));
}

static void check_text_decoder(const uint8_t *src, size_t len, bool ignore_bom) {
  char *out = malloc(len * 3 + 1);
  if (!out) abort();

  utf8_dec_t dec = { .ignore_bom = ignore_bom };
  utf8proc_ssize_t n = utf8_whatwg_decode(&dec, src, len, out, false, false);
  FUZZ_CHECK(n >= 0 && (size_t)n <= len * 3);
  FUZZ_CHECK(utf8_validate_bytes(out, (size_t)n));

  bool bom = !ignore_bom && len >= 3 && src[0] == 0xef && src[1] == 0xbb && src[2] == 0xbf;
  size_t body = bom ? 3 : 0;
  dec = (utf8_dec_t){ .ignore_bom = ignore_bom };
  utf8proc_ssize_t fatal = utf8_whatwg_decode(&dec, src, len, out, true, false);
  bool valid = utf8_validate_bytes((const char *)src + body, len - body);
  FUZZ_CHECK((fatal >= 0) == valid);
  if (valid) FUZZ_CHECK((size_t)fatal == len - body && memcmp(out, src + body, len - body) == 0);
  free(out);
}

static void check_latin1(const uint8_t *src, size_t len) {
  size_t utf8_len = 0, back_len = 0;
  bool is_latin1 = false;
  char *utf8 = latin1_to_utf8(src, len, &utf8_len);
  FUZZ_CHECK(utf8 && utf8_validate_bytes(utf8, utf8_len));
  uint8_t *back = utf8_to_latin1(utf8, utf8_len, &back_len, &is_latin1);
  FUZZ_CHECK(back && is_latin1 && back_len == len);
  FUZZ_CHECK(len == 0 || memcmp(back, src, len) == 0);
  free(utf8);
  free(back);
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool read_u16_escape(const char *q, size_t len, size_t i, uint32_t *unit) {
  if (i + 6 > len || q[i] != '\\' || q[i + 1] != 'u') return false;
  *unit = 0;
  for (int k = 2; k < 6; k++) {
    int v = hex_value(q[i + k]);
    if (v < 0) return false;
    *unit = (*unit << 4) | (uint32_t)v;
  }
  return true;
}

// Decodes a JSON string literal into WTF-8, the way JSON.parse would.
static char *json_unquote(const char *q, size_t len, size_t *out_len) {
  FUZZ_CHECK(len >= 2 && q[0] == '"' && q[len - 1] == '"');
  char *out = malloc(len * 2 + 1);
  if (!out) abort();
  size_t o = 0;

  for (size_t i = 1; i + 1 < len;) {
    unsigned char c = (unsigned char)q[i];
    FUZZ_CHECK(c >= 0x20 && c != '"');
    if (c != '\\') { out[o++] = (char)c; i++; continue; }

    FUZZ_CHECK(i + 2 < len);
    char e = q[i + 1];
    uint32_t cp = 0;
    switch (e) {
      case '"': case '\\': case '/': out[o++] = e; i += 2; continue;
      case 'b': out[o++] = '\b'; i += 2; continue;
      case 'f': out[o++] = '\f'; i += 2; continue;
      case 'n': out[o++] = '\n'; i += 2; continue;
      case 'r': out[o++] = '\r'; i += 2; continue;
      case 't': out[o++] = '\t'; i += 2; continue;
      case 'u': {
        FUZZ_CHECK(read_u16_escape(q, len - 1, i, &cp));
        i += 6;
        uint32_t lo = 0;
        if (cp >= 0xD800 && cp <= 0xDBFF && read_u16_escape(q, len - 1, i, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 6;
        }
        o += (size_t)utf8_encode(cp, out + o);
        continue;
      }
      default: FUZZ_FAIL("unexpected JSON escape");
    }
  }

  *out_len = o;
  return out;
}

// Joins surrogate halves encoded separately (ED A0-AF xx ED B0-BF xx) into
// one 4-byte sequence: both spell the same UTF-16 string.
static size_t join_split_pairs(const char *s, size_t len, char *out) {
  const unsigned char *b = (const unsigned char *)s;
  size_t o = 0;
  for (size_t i = 0; i < len;) {
    if (i + 6 <= len && b[i] == 0xED && b[i + 1] >= 0xA0 && b[i + 1] <= 0xAF &&
        b[i + 3] == 0xED && b[i + 4] >= 0xB0 && b[i + 4] <= 0xBF) {
      uint32_t hi = 0xD000 | ((uint32_t)(b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F);
      uint32_t lo = 0xD000 | ((uint32_t)(b[i + 4] & 0x3F) << 6) | (b[i + 5] & 0x3F);
      o += (size_t)utf8_encode(0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00), out + o);
      i += 6;
    } else out[o++] = s[i++];
  }
  return o;
}

static void check_json_quote(const char *s, size_t len) {
  if (!wtf8_validate_bytes(s, len)) return;
  size_t q_len = 0, back_len = 0;
  char *q = utf8_json_quote(s, len, &q_len);
  FUZZ_CHECK(q);
  char *back = json_unquote(q, q_len, &back_len);
  char *want = malloc(len ? len : 1);
  if (!want) abort();
  size_t want_len = join_split_pairs(s, len, want);
  FUZZ_CHECK(back_len == want_len && (want_len == 0 || memcmp(back, want, want_len) == 0));
  free(want);
  // lone surrogates are escaped, so the quoted text is always valid UTF-8
  FUZZ_CHECK(utf8_validate_bytes(q, q_len));
  free(q);
  free(back);
}

static void check_form_urlencoded(const char *s, size_t len) {
  char *enc = form_urlencode_n(s, len);
  FUZZ_CHECK(enc && strlen(enc) <= len * 3);
  size_t dec_len = 0;
  char *dec = form_urldecode_len(enc, &dec_len);
  FUZZ_CHECK(dec);

  size_t valid = utf8_valid_prefix(s, len, NULL);
  if (valid == len) {
    FUZZ_CHECK(dec_len == len && (len == 0 || memcmp(dec, s, len) == 0));
  } else {
    size_t fixed_len = 0;
    char *fixed = utf8_replace_invalid(s, len, valid, &fixed_len);
    FUZZ_CHECK(fixed && dec_len == fixed_len && memcmp(dec, fixed, fixed_len) == 0);
    free(fixed);
  }
  free(enc);
  free(dec);

  // decoding arbitrary text always yields valid UTF-8
  char *z = malloc(len + 1);
  if (!z) abort();
  if (len) memcpy(z, s, len);
  z[len] = '\0';
  size_t n = 0;
  char *raw = url_decode_component_len(z, &n);
  FUZZ_CHECK(raw && utf8_validate_bytes(raw, n));
  free(raw);
  raw = form_urldecode_len(z, &n);
  FUZZ_CHECK(raw && utf8_validate_bytes(raw, n));
  free(raw);
  free(z);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz_input_t in = { data, size };
  uint8_t control = fuzz_u8(&in);
  static const uint8_t quotes[] = { '"', '\'', '`', 0 };

  uint8_t *bytes = fuzz_dup(in.data, in.size);
  const char *text = (const char *)bytes;
  check_escapes(bytes, in.size, quotes[control & 3]);
  check_repair(text, in.size);
  check_text_decoder(bytes, in.size, control & 4);
  check_latin1(bytes, in.size);
  check_json_quote(text, in.size);
  check_form_urlencoded(text, in.size);
  free(bytes);
  return 0;
}
