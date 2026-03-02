#ifndef OXC_STRIP_H
#define OXC_STRIP_H

#include <stddef.h>

#define OXC_ERR_NULL_INPUT       -1
#define OXC_ERR_INVALID_UTF8     -2
#define OXC_ERR_PARSE_FAILED     -3
#define OXC_ERR_TRANSFORM_FAILED -4
#define OXC_ERR_OUTPUT_TOO_LARGE -5

char *OXC_strip_types_owned(
  const char *input,
  const char *filename,
  size_t *out_len,
  int *out_error,
  char *error_output,
  size_t error_output_len
);

char *OXC_get_hoisted_vars(
  const char *input,
  size_t input_len,
  size_t *out_len
);

char *OXC_get_func_hoisted_vars(
  const char *input,
  size_t input_len,
  size_t *out_len
);

void OXC_free_hoisted_vars(char *ptr, size_t len);
void OXC_free_stripped_output(char *ptr, size_t len);

#endif
