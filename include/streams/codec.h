#ifndef STREAMS_CODEC_H
#define STREAMS_CODEC_H

#include "types.h"
#include <stdbool.h>

extern ant_value_t g_tes_proto;
extern ant_value_t g_tds_proto;

void init_codec_stream_module(void);
void gc_mark_codec_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

bool tes_is_stream(ant_value_t obj);
bool tds_is_stream(ant_value_t obj);

ant_value_t tes_stream_readable(ant_value_t obj);
ant_value_t tes_stream_writable(ant_value_t obj);
ant_value_t tds_stream_readable(ant_value_t obj);
ant_value_t tds_stream_writable(ant_value_t obj);

#endif
