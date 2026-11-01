// intern_string for the standalone shape tests, which link src/shapes.c
// without the rest of the runtime (src/gc/strings.c defines the real one).
// Shapes intern array index keys through it.
#ifndef ANT_TESTS_SHAPE_TEST_INTERN_H
#define ANT_TESTS_SHAPE_TEST_INTERN_H

#include <stdlib.h>
#include <string.h>

const char *intern_string(ant_t *js, const char *str, size_t len) {
  static struct { char *str; size_t len; } table[4096];
  static size_t count;
  (void)js;

  for (size_t i = 0; i < count; i++)
    if (table[i].len == len && memcmp(table[i].str, str, len) == 0) return table[i].str;
  if (count == sizeof(table) / sizeof(*table)) return NULL;

  char *copy = malloc(len + 1);
  if (!copy) return NULL;
  memcpy(copy, str, len);
  copy[len] = '\0';
  table[count].str = copy;
  table[count].len = len;
  return table[count++].str;
}

#endif
