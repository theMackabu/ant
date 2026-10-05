#ifndef STREAMS_COMPRESSION_H
#define STREAMS_COMPRESSION_H

#include "types.h"
#include <stdbool.h>

typedef enum {
  ZFMT_GZIP = 0,
  ZFMT_DEFLATE,
  ZFMT_DEFLATE_RAW,
  ZFMT_BROTLI,
} zformat_t;

void init_compression_stream_module(ant_t *js);
void gc_mark_compression_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

#endif
