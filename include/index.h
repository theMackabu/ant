#ifndef ANT_INDEX_H
#define ANT_INDEX_H

#include "ant.h"
#include "value.h"

static constexpr double MAX_SAFE_INTEGER = 9007199254740991.0;

ant_value_t js_to_index_slow(ant_t *js, ant_value_t value, size_t *out);
ant_value_t js_to_length_slow(ant_t *js, ant_value_t value, size_t *out);

static inline ant_value_t js_to_index_fast(ant_t *js, ant_value_t value, size_t *out) {
  if (__builtin_expect(vtype(value) == kTypeNumber, 1)) {
    double number = tod(value);
    if (__builtin_expect(
      number >= 0.0 && number <= MAX_SAFE_INTEGER &&
      number <= (double)SIZE_MAX, 1 )
	) { *out = (size_t)number; return js_mkundef(); }
  }
  return js_to_index_slow(js, value, out);
}

static inline ant_value_t js_to_length_fast(ant_t *js, ant_value_t value, size_t *out) {
  if (__builtin_expect(vtype(value) == kTypeNumber, 1)) {
    double number = tod(value);
    if (__builtin_expect(
      number >= 0.0 && number <= MAX_SAFE_INTEGER &&
      number <= (double)SIZE_MAX, 1)
	) { *out = (size_t)number; return js_mkundef(); }
  }
  return js_to_length_slow(js, value, out);
}

#endif
