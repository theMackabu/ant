#ifndef STREAMS_COMPRESSION_H
#define STREAMS_COMPRESSION_H

#include "types.h"
#include <stdbool.h>

typedef enum {
  ZFMT_GZIP = 0,
  ZFMT_DEFLATE,
  ZFMT_DEFLATE_RAW,
} zformat_t;

extern ant_value_t g_cs_proto;
extern ant_value_t g_ds_proto;

void init_compression_stream_module(void);
void gc_mark_compression_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

bool cs_is_stream(ant_value_t obj);
bool ds_is_stream(ant_value_t obj);

ant_value_t cs_stream_readable(ant_value_t obj);
ant_value_t cs_stream_writable(ant_value_t obj);
ant_value_t ds_stream_readable(ant_value_t obj);
ant_value_t ds_stream_writable(ant_value_t obj);

#endif
