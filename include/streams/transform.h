#ifndef STREAMS_TRANSFORM_H
#define STREAMS_TRANSFORM_H

#include "types.h"
#include <stdbool.h>

void init_transform_stream_module(void);
void gc_mark_transform_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

#endif
