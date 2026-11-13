#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "types.h"

typedef enum {
  ANT_BASE64_LOOSE,
  ANT_BASE64_STRICT,
  ANT_BASE64_STOP_BEFORE_PARTIAL,
} ant_base64_last_chunk_t;

void js_base64_init_globals(ant_t *js, ant_value_t glob);

char *ant_base64_encode(const uint8_t *data, size_t len, size_t *out_len);
uint8_t *ant_base64_decode(const char *data, size_t len, size_t *out_len);

char *ant_base64url_encode(const uint8_t *data, size_t len, size_t *out_len);
uint8_t *ant_base64_decode_loose(const char *data, size_t len, size_t *out_len);

bool ant_base64_decode_into(
  const char *src, size_t len, bool url, ant_base64_last_chunk_t last_chunk,
  uint8_t *dst, size_t max_len, size_t *read, size_t *written, const char **error
);
