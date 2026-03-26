#include "utf8.h"
#include "escape.h"

static inline size_t decode_hex_escape(const uint8_t *in, size_t pos, uint8_t *out, size_t *out_pos) {
  uint32_t cp = (unhex(in[pos + 2]) << 4U) | unhex(in[pos + 3]);
  *out_pos += utf8_encode(cp, (char *)&out[*out_pos]);
  return 2;
}

static size_t decode_octal_escape(const uint8_t *in, size_t pos, uint8_t *out, size_t *out_pos) {
  uint8_t c = in[pos + 1];
  size_t extra = 0;
  int val = c - '0';
  
  if (in[pos + 2] >= '0' && in[pos + 2] <= '7') {
    val = val * 8 + (in[pos + 2] - '0'); extra++;
    if (in[pos + 3] >= '0' && in[pos + 3] <= '7' && val * 8 + (in[pos + 3] - '0') <= 255) {
      val = val * 8 + (in[pos + 3] - '0'); extra++;
    }
  }
  
  *out_pos += utf8_encode((uint32_t)val, (char *)&out[*out_pos]);
  return extra;
}

static size_t decode_unicode_braced(const uint8_t *in, size_t pos, size_t end, uint8_t *out, size_t *out_pos) {
  uint32_t cp = 0;
  size_t i = pos + 3;
  
  while (i < end && is_xdigit(in[i])) { cp = (cp << 4) | unhex(in[i]); i++; }
  if (i < end && in[i] == '}') {
    *out_pos += utf8_encode(cp, (char *)&out[*out_pos]);
    return i - pos - 1;
  }
  
  out[(*out_pos)++] = 'u';
  return 0;
}

static size_t decode_unicode_fixed(const uint8_t *in, size_t pos, size_t end, uint8_t *out, size_t *out_pos) {
  uint32_t cp =
    (unhex(in[pos + 2]) << 12U) | (unhex(in[pos + 3]) << 8U) |
    (unhex(in[pos + 4]) << 4U)  |  unhex(in[pos + 5]);

  if (cp >= 0xD800 && cp <= 0xDBFF && pos + 11 < end &&
      in[pos + 6] == '\\' && in[pos + 7] == 'u' &&
      is_xdigit(in[pos + 8]) && is_xdigit(in[pos + 9]) &&
      is_xdigit(in[pos + 10]) && is_xdigit(in[pos + 11])) {
    uint32_t lo =
      (unhex(in[pos + 8]) << 12U) | (unhex(in[pos + 9]) << 8U) |
      (unhex(in[pos + 10]) << 4U)  |  unhex(in[pos + 11]);
    if (lo >= 0xDC00 && lo <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
      *out_pos += utf8_encode(cp, (char *)&out[*out_pos]);
      return 10;
    }
  }

  *out_pos += utf8_encode(cp, (char *)&out[*out_pos]);
  return 4;
}

size_t decode_escape(const uint8_t *in, size_t pos, size_t end, uint8_t *out, size_t *out_pos, uint8_t quote) {
  uint8_t c = in[pos + 1];
  size_t advance = 0;

  switch (c) {
    case 'n':  out[(*out_pos)++] = '\n'; break;
    case 't':  out[(*out_pos)++] = '\t'; break;
    case 'r':  out[(*out_pos)++] = '\r'; break;
    case 'v':  out[(*out_pos)++] = '\v'; break;
    case 'f':  out[(*out_pos)++] = '\f'; break;
    case 'b':  out[(*out_pos)++] = '\b'; break;
    case '\\': out[(*out_pos)++] = '\\'; break;
    case '0':
      if (!(in[pos + 2] >= '0' && in[pos + 2] <= '7')) { out[(*out_pos)++] = '\0'; break; }
      __attribute__((fallthrough));
    case '1': case '2': case '3': case '4': case '5': case '6': case '7':
      advance = decode_octal_escape(in, pos, out, out_pos);
      break;
    case 'x':
      if (pos + 3 < end && is_xdigit(in[pos + 2]) && is_xdigit(in[pos + 3])) {
        advance = decode_hex_escape(in, pos, out, out_pos);
      } else out[(*out_pos)++] = c;
      break;
    case 'u':
      if (pos + 2 < end && in[pos + 2] == '{') {
        advance = decode_unicode_braced(in, pos, end, out, out_pos);
      } else if (
        pos + 5 < end && is_xdigit(in[pos + 2]) && is_xdigit(in[pos + 3]) &&
        is_xdigit(in[pos + 4]) && is_xdigit(in[pos + 5])
      ) advance = decode_unicode_fixed(in, pos, end, out, out_pos);
      else out[(*out_pos)++] = c;
      break;
    default:
      out[(*out_pos)++] = (c == quote) ? quote : c;
      break;
  }

  return advance;
}
