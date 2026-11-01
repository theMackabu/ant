#include "utf8.h"
#include "escape.h"
#include "ascii.h"

static inline size_t decode_hex_escape(const uint8_t *in, size_t pos, uint8_t *out, size_t *out_pos) {
  uint32_t cp = (unhex(in[pos + 2]) << 4U) | unhex(in[pos + 3]);
  *out_pos += utf8_encode(cp, (char *)&out[*out_pos]);
  return 2;
}

static inline bool is_octal_at(const uint8_t *in, size_t i, size_t end) {
  return i < end && in[i] >= '0' && in[i] <= '7';
}

static size_t decode_octal_escape(const uint8_t *in, size_t pos, size_t end, uint8_t *out, size_t *out_pos) {
  uint8_t c = in[pos + 1];
  size_t extra = 0;
  int val = c - '0';
  
  if (is_octal_at(in, pos + 2, end)) {
    val = val * 8 + (in[pos + 2] - '0'); extra++;
    if (is_octal_at(in, pos + 3, end) && val * 8 + (in[pos + 3] - '0') <= 255) {
      val = val * 8 + (in[pos + 3] - '0'); extra++;
    }
  }
  
  *out_pos += utf8_encode((uint32_t)val, (char *)&out[*out_pos]);
  return extra;
}

static inline void escape_flag(uint8_t *flags, escape_flags_t flag) {
  if (flags) *flags |= flag;
}

static size_t decode_unicode_braced(const uint8_t *in, size_t pos, size_t end, uint8_t *out, size_t *out_pos, uint8_t *flags) {
  uint32_t cp = 0;
  size_t i = pos + 3;
  
  for (; i < end && ant_ascii_is_xdigit(in[i]); i++) if (cp <= 0x10FFFF) cp = (cp << 4) | unhex(in[i]);
  if (i < end && in[i] == '}' && i > pos + 3 && cp <= 0x10FFFF) {
    *out_pos += utf8_encode(cp, (char *)&out[*out_pos]);
    return i - pos - 1;
  }
  
  bool closed = i < end && in[i] == '}' && i > pos + 3;
  escape_flag(flags, closed ? ESCAPE_BAD_CODEPOINT : ESCAPE_BAD_UNICODE);
  out[(*out_pos)++] = 'u';
  return 0;
}

static size_t decode_unicode_fixed(const uint8_t *in, size_t pos, size_t end, uint8_t *out, size_t *out_pos) {
  uint32_t cp =
    (unhex(in[pos + 2]) << 12U) | (unhex(in[pos + 3]) << 8U) |
    (unhex(in[pos + 4]) << 4U)  |  unhex(in[pos + 5]);

  if (cp >= 0xD800 && cp <= 0xDBFF && pos + 11 < end &&
      in[pos + 6] == '\\' && in[pos + 7] == 'u' &&
      ant_ascii_is_xdigit(in[pos + 8]) && ant_ascii_is_xdigit(in[pos + 9]) &&
      ant_ascii_is_xdigit(in[pos + 10]) && ant_ascii_is_xdigit(in[pos + 11])) {
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

const char *escape_error_message(uint8_t flags, uint8_t escape_char, escape_context_t context) {
  if (flags & ESCAPE_BAD_HEX) return "Invalid hexadecimal escape sequence";
  if (flags & ESCAPE_BAD_CODEPOINT) return "Undefined Unicode code-point";
  if (flags & ESCAPE_BAD_UNICODE) return "Invalid Unicode escape sequence";
  if (!(flags & ESCAPE_LEGACY) || context == ESCAPE_CONTEXT_SLOPPY) return NULL;

  bool eight_nine = escape_char == '8' || escape_char == '9';
  if (context == ESCAPE_CONTEXT_TEMPLATE) return eight_nine
    ? "\\8 and \\9 are not allowed in template strings."
    : "Octal escape sequences are not allowed in template strings.";
  
  return eight_nine
    ? "\\8 and \\9 are not allowed in strict mode."
    : "Octal escape sequences are not allowed in strict mode.";
}

size_t decode_escape(const uint8_t *in, size_t pos, size_t end, uint8_t *out, size_t *out_pos, uint8_t quote, uint8_t *flags) {
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
    case '\n': break;
    case '\r': advance = pos + 2 < end && in[pos + 2] == '\n'; break;
    case 0xE2:
      if (pos + 3 < end && in[pos + 2] == 0x80 && (in[pos + 3] == 0xA8 || in[pos + 3] == 0xA9)) advance = 2;
      else out[(*out_pos)++] = c;
      break;
    case '0':
      if (pos + 2 < end && in[pos + 2] >= '8' && in[pos + 2] <= '9') escape_flag(flags, ESCAPE_LEGACY);
      if (!is_octal_at(in, pos + 2, end)) { out[(*out_pos)++] = '\0'; break; }
      [[fallthrough]];
    case '1': case '2': case '3': case '4': case '5': case '6': case '7':
      escape_flag(flags, ESCAPE_LEGACY);
      advance = decode_octal_escape(in, pos, end, out, out_pos);
      break;
    case '8': case '9':
      escape_flag(flags, ESCAPE_LEGACY);
      out[(*out_pos)++] = c;
      break;
    case 'x':
      if (pos + 3 < end && ant_ascii_is_xdigit(in[pos + 2]) && ant_ascii_is_xdigit(in[pos + 3])) {
        advance = decode_hex_escape(in, pos, out, out_pos);
      } else {
        escape_flag(flags, ESCAPE_BAD_HEX);
        out[(*out_pos)++] = c;
      }
      break;
    case 'u':
      if (pos + 2 < end && in[pos + 2] == '{') {
        advance = decode_unicode_braced(in, pos, end, out, out_pos, flags);
      } else if (
        pos + 5 < end && ant_ascii_is_xdigit(in[pos + 2]) && ant_ascii_is_xdigit(in[pos + 3]) &&
        ant_ascii_is_xdigit(in[pos + 4]) && ant_ascii_is_xdigit(in[pos + 5])
      ) advance = decode_unicode_fixed(in, pos, end, out, out_pos);
      else {
        escape_flag(flags, ESCAPE_BAD_UNICODE);
        out[(*out_pos)++] = c;
      }
      break;
    default:
      out[(*out_pos)++] = (c == quote) ? quote : c;
      break;
  }

  return advance;
}
