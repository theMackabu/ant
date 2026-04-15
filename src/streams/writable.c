#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "gc/roots.h"
#include "silver/engine.h"
#include "modules/symbol.h"
#include "modules/assert.h"
#include "modules/abort.h"
#include "streams/writable.h"

ant_value_t g_ws_proto;
ant_value_t g_ws_writer_proto;
ant_value_t g_ws_controller_proto;
static ant_value_t g_close_sentinel;

bool ws_is_stream(ant_value_t obj) {
  return js_check_brand(obj, BRAND_WRITABLE_STREAM)
    && ws_get_stream(obj) != NULL;
}

bool ws_is_writer(ant_value_t obj) {
  return js_check_brand(obj, BRAND_WRITABLE_STREAM_WRITER)
    && vtype(js_get_slot(obj, SLOT_RS_CLOSED)) == T_PROMISE
    && vtype(js_get_slot(obj, SLOT_WS_READY)) == T_PROMISE;
}

bool ws_is_controller(ant_value_t obj) {
  return js_check_brand(obj, BRAND_WRITABLE_STREAM_CONTROLLER)
    && ws_get_controller(obj) != NULL
    && ws_is_stream(js_get_slot(obj, SLOT_ENTRIES));
}

ws_stream_t *ws_get_stream(ant_value_t obj) {
  ant_value_t s = js_get_slot(obj, SLOT_DATA);
  if (vtype(s) != T_NUM) return NULL;
  return (ws_stream_t *)(uintptr_t)(size_t)js_getnum(s);
}

ws_controller_t *ws_get_controller(ant_value_t obj) {
  ant_value_t s = js_get_slot(obj, SLOT_DATA);
  if (vtype(s) != T_NUM) return NULL;
  return (ws_controller_t *)(uintptr_t)(size_t)js_getnum(s);
}

static void ws_stream_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    free((ws_stream_t *)(uintptr_t)(size_t)js_getnum(entries[i].value));
    return;
  }}
}

static void ws_controller_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    ws_controller_t *ctrl = (ws_controller_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
    free(ctrl->queue_sizes);
    free(ctrl);
    return;
  }}
}

ant_value_t ws_stream_controller(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_ENTRIES);
}

ant_value_t ws_stream_writer(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_CTOR);
}

static inline ant_value_t ws_stream_stored_error(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_AUX);
}

static inline ant_value_t ws_stream_write_requests(ant_t *js, ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_SETTLED);
}

static inline ant_value_t ws_stream_in_flight_write(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_DEFAULT);
}

static inline ant_value_t ws_stream_close_request(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_WS_CLOSE);
}

static inline ant_value_t ws_stream_in_flight_close(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_WS_ABORT);
}

static inline ant_value_t ws_stream_pending_abort_promise(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_WS_READY);
}

static inline ant_value_t ws_ctrl_stream(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_ENTRIES);
}

static inline ant_value_t ws_ctrl_write_fn(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_WS_WRITE);
}

static inline ant_value_t ws_ctrl_close_fn(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_WS_CLOSE);
}

static inline ant_value_t ws_ctrl_abort_fn(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_WS_ABORT);
}

static inline ant_value_t ws_ctrl_size_fn(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_RS_SIZE);
}

static inline ant_value_t ws_ctrl_sink(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_CTOR);
}

static inline ant_value_t ws_ctrl_queue(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_AUX);
}

static inline ant_value_t ws_ctrl_signal(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_WS_SIGNAL);
}

static inline ant_value_t ws_writer_stream(ant_value_t writer_obj) {
  return js_get_slot(writer_obj, SLOT_ENTRIES);
}

static inline ant_value_t ws_writer_closed(ant_value_t writer_obj) {
  return js_get_slot(writer_obj, SLOT_RS_CLOSED);
}

ant_value_t ws_writer_ready(ant_value_t writer_obj) {
  return js_get_slot(writer_obj, SLOT_WS_READY);
}

static void ws_ctrl_queue_push(ant_t *js, ant_value_t ctrl_obj, ant_value_t value) {
  ant_value_t arr = ws_ctrl_queue(ctrl_obj);
  if (vtype(arr) == T_ARR) js_arr_push(js, arr, value);
}

static ant_value_t ws_ctrl_queue_shift(ant_t *js, ant_value_t ctrl_obj) {
  ant_value_t arr = ws_ctrl_queue(ctrl_obj);
  if (vtype(arr) != T_ARR) return js_mkundef();
  ant_object_t *aobj = js_obj_ptr(arr);
  if (aobj->u.array.len == 0) return js_mkundef();
  ant_value_t val = aobj->u.array.data[0];
  uint32_t new_len = aobj->u.array.len - 1;
  for (uint32_t i = 0; i < new_len; i++)
    aobj->u.array.data[i] = aobj->u.array.data[i + 1];
  aobj->u.array.len = new_len;
  return val;
}

static ant_value_t ws_ctrl_queue_peek(ant_t *js, ant_value_t ctrl_obj) {
  ant_value_t arr = ws_ctrl_queue(ctrl_obj);
  if (vtype(arr) != T_ARR) return js_mkundef();
  ant_object_t *aobj = js_obj_ptr(arr);
  if (aobj->u.array.len == 0) return js_mkundef();
  return aobj->u.array.data[0];
}

static bool ws_ctrl_queue_empty(ant_value_t ctrl_obj) {
  ant_value_t arr = ws_ctrl_queue(ctrl_obj);
  if (vtype(arr) != T_ARR) return true;
  ant_object_t *aobj = js_obj_ptr(arr);
  return aobj->u.array.len == 0;
}

static void ws_write_reqs_push(ant_t *js, ant_value_t stream_obj, ant_value_t promise) {
  ant_value_t arr = ws_stream_write_requests(js, stream_obj);
  if (vtype(arr) == T_ARR) js_arr_push(js, arr, promise);
}

static ant_value_t ws_write_reqs_shift(ant_t *js, ant_value_t stream_obj) {
  ant_value_t arr = ws_stream_write_requests(js, stream_obj);
  if (vtype(arr) != T_ARR) return js_mkundef();
  ant_object_t *aobj = js_obj_ptr(arr);
  if (aobj->u.array.len == 0) return js_mkundef();
  ant_value_t val = aobj->u.array.data[0];
  uint32_t new_len = aobj->u.array.len - 1;
  for (uint32_t i = 0; i < new_len; i++)
    aobj->u.array.data[i] = aobj->u.array.data[i + 1];
  aobj->u.array.len = new_len;
  return val;
}

static void ws_chain_promise(ant_t *js, ant_value_t val, ant_value_t res_fn, ant_value_t rej_fn) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, val);
  GC_ROOT_PIN(js, res_fn);
  GC_ROOT_PIN(js, rej_fn);

  ant_value_t promise = val;
  GC_ROOT_PIN(js, promise);
  if (vtype(promise) != T_PROMISE) {
    promise = js_mkpromise(js);
    GC_ROOT_PIN(js, promise);
    js_resolve_promise(js, promise, val);
  }

  ant_value_t then_result = js_promise_then(js, promise, res_fn, rej_fn);
  GC_ROOT_PIN(js, then_result);
  promise_mark_handled(then_result);
  GC_ROOT_RESTORE(js, root_mark);
}

static void ws_default_controller_clear_algorithms(ant_value_t ctrl_obj) {
  js_set_slot(ctrl_obj, SLOT_WS_WRITE, js_mkundef());
  js_set_slot(ctrl_obj, SLOT_WS_CLOSE, js_mkundef());
  js_set_slot(ctrl_obj, SLOT_WS_ABORT, js_mkundef());
  js_set_slot(ctrl_obj, SLOT_RS_SIZE, js_mkundef());
}

static double ws_default_controller_get_desired_size(ws_controller_t *ctrl) {
  return ctrl->strategy_hwm - ctrl->queue_total_size;
}

static bool ws_default_controller_get_backpressure(ws_controller_t *ctrl) {
  return ws_default_controller_get_desired_size(ctrl) <= 0;
}

bool writable_stream_close_queued_or_in_flight(ant_value_t stream_obj) {
  ant_value_t cr = ws_stream_close_request(stream_obj);
  ant_value_t icr = ws_stream_in_flight_close(stream_obj);
  return !is_undefined(cr) || !is_undefined(icr);
}

static bool writable_stream_has_operation_in_flight(ant_value_t stream_obj) {
  ant_value_t iw = ws_stream_in_flight_write(stream_obj);
  ant_value_t ic = ws_stream_in_flight_close(stream_obj);
  return !is_undefined(iw) || !is_undefined(ic);
}

static void ws_writer_replace_ready_promise_rejected(ant_t *js, ant_value_t writer_obj, ant_value_t error) {
  ant_value_t ready = js_mkpromise(js);
  js_reject_promise(js, ready, error);
  promise_mark_handled(ready);
  js_set_slot_wb(js, writer_obj, SLOT_WS_READY, ready);
}

static void ws_writer_replace_closed_promise_rejected(ant_t *js, ant_value_t writer_obj, ant_value_t error) {
  ant_value_t closed = js_mkpromise(js);
  js_reject_promise(js, closed, error);
  promise_mark_handled(closed);
  js_set_slot_wb(js, writer_obj, SLOT_RS_CLOSED, closed);
}

static void ws_writer_reject_ready_promise(ant_t *js, ant_value_t writer_obj, ant_value_t error) {
  ant_value_t ready = ws_writer_ready(writer_obj);
  if (!is_undefined(ready)) js_reject_promise(js, ready, error);
}

static void ws_writer_reject_closed_promise(ant_t *js, ant_value_t writer_obj, ant_value_t error) {
  ant_value_t closed = ws_writer_closed(writer_obj);
  if (!is_undefined(closed)) js_reject_promise(js, closed, error);
}

static void writable_stream_start_erroring(ant_t *js, ant_value_t stream_obj, ant_value_t reason) {
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream || stream->state != WS_STATE_WRITABLE) return;

  ant_value_t ctrl_obj = ws_stream_controller(stream_obj);
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);

  stream->state = WS_STATE_ERRORING;
  js_set_slot_wb(js, stream_obj, SLOT_AUX, reason);

  ant_value_t signal_ac = ws_ctrl_signal(ctrl_obj);
  if (is_object_type(signal_ac)) {
    ant_value_t signal = js_get(js, signal_ac, "signal");
    if (abort_signal_is_signal(signal))
    signal_do_abort(js, signal, reason);
  }

  ant_value_t writer_obj = ws_stream_writer(stream_obj);
  if (ws_is_writer(writer_obj))
    ws_writer_reject_ready_promise(js, writer_obj, reason);

  if (!writable_stream_has_operation_in_flight(stream_obj) && ctrl && ctrl->started)
    writable_stream_finish_erroring(js, stream_obj);
}

static void ws_reject_close_and_closed(ant_t *js, ant_value_t stream_obj) {
  ant_value_t stored_error = ws_stream_stored_error(stream_obj);
  ant_value_t cr = ws_stream_close_request(stream_obj);
  if (!is_undefined(cr)) {
    js_reject_promise(js, cr, stored_error);
    js_set_slot(stream_obj, SLOT_WS_CLOSE, js_mkundef());
  }
  ant_value_t writer_obj = ws_stream_writer(stream_obj);
  if (ws_is_writer(writer_obj))
    ws_writer_reject_closed_promise(js, writer_obj, stored_error);
}

static ant_value_t ws_finish_erroring_abort_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t stream_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  js_resolve_promise(js, p, js_mkundef());
  ws_reject_close_and_closed(js, stream_obj);
  return js_mkundef();
}

static ant_value_t ws_finish_erroring_abort_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t stream_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  js_reject_promise(js, p, reason);
  ws_reject_close_and_closed(js, stream_obj);
  return js_mkundef();
}

void writable_stream_finish_erroring(ant_t *js, ant_value_t stream_obj) {
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream || stream->state != WS_STATE_ERRORING) return;
  stream->state = WS_STATE_ERRORED;

  ant_value_t ctrl_obj = ws_stream_controller(stream_obj);
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);

  ant_value_t saved_abort_fn = ws_ctrl_abort_fn(ctrl_obj);
  ant_value_t saved_sink = ws_ctrl_sink(ctrl_obj);

  if (ctrl) {
    ctrl->queue_total_size = 0;
    ctrl->queue_sizes_len = 0;
  }
  ws_default_controller_clear_algorithms(ctrl_obj);
  ant_value_t stored_error = ws_stream_stored_error(stream_obj);

  ant_value_t wr_arr = ws_stream_write_requests(js, stream_obj);
  if (vtype(wr_arr) == T_ARR) {
    ant_offset_t len = js_arr_len(js, wr_arr);
    for (ant_offset_t i = 0; i < len; i++)
      js_reject_promise(js, js_arr_get(js, wr_arr, i), stored_error);
    ant_object_t *aobj = js_obj_ptr(wr_arr);
    aobj->u.array.len = 0;
  }

  if (stream->has_pending_abort) {
    stream->has_pending_abort = false;
    ant_value_t abort_promise = ws_stream_pending_abort_promise(stream_obj);
    js_set_slot(stream_obj, SLOT_WS_READY, js_mkundef());

    if (stream->pending_abort_was_already_erroring) {
      js_reject_promise(js, abort_promise, stored_error);
      ws_reject_close_and_closed(js, stream_obj);
      return;
    }

    ant_value_t result = js_mkundef();
    if (is_callable(saved_abort_fn)) {
      ant_value_t abort_args[1] = { stored_error };
      result = sv_vm_call(js->vm, js, saved_abort_fn, saved_sink, abort_args, 1, NULL, false);
    }

    if (is_err(result)) {
      ant_value_t thrown = js->thrown_value;
      js_reject_promise(js, abort_promise, is_object_type(thrown) ? thrown : result);
      ws_reject_close_and_closed(js, stream_obj);
    } else {
      ant_value_t wrapper = js_mkobj(js);
      js_set_slot(wrapper, SLOT_DATA, abort_promise);
      js_set_slot(wrapper, SLOT_ENTRIES, stream_obj);
      ant_value_t res_fn = js_heavy_mkfun(js, ws_finish_erroring_abort_resolve, wrapper);
      ant_value_t rej_fn = js_heavy_mkfun(js, ws_finish_erroring_abort_reject, wrapper);
      ws_chain_promise(js, result, res_fn, rej_fn);
    }
    
    return;
  }

  ws_reject_close_and_closed(js, stream_obj);
}

static void writable_stream_deal_with_rejection(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) return;
  if (stream->state == WS_STATE_WRITABLE) {
    writable_stream_start_erroring(js, stream_obj, error);
    return;
  }
  writable_stream_finish_erroring(js, stream_obj);
}

static void writable_stream_update_backpressure(ant_t *js, ant_value_t stream_obj, bool backpressure) {
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) return;
  ant_value_t writer_obj = ws_stream_writer(stream_obj);

  if (ws_is_writer(writer_obj) && stream->backpressure != backpressure) {
  if (backpressure) {
    ant_value_t ready = js_mkpromise(js);
    promise_mark_handled(ready);
    js_set_slot_wb(js, writer_obj, SLOT_WS_READY, ready);
  } else {
    ant_value_t ready = ws_writer_ready(writer_obj);
    if (!is_undefined(ready)) js_resolve_promise(js, ready, js_mkundef());
  }}
  
  stream->backpressure = backpressure;
}

static void writable_stream_finish_in_flight_write(ant_t *js, ant_value_t stream_obj) {
  ant_value_t p = ws_stream_in_flight_write(stream_obj);
  if (!is_undefined(p)) js_resolve_promise(js, p, js_mkundef());
  js_set_slot(stream_obj, SLOT_DEFAULT, js_mkundef());
}

static void writable_stream_finish_in_flight_write_with_error(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  ant_value_t p = ws_stream_in_flight_write(stream_obj);
  if (!is_undefined(p)) js_reject_promise(js, p, error);
  js_set_slot(stream_obj, SLOT_DEFAULT, js_mkundef());
  writable_stream_deal_with_rejection(js, stream_obj, error);
}

static void writable_stream_finish_in_flight_close(ant_t *js, ant_value_t stream_obj) {
  ant_value_t p = ws_stream_in_flight_close(stream_obj);
  if (!is_undefined(p)) js_resolve_promise(js, p, js_mkundef());
  js_set_slot(stream_obj, SLOT_WS_ABORT, js_mkundef());

  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) return;

  if (stream->state == WS_STATE_ERRORING) {
    js_set_slot(stream_obj, SLOT_AUX, js_mkundef());
    if (stream->has_pending_abort) {
      ant_value_t ap = ws_stream_pending_abort_promise(stream_obj);
      js_resolve_promise(js, ap, js_mkundef());
      stream->has_pending_abort = false;
      js_set_slot(stream_obj, SLOT_WS_READY, js_mkundef());
    }
  }
  stream->state = WS_STATE_CLOSED;

  ant_value_t writer_obj = ws_stream_writer(stream_obj);
  if (ws_is_writer(writer_obj)) {
    ant_value_t closed = ws_writer_closed(writer_obj);
    if (!is_undefined(closed)) js_resolve_promise(js, closed, js_mkundef());
  }
}

static void writable_stream_finish_in_flight_close_with_error(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  ant_value_t p = ws_stream_in_flight_close(stream_obj);
  if (!is_undefined(p)) js_reject_promise(js, p, error);
  js_set_slot(stream_obj, SLOT_WS_ABORT, js_mkundef());
  writable_stream_deal_with_rejection(js, stream_obj, error);
}

static void writable_stream_mark_first_write_in_flight(ant_t *js, ant_value_t stream_obj) {
  ant_value_t wr = ws_write_reqs_shift(js, stream_obj);
  js_set_slot_wb(js, stream_obj, SLOT_DEFAULT, wr);
}

static void writable_stream_mark_close_in_flight(ant_t *js, ant_value_t stream_obj) {
  ant_value_t cr = ws_stream_close_request(stream_obj);
  js_set_slot_wb(js, stream_obj, SLOT_WS_ABORT, cr);
  js_set_slot(stream_obj, SLOT_WS_CLOSE, js_mkundef());
}

static ant_value_t ws_process_write_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = js_get_slot(js->current_func, SLOT_DATA);
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);
  if (!ctrl) return js_mkundef();

  ant_value_t stream_obj = ws_ctrl_stream(ctrl_obj);
  writable_stream_finish_in_flight_write(js, stream_obj);

  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) return js_mkundef();

  ws_ctrl_queue_shift(js, ctrl_obj);
  if (ctrl->queue_sizes_len > 0) {
    double sz = ctrl->queue_sizes[0];
    ctrl->queue_sizes_len--;
    memmove(ctrl->queue_sizes, ctrl->queue_sizes + 1, ctrl->queue_sizes_len * sizeof(double));
    ctrl->queue_total_size -= sz;
    if (ctrl->queue_total_size < 0) ctrl->queue_total_size = 0;
  }

  if (!writable_stream_close_queued_or_in_flight(stream_obj) && stream->state == WS_STATE_WRITABLE) {
    bool bp = ws_default_controller_get_backpressure(ctrl);
    writable_stream_update_backpressure(js, stream_obj, bp);
  }

  ws_default_controller_advance_queue_if_needed(js, ctrl_obj);
  return js_mkundef();
}

static ant_value_t ws_process_write_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t stream_obj = ws_ctrl_stream(ctrl_obj);
  ws_stream_t *stream = ws_get_stream(stream_obj);

  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();

  if (stream && stream->state == WS_STATE_WRITABLE)
    ws_default_controller_clear_algorithms(ctrl_obj);

  writable_stream_finish_in_flight_write_with_error(js, stream_obj, reason);
  return js_mkundef();
}

static void ws_default_controller_process_write(ant_t *js, ant_value_t ctrl_obj, ant_value_t chunk) {
  ant_value_t stream_obj = ws_ctrl_stream(ctrl_obj);
  writable_stream_mark_first_write_in_flight(js, stream_obj);

  ant_value_t write_fn = ws_ctrl_write_fn(ctrl_obj);
  ant_value_t sink = ws_ctrl_sink(ctrl_obj);
  ant_value_t result = js_mkundef();
  if (is_callable(write_fn)) {
    ant_value_t write_args[2] = { chunk, ctrl_obj };
    result = sv_vm_call(js->vm, js, write_fn, sink, write_args, 2, NULL, false);
  }

  if (is_err(result)) {
    ws_stream_t *stream = ws_get_stream(stream_obj);
    ant_value_t thrown = js->thrown_value;
    ant_value_t err = is_object_type(thrown) ? thrown : result;
    if (stream && stream->state == WS_STATE_WRITABLE)
      ws_default_controller_clear_algorithms(ctrl_obj);
    writable_stream_finish_in_flight_write_with_error(js, stream_obj, err);
  } else {
    ant_value_t res_fn = js_heavy_mkfun(js, ws_process_write_resolve, ctrl_obj);
    ant_value_t rej_fn = js_heavy_mkfun(js, ws_process_write_reject, ctrl_obj);
    ws_chain_promise(js, result, res_fn, rej_fn);
  }
}

static ant_value_t ws_process_close_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_get_slot(js->current_func, SLOT_DATA);
  writable_stream_finish_in_flight_close(js, stream_obj);
  return js_mkundef();
}

static ant_value_t ws_process_close_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  writable_stream_finish_in_flight_close_with_error(js, stream_obj, reason);
  return js_mkundef();
}

static void ws_default_controller_process_close(ant_t *js, ant_value_t ctrl_obj) {
  ant_value_t stream_obj = ws_ctrl_stream(ctrl_obj);
  writable_stream_mark_close_in_flight(js, stream_obj);

  ws_ctrl_queue_shift(js, ctrl_obj);
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);
  if (ctrl && ctrl->queue_sizes_len > 0) {
    ctrl->queue_sizes_len--;
    memmove(ctrl->queue_sizes, ctrl->queue_sizes + 1, ctrl->queue_sizes_len * sizeof(double));
  }

  ant_value_t close_fn = ws_ctrl_close_fn(ctrl_obj);
  ant_value_t sink = ws_ctrl_sink(ctrl_obj);
  ws_default_controller_clear_algorithms(ctrl_obj);

  ant_value_t result = js_mkundef();
  if (is_callable(close_fn))
    result = sv_vm_call(js->vm, js, close_fn, sink, NULL, 0, NULL, false);

  if (is_err(result)) {
    ant_value_t thrown = js->thrown_value;
    ant_value_t err = is_object_type(thrown) ? thrown : result;
    writable_stream_finish_in_flight_close_with_error(js, stream_obj, err);
  } else {
    ant_value_t res_fn = js_heavy_mkfun(js, ws_process_close_resolve, stream_obj);
    ant_value_t rej_fn = js_heavy_mkfun(js, ws_process_close_reject, stream_obj);
    ws_chain_promise(js, result, res_fn, rej_fn);
  }
}

void ws_default_controller_advance_queue_if_needed(ant_t *js, ant_value_t ctrl_obj) {
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);
  if (!ctrl || !ctrl->started) return;

  ant_value_t stream_obj = ws_ctrl_stream(ctrl_obj);
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) return;

  if (!is_undefined(ws_stream_in_flight_write(stream_obj))) return;

  if (stream->state == WS_STATE_ERRORING) {
    writable_stream_finish_erroring(js, stream_obj);
    return;
  }

  if (ws_ctrl_queue_empty(ctrl_obj)) return;
  ant_value_t value = ws_ctrl_queue_peek(js, ctrl_obj);
  
  if (value == g_close_sentinel) ws_default_controller_process_close(js, ctrl_obj);
  else ws_default_controller_process_write(js, ctrl_obj, value);
}

static void ws_default_controller_close(ant_t *js, ant_value_t ctrl_obj) {
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);
  if (!ctrl) return;

  ws_ctrl_queue_push(js, ctrl_obj, g_close_sentinel);
  if (ctrl->queue_sizes_len >= ctrl->queue_sizes_cap) {
    uint32_t new_cap = ctrl->queue_sizes_cap ? ctrl->queue_sizes_cap * 2 : 8;
    double *ns = realloc(ctrl->queue_sizes, new_cap * sizeof(double));
    if (ns) { ctrl->queue_sizes = ns; ctrl->queue_sizes_cap = new_cap; }
  }
  if (ctrl->queue_sizes_len < ctrl->queue_sizes_cap)
    ctrl->queue_sizes[ctrl->queue_sizes_len++] = 0;

  ws_default_controller_advance_queue_if_needed(js, ctrl_obj);
}

static void ws_default_controller_write(ant_t *js, ant_value_t ctrl_obj, ant_value_t chunk, double chunk_size) {
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);
  if (!ctrl) return;

  ws_ctrl_queue_push(js, ctrl_obj, chunk);
  if (ctrl->queue_sizes_len >= ctrl->queue_sizes_cap) {
    uint32_t new_cap = ctrl->queue_sizes_cap ? ctrl->queue_sizes_cap * 2 : 8;
    double *ns = realloc(ctrl->queue_sizes, new_cap * sizeof(double));
    if (ns) { ctrl->queue_sizes = ns; ctrl->queue_sizes_cap = new_cap; }
  }
  if (ctrl->queue_sizes_len < ctrl->queue_sizes_cap)
    ctrl->queue_sizes[ctrl->queue_sizes_len++] = chunk_size;
  ctrl->queue_total_size += chunk_size;

  ant_value_t stream_obj = ws_ctrl_stream(ctrl_obj);
  if (!writable_stream_close_queued_or_in_flight(stream_obj)) {
    ws_stream_t *stream = ws_get_stream(stream_obj);
    if (stream && stream->state == WS_STATE_WRITABLE) {
      bool bp = ws_default_controller_get_backpressure(ctrl);
      writable_stream_update_backpressure(js, stream_obj, bp);
    }
  }

  ws_default_controller_advance_queue_if_needed(js, ctrl_obj);
}

void ws_default_controller_error(ant_t *js, ant_value_t ctrl_obj, ant_value_t error) {
  ant_value_t stream_obj = ws_ctrl_stream(ctrl_obj);
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream || stream->state != WS_STATE_WRITABLE) return;
  ws_default_controller_clear_algorithms(ctrl_obj);
  writable_stream_start_erroring(js, stream_obj, error);
}

ant_value_t writable_stream_close(ant_t *js, ant_value_t stream_obj) {
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) return js_mkundef();

  if (stream->state == WS_STATE_CLOSED || stream->state == WS_STATE_ERRORED) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot close a stream that is already closed or errored");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  if (writable_stream_close_queued_or_in_flight(stream_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot close an already-closing stream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }

  ant_value_t promise = js_mkpromise(js);
  js_set_slot_wb(js, stream_obj, SLOT_WS_CLOSE, promise);

  ant_value_t writer_obj = ws_stream_writer(stream_obj);
  if (ws_is_writer(writer_obj) && stream->backpressure && stream->state == WS_STATE_WRITABLE) {
    ant_value_t ready = ws_writer_ready(writer_obj);
    if (!is_undefined(ready)) js_resolve_promise(js, ready, js_mkundef());
  }

  ant_value_t ctrl_obj = ws_stream_controller(stream_obj);
  ws_default_controller_close(js, ctrl_obj);

  return promise;
}

static ant_value_t ws_abort_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t stream_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  js_resolve_promise(js, p, js_mkundef());

  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (stream) stream->has_pending_abort = false;
  js_set_slot(stream_obj, SLOT_WS_READY, js_mkundef());

  ant_value_t stored_error = ws_stream_stored_error(stream_obj);
  ant_value_t cr = ws_stream_close_request(stream_obj);
  
  if (!is_undefined(cr)) {
    js_reject_promise(js, cr, stored_error);
    js_set_slot(stream_obj, SLOT_WS_CLOSE, js_mkundef());
  }
  
  ant_value_t writer_obj = ws_stream_writer(stream_obj);
  if (ws_is_writer(writer_obj)) ws_writer_reject_closed_promise(js, writer_obj, stored_error);

  return js_mkundef();
}

static ant_value_t ws_abort_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t stream_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  js_reject_promise(js, p, reason);

  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (stream) stream->has_pending_abort = false;
  js_set_slot(stream_obj, SLOT_WS_READY, js_mkundef());

  ant_value_t stored_error = ws_stream_stored_error(stream_obj);
  ant_value_t cr = ws_stream_close_request(stream_obj);
  
  if (!is_undefined(cr)) {
    js_reject_promise(js, cr, stored_error);
    js_set_slot(stream_obj, SLOT_WS_CLOSE, js_mkundef());
  }
  
  ant_value_t writer_obj = ws_stream_writer(stream_obj);
  if (ws_is_writer(writer_obj))
    ws_writer_reject_closed_promise(js, writer_obj, stored_error);

  return js_mkundef();
}

ant_value_t writable_stream_abort(ant_t *js, ant_value_t stream_obj, ant_value_t reason) {
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) return js_mkundef();

  if (stream->state == WS_STATE_CLOSED || stream->state == WS_STATE_ERRORED) {
    ant_value_t p = js_mkpromise(js);
    js_resolve_promise(js, p, js_mkundef());
    return p;
  }

  if (stream->has_pending_abort) {
    return ws_stream_pending_abort_promise(stream_obj);
  }

  bool was_already_erroring = (stream->state == WS_STATE_ERRORING);

  ant_value_t promise = js_mkpromise(js);
  stream->has_pending_abort = true;
  stream->pending_abort_was_already_erroring = was_already_erroring;
  js_set_slot_wb(js, stream_obj, SLOT_WS_READY, promise);

  if (!was_already_erroring)
    writable_stream_start_erroring(js, stream_obj, reason);

  return promise;
}

ant_value_t ws_writer_write(ant_t *js, ant_value_t writer_obj, ant_value_t chunk) {
  ant_value_t stream_obj = ws_writer_stream(writer_obj);
  if (!ws_is_stream(stream_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Writer has no stream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }

  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WritableStream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }

  if (stream->state == WS_STATE_CLOSED) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot write to a closed WritableStream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  
  if (stream->state == WS_STATE_ERRORED) {
    ant_value_t p = js_mkpromise(js);
    js_reject_promise(js, p, ws_stream_stored_error(stream_obj));
    return p;
  }
  
  if (writable_stream_close_queued_or_in_flight(stream_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot write to a closing WritableStream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  
  if (stream->state == WS_STATE_ERRORING) {
    ant_value_t p = js_mkpromise(js);
    js_reject_promise(js, p, ws_stream_stored_error(stream_obj));
    return p;
  }

  ant_value_t ctrl_obj = ws_stream_controller(stream_obj);
  double chunk_size = 1;
  ant_value_t size_fn = ws_ctrl_size_fn(ctrl_obj);
  
  if (is_callable(size_fn)) {
    ant_value_t size_args[1] = { chunk };
    ant_value_t size_result = sv_vm_call(js->vm, js, size_fn, js_mkundef(), size_args, 1, NULL, false);
    if (is_err(size_result)) {
      ant_value_t thrown = js->thrown_value;
      ant_value_t err = is_object_type(thrown) ? thrown : size_result;
      ws_default_controller_error(js, ctrl_obj, err);
      ant_value_t p = js_mkpromise(js);
      js_reject_promise(js, p, err);
      return p;
    }
    if (vtype(size_result) == T_NUM) chunk_size = js_getnum(size_result);
    else chunk_size = js_to_number(js, size_result);
  }

  if (chunk_size < 0 || chunk_size != chunk_size || chunk_size == (double)INFINITY) {
    js_mkerr_typed(js, JS_ERR_RANGE,
      "The return value of a queuing strategy's size function must be a finite, non-NaN, non-negative number");
    ant_value_t err = is_object_type(js->thrown_value) ? js->thrown_value : js_mkundef();
    ws_default_controller_error(js, ctrl_obj, err);
    ant_value_t p = js_mkpromise(js);
    js_reject_promise(js, p, err);
    return p;
  }

  ant_value_t p = js_mkpromise(js);
  ws_write_reqs_push(js, stream_obj, p);
  ws_default_controller_write(js, ctrl_obj, chunk, chunk_size);

  return p;
}

static ant_value_t ws_start_resolve_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = js_get_slot(js->current_func, SLOT_DATA);
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);
  if (!ctrl) return js_mkundef();
  ctrl->started = true;
  ws_default_controller_advance_queue_if_needed(js, ctrl_obj);
  return js_mkundef();
}

static ant_value_t ws_start_reject_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = js_get_slot(js->current_func, SLOT_DATA);
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);
  if (!ctrl) return js_mkundef();
  ctrl->started = true;
  ant_value_t stream_obj = ws_ctrl_stream(ctrl_obj);
  writable_stream_deal_with_rejection(js, stream_obj, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t js_ws_controller_get_signal(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t signal_ctrl = ws_ctrl_signal(js->this_val);
  if (!is_object_type(signal_ctrl)) return js_mkundef();
  return js_get(js, signal_ctrl, "signal");
}

static ant_value_t js_ws_controller_error(ant_t *js, ant_value_t *args, int nargs) {
  ws_controller_t *ctrl = ws_get_controller(js->this_val);
  if (!ctrl) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WritableStreamDefaultController");
  ant_value_t stream_obj = ws_ctrl_stream(js->this_val);
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream || stream->state != WS_STATE_WRITABLE) return js_mkundef();
  ant_value_t e = (nargs > 0) ? args[0] : js_mkundef();
  ws_default_controller_error(js, js->this_val, e);
  return js_mkundef();
}

static ant_value_t js_ws_writer_get_closed(ant_t *js, ant_value_t *args, int nargs) {
  return ws_writer_closed(js->this_val);
}

static ant_value_t js_ws_writer_get_ready(ant_t *js, ant_value_t *args, int nargs) {
  return ws_writer_ready(js->this_val);
}

static ant_value_t js_ws_writer_get_desired_size(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = ws_writer_stream(js->this_val);
  if (!ws_is_stream(stream_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Writer has no stream");
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (!stream) return js_mknull();
  if (stream->state == WS_STATE_ERRORED || stream->state == WS_STATE_ERRORING) return js_mknull();
  if (stream->state == WS_STATE_CLOSED) return js_mknum(0);
  ant_value_t ctrl_obj = ws_stream_controller(stream_obj);
  ws_controller_t *ctrl = ws_get_controller(ctrl_obj);
  if (!ctrl) return js_mknull();
  return js_mknum(ws_default_controller_get_desired_size(ctrl));
}

static ant_value_t js_ws_writer_abort(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = ws_writer_stream(js->this_val);
  if (!ws_is_stream(stream_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Writer has no stream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  return writable_stream_abort(js, stream_obj, reason);
}

static ant_value_t js_ws_writer_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = ws_writer_stream(js->this_val);
  if (!ws_is_stream(stream_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Writer has no stream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  if (writable_stream_close_queued_or_in_flight(stream_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot close an already-closing stream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  return writable_stream_close(js, stream_obj);
}

static ant_value_t js_ws_writer_release_lock(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = ws_writer_stream(js->this_val);
  if (!ws_is_stream(stream_obj)) return js_mkundef();
  ant_value_t release_err = js_make_error_silent(js, JS_ERR_TYPE, "Writer was released");

  ws_writer_reject_ready_promise(js, js->this_val, release_err);
  ws_writer_reject_closed_promise(js, js->this_val, release_err);
  
  promise_mark_handled(ws_writer_ready(js->this_val));
  promise_mark_handled(ws_writer_closed(js->this_val));
  
  ws_writer_replace_ready_promise_rejected(js, js->this_val, release_err);
  ws_writer_replace_closed_promise_rejected(js, js->this_val, release_err);

  js_set_slot(stream_obj, SLOT_CTOR, js_mkundef());
  js_set_slot(js->this_val, SLOT_ENTRIES, js_mkundef());
  
  return js_mkundef();
}

static ant_value_t js_ws_writer_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = ws_writer_stream(js->this_val);
  if (!ws_is_stream(stream_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Writer has no stream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  return ws_writer_write(js, js->this_val, chunk);
}

ant_value_t js_ws_writer_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WritableStreamDefaultWriter constructor requires 'new'");
  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WritableStreamDefaultWriter requires a stream argument");

  ant_value_t stream_obj = args[0];
  if (!ws_is_stream(stream_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WritableStreamDefaultWriter argument must be a WritableStream");
  ws_stream_t *stream = ws_get_stream(stream_obj);
  if (ws_is_writer(ws_stream_writer(stream_obj)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WritableStream is already locked to a writer");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_ws_writer_proto);
  
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_WRITABLE_STREAM_WRITER));

  ant_value_t closed = js_mkpromise(js);
  ant_value_t ready = js_mkpromise(js);
  
  promise_mark_handled(closed);
  promise_mark_handled(ready);

  js_set_slot(obj, SLOT_ENTRIES, stream_obj);
  js_set_slot(obj, SLOT_RS_CLOSED, closed);
  js_set_slot(obj, SLOT_WS_READY, ready);
  js_set_slot(stream_obj, SLOT_CTOR, obj);

  if (stream->state == WS_STATE_WRITABLE) {
    if (writable_stream_close_queued_or_in_flight(stream_obj) || !stream->backpressure) js_resolve_promise(js, ready, js_mkundef());
  } else if (stream->state == WS_STATE_ERRORING) {
    ant_value_t stored_error = ws_stream_stored_error(stream_obj);
    js_reject_promise(js, ready, stored_error);
  } else if (stream->state == WS_STATE_CLOSED) {
    js_resolve_promise(js, ready, js_mkundef());
    js_resolve_promise(js, closed, js_mkundef());
  } else {
    ant_value_t stored_error = ws_stream_stored_error(stream_obj);
    js_reject_promise(js, ready, stored_error);
    js_reject_promise(js, closed, stored_error);
  }

  return obj;
}

ant_value_t ws_acquire_writer(ant_t *js, ant_value_t stream_obj) {
  ant_value_t writer_args[1] = { stream_obj };
  ant_value_t saved = js->new_target;
  
  js->new_target = g_ws_writer_proto;
  ant_value_t writer = js_ws_writer_ctor(js, writer_args, 1);
  js->new_target = saved;
  
  return writer;
}

static ant_value_t js_ws_get_locked(ant_t *js, ant_value_t *args, int nargs) {
  ws_stream_t *stream = ws_get_stream(js->this_val);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WritableStream");
  return js_bool(ws_is_writer(ws_stream_writer(js->this_val)));
}

static ant_value_t js_ws_abort(ant_t *js, ant_value_t *args, int nargs) {
  ws_stream_t *stream = ws_get_stream(js->this_val);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WritableStream");
  if (ws_is_writer(ws_stream_writer(js->this_val))) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot abort a locked WritableStream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  return writable_stream_abort(js, js->this_val, reason);
}

static ant_value_t js_ws_close(ant_t *js, ant_value_t *args, int nargs) {
  ws_stream_t *stream = ws_get_stream(js->this_val);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WritableStream");
  if (ws_is_writer(ws_stream_writer(js->this_val))) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot close a locked WritableStream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  if (writable_stream_close_queued_or_in_flight(js->this_val)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot close an already-closing stream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  return writable_stream_close(js, js->this_val);
}

static ant_value_t js_ws_get_writer(ant_t *js, ant_value_t *args, int nargs) {
  ws_stream_t *stream = ws_get_stream(js->this_val);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid WritableStream");

  ant_value_t writer_args[1] = { js->this_val };
  ant_value_t saved_new_target = js->new_target;
  js->new_target = g_ws_writer_proto;
  ant_value_t writer = js_ws_writer_ctor(js, writer_args, 1);
  js->new_target = saved_new_target;
  return writer;
}

static ant_value_t setup_ws_default_controller(
  ant_t *js, ant_value_t stream_obj, ant_value_t underlying_sink,
  ant_value_t write_fn, ant_value_t close_fn, ant_value_t abort_fn,
  ant_value_t size_fn, double hwm
) {
  ws_controller_t *ctrl = calloc(1, sizeof(ws_controller_t));
  if (!ctrl) return js_mkerr(js, "out of memory");
  ctrl->strategy_hwm = hwm;

  ant_value_t ctrl_obj = js_mkobj(js);
  js_set_proto_init(ctrl_obj, g_ws_controller_proto);
  js_set_slot(ctrl_obj, SLOT_BRAND, js_mknum(BRAND_WRITABLE_STREAM_CONTROLLER));
  js_set_slot(ctrl_obj, SLOT_DATA, ANT_PTR(ctrl));
  js_set_slot(ctrl_obj, SLOT_ENTRIES, stream_obj);
  js_set_slot(ctrl_obj, SLOT_WS_WRITE, write_fn);
  js_set_slot(ctrl_obj, SLOT_WS_CLOSE, close_fn);
  js_set_slot(ctrl_obj, SLOT_WS_ABORT, abort_fn);
  js_set_slot(ctrl_obj, SLOT_RS_SIZE, size_fn);
  js_set_slot(ctrl_obj, SLOT_CTOR, underlying_sink);
  js_set_slot(ctrl_obj, SLOT_AUX, js_mkarr(js));
  js_set_finalizer(ctrl_obj, ws_controller_finalize);

  ant_value_t ac_ctor = js_get(js, js_glob(js), "AbortController");
  ant_value_t ac = js_mkundef();
  if (is_callable(ac_ctor)) {
    ant_value_t ac_proto = js_get(js, ac_ctor, "prototype");
    ac = js_mkobj(js);
    if (is_object_type(ac_proto)) js_set_proto_init(ac, ac_proto);
    ant_value_t saved = js->new_target;
    js->new_target = ac_ctor;
    ant_value_t result = sv_vm_call(js->vm, js, ac_ctor, ac, NULL, 0, NULL, false);
    js->new_target = saved;
    if (is_err(result)) ac = js_mkundef();
  }
  
  js_set_slot(ctrl_obj, SLOT_WS_SIGNAL, ac);
  js_set_slot(stream_obj, SLOT_ENTRIES, ctrl_obj);

  bool backpressure = ws_default_controller_get_backpressure(ctrl);
  writable_stream_update_backpressure(js, stream_obj, backpressure);

  return ctrl_obj;
}

static ant_value_t js_ws_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "WritableStream constructor requires 'new'");

  ant_value_t underlying_sink = js_mkundef();
  if (nargs > 0 && !is_undefined(args[0])) {
    if (is_null(args[0]))
      return js_mkerr_typed(js, JS_ERR_TYPE, "The underlying sink cannot be null");
    underlying_sink = args[0];
  }

  if (is_object_type(underlying_sink)) {
    ant_value_t type_val = js_get(js, underlying_sink, "type");
    if (!is_undefined(type_val))
      return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid type is specified");
  }

  ant_value_t strategy = js_mkundef();
  if (nargs > 1 && !is_undefined(args[1]) && !is_null(args[1]))
    strategy = args[1];

  double hwm = 1;
  if (is_object_type(strategy)) {
    ant_value_t hwm_val = js_get(js, strategy, "highWaterMark");
    if (is_err(hwm_val)) return hwm_val;
    if (!is_undefined(hwm_val)) {
      hwm = js_to_number(js, hwm_val);
      if (hwm != hwm || hwm < 0) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid highWaterMark");
    }
  }

  ant_value_t size_fn = js_mkundef();
  if (is_object_type(strategy)) {
    ant_value_t s = js_get(js, strategy, "size");
    if (is_err(s)) return s;
    if (!is_undefined(s)) {
      if (!is_callable(s)) return js_mkerr_typed(js, JS_ERR_TYPE, "size must be a function");
      size_fn = s;
    }
  }

  ws_stream_t *st = calloc(1, sizeof(ws_stream_t));
  if (!st) return js_mkerr(js, "out of memory");
  st->state = WS_STATE_WRITABLE;

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_ws_proto);
  
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_WRITABLE_STREAM));
  js_set_slot(obj, SLOT_DATA, ANT_PTR(st));
  js_set_slot(obj, SLOT_SETTLED, js_mkarr(js));
  js_set_finalizer(obj, ws_stream_finalize);

  ant_value_t write_fn = js_mkundef();
  ant_value_t close_fn = js_mkundef();
  ant_value_t abort_fn = js_mkundef();
  ant_value_t start_fn = js_mkundef();

  if (is_object_type(underlying_sink)) {
    ant_value_t wv = js_getprop_fallback(js, underlying_sink, "write");
    if (is_err(wv)) return wv;
    if (!is_undefined(wv)) {
      if (!is_callable(wv)) return js_mkerr_typed(js, JS_ERR_TYPE, "write must be a function");
      write_fn = wv;
    }
    
    ant_value_t cv = js_getprop_fallback(js, underlying_sink, "close");
    if (is_err(cv)) return cv;
    if (!is_undefined(cv)) {
      if (!is_callable(cv)) return js_mkerr_typed(js, JS_ERR_TYPE, "close must be a function");
      close_fn = cv;
    }
    
    ant_value_t av = js_getprop_fallback(js, underlying_sink, "abort");
    if (is_err(av)) return av;
    if (!is_undefined(av)) {
      if (!is_callable(av)) return js_mkerr_typed(js, JS_ERR_TYPE, "abort must be a function");
      abort_fn = av;
    }
    
    ant_value_t sv = js_getprop_fallback(js, underlying_sink, "start");
    if (is_err(sv)) return sv;
    if (!is_undefined(sv)) {
      if (!is_callable(sv)) return js_mkerr_typed(js, JS_ERR_TYPE, "start must be a function");
      start_fn = sv;
    }
  }

  ant_value_t ctrl_obj = setup_ws_default_controller(js, obj, underlying_sink, write_fn, close_fn, abort_fn, size_fn, hwm);
  if (is_err(ctrl_obj)) return ctrl_obj;

  if (is_callable(start_fn)) {
    ant_value_t start_args[1] = { ctrl_obj };
    ant_value_t start_result = sv_vm_call(js->vm, js, start_fn, underlying_sink, start_args, 1, NULL, false);
    if (is_err(start_result)) return start_result;

    if (vtype(start_result) == T_PROMISE) {
      ant_value_t resolve_fn = js_heavy_mkfun(js, ws_start_resolve_handler, ctrl_obj);
      ant_value_t reject_fn = js_heavy_mkfun(js, ws_start_reject_handler, ctrl_obj);
      js_promise_then(js, start_result, resolve_fn, reject_fn);
    }

    if (vtype(start_result) != T_PROMISE) {
      ant_value_t resolved = js_mkpromise(js);
      js_resolve_promise(js, resolved, js_mkundef());
      ant_value_t res_fn = js_heavy_mkfun(js, ws_start_resolve_handler, ctrl_obj);
      ant_value_t rej_fn = js_heavy_mkfun(js, ws_start_reject_handler, ctrl_obj);
      js_promise_then(js, resolved, res_fn, rej_fn);
    }
  } else {
    ant_value_t resolved = js_mkpromise(js);
    js_resolve_promise(js, resolved, js_mkundef());
    ant_value_t res_fn = js_heavy_mkfun(js, ws_start_resolve_handler, ctrl_obj);
    ant_value_t rej_fn = js_heavy_mkfun(js, ws_start_reject_handler, ctrl_obj);
    js_promise_then(js, resolved, res_fn, rej_fn);
  }

  return obj;
}

static ant_value_t js_ws_controller_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "WritableStreamDefaultController cannot be constructed directly");
}

void gc_mark_writable_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  mark(js, g_ws_proto);
  mark(js, g_ws_writer_proto);
  mark(js, g_ws_controller_proto);
  mark(js, g_close_sentinel);
}

void init_writable_stream_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);

  g_close_sentinel = js_mkobj(js);
  g_ws_controller_proto = js_mkobj(js);
  
  js_set_getter_desc(js, g_ws_controller_proto, "signal", 6, js_mkfun(js_ws_controller_get_signal), JS_DESC_C);
  js_set(js, g_ws_controller_proto, "error", js_mkfun(js_ws_controller_error));
  js_set_descriptor(js, g_ws_controller_proto, "error", 5, JS_DESC_W | JS_DESC_C);
  js_set_sym(js, g_ws_controller_proto, get_toStringTag_sym(), js_mkstr(js, "WritableStreamDefaultController", 31));

  ant_value_t ctrl_ctor = js_make_ctor(js, js_ws_controller_ctor, g_ws_controller_proto, "WritableStreamDefaultController", 31);
  js_set(js, g, "WritableStreamDefaultController", ctrl_ctor);
  js_set_descriptor(js, g, "WritableStreamDefaultController", 31, JS_DESC_W | JS_DESC_C);

  g_ws_writer_proto = js_mkobj(js);
  js_set_getter_desc(js, g_ws_writer_proto, "closed", 6, js_mkfun(js_ws_writer_get_closed), JS_DESC_C);
  js_set_getter_desc(js, g_ws_writer_proto, "desiredSize", 11, js_mkfun(js_ws_writer_get_desired_size), JS_DESC_C);
  js_set_getter_desc(js, g_ws_writer_proto, "ready", 5, js_mkfun(js_ws_writer_get_ready), JS_DESC_C);
  js_set(js, g_ws_writer_proto, "abort", js_mkfun(js_ws_writer_abort));
  js_set_descriptor(js, g_ws_writer_proto, "abort", 5, JS_DESC_W | JS_DESC_C);
  js_set(js, g_ws_writer_proto, "close", js_mkfun(js_ws_writer_close));
  js_set_descriptor(js, g_ws_writer_proto, "close", 5, JS_DESC_W | JS_DESC_C);
  js_set(js, g_ws_writer_proto, "releaseLock", js_mkfun(js_ws_writer_release_lock));
  js_set_descriptor(js, g_ws_writer_proto, "releaseLock", 11, JS_DESC_W | JS_DESC_C);
  js_set(js, g_ws_writer_proto, "write", js_mkfun(js_ws_writer_write));
  js_set_descriptor(js, g_ws_writer_proto, "write", 5, JS_DESC_W | JS_DESC_C);
  js_set_sym(js, g_ws_writer_proto, get_toStringTag_sym(), js_mkstr(js, "WritableStreamDefaultWriter", 27));

  ant_value_t writer_ctor = js_make_ctor(js, js_ws_writer_ctor, g_ws_writer_proto, "WritableStreamDefaultWriter", 27);
  js_set(js, g, "WritableStreamDefaultWriter", writer_ctor);
  js_set_descriptor(js, g, "WritableStreamDefaultWriter", 27, JS_DESC_W | JS_DESC_C);

  g_ws_proto = js_mkobj(js);
  js_set_getter_desc(js, g_ws_proto, "locked", 6, js_mkfun(js_ws_get_locked), JS_DESC_C);
  js_set(js, g_ws_proto, "abort", js_mkfun(js_ws_abort));
  js_set_descriptor(js, g_ws_proto, "abort", 5, JS_DESC_W | JS_DESC_C);
  js_set(js, g_ws_proto, "close", js_mkfun(js_ws_close));
  js_set_descriptor(js, g_ws_proto, "close", 5, JS_DESC_W | JS_DESC_C);
  js_set(js, g_ws_proto, "getWriter", js_mkfun(js_ws_get_writer));
  js_set_descriptor(js, g_ws_proto, "getWriter", 9, JS_DESC_W | JS_DESC_C);
  js_set_sym(js, g_ws_proto, get_toStringTag_sym(), js_mkstr(js, "WritableStream", 14));

  ant_value_t ws_ctor = js_make_ctor(js, js_ws_ctor, g_ws_proto, "WritableStream", 14);
  js_set(js, g, "WritableStream", ws_ctor);
  js_set_descriptor(js, g, "WritableStream", 14, JS_DESC_W | JS_DESC_C);
}
