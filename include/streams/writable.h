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

extern ant_value_t g_ws_proto;
extern ant_value_t g_ws_writer_proto;
extern ant_value_t g_ws_controller_proto;

void init_writable_stream_module(void);
void gc_mark_writable_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

ws_stream_t *ws_get_stream(ant_value_t obj);
ws_controller_t *ws_get_controller(ant_value_t obj);

ant_value_t ws_writer_ready(ant_value_t writer_obj);
ant_value_t ws_stream_writer(ant_value_t stream_obj);
ant_value_t ws_stream_controller(ant_value_t stream_obj);
ant_value_t ws_acquire_writer(ant_t *js, ant_value_t stream_obj);
ant_value_t ws_writer_write(ant_t *js, ant_value_t writer_obj, ant_value_t chunk);

ant_value_t writable_stream_close(ant_t *js, ant_value_t stream_obj);
ant_value_t writable_stream_abort(ant_t *js, ant_value_t stream_obj, ant_value_t reason);

bool writable_stream_close_queued_or_in_flight(ant_value_t stream_obj);
void writable_stream_finish_erroring(ant_t *js, ant_value_t stream_obj);
void ws_default_controller_error(ant_t *js, ant_value_t ctrl_obj, ant_value_t error);
void ws_default_controller_advance_queue_if_needed(ant_t *js, ant_value_t ctrl_obj);

#endif
