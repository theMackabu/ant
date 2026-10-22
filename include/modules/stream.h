#ifndef ANT_STREAM_MODULE_H
#define ANT_STREAM_MODULE_H

#include "types.h"

typedef void (*stream_finalize_fn)(
  ant_t *js,
  ant_value_t stream_obj,
  void *state
);

void stream_init_constructors(ant_t *js);
void stream_begin_construct(ant_t *js, ant_value_t stream_obj);

ant_value_t stream_construct(
  ant_t *js, ant_value_t base_proto, ant_value_t raw_options,
  void (*init_fn)(ant_t *, ant_value_t, ant_value_t), ant_value_t new_target
);

ant_value_t stream_library(ant_t *js);
ant_value_t stream_promises_library(ant_t *js);
ant_value_t stream_web_library(ant_t *js);

ant_value_t stream_readable_constructor(ant_t *js);
ant_value_t stream_writable_constructor(ant_t *js);
ant_value_t stream_readable_prototype(ant_t *js);
ant_value_t stream_writable_prototype(ant_t *js);
ant_value_t stream_duplex_prototype(ant_t *js);
ant_value_t stream_transform_prototype(ant_t *js);

ant_value_t stream_construct_readable(ant_t *js, ant_value_t base_proto, ant_value_t options, ant_value_t new_target);
ant_value_t stream_construct_writable(ant_t *js, ant_value_t base_proto, ant_value_t options, ant_value_t new_target);
ant_value_t stream_construct_duplex(ant_t *js, ant_value_t base_proto, ant_value_t options, ant_value_t new_target);
ant_value_t stream_readable_push(ant_t *js, ant_value_t stream_obj, ant_value_t chunk, ant_value_t encoding);
ant_value_t stream_readable_maybe_read(ant_t *js, ant_value_t stream_obj);
ant_value_t stream_readable_flush(ant_t *js, ant_value_t stream_obj);
ant_value_t stream_readable_push_value(ant_t *js, ant_value_t stream_obj, ant_value_t chunk, ant_value_t encoding);
ant_value_t stream_readable_continue_flowing(ant_params_t);
ant_value_t stream_readable_begin_flowing(ant_t *js, ant_value_t stream_obj);
ant_value_t stream_writable_begin_end(ant_t *js, ant_value_t stream_obj, ant_value_t callback);
ant_value_t stream_destroy(ant_t *js, ant_value_t stream_obj, ant_value_t error);
ant_value_t stream_readable_resume(ant_t *js, ant_value_t stream_obj);
ant_value_t stream_readable_pause(ant_t *js, ant_value_t stream_obj);

void stream_init_readable_object(ant_t *js, ant_value_t obj, ant_value_t options);
void stream_init_writable_object(ant_t *js, ant_value_t obj, ant_value_t options);
void stream_init_duplex_object(ant_t *js, ant_value_t obj, ant_value_t options);

void *stream_get_attached_state(ant_value_t stream_obj);
void stream_clear_attached_state(ant_value_t stream_obj);
void stream_set_attached_state(ant_value_t stream_obj, void *state, stream_finalize_fn finalize);

void stream_set_destroyed(ant_t *js, ant_value_t stream_obj, bool destroyed);
void stream_set_readable(ant_t *js, ant_value_t stream_obj, bool readable);

#endif
