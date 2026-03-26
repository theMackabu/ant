#ifndef STREAMS_PIPES_H
#define STREAMS_PIPES_H

#include "types.h"
#include <stdbool.h>

void init_pipes_proto(ant_t *js, ant_value_t rs_proto);
ant_value_t readable_stream_tee(ant_t *js, ant_value_t source);

ant_value_t readable_stream_pipe_to(
  ant_t *js, ant_value_t source, ant_value_t dest,
  bool prevent_close, bool prevent_abort, bool prevent_cancel,
  ant_value_t signal
);

#endif
