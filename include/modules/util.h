#ifndef ANT_UTIL_MODULE_H
#define ANT_UTIL_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include "types.h"

typedef struct {
  bool (*text)(void *ctx, const char *s, size_t len);
  bool (*value)(void *ctx, ant_value_t value, char spec);
} ant_format_sink_t;

int ant_format_walk(
  ant_t *js,
  ant_value_t *args, int nargs, int fmt_index,
  const ant_format_sink_t *sink, void *ctx
);

ant_value_t util_library(ant_t *js);
ant_value_t util_types_library(ant_t *js);

#endif
