#include "utf8.h"
#include <string.h>

int utf8_sequence_length(unsigned char first_byte) {
  if ((first_byte & 0x80) == 0) return 1;
  if ((first_byte & 0xE0) == 0xC0) return 2;
  if ((first_byte & 0xF0) == 0xE0) return 3;
  if ((first_byte & 0xF8) == 0xF0) return 4;
  return -1;
}

int utf8_encode(uint32_t cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  } else {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }
}

uint32_t utf8_decode(const unsigned char *buf, size_t len, int *seq_len) {
  if (len == 0) { *seq_len = 0; return 0; }
  
  unsigned char first = buf[0];
  
  if (first < 0x80) { *seq_len = 1; return first; }
  if ((first & 0xE0) == 0xC0 && len >= 2) {
    if ((buf[1] & 0xC0) != 0x80) { *seq_len = 1; return 0xFFFD; }
    *seq_len = 2;
    return ((first & 0x1F) << 6) | (buf[1] & 0x3F);
  }
  if ((first & 0xF0) == 0xE0 && len >= 3) {
    if ((buf[1] & 0xC0) != 0x80 || (buf[2] & 0xC0) != 0x80) { *seq_len = 1; return 0xFFFD; }
    *seq_len = 3;
    return ((first & 0x0F) << 12) | ((buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
  }
  if ((first & 0xF8) == 0xF0 && len >= 4) {
    if ((buf[1] & 0xC0) != 0x80 || (buf[2] & 0xC0) != 0x80 || (buf[3] & 0xC0) != 0x80) { *seq_len = 1; return 0xFFFD; }
    *seq_len = 4;
    return ((first & 0x07) << 18) | ((buf[1] & 0x3F) << 12) | ((buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
  }
  
  *seq_len = 1;
  return 0xFFFD;
}

size_t utf8_strlen(const char *str, size_t byte_len) {
  size_t count = 0;
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  while (p < end) {
    int seq_len = utf8_sequence_length(*p);
    if (seq_len <= 0 || (size_t)seq_len > (size_t)(end - p)) {
      count++; p++;
    } else { count++; p += seq_len; }
  }
  return count;
}

size_t utf16_strlen(const char *str, size_t byte_len) {
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  
  // Fast path: check if string is ASCII-only (very common case)
  size_t i = 0;
  for (; i + 8 <= byte_len; i += 8) {
    uint64_t chunk;
    memcpy(&chunk, p + i, 8);
    if (chunk & 0x8080808080808080ULL) goto slow_path;
  }
  for (; i < byte_len; i++) {
    if (p[i] & 0x80) goto slow_path;
  }
  return byte_len;  // All ASCII: UTF-16 length == byte length
  
slow_path:;
  size_t count = i;  // ASCII chars counted before first non-ASCII
  p += i;
  while (p < end) {
    unsigned char c = *p;
    if ((c & 0xC0) != 0x80) {
      count++;
      if ((c & 0xF8) == 0xF0) count++;  // 4-byte UTF-8 = 2 UTF-16 units
    }
    p++;
  }
  return count;
}

int utf16_index_to_byte_offset(
  const char *str,
  size_t byte_len,
  size_t utf16_idx,
  size_t *out_char_bytes
) {
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  size_t utf16_pos = 0;
  
  while (p < end && utf16_pos < utf16_idx) {
    unsigned char c = *p;
    if (c < 0x80) { p++; utf16_pos++; }
    else if ((c & 0xE0) == 0xC0) { p += 2; utf16_pos++; }
    else if ((c & 0xF0) == 0xE0) { p += 3; utf16_pos++; }
    else if ((c & 0xF8) == 0xF0) { p += 4; utf16_pos += 2; }
    else { p++; utf16_pos++; }
    if (p > end) p = end;
  }
  
  if (p >= end) {
    if (utf16_pos == utf16_idx) {
      if (out_char_bytes) *out_char_bytes = 0;
      return (int)byte_len;
    }
    return -1;
  }
  
  unsigned char c = *p;
  size_t slen = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
  if (out_char_bytes) *out_char_bytes = slen;
  return (int)(p - (const unsigned char *)str);
}

int utf16_range_to_byte_range(
  const char *str,
  size_t byte_len,
  size_t utf16_start,
  size_t utf16_end,
  size_t *byte_start,
  size_t *byte_end
) {
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  size_t utf16_pos = 0;
  size_t b_start = 0, b_end = byte_len;
  int found_start = 0, found_end = 0;
  
  while (p < end) {
    if (utf16_pos == utf16_start) { b_start = p - (const unsigned char *)str; found_start = 1; }
    if (utf16_pos == utf16_end) { b_end = p - (const unsigned char *)str; found_end = 1; break; }
    
    unsigned char c = *p;
    if (c < 0x80) { p++; utf16_pos++; }
    else if ((c & 0xE0) == 0xC0) { p += 2; utf16_pos++; }
    else if ((c & 0xF0) == 0xE0) { p += 3; utf16_pos++; }
    else if ((c & 0xF8) == 0xF0) { p += 4; utf16_pos += 2; }
    else { p++; utf16_pos++; }
    if (p > end) p = end;
  }
  
  if (!found_start && utf16_start >= utf16_pos) b_start = byte_len;
  if (!found_end && utf16_end >= utf16_pos) b_end = byte_len;
  
  *byte_start = b_start;
  *byte_end = b_end;
  return 0;
}

uint32_t utf16_code_unit_at(const char *str, size_t byte_len, size_t utf16_idx) {
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  size_t utf16_pos = 0;
  
  while (p < end) {
    unsigned char c = *p;
    size_t units, slen;
    uint32_t cp;
    
    if (c < 0x80) { cp = c; slen = 1; units = 1; }
    else if ((c & 0xE0) == 0xC0 && p + 1 < end) {
      cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); slen = 2; units = 1;
    }
    else if ((c & 0xF0) == 0xE0 && p + 2 < end) {
      cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); slen = 3; units = 1;
    }
    else if ((c & 0xF8) == 0xF0 && p + 3 < end) {
      cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); slen = 4; units = 2;
    }
    else { cp = c; slen = 1; units = 1; }
    
    if (utf16_pos == utf16_idx) {
      if (units == 2) return 0xD800 + ((cp - 0x10000) >> 10);
      return cp;
    }
    if (units == 2 && utf16_pos + 1 == utf16_idx) {
      return 0xDC00 + ((cp - 0x10000) & 0x3FF);
    }
    p += slen;
    utf16_pos += units;
  }
  return 0xFFFFFFFF;
}

uint32_t utf16_codepoint_at(const char *str, size_t byte_len, size_t utf16_idx) {
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  size_t utf16_pos = 0;
  
  while (p < end) {
    unsigned char c = *p;
    size_t units, slen;
    uint32_t cp;
    
    if (c < 0x80) { cp = c; slen = 1; units = 1; }
    else if ((c & 0xE0) == 0xC0 && p + 1 < end) {
      cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); slen = 2; units = 1;
    }
    else if ((c & 0xF0) == 0xE0 && p + 2 < end) {
      cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); slen = 3; units = 1;
    }
    else if ((c & 0xF8) == 0xF0 && p + 3 < end) {
      cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); slen = 4; units = 2;
    }
    else { cp = c; slen = 1; units = 1; }
    
    if (utf16_pos == utf16_idx) return cp;
    if (units == 2 && utf16_pos + 1 == utf16_idx) {
      return 0xDC00 + ((cp - 0x10000) & 0x3FF);
    }
    p += slen;
    utf16_pos += units;
  }
  return 0xFFFFFFFF;
}
