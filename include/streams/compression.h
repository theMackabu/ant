#pragma once

#include "types.h"
#include <stdbool.h>

typedef enum {
  ZFMT_GZIP = 0,
  ZFMT_DEFLATE,
  ZFMT_DEFLATE_RAW,
  ZFMT_BROTLI,
} zformat_t;

__attribute__((cold)) 
void init_compression_stream_module(ant_t *js);
