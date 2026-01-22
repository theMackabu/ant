#ifndef OXC_STRIP_H
#define OXC_STRIP_H

#include <stddef.h>

#define OXC_ERR_NULL_INPUT       -1
#define OXC_ERR_INVALID_UTF8     -2
#define OXC_ERR_PARSE_FAILED     -3
#define OXC_ERR_TRANSFORM_FAILED -4
#define OXC_ERR_OUTPUT_TOO_LARGE -5

int OXC_strip_types(
  const char *input,
  const char *filename,
  char *output,
  size_t output_len
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

#endif
