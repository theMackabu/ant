#pragma once

#include "types.h"
#include "native_tag.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  RS_STATE_READABLE = 0,
  RS_STATE_CLOSED,
  RS_STATE_ERRORED,
} rs_state_t;

typedef struct {
  double *queue_sizes;
  double queue_total_size;
  double strategy_hwm;

  uint32_t queue_sizes_len;
  uint32_t queue_sizes_cap;
  uint32_t queue_sizes_head;
  uint32_t queue_head;
  uint32_t queue_count;

  bool close_requested: 1;
  bool defer_close: 1;
  bool in_enqueue: 1;
  bool pull_again: 1;
  bool pulling: 1;
  bool started: 1;
} rs_controller_t;

typedef struct {
  uint32_t read_reqs_head;
  uint32_t pending_reads;
  rs_state_t state;
  bool disturbed;
} rs_stream_t;

static constexpr uint32_t RS_STREAM_NATIVE_TAG = Ant_NativeTag("RSTR");
static constexpr uint32_t RS_CONTROLLER_NATIVE_TAG = Ant_NativeTag("RSCT");

__attribute__((cold)) 
void init_readable_stream_module(ant_t *js);
void gc_mark_readable_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t));

bool rs_is_stream(ant_value_t obj);
bool rs_is_reader(ant_value_t obj);
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

ant_value_t rs_cancel_reject(ant_params_t);
ant_value_t js_rs_reader_ctor(ant_params_t);
ant_value_t rs_reader_create(ant_t *js, ant_value_t stream_obj, ant_value_t new_target);
ant_value_t rs_cancel_resolve(ant_params_t);

ant_value_t rs_stream_controller(ant_t *js, ant_value_t stream_obj);
ant_value_t rs_default_reader_read(ant_t *js, ant_value_t reader_obj);
ant_value_t readable_stream_cancel(ant_t *js, ant_value_t stream_obj, ant_value_t reason);
ant_value_t rs_create_stream(ant_t *js, ant_value_t pull_fn, ant_value_t cancel_fn, double hwm);
ant_value_t rs_controller_enqueue(ant_t *js, ant_value_t ctrl_obj, ant_value_t chunk);

void rs_controller_close(ant_t *js, ant_value_t ctrl_obj);
void rs_default_controller_clear_algorithms(ant_value_t ctrl_obj);
void rs_ctrl_queue_push(ant_t *js, ant_value_t ctrl_obj, rs_controller_t *ctrl, ant_value_t value);
void rs_default_controller_call_pull_if_needed(ant_t *js, ant_value_t controller_obj);
void rs_default_reader_error_read_requests(ant_t *js, ant_value_t reader_obj, ant_value_t e);
void rs_fulfill_read_request(ant_t *js, ant_value_t stream_obj, ant_value_t chunk, bool done);

void readable_stream_close(ant_t *js, ant_value_t stream_obj);
void readable_stream_error(ant_t *js, ant_value_t stream_obj, ant_value_t e);

bool rs_reader_has_reqs(ant_t *js, ant_value_t reader_obj);
bool rs_default_controller_can_close_or_enqueue(rs_controller_t *ctrl, rs_stream_t *stream);
