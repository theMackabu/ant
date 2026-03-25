#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/symbol.h"
#include "streams/readable.h"

static ant_value_t g_rs_proto;
static ant_value_t g_reader_proto;
static ant_value_t g_controller_proto;

static rs_stream_t *rs_get_stream(ant_value_t obj) {
  ant_value_t s = js_get_slot(obj, SLOT_DATA);
  if (vtype(s) != T_NUM) return NULL;
  return (rs_stream_t *)(uintptr_t)(size_t)js_getnum(s);
}

static rs_controller_t *rs_get_controller(ant_value_t obj) {
  ant_value_t s = js_get_slot(obj, SLOT_DATA);
  if (vtype(s) != T_NUM) return NULL;
  return (rs_controller_t *)(uintptr_t)(size_t)js_getnum(s);
}

static void rs_stream_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    free((rs_stream_t *)(uintptr_t)(size_t)js_getnum(entries[i].value));
    return;
  }}
}

static void rs_controller_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    rs_controller_t *ctrl = (rs_controller_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
    free(ctrl->queue_sizes);
    free(ctrl);
    return;
  }}
}

static inline ant_value_t rs_stream_controller(ant_t *js, ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_ENTRIES);
}

static inline ant_value_t rs_stream_reader(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_CTOR);
}

static inline ant_value_t rs_stream_error(ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_BUFFER);
}

static inline ant_value_t rs_ctrl_stream(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_ENTRIES);
}

static inline ant_value_t rs_ctrl_pull(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_RS_PULL);
}

static inline ant_value_t rs_ctrl_cancel(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_RS_CANCEL);
}

static inline ant_value_t rs_ctrl_size(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_RS_SIZE);
}

static inline ant_value_t rs_ctrl_queue(ant_t *js, ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_BUFFER);
}

static inline ant_value_t rs_reader_stream(ant_value_t reader_obj) {
  return js_get_slot(reader_obj, SLOT_ENTRIES);
}

static inline ant_value_t rs_reader_closed(ant_value_t reader_obj) {
  return js_get_slot(reader_obj, SLOT_RS_CLOSED);
}

static inline ant_value_t rs_reader_reqs(ant_value_t reader_obj) {
  return js_get_slot(reader_obj, SLOT_BUFFER);
}

static bool rs_reader_has_reqs(ant_t *js, ant_value_t reader_obj) {
  ant_value_t arr = rs_reader_reqs(reader_obj);
  return vtype(arr) == T_ARR && js_arr_len(js, arr) > 0;
}

static void rs_ctrl_queue_push(ant_t *js, ant_value_t ctrl_obj, ant_value_t value) {
  ant_value_t arr = rs_ctrl_queue(js, ctrl_obj);
  if (vtype(arr) == T_ARR) js_arr_push(js, arr, value);
}

static ant_value_t rs_ctrl_queue_shift(ant_t *js, ant_value_t ctrl_obj) {
  ant_value_t arr = rs_ctrl_queue(js, ctrl_obj);
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

static ant_offset_t rs_ctrl_queue_len(ant_t *js, ant_value_t ctrl_obj) {
  ant_value_t arr = rs_ctrl_queue(js, ctrl_obj);
  if (vtype(arr) != T_ARR) return 0;
  return js_arr_len(js, arr);
}

static void rs_reader_reqs_push(ant_t *js, ant_value_t reader_obj, ant_value_t promise) {
  ant_value_t arr = rs_reader_reqs(reader_obj);
  if (vtype(arr) == T_ARR) js_arr_push(js, arr, promise);
}

static ant_value_t rs_reader_reqs_shift(ant_t *js, ant_value_t reader_obj) {
  ant_value_t arr = rs_reader_reqs(reader_obj);
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

static void rs_default_controller_call_pull_if_needed(ant_t *js, ant_value_t controller_obj);
static void readable_stream_close(ant_t *js, ant_value_t stream_obj);
static void readable_stream_error(ant_t *js, ant_value_t stream_obj, ant_value_t e);
static bool rs_default_controller_can_close_or_enqueue(rs_controller_t *ctrl, rs_stream_t *stream);

static void rs_default_controller_clear_algorithms(ant_value_t ctrl_obj) {
  js_set_slot(ctrl_obj, SLOT_RS_PULL, js_mkundef());
  js_set_slot(ctrl_obj, SLOT_RS_CANCEL, js_mkundef());
  js_set_slot(ctrl_obj, SLOT_RS_SIZE, js_mkundef());
}

static double rs_default_controller_get_desired_size(rs_controller_t *ctrl, rs_stream_t *stream) {
  if (!stream) return 0;
  if (stream->state == RS_STATE_ERRORED) return -1;
  if (stream->state == RS_STATE_CLOSED) return 0;
  return ctrl->strategy_hwm - ctrl->queue_total_size;
}

static bool rs_default_controller_should_call_pull(ant_t *js, rs_controller_t *ctrl, rs_stream_t *stream, ant_value_t ctrl_obj) {
  if (!rs_default_controller_can_close_or_enqueue(ctrl, stream)) return false;
  if (!ctrl->started) return false;

  ant_value_t stream_obj = rs_ctrl_stream(ctrl_obj);
  ant_value_t reader_obj = rs_stream_reader(stream_obj);
  if (is_object_type(reader_obj) && rs_reader_has_reqs(js, reader_obj)) return true;

  double desired = rs_default_controller_get_desired_size(ctrl, stream);
  return desired > 0;
}

static bool rs_default_controller_can_close_or_enqueue(rs_controller_t *ctrl, rs_stream_t *stream) {
  if (!ctrl || !stream) return false;
  if (ctrl->close_requested) return false;
  if (stream->state != RS_STATE_READABLE) return false;
  return true;
}

static void rs_fulfill_read_request(ant_t *js, ant_value_t stream_obj, ant_value_t chunk, bool done) {
  ant_value_t reader_obj = rs_stream_reader(stream_obj);
  if (!is_object_type(reader_obj)) return;
  ant_value_t promise = rs_reader_reqs_shift(js, reader_obj);
  if (vtype(promise) == T_UNDEF) return;
  ant_value_t result = js_iter_result(js, !done, chunk);
  js_resolve_promise(js, promise, result);
}

static void rs_default_reader_error_read_requests(ant_t *js, ant_value_t reader_obj, ant_value_t e) {
  ant_value_t arr = rs_reader_reqs(reader_obj);
  if (vtype(arr) != T_ARR) return;
  ant_offset_t len = js_arr_len(js, arr);
  for (ant_offset_t i = 0; i < len; i++)
    js_reject_promise(js, js_arr_get(js, arr, i), e);
  ant_object_t *aobj = js_obj_ptr(arr);
  aobj->u.array.len = 0;
}

static void readable_stream_close(ant_t *js, ant_value_t stream_obj) {
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream) return;
  stream->state = RS_STATE_CLOSED;

  ant_value_t reader_obj = rs_stream_reader(stream_obj);
  if (is_object_type(reader_obj)) {
    ant_value_t arr = rs_reader_reqs(reader_obj);
    if (vtype(arr) == T_ARR) {
      ant_offset_t len = js_arr_len(js, arr);
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t result = js_iter_result(js, false, js_mkundef());
        js_resolve_promise(js, js_arr_get(js, arr, i), result);
      }
      ant_object_t *aobj = js_obj_ptr(arr);
      aobj->u.array.len = 0;
    }
    js_resolve_promise(js, rs_reader_closed(reader_obj), js_mkundef());
  }
}

static void readable_stream_error(ant_t *js, ant_value_t stream_obj, ant_value_t e) {
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream || stream->state != RS_STATE_READABLE) return;
  stream->state = RS_STATE_ERRORED;
  js_set_slot(stream_obj, SLOT_BUFFER, e);

  ant_value_t reader_obj = rs_stream_reader(stream_obj);
  if (is_object_type(reader_obj)) {
    js_reject_promise(js, rs_reader_closed(reader_obj), e);
    rs_default_reader_error_read_requests(js, reader_obj, e);
  }
}

static ant_value_t rs_cancel_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t p = js_get_slot(js->current_func, SLOT_DATA);
  js_resolve_promise(js, p, js_mkundef());
  return js_mkundef();
}

static ant_value_t rs_cancel_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t p = js_get_slot(js->current_func, SLOT_DATA);
  js_reject_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t readable_stream_cancel(ant_t *js, ant_value_t stream_obj, ant_value_t reason) {
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream) return js_mkundef();
  stream->disturbed = true;

  if (stream->state == RS_STATE_CLOSED) {
    ant_value_t p = js_mkpromise(js);
    js_resolve_promise(js, p, js_mkundef());
    return p;
  }
  if (stream->state == RS_STATE_ERRORED) {
    ant_value_t p = js_mkpromise(js);
    js_reject_promise(js, p, rs_stream_error(stream_obj));
    return p;
  }

  readable_stream_close(js, stream_obj);

  ant_value_t ctrl_obj = rs_stream_controller(js, stream_obj);
  ant_value_t cancel_fn = rs_ctrl_cancel(ctrl_obj);
  ant_value_t result = js_mkundef();
  if (is_callable(cancel_fn)) {
    ant_value_t cancel_args[1] = { reason };
    result = sv_vm_call(js->vm, js, cancel_fn, ctrl_obj, cancel_args, 1, NULL, false);
  }
  
  rs_default_controller_clear_algorithms(ctrl_obj);
  ant_value_t p = js_mkpromise(js);

  if (is_err(result)) {
    ant_value_t thrown = js->thrown_value;
    js_reject_promise(js, p, is_object_type(thrown) ? thrown : result);
  } else if (vtype(result) == T_PROMISE) {
    ant_value_t res_fn = js_heavy_mkfun(js, rs_cancel_resolve, p);
    ant_value_t rej_fn = js_heavy_mkfun(js, rs_cancel_reject, p);
    ant_value_t then_fn = js_get(js, result, "then");
    if (is_callable(then_fn)) {
      ant_value_t then_args[2] = { res_fn, rej_fn };
      sv_vm_call(js->vm, js, then_fn, result, then_args, 2, NULL, false);
    }
  } else js_resolve_promise(js, p, js_mkundef());

  return p;
}

static ant_value_t rs_pull_resolve_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = js_get_slot(js->current_func, SLOT_DATA);
  rs_controller_t *ctrl = rs_get_controller(ctrl_obj);
  if (!ctrl) return js_mkundef();
  ctrl->pulling = false;
  if (ctrl->pull_again) {
    ctrl->pull_again = false;
    rs_default_controller_call_pull_if_needed(js, ctrl_obj);
  }
  return js_mkundef();
}

static ant_value_t rs_pull_reject_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = js_get_slot(js->current_func, SLOT_DATA);
  rs_controller_t *ctrl = rs_get_controller(ctrl_obj);
  if (!ctrl) return js_mkundef();
  ant_value_t stream_obj = rs_ctrl_stream(ctrl_obj);
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (stream && stream->state == RS_STATE_READABLE)
    readable_stream_error(js, stream_obj, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static void rs_default_controller_call_pull_if_needed(ant_t *js, ant_value_t controller_obj) {
  rs_controller_t *ctrl = rs_get_controller(controller_obj);
  if (!ctrl) return;
  ant_value_t stream_obj = rs_ctrl_stream(controller_obj);
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream) return;

  if (!rs_default_controller_should_call_pull(js, ctrl, stream, controller_obj)) return;

  if (ctrl->pulling) { ctrl->pull_again = true; return; }
  ctrl->pulling = true;

  ant_value_t pull_fn = rs_ctrl_pull(controller_obj);
  if (is_callable(pull_fn)) {
    ant_value_t args[1] = { controller_obj };
    ant_value_t result = sv_vm_call(js->vm, js, pull_fn, js_mkundef(), args, 1, NULL, false);

    if (vtype(result) == T_PROMISE) {
      ant_value_t resolve_fn = js_heavy_mkfun(js, rs_pull_resolve_handler, controller_obj);
      ant_value_t reject_fn = js_heavy_mkfun(js, rs_pull_reject_handler, controller_obj);
      ant_value_t then_fn = js_get(js, result, "then");
      if (is_callable(then_fn)) {
        ant_value_t then_args[2] = { resolve_fn, reject_fn };
        sv_vm_call(js->vm, js, then_fn, result, then_args, 2, NULL, false);
      }
    } else if (is_err(result)) {
      if (stream->state == RS_STATE_READABLE) {
        ant_value_t thrown = js->thrown_value;
        readable_stream_error(js, stream_obj, is_object_type(thrown) ? thrown : result);
      }
    } else {
      ant_value_t resolved = js_mkpromise(js);
      js_resolve_promise(js, resolved, js_mkundef());
      ant_value_t resolve_fn = js_heavy_mkfun(js, rs_pull_resolve_handler, controller_obj);
      ant_value_t reject_fn = js_heavy_mkfun(js, rs_pull_reject_handler, controller_obj);
      ant_value_t then_fn = js_get(js, resolved, "then");
      if (is_callable(then_fn)) {
        ant_value_t then_args[2] = { resolve_fn, reject_fn };
        sv_vm_call(js->vm, js, then_fn, resolved, then_args, 2, NULL, false);
      }
    }
  } else ctrl->pulling = false;
}

static ant_value_t rs_default_reader_read(ant_t *js, ant_value_t reader_obj) {
  ant_value_t stream_obj = rs_reader_stream(reader_obj);
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Reader has no stream");

  stream->disturbed = true;

  if (stream->state == RS_STATE_CLOSED) {
    ant_value_t p = js_mkpromise(js);
    js_resolve_promise(js, p, js_iter_result(js, false, js_mkundef()));
    return p;
  }
  if (stream->state == RS_STATE_ERRORED) {
    ant_value_t p = js_mkpromise(js);
    js_reject_promise(js, p, rs_stream_error(stream_obj));
    return p;
  }

  ant_value_t ctrl_obj = rs_stream_controller(js, stream_obj);
  rs_controller_t *ctrl = rs_get_controller(ctrl_obj);
  if (ctrl && rs_ctrl_queue_len(js, ctrl_obj) > 0) {
    ant_value_t chunk = rs_ctrl_queue_shift(js, ctrl_obj);
    double chunk_size = 1;
    if (ctrl->queue_sizes_len > 0) {
      chunk_size = ctrl->queue_sizes[0];
      ctrl->queue_sizes_len--;
      memmove(ctrl->queue_sizes, ctrl->queue_sizes + 1, ctrl->queue_sizes_len * sizeof(double));
    }
    ctrl->queue_total_size -= chunk_size;
    if (ctrl->queue_total_size < 0) ctrl->queue_total_size = 0;
    bool should_close = ctrl->close_requested && rs_ctrl_queue_len(js, ctrl_obj) == 0;
    ant_value_t p = js_mkpromise(js);
    js_resolve_promise(js, p, js_iter_result(js, true, chunk));
    if (should_close) {
      rs_default_controller_clear_algorithms(ctrl_obj);
      readable_stream_close(js, stream_obj);
    } else rs_default_controller_call_pull_if_needed(js, ctrl_obj);

    return p;
  }

  ant_value_t p = js_mkpromise(js);
  rs_reader_reqs_push(js, reader_obj, p);
  rs_default_controller_call_pull_if_needed(js, ctrl_obj);
  return p;
}

static ant_value_t js_rs_controller_get_desired_size(ant_t *js, ant_value_t *args, int nargs) {
  rs_controller_t *ctrl = rs_get_controller(js->this_val);
  if (!ctrl) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStreamDefaultController");
  ant_value_t stream_obj = rs_ctrl_stream(js->this_val);
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream || stream->state == RS_STATE_ERRORED) return js_mknull();
  return js_mknum(rs_default_controller_get_desired_size(ctrl, stream));
}

static ant_value_t js_rs_controller_close(ant_t *js, ant_value_t *args, int nargs) {
  rs_controller_t *ctrl = rs_get_controller(js->this_val);
  if (!ctrl) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStreamDefaultController");
  ant_value_t stream_obj = rs_ctrl_stream(js->this_val);
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!rs_default_controller_can_close_or_enqueue(ctrl, stream))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The stream is not in a state that permits close");
  ctrl->close_requested = true;
  if (rs_ctrl_queue_len(js, js->this_val) == 0) {
    rs_default_controller_clear_algorithms(js->this_val);
    readable_stream_close(js, stream_obj);
  }
  return js_mkundef();
}

static ant_value_t js_rs_controller_enqueue(ant_t *js, ant_value_t *args, int nargs) {
  rs_controller_t *ctrl = rs_get_controller(js->this_val);
  if (!ctrl) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStreamDefaultController");
  ant_value_t stream_obj = rs_ctrl_stream(js->this_val);
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!rs_default_controller_can_close_or_enqueue(ctrl, stream))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The stream is not in a state that permits enqueue");

  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();

  ant_value_t reader_obj = rs_stream_reader(stream_obj);
  if (is_object_type(reader_obj) && rs_reader_has_reqs(js, reader_obj)) {
    rs_fulfill_read_request(js, stream_obj, chunk, false);
    rs_default_controller_call_pull_if_needed(js, js->this_val);
    return js_mkundef();
  }

  double chunk_size = 1;
  ant_value_t size_fn = rs_ctrl_size(js->this_val);
  if (is_callable(size_fn)) {
    ant_value_t size_args[1] = { chunk };
    ant_value_t size_result = sv_vm_call(js->vm, js, size_fn, js_mkundef(), size_args, 1, NULL, false);
    if (is_err(size_result)) {
      ant_value_t thrown = js->thrown_value;
      ant_value_t err = is_object_type(thrown) ? thrown : size_result;
      readable_stream_error(js, stream_obj, err);
      if (is_object_type(thrown)) return js_throw(js, thrown);
      return size_result;
    }
    if (vtype(size_result) == T_NUM) chunk_size = js_getnum(size_result);
    else chunk_size = js_to_number(js, size_result);
  }

  if (chunk_size < 0 || chunk_size != chunk_size || chunk_size == (double)INFINITY) {
    js_mkerr_typed(js, JS_ERR_RANGE,
      "The return value of a queuing strategy's size function must be a finite, non-NaN, non-negative number");
    ant_value_t err = is_object_type(js->thrown_value) ? js->thrown_value : js_mkundef();
    readable_stream_error(js, stream_obj, err);
    return js_throw(js, err);
  }

  rs_ctrl_queue_push(js, js->this_val, chunk);
  if (ctrl->queue_sizes_len >= ctrl->queue_sizes_cap) {
    uint32_t new_cap = ctrl->queue_sizes_cap ? ctrl->queue_sizes_cap * 2 : 8;
    double *ns = realloc(ctrl->queue_sizes, new_cap * sizeof(double));
    if (ns) { ctrl->queue_sizes = ns; ctrl->queue_sizes_cap = new_cap; }
  }
  if (ctrl->queue_sizes_len < ctrl->queue_sizes_cap)
    ctrl->queue_sizes[ctrl->queue_sizes_len++] = chunk_size;
  ctrl->queue_total_size += chunk_size;
  rs_default_controller_call_pull_if_needed(js, js->this_val);
  return js_mkundef();
}

static ant_value_t js_rs_controller_error(ant_t *js, ant_value_t *args, int nargs) {
  rs_controller_t *ctrl = rs_get_controller(js->this_val);
  if (!ctrl) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStreamDefaultController");
  ant_value_t stream_obj = rs_ctrl_stream(js->this_val);
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream || stream->state != RS_STATE_READABLE) return js_mkundef();
  
  ant_value_t e = (nargs > 0) ? args[0] : js_mkundef();
  ctrl->queue_total_size = 0;
  ctrl->queue_sizes_len = 0;
  rs_default_controller_clear_algorithms(js->this_val);
  readable_stream_error(js, stream_obj, e);
  
  return js_mkundef();
}

static ant_value_t js_rs_reader_get_closed(ant_t *js, ant_value_t *args, int nargs) {
  return rs_reader_closed(js->this_val);
}

static ant_value_t js_rs_reader_read(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = rs_reader_stream(js->this_val);
  if (!is_object_type(stream_obj) || !rs_get_stream(stream_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot read from a released reader");
  return rs_default_reader_read(js, js->this_val);
}

static ant_value_t js_rs_reader_release_lock(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = rs_reader_stream(js->this_val);
  if (!is_object_type(stream_obj)) return js_mkundef();
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream) return js_mkundef();

  if (rs_reader_has_reqs(js, js->this_val)) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Reader was released");
    rs_default_reader_error_read_requests(js, js->this_val, js->thrown_value);
  }

  ant_value_t new_closed = js_mkpromise(js);
  js_mkerr_typed(js, JS_ERR_TYPE, "Reader was released");
  ant_value_t release_err = js->thrown_value;

  if (stream->state == RS_STATE_READABLE)
    js_reject_promise(js, rs_reader_closed(js->this_val), release_err);

  js_reject_promise(js, new_closed, release_err);
  js_set_slot(js->this_val, SLOT_RS_CLOSED, new_closed);

  js_set_slot(stream_obj, SLOT_CTOR, js_mkundef());
  js_set_slot(js->this_val, SLOT_ENTRIES, js_mkundef());
  return js_mkundef();
}

static ant_value_t js_rs_reader_cancel(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = rs_reader_stream(js->this_val);
  if (!is_object_type(stream_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot cancel a released reader");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  return readable_stream_cancel(js, stream_obj, reason);
}

static ant_value_t js_rs_reader_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStreamDefaultReader constructor requires 'new'");
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStreamDefaultReader requires a stream argument");

  ant_value_t stream_obj = args[0];
  if (!is_object_type(stream_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStreamDefaultReader argument must be a ReadableStream");
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStreamDefaultReader argument must be a ReadableStream");
  if (is_object_type(rs_stream_reader(stream_obj)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream is already locked to a reader");

  ant_value_t closed = js_mkpromise(js);
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_reader_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_ENTRIES, stream_obj);
  js_set_slot(obj, SLOT_RS_CLOSED, closed);
  js_set_slot(obj, SLOT_BUFFER, js_mkarr(js));
  js_set_slot(stream_obj, SLOT_CTOR, obj);

  if (stream->state == RS_STATE_CLOSED)
    js_resolve_promise(js, closed, js_mkundef());
  else if (stream->state == RS_STATE_ERRORED)
    js_reject_promise(js, closed, rs_stream_error(stream_obj));

  return obj;
}

static ant_value_t js_rs_get_locked(ant_t *js, ant_value_t *args, int nargs) {
  rs_stream_t *stream = rs_get_stream(js->this_val);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");
  return js_bool(is_object_type(rs_stream_reader(js->this_val)));
}

static ant_value_t js_rs_cancel(ant_t *js, ant_value_t *args, int nargs) {
  rs_stream_t *stream = rs_get_stream(js->this_val);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");
  if (is_object_type(rs_stream_reader(js->this_val))) {
    ant_value_t p = js_mkpromise(js);
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot cancel a locked ReadableStream");
    js_reject_promise(js, p, js->thrown_value);
    return p;
  }
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  return readable_stream_cancel(js, js->this_val, reason);
}

static ant_value_t js_rs_get_reader(ant_t *js, ant_value_t *args, int nargs) {
  rs_stream_t *stream = rs_get_stream(js->this_val);
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");

  if (nargs > 0 && is_object_type(args[0])) {
    ant_value_t mode = js_get(js, args[0], "mode");
    if (!is_undefined(mode)) {
      ant_value_t mode_str_v = mode;
      if (vtype(mode) != T_STR) {
        mode_str_v = js_tostring_val(js, mode);
        if (is_err(mode_str_v)) return mode_str_v;
      }
      size_t mode_len;
      const char *mode_str = js_getstr(js, mode_str_v, &mode_len);
      if (mode_str && mode_len == 4 && memcmp(mode_str, "byob", 4) == 0)
        return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStreamBYOBReader is not yet implemented");
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid reader mode");
    }
  }

  ant_value_t reader_args[1] = { js->this_val };
  ant_value_t saved_new_target = js->new_target;
  js->new_target = g_reader_proto;
  ant_value_t reader = js_rs_reader_ctor(js, reader_args, 1);
  js->new_target = saved_new_target;
  return reader;
}

static ant_value_t js_rs_tee(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream.prototype.tee is not yet implemented");
}

static ant_value_t js_rs_pipe_through(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream.prototype.pipeThrough is not yet implemented");
}

static ant_value_t js_rs_pipe_to(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream.prototype.pipeTo is not yet implemented");
}

static ant_value_t js_rs_values(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream async iteration is not yet implemented");
}

static ant_value_t rs_start_resolve_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = js_get_slot(js->current_func, SLOT_DATA);
  rs_controller_t *ctrl = rs_get_controller(ctrl_obj);
  if (!ctrl) return js_mkundef();
  ctrl->started = true;
  ctrl->pulling = false;
  ctrl->pull_again = false;
  rs_default_controller_call_pull_if_needed(js, ctrl_obj);
  return js_mkundef();
}

static ant_value_t rs_start_reject_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = js_get_slot(js->current_func, SLOT_DATA);
  rs_controller_t *ctrl = rs_get_controller(ctrl_obj);
  if (!ctrl) return js_mkundef();
  ant_value_t stream_obj = rs_ctrl_stream(ctrl_obj);
  rs_stream_t *stream = rs_get_stream(stream_obj);
  if (stream && stream->state == RS_STATE_READABLE)
    readable_stream_error(js, stream_obj, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t setup_default_controller(
  ant_t *js, ant_value_t stream_obj,
  ant_value_t pull_fn, ant_value_t cancel_fn, ant_value_t size_fn,
  double hwm
) {
  rs_controller_t *ctrl = calloc(1, sizeof(rs_controller_t));
  if (!ctrl) return js_mkerr(js, "out of memory");
  ctrl->strategy_hwm = hwm;

  ant_value_t ctrl_obj = js_mkobj(js);
  js_set_proto_init(ctrl_obj, g_controller_proto);
  js_set_slot(ctrl_obj, SLOT_DATA, ANT_PTR(ctrl));
  js_set_slot(ctrl_obj, SLOT_ENTRIES, stream_obj);
  js_set_slot(ctrl_obj, SLOT_RS_PULL, pull_fn);
  js_set_slot(ctrl_obj, SLOT_RS_CANCEL, cancel_fn);
  js_set_slot(ctrl_obj, SLOT_RS_SIZE, size_fn);
  js_set_slot(ctrl_obj, SLOT_BUFFER, js_mkarr(js));
  js_set_finalizer(ctrl_obj, rs_controller_finalize);

  js_set_slot(stream_obj, SLOT_ENTRIES, ctrl_obj);
  return ctrl_obj;
}

static ant_value_t js_rs_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream constructor requires 'new'");

  ant_value_t underlying_source = js_mkundef();
  if (nargs > 0 && !is_undefined(args[0])) {
    if (is_null(args[0]))
      return js_mkerr_typed(js, JS_ERR_TYPE, "The underlying source cannot be null");
    underlying_source = args[0];
  }

  if (is_object_type(underlying_source)) {
  ant_value_t type_val = js_get(js, underlying_source, "type");
  if (!is_undefined(type_val)) {
    ant_value_t coerced = type_val;
    if (vtype(type_val) != T_STR) {
      coerced = js_tostring_val(js, type_val);
      if (is_err(coerced)) return coerced;
    }
    size_t tlen;
    const char *tstr = js_getstr(js, coerced, &tlen);
    if (tstr && tlen == 5 && memcmp(tstr, "bytes", 5) == 0)
      return js_mkerr_typed(js, JS_ERR_RANGE, "ReadableStream byte sources are not yet implemented");
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid type is specified");
  }}

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

  rs_stream_t *st = calloc(1, sizeof(rs_stream_t));
  if (!st) return js_mkerr(js, "out of memory");
  st->state = RS_STATE_READABLE;

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_rs_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_DATA, ANT_PTR(st));
  js_set_finalizer(obj, rs_stream_finalize);

  ant_value_t pull_fn = js_mkundef();
  ant_value_t cancel_fn = js_mkundef();
  ant_value_t start_fn = js_mkundef();

  if (is_object_type(underlying_source)) {
    ant_value_t pv = js_get(js, underlying_source, "pull");
    if (is_err(pv)) return pv;
    if (!is_undefined(pv)) {
      if (!is_callable(pv)) return js_mkerr_typed(js, JS_ERR_TYPE, "pull must be a function");
      pull_fn = pv;
    }
    ant_value_t cv = js_get(js, underlying_source, "cancel");
    if (is_err(cv)) return cv;
    if (!is_undefined(cv)) {
      if (!is_callable(cv)) return js_mkerr_typed(js, JS_ERR_TYPE, "cancel must be a function");
      cancel_fn = cv;
    }
    ant_value_t sv = js_get(js, underlying_source, "start");
    if (is_err(sv)) return sv;
    if (!is_undefined(sv)) {
      if (!is_callable(sv)) return js_mkerr_typed(js, JS_ERR_TYPE, "start must be a function");
      start_fn = sv;
    }
  }

  ant_value_t ctrl_obj = setup_default_controller(js, obj, pull_fn, cancel_fn, size_fn, hwm);
  if (is_err(ctrl_obj)) return ctrl_obj;

  if (is_callable(start_fn)) {
    ant_value_t start_args[1] = { ctrl_obj };
    ant_value_t start_result = sv_vm_call(js->vm, js, start_fn, underlying_source, start_args, 1, NULL, false);
    if (is_err(start_result)) return start_result;

    if (vtype(start_result) == T_PROMISE) {
      ant_value_t resolve_fn = js_heavy_mkfun(js, rs_start_resolve_handler, ctrl_obj);
      ant_value_t reject_fn = js_heavy_mkfun(js, rs_start_reject_handler, ctrl_obj);
      ant_value_t then_fn = js_get(js, start_result, "then");
      if (is_callable(then_fn)) {
        ant_value_t then_args[2] = { resolve_fn, reject_fn };
        sv_vm_call(js->vm, js, then_fn, start_result, then_args, 2, NULL, false);
      }
    }

    if (vtype(start_result) != T_PROMISE) {
      ant_value_t resolved = js_mkpromise(js);
      js_resolve_promise(js, resolved, js_mkundef());
      ant_value_t res_fn = js_heavy_mkfun(js, rs_start_resolve_handler, ctrl_obj);
      ant_value_t rej_fn = js_heavy_mkfun(js, rs_start_reject_handler, ctrl_obj);
      ant_value_t then_fn = js_get(js, resolved, "then");
      if (is_callable(then_fn)) {
        ant_value_t then_args[2] = { res_fn, rej_fn };
        sv_vm_call(js->vm, js, then_fn, resolved, then_args, 2, NULL, false);
      }
    }
  } else {
    ant_value_t resolved = js_mkpromise(js);
    js_resolve_promise(js, resolved, js_mkundef());
    ant_value_t res_fn = js_heavy_mkfun(js, rs_start_resolve_handler, ctrl_obj);
    ant_value_t rej_fn = js_heavy_mkfun(js, rs_start_reject_handler, ctrl_obj);
    ant_value_t then_fn = js_get(js, resolved, "then");
    if (is_callable(then_fn)) {
      ant_value_t then_args[2] = { res_fn, rej_fn };
      sv_vm_call(js->vm, js, then_fn, resolved, then_args, 2, NULL, false);
    }
  }

  return obj;
}

static ant_value_t js_rs_controller_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStreamDefaultController cannot be constructed directly");
}

void gc_mark_readable_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  mark(js, g_rs_proto);
  mark(js, g_reader_proto);
  mark(js, g_controller_proto);
}

void init_readable_stream_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);

  g_controller_proto = js_mkobj(js);
  js_set_getter_desc(js, g_controller_proto, "desiredSize", 11,
    js_mkfun(js_rs_controller_get_desired_size), JS_DESC_C);
  js_set(js, g_controller_proto, "close", js_mkfun(js_rs_controller_close));
  js_set_descriptor(js, g_controller_proto, "close", 5, JS_DESC_W | JS_DESC_C);
  js_set(js, g_controller_proto, "enqueue", js_mkfun(js_rs_controller_enqueue));
  js_set_descriptor(js, g_controller_proto, "enqueue", 7, JS_DESC_W | JS_DESC_C);
  js_set(js, g_controller_proto, "error", js_mkfun(js_rs_controller_error));
  js_set_descriptor(js, g_controller_proto, "error", 5, JS_DESC_W | JS_DESC_C);
  js_set_sym(js, g_controller_proto, get_toStringTag_sym(),
    js_mkstr(js, "ReadableStreamDefaultController", 31));

  ant_value_t ctrl_ctor = js_make_ctor(js, js_rs_controller_ctor, g_controller_proto,
    "ReadableStreamDefaultController", 31);
  js_set(js, g, "ReadableStreamDefaultController", ctrl_ctor);
  js_set_descriptor(js, g, "ReadableStreamDefaultController", 31, JS_DESC_W | JS_DESC_C);

  g_reader_proto = js_mkobj(js);
  js_set_getter_desc(js, g_reader_proto, "closed", 6,
    js_mkfun(js_rs_reader_get_closed), JS_DESC_C);
  js_set(js, g_reader_proto, "read", js_mkfun(js_rs_reader_read));
  js_set_descriptor(js, g_reader_proto, "read", 4, JS_DESC_W | JS_DESC_C);
  js_set(js, g_reader_proto, "releaseLock", js_mkfun(js_rs_reader_release_lock));
  js_set_descriptor(js, g_reader_proto, "releaseLock", 11, JS_DESC_W | JS_DESC_C);
  js_set(js, g_reader_proto, "cancel", js_mkfun(js_rs_reader_cancel));
  js_set_descriptor(js, g_reader_proto, "cancel", 6, JS_DESC_W | JS_DESC_C);
  js_set_sym(js, g_reader_proto, get_toStringTag_sym(),
    js_mkstr(js, "ReadableStreamDefaultReader", 27));

  ant_value_t reader_ctor = js_make_ctor(js, js_rs_reader_ctor, g_reader_proto,
    "ReadableStreamDefaultReader", 27);
  js_set(js, g, "ReadableStreamDefaultReader", reader_ctor);
  js_set_descriptor(js, g, "ReadableStreamDefaultReader", 27, JS_DESC_W | JS_DESC_C);

  g_rs_proto = js_mkobj(js);
  js_set_getter_desc(js, g_rs_proto, "locked", 6,
    js_mkfun(js_rs_get_locked), JS_DESC_C);
  js_set(js, g_rs_proto, "cancel", js_mkfun(js_rs_cancel));
  js_set_descriptor(js, g_rs_proto, "cancel", 6, JS_DESC_W | JS_DESC_C);
  js_set(js, g_rs_proto, "getReader", js_mkfun(js_rs_get_reader));
  js_set_descriptor(js, g_rs_proto, "getReader", 9, JS_DESC_W | JS_DESC_C);
  js_set(js, g_rs_proto, "tee", js_mkfun(js_rs_tee));
  js_set_descriptor(js, g_rs_proto, "tee", 3, JS_DESC_W | JS_DESC_C);
  js_set(js, g_rs_proto, "pipeThrough", js_mkfun(js_rs_pipe_through));
  js_set_descriptor(js, g_rs_proto, "pipeThrough", 11, JS_DESC_W | JS_DESC_C);
  js_set(js, g_rs_proto, "pipeTo", js_mkfun(js_rs_pipe_to));
  js_set_descriptor(js, g_rs_proto, "pipeTo", 6, JS_DESC_W | JS_DESC_C);
  js_set(js, g_rs_proto, "values", js_mkfun(js_rs_values));
  js_set_descriptor(js, g_rs_proto, "values", 6, JS_DESC_W | JS_DESC_C);
  js_set_sym(js, g_rs_proto, get_toStringTag_sym(),
    js_mkstr(js, "ReadableStream", 14));

  ant_value_t rs_ctor = js_make_ctor(js, js_rs_ctor, g_rs_proto, "ReadableStream", 14);
  js_set(js, g, "ReadableStream", rs_ctor);
  js_set_descriptor(js, g, "ReadableStream", 14, JS_DESC_W | JS_DESC_C);
}
