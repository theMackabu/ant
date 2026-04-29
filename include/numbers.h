#ifndef ANT_NUMBER_CONVERSION_H
#define ANT_NUMBER_CONVERSION_H

#include <stdbool.h>
#include <stddef.h>

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

size_t ant_number_to_shortest(double value, char *buf, size_t len);
size_t ant_number_to_fixed(double value, int digits, char *buf, size_t len);
size_t ant_number_to_precision(double value, int precision, char *buf, size_t len);
size_t ant_number_to_exponential(double value, int digits, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
