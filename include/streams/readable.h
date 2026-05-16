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
  bool defer_close;
  bool in_enqueue;
  bool pull_again;
  bool pulling;
  bool started;
} rs_controller_t;

typedef struct {
  rs_state_t state;
  bool disturbed;
} rs_stream_t;

enum {
  RS_STREAM_NATIVE_TAG = 0x52535452u,    // RSTR
  RS_CONTROLLER_NATIVE_TAG = 0x52534354u // RSCT
};


void init_readable_stream_module(void);
void gc_mark_readable_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

bool rs_is_stream(ant_value_t obj);
bool rs_is_reader(ant_value_t obj);
bool rs_is_controller(ant_value_t obj);
bool rs_stream_locked(ant_value_t stream_obj);
bool rs_stream_disturbed(ant_value_t stream_obj);
bool rs_stream_unusable(ant_value_t stream_obj);

rs_stream_t *rs_get_stream(ant_value_t obj);
rs_controller_t *rs_get_controller(ant_value_t obj);
ant_offset_t rs_ctrl_queue_len(ant_t *js, ant_value_t ctrl_obj);

ant_value_t rs_ctrl_size(ant_value_t ctrl_obj);
ant_value_t rs_reader_reqs(ant_value_t reader_obj);
ant_value_t rs_stream_error(ant_value_t stream_obj);
ant_value_t rs_stream_reader(ant_value_t stream_obj);
ant_value_t rs_reader_stream(ant_value_t reader_obj);
ant_value_t rs_reader_closed(ant_value_t reader_obj);

ant_value_t rs_stream_controller(ant_t *js, ant_value_t stream_obj);
ant_value_t rs_default_reader_read(ant_t *js, ant_value_t reader_obj);
ant_value_t rs_cancel_reject(ant_t *js, ant_value_t *args, int nargs);
ant_value_t js_rs_reader_ctor(ant_t *js, ant_value_t *args, int nargs);
ant_value_t rs_cancel_resolve(ant_t *js, ant_value_t *args, int nargs);
ant_value_t readable_stream_cancel(ant_t *js, ant_value_t stream_obj, ant_value_t reason);
ant_value_t rs_create_stream(ant_t *js, ant_value_t pull_fn, ant_value_t cancel_fn, double hwm);
ant_value_t rs_controller_enqueue(ant_t *js, ant_value_t ctrl_obj, ant_value_t chunk);

void rs_controller_close(ant_t *js, ant_value_t ctrl_obj);
void rs_default_controller_clear_algorithms(ant_value_t ctrl_obj);
void rs_ctrl_queue_push(ant_t *js, ant_value_t ctrl_obj, ant_value_t value);
void rs_default_controller_call_pull_if_needed(ant_t *js, ant_value_t controller_obj);
void rs_default_reader_error_read_requests(ant_t *js, ant_value_t reader_obj, ant_value_t e);
void rs_fulfill_read_request(ant_t *js, ant_value_t stream_obj, ant_value_t chunk, bool done);

void readable_stream_close(ant_t *js, ant_value_t stream_obj);
void readable_stream_error(ant_t *js, ant_value_t stream_obj, ant_value_t e);

bool rs_reader_has_reqs(ant_t *js, ant_value_t reader_obj);
bool rs_default_controller_can_close_or_enqueue(rs_controller_t *ctrl, rs_stream_t *stream);

#endif
