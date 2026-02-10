#ifndef STRINGS_H
#define STRINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint8_t unhex(uint8_t c) {
  return (c & 0xF) + (c >> 6) * 9;
}

static inline bool is_xdigit(int c) {
  return (unsigned)c < 256 && (
    (c >= '0' && c <= '9') ||
    (c >= 'a' && c <= 'f') ||
    (c >= 'A' && c <= 'F')
  );
}

size_t decode_escape(
  const uint8_t *in, size_t pos, size_t end,
  uint8_t *out, size_t *out_pos, uint8_t quote
);

#endif
