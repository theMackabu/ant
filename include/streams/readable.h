#ifndef STREAMS_READABLE_H
#define STREAMS_READABLE_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  RS_STATE_READABLE = 0,
  RS_STATE_CLOSED,
  RS_STATE_ERRORED,
} rs_state_t;

typedef struct {
  double *queue_sizes;
  uint32_t queue_sizes_len;
  uint32_t queue_sizes_cap;
  double queue_total_size;
  double strategy_hwm;
  bool close_requested;
  bool pull_again;
  bool pulling;
  bool started;
} rs_controller_t;

typedef struct {
  rs_state_t state;
  bool disturbed;
} rs_stream_t;

void init_readable_stream_module(void);
void gc_mark_readable_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

rs_stream_t *rs_get_stream(ant_value_t obj);
ant_value_t rs_stream_controller(ant_t *js, ant_value_t stream_obj);
ant_value_t rs_stream_reader(ant_value_t stream_obj);

void readable_stream_close(ant_t *js, ant_value_t stream_obj);
void readable_stream_error(ant_t *js, ant_value_t stream_obj, ant_value_t e);
ant_value_t readable_stream_cancel(ant_t *js, ant_value_t stream_obj, ant_value_t reason);

#endif
