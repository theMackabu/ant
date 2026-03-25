#ifndef STREAMS_WRITABLE_H
#define STREAMS_WRITABLE_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  WS_STATE_WRITABLE = 0,
  WS_STATE_CLOSED,
  WS_STATE_ERRORING,
  WS_STATE_ERRORED,
} ws_state_t;

typedef struct {
  double *queue_sizes;
  uint32_t queue_sizes_len;
  uint32_t queue_sizes_cap;
  double queue_total_size;
  double strategy_hwm;
  bool close_requested;
  bool started;
} ws_controller_t;

typedef struct {
  ws_state_t state;
  bool backpressure;
  bool has_pending_abort;
  bool pending_abort_was_already_erroring;
} ws_stream_t;

void init_writable_stream_module(void);
void gc_mark_writable_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

ws_stream_t *ws_get_stream(ant_value_t obj);
ant_value_t ws_stream_controller(ant_value_t stream_obj);
ant_value_t ws_stream_writer(ant_value_t stream_obj);

ant_value_t writable_stream_close(ant_t *js, ant_value_t stream_obj);
ant_value_t writable_stream_abort(ant_t *js, ant_value_t stream_obj, ant_value_t reason);

bool writable_stream_close_queued_or_in_flight(ant_value_t stream_obj);
void ws_default_controller_error(ant_t *js, ant_value_t ctrl_obj, ant_value_t error);

#endif
