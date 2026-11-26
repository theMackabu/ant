#include "escape.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(const char *escape, size_t end, const char *want, size_t want_len, size_t want_advance, bool legacy) {
  size_t len = strlen(escape);
  uint8_t *in = malloc(len);
  assert(in);
  memcpy(in, escape, len);

  uint8_t out[8] = {0};
  size_t out_len = 0;
  uint8_t flags = 0;
  size_t advance = decode_escape(in, 0, end, out, &out_len, '"', &flags);

  assert(advance == want_advance);
  assert(out_len == want_len);
  assert(memcmp(out, want, want_len) == 0);
  assert(((flags & ESCAPE_LEGACY) != 0) == legacy);
  free(in);
}

int main(void) {
  expect("\\0", 2, "\x00", 1, 0, false);
  expect("\\1", 2, "\x01", 1, 0, true);
  expect("\\7", 2, "\x07", 1, 0, true);
  expect("\\07", 3, "\x07", 1, 1, true);
  expect("\\12", 3, "\x0a", 1, 1, true);
  expect("\\123", 4, "\x53", 1, 2, true);
  expect("\\377", 4, "\xc3\xbf", 2, 2, true);
  expect("\\400", 4, "\x20", 1, 1, true);
  expect("\\777", 4, "\x3f", 1, 1, true);
  expect("\\123", 2, "\x01", 1, 0, true);
  expect("\\123", 3, "\x0a", 1, 1, true);
  puts("PASS octal escapes stop at the end of the input");
}
