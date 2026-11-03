#include <stdint.h>
#include <string.h>
#include "highlight.h"

int highlight_js_line_clipped(
  const char *line, size_t line_len, size_t max_cols,
  char *out, size_t out_size, highlight_state *state
) {
  size_t len = line_len < max_cols ? line_len : max_cols;
  
  if (len >= out_size) len = out_size ? out_size - 1 : 0;
  if (len) memcpy(out, line, len);
  if (out_size) out[len] = '\0';
  
  return (int)len;
}

