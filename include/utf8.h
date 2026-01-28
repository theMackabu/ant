#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdint.h>

int utf8_sequence_length(unsigned char first_byte);
int utf8_encode(uint32_t codepoint, char *out);
uint32_t utf8_decode(const unsigned char *buf, size_t len, int *seq_len);

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

#endif
