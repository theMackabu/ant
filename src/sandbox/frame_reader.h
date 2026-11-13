#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const uint8_t *p;
  const uint8_t *end;
} sandbox_frame_reader_t;

static inline uint16_t sandbox_load16(const uint8_t *in) {
  return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static inline uint32_t sandbox_load32(const uint8_t *in) {
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)| ((uint32_t)in[3] << 24);
}

static inline bool sandbox_read_u32(sandbox_frame_reader_t *r, uint32_t *out) {
  if ((size_t)(r->end - r->p) < 4) return false;
  *out = sandbox_load32(r->p);
  r->p += 4;
  return true;
}

static inline bool sandbox_read_u16(sandbox_frame_reader_t *r, uint16_t *out) {
  if ((size_t)(r->end - r->p) < 2) return false;
  *out = sandbox_load16(r->p);
  r->p += 2;
  return true;
}
