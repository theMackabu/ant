#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"

#include "gc/roots.h"
#include "silver/call.h"
#include "modules/assert.h"
#include "streams/transform.h"
#include "streams/readable.h"
#include "streams/writable.h"

static inline bool ts_has_stream_shape(ant_value_t obj) {
  return vtype(js_get_slot(obj, SLOT_DATA)) == kTypeNumber
    && rs_is_stream(js_get_slot(obj, SLOT_ENTRIES))
    && ws_is_stream(js_get_slot(obj, SLOT_CTOR));
}

static inline bool ts_has_controller_shape(ant_value_t obj) {
  ant_value_t ts_obj = js_get_slot(obj, SLOT_DATA);
  return is_object_type(ts_obj)
    && js_check_brand(ts_obj, BRAND_TRANSFORM_STREAM)
    && vtype(js_get_slot(ts_obj, SLOT_DATA)) == kTypeNumber
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
  return Ant_Value_AsNumberUnchecked(js_get_slot(ts_obj, SLOT_DATA)) != 0;
}

static inline void ts_set_bp_flag(ant_value_t ts_obj, bool bp) {
  js_set_slot(ts_obj, SLOT_DATA, Ant_Value_FromNumber(bp ? 1 : 0));
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
  return js_get_slot(ts_obj, SLOT_WS_ABORT) == Ant_Value_True;
}

static inline bool ts_is_flushing(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_WS_CLOSE) == Ant_Value_True;
}

static inline ant_value_t ts_cancel_settle_error(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_RS_SIZE);
}

static inline bool ts_cancel_joined_abort(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_WS_WRITE) == Ant_Value_True;
}

static inline bool ts_cancel_has_user_handler(ant_value_t ts_obj) {
  return js_get_slot(ts_obj, SLOT_WS_SIGNAL) == Ant_Value_True;
}

static inline void ts_set_flushing(ant_value_t ts_obj, bool flushing) {
  js_set_slot(ts_obj, SLOT_WS_CLOSE, flushing ? Ant_Value_True : Ant_Value_False);
}

static inline void ts_set_cancel_state(ant_t *js, ant_value_t ts_obj, ant_value_t promise, bool started_by_abort, bool has_user_handler) {
  js_set_slot_wb(js, ts_obj, SLOT_RS_CANCEL, promise);
  js_set_slot(ts_obj, SLOT_WS_ABORT, started_by_abort ? Ant_Value_True : Ant_Value_False);
  js_set_slot(ts_obj, SLOT_RS_SIZE, Ant_Value_Undefined);
  js_set_slot(ts_obj, SLOT_WS_WRITE, Ant_Value_False);
  js_set_slot(ts_obj, SLOT_WS_SIGNAL, has_user_handler ? Ant_Value_True : Ant_Value_False);
}

static inline void ts_clear_cancel_state(ant_value_t ts_obj, ant_value_t promise) {
if (ts_cancel_promise(ts_obj) == promise) {
  js_set_slot(ts_obj, SLOT_RS_CANCEL, Ant_Value_Undefined);
  js_set_slot(ts_obj, SLOT_WS_ABORT, Ant_Value_False);
  js_set_slot(ts_obj, SLOT_WS_SIGNAL, Ant_Value_False);
}}

static void ts_ctrl_clear_algorithms(ant_value_t ctrl_obj) {
  js_set_slot(ctrl_obj, SLOT_ENTRIES, Ant_Value_Undefined);
  js_set_slot(ctrl_obj, SLOT_CTOR, Ant_Value_Undefined);
  js_set_slot(ctrl_obj, SLOT_AUX, Ant_Value_Undefined);
}

static void ts_chain_promise(ant_t *js, ant_value_t val, ant_value_t res_fn, ant_value_t rej_fn) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, val);
  GC_ROOT_PIN(js, res_fn);
  GC_ROOT_PIN(js, rej_fn);

  ant_value_t promise = val;
  GC_ROOT_PIN(js, promise);
  if (vtype(promise) != kTypePromise) {
    promise = js_mkpromise(js);
    GC_ROOT_PIN(js, promise);
    js_resolve_promise(js, promise, val);
  }

  Ant_Promise_Observe(js, promise, res_fn, rej_fn);
  GC_ROOT_RESTORE(js, root_mark);
}

static void ts_error(ant_t *js, ant_value_t ts_obj, ant_value_t e) {
  ant_value_t readable = ts_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  rs_controller_t *rc = rs_get_controller(rs_ctrl);
  if (rc) {
    rc->queue_total_size = 0;
    rc->queue_sizes_len = 0;
    rc->queue_sizes_head = 0;
    rs_default_controller_clear_algorithms(rs_ctrl);
  }
  readable_stream_error(js, readable, e);

  if (ts_get_backpressure(ts_obj)) {
    ant_value_t bp = ts_bp_promise(ts_obj);
    if (vtype(bp) == kTypePromise) js_resolve_promise(js, bp, Ant_Value_Undefined);
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
    if (vtype(bp) == kTypePromise) js_resolve_promise(js, bp, Ant_Value_Undefined);
    ts_set_bp_flag(ts_obj, false);
  }
}

static void ts_set_backpressure(ant_t *js, ant_value_t ts_obj, bool backpressure) {
  if (ts_get_backpressure(ts_obj)) {
    ant_value_t bp = ts_bp_promise(ts_obj);
    if (vtype(bp) == kTypePromise) js_resolve_promise(js, bp, Ant_Value_Undefined);
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
    ant_value_t err = js_take_thrown(js, enqueue_result);
    ts_error_writable_and_unblock_write(js, ts_obj, err);
    return js_throw(js, err);
  }

  rs_controller_t *rc = rs_get_controller(rs_ctrl);
  bool bp = rc && ((rc->strategy_hwm - rc->queue_total_size) <= 0);
  if (bp != ts_get_backpressure(ts_obj)) ts_set_backpressure(js, ts_obj, bp);
    
  return Ant_Value_Undefined;
}

void ts_ctrl_error(ant_t *js, ant_value_t ctrl_obj, ant_value_t e) {
  ant_value_t ts_obj = ts_ctrl_stream(ctrl_obj);
  if (vtype(ts_cancel_promise(ts_obj)) == kTypePromise && ts_cancel_has_user_handler(ts_obj))
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
    ant_value_t err = Ant_Error_Create(js, JS_ERR_TYPE, "TransformStream readable side terminated");
    ts_error_writable_and_unblock_write(js, ts_obj, err);
  }
}

static ant_value_t ts_transform_resolve(ant_params_t) {
  ant_value_t p = js_get_slot(call_func, SLOT_DATA);
  js_resolve_promise(js, p, Ant_Value_Undefined);
  return Ant_Value_Undefined;
}

static ant_value_t ts_transform_reject(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t e = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ts_error(js, ts_obj, e);
  js_reject_promise(js, p, e);
  return Ant_Value_Undefined;
}

static ant_value_t ts_ctrl_perform_transform(ant_t *js, ant_value_t ctrl_obj, ant_value_t chunk) {
  ant_value_t transform_fn = ts_ctrl_transform_fn(ctrl_obj);
  ant_value_t ts_obj = ts_ctrl_stream(ctrl_obj);
  ant_value_t p = js_mkpromise(js);
  promise_mark_handled(p);

  if (is_callable(transform_fn)) {
    ant_value_t call_args[2] = { chunk, ctrl_obj };
    ant_value_t result = sv_vm_call(js->vm, js, transform_fn, ts_ctrl_transformer(ctrl_obj), call_args, 2, NULL, Ant_Value_Undefined);

    if (is_err(result)) {
      ant_value_t err = js_take_thrown(js, result);
      ts_error(js, ts_obj, err);
      js_reject_promise(js, p, err);
      return p;
    }

    ant_value_t res_fn = js_heavy_mkfun(js, ts_transform_resolve, p);
    ant_value_t wrapper = Ant_Object_CreateBare(js);
    js_set_slot_wb(js, wrapper, SLOT_DATA, p);
    js_set_slot_wb(js, wrapper, SLOT_ENTRIES, ts_obj);
    
    ant_value_t rej_fn = js_heavy_mkfun(js, ts_transform_reject, wrapper);
    ts_chain_promise(js, result, res_fn, rej_fn);
  } else {
    ant_value_t enqueue_result = ts_ctrl_enqueue(js, ctrl_obj, chunk);
    if (is_err(enqueue_result)) {
      ant_value_t err = js_take_thrown(js, enqueue_result);
      js_reject_promise(js, p, err);
      return p;
    }
    js_resolve_promise(js, p, Ant_Value_Undefined);
  }

  return p;
}

static ant_value_t ts_cancel_base_resolve(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ts_clear_cancel_state(ts_obj, p);
  js_resolve_promise(js, p, Ant_Value_Undefined);
  return Ant_Value_Undefined;
}

static ant_value_t ts_cancel_base_reject(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t err = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ts_clear_cancel_state(ts_obj, p);
  js_reject_promise(js, p, err);
  return Ant_Value_Undefined;
}

static ant_value_t ts_run_cancel_algorithm(ant_t *js, ant_value_t ts_obj, ant_value_t cancel_fn, ant_value_t reason, bool started_by_abort) {
  ant_value_t existing = ts_cancel_promise(ts_obj);
  if (vtype(existing) == kTypePromise) return existing;

  ant_value_t p = js_mkpromise(js);
  promise_mark_handled(p);
  ts_set_cancel_state(js, ts_obj, p, started_by_abort, is_callable(cancel_fn));
  ts_ctrl_clear_algorithms(ts_controller(ts_obj));

  ant_value_t result = Ant_Value_Undefined;
  if (is_callable(cancel_fn)) {
    ant_value_t cancel_args[1] = { reason };
    result = sv_vm_call(js->vm, js, cancel_fn, ts_ctrl_transformer(ts_controller(ts_obj)), cancel_args, 1, NULL, Ant_Value_Undefined);
  }

  if (is_err(result)) {
    ant_value_t err = js_take_thrown(js, result);
    ts_clear_cancel_state(ts_obj, p);
    js_reject_promise(js, p, err);
    return p;
  }

  ant_value_t wrapper = Ant_Object_CreateBare(js);
  js_set_slot_wb(js, wrapper, SLOT_DATA, p);
  js_set_slot_wb(js, wrapper, SLOT_ENTRIES, ts_obj);
  
  ant_value_t res_fn = js_heavy_mkfun(js, ts_cancel_base_resolve, wrapper);
  ant_value_t rej_fn = js_heavy_mkfun(js, ts_cancel_base_reject, wrapper);
  ts_chain_promise(js, result, res_fn, rej_fn);

  return p;
}

static ant_value_t ts_source_cancel_resolve(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t reason = js_get_slot(wrapper, SLOT_AUX);

  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  ant_value_t settle_error = ts_cancel_settle_error(ts_obj);
  if (is_object_type(settle_error)) {
    js_reject_promise(js, p, settle_error);
    return Ant_Value_Undefined;
  }
  if (rs && rs->state == RS_STATE_ERRORED) {
    js_reject_promise(js, p, rs_stream_error(readable));
    return Ant_Value_Undefined;
  }
  ant_value_t writable = ts_writable(ts_obj);
  ws_stream_t *ws = ws_get_stream(writable);

  if (ws && ws->state == WS_STATE_WRITABLE)
    ts_error_writable_and_unblock_write(js, ts_obj, reason);

  js_resolve_promise(js, p, Ant_Value_Undefined);
  return Ant_Value_Undefined;
}

static ant_value_t ts_source_cancel_reject(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t err = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ts_error_writable_and_unblock_write(js, ts_obj, err);
  js_reject_promise(js, p, err);
  return Ant_Value_Undefined;
}

static ant_value_t ts_abort_cancel_resolve(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t reason = js_get_slot(wrapper, SLOT_AUX);

  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  
  if (rs && rs->state == RS_STATE_ERRORED) {
    js_reject_promise(js, p, rs_stream_error(readable));
    return Ant_Value_Undefined;
  }

  if (ts_cancel_joined_abort(ts_obj)) js_set_slot_wb(js, ts_obj, SLOT_RS_SIZE, reason);
  ts_error(js, ts_obj, reason);
  
  if (ts_cancel_joined_abort(ts_obj)) js_reject_promise(js, p, reason);
  else js_resolve_promise(js, p, Ant_Value_Undefined);
  
  return Ant_Value_Undefined;
}

static ant_value_t ts_abort_cancel_reject(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t err = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ts_error(js, ts_obj, err);
  js_reject_promise(js, p, err);
  return Ant_Value_Undefined;
}

static ant_value_t ts_close_cancel_resolve(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  ant_value_t settle_error = ts_cancel_settle_error(ts_obj);
  if (is_object_type(settle_error)) {
    js_reject_promise(js, p, settle_error);
    return Ant_Value_Undefined;
  }
  if (rs && rs->state == RS_STATE_ERRORED) {
    js_reject_promise(js, p, rs_stream_error(readable));
    return Ant_Value_Undefined;
  }
  js_resolve_promise(js, p, Ant_Value_Undefined);
  return Ant_Value_Undefined;
}

static ant_value_t ts_close_cancel_reject(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t err = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  js_reject_promise(js, p, err);
  return Ant_Value_Undefined;
}

static ant_value_t ts_sink_write_bp_resolve(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t ctrl_obj = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t chunk = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_CTOR);
  ws_stream_t *ws = ws_get_stream(ts_writable(ts_obj));

  if (ws && ws->state == WS_STATE_ERRORING) {
    ant_value_t err = ts_writable_stored_error(ts_obj);
    if (!is_object_type(err))
      err = Ant_Error_Create(js, JS_ERR_TYPE, "WritableStream is in erroring state");
    ant_value_t fp = ts_ctrl_finish_promise(ctrl_obj);
    if (vtype(fp) == kTypePromise) js_reject_promise(js, fp, err);
    return Ant_Value_Undefined;
  }

  ant_value_t transform_p = ts_ctrl_perform_transform(js, ctrl_obj, chunk);
  ant_value_t fp = ts_ctrl_finish_promise(ctrl_obj);
  if (vtype(fp) == kTypePromise) {
    ant_value_t resolve_fn = js_heavy_mkfun(js, ts_transform_resolve, fp);
    ant_value_t rej_wrapper = Ant_Object_CreateBare(js);
    js_set_slot_wb(js, rej_wrapper, SLOT_DATA, fp);
    js_set_slot_wb(js, rej_wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t reject_fn = js_heavy_mkfun(js, ts_transform_reject, rej_wrapper);
    ts_chain_promise(js, transform_p, resolve_fn, reject_fn);
  }
  return Ant_Value_Undefined;
}

static ant_value_t ts_sink_write(ant_params_t) {
  ant_value_t ts_obj = js_get_slot(call_func, SLOT_DATA);
  ant_value_t chunk = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ant_value_t ctrl_obj = ts_controller(ts_obj);

  ant_value_t finish_p = js_mkpromise(js);
  promise_mark_handled(finish_p);
  js_set_slot_wb(js, ctrl_obj, SLOT_RS_PULL, finish_p);

  if (ts_get_backpressure(ts_obj)) {
    ant_value_t wrapper = Ant_Object_CreateBare(js);
    js_set_slot_wb(js, wrapper, SLOT_DATA, ctrl_obj);
    js_set_slot_wb(js, wrapper, SLOT_ENTRIES, chunk);
    js_set_slot_wb(js, wrapper, SLOT_CTOR, ts_obj);
    ant_value_t res_fn = js_heavy_mkfun(js, ts_sink_write_bp_resolve, wrapper);
    ant_value_t rej_wrapper = Ant_Object_CreateBare(js);
    js_set_slot_wb(js, rej_wrapper, SLOT_DATA, finish_p);
    js_set_slot_wb(js, rej_wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t rej_fn = js_heavy_mkfun(js, ts_transform_reject, rej_wrapper);
    ant_value_t bp = ts_bp_promise(ts_obj);
    ts_chain_promise(js, bp, res_fn, rej_fn);
  } else {
    ant_value_t transform_p = ts_ctrl_perform_transform(js, ctrl_obj, chunk);
    ant_value_t resolve_fn = js_heavy_mkfun(js, ts_transform_resolve, finish_p);
    ant_value_t rej_wrapper = Ant_Object_CreateBare(js);
    js_set_slot_wb(js, rej_wrapper, SLOT_DATA, finish_p);
    js_set_slot_wb(js, rej_wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t reject_fn = js_heavy_mkfun(js, ts_transform_reject, rej_wrapper);
    ts_chain_promise(js, transform_p, resolve_fn, reject_fn);
  }

  return finish_p;
}

static ant_value_t ts_sink_abort(ant_params_t) {
  ant_value_t ts_obj = js_get_slot(call_func, SLOT_DATA);
  ant_value_t ctrl_obj = ts_controller(ts_obj);
  ant_value_t cancel_fn = ts_ctrl_cancel_fn(ctrl_obj);
  ant_value_t reason = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  
  bool joined_source_cancel = vtype(ts_cancel_promise(ts_obj)) == kTypePromise && !ts_cancel_started_by_abort(ts_obj);
  if (joined_source_cancel && ts_cancel_has_user_handler(ts_obj))
    js_set_slot(ts_obj, SLOT_WS_WRITE, Ant_Value_True);

  ant_value_t p = js_mkpromise(js);
  ant_value_t base = ts_run_cancel_algorithm(js, ts_obj, cancel_fn, reason, true);
  ant_value_t wrapper = Ant_Object_CreateBare(js);
  js_set_slot_wb(js, wrapper, SLOT_DATA, p);
  js_set_slot_wb(js, wrapper, SLOT_ENTRIES, ts_obj);
  js_set_slot_wb(js, wrapper, SLOT_AUX, reason);
  js_set_slot(wrapper, SLOT_CTOR, ts_cancel_started_by_abort(ts_obj) ? Ant_Value_True : Ant_Value_False);
  
  ant_value_t res_fn = js_heavy_mkfun(js, ts_abort_cancel_resolve, wrapper);
  ant_value_t rej_fn = js_heavy_mkfun(js, ts_abort_cancel_reject, wrapper);
  ts_chain_promise(js, base, res_fn, rej_fn);
  
  return p;
}

static ant_value_t ts_sink_close_resolve(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);

  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  ts_set_flushing(ts_obj, false);

  if (rs && rs->state == RS_STATE_READABLE) {
    rs_controller_close(js, rs_ctrl);
    js_resolve_promise(js, p, Ant_Value_Undefined);
  } else if (rs && rs->state == RS_STATE_CLOSED) {
    js_resolve_promise(js, p, Ant_Value_Undefined);
  } else if (rs && rs->state == RS_STATE_ERRORED) {
    js_reject_promise(js, p, rs_stream_error(readable));
  } else {
    js_reject_promise(js, p, Ant_Error_Create(js, JS_ERR_TYPE, "TransformStream readable side is not in a readable state"));
  }

  return Ant_Value_Undefined;
}

static ant_value_t ts_sink_close_reject(ant_params_t) {
  ant_value_t wrapper = js_get_slot(call_func, SLOT_DATA);
  ant_value_t p = js_get_slot(wrapper, SLOT_DATA);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);
  ant_value_t e = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ts_set_flushing(ts_obj, false);
  ts_error(js, ts_obj, e);
  js_reject_promise(js, p, e);
  return Ant_Value_Undefined;
}

static ant_value_t ts_sink_close(ant_params_t) {
  ant_value_t ts_obj = js_get_slot(call_func, SLOT_DATA);
  ant_value_t ctrl_obj = ts_controller(ts_obj);
  ant_value_t readable = ts_readable(ts_obj);

  ant_value_t flush_fn = ts_ctrl_flush_fn(ctrl_obj);
  ant_value_t p = js_mkpromise(js);
  promise_mark_handled(p);

  ant_value_t cancel_p = ts_cancel_promise(ts_obj);
  if (vtype(cancel_p) == kTypePromise) {
    ant_value_t wrapper = Ant_Object_CreateBare(js);
    js_set_slot_wb(js, wrapper, SLOT_DATA, p);
    js_set_slot_wb(js, wrapper, SLOT_ENTRIES, ts_obj);
    ant_value_t res_fn = js_heavy_mkfun(js, ts_close_cancel_resolve, wrapper);
    ant_value_t rej_fn = js_heavy_mkfun(js, ts_close_cancel_reject, wrapper);
    ts_chain_promise(js, cancel_p, res_fn, rej_fn);
    return p;
  }

  ts_ctrl_clear_algorithms(ctrl_obj);
  ts_set_flushing(ts_obj, true);

  if (is_callable(flush_fn)) {
    ant_value_t flush_args[1] = { ctrl_obj };
    ant_value_t result = sv_vm_call(js->vm, js, flush_fn, ts_ctrl_transformer(ctrl_obj), flush_args, 1, NULL, Ant_Value_Undefined);

    if (is_err(result)) {
      ant_value_t err = js_take_thrown(js, result);
      ts_error(js, ts_obj, err);
      ts_set_flushing(ts_obj, false);
      js_reject_promise(js, p, err);
      return p;
    }

    ant_value_t wrapper = Ant_Object_CreateBare(js);
    js_set_slot_wb(js, wrapper, SLOT_DATA, p);
    js_set_slot_wb(js, wrapper, SLOT_ENTRIES, ts_obj);
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
    js_resolve_promise(js, p, Ant_Value_Undefined);
  }

  return p;
}

static ant_value_t ts_source_pull(ant_params_t) {
  ant_value_t ts_obj = js_get_slot(call_func, SLOT_DATA);

  if (ts_get_backpressure(ts_obj)) {
    ts_set_backpressure(js, ts_obj, false);
    ant_value_t p = js_mkpromise(js);
    js_resolve_promise(js, p, Ant_Value_Undefined);
    return p;
  }

  ant_value_t p = js_mkpromise(js);
  js_resolve_promise(js, p, Ant_Value_Undefined);
  return p;
}

static ant_value_t ts_source_cancel(ant_params_t) {
  ant_value_t ts_obj = js_get_slot(call_func, SLOT_DATA);
  ant_value_t ctrl_obj = ts_controller(ts_obj);
  ant_value_t cancel_fn = ts_ctrl_cancel_fn(ctrl_obj);
  ant_value_t reason = (nargs > 0) ? args[0] : Ant_Value_Undefined;

  if (ts_is_flushing(ts_obj)) {
    ant_value_t p = js_mkpromise(js);
    js_resolve_promise(js, p, Ant_Value_Undefined);
    return p;
  }

  ant_value_t p = js_mkpromise(js);
  ant_value_t base = ts_run_cancel_algorithm(js, ts_obj, cancel_fn, reason, false);
  ant_value_t wrapper = Ant_Object_CreateBare(js);
  
  js_set_slot_wb(js, wrapper, SLOT_DATA, p);
  js_set_slot_wb(js, wrapper, SLOT_ENTRIES, ts_obj);
  js_set_slot_wb(js, wrapper, SLOT_AUX, reason);
  js_set_slot(wrapper, SLOT_CTOR, ts_cancel_started_by_abort(ts_obj) ? Ant_Value_True : Ant_Value_False);
  
  ant_value_t res_fn = js_heavy_mkfun(js, ts_source_cancel_resolve, wrapper);
  ant_value_t rej_fn = js_heavy_mkfun(js, ts_source_cancel_reject, wrapper);
  ts_chain_promise(js, base, res_fn, rej_fn);
  
  return p;
}

static ant_value_t js_ts_ctrl_get_desired_size(ant_params_t) {
  ant_value_t ts_obj = ts_ctrl_stream(call_this);
  ant_value_t readable = ts_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  rs_controller_t *rc = rs_get_controller(rs_ctrl);
  rs_stream_t *rs = rs_get_stream(readable);
  if (!rc || !rs) return Ant_Value_Null;
  if (rs->state == RS_STATE_ERRORED) return Ant_Value_Null;
  if (rs->state == RS_STATE_CLOSED) return Ant_Value_FromNumber(0);
  return Ant_Value_FromNumber(rc->strategy_hwm - rc->queue_total_size);
}

static ant_value_t js_ts_ctrl_enqueue(ant_params_t) {
  ant_value_t ts_obj = ts_ctrl_stream(call_this);
  ant_value_t readable = ts_readable(ts_obj);
  rs_stream_t *rs = rs_get_stream(readable);
  if (!rs || rs->state != RS_STATE_READABLE)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Readable side is not in a readable state");

  ant_value_t chunk = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  return ts_ctrl_enqueue(js, call_this, chunk);
}

static ant_value_t js_ts_ctrl_error(ant_params_t) {
  ant_value_t e = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  ts_ctrl_error(js, call_this, e);
  return Ant_Value_Undefined;
}

static ant_value_t js_ts_ctrl_terminate(ant_params_t) {
  ts_ctrl_terminate(js, call_this);
  return Ant_Value_Undefined;
}

static ant_value_t js_ts_get_readable(ant_params_t) {
  if (!ts_is_stream(call_this)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStream");
  return ts_readable(call_this);
}

static ant_value_t js_ts_get_writable(ant_params_t) {
  if (!ts_is_stream(call_this)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStream");
  return ts_writable(call_this);
}

static ant_value_t ts_start_resolve(ant_params_t) {
  ant_value_t ts_obj = js_get_slot(call_func, SLOT_DATA);
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

  return Ant_Value_Undefined;
}

static ant_value_t ts_start_reject(ant_params_t) {
  ant_value_t ts_obj = js_get_slot(call_func, SLOT_DATA);
  ant_value_t e = (nargs > 0) ? args[0] : Ant_Value_Undefined;

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

  return Ant_Value_Undefined;
}

typedef struct {
  ant_value_t transform;
  ant_value_t flush;
  ant_value_t cancel;
  ant_value_t start;
} ts_transformer_t;

static ant_value_t ts_read_callable(ant_t *js, ant_value_t obj, const char *name, ant_value_t *out) {
  ant_value_t fn = js_getprop_fallback(js, obj, name);
  if (is_err(fn) || is_undefined(fn)) return fn;
  if (!is_callable(fn)) return js_mkerr_typed(js, JS_ERR_TYPE, "%s must be a function", name);
  *out = fn;
  return Ant_Value_Undefined;
}

static ant_value_t ts_check_transformer_types(ant_t *js, ant_value_t transformer) {
  static const char *const keys[] = { "readableType", "writableType" };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    ant_value_t type = js_getprop_fallback(js, transformer, keys[i]);
    if (is_err(type)) return type;
    if (!is_undefined(type)) return js_mkerr_typed(js, JS_ERR_RANGE, "%s is not supported", keys[i]);
  }
  return Ant_Value_Undefined;
}

static ant_value_t ts_read_strategy(
  ant_t *js, ant_value_t strategy, double *hwm, ant_value_t *size_fn
) {
  if (!is_object_type(strategy)) return Ant_Value_Undefined;

  ant_value_t hwm_val = js_getprop_fallback(js, strategy, "highWaterMark");
  if (is_err(hwm_val)) return hwm_val;
  if (!is_undefined(hwm_val)) {
    *hwm = js_to_number(js, hwm_val);
    if (*hwm != *hwm || *hwm < 0) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid highWaterMark");
  }

  return ts_read_callable(js, strategy, "size", size_fn);
}

static ant_value_t ts_read_transformer(ant_t *js, ant_value_t transformer, ts_transformer_t *out) {
  if (!is_object_type(transformer)) return Ant_Value_Undefined;

  ant_value_t err = ts_read_callable(js, transformer, "transform", &out->transform);
  if (!is_err(err)) err = ts_read_callable(js, transformer, "flush", &out->flush);
  if (!is_err(err)) err = ts_read_callable(js, transformer, "cancel", &out->cancel);
  if (!is_err(err)) err = ts_read_callable(js, transformer, "start", &out->start);
  
  return err;
}

static ant_value_t ts_create_controller(
  ant_t *js, ant_value_t ts_obj, ant_value_t transformer, const ts_transformer_t *fns
) {
  ant_value_t ctrl_obj = Ant_Object_CreateBare(js);
  js_set_proto_init(ctrl_obj, js->builtins.ts_ctrl_proto);
  js_set_slot(ctrl_obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_TRANSFORM_STREAM_CONTROLLER));\
  
  js_set_slot_wb(js, ctrl_obj, SLOT_DATA, ts_obj);
  js_set_slot_wb(js, ctrl_obj, SLOT_ENTRIES, fns->transform);
  js_set_slot_wb(js, ctrl_obj, SLOT_CTOR, fns->flush);
  js_set_slot_wb(js, ctrl_obj, SLOT_AUX, fns->cancel);
  js_set_slot_wb(js, ctrl_obj, SLOT_SETTLED, transformer);
  js_set_slot(ctrl_obj, SLOT_RS_PULL, Ant_Value_Undefined);
  
  return ctrl_obj;
}

static ant_value_t ts_create_readable(ant_t *js, ant_value_t ts_obj, double hwm, ant_value_t size_fn) {
  rs_stream_t *rst = calloc(1, sizeof(rs_stream_t));
  rs_controller_t *rcc = calloc(1, sizeof(rs_controller_t));
  
  if (!rst || !rcc) {
    free(rst);
    free(rcc);
    return Ant_Error_CreateOutOfMemory(js);
  }
  
  rst->state = RS_STATE_READABLE;
  rcc->strategy_hwm = hwm;

  ant_value_t rs_obj = Ant_Object_CreateBare(js);
  js_set_proto_init(rs_obj, js->builtins.rs_proto);
  js_set_slot(rs_obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_READABLE_STREAM));
  js_set_native(rs_obj, rst, RS_STREAM_NATIVE_TAG);
  js_set_finalizer(rs_obj, ts_rs_finalize);

  ant_value_t rs_ctrl_obj = Ant_Object_CreateBare(js);
  js_set_proto_init(rs_ctrl_obj, js->builtins.controller_proto);
  js_set_slot(rs_ctrl_obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_READABLE_STREAM_CONTROLLER));
  js_set_native(rs_ctrl_obj, rcc, RS_CONTROLLER_NATIVE_TAG);
  js_set_finalizer(rs_ctrl_obj, ts_rs_ctrl_finalize);
  
  js_set_slot_wb(js, rs_ctrl_obj, SLOT_ENTRIES, rs_obj);
  js_set_slot_wb(js, rs_ctrl_obj, SLOT_RS_PULL, js_heavy_mkfun(js, ts_source_pull, ts_obj));
  js_set_slot_wb(js, rs_ctrl_obj, SLOT_RS_CANCEL, js_heavy_mkfun(js, ts_source_cancel, ts_obj));
  js_set_slot_wb(js, rs_ctrl_obj, SLOT_RS_SIZE, size_fn);
  js_set_slot_wb(js, rs_ctrl_obj, SLOT_AUX, Ant_Array_Create(js));
  js_set_slot_wb(js, rs_obj, SLOT_ENTRIES, rs_ctrl_obj);
  
  return rs_obj;
}

static ant_value_t ts_create_writable(ant_t *js, ant_value_t ts_obj, double hwm, ant_value_t size_fn) {
  ws_stream_t *wst = calloc(1, sizeof(ws_stream_t));
  ws_controller_t *wc = calloc(1, sizeof(ws_controller_t));
  
  if (!wst || !wc) {
    free(wst);
    free(wc);
    return Ant_Error_CreateOutOfMemory(js);
  }
  
  wst->state = WS_STATE_WRITABLE;
  wc->strategy_hwm = hwm;

  ant_value_t ws_obj = Ant_Object_CreateBare(js);
  js_set_proto_init(ws_obj, js->builtins.ws_proto);
  js_set_slot(ws_obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_WRITABLE_STREAM));
  js_set_native(ws_obj, wst, WS_STREAM_NATIVE_TAG);
  js_set_finalizer(ws_obj, ts_ws_finalize);
  js_set_slot_wb(js, ws_obj, SLOT_SETTLED, Ant_Array_Create(js));

  ant_value_t ws_ctrl_obj = Ant_Object_CreateBare(js);
  js_set_proto_init(ws_ctrl_obj, js->builtins.ws_controller_proto);
  js_set_slot(ws_ctrl_obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_WRITABLE_STREAM_CONTROLLER));
  js_set_native(ws_ctrl_obj, wc, WS_CONTROLLER_NATIVE_TAG);
  js_set_finalizer(ws_ctrl_obj, ts_ws_ctrl_finalize);
  js_set_slot_wb(js, ws_ctrl_obj, SLOT_ENTRIES, ws_obj);
  js_set_slot_wb(js, ws_ctrl_obj, SLOT_WS_WRITE, js_heavy_mkfun(js, ts_sink_write, ts_obj));
  js_set_slot_wb(js, ws_ctrl_obj, SLOT_WS_CLOSE, js_heavy_mkfun(js, ts_sink_close, ts_obj));
  js_set_slot_wb(js, ws_ctrl_obj, SLOT_WS_ABORT, js_heavy_mkfun(js, ts_sink_abort, ts_obj));
  js_set_slot_wb(js, ws_ctrl_obj, SLOT_RS_SIZE, size_fn);
  js_set_slot(ws_ctrl_obj, SLOT_CTOR, Ant_Value_Undefined);
  js_set_slot_wb(js, ws_ctrl_obj, SLOT_AUX, Ant_Array_Create(js));
  js_set_slot_wb(js, ws_obj, SLOT_ENTRIES, ws_ctrl_obj);
  
  return ws_obj;
}

static ant_value_t ts_run_start(
  ant_t *js, ant_value_t ts_obj, ant_value_t ctrl_obj,
  ant_value_t transformer, ant_value_t start_fn
) {
  ant_value_t start_result = Ant_Value_Undefined;
  if (is_callable(start_fn)) {
    ant_value_t start_args[1] = { ctrl_obj };
    start_result = sv_vm_call(js->vm, js, start_fn, transformer, start_args, 1, NULL, Ant_Value_Undefined);
    if (is_err(start_result)) return start_result;
  }

  ant_value_t res_fn = js_heavy_mkfun(js, ts_start_resolve, ts_obj);
  ant_value_t rej_fn = js_heavy_mkfun(js, ts_start_reject, ts_obj);
  ts_chain_promise(js, start_result, res_fn, rej_fn);
  
  return ts_obj;
}

ant_value_t ts_create(
  ant_t *js, ant_value_t transformer, ant_value_t writable_strategy, 
  ant_value_t readable_strategy, ant_value_t new_target
) {
  ant_value_t err = is_object_type(transformer) ? ts_check_transformer_types(js, transformer) : Ant_Value_Undefined;
  if (is_err(err)) return err;

  double writable_hwm = 1;
  ant_value_t writable_size_fn = Ant_Value_Undefined;
  err = ts_read_strategy(js, writable_strategy, &writable_hwm, &writable_size_fn);
  if (is_err(err)) return err;

  double readable_hwm = 0;
  ant_value_t readable_size_fn = Ant_Value_Undefined;
  err = ts_read_strategy(js, readable_strategy, &readable_hwm, &readable_size_fn);
  if (is_err(err)) return err;

  ts_transformer_t fns = { Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined };
  err = ts_read_transformer(js, transformer, &fns);
  if (is_err(err)) return err;

  ant_value_t ts_obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.ts_proto, new_target);
  if (is_object_type(proto)) js_set_proto_init(ts_obj, proto);
  js_set_slot(ts_obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_TRANSFORM_STREAM));
  js_set_slot(ts_obj, SLOT_DATA, Ant_Value_FromNumber(0));
  js_set_slot(ts_obj, SLOT_RS_CANCEL, Ant_Value_Undefined);
  js_set_slot(ts_obj, SLOT_WS_ABORT, Ant_Value_False);
  js_set_slot(ts_obj, SLOT_WS_CLOSE, Ant_Value_False);
  js_set_slot(ts_obj, SLOT_RS_SIZE, Ant_Value_Undefined);
  js_set_slot(ts_obj, SLOT_WS_WRITE, Ant_Value_False);
  js_set_slot(ts_obj, SLOT_WS_SIGNAL, Ant_Value_False);

  ant_value_t ctrl_obj = ts_create_controller(js, ts_obj, transformer, &fns);
  js_set_slot_wb(js, ts_obj, SLOT_DEFAULT, ctrl_obj);

  ant_value_t rs_obj = ts_create_readable(js, ts_obj, readable_hwm, readable_size_fn);
  if (is_err(rs_obj)) return rs_obj;
  js_set_slot_wb(js, ts_obj, SLOT_ENTRIES, rs_obj);

  ant_value_t ws_obj = ts_create_writable(js, ts_obj, writable_hwm, writable_size_fn);
  if (is_err(ws_obj)) return ws_obj;
  js_set_slot_wb(js, ts_obj, SLOT_CTOR, ws_obj);

  ts_set_backpressure(js, ts_obj, true);
  return ts_run_start(js, ts_obj, ctrl_obj, transformer, fns.start);
}

ant_value_t js_ts_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TransformStream constructor requires 'new'");

  ant_value_t transformer = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t writable_strategy = nargs > 1 && !is_null(args[1]) ? args[1] : Ant_Value_Undefined;
  ant_value_t readable_strategy = nargs > 2 && !is_null(args[2]) ? args[2] : Ant_Value_Undefined;
  return ts_create(js, transformer, writable_strategy, readable_strategy, call_new_target);
}

static ant_value_t js_ts_ctrl_ctor(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "TransformStreamDefaultController cannot be constructed directly");
}

void init_transform_stream_module(ant_t *js) {
  ant_value_t g = js_glob(js);

  js->builtins.ts_ctrl_proto = Ant_Object_CreateBare(js);
  js_set_getter_desc(js, js->builtins.ts_ctrl_proto, "desiredSize", 11, Ant_Function_Create(js_ts_ctrl_get_desired_size), JS_DESC_C);
  js_set(js, js->builtins.ts_ctrl_proto, "enqueue", Ant_Function_Create(js_ts_ctrl_enqueue));
  js_set_descriptor(js, js->builtins.ts_ctrl_proto, "enqueue", 7, JS_DESC_W | JS_DESC_C);
  js_set(js, js->builtins.ts_ctrl_proto, "error", Ant_Function_Create(js_ts_ctrl_error));
  js_set_descriptor(js, js->builtins.ts_ctrl_proto, "error", 5, JS_DESC_W | JS_DESC_C);
  js_set(js, js->builtins.ts_ctrl_proto, "terminate", Ant_Function_Create(js_ts_ctrl_terminate));
  js_set_descriptor(js, js->builtins.ts_ctrl_proto, "terminate", 9, JS_DESC_W | JS_DESC_C);
  js_set_tostringtag(js, js->builtins.ts_ctrl_proto, js_mkstr(js, "TransformStreamDefaultController", 32));

  ant_value_t ctrl_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_ts_ctrl_ctor, js->builtins.ts_ctrl_proto, "TransformStreamDefaultController", 0);
  js_set(js, g, "TransformStreamDefaultController", ctrl_ctor);
  js_set_descriptor(js, g, "TransformStreamDefaultController", 32, JS_DESC_W | JS_DESC_C);

  js->builtins.ts_proto = Ant_Object_CreateBare(js);
  js_set_getter_desc(js, js->builtins.ts_proto, "readable", 8, Ant_Function_Create(js_ts_get_readable), JS_DESC_C);
  js_set_getter_desc(js, js->builtins.ts_proto, "writable", 8, Ant_Function_Create(js_ts_get_writable), JS_DESC_C);
  js_set_tostringtag(js, js->builtins.ts_proto, js_mkstr(js, "TransformStream", 15));

  ant_value_t ts_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_ts_ctor, js->builtins.ts_proto, "TransformStream", 0);
  js_set(js, g, "TransformStream", ts_ctor);
  js_set_descriptor(js, g, "TransformStream", 15, JS_DESC_W | JS_DESC_C);
}
