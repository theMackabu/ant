#ifndef STREAMS_TRANSFORM_H
#define STREAMS_TRANSFORM_H

#include "types.h"
#include <stdbool.h>

extern ant_value_t g_ts_proto;
extern ant_value_t g_ts_ctrl_proto;

void init_transform_stream_module(void);
void gc_mark_transform_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

bool ts_is_stream(ant_value_t obj);
bool ts_is_controller(ant_value_t obj);

void ts_ctrl_error(ant_t *js, ant_value_t ctrl_obj, ant_value_t e);
void ts_ctrl_terminate(ant_t *js, ant_value_t ctrl_obj);

ant_value_t ts_stream_readable(ant_value_t ts_obj);
ant_value_t ts_stream_writable(ant_value_t ts_obj);

ant_value_t ts_stream_controller(ant_value_t ts_obj);
ant_value_t ts_ctrl_enqueue(ant_t *js, ant_value_t ctrl_obj, ant_value_t chunk);
ant_value_t js_ts_ctor(ant_t *js, ant_value_t *args, int nargs);

#endif
