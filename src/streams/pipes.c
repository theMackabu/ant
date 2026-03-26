#include <string.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/assert.h"
#include "modules/abort.h"
#include "streams/pipes.h"
#include "streams/readable.h"
#include "streams/writable.h"

static void pipes_chain_thenable(
  ant_t *js, ant_value_t value,
  ant_value_t on_resolve, ant_value_t on_reject
) {
  ant_value_t thenable = value;
  if (vtype(thenable) != T_PROMISE) {
    thenable = js_mkpromise(js);
    js_resolve_promise(js, thenable, value);
  }

  ant_value_t then_fn = js_get(js, thenable, "then");
  if (!is_callable(then_fn)) return;

  ant_value_t then_args[2] = { on_resolve, on_reject };
  sv_vm_call(js->vm, js, then_fn, thenable, then_args, 2, NULL, false);
}

typedef struct {
  bool settled;
  bool shutting_down;
  bool in_flight;
  bool prevent_close;
  bool prevent_abort;
  bool prevent_cancel;
} pipe_state_t;

static void pipe_state_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    free((pipe_state_t *)(uintptr_t)(size_t)js_getnum(entries[i].value));
    return;
  }}
}

static pipe_state_t *pipe_get_state(ant_value_t state) {
  ant_value_t s = js_get_slot(state, SLOT_DATA);
  if (vtype(s) != T_NUM) return NULL;
  return (pipe_state_t *)(uintptr_t)(size_t)js_getnum(s);
}

static ant_value_t pipe_state_source(ant_value_t state) {
  return js_get_slot(state, SLOT_ENTRIES);
}

static ant_value_t pipe_state_dest(ant_value_t state) {
  return js_get_slot(state, SLOT_CTOR);
}

static ant_value_t pipe_state_reader(ant_value_t state) {
  return js_get_slot(state, SLOT_BUFFER);
}

static ant_value_t pipe_state_writer(ant_value_t state) {
  return js_get_slot(state, SLOT_DEFAULT);
}

static ant_value_t pipe_state_promise(ant_value_t state) {
  return js_get_slot(state, SLOT_RS_PULL);
}

static void pipes_release_reader(ant_t *js, ant_value_t reader_obj) {
  ant_value_t stream_obj = rs_reader_stream(reader_obj);
  if (!is_object_type(stream_obj)) return;

  if (rs_reader_has_reqs(js, reader_obj)) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Reader was released");
    rs_default_reader_error_read_requests(js, reader_obj, js->thrown_value);
  }

  ant_value_t old_closed = rs_reader_closed(reader_obj);
  ant_value_t new_closed = js_mkpromise(js);
  js_mkerr_typed(js, JS_ERR_TYPE, "Reader was released");
  ant_value_t release_err = js->thrown_value;
  rs_stream_t *rs = rs_get_stream(stream_obj);
  
  if (rs && rs->state == RS_STATE_READABLE) {
    js_reject_promise(js, old_closed, release_err);
    promise_mark_handled(old_closed);
  }
  
  js_reject_promise(js, new_closed, release_err);
  promise_mark_handled(new_closed);
  js_set_slot(reader_obj, SLOT_RS_CLOSED, new_closed);
  js_set_slot(stream_obj, SLOT_CTOR, js_mkundef());
  js_set_slot(reader_obj, SLOT_ENTRIES, js_mkundef());
}

static void pipes_release_writer(ant_t *js, ant_value_t writer_obj) {
  ant_value_t ws_obj = js_get_slot(writer_obj, SLOT_ENTRIES);
  if (!is_object_type(ws_obj)) return;

  js_mkerr_typed(js, JS_ERR_TYPE, "Writer was released");
  ant_value_t rel_err = js->thrown_value;
  ant_value_t ready = js_mkpromise(js);
  js_reject_promise(js, ready, rel_err);
  promise_mark_handled(ready);
  js_set_slot(writer_obj, SLOT_WS_READY, ready);
  
  ant_value_t closed = js_mkpromise(js);
  js_reject_promise(js, closed, rel_err);
  promise_mark_handled(closed);
  js_set_slot(writer_obj, SLOT_RS_CLOSED, closed);
  js_set_slot(ws_obj, SLOT_CTOR, js_mkundef());
  js_set_slot(writer_obj, SLOT_ENTRIES, js_mkundef());
}

static void pipes_release_locks(ant_t *js, ant_value_t state) {
  ant_value_t reader = pipe_state_reader(state);
  if (is_object_type(reader)) {
    pipes_release_reader(js, reader);
    js_set_slot(state, SLOT_BUFFER, js_mkundef());
  }

  ant_value_t writer = pipe_state_writer(state);
  if (is_object_type(writer)) {
    pipes_release_writer(js, writer);
    js_set_slot(state, SLOT_DEFAULT, js_mkundef());
  }
}

static void pipes_settle(ant_t *js, ant_value_t state, bool ok, ant_value_t value) {
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst || pst->settled) return;

  pst->settled = true;
  pst->shutting_down = true;
  pipes_release_locks(js, state);

  ant_value_t promise = pipe_state_promise(state);
  if (ok) js_resolve_promise(js, promise, value);
  else js_reject_promise(js, promise, value);
}

static void pipes_ignore_promise(ant_value_t maybe_promise) {
  promise_mark_handled(maybe_promise);
}

static void pipes_shutdown_from_source_error(ant_t *js, ant_value_t state, ant_value_t error) {
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst || pst->settled || pst->shutting_down) return;

  pst->shutting_down = true;
  pst->in_flight = false;

  if (!pst->prevent_abort) {
    ant_value_t result = writable_stream_abort(js, pipe_state_dest(state), error);
    pipes_ignore_promise(result);
  }

  pipes_settle(js, state, false, error);
}

static void pipes_shutdown_from_dest_error(ant_t *js, ant_value_t state, ant_value_t error) {
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst || pst->settled || pst->shutting_down) return;

  pst->shutting_down = true;
  pst->in_flight = false;

  if (!pst->prevent_cancel) {
    ant_value_t result = readable_stream_cancel(js, pipe_state_source(state), error);
    pipes_ignore_promise(result);
  }

  pipes_settle(js, state, false, error);
}

static void pipes_shutdown_from_abort(ant_t *js, ant_value_t state, ant_value_t reason) {
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst || pst->settled || pst->shutting_down) return;

  pst->shutting_down = true;
  pst->in_flight = false;

  if (!pst->prevent_abort) {
    ant_value_t result = writable_stream_abort(js, pipe_state_dest(state), reason);
    pipes_ignore_promise(result);
  }
  if (!pst->prevent_cancel) {
    ant_value_t result = readable_stream_cancel(js, pipe_state_source(state), reason);
    pipes_ignore_promise(result);
  }

  pipes_settle(js, state, false, reason);
}

static void pipes_pump(ant_t *js, ant_value_t state);

static ant_value_t pipe_write_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst) return js_mkundef();
  pst->in_flight = false;
  if (pst->settled || pst->shutting_down)
    return js_mkundef();
  pipes_pump(js, state);
  return js_mkundef();
}

static ant_value_t pipe_dest_error(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t error = (nargs > 0) ? args[0] : js_mkundef();
  pipes_shutdown_from_dest_error(js, state, error);
  return js_mkundef();
}

static ant_value_t pipe_close_dest_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  pipes_settle(js, state, true, js_mkundef());
  return js_mkundef();
}

static ant_value_t pipe_close_dest_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t error = (nargs > 0) ? args[0] : js_mkundef();
  pipes_settle(js, state, false, error);
  return js_mkundef();
}

static ant_value_t pipe_read_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst || pst->settled || pst->shutting_down) {
    if (pst) pst->in_flight = false;
    return js_mkundef();
  }

  ant_value_t result = (nargs > 0) ? args[0] : js_mkundef();
  bool done = js_truthy(js, js_get(js, result, "done"));

  if (done) {
    pst->in_flight = false;
    if (pst->prevent_close) {
      pipes_settle(js, state, true, js_mkundef());
      return js_mkundef();
    }

    ant_value_t close_promise = writable_stream_close(js, pipe_state_dest(state));
    ant_value_t on_resolve = js_heavy_mkfun(js, pipe_close_dest_resolve, state);
    ant_value_t on_reject = js_heavy_mkfun(js, pipe_close_dest_reject, state);
    pipes_chain_thenable(js, close_promise, on_resolve, on_reject);
    return js_mkundef();
  }

  ant_value_t value = js_get(js, result, "value");
  ant_value_t write_promise = ws_writer_write(js, pipe_state_writer(state), value);
  ant_value_t on_resolve = js_heavy_mkfun(js, pipe_write_resolve, state);
  ant_value_t on_reject = js_heavy_mkfun(js, pipe_dest_error, state);
  pipes_chain_thenable(js, write_promise, on_resolve, on_reject);
  return js_mkundef();
}

static ant_value_t pipe_source_error(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t error = (nargs > 0) ? args[0] : js_mkundef();
  pipes_shutdown_from_source_error(js, state, error);
  return js_mkundef();
}

static ant_value_t pipe_ready_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst || pst->settled || pst->shutting_down) {
    if (pst) pst->in_flight = false;
    return js_mkundef();
  }

  ant_value_t read_promise = rs_default_reader_read(js, pipe_state_reader(state));
  ant_value_t on_resolve = js_heavy_mkfun(js, pipe_read_resolve, state);
  ant_value_t on_reject = js_heavy_mkfun(js, pipe_source_error, state);
  pipes_chain_thenable(js, read_promise, on_resolve, on_reject);
  return js_mkundef();
}

static ant_value_t pipe_abort_listener(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst || pst->settled || pst->shutting_down)
    return js_mkundef();

  ant_value_t signal = js_get_slot(state, SLOT_RS_CANCEL);
  pipes_shutdown_from_abort(js, state, abort_signal_get_reason(signal));
  return js_mkundef();
}

static void pipes_pump(ant_t *js, ant_value_t state) {
  pipe_state_t *pst = pipe_get_state(state);
  if (!pst || pst->settled || pst->shutting_down || pst->in_flight) return;

  pst->in_flight = true;

  ant_value_t writer = pipe_state_writer(state);
  ant_value_t ready = ws_writer_ready(writer);
  ant_value_t on_resolve = js_heavy_mkfun(js, pipe_ready_resolve, state);
  ant_value_t on_reject = js_heavy_mkfun(js, pipe_dest_error, state);
  pipes_chain_thenable(js, ready, on_resolve, on_reject);
}

static ant_value_t pipe_create_rejected(ant_t *js, ant_value_t error) {
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, error);
  return promise;
}

static void pipes_parse_options(
  ant_t *js, ant_value_t options,
  bool *prevent_close, bool *prevent_abort, bool *prevent_cancel,
  ant_value_t *signal
) {
  *prevent_close = false;
  *prevent_abort = false;
  *prevent_cancel = false;
  *signal = js_mkundef();

  if (!is_object_type(options)) return;

  *prevent_close = js_truthy(js, js_get(js, options, "preventClose"));
  *prevent_abort = js_truthy(js, js_get(js, options, "preventAbort"));
  *prevent_cancel = js_truthy(js, js_get(js, options, "preventCancel"));
  *signal = js_get(js, options, "signal");
}

ant_value_t readable_stream_pipe_to(
  ant_t *js, ant_value_t source, ant_value_t dest,
  bool prevent_close, bool prevent_abort, bool prevent_cancel,
  ant_value_t signal
) {
  rs_stream_t *rs = rs_get_stream(source);
  ws_stream_t *ws = ws_get_stream(dest);
  if (!rs || !ws) {
    js_mkerr_typed(js, JS_ERR_TYPE, "pipeTo requires a ReadableStream and WritableStream");
    return pipe_create_rejected(js, js->thrown_value);
  }

  if (is_object_type(rs_stream_reader(source))) {
    js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream is already locked");
    return pipe_create_rejected(js, js->thrown_value);
  }
  if (is_object_type(ws_stream_writer(dest))) {
    js_mkerr_typed(js, JS_ERR_TYPE, "WritableStream is already locked");
    return pipe_create_rejected(js, js->thrown_value);
  }

  if (!is_undefined(signal) && !is_object_type(signal)) {
    js_mkerr_typed(js, JS_ERR_TYPE, "pipeTo option 'signal' must be an AbortSignal");
    return pipe_create_rejected(js, js->thrown_value);
  }

  ant_value_t reader_args[1] = { source };
  ant_value_t saved = js->new_target;
  js->new_target = g_reader_proto;
  ant_value_t reader = js_rs_reader_ctor(js, reader_args, 1);
  js->new_target = saved;
  if (is_err(reader)) return pipe_create_rejected(js, js->thrown_value);

  ant_value_t writer = ws_acquire_writer(js, dest);
  if (is_err(writer)) {
    pipes_release_reader(js, reader);
    return pipe_create_rejected(js, js->thrown_value);
  }

  pipe_state_t *pst = calloc(1, sizeof(pipe_state_t));
  if (!pst) return js_mkerr(js, "out of memory");
  pst->prevent_close = prevent_close;
  pst->prevent_abort = prevent_abort;
  pst->prevent_cancel = prevent_cancel;

  ant_value_t promise = js_mkpromise(js);
  ant_value_t state = js_mkobj(js);
  js_set_slot(state, SLOT_DATA, ANT_PTR(pst));
  js_set_slot(state, SLOT_ENTRIES, source);
  js_set_slot(state, SLOT_CTOR, dest);
  js_set_slot(state, SLOT_BUFFER, reader);
  js_set_slot(state, SLOT_DEFAULT, writer);
  js_set_slot(state, SLOT_RS_PULL, promise);
  js_set_finalizer(state, pipe_state_finalize);
  js_set_slot(state, SLOT_RS_CANCEL, signal);

  promise_mark_handled(rs_reader_closed(reader));
  promise_mark_handled(js_get_slot(writer, SLOT_RS_CLOSED));
  promise_mark_handled(js_get_slot(writer, SLOT_WS_READY));

  if (is_object_type(signal) && abort_signal_is_aborted(signal)) {
    pipes_shutdown_from_abort(js, state, abort_signal_get_reason(signal));
    return promise;
  }

  if (is_object_type(signal)) {
    ant_value_t listener = js_heavy_mkfun(js, pipe_abort_listener, state);
    abort_signal_add_listener(js, signal, listener);
  }

  pipes_pump(js, state);
  return promise;
}

static ant_value_t js_rs_pipe_to(ant_t *js, ant_value_t *args, int nargs) {
  if (!is_object_type(js->this_val)) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");
    return pipe_create_rejected(js, js->thrown_value);
  }
  
  rs_stream_t *stream = rs_get_stream(js->this_val);
  if (!stream) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");
    return pipe_create_rejected(js, js->thrown_value);
  }

  ant_value_t dest = (nargs > 0) ? args[0] : js_mkundef();
  bool prevent_close, prevent_abort, prevent_cancel;
  ant_value_t signal;
  pipes_parse_options(js, nargs > 1 ? args[1] : js_mkundef(),
    &prevent_close, &prevent_abort, &prevent_cancel, &signal);
  return readable_stream_pipe_to(js, js->this_val, dest,
    prevent_close, prevent_abort, prevent_cancel, signal);
}

static ant_value_t js_rs_pipe_through(ant_t *js, ant_value_t *args, int nargs) {
  if (!is_object_type(js->this_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");
  rs_stream_t *stream = rs_get_stream(js->this_val);
  
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");
  if (is_object_type(rs_stream_reader(js->this_val)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream is already locked");
  if (nargs < 1 || !is_object_type(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "pipeThrough requires a transform object");

  ant_value_t transform = args[0];
  ant_value_t writable = js_get(js, transform, "writable");
  ant_value_t readable = js_get(js, transform, "readable");
  if (!is_object_type(writable) || !ws_get_stream(writable))
    return js_mkerr_typed(js, JS_ERR_TYPE, "pipeThrough transform.writable must be a WritableStream");
  if (!is_object_type(readable) || !rs_get_stream(readable))
    return js_mkerr_typed(js, JS_ERR_TYPE, "pipeThrough transform.readable must be a ReadableStream");
  if (is_object_type(ws_stream_writer(writable)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "WritableStream is already locked");

  bool prevent_close, prevent_abort, prevent_cancel;
  ant_value_t signal;
  pipes_parse_options(js, nargs > 1 ? args[1] : js_mkundef(),
    &prevent_close, &prevent_abort, &prevent_cancel, &signal);

  ant_value_t pipe_promise = readable_stream_pipe_to(js, js->this_val, writable,
    prevent_close, prevent_abort, prevent_cancel, signal);
  promise_mark_handled(pipe_promise);
  return readable;
}

typedef struct {
  bool pulling;
  bool done;
  bool canceled1;
  bool canceled2;
} tee_state_t;

static void tee_state_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    free((tee_state_t *)(uintptr_t)(size_t)js_getnum(entries[i].value));
    return;
  }}
}

static tee_state_t *tee_get_state(ant_value_t state) {
  ant_value_t s = js_get_slot(state, SLOT_DATA);
  if (vtype(s) != T_NUM) return NULL;
  return (tee_state_t *)(uintptr_t)(size_t)js_getnum(s);
}

static ant_value_t tee_state_reader(ant_value_t state) {
  return js_get_slot(state, SLOT_BUFFER);
}

static void tee_release_reader(ant_t *js, ant_value_t state) {
  ant_value_t reader = tee_state_reader(state);
  if (!is_object_type(reader)) return;
  pipes_release_reader(js, reader);
  js_set_slot(state, SLOT_BUFFER, js_mkundef());
}

static void tee_resolve_cancel_promises(ant_t *js, ant_value_t state) {
  ant_value_t p1 = js_get_slot(state, SLOT_RS_CLOSED);
  ant_value_t p2 = js_get_slot(state, SLOT_RS_SIZE);
  if (vtype(p1) == T_PROMISE) {
    js_resolve_promise(js, p1, js_mkundef());
    js_set_slot(state, SLOT_RS_CLOSED, js_mkundef());
  }
  if (vtype(p2) == T_PROMISE) {
    js_resolve_promise(js, p2, js_mkundef());
    js_set_slot(state, SLOT_RS_SIZE, js_mkundef());
  }
}

static void tee_reject_cancel_promises(ant_t *js, ant_value_t state, ant_value_t error) {
  ant_value_t p1 = js_get_slot(state, SLOT_RS_CLOSED);
  ant_value_t p2 = js_get_slot(state, SLOT_RS_SIZE);
  if (vtype(p1) == T_PROMISE) {
    js_reject_promise(js, p1, error);
    js_set_slot(state, SLOT_RS_CLOSED, js_mkundef());
  }
  if (vtype(p2) == T_PROMISE) {
    js_reject_promise(js, p2, error);
    js_set_slot(state, SLOT_RS_SIZE, js_mkundef());
  }
}

static void tee_finalize(ant_t *js, ant_value_t state) {
  tee_state_t *st = tee_get_state(state);
  if (!st || st->done) return;
  st->done = true;
  tee_release_reader(js, state);
}

static void tee_close_branch(ant_t *js, ant_value_t branch_stream) {
  ant_value_t ctrl = rs_stream_controller(js, branch_stream);
  rs_controller_t *c = rs_get_controller(ctrl);
  if (!c || c->close_requested) return;
  c->close_requested = true;
  if (rs_ctrl_queue_len(js, ctrl) == 0) {
    rs_default_controller_clear_algorithms(ctrl);
    readable_stream_close(js, branch_stream);
  }
}

static void tee_enqueue_branch(ant_t *js, ant_value_t branch_stream, ant_value_t value) {
  ant_value_t ctrl = rs_stream_controller(js, branch_stream);
  rs_controller_t *c = rs_get_controller(ctrl);
  if (!c || !rs_default_controller_can_close_or_enqueue(c, rs_get_stream(branch_stream)))
    return;

  ant_value_t r = rs_stream_reader(branch_stream);
  if (is_object_type(r) && rs_reader_has_reqs(js, r)) {
    rs_fulfill_read_request(js, branch_stream, value, false);
    rs_default_controller_call_pull_if_needed(js, ctrl);
    return;
  }

  double chunk_size = 1;
  ant_value_t size_fn = rs_ctrl_size(ctrl);
  if (is_callable(size_fn)) {
    ant_value_t sa[1] = { value };
    ant_value_t sr = sv_vm_call(js->vm, js, size_fn, js_mkundef(), sa, 1, NULL, false);
    if (!is_err(sr))
      chunk_size = vtype(sr) == T_NUM ? js_getnum(sr) : js_to_number(js, sr);
  }

  rs_ctrl_queue_push(js, ctrl, value);
  if (c->queue_sizes_len >= c->queue_sizes_cap) {
    uint32_t nc = c->queue_sizes_cap ? c->queue_sizes_cap * 2 : 8;
    double *ns = realloc(c->queue_sizes, nc * sizeof(double));
    if (ns) { c->queue_sizes = ns; c->queue_sizes_cap = nc; }
  }
  if (c->queue_sizes_len < c->queue_sizes_cap)
    c->queue_sizes[c->queue_sizes_len++] = chunk_size;
  c->queue_total_size += chunk_size;
  rs_default_controller_call_pull_if_needed(js, ctrl);
}

static void tee_error_branch(ant_t *js, ant_value_t branch_stream, ant_value_t error) {
  readable_stream_error(js, branch_stream, error);
}

static void tee_pull(ant_t *js, ant_value_t state);

static ant_value_t tee_cancel_both_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  tee_resolve_cancel_promises(js, state);
  tee_finalize(js, state);
  return js_mkundef();
}

static ant_value_t tee_cancel_both_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t error = (nargs > 0) ? args[0] : js_mkundef();
  tee_reject_cancel_promises(js, state, error);
  tee_finalize(js, state);
  return js_mkundef();
}

static ant_value_t tee_read_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  tee_state_t *st = tee_get_state(state);
  if (!st) return js_mkundef();
  st->pulling = false;
  if (st->done) return js_mkundef();

  ant_value_t result = (nargs > 0) ? args[0] : js_mkundef();
  bool done = js_truthy(js, js_get(js, result, "done"));
  ant_value_t branch1 = js_get_slot(state, SLOT_CTOR);
  ant_value_t branch2 = js_get_slot(state, SLOT_DEFAULT);

  if (done) {
    if (!st->canceled1) tee_close_branch(js, branch1);
    if (!st->canceled2) tee_close_branch(js, branch2);
    tee_resolve_cancel_promises(js, state);
    tee_finalize(js, state);
    return js_mkundef();
  }

  ant_value_t value = js_get(js, result, "value");
  if (!st->canceled1) tee_enqueue_branch(js, branch1, value);
  if (!st->canceled2) tee_enqueue_branch(js, branch2, value);
  return js_mkundef();
}

static ant_value_t tee_read_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  tee_state_t *st = tee_get_state(state);
  if (!st) return js_mkundef();
  ant_value_t error = (nargs > 0) ? args[0] : js_mkundef();
  st->pulling = false;
  if (st->done) return js_mkundef();

  ant_value_t branch1 = js_get_slot(state, SLOT_CTOR);
  ant_value_t branch2 = js_get_slot(state, SLOT_DEFAULT);
  
  if (!st->canceled1) tee_error_branch(js, branch1, error);
  if (!st->canceled2) tee_error_branch(js, branch2, error);
  tee_resolve_cancel_promises(js, state);
  tee_finalize(js, state);
  
  return js_mkundef();
}

static void tee_pull(ant_t *js, ant_value_t state) {
  tee_state_t *st = tee_get_state(state);
  if (!st || st->done || st->pulling) return;
  if (st->canceled1 && st->canceled2) return;

  st->pulling = true;
  ant_value_t read_promise = rs_default_reader_read(js, tee_state_reader(state));
  ant_value_t on_resolve = js_heavy_mkfun(js, tee_read_resolve, state);
  ant_value_t on_reject = js_heavy_mkfun(js, tee_read_reject, state);
  pipes_chain_thenable(js, read_promise, on_resolve, on_reject);
}

static ant_value_t tee_branch_pull(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js->current_func, SLOT_DATA);
  tee_pull(js, state);
  
  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, js_mkundef());
  
  return promise;
}

static ant_value_t tee_branch_cancel(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t state = js_get_slot(wrapper, SLOT_DATA);
  
  int branch = (int)js_getnum(js_get_slot(wrapper, SLOT_ENTRIES));
  ant_value_t reason = (nargs > 0) ? args[0] : js_mkundef();
  tee_state_t *st = tee_get_state(state);
  if (!st) return js_mkundef();

  bool is_b1 = (branch == 1);
  internal_slot_t reason_slot = is_b1 ? SLOT_RS_PULL : SLOT_RS_CANCEL;
  internal_slot_t promise_slot = is_b1 ? SLOT_RS_CLOSED : SLOT_RS_SIZE;
  bool already_canceled = is_b1 ? st->canceled1 : st->canceled2;

  if (already_canceled) {
    ant_value_t existing = js_get_slot(state, promise_slot);
    if (vtype(existing) == T_PROMISE) return existing;
    ant_value_t resolved = js_mkpromise(js);
    js_resolve_promise(js, resolved, js_mkundef());
    return resolved;
  }

  if (is_b1) st->canceled1 = true;
  else st->canceled2 = true;
  js_set_slot(state, reason_slot, reason);

  ant_value_t promise = js_mkpromise(js);
  js_set_slot(state, promise_slot, promise);

  if (st->done) {
    js_resolve_promise(js, promise, js_mkundef());
    js_set_slot(state, promise_slot, js_mkundef());
    return promise;
  }

  if (st->canceled1 && st->canceled2) {
    ant_value_t reasons = js_mkarr(js);
    js_arr_push(js, reasons, js_get_slot(state, SLOT_RS_PULL));
    js_arr_push(js, reasons, js_get_slot(state, SLOT_RS_CANCEL));
    
    ant_value_t orig_stream = js_get_slot(state, SLOT_ENTRIES);
    ant_value_t cancel_promise = readable_stream_cancel(js, orig_stream, reasons);
    ant_value_t on_resolve = js_heavy_mkfun(js, tee_cancel_both_resolve, state);
    ant_value_t on_reject = js_heavy_mkfun(js, tee_cancel_both_reject, state);
    pipes_chain_thenable(js, cancel_promise, on_resolve, on_reject);
  }

  return promise;
}

static ant_value_t js_rs_tee(ant_t *js, ant_value_t *args, int nargs) {
  if (!is_object_type(js->this_val)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");
  rs_stream_t *stream = rs_get_stream(js->this_val);
  
  if (!stream) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid ReadableStream");
  if (is_object_type(rs_stream_reader(js->this_val)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "ReadableStream is already locked");

  ant_value_t reader_args[1] = { js->this_val };
  ant_value_t saved = js->new_target;
  js->new_target = g_reader_proto;
  ant_value_t reader = js_rs_reader_ctor(js, reader_args, 1);
  js->new_target = saved;
  if (is_err(reader)) return reader;

  tee_state_t *st = calloc(1, sizeof(tee_state_t));
  if (!st) return js_mkerr(js, "out of memory");

  ant_value_t state = js_mkobj(js);
  js_set_slot(state, SLOT_DATA, ANT_PTR(st));
  js_set_slot(state, SLOT_ENTRIES, js->this_val);
  js_set_slot(state, SLOT_BUFFER, reader);
  js_set_slot(state, SLOT_RS_PULL, js_mkundef());
  js_set_slot(state, SLOT_RS_CANCEL, js_mkundef());
  js_set_slot(state, SLOT_RS_CLOSED, js_mkundef());
  js_set_slot(state, SLOT_RS_SIZE, js_mkundef());
  js_set_finalizer(state, tee_state_finalize);

  ant_value_t pull1 = js_heavy_mkfun(js, tee_branch_pull, state);
  ant_value_t pull2 = js_heavy_mkfun(js, tee_branch_pull, state);

  ant_value_t cancel1_wrap = js_mkobj(js);
  js_set_slot(cancel1_wrap, SLOT_DATA, state);
  js_set_slot(cancel1_wrap, SLOT_ENTRIES, js_mknum(1));
  ant_value_t cancel1 = js_heavy_mkfun(js, tee_branch_cancel, cancel1_wrap);

  ant_value_t cancel2_wrap = js_mkobj(js);
  js_set_slot(cancel2_wrap, SLOT_DATA, state);
  js_set_slot(cancel2_wrap, SLOT_ENTRIES, js_mknum(2));
  ant_value_t cancel2 = js_heavy_mkfun(js, tee_branch_cancel, cancel2_wrap);

  ant_value_t branch1 = rs_create_stream(js, pull1, cancel1, 1);
  ant_value_t branch2 = rs_create_stream(js, pull2, cancel2, 1);
  if (is_err(branch1) || is_err(branch2)) {
    tee_release_reader(js, state);
    return is_err(branch1) ? branch1 : branch2;
  }

  js_set_slot(state, SLOT_CTOR, branch1);
  js_set_slot(state, SLOT_DEFAULT, branch2);

  ant_value_t result = js_mkarr(js);
  js_arr_push(js, result, branch1);
  js_arr_push(js, result, branch2);
  
  return result;
}

void init_pipes_proto(ant_t *js, ant_value_t rs_proto) {
  js_set(js, rs_proto, "pipeTo", js_mkfun(js_rs_pipe_to));
  js_set_descriptor(js, rs_proto, "pipeTo", 6, JS_DESC_W | JS_DESC_C);
  js_set(js, rs_proto, "pipeThrough", js_mkfun(js_rs_pipe_through));
  js_set_descriptor(js, rs_proto, "pipeThrough", 11, JS_DESC_W | JS_DESC_C);
  js_set(js, rs_proto, "tee", js_mkfun(js_rs_tee));
  js_set_descriptor(js, rs_proto, "tee", 3, JS_DESC_W | JS_DESC_C);
}
