#pragma once

#include "types.h"
#include <stdbool.h>

__attribute__((cold)) 
void init_codec_stream_module(ant_t *js);
void gc_mark_codec_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));
