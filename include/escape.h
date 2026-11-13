#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum: uint8_t {
  ESCAPE_LEGACY = 1 << 0,
  ESCAPE_BAD_HEX = 1 << 1,
  ESCAPE_BAD_UNICODE = 1 << 2,
  ESCAPE_BAD_CODEPOINT = 1 << 3,
  ESCAPE_MALFORMED = ESCAPE_BAD_HEX | ESCAPE_BAD_UNICODE | ESCAPE_BAD_CODEPOINT,
} escape_flags_t;

typedef enum: uint8_t {
  ESCAPE_CONTEXT_SLOPPY,
  ESCAPE_CONTEXT_STRICT,
  ESCAPE_CONTEXT_TEMPLATE,
} escape_context_t;

const char *escape_error_message(uint8_t flags, uint8_t escape_char, escape_context_t context);
size_t decode_escape(const uint8_t *in, size_t pos, size_t end, uint8_t *out, size_t *out_pos, uint8_t quote, uint8_t *flags);

static inline uint8_t unhex(uint8_t c) {
  return (c & 0xF) + (c >> 6) * 9;
}
