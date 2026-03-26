#ifndef STREAMS_TRANSFORM_H
#define STREAMS_TRANSFORM_H

#include "types.h"
#include <stdbool.h>

void init_transform_stream_module(void);
void gc_mark_transform_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

bool ts_is_stream(ant_value_t obj);
bool ts_is_controller(ant_value_t obj);

ant_value_t ts_stream_readable(ant_value_t ts_obj);
ant_value_t ts_stream_writable(ant_value_t ts_obj);

#endif
