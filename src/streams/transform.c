#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "gc/roots.h"
#include "silver/engine.h"
#include "modules/assert.h"
#include "modules/symbol.h"
#include "streams/transform.h"
#include "streams/readable.h"
#include "streams/writable.h"


static inline bool ts_has_stream_shape(ant_value_t obj) {
  return vtype(js_get_slot(obj, SLOT_DATA)) == T_NUM
    && rs_is_stream(js_get_slot(obj, SLOT_ENTRIES))
    && ws_is_stream(js_get_slot(obj, SLOT_CTOR));
}

static inline bool ts_has_controller_shape(ant_value_t obj) {
  ant_value_t ts_obj = js_get_slot(obj, SLOT_DATA);
  return is_object_type(ts_obj)
    && js_check_brand(ts_obj, BRAND_TRANSFORM_STREAM)
    && vtype(js_get_slot(ts_obj, SLOT_DATA)) == T_NUM
    && js_get_slot(ts_obj, SLOT_DEFAULT) == obj;
}

bool ts_is_controller(ant_value_t obj) {
  return js_check_brand(obj, BRAND_TRANSFORM_STREAM_CONTROLLER)
    && ts_has_controller_shape(obj);
}

bool ts_is_stream(ant_value_t obj) {
  ant_value_t ctrl_obj = js_get_slot(obj, SLOT_DEFAULT);
  return js_check_brand(obj, BRAND_TRANSFORM_STREAM)
    && ts_has_stream_shape(obj)
    && js_check_brand(ctrl_obj, BRAND_TRANSFORM_STREAM_CONTROLLER)
    && ts_has_controller_shape(ctrl_obj);
}

ant_value_t ts_stream_readable(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_ENTRIES);
}

ant_value_t ts_stream_writable(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_CTOR);
}

static void ts_ws_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, WS_STREAM_NATIVE_TAG));
  js_clear_native(value, WS_STREAM_NATIVE_TAG);
}

static void ts_ws_ctrl_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ws_controller_t *ctrl = (ws_controller_t *)js_get_native(value, WS_CONTROLLER_NATIVE_TAG);
  if (!ctrl) return;
  free(ctrl->queue_sizes);
  free(ctrl);
  js_clear_native(value, WS_CONTROLLER_NATIVE_TAG);
}

static void ts_rs_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, RS_STREAM_NATIVE_TAG));
  js_clear_native(value, RS_STREAM_NATIVE_TAG);
}

static void ts_rs_ctrl_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  rs_controller_t *ctrl = (rs_controller_t *)js_get_native(value, RS_CONTROLLER_NATIVE_TAG);
  if (!ctrl) return;
  free(ctrl->queue_sizes);
  free(ctrl);
  js_clear_native(value, RS_CONTROLLER_NATIVE_TAG);
}

static inline bool ts_get_backpressure(ant_value_t ts_obj) {
  return js_getnum(js_get_slot(ts_obj, SLOT_DATA)) != 0;
}

static inline void ts_set_bp_flag(ant_value_t ts_obj, bool bp) {
  js_set_slot(ts_obj, SLOT_DATA, js_mknum(bp ? 1 : 0));
}

static inline ant_value_t ts_readable(ant_value_t ts_obj) {
  return ts_stream_readable(ts_obj);
}

static inline ant_value_t ts_writable(ant_value_t ts_obj) {
  return ts_stream_writable(ts_obj);
}

static inline ant_value_t ts_bp_promise(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_AUX);
}

ant_value_t ts_stream_controller(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_DEFAULT);
}

static inline ant_value_t ts_controller(ant_value_t ts_obj) {
  return ts_stream_controller(ts_obj);
}

static inline ant_value_t ts_ctrl_transform_fn(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_ENTRIES);
}

static inline ant_value_t ts_ctrl_flush_fn(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_CTOR);
}

static inline ant_value_t ts_ctrl_cancel_fn(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_AUX);
}

static inline ant_value_t ts_ctrl_transformer(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_SETTLED);
}

static inline ant_value_t ts_ctrl_stream(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_DATA);
}

static inline ant_value_t ts_ctrl_finish_promise(ant_value_t ctrl_obj) {
  return js_get_slot(ctrl_obj, SLOT_RS_PULL);
}

static inline ant_value_t ts_writable_stored_error(ant_value_t ts_obj) {
  return js_get_slot(ts_writable(ts_obj), SLOT_AUX);
}

static inline ant_value_t ts_cancel_promise(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_RS_CANCEL);
}

static inline bool ts_cancel_started_by_abort(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_WS_ABORT) == js_true;
}

static inline bool ts_is_flushing(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_WS_CLOSE) == js_true;
}

static inline ant_value_t ts_cancel_settle_error(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_RS_SIZE);
}

static inline bool ts_cancel_joined_abort(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_WS_WRITE) == js_true;
}

static inline bool ts_cancel_has_user_handler(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_WS_SIGNAL) == js_true;
}

static inline void ts_set_flushing(ant_value_t ts_obj, bool flushing) {
  js_set_slot(ts_obj, SLOT_WS_CLOSE, flushing ? js_true : js_false);
}

static inline void ts_set_cancel_state(ant_t *js, ant_value_t ts_obj, ant_value_t promise, bool started_by_abort, bool has_user_handler) {
  js_set_slot_wb(js, ts_obj, SLOT_RS_CANCEL, promise);
  js_set_slot(ts_obj, SLOT_WS_ABORT, started_by_abort ? js_true : js_false);
  js_set_slot(ts_obj, SLOT_RS_SIZE, js_mkundef());
  js_set_slot(ts_obj, SLOT_WS_WRITE, js_false);
  js_set_slot(ts_obj, SLOT_WS_SIGNAL, has_user_handler ? js_true : js_false);
}

static inline void ts_clear_cancel_state(ant_value_t ts_obj, ant_value_t promise) {
if (ts_cancel_promise(ts_obj) == promise) {
  js_set_slot(ts_obj, SLOT_RS_CANCEL, js_mkundef());
  js_set_slot(ts_obj, SLOT_WS_ABORT, js_false);
  js_set_slot(ts_obj, SLOT_WS_SIGNAL, js_false);
}}

static void ts_ctrl_clear_algorithms(ant_value_t ctrl_obj) {
  js_set_slot(ctrl_obj, SLOT_ENTRIES, js_mkundef());
  js_set_slot(ctrl_obj, SLOT_CTOR, js_mkundef());
  js_set_slot(ctrl_obj, SLOT_AUX, js_mkundef());
}

static ant_value_t ts_take_thrown_or(ant_t *js, ant_value_t fallback) {
  ant_value_t thrown = js->thrown_exists ? js->thrown_value : js_mkundef();
  ant_value_t err = is_object_type(thrown) ? thrown : fallback;
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
  js->thrown_stack = js_mkundef();
  return err;
}

static void ts_chain_promise(ant_t *js, ant_value_t val, ant_value_t res_fn, ant_value_t rej_fn) {
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

static void ts_error(ant_t *js, ant_value_t ts_obj, ant_value_t e) {
  ant_value_t readable = ts_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  rs_controller_t *rc = rs_get_controller(rs_ctrl);
  if (rc) {
    rc->queue_total_size = 0;
    rc->queue_sizes_len = 0;
    rs_default_controller_clear_algorithms(rs_ctrl);
  }
  readable_stream_error(js, readable, e);

  if (ts_get_backpressure(ts_obj)) {
    ant_value_t bp = ts_bp_promise(ts_obj);
    if (vtype(bp) == T_PROMISE) js_resolve_promise(js, bp, js_mkundef());
    ts_set_bp_flag(ts_obj, false);
  }
}

static void ts_error_writable_and_unblock_write(ant_t *js, ant_value_t ts_obj, ant_value_t e) {
  ant_value_t ctrl_obj = ts_controller(ts_obj);
  ts_ctrl_clear_algorithms(ctrl_obj);

  ant_value_t writable = ts_writable(ts_obj);
  ws_stream_t *ws = ws_get_stream(writable);
  if (ws && ws->state == WS_STATE_WRITABLE)
    ws_default_controller_error(js, ws_stream_controller(writable), e);

  if (ts_get_backpressure(ts_obj)) {
    ant_value_t bp = ts_bp_promise(ts_obj);
    if (vtype(bp) == T_PROMISE) js_resolve_promise(js, bp, js_mkundef());
    ts_set_bp_flag(ts_obj, false);
  }
}

static void ts_set_backpressure(ant_t *js, ant_value_t ts_obj, bool backpressure) {
  if (ts_get_backpressure(ts_obj)) {
    ant_value_t bp = ts_bp_promise(ts_obj);
    if (vtype(bp) == T_PROMISE) js_resolve_promise(js, bp, js_mkundef());
  }
  ant_value_t new_bp = js_mkpromise(js);
  js_set_slot_wb(js, ts_obj, SLOT_AUX, new_bp);
  ts_set_bp_flag(ts_obj, backpressure);
}

ant_value_t ts_ctrl_enqueue(ant_t *js, ant_value_t ctrl_obj, ant_value_t chunk) {
  ant_value_t ts_obj = ts_ctrl_stream(ctrl_obj);
  ant_value_t readable = ts_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  rs_stream_t *rs = rs_get_stream(readable);
  
  if (!rs || rs->state != RS_STATE_READABLE)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Readable side is not in a readable state");

  ant_value_t enqueue_result = rs_controller_enqueue(js, rs_ctrl, chunk);
  if (is_err(enqueue_result)) {
    ant_value_t err = ts_take_thrown_or(js, enqueue_result);
    ts_error_writable_and_unblock_write(js, ts_obj, err);
    return js_throw(js, err);
  }

  rs_controller_t *rc = rs_get_controller(rs_ctrl);
  bool bp = rc && ((rc->strategy_hwm - rc->queue_total_size) <= 0);
  if (bp != ts_get_backpressure(ts_obj)) ts_set_backpressure(js, ts_obj, bp);
    
  return js_mkundef();
}

void ts_ctrl_error(ant_t *js, ant_value_t ctrl_obj, ant_value_t e) {
  ant_value_t ts_obj = ts_ctrl_stream(ctrl_obj);
  if (vtype(ts_cancel_promise(ts_obj)) == T_PROMISE && ts_cancel_has_user_handler(ts_obj))
    js_set_slot_wb(js, ts_obj, SLOT_RS_SIZE, e);
  ts_error(js, ts_obj, e);
  ts_error_writable_and_unblock_write(js, ts_obj, e);
}

void ts_ctrl_terminate(ant_t *js, ant_value_t ctrl_obj) {
  ant_value_t ts_obj = ts_ctrl_stream(ctrl_obj);
  ant_value_t readable = ts_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  
  rs_controller_close(js, rs_ctrl);
  ant_value_t writable = ts_writable(ts_obj);
  ws_stream_t *ws = ws_get_stream(writable);
  
  if (ws && ws->state == WS_STATE_WRITABLE) {
    ant_value_t err = js_make_error_silent(js, JS_ERR_TYPE, "TransformStream readable side terminated");
    ts_error_writable_and_unblock_write(js, ts_obj, err);
  }
}

static ant_value_t ts_transform_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t p = js_get_slot(js->current_func, SLOT_DATA);
  js_resolve_promise(js, p, js_mkundef());
  return js_mkundef();
}

static ant_value_t ts_transform_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t e = (nargs > 0) ? args[0] : js_mkundef();
  ts_error(js, ts_obj, e);
  js_reject_promise(js, p, e);
  return js_mkundef();
}

static ant_value_t ts_ctrl_perform_transform(ant_t *js, ant_value_t ctrl_obj, ant_value_t chunk) {
  ant_value_t transform_fn = ts_ctrl_transform_fn(ctrl_obj);
  ant_value_t ts_obj = ts_ctrl_stream(ctrl_obj);
  ant_value_t p = js_mkpromise(js);
  promise_mark_handled(p);

  if (is_callable(transform_fn)) {
    ant_value_t call_args[2] = { chunk, ctrl_obj };
    ant_value_t result = sv_vm_call(js->vm, js, transform_fn, ts_ctrl_transformer(ctrl_obj), call_args, 2, NULL, false);

    if (is_err(result)) {
      ant_value_t err = ts_take_thrown_or(js, result);
      ts_error(js, ts_obj, err);
      js_reject_promise(js, p, err);
      return p;
    }

    ant_value_t res_fn = js_heavy_mkfun(js, ts_transform_resolve, p);
    ant_value_t wrapper = js_mkobj(js);
    js_set_slot(wrapper, SLOT_DATA, p);
    js_set_slot(wrapper, SLOT_ENTRIES, ts_obj);
    
    ant_value_t rej_fn = js_heavy_mkfun(js, ts_transform_reject, wrapper);
    ts_chain_promise(js, result, res_fn, rej_fn);
  } else {
    ant_value_t enqueue_result = ts_ctrl_enqueue(js, ctrl_obj, chunk);
    if (is_err(enqueue_result)) {
      ant_value_t err = ts_take_thrown_or(js, enqueue_result);
      js_reject_promise(js, p, err);
      return p;
    }
    js_resolve_promise(js, p, js_mkundef());
  }

  return p;
}

static ant_value_t ts_cancel_base_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ts_clear_cancel_state(ts_obj, p);
  js_resolve_promise(js, p, js_mkundef());
  return js_mkundef();
}

static ant_value_t ts_cancel_base_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t err = (nargs > 0) ? args[0] : js_mkundef();
  ts_clear_cancel_state(ts_obj, p);
  js_reject_promise(js, p, err);
  return js_mkundef();
}

static ant_value_t ts_run_cancel_algorithm(ant_t *js, ant_value_t ts_obj, ant_value_t cancel_fn, ant_value_t reason, bool started_by_abort) {
  ant_value_t existing = ts_cancel_promise(ts_obj);
  if (vtype(existing) == T_PROMISE) return existing;

  ant_value_t p = js_mkpromise(js);
  promise_mark_handled(p);
  ts_set_cancel_state(js, ts_obj, p, started_by_abort, is_callable(cancel_fn));
  ts_ctrl_clear_algorithms(ts_controller(ts_obj));

  ant_value_t result = js_mkundef();
  if (is_callable(cancel_fn)) {
    ant_value_t cancel_args[1] = { reason };
    result = sv_vm_call(js->vm, js, cancel_fn, ts_ctrl_transformer(ts_controller(ts_obj)), cancel_args, 1, NULL, false);
  }

  if (is_err(result)) {
    ant_value_t err = ts_take_thrown_or(js, result);
    ts_clear_cancel_state(ts_obj, p);
    js_reject_promise(js, p, err);
    return p;
  }

  ant_value_t wrapper = js_mkobj(js);
  js_set_slot(wrapper, SLOT_DATA, p);
  js_set_slot(wrapper, SLOT_ENTRIES, ts_obj);
  
  ant_value_t res_fn = js_heavy_mkfun(js, ts_cancel_base_resolve, wrapper);
  ant_value_t rej_fn = js_heavy_mkfun(js, ts_cancel_base_reject, wrapper);
  ts_chain_promise(js, result, res_fn, rej_fn);

  return p;
}

static ant_value_t ts_source_cancel_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t reason = js_get_slot(wrapper, SLOT_AUX);

  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  ant_value_t settle_error = ts_cancel_settle_error(ts_obj);
  if (is_object_type(settle_error)) {
    js_reject_promise(js, p, settle_error);
    return js_mkundef();
  }
  if (rs && rs->state == RS_STATE_ERRORED) {
    js_reject_promise(js, p, rs_stream_error(readable));
    return js_mkundef();
  }
  ant_value_t writable = ts_writable(ts_obj);
  ws_stream_t *ws = ws_get_stream(writable);

  if (ws && ws->state == WS_STATE_WRITABLE)
    ts_error_writable_and_unblock_write(js, ts_obj, reason);

  js_resolve_promise(js, p, js_mkundef());
  return js_mkundef();
}

static ant_value_t ts_source_cancel_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t err = (nargs > 0) ? args[0] : js_mkundef();
  ts_error_writable_and_unblock_write(js, ts_obj, err);
  js_reject_promise(js, p, err);
  return js_mkundef();
}

static ant_value_t ts_abort_cancel_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t reason = js_get_slot(wrapper, SLOT_AUX);

  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  
  if (rs && rs->state == RS_STATE_ERRORED) {
    js_reject_promise(js, p, rs_stream_error(readable));
    return js_mkundef();
  }

  if (ts_cancel_joined_abort(ts_obj)) js_set_slot_wb(js, ts_obj, SLOT_RS_SIZE, reason);
  ts_error(js, ts_obj, reason);
  
  if (ts_cancel_joined_abort(ts_obj)) js_reject_promise(js, p, reason);
  else js_resolve_promise(js, p, js_mkundef());
  
  return js_mkundef();
}

static ant_value_t ts_abort_cancel_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t err = (nargs > 0) ? args[0] : js_mkundef();
  ts_error(js, ts_obj, err);
  js_reject_promise(js, p, err);
  return js_mkundef();
}

static ant_value_t ts_close_cancel_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  ant_value_t settle_error = ts_cancel_settle_error(ts_obj);
  if (is_object_type(settle_error)) {
    js_reject_promise(js, p, settle_error);
    return js_mkundef();
  }
  if (rs && rs->state == RS_STATE_ERRORED) {
    js_reject_promise(js, p, rs_stream_error(readable));
    return js_mkundef();
  }
  js_resolve_promise(js, p, js_mkundef());
  return js_mkundef();
}

static ant_value_t ts_close_cancel_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t err = (nargs > 0) ? args[0] : js_mkundef();
  js_reject_promise(js, p, err);
  return js_mkundef();
}

static ant_value_t ts_sink_write_bp_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t ctrl_obj = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t chunk = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_CTOR);
  ws_stream_t *ws = ws_get_stream(ts_writable(ts_obj));

  if (ws && ws->state == WS_STATE_ERRORING) {
    ant_value_t err = ts_writable_stored_error(ts_obj);
    if (!is_object_type(err))
      err = js_make_error_silent(js, JS_ERR_TYPE, "WritableStream is in erroring state");
    ant_value_t fp = ts_ctrl_finish_promise(ctrl_obj);
    if (vtype(fp) == T_PROMISE) js_reject_promise(js, fp, err);
    return js_mkundef();
  }

  ant_value_t transform_p = ts_ctrl_perform_transform(js, ctrl_obj, chunk);
  ant_value_t fp = ts_ctrl_finish_promise(ctrl_obj);
  if (vtype(fp) == T_PROMISE) {
    ant_value_t resolve_fn = js_heavy_mkfun(js, ts_transform_resolve, fp);
    ant_value_t rej_wrapper = js_mkobj(js);
    js_set_slot(rej_wrapper, SLOT_DATA, fp);
    js_set_slot(rej_wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t reject_fn = js_heavy_mkfun(js, ts_transform_reject, rej_wrapper);
    ts_chain_promise(js, transform_p, resolve_fn, reject_fn);
  }
  return js_mkundef();
}

static ant_value_t ts_sink_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t ctrl_obj = ts_controller(ts_obj);

  ant_value_t finish_p = js_mkpromise(js);
  promise_mark_handled(finish_p);
  js_set_slot_wb(js, ctrl_obj, SLOT_RS_PULL, finish_p);

  if (ts_get_backpressure(ts_obj)) {
    ant_value_t wrapper = js_mkobj(js);
    js_set_slot(wrapper, SLOT_DATA, ctrl_obj);
    js_set_slot(wrapper, SLOT_ENTRIES, chunk);
    js_set_slot(wrapper, SLOT_CTOR, ts_obj);
    ant_value_t res_fn = js_heavy_mkfun(js, ts_sink_write_bp_resolve, wrapper);
    ant_value_t rej_wrapper = js_mkobj(js);
    js_set_slot(rej_wrapper, SLOT_DATA, finish_p);
    js_set_slot(rej_wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t rej_fn = js_heavy_mkfun(js, ts_transform_reject, rej_wrapper);
    ant_value_t bp = ts_bp_promise(ts_obj);
    ts_chain_promise(js, bp, res_fn, rej_fn);
  } else {
    ant_value_t transform_p = ts_ctrl_perform_transform(js, ctrl_obj, chunk);
    ant_value_t resolve_fn = js_heavy_mkfun(js, ts_transform_resolve, finish_p);
    ant_value_t rej_wrapper = js_mkobj(js);
    js_set_slot(rej_wrapper, SLOT_DATA, finish_p);
    js_set_slot(rej_wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t reject_fn = js_heavy_mkfun(js, ts_transform_reject, rej_wrapper);
    ts_chain_promise(js, transform_p, resolve_fn, reject_fn);
  }

  return finish_p;
}

static ant_value_t ts_sink_abort(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t ctrl_obj = ts_controller(ts_obj);
  ant_value_t cancel_fn = ts_ctrl_cancel_fn(ctrl_obj);
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  
  bool joined_source_cancel = vtype(ts_cancel_promise(ts_obj)) == T_PROMISE && !ts_cancel_started_by_abort(ts_obj);
  if (joined_source_cancel && ts_cancel_has_user_handler(ts_obj))
    js_set_slot(ts_obj, SLOT_WS_WRITE, js_true);

  ant_value_t p = js_mkpromise(js);
  ant_value_t base = ts_run_cancel_algorithm(js, ts_obj, cancel_fn, reason, true);
  ant_value_t wrapper = js_mkobj(js);
  js_set_slot(wrapper, SLOT_DATA, p);
  js_set_slot(wrapper, SLOT_ENTRIES, ts_obj);
  js_set_slot(wrapper, SLOT_AUX, reason);
  js_set_slot(wrapper, SLOT_CTOR, ts_cancel_started_by_abort(ts_obj) ? js_true : js_false);
  
  ant_value_t res_fn = js_heavy_mkfun(js, ts_abort_cancel_resolve, wrapper);
  ant_value_t rej_fn = js_heavy_mkfun(js, ts_abort_cancel_reject, wrapper);
  ts_chain_promise(js, base, res_fn, rej_fn);
  
  return p;
}

static ant_value_t ts_sink_close_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);

  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  ts_set_flushing(ts_obj, false);

  if (rs && rs->state == RS_STATE_READABLE) {
    rs_controller_close(js, rs_ctrl);
    js_resolve_promise(js, p, js_mkundef());
  } else if (rs && rs->state == RS_STATE_CLOSED) {
    js_resolve_promise(js, p, js_mkundef());
  } else if (rs && rs->state == RS_STATE_ERRORED) {
    js_reject_promise(js, p, rs_stream_error(readable));
  } else {
    js_reject_promise(js, p, js_make_error_silent(js, JS_ERR_TYPE, "TransformStream readable side is not in a readable state"));
  }

  return js_mkundef();
}

static ant_value_t ts_sink_close_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t e = (nargs > 0) ? args[0] : js_mkundef();
  ts_set_flushing(ts_obj, false);
  ts_error(js, ts_obj, e);
  js_reject_promise(js, p, e);
  return js_mkundef();
}

static ant_value_t ts_sink_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t ctrl_obj = ts_controller(ts_obj);
  ant_value_t readable = ts_readable(ts_obj);

  ant_value_t flush_fn = ts_ctrl_flush_fn(ctrl_obj);
  ant_value_t p = js_mkpromise(js);
  promise_mark_handled(p);

  ant_value_t cancel_p = ts_cancel_promise(ts_obj);
  if (vtype(cancel_p) == T_PROMISE) {
    ant_value_t wrapper = js_mkobj(js);
    js_set_slot(wrapper, SLOT_DATA, p);
    js_set_slot(wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t res_fn = js_heavy_mkfun(js, ts_close_cancel_resolve, wrapper);
    ant_value_t rej_fn = js_heavy_mkfun(js, ts_close_cancel_reject, wrapper);
    ts_chain_promise(js, cancel_p, res_fn, rej_fn);
    return p;
  }

  ts_ctrl_clear_algorithms(ctrl_obj);
  ts_set_flushing(ts_obj, true);

  if (is_callable(flush_fn)) {
    ant_value_t flush_args[1] = { ctrl_obj };
    ant_value_t result = sv_vm_call(js->vm, js, flush_fn, ts_ctrl_transformer(ctrl_obj), flush_args, 1, NULL, false);

    if (is_err(result)) {
      ant_value_t err = ts_take_thrown_or(js, result);
      ts_error(js, ts_obj, err);
      ts_set_flushing(ts_obj, false);
      js_reject_promise(js, p, err);
      return p;
    }

    ant_value_t wrapper = js_mkobj(js);
    js_set_slot(wrapper, SLOT_DATA, p);
    js_set_slot(wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t res_fn = js_heavy_mkfun(js, ts_sink_close_resolve, wrapper);
    ant_value_t rej_fn = js_heavy_mkfun(js, ts_sink_close_reject, wrapper);

    ts_chain_promise(js, result, res_fn, rej_fn);
  } else {
    rs_stream_t *rs = rs_get_stream(readable);
    if (rs && rs->state == RS_STATE_READABLE) {
      ant_value_t rs_ctrl = rs_stream_controller(js, readable);
      rs_controller_t *rc = rs_get_controller(rs_ctrl);
      if (rc) rc->defer_close = true;
      rs_controller_close(js, rs_ctrl);
      if (rc) rc->defer_close = false;
    }
    ts_set_flushing(ts_obj, false);
    js_resolve_promise(js, p, js_mkundef());
  }

  return p;
}

static ant_value_t ts_source_pull_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t p = js_get_slot(js->current_func, SLOT_DATA);
  js_resolve_promise(js, p, js_mkundef());
  return js_mkundef();
}

static ant_value_t ts_source_pull(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = js_get_slot(js->current_func, SLOT_DATA);

  if (ts_get_backpressure(ts_obj)) {
    ts_set_backpressure(js, ts_obj, false);
    ant_value_t p = js_mkpromise(js);
    js_resolve_promise(js, p, js_mkundef());
    return p;
  }

  ant_value_t p = js_mkpromise(js);
  js_resolve_promise(js, p, js_mkundef());
  return p;
}

static ant_value_t ts_source_cancel(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t ctrl_obj = ts_controller(ts_obj);
  ant_value_t cancel_fn = ts_ctrl_cancel_fn(ctrl_obj);
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();

  if (ts_is_flushing(ts_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_resolve_promise(js, p, js_mkundef());
    return p;
  }

  ant_value_t p = js_mkpromise(js);
  ant_value_t base = ts_run_cancel_algorithm(js, ts_obj, cancel_fn, reason, false);
  ant_value_t wrapper = js_mkobj(js);
  js_set_slot(wrapper, SLOT_DATA, p);
  js_set_slot(wrapper, SLOT_ENTRIES, ts_obj);
  js_set_slot(wrapper, SLOT_AUX, reason);
  js_set_slot(wrapper, SLOT_CTOR, ts_cancel_started_by_abort(ts_obj) ? js_true : js_false);
  ant_value_t res_fn = js_heavy_mkfun(js, ts_source_cancel_resolve, wrapper);
  ant_value_t rej_fn = js_heavy_mkfun(js, ts_source_cancel_reject, wrapper);
  ts_chain_promise(js, base, res_fn, rej_fn);
  return p;
}

static ant_value_t js_ts_ctrl_get_desired_size(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = ts_ctrl_stream(js->this_val);
  ant_value_t readable = ts_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  rs_controller_t *rc = rs_get_controller(rs_ctrl);
  rs_stream_t *rs = rs_get_stream(readable);
  if (!rc || !rs) return js_mknull();
  if (rs->state == RS_STATE_ERRORED) return js_mknull();
  if (rs->state == RS_STATE_CLOSED) return js_mknum(0);
  return js_mknum(rc->strategy_hwm - rc->queue_total_size);
}

static ant_value_t js_ts_ctrl_enqueue(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = ts_ctrl_stream(js->this_val);
  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  if (!rs || rs->state != RS_STATE_READABLE)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Readable side is not in a readable state");

  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  return ts_ctrl_enqueue(js, js->this_val, chunk);
}

static ant_value_t js_ts_ctrl_error(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t e = (nargs > 0) ? args[0] : js_mkundef();
  ts_ctrl_error(js, js->this_val, e);
  return js_mkundef();
}

static ant_value_t js_ts_ctrl_terminate(ant_t *js, ant_value_t *args, int nargs) {
  ts_ctrl_terminate(js, js->this_val);
  return js_mkundef();
}

static ant_value_t js_ts_get_readable(ant_t *js, ant_value_t *args, int nargs) {
  if (!ts_is_stream(js->this_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStream");
  return ts_readable(js->this_val);
}

static ant_value_t js_ts_get_writable(ant_t *js, ant_value_t *args, int nargs) {
  if (!ts_is_stream(js->this_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStream");
  return ts_writable(js->this_val);
}

static ant_value_t ts_start_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t writable = ts_writable(ts_obj);
  ant_value_t ws_ctrl = ws_stream_controller(writable);
  ws_controller_t *wc = ws_get_controller(ws_ctrl);
  if (wc) wc->started = true;
  ws_default_controller_advance_queue_if_needed(js, ws_ctrl);

  ant_value_t readable = ts_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  rs_controller_t *rc = rs_get_controller(rs_ctrl);
  if (rc) {
    rc->started = true;
    rc->pulling = false;
    rc->pull_again = false;
    rs_default_controller_call_pull_if_needed(js, rs_ctrl);
  }

  return js_mkundef();
}

static ant_value_t ts_start_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t e = (nargs > 0) ? args[0] : js_mkundef();

  ant_value_t writable = ts_writable(ts_obj);
  ant_value_t ws_ctrl = ws_stream_controller(writable);
  ws_controller_t *wc = ws_get_controller(ws_ctrl);
  if (wc) wc->started = true;

  ant_value_t readable = ts_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  rs_controller_t *rc = rs_get_controller(rs_ctrl);
  if (rc) rc->started = true;

  readable_stream_error(js, readable, e);

  ws_stream_t *ws = ws_get_stream(writable);
  if (ws && ws->state == WS_STATE_WRITABLE)
    ws_default_controller_error(js, ws_ctrl, e);
  else if (ws && ws->state == WS_STATE_ERRORING)
    writable_stream_finish_erroring(js, writable);

  return js_mkundef();
}

ant_value_t js_ts_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TransformStream constructor requires 'new'");

  ant_value_t transformer = js_mkundef();
  if (nargs > 0 && !is_undefined(args[0]))
    transformer = args[0];

  if (is_object_type(transformer)) {
    ant_value_t rt_val = js_getprop_fallback(js, transformer, "readableType");
    if (!is_undefined(rt_val))
      return js_mkerr_typed(js, JS_ERR_RANGE, "readableType is not supported");
    ant_value_t wt_val = js_getprop_fallback(js, transformer, "writableType");
    if (!is_undefined(wt_val))
      return js_mkerr_typed(js, JS_ERR_RANGE, "writableType is not supported");
  }

  ant_value_t writable_strategy = js_mkundef();
  if (nargs > 1 && !is_undefined(args[1]) && !is_null(args[1]))
    writable_strategy = args[1];

  ant_value_t readable_strategy = js_mkundef();
  if (nargs > 2 && !is_undefined(args[2]) && !is_null(args[2]))
    readable_strategy = args[2];

  double writable_hwm = 1;
  ant_value_t writable_size_fn = js_mkundef();
  if (is_object_type(writable_strategy)) {
    ant_value_t hwm_val = js_getprop_fallback(js, writable_strategy, "highWaterMark");
    if (is_err(hwm_val)) return hwm_val;
    if (!is_undefined(hwm_val)) {
      writable_hwm = js_to_number(js, hwm_val);
      if (writable_hwm != writable_hwm || writable_hwm < 0)
        return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid highWaterMark");
    }
    ant_value_t s = js_getprop_fallback(js, writable_strategy, "size");
    if (is_err(s)) return s;
    if (!is_undefined(s)) {
      if (!is_callable(s)) return js_mkerr_typed(js, JS_ERR_TYPE, "size must be a function");
      writable_size_fn = s;
    }
  }

  double readable_hwm = 0;
  ant_value_t readable_size_fn = js_mkundef();
  if (is_object_type(readable_strategy)) {
    ant_value_t hwm_val = js_getprop_fallback(js, readable_strategy, "highWaterMark");
    if (is_err(hwm_val)) return hwm_val;
    if (!is_undefined(hwm_val)) {
      readable_hwm = js_to_number(js, hwm_val);
      if (readable_hwm != readable_hwm || readable_hwm < 0)
        return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid highWaterMark");
    }
    ant_value_t s = js_getprop_fallback(js, readable_strategy, "size");
    if (is_err(s)) return s;
    if (!is_undefined(s)) {
      if (!is_callable(s)) return js_mkerr_typed(js, JS_ERR_TYPE, "size must be a function");
      readable_size_fn = s;
    }
  }

  ant_value_t transform_fn = js_mkundef();
  ant_value_t flush_fn = js_mkundef();
  ant_value_t cancel_fn = js_mkundef();
  ant_value_t start_fn = js_mkundef();

  if (is_object_type(transformer)) {
    ant_value_t tv = js_getprop_fallback(js, transformer, "transform");
    if (is_err(tv)) return tv;
    if (!is_undefined(tv)) {
      if (!is_callable(tv)) return js_mkerr_typed(js, JS_ERR_TYPE, "transform must be a function");
      transform_fn = tv;
    }
    ant_value_t fv = js_getprop_fallback(js, transformer, "flush");
    if (is_err(fv)) return fv;
    if (!is_undefined(fv)) {
      if (!is_callable(fv)) return js_mkerr_typed(js, JS_ERR_TYPE, "flush must be a function");
      flush_fn = fv;
    }
    ant_value_t cv = js_getprop_fallback(js, transformer, "cancel");
    if (is_err(cv)) return cv;
    if (!is_undefined(cv)) {
      if (!is_callable(cv)) return js_mkerr_typed(js, JS_ERR_TYPE, "cancel must be a function");
      cancel_fn = cv;
    }
    ant_value_t sv = js_getprop_fallback(js, transformer, "start");
    if (is_err(sv)) return sv;
    if (!is_undefined(sv)) {
      if (!is_callable(sv)) return js_mkerr_typed(js, JS_ERR_TYPE, "start must be a function");
      start_fn = sv;
    }
  }

  ant_value_t ts_obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.ts_proto);
  if (is_object_type(proto)) js_set_proto_init(ts_obj, proto);
  js_set_slot(ts_obj, SLOT_BRAND, js_mknum(BRAND_TRANSFORM_STREAM));
  js_set_slot(ts_obj, SLOT_DATA, js_mknum(0));

  ant_value_t ctrl_obj = js_mkobj(js);
  js_set_proto_init(ctrl_obj, js->sym.ts_ctrl_proto);
  js_set_slot(ctrl_obj, SLOT_BRAND, js_mknum(BRAND_TRANSFORM_STREAM_CONTROLLER));
  js_set_slot(ctrl_obj, SLOT_DATA, ts_obj);
  js_set_slot(ctrl_obj, SLOT_ENTRIES, transform_fn);
  js_set_slot(ctrl_obj, SLOT_CTOR, flush_fn);
  js_set_slot(ctrl_obj, SLOT_AUX, cancel_fn);
  js_set_slot(ctrl_obj, SLOT_SETTLED, transformer);
  js_set_slot(ctrl_obj, SLOT_RS_PULL, js_mkundef());
  js_set_slot(ts_obj, SLOT_DEFAULT, ctrl_obj);
  js_set_slot(ts_obj, SLOT_RS_CANCEL, js_mkundef());
  js_set_slot(ts_obj, SLOT_WS_ABORT, js_false);
  js_set_slot(ts_obj, SLOT_WS_CLOSE, js_false);
  js_set_slot(ts_obj, SLOT_RS_SIZE, js_mkundef());
  js_set_slot(ts_obj, SLOT_WS_WRITE, js_false);
  js_set_slot(ts_obj, SLOT_WS_SIGNAL, js_false);

  ant_value_t sink_write = js_heavy_mkfun(js, ts_sink_write, ts_obj);
  ant_value_t sink_abort = js_heavy_mkfun(js, ts_sink_abort, ts_obj);
  ant_value_t sink_close = js_heavy_mkfun(js, ts_sink_close, ts_obj);

  ant_value_t source_pull = js_heavy_mkfun(js, ts_source_pull, ts_obj);
  ant_value_t source_cancel = js_heavy_mkfun(js, ts_source_cancel, ts_obj);

  rs_stream_t *rst = calloc(1, sizeof(rs_stream_t));
  if (!rst) return js_mkerr(js, "out of memory");
  rst->state = RS_STATE_READABLE;

  ant_value_t rs_obj = js_mkobj(js);
  js_set_proto_init(rs_obj, js->sym.rs_proto);
  js_set_slot(rs_obj, SLOT_BRAND, js_mknum(BRAND_READABLE_STREAM));
  js_set_native(rs_obj, rst, RS_STREAM_NATIVE_TAG);
  js_set_finalizer(rs_obj, ts_rs_finalize);

  rs_controller_t *rcc = calloc(1, sizeof(rs_controller_t));
  if (!rcc) { free(rst); return js_mkerr(js, "out of memory"); }
  rcc->strategy_hwm = readable_hwm;

  ant_value_t rs_ctrl_obj = js_mkobj(js);
  js_set_proto_init(rs_ctrl_obj, js->sym.controller_proto);
  js_set_slot(rs_ctrl_obj, SLOT_BRAND, js_mknum(BRAND_READABLE_STREAM_CONTROLLER));
  js_set_native(rs_ctrl_obj, rcc, RS_CONTROLLER_NATIVE_TAG);
  js_set_slot(rs_ctrl_obj, SLOT_ENTRIES, rs_obj);
  js_set_slot(rs_ctrl_obj, SLOT_RS_PULL, source_pull);
  js_set_slot(rs_ctrl_obj, SLOT_RS_CANCEL, source_cancel);
  js_set_slot(rs_ctrl_obj, SLOT_RS_SIZE, readable_size_fn);
  js_set_slot(rs_ctrl_obj, SLOT_AUX, js_mkarr(js));
  js_set_finalizer(rs_ctrl_obj, ts_rs_ctrl_finalize);
  js_set_slot(rs_obj, SLOT_ENTRIES, rs_ctrl_obj);

  js_set_slot(ts_obj, SLOT_ENTRIES, rs_obj);

  ws_stream_t *wst = calloc(1, sizeof(ws_stream_t));
  if (!wst) return js_mkerr(js, "out of memory");
  wst->state = WS_STATE_WRITABLE;

  ant_value_t ws_obj = js_mkobj(js);
  js_set_proto_init(ws_obj, js->sym.ws_proto);
  js_set_slot(ws_obj, SLOT_BRAND, js_mknum(BRAND_WRITABLE_STREAM));
  js_set_native(ws_obj, wst, WS_STREAM_NATIVE_TAG);
  js_set_slot(ws_obj, SLOT_SETTLED, js_mkarr(js));
  js_set_finalizer(ws_obj, ts_ws_finalize);

  js_set_slot(ts_obj, SLOT_CTOR, ws_obj);

  ant_value_t bp_promise = js_mkpromise(js);
  js_set_slot_wb(js, ts_obj, SLOT_AUX, bp_promise);

  ts_set_backpressure(js, ts_obj, true);

  ws_controller_t *wc = calloc(1, sizeof(ws_controller_t));
  if (!wc) { free(wst); return js_mkerr(js, "out of memory"); }
  wc->strategy_hwm = writable_hwm;

  ant_value_t ws_ctrl_obj = js_mkobj(js);
  js_set_proto_init(ws_ctrl_obj, js->sym.ws_controller_proto);
  js_set_slot(ws_ctrl_obj, SLOT_BRAND, js_mknum(BRAND_WRITABLE_STREAM_CONTROLLER));
  js_set_native(ws_ctrl_obj, wc, WS_CONTROLLER_NATIVE_TAG);
  js_set_slot(ws_ctrl_obj, SLOT_ENTRIES, ws_obj);
  js_set_slot(ws_ctrl_obj, SLOT_WS_WRITE, sink_write);
  js_set_slot(ws_ctrl_obj, SLOT_WS_CLOSE, sink_close);
  js_set_slot(ws_ctrl_obj, SLOT_WS_ABORT, sink_abort);
  js_set_slot(ws_ctrl_obj, SLOT_RS_SIZE, writable_size_fn);
  js_set_slot(ws_ctrl_obj, SLOT_CTOR, js_mkundef());
  js_set_slot(ws_ctrl_obj, SLOT_AUX, js_mkarr(js));
  js_set_finalizer(ws_ctrl_obj, ts_ws_ctrl_finalize);

  js_set_slot(ws_obj, SLOT_ENTRIES, ws_ctrl_obj);

  if (is_callable(start_fn)) {
    ant_value_t start_args[1] = { ctrl_obj };
    ant_value_t start_result = sv_vm_call(js->vm, js, start_fn, transformer, start_args, 1, NULL, false);
    if (is_err(start_result)) { return start_result; }

    if (vtype(start_result) == T_PROMISE) {
      ant_value_t resolve_fn = js_heavy_mkfun(js, ts_start_resolve, ts_obj);
      ant_value_t reject_fn = js_heavy_mkfun(js, ts_start_reject, ts_obj);
      js_promise_then(js, start_result, resolve_fn, reject_fn);
    }

    if (vtype(start_result) != T_PROMISE) {
      ant_value_t resolved = js_mkpromise(js);
      js_resolve_promise(js, resolved, js_mkundef());
      ant_value_t res_fn = js_heavy_mkfun(js, ts_start_resolve, ts_obj);
      ant_value_t rej_fn = js_heavy_mkfun(js, ts_start_reject, ts_obj);
      js_promise_then(js, resolved, res_fn, rej_fn);
    }
  } else {
    ant_value_t resolved = js_mkpromise(js);
    js_resolve_promise(js, resolved, js_mkundef());
    ant_value_t res_fn = js_heavy_mkfun(js, ts_start_resolve, ts_obj);
    ant_value_t rej_fn = js_heavy_mkfun(js, ts_start_reject, ts_obj);
    js_promise_then(js, resolved, res_fn, rej_fn);
  }

  return ts_obj;
}

static ant_value_t js_ts_ctrl_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "TransformStreamDefaultController cannot be constructed directly");
}

void init_transform_stream_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);

  js->sym.ts_ctrl_proto = js_mkobj(js);
  js_set_getter_desc(js, js->sym.ts_ctrl_proto, "desiredSize", 11, js_mkfun(js_ts_ctrl_get_desired_size), JS_DESC_C);
  js_set(js, js->sym.ts_ctrl_proto, "enqueue", js_mkfun(js_ts_ctrl_enqueue));
  js_set_descriptor(js, js->sym.ts_ctrl_proto, "enqueue", 7, JS_DESC_W | JS_DESC_C);
  js_set(js, js->sym.ts_ctrl_proto, "error", js_mkfun(js_ts_ctrl_error));
  js_set_descriptor(js, js->sym.ts_ctrl_proto, "error", 5, JS_DESC_W | JS_DESC_C);
  js_set(js, js->sym.ts_ctrl_proto, "terminate", js_mkfun(js_ts_ctrl_terminate));
  js_set_descriptor(js, js->sym.ts_ctrl_proto, "terminate", 9, JS_DESC_W | JS_DESC_C);
  js_set_sym(js, js->sym.ts_ctrl_proto, get_toStringTag_sym(), js_mkstr(js, "TransformStreamDefaultController", 32));

  ant_value_t ctrl_ctor = js_make_ctor(js, js_ts_ctrl_ctor, js->sym.ts_ctrl_proto, "TransformStreamDefaultController", 32);
  js_set(js, g, "TransformStreamDefaultController", ctrl_ctor);
  js_set_descriptor(js, g, "TransformStreamDefaultController", 32, JS_DESC_W | JS_DESC_C);

  js->sym.ts_proto = js_mkobj(js);
  js_set_getter_desc(js, js->sym.ts_proto, "readable", 8, js_mkfun(js_ts_get_readable), JS_DESC_C);
  js_set_getter_desc(js, js->sym.ts_proto, "writable", 8, js_mkfun(js_ts_get_writable), JS_DESC_C);
  js_set_sym(js, js->sym.ts_proto, get_toStringTag_sym(), js_mkstr(js, "TransformStream", 15));

  ant_value_t ts_ctor = js_make_ctor(js, js_ts_ctor, js->sym.ts_proto, "TransformStream", 15);
  js_set(js, g, "TransformStream", ts_ctor);
  js_set_descriptor(js, g, "TransformStream", 15, JS_DESC_W | JS_DESC_C);
}

void gc_mark_transform_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  mark(js, js->sym.ts_proto);
  mark(js, js->sym.ts_ctrl_proto);
}
