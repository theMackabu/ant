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
  int is_module,
  size_t *out_len,
  int *out_error,
  char *error_output,
  size_t error_output_len
);

#endif
