#ifndef ANT_STREAM_MODULE_H
#define ANT_STREAM_MODULE_H

#include "types.h"

void stream_init_constructors(ant_t *js);

ant_value_t stream_library(ant_t *js);
ant_value_t stream_promises_library(ant_t *js);

ant_value_t stream_readable_constructor(ant_t *js);
ant_value_t stream_writable_constructor(ant_t *js);
ant_value_t stream_readable_prototype(ant_t *js);
ant_value_t stream_writable_prototype(ant_t *js);

ant_value_t stream_construct_readable(ant_t *js, ant_value_t base_proto, ant_value_t options);
ant_value_t stream_construct_writable(ant_t *js, ant_value_t base_proto, ant_value_t options);
ant_value_t stream_readable_push(ant_t *js, ant_value_t stream_obj, ant_value_t chunk, ant_value_t encoding);

void stream_init_readable_object(ant_t *js, ant_value_t obj, ant_value_t options);
void stream_init_writable_object(ant_t *js, ant_value_t obj, ant_value_t options);

#endif
