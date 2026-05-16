#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "ptr.h"
#include "internal.h"
#include "silver/engine.h"
#include "esm/loader.h"
#include "gc/roots.h"

#include "modules/assert.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/stream.h"
#include "modules/symbol.h"
#include "modules/string_decoder.h"

enum { STREAM_NATIVE_TAG = 0x5354524Du }; // STRM







static double g_default_high_water_mark = 16384.0;
static double g_default_object_high_water_mark = 16.0;

static ant_value_t stream_noop(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static bool stream_is_instance(ant_value_t value) {
  return is_object_type(value) && js_check_native_tag(value, STREAM_NATIVE_TAG);
}

static inline void stream_set_end_callback(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  js_set_slot_wb(js, stream_obj, SLOT_AUX, callback);
}

static stream_private_state_t *stream_private_state(ant_value_t stream_obj) {
  if (!stream_is_instance(stream_obj)) return NULL;
  return (stream_private_state_t *)js_get_native(stream_obj, STREAM_NATIVE_TAG);
}

static ant_value_t stream_require_this(ant_t *js, ant_value_t value, const char *label) {
  if (!stream_is_instance(value))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid %s", label);
  return value;
}

static ant_value_t stream_truthy_or_object(ant_t *js, ant_value_t value) {
  return js_truthy(js, value) ? value : js_mkobj(js);
}

static ant_value_t stream_readable_state(ant_t *js, ant_value_t stream_obj) {
  return js_get(js, stream_obj, "_readableState");
}

static ant_value_t stream_writable_state(ant_t *js, ant_value_t stream_obj) {
  return js_get(js, stream_obj, "_writableState");
}

static ant_value_t stream_pipes(ant_t *js, ant_value_t stream_obj) {
  return js_get(js, stream_obj, "_pipes");
}

static bool stream_key_is_cstr(ant_t *js, ant_value_t value, const char *expected) {
  size_t len = 0;
  const char *s = NULL;
  if (vtype(value) != T_STR) return false;
  s = js_getstr(js, value, &len);
  return s && len == strlen(expected) && memcmp(s, expected, len) == 0;
}

static ant_value_t stream_event_key(ant_t *js, ant_value_t value) {
  uint8_t t = vtype(value);
  if (t == T_STR || t == T_SYMBOL) return value;
  return js_mkerr(js, "event must be a string or Symbol");
}

void *stream_get_attached_state(ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  return priv ? priv->attached_state : NULL;
}

void stream_set_attached_state(
  ant_value_t stream_obj,
  void *state,
  stream_finalize_fn finalize
) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return;
  priv->attached_state = state;
  priv->attached_state_finalize = finalize;
}

void stream_clear_attached_state(ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return;
  priv->attached_state = NULL;
  priv->attached_state_finalize = NULL;
}

static void stream_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t stream_obj = js_obj_from_ptr(obj);
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return;
  js_clear_native(stream_obj, STREAM_NATIVE_TAG);
  if (priv->attached_state && priv->attached_state_finalize)
    priv->attached_state_finalize(js, stream_obj, priv->attached_state);
  free(priv);
}

static ant_value_t stream_call(
  ant_t *js,
  ant_value_t fn,
  ant_value_t this_val,
  ant_value_t *args,
  int nargs,
  bool is_ctor
) {
  if (!is_callable(fn)) return js_mkundef();
  if (sv_check_c_stack_overflow(js))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");

  sv_call_mode_t mode = is_ctor ? SV_CALL_MODE_CONSTRUCT : SV_CALL_MODE_NORMAL;
  sv_call_plan_t plan;
  ant_value_t err = sv_prepare_call(js->vm, js, fn, this_val, args, nargs, NULL, mode, &plan);
  if (is_err(err)) return err;

  return sv_execute_call_plan(js->vm, js, &plan, NULL);
}

static ant_value_t stream_call_prop(
  ant_t *js,
  ant_value_t target,
  const char *name,
  ant_value_t *args,
  int nargs
) {
  ant_value_t fn = js_getprop_fallback(js, target, name);
  if (is_err(fn) || !is_callable(fn)) return js_mkundef();
  return stream_call(js, fn, target, args, nargs, false);
}

static void stream_call_callback(ant_t *js, ant_value_t fn, ant_value_t *args, int nargs) {
  if (!is_callable(fn)) return;
  stream_call(js, fn, js_mkundef(), args, nargs, false);
}

static void stream_schedule_microtask(ant_t *js, ant_cfunc_t fn, ant_value_t data) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t cb = js_heavy_mkfun(js, fn, data);
  ant_value_t then_result = 0;

  js_resolve_promise(js, promise, js_mkundef());
  then_result = js_promise_then(js, promise, cb, js_mkundef());
  promise_mark_handled(then_result);
}

static ant_value_t stream_buffer_ctor(ant_t *js) {
  ant_value_t ns = js_esm_import_sync_cstr(js, "buffer", 6);
  if (is_err(ns)) return ns;
  return js_get(js, ns, "Buffer");
}

static ant_value_t stream_readable_decoder(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  if (!is_object_type(state)) return js_mkundef();
  return js_get(js, state, "decoder");
}

static ant_value_t stream_readable_decode_chunk(
  ant_t *js, ant_value_t stream_obj,
  ant_value_t chunk, bool flush
) {
  ant_value_t decoder = stream_readable_decoder(js, stream_obj);
  if (!is_object_type(decoder)) return chunk;
  return string_decoder_decode_value(js, decoder, chunk, flush);
}

static bool stream_value_is_empty_string(ant_t *js, ant_value_t value) {
  size_t len = 0;
  if (vtype(value) != T_STR) return false;
  (void)js_getstr(js, value, &len);
  return len == 0;
}

static ant_value_t stream_make_buffer(ant_t *js, ant_value_t value, ant_value_t encoding) {
  ant_value_t buffer_ctor = stream_buffer_ctor(js);
  ant_value_t from_fn = 0;
  ant_value_t args[2];

  if (is_err(buffer_ctor)) return buffer_ctor;
  from_fn = js_get(js, buffer_ctor, "from");
  if (is_err(from_fn) || !is_callable(from_fn))
    return js_mkerr(js, "Buffer.from is not available");
    
  args[0] = value;
  args[1] = encoding;
  return stream_call(js, from_fn, buffer_ctor, args, 2, false);
}

static ant_value_t stream_normalize_chunk(
  ant_t *js,
  ant_value_t chunk,
  bool object_mode,
  ant_value_t encoding
) {
  ant_value_t str_val = 0;

  if (
    object_mode || is_null(chunk) || is_undefined(chunk) ||
    vtype(chunk) == T_TYPEDARRAY || buffer_is_binary_source(chunk)
  ) return chunk;

  if (vtype(chunk) == T_STR) return stream_make_buffer(js, chunk, encoding);

  str_val = js_tostring_val(js, chunk);
  if (is_err(str_val)) return str_val;
  
  return stream_make_buffer(js, str_val, encoding);
}

static ant_value_t stream_readable_buffer(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  if (!is_object_type(state)) return js_mkundef();
  return js_get(js, state, "buffer");
}

static ant_offset_t stream_readable_buffer_head(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  ant_value_t head = is_object_type(state) ? js_get(js, state, "bufferHead") : js_mkundef();
  return vtype(head) == T_NUM ? (ant_offset_t)js_getnum(head) : 0;
}

static void stream_set_readable_buffer_head(ant_t *js, ant_value_t stream_obj, ant_offset_t head) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  if (is_object_type(state)) js_set(js, state, "bufferHead", js_mknum((double)head));
}

static ant_offset_t stream_readable_buffer_len(ant_t *js, ant_value_t stream_obj) {
  ant_value_t buffer = stream_readable_buffer(js, stream_obj);
  ant_offset_t head = stream_readable_buffer_head(js, stream_obj);
  ant_offset_t len = vtype(buffer) == T_ARR ? js_arr_len(js, buffer) : 0;
  return len > head ? len - head : 0;
}

static double stream_chunk_size(ant_t *js, ant_value_t chunk, bool object_mode) {
  const uint8_t *bytes = NULL;
  size_t byte_len = 0;
  if (object_mode) return 1;
  if (buffer_source_get_bytes(js, chunk, &bytes, &byte_len)) return (double)byte_len;
  return 1;
}

static void stream_compact_readable_buffer(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  ant_value_t buffer = stream_readable_buffer(js, stream_obj);
  ant_offset_t head = stream_readable_buffer_head(js, stream_obj);
  ant_offset_t len = vtype(buffer) == T_ARR ? js_arr_len(js, buffer) : 0;
  ant_value_t compact = 0;

  if (!is_object_type(state) || vtype(buffer) != T_ARR) return;
  if (head == 0) return;
  
  if (head >= len) {
    compact = js_mkarr(js);
    js_set(js, state, "buffer", compact);
    js_set(js, state, "bufferHead", js_mknum(0));
    return;
  }

  if (head <= 32 && head * 2 < len) return;
  compact = js_mkarr(js);
  for (ant_offset_t i = head; i < len; i++) js_arr_push(js, compact, js_arr_get(js, buffer, i));
  
  js_set(js, state, "buffer", compact);
  js_set(js, state, "bufferHead", js_mknum(0));
}

static void stream_buffer_push(ant_t *js, ant_value_t stream_obj, ant_value_t value) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  ant_value_t buffer = stream_readable_buffer(js, stream_obj);
  ant_value_t length = 0;
  bool object_mode = false;

  if (!is_object_type(state) || vtype(buffer) != T_ARR) return;
  js_arr_push(js, buffer, value);
  object_mode = js_truthy(js, js_get(js, state, "objectMode"));
  length = js_get(js, state, "length");
  js_set(js, state, "length", js_mknum((vtype(length) == T_NUM ? tod(length) : 0) + stream_chunk_size(js, value, object_mode)));
}

static ant_value_t stream_buffer_shift(ant_t *js, ant_value_t stream_obj) {
  ant_value_t buffer = stream_readable_buffer(js, stream_obj);
  ant_offset_t head = stream_readable_buffer_head(js, stream_obj);
  ant_offset_t len = vtype(buffer) == T_ARR ? js_arr_len(js, buffer) : 0;
  ant_value_t value = js_mkundef();

  if (vtype(buffer) != T_ARR || head >= len) return js_mkundef();
  value = js_arr_get(js, buffer, head);
  ant_value_t state = stream_readable_state(js, stream_obj);
  
  if (is_object_type(state)) {
    ant_value_t length = js_get(js, state, "length");
    bool object_mode = js_truthy(js, js_get(js, state, "objectMode"));
    double next_length = (vtype(length) == T_NUM ? tod(length) : 0) - stream_chunk_size(js, value, object_mode);
    js_set(js, state, "length", js_mknum(next_length > 0 ? next_length : 0));
    js_set(js, state, "dataEmitted", js_true);
  }
  
  stream_set_readable_buffer_head(js, stream_obj, head + 1);
  stream_compact_readable_buffer(js, stream_obj);
  
  return value;
}

static bool stream_listener_count_positive(ant_t *js, ant_value_t target, const char *event_name) {
  ant_value_t args[1];
  ant_value_t result = 0;

  args[0] = js_mkstr(js, event_name, strlen(event_name));
  result = stream_call_prop(js, target, "listenerCount", args, 1);
  return vtype(result) == T_NUM && js_getnum(result) > 0;
}

static void stream_remove_listener(
  ant_t *js,
  ant_value_t target,
  const char *event_name,
  ant_value_t listener
) {
  ant_value_t args[2];
  args[0] = js_mkstr(js, event_name, strlen(event_name));
  args[1] = listener;
  stream_call_prop(js, target, "removeListener", args, 2);
}

static ant_value_t stream_get_option(ant_t *js, ant_value_t options, const char *name) {
  if (!is_object_type(options)) return js_mkundef();
  return js_get(js, options, name);
}

static double stream_default_high_water_mark(bool object_mode) {
  return object_mode ? g_default_object_high_water_mark : g_default_high_water_mark;
}

static double stream_high_water_mark_from_options(ant_t *js, ant_value_t options, bool object_mode) {
  ant_value_t hwm = stream_get_option(js, options, "highWaterMark");
  return (vtype(hwm) == T_NUM && js_getnum(hwm) > 0)
    ? js_getnum(hwm)
    : stream_default_high_water_mark(object_mode);
}

static ant_value_t stream_make_base_object(ant_t *js, ant_value_t proto) {
  ant_value_t obj = js_mkobj(js);
  stream_private_state_t *priv = calloc(1, sizeof(*priv));
  
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, priv, STREAM_NATIVE_TAG);
  js_set_slot(obj, SLOT_AUX, js_mkundef());
  js_set_finalizer(obj, stream_finalize);
  
  return obj;
}

static void stream_init_base(ant_t *js, ant_value_t obj, ant_value_t raw_options) {
  ant_value_t pipes = js_mkarr(js);
  js_set(js, obj, "readable", js_true);
  js_set(js, obj, "writable", js_true);
  js_set(js, obj, "destroyed", js_false);
  js_set(js, obj, "_paused", js_false);
  js_set(js, obj, "_pipes", pipes);
  js_set(js, obj, "_streamOptions", stream_truthy_or_object(js, raw_options));
}

static void stream_init_readable(ant_t *js, ant_value_t obj, ant_value_t raw_options) {
  ant_value_t options = is_object_type(raw_options) ? raw_options : js_mkobj(js);
  ant_value_t state = js_mkobj(js);
  ant_value_t read_fn = stream_get_option(js, options, "read");

  bool object_mode = js_truthy(js, stream_get_option(js, options, "objectMode"));
  double high_water_mark = stream_high_water_mark_from_options(js, options, object_mode);

  stream_init_base(js, obj, raw_options);
  js_set(js, obj, "readable", js_true);
  js_set(js, obj, "writable", js_false);
  js_set(js, obj, "readableEnded", js_false);

  js_set(js, state, "objectMode", js_bool(object_mode));
  js_set(js, state, "ended", js_false);
  js_set(js, state, "endEmitted", js_false);
  js_set(js, state, "endScheduled", js_false);
  js_set(js, state, "dataEmitted", js_false);
  js_set(js, state, "errored", js_mkundef());
  js_set(js, state, "flowing", js_false);
  js_set(js, state, "flowingReadScheduled", js_false);
  js_set(js, state, "reading", js_false);
  js_set(js, state, "highWaterMark", js_mknum(high_water_mark));
  js_set(js, state, "length", js_mknum(0));
  js_set(js, state, "buffer", js_mkarr(js));
  js_set(js, state, "bufferHead", js_mknum(0));
  js_set(js, obj, "_readableState", state);

  if (is_callable(read_fn)) js_set(js, obj, "_read", read_fn);
}

static void stream_init_writable(ant_t *js, ant_value_t obj, ant_value_t raw_options) {
  ant_value_t options = is_object_type(raw_options) ? raw_options : js_mkobj(js);
  ant_value_t state = js_mkobj(js);
  bool object_mode = js_truthy(js, stream_get_option(js, options, "objectMode"))
    || js_truthy(js, stream_get_option(js, options, "writableObjectMode"));
  ant_value_t write_fn = stream_get_option(js, options, "write");
  ant_value_t final_fn = stream_get_option(js, options, "final");
  ant_value_t destroy_fn = stream_get_option(js, options, "destroy");

  stream_init_base(js, obj, raw_options);
  js_set(js, obj, "readable", js_false);
  js_set(js, obj, "writable", js_true);
  js_set(js, obj, "writableEnded", js_false);
  js_set(js, obj, "writableFinished", js_false);

  js_set(js, state, "objectMode", js_bool(object_mode));
  js_set(js, state, "finished", js_false);
  js_set(js, state, "ended", js_false);
  js_set(js, state, "errored", js_mkundef());
  js_set(js, obj, "_writableState", state);

  if (is_callable(write_fn)) js_set(js, obj, "_write", write_fn);
  if (is_callable(final_fn)) js_set(js, obj, "_final", final_fn);
  if (is_callable(destroy_fn)) js_set(js, obj, "_destroy", destroy_fn);
}

static ant_value_t stream_construct(
  ant_t *js,
  ant_value_t base_proto,
  ant_value_t raw_options,
  void (*init_fn)(ant_t *, ant_value_t, ant_value_t)
) {
  ant_value_t proto = js_instance_proto_from_new_target(js, base_proto);
  ant_value_t obj = stream_make_base_object(js, is_object_type(proto) ? proto : base_proto);
  init_fn(js, obj, raw_options);
  return obj;
}

static ant_value_t stream_emit_named(ant_t *js, ant_value_t stream_obj, const char *event_name) {
  return js_bool(eventemitter_emit_args(js, stream_obj, event_name, NULL, 0));
}

static void stream_set_errored(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  if (is_object_type(state)) js_set(js, state, "errored", error);

  state = stream_writable_state(js, stream_obj);
  if (is_object_type(state)) js_set(js, state, "errored", error);
}

static void stream_emit_error(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  ant_value_t args[1];
  stream_set_errored(js, stream_obj, error);
  args[0] = error;
  eventemitter_emit_args(js, stream_obj, "error", args, 1);
}

static void stream_readable_schedule_flowing_tick(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);

  if (!is_object_type(state)) return;
  if (!js_truthy(js, js_get(js, state, "flowing"))) return;
  if (js_truthy(js, js_get(js, stream_obj, "destroyed"))) return;
  if (js_truthy(js, js_get(js, state, "flowingReadScheduled"))) return;
  js_set(js, state, "flowingReadScheduled", js_true);
  stream_schedule_microtask(js, stream_readable_continue_flowing, stream_obj);
}

static void stream_readable_schedule_continue_flowing(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);

  if (!is_object_type(state)) return;
  if (!js_truthy(js, js_get(js, state, "flowing"))) return;
  if (js_truthy(js, js_get(js, stream_obj, "destroyed"))) return;
  if (js_truthy(js, js_get(js, state, "ended"))) return;
  if (stream_readable_buffer_len(js, stream_obj) > 0) return;
  stream_readable_schedule_flowing_tick(js, stream_obj);
}


static ant_value_t js_stream_pause(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  if (is_err(stream_obj)) return stream_obj;

  js_set(js, stream_obj, "_paused", js_true);
  stream_emit_named(js, stream_obj, "pause");
  return stream_obj;
}

static ant_value_t js_stream_resume(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  if (is_err(stream_obj)) return stream_obj;

  js_set(js, stream_obj, "_paused", js_false);
  stream_emit_named(js, stream_obj, "resume");
  return stream_obj;
}

static ant_value_t js_stream_is_paused(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t paused = 0;
  if (is_err(stream_obj)) return stream_obj;
  paused = js_get(js, stream_obj, "_paused");
  return js_bool(js_truthy(js, paused));
}

static void stream_pipe_remove_state(ant_t *js, ant_value_t source, ant_value_t state_obj) {
  ant_value_t pipes = stream_pipes(js, source);
  ant_offset_t len = vtype(pipes) == T_ARR ? js_arr_len(js, pipes) : 0;
  ant_value_t next = js_mkarr(js);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = js_arr_get(js, pipes, i);
    if (item != state_obj) js_arr_push(js, next, item);
  }

  js_set(js, source, "_pipes", next);
}

static void stream_pipe_cleanup(ant_t *js, ant_value_t state_obj) {
  ant_value_t cleaned = js_get(js, state_obj, "cleaned");
  ant_value_t source = js_get(js, state_obj, "source");
  ant_value_t dest = js_get(js, state_obj, "dest");
  ant_value_t on_data = js_get(js, state_obj, "onData");
  ant_value_t on_drain = js_get(js, state_obj, "onDrain");
  ant_value_t on_end = js_get(js, state_obj, "onEnd");
  ant_value_t on_close = js_get(js, state_obj, "onClose");
  ant_value_t on_error = js_get(js, state_obj, "onError");

  if (js_truthy(js, cleaned)) return;
  js_set(js, state_obj, "cleaned", js_true);

  if (stream_is_instance(source)) {
    stream_remove_listener(js, source, "data", on_data);
    stream_remove_listener(js, source, "end", on_end);
    stream_remove_listener(js, source, "close", on_close);
    stream_remove_listener(js, source, "error", on_error);
    stream_pipe_remove_state(js, source, state_obj);
  }

  if (is_object_type(dest))
    stream_remove_listener(js, dest, "drain", on_drain);
}

static ant_value_t stream_pipe_on_data(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t source = js_get(js, state_obj, "source");
  ant_value_t dest = js_get(js, state_obj, "dest");
  ant_value_t result = js_mkundef();

  if (!is_object_type(dest)) return js_mkundef();
  result = stream_call_prop(js, dest, "write", nargs > 0 ? &args[0] : NULL, nargs > 0 ? 1 : 0);
  if (is_err(result)) return result;

  if (result == js_false) stream_call_prop(js, source, "pause", NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_drain(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t source = js_get(js, state_obj, "source");
  stream_call_prop(js, source, "resume", NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_end(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t dest = js_get(js, state_obj, "dest");
  bool end_dest = js_truthy(js, js_get(js, state_obj, "end"));
  stream_pipe_cleanup(js, state_obj);
  if (end_dest) stream_call_prop(js, dest, "end", NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  stream_pipe_cleanup(js, state_obj);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_error(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t dest = js_get(js, state_obj, "dest");
  stream_pipe_cleanup(js, state_obj);
  if (is_object_type(dest) && stream_listener_count_positive(js, dest, "error") && nargs > 0)
    eventemitter_emit_args(js, dest, "error", &args[0], 1);
  return js_mkundef();
}

static ant_value_t js_stream_pipe(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t state_obj = 0;
  ant_value_t readable_state = 0;
  bool end_dest = true;

  if (is_err(source)) return source;
  if (nargs < 1 || !is_object_type(args[0])) return js_mkerr(js, "pipe requires a destination stream");
  if (is_object_type(options)) {
    ant_value_t end_val = js_get(js, options, "end");
    if (!is_undefined(end_val)) end_dest = end_val != js_false;
  }

  state_obj = js_mkobj(js);
  js_set(js, state_obj, "source", source);
  js_set(js, state_obj, "dest", args[0]);
  js_set(js, state_obj, "end", js_bool(end_dest));
  js_set(js, state_obj, "cleaned", js_false);
  js_set(js, state_obj, "onData", js_heavy_mkfun(js, stream_pipe_on_data, state_obj));
  js_set(js, state_obj, "onDrain", js_heavy_mkfun(js, stream_pipe_on_drain, state_obj));
  js_set(js, state_obj, "onEnd", js_heavy_mkfun(js, stream_pipe_on_end, state_obj));
  js_set(js, state_obj, "onClose", js_heavy_mkfun(js, stream_pipe_on_close, state_obj));
  js_set(js, state_obj, "onError", js_heavy_mkfun(js, stream_pipe_on_error, state_obj));

  js_arr_push(js, stream_pipes(js, source), state_obj);
  eventemitter_add_listener(js, source, "data", js_get(js, state_obj, "onData"), false);
  eventemitter_add_listener(js, source, "end", js_get(js, state_obj, "onEnd"), true);
  eventemitter_add_listener(js, source, "close", js_get(js, state_obj, "onClose"), true);
  eventemitter_add_listener(js, source, "error", js_get(js, state_obj, "onError"), false);
  eventemitter_add_listener(js, args[0], "drain", js_get(js, state_obj, "onDrain"), false);
  eventemitter_emit_args(js, args[0], "pipe", &source, 1);
  readable_state = stream_readable_state(js, source);
  if (is_object_type(readable_state)) js_set(js, readable_state, "flowing", js_true);
  stream_call_prop(js, source, "resume", NULL, 0);

  return args[0];
}

static ant_value_t js_stream_unpipe(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t pipes = 0;
  ant_value_t matches = 0;
  ant_offset_t len = 0;
  ant_value_t dest = nargs > 0 ? args[0] : js_mkundef();

  if (is_err(source)) return source;
  pipes = stream_pipes(js, source);
  if (vtype(pipes) != T_ARR) return source;

  matches = js_mkarr(js);
  len = js_arr_len(js, pipes);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t state_obj = js_arr_get(js, pipes, i);
    ant_value_t entry_dest = js_get(js, state_obj, "dest");
    if (!is_object_type(dest) || entry_dest == dest) js_arr_push(js, matches, state_obj);
  }

  len = js_arr_len(js, matches);
  for (ant_offset_t i = 0; i < len; i++) stream_pipe_cleanup(js, js_arr_get(js, matches, i));
  return source;
}

static ant_value_t stream_destroy_done(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_get(js, state_obj, "stream");
  ant_value_t destroyed_err = (nargs > 0) ? args[0] : js_mkundef();
  if (!is_null(destroyed_err) && !is_undefined(destroyed_err)) stream_emit_error(js, stream_obj, destroyed_err);
  stream_emit_named(js, stream_obj, "close");
  return js_mkundef();
}

static ant_value_t stream_once_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t fn = js_get(js, state_obj, "fn");
  ant_value_t this_val = js_get(js, state_obj, "thisVal");
  ant_value_t called = js_get(js, state_obj, "called");

  if (js_truthy(js, called)) return js_mkundef();
  js_set(js, state_obj, "called", js_true);
  return stream_call(js, fn, this_val, args, nargs, false);
}

static ant_value_t stream_make_once(ant_t *js, ant_value_t fn, ant_value_t this_val) {
  ant_value_t state_obj = js_mkobj(js);
  js_set(js, state_obj, "fn", fn);
  js_set(js, state_obj, "thisVal", this_val);
  js_set(js, state_obj, "called", js_false);
  return js_heavy_mkfun(js, stream_once_call, state_obj);
}

static ant_value_t js_stream_destroy(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t destroy_fn = 0;
  ant_value_t done_state = 0;
  ant_value_t done = 0;
  ant_value_t destroy_args[2];
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t result = 0;

  if (is_err(stream_obj)) return stream_obj;
  if (js_truthy(js, js_get(js, stream_obj, "destroyed"))) return stream_obj;

  js_set(js, stream_obj, "destroyed", js_true);
  if (!is_undefined(error) && !is_null(error)) stream_set_errored(js, stream_obj, error);

  done_state = js_mkobj(js);
  js_set(js, done_state, "stream", stream_obj);
  done = stream_make_once(js, js_heavy_mkfun(js, stream_destroy_done, done_state), js_mkundef());
  destroy_fn = js_getprop_fallback(js, stream_obj, "_destroy");

  if (is_callable(destroy_fn)) {
    destroy_args[0] = is_undefined(error) ? js_mknull() : error;
    destroy_args[1] = done;
    result = stream_call(js, destroy_fn, stream_obj, destroy_args, 2, false);
    return is_err(result) ? result : stream_obj;
  }

  destroy_args[0] = is_undefined(error) ? js_mknull() : error;
  stream_call_callback(js, done, destroy_args, 1);
  return stream_obj;
}

static ant_value_t js_readable__read(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static ant_value_t stream_readable_start_flowing(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  return stream_readable_begin_flowing(js, stream_obj);
}

ant_value_t stream_readable_continue_flowing(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t state = stream_readable_state(js, stream_obj);

  if (!is_object_type(state)) return js_mkundef();
  js_set(js, state, "flowingReadScheduled", js_false);

  if (!js_truthy(js, js_get(js, state, "flowing"))) return js_mkundef();
  if (js_truthy(js, js_get(js, stream_obj, "destroyed"))) return js_mkundef();

  stream_readable_maybe_read(js, stream_obj);
  stream_readable_flush(js, stream_obj);
  
  return js_mkundef();
}

ant_value_t stream_readable_begin_flowing(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);

  if (!is_object_type(state)) return js_mkundef();
  if (!js_truthy(js, js_get(js, state, "flowing"))) return js_mkundef();

  {
    ant_value_t saved_this = js->this_val;
    js->this_val = stream_obj;
    js_stream_resume(js, NULL, 0);
    js->this_val = saved_this;
  }

  stream_readable_maybe_read(js, stream_obj);
  stream_readable_flush(js, stream_obj);
  
  return js_mkundef();
}

static ant_value_t stream_readable_emit_end(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t state = stream_readable_state(js, stream_obj);
  ant_value_t tail = 0;

  if (!is_object_type(state)) return js_mkundef();
  js_set(js, state, "endScheduled", js_false);

  if (!js_truthy(js, js_get(js, state, "flowing"))) return js_mkundef();
  if (!js_truthy(js, js_get(js, state, "ended"))) return js_mkundef();
  if (stream_readable_buffer_len(js, stream_obj) != 0) return js_mkundef();
  if (js_truthy(js, js_get(js, state, "endEmitted"))) return js_mkundef();
  if (js_truthy(js, js_get(js, stream_obj, "destroyed"))) return js_mkundef();

  tail = stream_readable_decode_chunk(js, stream_obj, js_mkundef(), true);
  if (is_err(tail)) return tail;
  if (!is_undefined(tail) && !stream_value_is_empty_string(js, tail)) {
    js_set(js, state, "dataEmitted", js_true);
    eventemitter_emit_args(js, stream_obj, "data", &tail, 1);
  }
  js_set(js, state, "endEmitted", js_true);
  js_set(js, stream_obj, "readableEnded", js_true);
  stream_emit_named(js, stream_obj, "end");
  stream_emit_named(js, stream_obj, "close");

  return js_mkundef();
}

static void stream_readable_schedule_end(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);

  if (!is_object_type(state)) return;
  if (js_truthy(js, js_get(js, state, "endScheduled"))) return;
  js_set(js, state, "endScheduled", js_true);
  stream_schedule_microtask(js, stream_readable_emit_end, stream_obj);
}

ant_value_t stream_readable_flush(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  bool emitted_data = false;

  if (!is_object_type(state)) return js_mkundef();

  while (js_truthy(js, js_get(js, state, "flowing")) && stream_readable_buffer_len(js, stream_obj) > 0) {
    ant_value_t chunk = stream_buffer_shift(js, stream_obj);
    chunk = stream_readable_decode_chunk(js, stream_obj, chunk, false);
    if (is_err(chunk)) return chunk;
    emitted_data = true;
    eventemitter_emit_args(js, stream_obj, "data", &chunk, 1);
  }

  if (
    js_truthy(js, js_get(js, state, "flowing")) &&
    js_truthy(js, js_get(js, state, "ended")) &&
    stream_readable_buffer_len(js, stream_obj) == 0 &&
    !js_truthy(js, js_get(js, state, "endEmitted"))
  ) {
    stream_readable_schedule_end(js, stream_obj);
  } else if (emitted_data) stream_readable_schedule_continue_flowing(js, stream_obj);

  return js_mkundef();
}

ant_value_t stream_readable_maybe_read(ant_t *js, ant_value_t stream_obj) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  
  ant_value_t read_fn = 0;
  ant_value_t args[1];
  ant_value_t hwm = 0;
  ant_value_t length = 0;

  if (!is_object_type(state)) return js_mkundef();
  if (js_truthy(js, js_get(js, stream_obj, "destroyed"))) return js_mkundef();
  if (js_truthy(js, js_get(js, state, "reading"))) return js_mkundef();
  if (js_truthy(js, js_get(js, state, "ended"))) return js_mkundef();

  hwm = js_get(js, state, "highWaterMark");
  length = js_get(js, state, "length");
  if (vtype(hwm) == T_NUM && vtype(length) == T_NUM && tod(length) >= tod(hwm))
    return js_mkundef();

  read_fn = js_getprop_fallback(js, stream_obj, "_read");
  js_set(js, state, "reading", js_true);
  args[0] = hwm;
  
  if (is_callable(read_fn)) stream_call(js, read_fn, stream_obj, args, 1, false);
  js_set(js, state, "reading", js_false);
  
  return js_mkundef();
}

ant_value_t stream_readable_push_value(
  ant_t *js,
  ant_value_t stream_obj,
  ant_value_t chunk,
  ant_value_t encoding
) {
  ant_value_t state = stream_readable_state(js, stream_obj);
  ant_value_t normalized = 0;

  if (!is_object_type(state)) return js_false;
  if (js_truthy(js, js_get(js, stream_obj, "destroyed"))) return js_false;

  if (is_null(chunk)) {
    js_set(js, state, "ended", js_true);
    stream_readable_flush(js, stream_obj);
    return js_false;
  }

  normalized = stream_normalize_chunk(
    js, chunk,
    js_truthy(js, js_get(js, state, "objectMode")),
    is_undefined(encoding) ? js_mkstr(js, "utf8", 4) : encoding
  );
  if (is_err(normalized)) return normalized;

  stream_buffer_push(js, stream_obj, normalized);
  if (js_truthy(js, js_get(js, state, "flowing"))) stream_readable_flush(js, stream_obj);
  
  return js_bool(js_truthy(js, js_get(js, state, "flowing")));
}

static ant_value_t js_readable_push(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t encoding = nargs > 1 ? args[1] : js_mkundef();
  if (is_err(stream_obj)) return stream_obj;
  return stream_readable_push_value(js, stream_obj, chunk, encoding);
}

static ant_value_t js_readable_read(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t state = 0;
  ant_value_t chunk = 0;

  if (is_err(stream_obj)) return stream_obj;
  state = stream_readable_state(js, stream_obj);
  if (!is_object_type(state)) return js_mknull();

  if (nargs > 0 && vtype(args[0]) == T_NUM && tod(args[0]) == 0.0) {
    stream_readable_maybe_read(js, stream_obj);
    return js_mknull();
  }

  if (stream_readable_buffer_len(js, stream_obj) == 0) stream_readable_maybe_read(js, stream_obj);
  if (stream_readable_buffer_len(js, stream_obj) == 0) return js_mknull();

  chunk = stream_buffer_shift(js, stream_obj);
  chunk = stream_readable_decode_chunk(js, stream_obj, chunk, false);
  if (is_err(chunk)) return chunk;
  if (js_truthy(js, js_get(js, state, "flowing"))) stream_readable_flush(js, stream_obj);
  
  return chunk;
}

static ant_value_t js_readable_set_encoding(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t state = 0; ant_value_t decoder = 0;

  ant_value_t encoding = nargs > 0 && !is_undefined(args[0]) ? args[0] : js_mkstr(js, "utf8", 4);
  ant_value_t encoding_str = 0;

  if (is_err(stream_obj)) return stream_obj;
  state = stream_readable_state(js, stream_obj);
  if (!is_object_type(state)) return stream_obj;

  decoder = string_decoder_create(js, encoding);
  if (is_err(decoder)) return decoder;
  encoding_str = js_tostring_val(js, encoding);
  if (is_err(encoding_str)) return encoding_str;

  js_set(js, state, "decoder", decoder);
  js_set(js, stream_obj, "encoding", encoding_str);
  js_set(js, stream_obj, "readableEncoding", encoding_str);
  
  return stream_obj;
}

static ant_value_t js_readable_on(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t key = 0;
  ant_value_t state = 0;

  if (is_err(stream_obj)) return stream_obj;
  if (nargs < 2) return js_mkerr(js, "on requires 2 arguments (event, listener)");
  key = stream_event_key(js, args[0]);
  
  if (is_err(key)) return key;
  if (!eventemitter_add_listener_val(js, stream_obj, key, args[1], false))
    return js_mkerr(js, "listener must be a function");
    
  if (stream_key_is_cstr(js, key, "data")) {
    state = stream_readable_state(js, stream_obj);
    if (is_object_type(state)) js_set(js, state, "flowing", js_true);
    stream_schedule_microtask(js, stream_readable_start_flowing, stream_obj);
  }

  return stream_obj;
}

static ant_value_t js_readable_resume(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t state = 0;
  if (is_err(stream_obj)) return stream_obj;

  state = stream_readable_state(js, stream_obj);
  if (is_object_type(state)) js_set(js, state, "flowing", js_true);
  
  js_stream_resume(js, NULL, 0);
  stream_readable_schedule_flowing_tick(js, stream_obj);
  
  return stream_obj;
}

static ant_value_t js_readable_pause(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t state = 0;
  if (is_err(stream_obj)) return stream_obj;

  state = stream_readable_state(js, stream_obj);
  if (is_object_type(state)) js_set(js, state, "flowing", js_false);
  return js_stream_pause(js, args, nargs);
}

static ant_value_t js_writable__write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t callback = nargs > 2 ? args[2] : js_mkundef();
  stream_call_callback(js, callback, NULL, 0);
  return js_mkundef();
}

static ant_value_t js_writable__final(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  stream_call_callback(js, callback, NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_writable_write_done(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_get(js, state_obj, "stream");
  ant_value_t callback = js_get(js, state_obj, "callback");
  
  stream_private_state_t *priv = stream_private_state(stream_obj);
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  if (priv) priv->writing = false;

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };
    js_set(js, state_obj, "done", js_true);
    if (priv) priv->pending_final = false; {
      ant_value_t saved_this = js->this_val;
      js->this_val = stream_obj;
      js_stream_destroy(js, destroy_args, 1);
      js->this_val = saved_this;
    }
    
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    return js_mkundef();
  }

  if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);
  stream_emit_named(js, stream_obj, "drain");
  
  if (priv && priv->pending_final && !priv->final_started) {
    ant_value_t end_callback = js_get_slot(stream_obj, SLOT_AUX);
    priv->pending_final = false;
    stream_set_end_callback(js, stream_obj, js_mkundef());
    return stream_writable_begin_end(js, stream_obj, end_callback);
  }
  
  return js_mkundef();
}

static ant_value_t stream_writable_write_impl(
  ant_t *js,
  ant_value_t stream_obj,
  ant_value_t chunk,
  ant_value_t encoding,
  ant_value_t callback,
  bool allow_after_end
) {
  ant_value_t state = 0;
  ant_value_t normalized = 0;
  ant_value_t write_fn = 0;
  ant_value_t done_state = 0;
  ant_value_t done = 0;
  ant_value_t write_args[3];

  state = stream_writable_state(js, stream_obj);
  if (!is_object_type(state)) return js_false;

  if (
    (!allow_after_end && js_truthy(js, js_get(js, stream_obj, "writableEnded"))) ||
    js_truthy(js, js_get(js, stream_obj, "destroyed"))
  ) {
    ant_value_t err = js_mkerr(js, "write after end");
    stream_set_errored(js, stream_obj, err);
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    else stream_emit_error(js, stream_obj, err);
    return js_false;
  }

  normalized = stream_normalize_chunk(
    js, chunk,
    js_truthy(js, js_get(js, state, "objectMode")),
    encoding
  );
  
  if (is_err(normalized)) return normalized;
  done_state = js_mkobj(js);
  
  js_set(js, done_state, "stream", stream_obj);
  js_set(js, done_state, "callback", callback);
  js_set(js, done_state, "done", js_false);
  
  done = stream_make_once(js, js_heavy_mkfun(js, stream_writable_write_done, done_state), js_mkundef());
  write_fn = js_getprop_fallback(js, stream_obj, "_write");
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (priv) priv->writing = true;

  write_args[0] = normalized;
  write_args[1] = encoding;
  write_args[2] = done;
  
  if (is_callable(write_fn)) {
  ant_value_t result = stream_call(js, write_fn, stream_obj, write_args, 3, false);
  if (is_err(result)) {
    ant_value_t err_args[1] = { result };
    stream_call_callback(js, done, err_args, 1);
    return js_false;
  }}

  return js_bool(!js_truthy(js, js_get(js, stream_obj, "destroyed")));
}

static ant_value_t js_writable_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Writable");
  ant_value_t callback = js_mkundef();
  ant_value_t encoding = js_mkstr(js, "utf8", 4);

  if (is_err(stream_obj)) return stream_obj;

  if (nargs > 1 && is_callable(args[1])) callback = args[1];
  else if (nargs > 1 && !is_undefined(args[1])) encoding = args[1];
  if (nargs > 2 && is_callable(args[2])) callback = args[2];

  return stream_writable_write_impl(
    js, stream_obj,
    nargs > 0 ? args[0] : js_mkundef(),
    encoding, callback, false
  );
}

static ant_value_t stream_writable_end_done(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_get(js, state_obj, "stream");
  ant_value_t callback = js_get(js, state_obj, "callback");
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };
    ant_value_t saved_this = js->this_val;
    js->this_val = stream_obj;
    js_stream_destroy(js, destroy_args, 1);
    js->this_val = saved_this;
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    return js_mkundef();
  }

  js_set(js, stream_obj, "writableFinished", js_true);
  js_set(js, stream_writable_state(js, stream_obj), "finished", js_true);
  stream_emit_named(js, stream_obj, "finish");
  
  if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);
  if (!js_truthy(js, js_get(js, stream_obj, "readable"))) stream_emit_named(js, stream_obj, "close");
  
  return js_mkundef();
}

ant_value_t stream_writable_begin_end(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  ant_value_t final_fn = 0;
  ant_value_t final_args[1];
  ant_value_t done_state = 0;
  ant_value_t done = 0;
  stream_private_state_t *priv = stream_private_state(stream_obj);

  done_state = js_mkobj(js);
  js_set(js, done_state, "stream", stream_obj);
  js_set(js, done_state, "callback", callback);
  
  done = stream_make_once(js, js_heavy_mkfun(js, stream_writable_end_done, done_state), js_mkundef());
  if (priv) {
    priv->final_started = true;
    priv->pending_final = false;
  }
  
  stream_set_end_callback(js, stream_obj, js_mkundef());
  final_fn = js_getprop_fallback(js, stream_obj, "_final");
  final_args[0] = done;
  
  if (is_callable(final_fn)) stream_call(js, final_fn, stream_obj, final_args, 1, false);
  else stream_call_callback(js, done, NULL, 0);

  return stream_obj;
}

static ant_value_t stream_writable_end_after_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_get(js, state_obj, "stream");
  ant_value_t callback = js_get(js, state_obj, "callback");
  stream_private_state_t *priv = stream_private_state(stream_obj);
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };
    ant_value_t saved_this = js->this_val;
    js->this_val = stream_obj;
    js_stream_destroy(js, destroy_args, 1);
    js->this_val = saved_this;
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    return js_mkundef();
  }

  if (priv) priv->pending_final = false;
  stream_set_end_callback(js, stream_obj, js_mkundef());

  return stream_writable_begin_end(js, stream_obj, callback);
}

static ant_value_t js_writable_end(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Writable");
  ant_value_t callback = js_mkundef();
  ant_value_t chunk = js_mkundef();
  ant_value_t encoding = js_mkundef();
  ant_value_t after_write_state = 0;
  ant_value_t after_write = 0;
  stream_private_state_t *priv = NULL;

  if (is_err(stream_obj)) return stream_obj;
  if (nargs > 0 && is_callable(args[0])) callback = args[0];
  else {
    if (nargs > 0) chunk = args[0];
    if (nargs > 1 && is_callable(args[1])) callback = args[1];
    else if (nargs > 1) encoding = args[1];
    if (nargs > 2 && is_callable(args[2])) callback = args[2];
  }

  if (js_truthy(js, js_get(js, stream_obj, "writableEnded"))) {
    if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);
    return stream_obj;
  }

  js_set(js, stream_obj, "writableEnded", js_true);
  js_set(js, stream_writable_state(js, stream_obj), "ended", js_true);
  priv = stream_private_state(stream_obj);

  if (!is_undefined(chunk) && !is_null(chunk)) {
    after_write_state = js_mkobj(js);
    js_set(js, after_write_state, "stream", stream_obj);
    js_set(js, after_write_state, "callback", callback);
    
    after_write = stream_make_once(
      js, js_heavy_mkfun(js, stream_writable_end_after_write, after_write_state),
      js_mkundef()
    );
    
    stream_writable_write_impl(js, stream_obj, chunk, encoding, after_write, true);
    return stream_obj;
  }

  if (priv && priv->writing && !priv->final_started) {
    priv->pending_final = true;
    stream_set_end_callback(js, stream_obj, callback);
    return stream_obj;
  }

  return stream_writable_begin_end(js, stream_obj, callback);
}

static ant_value_t js_transform__transform(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t callback = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t cb_args[2];
  cb_args[0] = js_mknull();
  cb_args[1] = nargs > 0 ? args[0] : js_mkundef();
  stream_call_callback(js, callback, cb_args, 2);
  return js_mkundef();
}

static ant_value_t stream_transform_write_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_get(js, state_obj, "stream");
  ant_value_t outer_callback = js_get(js, state_obj, "callback");

  if (nargs > 0 && !is_null(args[0]) && !is_undefined(args[0])) {
    if (is_callable(outer_callback)) stream_call(js, outer_callback, stream_obj, &args[0], 1, false);
    return js_mkundef();
  }

  if (nargs > 1 && !is_null(args[1]) && !is_undefined(args[1]))
    stream_readable_push_value(js, stream_obj, args[1], js_mkundef());

  if (is_callable(outer_callback)) stream_call(js, outer_callback, stream_obj, NULL, 0, false);
  return js_mkundef();
}

static ant_value_t js_transform__write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Transform");
  ant_value_t transform_fn = 0;
  ant_value_t cb_state = 0;
  ant_value_t cb = 0;
  ant_value_t call_args[3];

  if (is_err(stream_obj)) return stream_obj;
  transform_fn = js_getprop_fallback(js, stream_obj, "_transform");

  cb_state = js_mkobj(js);
  js_set(js, cb_state, "stream", stream_obj);
  js_set(js, cb_state, "callback", nargs > 2 ? args[2] : js_mkundef());
  cb = js_heavy_mkfun(js, stream_transform_write_callback, cb_state);

  call_args[0] = nargs > 0 ? args[0] : js_mkundef();
  call_args[1] = nargs > 1 ? args[1] : js_mkstr(js, "utf8", 4);
  call_args[2] = cb;

  return stream_call(js, transform_fn, stream_obj, call_args, 3, false);
}

static ant_value_t stream_transform_final_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_get(js, state_obj, "stream");
  ant_value_t callback = js_get(js, state_obj, "callback");

  if (nargs > 0 && !is_null(args[0]) && !is_undefined(args[0])) {
    if (is_callable(callback)) stream_call(js, callback, stream_obj, &args[0], 1, false);
    return js_mkundef();
  }

  if (nargs > 1 && !is_null(args[1]) && !is_undefined(args[1]))
    stream_readable_push_value(js, stream_obj, args[1], js_mkundef());
  stream_readable_push_value(js, stream_obj, js_mknull(), js_mkundef());
  if (is_callable(callback)) stream_call(js, callback, stream_obj, NULL, 0, false);
  
  return js_mkundef();
}

static ant_value_t js_transform__final(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Transform");
  ant_value_t flush_fn = 0;
  ant_value_t cb_state = 0;
  ant_value_t cb = 0;
  ant_value_t call_args[1];

  if (is_err(stream_obj)) return stream_obj;
  flush_fn = js_getprop_fallback(js, stream_obj, "_flush");
  
  if (!is_callable(flush_fn)) {
    stream_readable_push_value(js, stream_obj, js_mknull(), js_mkundef());
    stream_call_callback(js, nargs > 0 ? args[0] : js_mkundef(), NULL, 0);
    return js_mkundef();
  }

  cb_state = js_mkobj(js);
  js_set(js, cb_state, "stream", stream_obj);
  js_set(js, cb_state, "callback", nargs > 0 ? args[0] : js_mkundef());
  cb = js_heavy_mkfun(js, stream_transform_final_callback, cb_state);
  call_args[0] = cb;
  return stream_call(js, flush_fn, stream_obj, call_args, 1, false);
}

static ant_value_t js_passthrough__transform(ant_t *js, ant_value_t *args, int nargs) {
  return js_transform__transform(js, args, nargs);
}

static ant_value_t stream_finished_cleanup(ant_t *js, ant_value_t state_obj) {
  ant_value_t stream_obj = js_get(js, state_obj, "stream");
  if (stream_is_instance(stream_obj) || is_object_type(stream_obj)) {
    stream_remove_listener(js, stream_obj, "end", js_get(js, state_obj, "onFinish"));
    stream_remove_listener(js, stream_obj, "finish", js_get(js, state_obj, "onFinish"));
    stream_remove_listener(js, stream_obj, "close", js_get(js, state_obj, "onFinish"));
    stream_remove_listener(js, stream_obj, "error", js_get(js, state_obj, "onError"));
  }
  return js_mkundef();
}

static ant_value_t stream_finished_fire(ant_t *js, ant_value_t state_obj, ant_value_t error) {
  ant_value_t called = js_get(js, state_obj, "called");
  ant_value_t callback = js_get(js, state_obj, "callback");
  ant_value_t cb_args[1];

  if (js_truthy(js, called)) return js_mkundef();
  js_set(js, state_obj, "called", js_true);
  stream_finished_cleanup(js, state_obj);

  if (is_undefined(error)) stream_call_callback(js, callback, NULL, 0);
  else {
    cb_args[0] = error;
    stream_call_callback(js, callback, cb_args, 1);
  }
  return js_mkundef();
}

static ant_value_t stream_finished_on_finish(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  return stream_finished_fire(js, state_obj, js_mkundef());
}

static ant_value_t stream_finished_on_error(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();
  return stream_finished_fire(js, state_obj, error);
}

static ant_value_t stream_finished_register(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  ant_value_t state_obj = js_mkobj(js);
  ant_value_t on_finish = 0;
  ant_value_t on_error = 0;

  if (!is_callable(callback)) callback = js_mkfun(stream_noop);

  js_set(js, state_obj, "stream", stream_obj);
  js_set(js, state_obj, "callback", callback);
  js_set(js, state_obj, "called", js_false);
  on_finish = js_heavy_mkfun(js, stream_finished_on_finish, state_obj);
  on_error = js_heavy_mkfun(js, stream_finished_on_error, state_obj);
  js_set(js, state_obj, "onFinish", on_finish);
  js_set(js, state_obj, "onError", on_error);

  eventemitter_add_listener(js, stream_obj, "end", on_finish, false);
  eventemitter_add_listener(js, stream_obj, "finish", on_finish, false);
  eventemitter_add_listener(js, stream_obj, "close", on_finish, false);
  eventemitter_add_listener(js, stream_obj, "error", on_error, false);
  return stream_obj;
}

static ant_value_t js_stream_finished(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t callback = nargs > 1 ? args[1] : js_mkundef();
  if (nargs < 1 || !is_object_type(args[0])) return js_mkerr(js, "finished requires a stream");
  return stream_finished_register(js, args[0], callback);
}

static ant_value_t stream_pipeline_done(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t callback = js_get(js, state_obj, "callback");
  ant_value_t called = js_get(js, state_obj, "called");
  ant_value_t cb_args[1];

  if (js_truthy(js, called)) return js_mkundef();
  js_set(js, state_obj, "called", js_true);

  if (nargs > 0 && !is_undefined(args[0])) {
    cb_args[0] = args[0];
    stream_call_callback(js, callback, cb_args, 1);
  } else stream_call_callback(js, callback, NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_pipeline_error(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t done = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (nargs > 0 && !is_undefined(args[0])) stream_call_callback(js, done, &args[0], 1);
  return js_mkundef();
}

static ant_value_t stream_pipeline_schedule_done(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t done = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  stream_call_callback(js, done, NULL, 0);
  return js_mkundef();
}

static ant_value_t js_stream_pipeline(ant_t *js, ant_value_t *args, int nargs) {
  int stream_count = nargs;
  ant_value_t callback = js_mkundef();
  ant_value_t done_state = 0;
  ant_value_t done = 0;

  if (nargs > 0 && is_callable(args[nargs - 1])) {
    callback = args[nargs - 1];
    stream_count--;
  }
  
  if (!is_callable(callback)) callback = js_mkfun(stream_noop);
  if (stream_count <= 0) return js_mkundef();

  done_state = js_mkobj(js);
  js_set(js, done_state, "callback", callback);
  js_set(js, done_state, "called", js_false);
  done = js_heavy_mkfun(js, stream_pipeline_done, done_state);

  if (stream_count < 2) {
    stream_schedule_microtask(js, stream_pipeline_schedule_done, done);
    return args[0];
  }

  for (int i = 0; i < stream_count - 1; i++) {
    ant_value_t error_cb = js_heavy_mkfun(js, stream_pipeline_error, done);
    ant_value_t finished_args[2];
    
    finished_args[0] = args[i];
    finished_args[1] = error_cb;
    js_stream_finished(js, finished_args, 2);
    
    ant_value_t pipe_args[2];
    pipe_args[0] = args[i + 1];
    pipe_args[1] = js_mkundef();
    stream_call_prop(js, args[i], "pipe", pipe_args, 2);
  }

  {
    ant_value_t finished_args[2];
    finished_args[0] = args[stream_count - 1];
    finished_args[1] = done;
    js_stream_finished(js, finished_args, 2);
  }

  return args[stream_count - 1];
}

static ant_value_t stream_promise_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (nargs > 0 && !is_undefined(args[0])) js_reject_promise(js, promise, args[0]);
  else js_resolve_promise(js, promise, js_mkundef());
  return js_mkundef();
}

static ant_value_t js_stream_promises_finished(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t finished_args[2];
  if (nargs < 1 || !is_object_type(args[0])) {
    js_reject_promise(js, promise, js_mkerr(js, "finished requires a stream"));
    return promise;
  }
  finished_args[0] = args[0];
  finished_args[1] = js_heavy_mkfun(js, stream_promise_callback, promise);
  js_stream_finished(js, finished_args, 2);
  return promise;
}

static ant_value_t js_stream_promises_pipeline(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t *call_args = NULL;

  if (nargs <= 0) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  call_args = malloc((size_t)(nargs + 1) * sizeof(*call_args));
  if (!call_args) {
    js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
    return promise;
  }

  for (int i = 0; i < nargs; i++) call_args[i] = args[i];
  call_args[nargs] = js_heavy_mkfun(js, stream_promise_callback, promise);
  js_stream_pipeline(js, call_args, nargs + 1);
  free(call_args);
  return promise;
}

static void stream_release_reader(ant_t *js, ant_value_t state_obj) {
  ant_value_t reader = js_get(js, state_obj, "reader");
  if (!is_object_type(reader)) return;
  stream_call_prop(js, reader, "releaseLock", NULL, 0);
}

static ant_value_t stream_readable_from_step(ant_t *js, ant_value_t *args, int nargs);

static void stream_readable_from_schedule(ant_t *js, ant_value_t state_obj) {
  stream_schedule_microtask(js, stream_readable_from_step, state_obj);
}

static ant_value_t stream_readable_from_fail(ant_t *js, ant_value_t state_obj, ant_value_t error) {
  ant_value_t readable = js_get(js, state_obj, "readable");
  stream_release_reader(js, state_obj);
  if (stream_is_instance(readable)) {
    ant_value_t destroy_args[1] = { error };
    ant_value_t saved_this = js->this_val;
    js->this_val = readable;
    js_stream_destroy(js, destroy_args, 1);
    js->this_val = saved_this;
  }
  return js_mkundef();
}

static ant_value_t stream_readable_from_handle_result(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t readable = js_get(js, state_obj, "readable");
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t done = 0;
  ant_value_t value = 0;

  if (js_truthy(js, js_get(js, readable, "destroyed"))) {
    stream_release_reader(js, state_obj);
    return js_mkundef();
  }

  if (!is_object_type(result)) return stream_readable_from_fail(js, state_obj, js_mkerr(js, "iterator step must be an object"));
  done = js_get(js, result, "done");
  value = js_get(js, result, "value");
  if (js_truthy(js, done)) {
    stream_release_reader(js, state_obj);
    stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
    return js_mkundef();
  }

  stream_readable_push_value(js, readable, value, js_mkundef());
  if (!js_truthy(js, js_get(js, readable, "destroyed"))) stream_readable_from_schedule(js, state_obj);
  return js_mkundef();
}

static ant_value_t stream_readable_from_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t error = nargs > 0 ? args[0] : js_mkerr(js, "stream iteration failed");
  return stream_readable_from_fail(js, state_obj, error);
}

static ant_value_t stream_readable_from_step(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t readable = js_get(js, state_obj, "readable");
  ant_value_t mode = js_get(js, state_obj, "mode");
  ant_value_t iterator = js_get(js, state_obj, "iterator");
  
  ant_value_t next_result = js_mkundef();
  ant_value_t on_resolve = js_heavy_mkfun(js, stream_readable_from_handle_result, state_obj);
  ant_value_t on_reject = js_heavy_mkfun(js, stream_readable_from_reject, state_obj);
  ant_value_t then_result = 0;

  if (js_truthy(js, js_get(js, readable, "destroyed"))) {
    stream_release_reader(js, state_obj);
    return js_mkundef();
  }

  if (stream_key_is_cstr(js, mode, "reader")) next_result = stream_call_prop(js, iterator, "read", NULL, 0);
  else next_result = stream_call_prop(js, iterator, "next", NULL, 0);

  if (is_err(next_result)) return stream_readable_from_fail(js, state_obj, next_result);
  if (vtype(next_result) == T_PROMISE) {
    then_result = js_promise_then(js, next_result, on_resolve, on_reject);
    promise_mark_handled(then_result);
    return js_mkundef();
  }

  ant_value_t one_arg[1] = { next_result };
  return stream_readable_from_handle_result(js, one_arg, 1);
}

static ant_value_t stream_readable_from_start(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t readable = js_get(js, state_obj, "readable");
  ant_value_t source = js_get(js, state_obj, "source");
  
  ant_value_t async_iter_fn = 0;
  ant_value_t reader_fn = 0;
  js_iter_t it;

  if (js_truthy(js, js_get(js, readable, "destroyed"))) return js_mkundef();

  async_iter_fn = is_object_type(source) ? js_get_sym(js, source, get_asyncIterator_sym()) : js_mkundef();
  if (is_callable(async_iter_fn)) {
    ant_value_t iterator = stream_call(js, async_iter_fn, source, NULL, 0, false);
    if (is_err(iterator)) return stream_readable_from_fail(js, state_obj, iterator);
    js_set(js, state_obj, "iterator", iterator);
    js_set(js, state_obj, "mode", js_mkstr(js, "async", 5));
    stream_readable_from_schedule(js, state_obj);
    return js_mkundef();
  }

  if (js_iter_open(js, source, &it)) {
    ant_value_t value = 0;
    while (js_iter_next(js, &it, &value)) {
      if (js_truthy(js, js_get(js, readable, "destroyed"))) break;
      stream_readable_push_value(js, readable, value, js_mkundef());
    }
    js_iter_close(js, &it);
    if (!js_truthy(js, js_get(js, readable, "destroyed")))
      stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
    return js_mkundef();
  }

  reader_fn = is_object_type(source) ? js_get(js, source, "getReader") : js_mkundef();
  if (is_callable(reader_fn)) {
    ant_value_t reader = stream_call(js, reader_fn, source, NULL, 0, false);
    if (is_err(reader)) return stream_readable_from_fail(js, state_obj, reader);
    js_set(js, state_obj, "reader", reader);
    js_set(js, state_obj, "iterator", reader);
    js_set(js, state_obj, "mode", js_mkstr(js, "reader", 6));
    stream_readable_from_schedule(js, state_obj);
    return js_mkundef();
  }

  if (!is_undefined(source)) stream_readable_push_value(js, readable, source, js_mkundef());
  stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
  
  return js_mkundef();
}

static ant_value_t js_readable_from(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctor_args[1];
  ant_value_t readable = 0;
  ant_value_t state_obj = 0;

  ctor_args[0] = nargs > 1 ? args[1] : js_mkundef();
  readable = stream_construct(js, js->sym.stream_readable_proto, ctor_args[0], stream_init_readable);
  if (is_err(readable)) return readable;

  state_obj = js_mkobj(js);
  js_set(js, state_obj, "readable", readable);
  js_set(js, state_obj, "source", nargs > 0 ? args[0] : js_mkundef());
  js_set(js, state_obj, "iterator", js_mkundef());
  js_set(js, state_obj, "reader", js_mkundef());
  js_set(js, state_obj, "mode", js_mkundef());
  stream_schedule_microtask(js, stream_readable_from_start, state_obj);
  
  return readable;
}

static ant_value_t js_readable_from_web(ant_t *js, ant_value_t *args, int nargs) {
  return js_readable_from(js, args, nargs);
}

static ant_value_t js_stream_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return stream_construct(js, js->sym.stream_proto, nargs > 0 ? args[0] : js_mkundef(), stream_init_base);
}

static ant_value_t js_readable_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return stream_construct(js, js->sym.stream_readable_proto, nargs > 0 ? args[0] : js_mkundef(), stream_init_readable);
}

static ant_value_t js_writable_ctor(ant_t *js, ant_value_t *args, int nargs) {
  return stream_construct(js, js->sym.stream_writable_proto, nargs > 0 ? args[0] : js_mkundef(), stream_init_writable);
}

static ant_value_t js_duplex_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.stream_duplex_proto);
  ant_value_t obj = stream_make_base_object(js, is_object_type(proto) ? proto : js->sym.stream_duplex_proto);
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t options_obj = is_object_type(options) ? options : js_mkobj(js);

  stream_init_readable(js, obj, options);
  stream_init_writable(js, obj, options);
  
  js_set(js, obj, "readable", js_true);
  js_set(js, obj, "writable", js_true);
  js_set(js, obj, "allowHalfOpen", js_bool(stream_get_option(js, options_obj, "allowHalfOpen") != js_false));
  
  return obj;
}

static ant_value_t js_transform_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.stream_transform_proto);
  ant_value_t obj = stream_make_base_object(js, is_object_type(proto) ? proto : js->sym.stream_transform_proto);
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t options_obj = is_object_type(options) ? options : js_mkobj(js);
  
  ant_value_t transform_fn = 0;
  ant_value_t flush_fn = 0;

  stream_init_readable(js, obj, options);
  stream_init_writable(js, obj, options);
  
  js_set(js, obj, "readable", js_true);
  js_set(js, obj, "writable", js_true);
  js_set(js, obj, "allowHalfOpen", js_bool(stream_get_option(js, options_obj, "allowHalfOpen") != js_false));

  transform_fn = stream_get_option(js, options_obj, "transform");
  flush_fn = stream_get_option(js, options_obj, "flush");
  
  if (is_callable(transform_fn)) js_set(js, obj, "_transform", transform_fn);
  if (is_callable(flush_fn)) js_set(js, obj, "_flush", flush_fn);
  
  return obj;
}

static ant_value_t js_passthrough_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.stream_passthrough_proto);
  ant_value_t obj = stream_make_base_object(js, is_object_type(proto) ? proto : js->sym.stream_passthrough_proto);
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t options_obj = is_object_type(options) ? options : js_mkobj(js);

  stream_init_readable(js, obj, options);
  stream_init_writable(js, obj, options);
  
  js_set(js, obj, "readable", js_true);
  js_set(js, obj, "writable", js_true);
  js_set(js, obj, "allowHalfOpen", js_bool(stream_get_option(js, options_obj, "allowHalfOpen") != js_false));
  
  return obj;
}

void stream_init_constructors(ant_t *js) {
  ant_value_t events = 0;
  ant_value_t ee_ctor = 0;
  ant_value_t ee_proto = 0;

  if (js->sym.stream_ctor) return;

  events = events_library(js);
  ee_ctor = js_get(js, events, "EventEmitter");
  ee_proto = js_get(js, ee_ctor, "prototype");

  js->sym.stream_proto = js_mkobj(js);
  js_set_proto_init(js->sym.stream_proto, ee_proto);
  js_set(js, js->sym.stream_proto, "pipe", js_mkfun(js_stream_pipe));
  js_set(js, js->sym.stream_proto, "unpipe", js_mkfun(js_stream_unpipe));
  js_set(js, js->sym.stream_proto, "pause", js_mkfun(js_stream_pause));
  js_set(js, js->sym.stream_proto, "resume", js_mkfun(js_stream_resume));
  js_set(js, js->sym.stream_proto, "isPaused", js_mkfun(js_stream_is_paused));
  js_set(js, js->sym.stream_proto, "destroy", js_mkfun(js_stream_destroy));
  js_set_sym(js, js->sym.stream_proto, get_toStringTag_sym(), js_mkstr(js, "Stream", 6));
  js->sym.stream_ctor = js_make_ctor(js, js_stream_ctor, js->sym.stream_proto, "Stream", 6);

  js->sym.stream_readable_proto = js_mkobj(js);
  js_set_proto_init(js->sym.stream_readable_proto, js->sym.stream_proto);
  js_set(js, js->sym.stream_readable_proto, "_read", js_mkfun(js_readable__read));
  js_set(js, js->sym.stream_readable_proto, "push", js_mkfun(js_readable_push));
  js_set(js, js->sym.stream_readable_proto, "read", js_mkfun(js_readable_read));
  js_set(js, js->sym.stream_readable_proto, "setEncoding", js_mkfun(js_readable_set_encoding));
  js_set(js, js->sym.stream_readable_proto, "on", js_mkfun(js_readable_on));
  js_set(js, js->sym.stream_readable_proto, "resume", js_mkfun(js_readable_resume));
  js_set(js, js->sym.stream_readable_proto, "pause", js_mkfun(js_readable_pause));
  js_set_sym(js, js->sym.stream_readable_proto, get_toStringTag_sym(), js_mkstr(js, "Readable", 8));
  js->sym.stream_readable_ctor = js_make_ctor(js, js_readable_ctor, js->sym.stream_readable_proto, "Readable", 8);
  js_set(js, js->sym.stream_readable_ctor, "from", js_mkfun(js_readable_from));
  js_set(js, js->sym.stream_readable_ctor, "fromWeb", js_mkfun(js_readable_from_web));

  js->sym.stream_writable_proto = js_mkobj(js);
  js_set_proto_init(js->sym.stream_writable_proto, js->sym.stream_proto);
  js_set(js, js->sym.stream_writable_proto, "_write", js_mkfun(js_writable__write));
  js_set(js, js->sym.stream_writable_proto, "_final", js_mkfun(js_writable__final));
  js_set(js, js->sym.stream_writable_proto, "write", js_mkfun(js_writable_write));
  js_set(js, js->sym.stream_writable_proto, "end", js_mkfun(js_writable_end));
  js_set(js, js->sym.stream_writable_proto, "cork", js_mkfun(stream_noop));
  js_set(js, js->sym.stream_writable_proto, "uncork", js_mkfun(stream_noop));
  js_set_sym(js, js->sym.stream_writable_proto, get_toStringTag_sym(), js_mkstr(js, "Writable", 8));
  js->sym.stream_writable_ctor = js_make_ctor(js, js_writable_ctor, js->sym.stream_writable_proto, "Writable", 8);

  js->sym.stream_duplex_proto = js_mkobj(js);
  js_set_proto_init(js->sym.stream_duplex_proto, js->sym.stream_readable_proto);
  js_set(js, js->sym.stream_duplex_proto, "_write", js_mkfun(js_writable__write));
  js_set(js, js->sym.stream_duplex_proto, "_final", js_mkfun(js_writable__final));
  js_set(js, js->sym.stream_duplex_proto, "write", js_mkfun(js_writable_write));
  js_set(js, js->sym.stream_duplex_proto, "end", js_mkfun(js_writable_end));
  js_set(js, js->sym.stream_duplex_proto, "cork", js_mkfun(stream_noop));
  js_set(js, js->sym.stream_duplex_proto, "uncork", js_mkfun(stream_noop));
  js_set_sym(js, js->sym.stream_duplex_proto, get_toStringTag_sym(), js_mkstr(js, "Duplex", 6));
  js->sym.stream_duplex_ctor = js_make_ctor(js, js_duplex_ctor, js->sym.stream_duplex_proto, "Duplex", 6);

  js->sym.stream_transform_proto = js_mkobj(js);
  js_set_proto_init(js->sym.stream_transform_proto, js->sym.stream_duplex_proto);
  js_set(js, js->sym.stream_transform_proto, "_transform", js_mkfun(js_transform__transform));
  js_set(js, js->sym.stream_transform_proto, "_write", js_mkfun(js_transform__write));
  js_set(js, js->sym.stream_transform_proto, "_final", js_mkfun(js_transform__final));
  js_set_sym(js, js->sym.stream_transform_proto, get_toStringTag_sym(), js_mkstr(js, "Transform", 9));
  js->sym.stream_transform_ctor = js_make_ctor(js, js_transform_ctor, js->sym.stream_transform_proto, "Transform", 9);

  js->sym.stream_passthrough_proto = js_mkobj(js);
  js_set_proto_init(js->sym.stream_passthrough_proto, js->sym.stream_transform_proto);
  js_set(js, js->sym.stream_passthrough_proto, "_transform", js_mkfun(js_passthrough__transform));
  js_set_sym(js, js->sym.stream_passthrough_proto, get_toStringTag_sym(), js_mkstr(js, "PassThrough", 11));
  js->sym.stream_passthrough_ctor = js_make_ctor(js, js_passthrough_ctor, js->sym.stream_passthrough_proto, "PassThrough", 11);

  gc_register_root(&js->sym.stream_proto);
  gc_register_root(&js->sym.stream_ctor);
  gc_register_root(&js->sym.stream_readable_proto);
  gc_register_root(&js->sym.stream_readable_ctor);
  gc_register_root(&js->sym.stream_writable_proto);
  gc_register_root(&js->sym.stream_writable_ctor);
  gc_register_root(&js->sym.stream_duplex_proto);
  gc_register_root(&js->sym.stream_duplex_ctor);
  gc_register_root(&js->sym.stream_transform_proto);
  gc_register_root(&js->sym.stream_transform_ctor);
  gc_register_root(&js->sym.stream_passthrough_proto);
  gc_register_root(&js->sym.stream_passthrough_ctor);
}

ant_value_t stream_readable_constructor(ant_t *js) {
  stream_init_constructors(js);
  return js->sym.stream_readable_ctor;
}

ant_value_t stream_writable_constructor(ant_t *js) {
  stream_init_constructors(js);
  return js->sym.stream_writable_ctor;
}

ant_value_t stream_readable_prototype(ant_t *js) {
  stream_init_constructors(js);
  return js->sym.stream_readable_proto;
}

ant_value_t stream_writable_prototype(ant_t *js) {
  stream_init_constructors(js);
  return js->sym.stream_writable_proto;
}

ant_value_t stream_duplex_prototype(ant_t *js) {
  stream_init_constructors(js);
  return js->sym.stream_duplex_proto;
}

ant_value_t stream_construct_readable(ant_t *js, ant_value_t base_proto, ant_value_t options) {
  stream_init_constructors(js);
  return stream_construct(js, base_proto, options, stream_init_readable);
}

ant_value_t stream_construct_writable(ant_t *js, ant_value_t base_proto, ant_value_t options) {
  stream_init_constructors(js);
  return stream_construct(js, base_proto, options, stream_init_writable);
}

void stream_init_readable_object(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_constructors(js);
  if (!is_object_type(obj)) return;
  js_set_native(obj, NULL, STREAM_NATIVE_TAG);
  stream_init_readable(js, obj, options);
}

void stream_init_writable_object(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_constructors(js);
  if (!is_object_type(obj)) return;
  js_set_native(obj, NULL, STREAM_NATIVE_TAG);
  stream_init_writable(js, obj, options);
}

void stream_init_duplex_object(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_constructors(js);
  if (!is_object_type(obj)) return;
  js_set_native(obj, NULL, STREAM_NATIVE_TAG);
  stream_init_readable(js, obj, options);
  stream_init_writable(js, obj, options);
}

ant_value_t stream_readable_push(ant_t *js, ant_value_t stream_obj, ant_value_t chunk, ant_value_t encoding) {
  stream_init_constructors(js);
  return stream_readable_push_value(js, stream_obj, chunk, encoding);
}

static ant_value_t js_stream_get_default_high_water_mark(ant_t *js, ant_value_t *args, int nargs) {
  bool object_mode = nargs > 0 && js_truthy(js, args[0]);
  return js_mknum(stream_default_high_water_mark(object_mode));
}

static ant_value_t js_stream_set_default_high_water_mark(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || vtype(args[1]) != T_NUM || js_getnum(args[1]) < 0)
    return js_mkerr_typed(js, JS_ERR_RANGE, "setDefaultHighWaterMark requires a non-negative number");

  bool object_mode = js_truthy(js, args[0]);
  if (object_mode) g_default_object_high_water_mark = js_getnum(args[1]);
  else g_default_high_water_mark = js_getnum(args[1]);

  return js_mkundef();
}

static ant_value_t js_stream_is_destroyed(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(stream_obj)) return js_false;
  return js_bool(js_truthy(js, js_get(js, stream_obj, "destroyed")));
}

static ant_value_t js_stream_is_disturbed(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t state = 0;

  if (!is_object_type(stream_obj)) return js_false;
  state = stream_readable_state(js, stream_obj);
  if (is_object_type(state)) {
    if (js_truthy(js, js_get(js, state, "dataEmitted"))) return js_true;
  }
  return js_bool(js_truthy(js, js_get(js, stream_obj, "destroyed")));
}

static ant_value_t js_stream_is_errored(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t state = 0;

  if (!is_object_type(stream_obj)) return js_false;
  state = stream_readable_state(js, stream_obj);
  if (is_object_type(state) && js_truthy(js, js_get(js, state, "errored"))) return js_true;
  state = stream_writable_state(js, stream_obj);
  if (is_object_type(state) && js_truthy(js, js_get(js, state, "errored"))) return js_true;
  return js_false;
}

static ant_value_t js_stream_is_readable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t state = 0;

  if (!is_object_type(stream_obj)) return js_false;
  if (js_truthy(js, js_get(js, stream_obj, "destroyed"))) return js_false;
  if (js_truthy(js, js_get(js, stream_obj, "readableEnded"))) return js_false;
  state = stream_readable_state(js, stream_obj);
  
  if (is_object_type(state)) {
    if (js_truthy(js, js_get(js, state, "ended"))) return js_false;
    if (js_truthy(js, js_get(js, state, "errored"))) return js_false;
  }
  
  state = stream_writable_state(js, stream_obj);
  if (is_object_type(state) && js_truthy(js, js_get(js, state, "errored"))) return js_false;
  return js_bool(js_truthy(js, js_get(js, stream_obj, "readable")));
}

static void stream_web_copy_global(ant_t *js, ant_value_t obj, const char *name) {
  ant_value_t value = js_get(js, js->global, name);
  if (is_err(value)) return;
  js_set(js, obj, name, value);
}

// TODO: remove copy-on-start
static void stream_web_define_common(ant_t *js, ant_value_t obj) {
  stream_web_copy_global(js, obj, "ReadableStream");
  stream_web_copy_global(js, obj, "ReadableStreamDefaultReader");
  stream_web_copy_global(js, obj, "ReadableStreamDefaultController");
  stream_web_copy_global(js, obj, "WritableStream");
  stream_web_copy_global(js, obj, "WritableStreamDefaultWriter");
  stream_web_copy_global(js, obj, "WritableStreamDefaultController");
  stream_web_copy_global(js, obj, "TransformStream");
  stream_web_copy_global(js, obj, "TransformStreamDefaultController");
  stream_web_copy_global(js, obj, "ByteLengthQueuingStrategy");
  stream_web_copy_global(js, obj, "CountQueuingStrategy");
  stream_web_copy_global(js, obj, "TextEncoderStream");
  stream_web_copy_global(js, obj, "TextDecoderStream");
  stream_web_copy_global(js, obj, "CompressionStream");
  stream_web_copy_global(js, obj, "DecompressionStream");
}

ant_value_t stream_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t promises = js_mkobj(js);
  stream_init_constructors(js);

  js_set(js, promises, "pipeline", js_mkfun(js_stream_promises_pipeline));
  js_set(js, promises, "finished", js_mkfun(js_stream_promises_finished));

  js_set_module_default(js, lib, js->sym.stream_ctor, "Stream");
  js_set(js, lib, "Readable", js->sym.stream_readable_ctor);
  js_set(js, lib, "Writable", js->sym.stream_writable_ctor);
  js_set(js, lib, "Duplex", js->sym.stream_duplex_ctor);
  js_set(js, lib, "Transform", js->sym.stream_transform_ctor);
  js_set(js, lib, "PassThrough", js->sym.stream_passthrough_ctor);
  js_set(js, lib, "pipeline", js_mkfun(js_stream_pipeline));
  js_set(js, lib, "finished", js_mkfun(js_stream_finished));
  js_set(js, lib, "getDefaultHighWaterMark", js_mkfun(js_stream_get_default_high_water_mark));
  js_set(js, lib, "setDefaultHighWaterMark", js_mkfun(js_stream_set_default_high_water_mark));
  js_set(js, lib, "isDestroyed", js_mkfun(js_stream_is_destroyed));
  js_set(js, lib, "isDisturbed", js_mkfun(js_stream_is_disturbed));
  js_set(js, lib, "isErrored", js_mkfun(js_stream_is_errored));
  js_set(js, lib, "isReadable", js_mkfun(js_stream_is_readable));
  js_set(js, lib, "promises", promises);

  js_set(js, js->sym.stream_ctor, "Readable", js->sym.stream_readable_ctor);
  js_set(js, js->sym.stream_ctor, "Writable", js->sym.stream_writable_ctor);
  js_set(js, js->sym.stream_ctor, "Duplex", js->sym.stream_duplex_ctor);
  js_set(js, js->sym.stream_ctor, "Transform", js->sym.stream_transform_ctor);
  js_set(js, js->sym.stream_ctor, "PassThrough", js->sym.stream_passthrough_ctor);
  js_set(js, js->sym.stream_ctor, "pipeline", js_get(js, lib, "pipeline"));
  js_set(js, js->sym.stream_ctor, "finished", js_get(js, lib, "finished"));
  js_set(js, js->sym.stream_ctor, "getDefaultHighWaterMark", js_get(js, lib, "getDefaultHighWaterMark"));
  js_set(js, js->sym.stream_ctor, "setDefaultHighWaterMark", js_get(js, lib, "setDefaultHighWaterMark"));
  js_set(js, js->sym.stream_ctor, "isDestroyed", js_get(js, lib, "isDestroyed"));
  js_set(js, js->sym.stream_ctor, "isDisturbed", js_get(js, lib, "isDisturbed"));
  js_set(js, js->sym.stream_ctor, "isErrored", js_get(js, lib, "isErrored"));
  js_set(js, js->sym.stream_ctor, "isReadable", js_get(js, lib, "isReadable"));
  js_set(js, js->sym.stream_ctor, "promises", promises);

  js_set(js, promises, "default", promises);
  js_set_slot_wb(js, promises, SLOT_DEFAULT, promises);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "stream", 6));
  
  return lib;
}

ant_value_t stream_promises_library(ant_t *js) {
  ant_value_t stream_ns = js_esm_import_sync_cstr(js, "stream", 6);
  ant_value_t promises = 0;
  
  if (is_err(stream_ns)) return stream_ns;
  promises = js_get(js, stream_ns, "promises");
  
  if (!is_object_type(promises)) return js_mkerr(js, "stream.promises is not available");
  js_set(js, promises, "default", promises);
  js_set_slot_wb(js, promises, SLOT_DEFAULT, promises);
  
  return promises;
}

ant_value_t stream_web_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  stream_web_define_common(js, lib);
  js_set(js, lib, "default", lib);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, lib);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "stream/web", 10));

  return lib;
}
