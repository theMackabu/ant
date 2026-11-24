#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ANT_NUMBER_PARSE_DECIMAL,
  ANT_NUMBER_PARSE_JS_NUMBER,
  ANT_NUMBER_PARSE_FLOAT_PREFIX,
} ant_number_parse_mode_t;

bool ant_number_parse(
  const char *str, size_t len,
  ant_number_parse_mode_t mode,
  double *out, size_t *processed
);

size_t ant_js_trim_prefix_len(const char *str, size_t len);
size_t ant_js_trim_suffix_len(const char *str, size_t len);

size_t ant_number_to_shortest(double value, char *buf, size_t len);
size_t ant_number_to_fixed(double value, int digits, char *buf, size_t len);
size_t ant_number_to_precision(double value, int precision, char *buf, size_t len);
size_t ant_number_to_exponential(double value, int digits, char *buf, size_t len);

static constexpr size_t ANT_NUMBER_RADIX_BUFFER_SIZE = 2200;
size_t ant_number_to_radix(double value, int radix, char *buffer, size_t size, const char **out_start);
double ant_number_parse_int(const char *str, size_t len, int radix);

static inline bool ant_number_as_int32(double value, int32_t *out) {
  if (!(value >= -2147483648.0 && value <= 2147483647.0)) return false;
  int32_t iv = (int32_t)value;
  if ((double)iv != value) return false;
  *out = iv;
  return true;
}

static inline size_t ant_int32_chars_len(int32_t value) {
  static const uint32_t powers[] = {
    0, 10, 100, 1000, 10000, 100000, 
    1000000, 10000000, 100000000, 1000000000,
  };

  uint32_t u = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
  uint32_t estimate = ((32u - (uint32_t)__builtin_clz(u | 1)) * 1233u) >> 12;
  
  return (size_t)estimate + 1 - (u < powers[estimate]) + (value < 0);
}

static inline void ant_int32_write(char *dst, size_t len, int32_t value) {
  static const char pairs[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";
  
  uint32_t u = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
  char *p = dst + len;
  
  while (u >= 100) {
    uint32_t pair = u % 100;
    u /= 100;
    p -= 2;
    memcpy(p, pairs + pair * 2, 2);
  }
  
  if (u >= 10) {
    p -= 2;
    memcpy(p, pairs + u * 2, 2);
  } else *--p = (char)('0' + u);
  
  if (value < 0) *--p = '-';
}

#ifdef __cplusplus
}
#endif
