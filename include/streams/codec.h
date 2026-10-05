#ifndef STREAMS_CODEC_H
#define STREAMS_CODEC_H

#include "types.h"
#include <stdbool.h>

void init_codec_stream_module(ant_t *js);
void gc_mark_codec_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

#endif
