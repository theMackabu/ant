// Shared helpers for the tests/fuzz targets.
#ifndef ANT_TESTS_FUZZ_H
#define ANT_TESTS_FUZZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A property failure aborts, so libFuzzer saves the input and the replay
// driver stops the test.
#define FUZZ_CHECK(cond) do {                                             \
  if (!(cond)) {                                                          \
    fprintf(stderr, "%s:%d: property failed: %s\n", __FILE__, __LINE__, #cond); \
    abort();                                                              \
  }                                                                       \
} while (0)

#define FUZZ_FAIL(message) do {                                           \
  fprintf(stderr, "%s:%d: property failed: %s\n", __FILE__, __LINE__, message); \
  abort();                                                                \
} while (0)

// Consumes control values from the front of a fuzz input.
typedef struct {
  const uint8_t *data;
  size_t size;
} fuzz_input_t;

static inline uint8_t fuzz_u8(fuzz_input_t *in) {
  if (in->size == 0) return 0;
  in->size--;
  return *in->data++;
}

static inline uint64_t fuzz_u64(fuzz_input_t *in) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | fuzz_u8(in);
  return v;
}

// Deterministic split points derived from a fuzz-chosen seed.
typedef struct {
  uint32_t state;
  uint8_t max_chunk;
} fuzz_splitter_t;

static inline fuzz_splitter_t fuzz_splitter(uint8_t seed) {
  // seed 0 splits at every byte; others mix short and long chunks
  return (fuzz_splitter_t){ .state = 0x9e3779b9u ^ seed, .max_chunk = seed ? (uint8_t)(1 + seed % 32) : 1 };
}

static inline size_t fuzz_next_chunk(fuzz_splitter_t *s, size_t remaining) {
  s->state ^= s->state << 13;
  s->state ^= s->state >> 17;
  s->state ^= s->state << 5;
  size_t n = 1 + s->state % s->max_chunk;
  return n < remaining ? n : remaining;
}

static inline void *fuzz_dup(const void *data, size_t size) {
  void *copy = malloc(size ? size : 1);
  if (!copy) abort();
  if (size) memcpy(copy, data, size);
  return copy;
}

#endif
