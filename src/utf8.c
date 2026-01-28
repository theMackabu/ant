#include "utf8.h"

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
  int slen = utf8_sequence_length(first);
  
  if (slen < 0 || (size_t)slen > len) {
    *seq_len = 1;
    return 0xFFFD;
  }
  
  *seq_len = slen;
  
  if (slen == 1) return first;
  if (slen == 2) return ((first & 0x1F) << 6) | (buf[1] & 0x3F);
  if (slen == 3) return ((first & 0x0F) << 12) | ((buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
  return ((first & 0x07) << 18) | ((buf[1] & 0x3F) << 12) | ((buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
}

size_t utf8_strlen(const char *str, size_t byte_len) {
  size_t count = 0;
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  
  while (p < end) {
    int seq_len = utf8_sequence_length(*p);
    if (seq_len < 0) { p++; count++; continue; }
    p += seq_len;
    count++;
  }
  return count;
}

size_t utf16_strlen(const char *str, size_t byte_len) {
  size_t count = 0;
  const unsigned char *p = (const unsigned char *)str;
  const unsigned char *end = p + byte_len;
  
  while (p < end) {
    int seq_len;
    uint32_t cp = utf8_decode(p, end - p, &seq_len);
    p += seq_len;
    count += (cp >= 0x10000) ? 2 : 1;
  }
  return count;
}
