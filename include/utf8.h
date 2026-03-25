#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <utf8proc.h>

typedef struct {
  bool ignore_bom;
  bool bom_seen;
  uint8_t pend_buf[3];
  int pend_pos;
} utf8_dec_t;

utf8proc_ssize_t utf8_whatwg_decode(
  utf8_dec_t *dec, const uint8_t *src, size_t len,
  char *out, bool fatal, bool stream
);

size_t utf8_char_len_at(const char *str, size_t byte_len, size_t pos);
size_t utf8_strlen(const char *str, size_t byte_len);
size_t utf16_strlen(const char *str, size_t byte_len);

int utf16_index_to_byte_offset(
  const char *str,
  size_t byte_len,
  size_t utf16_idx,
  size_t *out_char_bytes
);

int utf16_range_to_byte_range(
  const char *str,
  size_t byte_len,
  size_t utf16_start,
  size_t utf16_end,
  size_t *byte_start,
  size_t *byte_end
);

uint32_t utf16_code_unit_at(const char *str, size_t byte_len, size_t utf16_idx);
uint32_t utf16_codepoint_at(const char *str, size_t byte_len, size_t utf16_idx);

static inline int utf8_sequence_length(unsigned char first_byte) {
  if ((first_byte & 0x80) == 0) return 1;
  if ((first_byte & 0xE0) == 0xC0) return 2;
  if ((first_byte & 0xF0) == 0xE0) return 3;
  if ((first_byte & 0xF8) == 0xF0) return 4;
  return -1;
}

static inline int utf8_encode(uint32_t cp, char *out) {
  return (int)
    utf8proc_encode_char((utf8proc_int32_t)cp, 
    (utf8proc_uint8_t *)out);
}

static inline utf8proc_ssize_t utf8_next(
  const utf8proc_uint8_t *p,
  utf8proc_ssize_t len,
  utf8proc_int32_t *cp
) {
  utf8proc_ssize_t n = utf8proc_iterate(p, len, cp);
  return n > 0 ? n : 1;
}

#endif
