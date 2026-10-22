#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "descriptors.h"
#include "modules/timer.h"
#include "internal.h"
#include "silver/call.h"
#include "esm/loader.h"
#include "gc.h"
#include "gc/modules.h"
#include "gc/roots.h"

#include "modules/assert.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/stream.h"
#include "modules/iterator.h"
#include "modules/string_decoder.h"
#include "modules/symbol.h"
#include "streams/readable.h"

static constexpr uint32_t STREAM_NATIVE_TAG = Ant_NativeTag("STRM");
static constexpr uint32_t STREAM_READABLE_STATE_TAG = Ant_NativeTag("STRR");
static constexpr uint32_t STREAM_WRITABLE_STATE_TAG = Ant_NativeTag("STRW");

enum: int {
  STREAM_WRITE_ENTRY_CHUNK,
  STREAM_WRITE_ENTRY_ENCODING,
  STREAM_WRITE_ENTRY_CALLBACK,
  STREAM_WRITE_ENTRY_SIZE,
  STREAM_WRITE_ENTRY_STRIDE,
};

typedef struct {
  ant_object_t *obj;
  ant_value_t buffer;
  ant_value_t decoder;
  ant_value_t errored;
  double length;
  double high_water_mark;
  ant_offset_t buffer_head;
  bool object_mode;
  bool auto_destroy;
  bool ended;
  bool end_emitted;
  bool destroyed;
  bool end_scheduled;
  bool end_requested;
  bool readable_listening;
  bool readable_scheduled;
  bool data_emitted;
  bool flowing;
  bool flowing_read_scheduled;
  bool reading;
} stream_readable_state_t;

typedef struct {
  ant_object_t *obj;
  ant_value_t buffer;
  ant_value_t errored;
  double length;
  double high_water_mark;
  ant_offset_t buffer_head;
  bool object_mode;
  bool auto_destroy;
  bool finished;
  bool ended;
  bool need_drain;
} stream_writable_state_t;

static ant_value_t stream_noop(ant_params_t) {
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

static inline void stream_state_store(ant_t *js, ant_object_t *owner, ant_value_t *field, ant_value_t value) {
  *field = value;
  gc_write_barrier(js, owner, value);
}

static stream_readable_state_t *stream_rs_of_state(ant_value_t state) {
  return is_object_type(state) ? js_get_native(state, STREAM_READABLE_STATE_TAG) : NULL;
}

static stream_writable_state_t *stream_ws_of_state(ant_value_t state) {
  return is_object_type(state) ? js_get_native(state, STREAM_WRITABLE_STATE_TAG) : NULL;
}

static stream_readable_state_t *stream_rs(ant_value_t stream_obj) {
  if (!is_object_type(stream_obj)) return NULL;
  return stream_rs_of_state(js_get_slot(stream_obj, SLOT_STREAM_READABLE_STATE));
}

static stream_writable_state_t *stream_ws(ant_value_t stream_obj) {
  if (!is_object_type(stream_obj)) return NULL;
  return stream_ws_of_state(js_get_slot(stream_obj, SLOT_STREAM_WRITABLE_STATE));
}

static ant_value_t stream_state_field(ant_t *js, ant_value_t stream_obj, const char *state_key, const char *field) {
  ant_value_t state = js_get(js, stream_obj, state_key);
  return is_object_type(state) ? js_get(js, state, field) : js_mkundef();
}

static bool stream_is_destroyed(ant_t *js, ant_value_t stream_obj) {
  return js_truthy(js, js_get(js, stream_obj, "destroyed"));
}

static void stream_state_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t state = js_obj_from_ptr(obj);
  void *rs = js_get_native(state, STREAM_READABLE_STATE_TAG);
  void *ws = js_get_native(state, STREAM_WRITABLE_STATE_TAG);

  if (rs) js_clear_native(state, STREAM_READABLE_STATE_TAG);
  if (ws) js_clear_native(state, STREAM_WRITABLE_STATE_TAG);
  free(rs); free(ws);
}

void gc_mark_stream_state_object(ant_t *js, ant_value_t value, gc_mark_fn mark) {
  stream_readable_state_t *rs = js_get_native(value, STREAM_READABLE_STATE_TAG);
  if (rs) {
    mark(js, rs->buffer);
    mark(js, rs->decoder);
    mark(js, rs->errored);
  }

  stream_writable_state_t *ws = js_get_native(value, STREAM_WRITABLE_STATE_TAG);
  if (ws) {
    mark(js, ws->buffer);
    mark(js, ws->errored);
  }
}

typedef enum {
  STREAM_FIELD_BOOL,
  STREAM_FIELD_NUM,
  STREAM_FIELD_VALUE,
} stream_field_kind_t;

typedef struct {
  const char *name;
  uint32_t tag;
  stream_field_kind_t kind;
  size_t offset;
} stream_state_field_t;

#define STREAM_RS_FIELD(name, kind, field) { name, STREAM_READABLE_STATE_TAG, STREAM_FIELD_##kind, offsetof(stream_readable_state_t, field) }
#define STREAM_WS_FIELD(name, kind, field) { name, STREAM_WRITABLE_STATE_TAG, STREAM_FIELD_##kind, offsetof(stream_writable_state_t, field) }

static const stream_state_field_t stream_state_fields[] = {
  STREAM_RS_FIELD("objectMode",        BOOL,  object_mode),
  STREAM_RS_FIELD("highWaterMark",     NUM,   high_water_mark),
  STREAM_RS_FIELD("buffer",            VALUE, buffer),
  STREAM_RS_FIELD("length",            NUM,   length),
  STREAM_RS_FIELD("flowing",           BOOL,  flowing),
  STREAM_RS_FIELD("ended",             BOOL,  ended),
  STREAM_RS_FIELD("endEmitted",        BOOL,  end_emitted),
  STREAM_RS_FIELD("reading",           BOOL,  reading),
  STREAM_RS_FIELD("readableListening", BOOL,  readable_listening),
  STREAM_RS_FIELD("dataEmitted",       BOOL,  data_emitted),
  STREAM_RS_FIELD("autoDestroy",       BOOL,  auto_destroy),
  STREAM_RS_FIELD("destroyed",         BOOL,  destroyed),
  STREAM_RS_FIELD("errored",           VALUE, errored),
  STREAM_RS_FIELD("decoder",           VALUE, decoder),
  STREAM_WS_FIELD("objectMode",        BOOL,  object_mode),
  STREAM_WS_FIELD("highWaterMark",     NUM,   high_water_mark),
  STREAM_WS_FIELD("length",            NUM,   length),
  STREAM_WS_FIELD("needDrain",         BOOL,  need_drain),
  STREAM_WS_FIELD("ended",             BOOL,  ended),
  STREAM_WS_FIELD("finished",          BOOL,  finished),
  STREAM_WS_FIELD("autoDestroy",       BOOL,  auto_destroy),
  STREAM_WS_FIELD("errored",           VALUE, errored),
};

static void *stream_state_field_ptr(ant_t *js, const stream_state_field_t **field) {
  ant_value_t index = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t state = js_getthis(js);
  
  *field = &stream_state_fields[(size_t)js_getnum(index)];
  char *base = is_object_type(state) ? js_get_native(state, (*field)->tag) : NULL;
  
  return base ? base + (*field)->offset : NULL;
}

static ant_value_t stream_state_get(ant_params_t) {
  const stream_state_field_t *field;
  void *ptr = stream_state_field_ptr(js, &field);
  if (!ptr) return js_mkundef();
  
  switch (field->kind) {
    case STREAM_FIELD_BOOL: return js_bool(*(bool *)ptr);
    case STREAM_FIELD_NUM: return js_mknum(*(double *)ptr);
    case STREAM_FIELD_VALUE: return *(ant_value_t *)ptr;
  }
  
  return js_mkundef();
}

static ant_value_t stream_state_set(ant_params_t) {
  const stream_state_field_t *field;
  void *ptr = stream_state_field_ptr(js, &field);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  if (!ptr) return js_mkundef();
  
  switch (field->kind) {
    case STREAM_FIELD_BOOL: *(bool *)ptr = js_truthy(js, value); break;
    case STREAM_FIELD_NUM: *(double *)ptr = js_to_number(js, value); break;
    case STREAM_FIELD_VALUE: stream_state_store(js, js_obj_ptr(js_getthis(js)), ptr, value); break;
  }
  
  return js_mkundef();
}

static ant_value_t stream_make_state_proto(ant_t *js, uint32_t tag) {
  ant_value_t proto = Ant_Object_CreateBare(js);
  
  for (size_t i = 0; i < countof(stream_state_fields); i++) {
    const stream_state_field_t *field = &stream_state_fields[i];
    if (field->tag != tag) continue;
    
    js_set_accessor_desc(
      js, proto, field->name, strlen(field->name),
      js_heavy_mkfun(js, stream_state_get, js_mknum((double)i)),
      js_heavy_mkfun(js, stream_state_set, js_mknum((double)i)),
      JS_DESC_E | JS_DESC_C
    );
  }
  
  return proto;
}

static ant_value_t stream_make_state_object(ant_t *js, ant_value_t proto, void *state, uint32_t tag) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, proto);
  js_set_native(obj, state, tag);
  js_set_finalizer(obj, stream_state_finalize);
  return obj;
}

static ant_value_t stream_pipes(ant_t *js, ant_value_t stream_obj) {
  return js_get_slot(stream_obj, SLOT_STREAM_PIPES);
}

static bool stream_key_is_cstr(ant_t *js, ant_value_t value, const char *expected) {
  size_t len = 0;
  const char *s = NULL;
  if (vtype(value) != kTypeString) return false;
  s = js_getstr(js, value, &len);
  return s && len == strlen(expected) && memcmp(s, expected, len) == 0;
}

static ant_value_t stream_event_key(ant_t *js, ant_value_t value) {
  uint8_t t = vtype(value);
  if (t == kTypeString || t == kTypeSymbol) return value;
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
  ant_t *js, ant_value_t fn, ant_value_t this_val, ant_value_t *args, int nargs
) {
  if (!is_callable(fn)) return js_mkundef();
  if (sv_check_c_stack_overflow(js)) return Ant_Error_CreateStackOverflow(js);

  return sv_vm_call_mode(
    js->vm, js, fn, this_val, args, nargs, NULL,
    SV_CALL_MODE_NORMAL, js_mkundef()
  );
}

static ant_value_t stream_call_prop(
  ant_t *js,
  ant_value_t target,
  const char *name,
  ant_value_t *args,
  int nargs
) {
  ant_value_t fn = js_getprop_fallback(js, target, name);
  if (is_err(fn)) return fn;
  if (!is_callable(fn)) return js_mkundef();
  return stream_call(js, fn, target, args, nargs);
}

static void stream_call_callback(ant_t *js, ant_value_t fn, ant_value_t *args, int nargs) {
  if (!is_callable(fn)) return;
  stream_call(js, fn, js_mkundef(), args, nargs);
}

static ant_value_t stream_pair(ant_t *js, ant_value_t first, ant_value_t second) {
  ant_value_t pair = js_mkarr(js);
  js_arr_push(js, pair, first);
  js_arr_push(js, pair, second);
  return pair;
}

static void stream_schedule_microtask(ant_t *js, ant_cfunc_t fn, ant_value_t data) {
  queue_microtask_with_args(js, js_mkfun_dyn(fn), &data, 1);
}

static ant_value_t stream_buffer_ctor(ant_t *js) {
  ant_value_t ns = js_esm_import_sync_cstr(js, "buffer", 6);
  if (is_err(ns)) return ns;
  return js_get(js, ns, "Buffer");
}

static ant_value_t stream_readable_decode_chunk(
  ant_t *js, stream_readable_state_t *rs,
  ant_value_t chunk, bool flush
) {
  if (!rs || !is_object_type(rs->decoder)) return chunk;
  return string_decoder_decode_value(js, rs->decoder, chunk, flush);
}

static bool stream_value_is_empty_string(ant_t *js, ant_value_t value) {
  size_t len = 0;
  if (vtype(value) != kTypeString) return false;
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
  return stream_call(js, from_fn, buffer_ctor, args, 2);
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
    vtype(chunk) == kTypeTypedArray || buffer_is_binary_source(chunk)
  ) return chunk;

  if (vtype(chunk) == kTypeString) return stream_make_buffer(js, chunk, encoding);

  str_val = js_tostring_val(js, chunk);
  if (is_err(str_val)) return str_val;

  return stream_make_buffer(js, str_val, encoding);
}

static ant_offset_t stream_readable_buffer_len(ant_t *js, stream_readable_state_t *rs) {
  if (!rs || vtype(rs->buffer) != kTypeArray) return 0;
  ant_offset_t len = js_arr_len(js, rs->buffer);
  return len > rs->buffer_head ? len - rs->buffer_head : 0;
}

static double stream_chunk_size(ant_t *js, ant_value_t chunk, bool object_mode) {
  const uint8_t *bytes = NULL;
  size_t byte_len = 0;
  if (object_mode) return 1;
  if (buffer_source_get_bytes(js, chunk, &bytes, &byte_len)) return (double)byte_len;
  return 1;
}

static void stream_compact_readable_buffer(ant_t *js, stream_readable_state_t *rs) {
  ant_offset_t head = rs->buffer_head;
  ant_value_t buffer = rs->buffer;

  if (vtype(buffer) != kTypeArray || head == 0) return;
  ant_offset_t len = js_arr_len(js, buffer);

  if (head >= len) {
    stream_state_store(js, rs->obj, &rs->buffer, js_mkarr(js));
    rs->buffer_head = 0;
    return;
  }

  if (head <= 32 && head * 2 < len) return;
  ant_value_t compact = js_mkarr(js);
  for (ant_offset_t i = head; i < len; i++) js_arr_push(js, compact, js_arr_get(js, buffer, i));

  stream_state_store(js, rs->obj, &rs->buffer, compact);
  rs->buffer_head = 0;
}

static void stream_buffer_push(ant_t *js, stream_readable_state_t *rs, ant_value_t value) {
  if (vtype(rs->buffer) != kTypeArray) return;
  js_arr_push(js, rs->buffer, value);
  rs->length += stream_chunk_size(js, value, rs->object_mode);
}

static ant_value_t stream_buffer_shift(ant_t *js, stream_readable_state_t *rs) {
  ant_value_t buffer = rs->buffer;
  if (vtype(buffer) != kTypeArray || rs->buffer_head >= js_arr_len(js, buffer)) return js_mkundef();

  ant_value_t value = js_arr_get(js, buffer, rs->buffer_head);
  double next_length = rs->length - stream_chunk_size(js, value, rs->object_mode);

  rs->length = next_length > 0 ? next_length : 0;
  rs->data_emitted = true;
  rs->buffer_head++;
  stream_compact_readable_buffer(js, rs);

  return value;
}

static bool stream_listener_count_positive(ant_t *js, ant_value_t target, const char *event_name) {
  ant_value_t args[1];
  ant_value_t result = 0;

  args[0] = js_mkstr(js, event_name, strlen(event_name));
  result = stream_call_prop(js, target, "listenerCount", args, 1);
  return vtype(result) == kTypeNumber && js_getnum(result) > 0;
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

static ant_value_t stream_add_listener(
  ant_t *js,
  ant_value_t target,
  const char *event_name,
  ant_value_t listener,
  bool once
) {
  const char *method = once ? "once" : "on";
  ant_value_t add = js_getprop_fallback(js, target, method);
  if (is_err(add)) return add;

  if (is_callable(add)) {
    ant_value_t args[2];
    args[0] = js_mkstr(js, event_name, strlen(event_name));
    args[1] = listener;
    return stream_call(js, add, target, args, 2);
  }

  eventemitter_add_listener(js, target, event_name, listener, once);
  return js_mkundef();
}

static ant_value_t stream_get_option(ant_t *js, ant_value_t options, const char *name) {
  if (!is_object_type(options)) return js_mkundef();
  return js_get(js, options, name);
}

static double stream_default_high_water_mark(ant_t *js, bool object_mode) {
  return object_mode
    ? js->defaults.stream_object_high_water_mark
    : js->defaults.stream_high_water_mark;
}

static double stream_high_water_mark_from_options(ant_t *js, ant_value_t options, bool object_mode) {
  ant_value_t hwm = stream_get_option(js, options, "highWaterMark");
  return (vtype(hwm) == kTypeNumber && js_getnum(hwm) > 0)
    ? js_getnum(hwm)
    : stream_default_high_water_mark(js, object_mode);
}

static bool stream_auto_destroy_from_options(ant_t *js, ant_value_t options) {
  ant_value_t auto_destroy = stream_get_option(js, options, "autoDestroy");
  return is_undefined(auto_destroy) || js_truthy(js, auto_destroy);
}

static ant_value_t stream_make_base_object(ant_t *js, ant_value_t proto) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  stream_private_state_t *priv = calloc(1, sizeof(*priv));

  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, priv, STREAM_NATIVE_TAG);
  js_set_slot(obj, SLOT_AUX, js_mkundef());
  js_set_finalizer(obj, stream_finalize);

  return obj;
}

static void stream_init_base(ant_t *js, ant_value_t obj, ant_value_t raw_options) {
  ant_value_t construct_fn = stream_get_option(js, raw_options, "construct");
  ant_value_t destroy_fn = stream_get_option(js, raw_options, "destroy");

  if (is_callable(construct_fn)) {
    js_set(js, obj, "_construct", construct_fn);
    stream_begin_construct(js, obj);
  }

  if (is_callable(destroy_fn)) js_set(js, obj, "_destroy", destroy_fn);

  js_set(js, obj, "readable", js_true);
  js_set(js, obj, "destroyed", js_false);
}

static void stream_init_readable(ant_t *js, ant_value_t obj, ant_value_t raw_options) {
  ant_value_t options = is_object_type(raw_options) ? raw_options : js_mkundef();
  ant_value_t read_fn = stream_get_option(js, options, "read");
  stream_readable_state_t *rs = calloc(1, sizeof(*rs));
  if (!rs) return;

  stream_init_base(js, obj, raw_options);
  js_set(js, obj, "readable", js_true);
  js_set(js, obj, "readableEnded", js_false);

  rs->object_mode = js_truthy(js, stream_get_option(js, options, "objectMode"));
  rs->high_water_mark = stream_high_water_mark_from_options(js, options, rs->object_mode);
  rs->auto_destroy = stream_auto_destroy_from_options(js, options);
  rs->buffer = js_mkundef();
  rs->decoder = js_mkundef();
  rs->errored = js_mkundef();

  ant_value_t state = stream_make_state_object(js, js->builtins.readable_state_proto, rs, STREAM_READABLE_STATE_TAG);
  rs->obj = js_obj_ptr(state);
  stream_state_store(js, rs->obj, &rs->buffer, js_mkarr(js));

  js_set_slot_wb(js, obj, SLOT_STREAM_READABLE_STATE, state);
  js_set(js, obj, "_readableState", state);
  if (is_callable(read_fn)) js_set(js, obj, "_read", read_fn);
}

static ant_value_t js_writable_length_getter(ant_params_t) {
  stream_writable_state_t *ws = stream_ws(js_getthis(js));
  return js_mknum(ws ? ws->length : 0);
}

static ant_value_t js_writable_need_drain_getter(ant_params_t) {
  stream_writable_state_t *ws = stream_ws(js_getthis(js));
  return js_bool(ws && ws->need_drain);
}

static ant_value_t js_writable_writable_getter(ant_params_t) {
  ant_value_t stream_obj = js_getthis(js);
  stream_writable_state_t *ws = stream_ws(stream_obj);

  if (!ws) return js_false;
  if (stream_is_destroyed(js, stream_obj)) return js_false;

  return js_bool(!ws->ended);
}

static ant_value_t js_writable_ended_getter(ant_params_t) {
  stream_writable_state_t *ws = stream_ws(js_getthis(js));
  return js_bool(ws && ws->ended);
}

static ant_value_t js_writable_finished_getter(ant_params_t) {
  stream_writable_state_t *ws = stream_ws(js_getthis(js));
  return js_bool(ws && ws->finished);
}

static ant_value_t js_writable_hwm_getter(ant_params_t) {
  stream_writable_state_t *ws = stream_ws(js_getthis(js));
  return js_mknum(ws ? ws->high_water_mark : 0);
}

static void stream_define_writable_accessors(ant_t *js, ant_value_t proto) {
  js_set_getter_desc(js, proto, "writable", 8, Ant_Function_Create(js_writable_writable_getter), JS_DESC_C);
  js_set_getter_desc(js, proto, "writableEnded", 13, Ant_Function_Create(js_writable_ended_getter), JS_DESC_C);
  js_set_getter_desc(js, proto, "writableFinished", 16, Ant_Function_Create(js_writable_finished_getter), JS_DESC_C);
  js_set_getter_desc(js, proto, "writableHighWaterMark", 21, Ant_Function_Create(js_writable_hwm_getter), JS_DESC_C);
  js_set_getter_desc(js, proto, "writableLength", 14, Ant_Function_Create(js_writable_length_getter), JS_DESC_C);
  js_set_getter_desc(js, proto, "writableNeedDrain", 17, Ant_Function_Create(js_writable_need_drain_getter), JS_DESC_C);
}

static void stream_init_writable(ant_t *js, ant_value_t obj, ant_value_t raw_options) {
  ant_value_t options = is_object_type(raw_options) ? raw_options : js_mkundef();
  ant_value_t write_fn = stream_get_option(js, options, "write");
  ant_value_t final_fn = stream_get_option(js, options, "final");
  stream_writable_state_t *ws = calloc(1, sizeof(*ws));
  if (!ws) return;

  stream_init_base(js, obj, raw_options);
  js_set(js, obj, "readable", js_false);

  ws->object_mode =
    js_truthy(js, stream_get_option(js, options, "objectMode")) ||
    js_truthy(js, stream_get_option(js, options, "writableObjectMode"));
  ws->high_water_mark = stream_high_water_mark_from_options(js, options, ws->object_mode);
  ws->auto_destroy = stream_auto_destroy_from_options(js, options);
  ws->buffer = js_mkundef();
  ws->errored = js_mkundef();

  ant_value_t state = stream_make_state_object(js, js->builtins.writable_state_proto, ws, STREAM_WRITABLE_STATE_TAG);
  ws->obj = js_obj_ptr(state);
  stream_state_store(js, ws->obj, &ws->buffer, js_mkarr(js));

  js_set_slot_wb(js, obj, SLOT_STREAM_WRITABLE_STATE, state);
  js_set(js, obj, "_writableState", state);
  if (is_callable(write_fn)) js_set(js, obj, "_write", write_fn);
  if (is_callable(final_fn)) js_set(js, obj, "_final", final_fn);
}

static ant_value_t stream_emit_named(ant_t *js, ant_value_t stream_obj, const char *event_name) {
  return js_bool(eventemitter_emit_args(js, stream_obj, event_name, NULL, 0));
}

static void stream_set_errored(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  stream_readable_state_t *rs = stream_rs(stream_obj);
  stream_writable_state_t *ws = stream_ws(stream_obj);
  if (rs) stream_state_store(js, rs->obj, &rs->errored, error);
  if (ws) stream_state_store(js, ws->obj, &ws->errored, error);
}

static void stream_emit_error(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  ant_value_t args[1];
  stream_set_errored(js, stream_obj, error);
  args[0] = error;
  eventemitter_emit_args(js, stream_obj, "error", args, 1);
}

static void stream_readable_schedule_flowing_tick(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);

  if (!rs || !rs->flowing) return;
  if (stream_is_destroyed(js, stream_obj)) return;
  if (rs->flowing_read_scheduled) return;
  rs->flowing_read_scheduled = true;
  stream_schedule_microtask(js, stream_readable_continue_flowing, stream_obj);
}

static void stream_readable_schedule_continue_flowing(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);

  if (!rs || !rs->flowing) return;
  if (stream_is_destroyed(js, stream_obj)) return;
  if (rs->ended) return;
  if (stream_readable_buffer_len(js, rs) > 0) return;
  stream_readable_schedule_flowing_tick(js, stream_obj);
}

static ant_value_t stream_set_paused(ant_t *js, ant_value_t stream_obj, bool paused) {
  js_set_slot(stream_obj, SLOT_STREAM_PAUSED, js_bool(paused));
  stream_emit_named(js, stream_obj, paused ? "pause" : "resume");
  return stream_obj;
}

static ant_value_t js_stream_pause(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  if (is_err(stream_obj)) return stream_obj;
  return stream_set_paused(js, stream_obj, true);
}

static ant_value_t js_stream_resume(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  if (is_err(stream_obj)) return stream_obj;
  return stream_set_paused(js, stream_obj, false);
}

static ant_value_t js_stream_is_paused(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  if (is_err(stream_obj)) return stream_obj;
  return js_bool(js_get_slot(stream_obj, SLOT_STREAM_PAUSED) == js_true);
}

static void stream_pipe_remove_state(ant_t *js, ant_value_t source, ant_value_t state_obj) {
  ant_value_t pipes = stream_pipes(js, source);
  ant_offset_t len = vtype(pipes) == kTypeArray ? js_arr_len(js, pipes) : 0;
  ant_value_t next = js_mkarr(js);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = js_arr_get(js, pipes, i);
    if (item != state_obj) js_arr_push(js, next, item);
  }

  js_set_slot_wb(js, source, SLOT_STREAM_PIPES, next);
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

static ant_value_t stream_pipe_on_data(ant_params_t) {
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

static ant_value_t stream_pipe_on_drain(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t source = js_get(js, state_obj, "source");
  stream_call_prop(js, source, "resume", NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_end(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t dest = js_get(js, state_obj, "dest");
  bool end_dest = js_truthy(js, js_get(js, state_obj, "end"));
  stream_pipe_cleanup(js, state_obj);
  if (end_dest) stream_call_prop(js, dest, "end", NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_close(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  stream_pipe_cleanup(js, state_obj);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_error(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t dest = js_get(js, state_obj, "dest");
  stream_pipe_cleanup(js, state_obj);
  if (is_object_type(dest) && stream_listener_count_positive(js, dest, "error") && nargs > 0)
    eventemitter_emit_args(js, dest, "error", &args[0], 1);
  return js_mkundef();
}

static ant_value_t js_stream_pipe(ant_params_t) {
  ant_value_t source = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t state_obj = 0;
  bool end_dest = true;

  if (is_err(source)) return source;
  if (nargs < 1 || !is_object_type(args[0])) return js_mkerr(js, "pipe requires a destination stream");
  if (is_object_type(options)) {
    ant_value_t end_val = js_get(js, options, "end");
    if (!is_undefined(end_val)) end_dest = end_val != js_false;
  }

  state_obj = Ant_Object_CreateBare(js);
  js_set(js, state_obj, "source", source);
  js_set(js, state_obj, "dest", args[0]);
  js_set(js, state_obj, "end", js_bool(end_dest));
  js_set(js, state_obj, "cleaned", js_false);
  js_set(js, state_obj, "onData", js_heavy_mkfun(js, stream_pipe_on_data, state_obj));
  js_set(js, state_obj, "onDrain", js_heavy_mkfun(js, stream_pipe_on_drain, state_obj));
  js_set(js, state_obj, "onEnd", js_heavy_mkfun(js, stream_pipe_on_end, state_obj));
  js_set(js, state_obj, "onClose", js_heavy_mkfun(js, stream_pipe_on_close, state_obj));
  js_set(js, state_obj, "onError", js_heavy_mkfun(js, stream_pipe_on_error, state_obj));

  ant_value_t pipes = stream_pipes(js, source);
  if (vtype(pipes) != kTypeArray) {
    pipes = js_mkarr(js);
    js_set_slot_wb(js, source, SLOT_STREAM_PIPES, pipes);
  }
  
  js_arr_push(js, pipes, state_obj);
  stream_add_listener(js, source, "data", js_get(js, state_obj, "onData"), false);
  stream_add_listener(js, source, "end", js_get(js, state_obj, "onEnd"), true);
  stream_add_listener(js, source, "close", js_get(js, state_obj, "onClose"), true);
  stream_add_listener(js, source, "error", js_get(js, state_obj, "onError"), false);
  stream_add_listener(js, args[0], "drain", js_get(js, state_obj, "onDrain"), false);
  eventemitter_emit_args(js, args[0], "pipe", &source, 1);
  stream_readable_state_t *rs = stream_rs(source);
  if (rs) rs->flowing = true;
  stream_call_prop(js, source, "resume", NULL, 0);

  return args[0];
}

static ant_value_t js_stream_unpipe(ant_params_t) {
  ant_value_t source = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t pipes = 0;
  ant_value_t matches = 0;
  ant_offset_t len = 0;
  ant_value_t dest = nargs > 0 ? args[0] : js_mkundef();

  if (is_err(source)) return source;
  pipes = stream_pipes(js, source);
  if (vtype(pipes) != kTypeArray) return source;

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

static ant_value_t stream_destroy_emit_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t destroyed_err = nargs > 1 ? args[1] : js_mkundef();
  if (!is_null(destroyed_err) && !is_undefined(destroyed_err)) stream_emit_error(js, stream_obj, destroyed_err);
  stream_emit_named(js, stream_obj, "close");
  return js_mkundef();
}

static void stream_schedule_destroy_emit(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  ant_value_t tick_args[2] = { stream_obj, error };
  queue_microtask_with_args(js, js_mkfun_dyn(stream_destroy_emit_tick), tick_args, 2);
}

static ant_value_t stream_destroy_done(ant_params_t) {
  ant_value_t done_state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (js_arr_get(js, done_state, 1) == js_true) return js_mkundef();
  
  js_arr_set_index(js, done_state, 1, js_true);
  stream_schedule_destroy_emit(js, js_arr_get(js, done_state, 0), nargs > 0 ? args[0] : js_mkundef());
  
  return js_mkundef();
}

static ant_value_t stream_once_call(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (js_arr_get(js, state_obj, 2) == js_true) return js_mkundef();
  
  js_arr_set_index(js, state_obj, 2, js_true);
  return stream_call(js, js_arr_get(js, state_obj, 0), js_arr_get(js, state_obj, 1), args, nargs);
}

static ant_value_t stream_make_once(ant_t *js, ant_value_t fn, ant_value_t this_val) {
  ant_value_t state_obj = js_mkarr(js);
  js_arr_push(js, state_obj, fn);
  js_arr_push(js, state_obj, this_val);
  js_arr_push(js, state_obj, js_false);
  return js_heavy_mkfun(js, stream_once_call, state_obj);
}

static ant_value_t js_stream__destroy(ant_params_t) {
  ant_value_t error = nargs > 0 ? args[0] : js_mknull();
  stream_call_callback(js, nargs > 1 ? args[1] : js_mkundef(), &error, 1);
  return js_mkundef();
}

static ant_value_t stream_run_destroy(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  ant_value_t destroy_fn = js_getprop_fallback(js, stream_obj, "_destroy");
  ant_value_t destroy_error = is_undefined(error) ? js_mknull() : error;
  
  if (!is_callable(destroy_fn) || destroy_fn == js->builtins.stream_default_destroy) {
    stream_schedule_destroy_emit(js, stream_obj, destroy_error);
    return stream_obj;
  }

  ant_value_t done_state = js_mkarr(js);
  js_arr_push(js, done_state, stream_obj);
  js_arr_push(js, done_state, js_false);
  
  ant_value_t destroy_args[2] = { destroy_error, js_heavy_mkfun(js, stream_destroy_done, done_state) };
  ant_value_t result = stream_call(js, destroy_fn, stream_obj, destroy_args, 2);
  
  return is_err(result) ? result : stream_obj;
}

static ant_value_t js_stream_destroy(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();

  if (is_err(stream_obj)) return stream_obj;
  if (stream_is_destroyed(js, stream_obj)) return stream_obj;

  js_set(js, stream_obj, "destroyed", js_true);
  stream_readable_state_t *rs = stream_rs(stream_obj);
  
  if (rs) {
    rs->destroyed = true;
    js_set(js, stream_obj, "readable", js_false);
  }
  
  if (!is_undefined(error) && !is_null(error)) stream_set_errored(js, stream_obj, error);
  stream_private_state_t *priv = stream_private_state(stream_obj);
  
  if (priv && priv->constructing) {
    priv->destroy_pending = true;
    js_set_slot_wb(js, stream_obj, SLOT_STREAM_DESTROY_ERROR, error);
    return stream_obj;
  }

  return stream_run_destroy(js, stream_obj, error);
}

static ant_value_t js_readable__read(ant_params_t) {
  return js_mkundef();
}

static ant_value_t stream_readable_start_flowing(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  return stream_readable_begin_flowing(js, stream_obj);
}

static ant_value_t stream_readable_read_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  return stream_readable_maybe_read(js, stream_obj);
}

static ant_value_t stream_readable_emit_readable(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_readable_state_t *rs = stream_rs(stream_obj);

  if (!rs) return js_mkundef();
  rs->readable_scheduled = false;

  if (rs->flowing || rs->end_emitted) return js_mkundef();
  if (stream_is_destroyed(js, stream_obj)) return js_mkundef();
  if (stream_readable_buffer_len(js, rs) == 0 && !rs->ended) return js_mkundef();

  stream_emit_named(js, stream_obj, "readable");
  return js_mkundef();
}

static void stream_readable_schedule_readable(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);

  if (!rs || !rs->readable_listening) return;
  if (rs->flowing || rs->readable_scheduled) return;
  rs->readable_scheduled = true;
  stream_schedule_microtask(js, stream_readable_emit_readable, stream_obj);
}

ant_value_t stream_readable_continue_flowing(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_readable_state_t *rs = stream_rs(stream_obj);

  if (!rs) return js_mkundef();
  rs->flowing_read_scheduled = false;

  if (!rs->flowing) return js_mkundef();
  if (stream_is_destroyed(js, stream_obj)) return js_mkundef();

  stream_readable_maybe_read(js, stream_obj);
  stream_readable_flush(js, stream_obj);
  
  return js_mkundef();
}

ant_value_t stream_readable_begin_flowing(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);
  if (!rs || !rs->flowing) return js_mkundef();

  sv_call_native_this(js, js_stream_resume, stream_obj, NULL, 0);
  stream_readable_maybe_read(js, stream_obj);
  stream_readable_flush(js, stream_obj);
  
  return js_mkundef();
}

static bool stream_auto_destroy(ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);
  stream_writable_state_t *ws = stream_ws(stream_obj);
  return rs ? rs->auto_destroy : !ws || ws->auto_destroy;
}

static ant_value_t stream_half_open_end_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_writable_state_t *ws = stream_ws(stream_obj);
  if ((ws && ws->ended) || stream_is_destroyed(js, stream_obj)) return js_mkundef();
  return stream_call_prop(js, stream_obj, "end", NULL, 0);
}

static ant_value_t stream_readable_emit_end(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_readable_state_t *rs = stream_rs(stream_obj);
  ant_value_t tail = 0;

  if (!rs) return js_mkundef();
  rs->end_scheduled = false;

  if (!rs->flowing && !rs->end_requested) return js_mkundef();
  if (!rs->ended || rs->end_emitted) return js_mkundef();
  if (stream_readable_buffer_len(js, rs) != 0) return js_mkundef();
  if (stream_is_destroyed(js, stream_obj)) return js_mkundef();

  tail = stream_readable_decode_chunk(js, rs, js_mkundef(), true);
  if (is_err(tail)) return tail;
  
  if (!is_undefined(tail) && !stream_value_is_empty_string(js, tail)) {
    rs->data_emitted = true;
    eventemitter_emit_args(js, stream_obj, "data", &tail, 1);
  }
  
  rs->end_emitted = true;
  js_set(js, stream_obj, "readable", js_false);
  js_set(js, stream_obj, "readableEnded", js_true);
  stream_emit_named(js, stream_obj, "end");

  stream_writable_state_t *ws = stream_ws(stream_obj);
  bool writable_done = !ws || ws->finished;
  
  if (writable_done && stream_auto_destroy(stream_obj))
    sv_call_native_this(js, js_stream_destroy, stream_obj, NULL, 0);
  else if (!writable_done && js_get(js, stream_obj, "allowHalfOpen") == js_false)
    stream_schedule_microtask(js, stream_half_open_end_tick, stream_obj);

  return js_mkundef();
}

static void stream_readable_schedule_end(ant_t *js, ant_value_t stream_obj, stream_readable_state_t *rs) {
  if (rs->end_scheduled) return;
  rs->end_scheduled = true;
  stream_schedule_microtask(js, stream_readable_emit_end, stream_obj);
}

static void stream_readable_end_if_drained(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);

  if (!rs || !rs->ended) return;
  if (stream_readable_buffer_len(js, rs) != 0) return;
  rs->end_requested = true;
  stream_readable_schedule_end(js, stream_obj, rs);
}

ant_value_t stream_readable_flush(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);
  bool emitted_data = false;

  if (!rs) return js_mkundef();

  while (rs->flowing && stream_readable_buffer_len(js, rs) > 0) {
    ant_value_t chunk = stream_buffer_shift(js, rs);
    chunk = stream_readable_decode_chunk(js, rs, chunk, false);
    if (is_err(chunk)) return chunk;
    emitted_data = true;
    eventemitter_emit_args(js, stream_obj, "data", &chunk, 1);
  }

  if (rs->flowing && rs->ended && !rs->end_emitted && stream_readable_buffer_len(js, rs) == 0) {
    stream_readable_schedule_end(js, stream_obj, rs);
  } else if (emitted_data) stream_readable_schedule_continue_flowing(js, stream_obj);

  return js_mkundef();
}

ant_value_t stream_readable_maybe_read(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);
  stream_private_state_t *priv = stream_private_state(stream_obj);

  if (!rs) return js_mkundef();
  if (priv && priv->constructing) return js_mkundef();
  if (rs->reading || rs->ended) return js_mkundef();
  if (stream_is_destroyed(js, stream_obj)) return js_mkundef();
  if (rs->length >= rs->high_water_mark) return js_mkundef();

  ant_value_t read_fn = js_getprop_fallback(js, stream_obj, "_read");
  ant_value_t args[1] = { js_mknum(rs->high_water_mark) };
  
  rs->reading = true;
  if (is_callable(read_fn)) stream_call(js, read_fn, stream_obj, args, 1);
  rs->reading = false;
  
  return js_mkundef();
}

ant_value_t stream_readable_push_value(
  ant_t *js,
  ant_value_t stream_obj,
  ant_value_t chunk,
  ant_value_t encoding
) {
  stream_readable_state_t *rs = stream_rs(stream_obj);
  ant_value_t normalized = 0;

  if (!rs) return js_false;
  if (stream_is_destroyed(js, stream_obj)) return js_false;

  if (is_null(chunk)) {
    rs->ended = true;
    stream_readable_flush(js, stream_obj);
    stream_readable_schedule_readable(js, stream_obj);
    return js_false;
  }

  normalized = stream_normalize_chunk(js, chunk, rs->object_mode, encoding);
  if (is_err(normalized)) return normalized;
  
  stream_buffer_push(js, rs, normalized);
  if (rs->flowing) stream_readable_flush(js, stream_obj);
  else stream_readable_schedule_readable(js, stream_obj);

  return js_bool(rs->length < rs->high_water_mark);
}

static ant_value_t js_readable_push(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t encoding = nargs > 1 ? args[1] : js_mkundef();
  if (is_err(stream_obj)) return stream_obj;
  return stream_readable_push_value(js, stream_obj, chunk, encoding);
}

static ant_value_t js_readable_read(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t chunk = 0;

  if (is_err(stream_obj)) return stream_obj;
  stream_readable_state_t *rs = stream_rs(stream_obj);
  if (!rs) return js_mknull();

  if (nargs > 0 && vtype(args[0]) == kTypeNumber && tod(args[0]) == 0.0) {
    stream_readable_maybe_read(js, stream_obj);
    return js_mknull();
  }

  if (stream_readable_buffer_len(js, rs) == 0) stream_readable_maybe_read(js, stream_obj);
  if (stream_readable_buffer_len(js, rs) == 0) {
    stream_readable_end_if_drained(js, stream_obj);
    return js_mknull();
  }

  chunk = stream_buffer_shift(js, rs);
  chunk = stream_readable_decode_chunk(js, rs, chunk, false);
  if (is_err(chunk)) return chunk;
  if (rs->flowing) stream_readable_flush(js, stream_obj);
  else stream_readable_end_if_drained(js, stream_obj);

  return chunk;
}

static ant_value_t js_readable_set_encoding(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t decoder = 0;

  ant_value_t encoding = nargs > 0 && !is_undefined(args[0]) ? args[0] : js->builtins.stream_utf8;
  ant_value_t encoding_str = 0;

  if (is_err(stream_obj)) return stream_obj;
  stream_readable_state_t *rs = stream_rs(stream_obj);
  if (!rs) return stream_obj;

  decoder = string_decoder_create(js, encoding, js_mkundef());
  if (is_err(decoder)) return decoder;
  encoding_str = js_tostring_val(js, encoding);
  if (is_err(encoding_str)) return encoding_str;

  stream_state_store(js, rs->obj, &rs->decoder, decoder);
  js_set(js, stream_obj, "encoding", encoding_str);
  js_set(js, stream_obj, "readableEncoding", encoding_str);
  
  return stream_obj;
}

static ant_value_t js_readable_add(ant_native_params_t, bool once) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t key = 0;

  if (is_err(stream_obj)) return stream_obj;
  if (nargs < 2) return js_mkerr(js, "on requires 2 arguments (event, listener)");
  key = stream_event_key(js, args[0]);

  if (is_err(key)) return key;
  if (!eventemitter_add_listener_val(js, stream_obj, key, args[1], once))
    return js_mkerr(js, "listener must be a function");

  stream_readable_state_t *rs = stream_rs(stream_obj);
  if (!rs) return stream_obj;

  if (stream_key_is_cstr(js, key, "data") && !rs->readable_listening) {
    rs->flowing = true;
    stream_schedule_microtask(js, stream_readable_start_flowing, stream_obj);
  } else if (stream_key_is_cstr(js, key, "readable")) {
    rs->readable_listening = true;
    rs->flowing = false;
    if (stream_readable_buffer_len(js, rs) > 0 || rs->ended)
      stream_readable_schedule_readable(js, stream_obj);
    else stream_schedule_microtask(js, stream_readable_read_tick, stream_obj);
  }

  return stream_obj;
}

static ant_value_t js_readable_on(ant_params_t) {
  return js_readable_add(js, args, nargs, false);
}

static ant_value_t js_readable_once(ant_params_t) {
  return js_readable_add(js, args, nargs, true);
}

ant_value_t stream_readable_resume(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);
  if (rs) rs->flowing = true;

  stream_set_paused(js, stream_obj, false);
  stream_readable_schedule_flowing_tick(js, stream_obj);
  return stream_obj;
}

ant_value_t stream_readable_pause(ant_t *js, ant_value_t stream_obj) {
  stream_readable_state_t *rs = stream_rs(stream_obj);
  if (rs) rs->flowing = false;
  return stream_set_paused(js, stream_obj, true);
}

static ant_value_t js_readable_resume(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  if (is_err(stream_obj)) return stream_obj;
  return stream_readable_resume(js, stream_obj);
}

static ant_value_t js_readable_pause(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  if (is_err(stream_obj)) return stream_obj;
  return stream_readable_pause(js, stream_obj);
}

static ant_value_t js_writable__write(ant_params_t) {
  ant_value_t callback = nargs > 2 ? args[2] : js_mkundef();
  stream_call_callback(js, callback, NULL, 0);
  return js_mkundef();
}

static ant_value_t js_writable__final(ant_params_t) {
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  stream_call_callback(js, callback, NULL, 0);
  return js_mkundef();
}

typedef enum {
  STREAM_WRITE_THROWN = -1,
  STREAM_WRITE_PENDING = 0,
  STREAM_WRITE_COMPLETE = 1,
} stream_write_status_t;

enum {
  STREAM_WRITE_DONE_STREAM,
  STREAM_WRITE_DONE_CALLBACK,
  STREAM_WRITE_DONE_SIZE,
  STREAM_WRITE_DONE_CALLED,
};

static stream_write_status_t stream_writable_start(
  ant_t *js,
  ant_value_t stream_obj,
  ant_value_t chunk,
  ant_value_t encoding,
  ant_value_t callback,
  double chunk_size
);

static void stream_writable_set_length(stream_writable_state_t *ws, double length) {
  ws->length = length < 0 ? 0 : length;
}

static bool stream_writable_has_buffered(ant_t *js, stream_writable_state_t *ws) {
  return vtype(ws->buffer) == kTypeArray && ws->buffer_head < js_arr_len(js, ws->buffer);
}

static void stream_writable_compact_buffer(ant_t *js, stream_writable_state_t *ws) {
  ant_value_t buffer = ws->buffer;
  ant_offset_t head = ws->buffer_head;
  ant_offset_t len = vtype(buffer) == kTypeArray ? js_arr_len(js, buffer) : 0;

  if (vtype(buffer) != kTypeArray || head == 0) return;
  if (head < len && (head < 32 * STREAM_WRITE_ENTRY_STRIDE || head * 2 < len)) return;

  ant_value_t compact = js_mkarr(js);
  for (ant_offset_t i = head; i < len; i++)
    js_arr_push(js, compact, js_arr_get(js, buffer, i));

  stream_state_store(js, ws->obj, &ws->buffer, compact);
  ws->buffer_head = 0;
}

static void stream_writable_enqueue(
  ant_t *js,
  stream_writable_state_t *ws,
  ant_value_t chunk,
  ant_value_t encoding,
  ant_value_t callback,
  double chunk_size
) {
  if (vtype(ws->buffer) != kTypeArray) {
    stream_state_store(js, ws->obj, &ws->buffer, js_mkarr(js));
    ws->buffer_head = 0;
  }

  js_arr_push(js, ws->buffer, chunk);
  js_arr_push(js, ws->buffer, encoding);
  js_arr_push(js, ws->buffer, callback);
  js_arr_push(js, ws->buffer, js_mknum(chunk_size));
}

static bool stream_writable_dequeue(ant_t *js, stream_writable_state_t *ws, ant_value_t entry[STREAM_WRITE_ENTRY_STRIDE]) {
  if (!stream_writable_has_buffered(js, ws)) return false;
  for (int i = 0; i < STREAM_WRITE_ENTRY_STRIDE; i++)
    entry[i] = js_arr_get(js, ws->buffer, ws->buffer_head + (ant_offset_t)i);

  ws->buffer_head += STREAM_WRITE_ENTRY_STRIDE;
  stream_writable_compact_buffer(js, ws);

  return true;
}

static void stream_writable_fail_buffered(ant_t *js, ant_value_t stream_obj, ant_value_t err) {
  stream_writable_state_t *ws = stream_ws(stream_obj);
  ant_value_t entry[STREAM_WRITE_ENTRY_STRIDE];
  if (!ws) return;

  while (stream_writable_dequeue(js, ws, entry)) {
    ant_value_t entry_cb = entry[STREAM_WRITE_ENTRY_CALLBACK];
    if (is_callable(entry_cb)) stream_call_callback(js, entry_cb, &err, 1);
  }

  stream_writable_set_length(ws, 0);
  ws->need_drain = false;
}

static ant_value_t stream_writable_emit_drain(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_emit_named(js, stream_obj, "drain");
  return js_mkundef();
}

static ant_value_t stream_writable_drain(ant_t *js, ant_value_t stream_obj) {
  stream_writable_state_t *ws = stream_ws(stream_obj);
  stream_private_state_t *priv = stream_private_state(stream_obj);
  ant_value_t entry[STREAM_WRITE_ENTRY_STRIDE];

  if (priv && priv->draining) return js_mkundef();
  if (priv) priv->draining = true;

  while (ws && !stream_is_destroyed(js, stream_obj) && stream_writable_dequeue(js, ws, entry)) {
    ant_value_t size = entry[STREAM_WRITE_ENTRY_SIZE];

    stream_write_status_t status = stream_writable_start(
      js, stream_obj,
      entry[STREAM_WRITE_ENTRY_CHUNK],
      entry[STREAM_WRITE_ENTRY_ENCODING],
      entry[STREAM_WRITE_ENTRY_CALLBACK],
      vtype(size) == kTypeNumber ? js_getnum(size) : 0
    );

    if (status == STREAM_WRITE_COMPLETE) continue;

    if (status == STREAM_WRITE_THROWN) {
      if (priv) priv->draining = false;
      return Ant_Exception_Current(js);
    }

    break;
  }

  if (priv) priv->draining = false;

  if (
    ws && (!priv || !priv->writing) &&
    !stream_writable_has_buffered(js, ws) &&
    ws->length <= 0 && ws->need_drain
  ) {
    ws->need_drain = false;

    if (priv && priv->sync) stream_schedule_microtask(js, stream_writable_emit_drain, stream_obj);
    else stream_emit_named(js, stream_obj, "drain");
  }

  if (priv && !priv->writing && priv->pending_final && !priv->final_started) {
    ant_value_t end_callback = js_get_slot(stream_obj, SLOT_AUX);
    priv->pending_final = false;
    stream_set_end_callback(js, stream_obj, js_mkundef());
    return stream_writable_begin_end(js, stream_obj, end_callback);
  }

  return js_mkundef();
}

static ant_value_t stream_writable_write_done(ant_params_t) {
  ant_value_t done_state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (js_arr_get(js, done_state, STREAM_WRITE_DONE_CALLED) == js_true) return js_mkundef();
  js_arr_set_index(js, done_state, STREAM_WRITE_DONE_CALLED, js_true);

  ant_value_t stream_obj = js_arr_get(js, done_state, STREAM_WRITE_DONE_STREAM);
  ant_value_t callback = js_arr_get(js, done_state, STREAM_WRITE_DONE_CALLBACK);
  ant_value_t size = js_arr_get(js, done_state, STREAM_WRITE_DONE_SIZE);

  stream_writable_state_t *ws = stream_ws(stream_obj);
  stream_private_state_t *priv = stream_private_state(stream_obj);
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();
  bool has_buffered = false;

  if (ws) {
    stream_writable_set_length(ws, ws->length - (vtype(size) == kTypeNumber ? js_getnum(size) : 0));
    has_buffered = stream_writable_has_buffered(js, ws);
  }

  if (priv && !has_buffered) priv->writing = false;

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };

    if (priv) {
      priv->pending_final = false;
      priv->writing = false;
    }

    stream_set_errored(js, stream_obj, err);
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);

    stream_writable_fail_buffered(js, stream_obj, err);
    sv_call_native_this(js, js_stream_destroy, stream_obj, destroy_args, 1);

    return js_mkundef();
  }

  if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);

  if (stream_is_destroyed(js, stream_obj) && ws && stream_writable_has_buffered(js, ws)) {
    ant_value_t destroyed_err = Ant_Error_Create(
      js, JS_ERR_GENERIC, "Cannot call write after a stream was destroyed"
    );

    js_set(js, destroyed_err, "code", js_mkstr(js, "ERR_STREAM_DESTROYED", 20));
    stream_writable_fail_buffered(js, stream_obj, destroyed_err);
    return js_mkundef();
  }

  return stream_writable_drain(js, stream_obj);
}

static stream_write_status_t stream_writable_start(
  ant_t *js,
  ant_value_t stream_obj,
  ant_value_t chunk,
  ant_value_t encoding,
  ant_value_t callback,
  double chunk_size
) {
  ant_value_t done_state = js_mkarr(js);
  ant_value_t write_args[3];
  stream_private_state_t *priv = stream_private_state(stream_obj);

  js_arr_push(js, done_state, stream_obj);
  js_arr_push(js, done_state, callback);
  js_arr_push(js, done_state, js_mknum(chunk_size));
  js_arr_push(js, done_state, js_false);

  ant_value_t done = js_heavy_mkfun(js, stream_writable_write_done, done_state);
  if (priv) priv->writing = true;

  ant_value_t write_fn = js_getprop_fallback(js, stream_obj, "_write");
  write_args[0] = chunk;
  write_args[1] = encoding;
  write_args[2] = done;

  bool prev_sync = priv ? priv->sync : false;
  if (priv) priv->sync = true;

  if (is_callable(write_fn)) {
    ant_value_t result = stream_call(js, write_fn, stream_obj, write_args, 3);
    if (is_err(result)) {
      if (priv) priv->sync = prev_sync;
      return STREAM_WRITE_THROWN;
    }
  } else stream_call_callback(js, done, NULL, 0);

  if (priv) priv->sync = prev_sync;
  return js_arr_get(js, done_state, STREAM_WRITE_DONE_CALLED) == js_true
    ? STREAM_WRITE_COMPLETE : STREAM_WRITE_PENDING;
}

static ant_value_t stream_errored_write_cb_tick(ant_params_t) {
  ant_value_t state_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t callback = js_arr_get(js, state_obj, 0);
  ant_value_t error = js_arr_get(js, state_obj, 1);
  if (is_callable(callback)) stream_call_callback(js, callback, &error, 1);
  return js_mkundef();
}

static void stream_write_callback_later(ant_t *js, ant_value_t callback, ant_value_t error) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, error);

  ant_value_t tick_state = stream_pair(js, callback, error);
  GC_ROOT_PIN(js, tick_state);

  stream_schedule_microtask(js, stream_errored_write_cb_tick, tick_state);
  GC_ROOT_RESTORE(js, mark);
}

static ant_value_t stream_write_state_error(ant_t *js, bool ending) {
  const char *code = ending ? "ERR_STREAM_WRITE_AFTER_END" : "ERR_STREAM_DESTROYED";

  ant_value_t err = Ant_Error_Create(
    js, JS_ERR_GENERIC, ending
      ? "write after end"
      : "Cannot call write after a stream was destroyed"
  );

  if (is_object_type(err)) js_set(js, err, "code", js_mkstr(js, code, strlen(code)));
  return err;
}

static ant_value_t stream_writable_write_impl(
  ant_t *js,
  ant_value_t stream_obj,
  ant_value_t chunk,
  ant_value_t encoding,
  ant_value_t callback,
  bool allow_after_end
) {
  stream_writable_state_t *ws = stream_ws(stream_obj);
  if (!ws) return js_false;

  ant_value_t prior_error = ws->errored;
  if (!is_undefined(prior_error) && !is_null(prior_error)) {
    if (is_callable(callback)) stream_write_callback_later(js, callback, prior_error);
    return js_false;
  }

  bool ending = !allow_after_end && ws->ended;
  if (ending || stream_is_destroyed(js, stream_obj)) {
    ant_value_t err = stream_write_state_error(js, ending);
    stream_set_errored(js, stream_obj, err);

    if (is_callable(callback)) stream_write_callback_later(js, callback, err);
    else stream_emit_error(js, stream_obj, err);

    return js_false;
  }

  ant_value_t normalized = stream_normalize_chunk(js, chunk, ws->object_mode, encoding);
  if (is_err(normalized)) return normalized;

  double chunk_size = stream_chunk_size(js, normalized, ws->object_mode);
  double length = ws->length + chunk_size;
  bool accepted = length < ws->high_water_mark;

  stream_writable_set_length(ws, length);
  if (!accepted) ws->need_drain = true;

  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (priv && (priv->writing || priv->constructing)) stream_writable_enqueue(js, ws, normalized, encoding, callback, chunk_size);
  else if (stream_writable_start(js, stream_obj, normalized, encoding, callback, chunk_size) == STREAM_WRITE_THROWN)
    return Ant_Exception_Current(js);

  return js_bool(accepted && !stream_is_destroyed(js, stream_obj));
}

static ant_value_t js_writable_write(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Writable");
  ant_value_t callback = js_mkundef();
  ant_value_t encoding = js->builtins.stream_utf8;

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

static ant_value_t stream_writable_finish_tick(ant_params_t) {
  ant_value_t state_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t stream_obj = js_arr_get(js, state_obj, 0);
  ant_value_t callback = js_arr_get(js, state_obj, 1);
  stream_writable_state_t *ws = stream_ws(stream_obj);

  if (stream_is_destroyed(js, stream_obj)) {
    if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);
    return js_mkundef();
  }

  if (ws) ws->finished = true;
  if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);
  stream_emit_named(js, stream_obj, "finish");

  if (!js_truthy(js, js_get(js, stream_obj, "readable")) && stream_auto_destroy(stream_obj))
    sv_call_native_this(js, js_stream_destroy, stream_obj, NULL, 0);

  return js_mkundef();
}

static ant_value_t stream_writable_end_done(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_arr_get(js, state_obj, 0);
  ant_value_t callback = js_arr_get(js, state_obj, 1);
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };
    sv_call_native_this(js, js_stream_destroy, stream_obj, destroy_args, 1);
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    return js_mkundef();
  }

  stream_schedule_microtask(js, stream_writable_finish_tick, state_obj);
  return js_mkundef();
}

ant_value_t stream_writable_begin_end(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  ant_value_t final_fn = 0;
  ant_value_t final_args[1];
  ant_value_t done_state = 0;
  ant_value_t done = 0;
  stream_private_state_t *priv = stream_private_state(stream_obj);

  done_state = stream_pair(js, stream_obj, callback);
  
  done = stream_make_once(js, js_heavy_mkfun(js, stream_writable_end_done, done_state), js_mkundef());
  if (priv) {
    priv->final_started = true;
    priv->pending_final = false;
  }
  
  stream_set_end_callback(js, stream_obj, js_mkundef());
  final_fn = js_getprop_fallback(js, stream_obj, "_final");
  final_args[0] = done;
  
  if (is_callable(final_fn)) stream_call(js, final_fn, stream_obj, final_args, 1);
  else stream_call_callback(js, done, NULL, 0);

  return stream_obj;
}

static ant_value_t stream_writable_end_after_write(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_arr_get(js, state_obj, 0);
  ant_value_t callback = js_arr_get(js, state_obj, 1);
  stream_private_state_t *priv = stream_private_state(stream_obj);
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };
    sv_call_native_this(js, js_stream_destroy, stream_obj, destroy_args, 1);
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    return js_mkundef();
  }

  if (priv) priv->pending_final = false;
  stream_set_end_callback(js, stream_obj, js_mkundef());

  return stream_writable_begin_end(js, stream_obj, callback);
}

static ant_value_t js_writable_end(ant_params_t) {
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

  stream_writable_state_t *ws = stream_ws(stream_obj);
  if (ws) ws->ended = true;
  priv = stream_private_state(stream_obj);

  if (!is_undefined(chunk) && !is_null(chunk)) {
    after_write_state = stream_pair(js, stream_obj, callback);
    
    after_write = stream_make_once(
      js, js_heavy_mkfun(js, stream_writable_end_after_write, after_write_state),
      js_mkundef()
    );
    
    ant_value_t result = stream_writable_write_impl(js, stream_obj, chunk, encoding, after_write, true);
    return is_err(result) ? result : stream_obj;
  }

  if (priv && (priv->writing || priv->constructing) && !priv->final_started) {
    priv->pending_final = true;
    stream_set_end_callback(js, stream_obj, callback);
    return stream_obj;
  }

  return stream_writable_begin_end(js, stream_obj, callback);
}

static ant_value_t js_transform__transform(ant_params_t) {
  ant_value_t callback = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t cb_args[2];
  cb_args[0] = js_mknull();
  cb_args[1] = nargs > 0 ? args[0] : js_mkundef();
  stream_call_callback(js, callback, cb_args, 2);
  return js_mkundef();
}

static ant_value_t stream_transform_write_callback(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_arr_get(js, state_obj, 0);
  ant_value_t outer_callback = js_arr_get(js, state_obj, 1);

  if (nargs > 0 && !is_null(args[0]) && !is_undefined(args[0])) {
    if (is_callable(outer_callback)) stream_call(js, outer_callback, stream_obj, &args[0], 1);
    return js_mkundef();
  }

  if (nargs > 1 && !is_null(args[1]) && !is_undefined(args[1]))
    stream_readable_push_value(js, stream_obj, args[1], js_mkundef());

  if (is_callable(outer_callback)) stream_call(js, outer_callback, stream_obj, NULL, 0);
  return js_mkundef();
}

static ant_value_t js_transform__write(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Transform");
  ant_value_t transform_fn = 0;
  ant_value_t cb_state = 0;
  ant_value_t cb = 0;
  ant_value_t call_args[3];

  if (is_err(stream_obj)) return stream_obj;
  transform_fn = js_getprop_fallback(js, stream_obj, "_transform");

  cb_state = stream_pair(js, stream_obj, nargs > 2 ? args[2] : js_mkundef());
  cb = js_heavy_mkfun(js, stream_transform_write_callback, cb_state);

  call_args[0] = nargs > 0 ? args[0] : js_mkundef();
  call_args[1] = nargs > 1 ? args[1] : js->builtins.stream_utf8;
  call_args[2] = cb;

  return stream_call(js, transform_fn, stream_obj, call_args, 3);
}

static ant_value_t stream_transform_final_callback(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = js_arr_get(js, state_obj, 0);
  ant_value_t callback = js_arr_get(js, state_obj, 1);

  if (nargs > 0 && !is_null(args[0]) && !is_undefined(args[0])) {
    if (is_callable(callback)) stream_call(js, callback, stream_obj, &args[0], 1);
    return js_mkundef();
  }

  if (nargs > 1 && !is_null(args[1]) && !is_undefined(args[1]))
    stream_readable_push_value(js, stream_obj, args[1], js_mkundef());
  stream_readable_push_value(js, stream_obj, js_mknull(), js_mkundef());
  if (is_callable(callback)) stream_call(js, callback, stream_obj, NULL, 0);
  
  return js_mkundef();
}

static ant_value_t js_transform__final(ant_params_t) {
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

  cb_state = stream_pair(js, stream_obj, nargs > 0 ? args[0] : js_mkundef());
  cb = js_heavy_mkfun(js, stream_transform_final_callback, cb_state);
  call_args[0] = cb;
  return stream_call(js, flush_fn, stream_obj, call_args, 1);
}

static ant_value_t js_passthrough__transform(ant_params_t) {
  return js_transform__transform(js, args, nargs, js_mkundef());
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

static ant_value_t stream_finished_on_finish(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  return stream_finished_fire(js, state_obj, js_mkundef());
}

static ant_value_t stream_finished_on_error(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();
  return stream_finished_fire(js, state_obj, error);
}

static ant_value_t stream_finished_register(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  ant_value_t state_obj = Ant_Object_CreateBare(js);
  ant_value_t on_finish = 0;
  ant_value_t on_error = 0;

  if (!is_callable(callback)) callback = Ant_Function_Create(stream_noop);

  js_set(js, state_obj, "stream", stream_obj);
  js_set(js, state_obj, "callback", callback);
  js_set(js, state_obj, "called", js_false);
  on_finish = js_heavy_mkfun(js, stream_finished_on_finish, state_obj);
  on_error = js_heavy_mkfun(js, stream_finished_on_error, state_obj);
  js_set(js, state_obj, "onFinish", on_finish);
  js_set(js, state_obj, "onError", on_error);

  ant_value_t added = stream_add_listener(js, stream_obj, "end", on_finish, false);
  if (is_err(added)) return added;
  
  added = stream_add_listener(js, stream_obj, "finish", on_finish, false);
  if (is_err(added)) return added;
  
  added = stream_add_listener(js, stream_obj, "close", on_finish, false);
  if (is_err(added)) return added;
  
  added = stream_add_listener(js, stream_obj, "error", on_error, false);
  if (is_err(added)) return added;

  return stream_obj;
}

static ant_value_t js_stream_finished(ant_params_t) {
  ant_value_t callback = nargs > 1 ? args[1] : js_mkundef();
  if (nargs < 1 || !is_object_type(args[0])) return js_mkerr(js, "finished requires a stream");
  return stream_finished_register(js, args[0], callback);
}

static ant_value_t stream_pipeline_done(ant_params_t) {
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

static ant_value_t stream_pipeline_error(ant_params_t) {
  ant_value_t done = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (nargs > 0 && !is_undefined(args[0])) stream_call_callback(js, done, &args[0], 1);
  return js_mkundef();
}

static ant_value_t stream_pipeline_schedule_done(ant_params_t) {
  ant_value_t done = nargs > 0 ? args[0] : js_mkundef();
  stream_call_callback(js, done, NULL, 0);
  return js_mkundef();
}

static ant_value_t js_stream_pipeline(ant_params_t) {
  int stream_count = nargs;
  ant_value_t callback = js_mkundef();
  ant_value_t done_state = 0;
  ant_value_t done = 0;

  if (nargs > 0 && is_callable(args[nargs - 1])) {
    callback = args[nargs - 1];
    stream_count--;
  }
  
  if (!is_callable(callback)) callback = Ant_Function_Create(stream_noop);
  if (stream_count <= 0) return js_mkundef();

  done_state = Ant_Object_CreateBare(js);
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
    ant_value_t registered = js_stream_finished(js, finished_args, 2, js_mkundef());
    if (is_err(registered)) return registered;
    
    ant_value_t pipe_args[2];
    pipe_args[0] = args[i + 1];
    pipe_args[1] = js_mkundef();
    ant_value_t piped = stream_call_prop(js, args[i], "pipe", pipe_args, 2);
    if (is_err(piped)) return piped;
  }

  {
    ant_value_t finished_args[2];
    finished_args[0] = args[stream_count - 1];
    finished_args[1] = done;
    ant_value_t registered = js_stream_finished(js, finished_args, 2, js_mkundef());
    if (is_err(registered)) return registered;
  }

  return args[stream_count - 1];
}

static ant_value_t stream_promise_callback(ant_params_t) {
  ant_value_t promise = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (nargs > 0 && !is_undefined(args[0])) js_reject_promise(js, promise, args[0]);
  else js_resolve_promise(js, promise, js_mkundef());
  return js_mkundef();
}

static ant_value_t js_stream_promises_finished(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t finished_args[2];
  if (nargs < 1 || !is_object_type(args[0])) {
    js_reject_promise(js, promise, js_mkerr(js, "finished requires a stream"));
    return promise;
  }
  finished_args[0] = args[0];
  finished_args[1] = js_heavy_mkfun(js, stream_promise_callback, promise);
  ant_value_t result = js_stream_finished(js, finished_args, 2, js_mkundef());
  if (is_err(result)) js_reject_promise(js, promise, result);
  return promise;
}

static ant_value_t js_stream_promises_pipeline(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t *call_args = NULL;

  if (nargs <= 0) {
    js_resolve_promise(js, promise, js_mkundef());
    return promise;
  }

  call_args = malloc((size_t)(nargs + 1) * sizeof(*call_args));
  if (!call_args) {
    js_reject_promise(js, promise, Ant_Error_Create(js, JS_ERR_GENERIC, "out of memory"));
    return promise;
  }

  for (int i = 0; i < nargs; i++) call_args[i] = args[i];
  GC_ROOT_SAVE(root_mark, js);
  
  call_args[nargs] = js_heavy_mkfun(js, stream_promise_callback, promise);
  GC_ROOT_PIN(js, call_args[nargs]);
  
  ant_value_t result = js_stream_pipeline(js, call_args, nargs + 1, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);

  free(call_args);
  if (is_err(result)) js_reject_promise(js, promise, result);

  return promise;
}

static void stream_release_reader(ant_t *js, ant_value_t state_obj) {
  ant_value_t reader = js_get(js, state_obj, "reader");
  if (!is_object_type(reader)) return;
  stream_call_prop(js, reader, "releaseLock", NULL, 0);
}

static ant_value_t stream_readable_from_step(ant_params_t);

static void stream_readable_from_schedule(ant_t *js, ant_value_t state_obj) {
  stream_schedule_microtask(js, stream_readable_from_step, state_obj);
}

static ant_value_t stream_readable_from_fail(ant_t *js, ant_value_t state_obj, ant_value_t error) {
  GC_ROOT_SAVE(root_mark, js);
  if (is_err(error)) error = js_take_thrown(js, error);

  GC_ROOT_PIN(js, error);
  ant_value_t readable = js_get(js, state_obj, "readable");

  stream_release_reader(js, state_obj);
  if (stream_is_instance(readable)) {
    ant_value_t destroy_args[1] = { error };
    sv_call_native_this(js, js_stream_destroy, readable, destroy_args, 1);
  }
  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t stream_readable_from_handle_result(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t readable = js_get(js, state_obj, "readable");
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t done = 0;
  ant_value_t value = 0;

  if (stream_is_destroyed(js, readable)) {
    stream_release_reader(js, state_obj);
    return js_mkundef();
  }

  if (!is_object_type(result)) return stream_readable_from_fail(
    js, state_obj, Ant_Error_Create(js, JS_ERR_TYPE, "iterator step must be an object"));
  done = js_get(js, result, "done");
  if (is_err(done)) return stream_readable_from_fail(js, state_obj, done);
  value = js_get(js, result, "value");
  if (is_err(value)) return stream_readable_from_fail(js, state_obj, value);
  if (js_truthy(js, done)) {
    stream_release_reader(js, state_obj);
    stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
    return js_mkundef();
  }

  stream_readable_push_value(js, readable, value, js_mkundef());
  if (!stream_is_destroyed(js, readable)) stream_readable_from_schedule(js, state_obj);
  return js_mkundef();
}

static ant_value_t stream_readable_from_reject(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t error = nargs > 0 ? args[0] : Ant_Error_Create(js, JS_ERR_GENERIC, "stream iteration failed");
  return stream_readable_from_fail(js, state_obj, error);
}

static ant_value_t stream_readable_from_step(ant_params_t) {
  ant_value_t state_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t readable = js_get(js, state_obj, "readable");
  ant_value_t mode = js_get(js, state_obj, "mode");
  ant_value_t iterator = js_get(js, state_obj, "iterator");
  
  ant_value_t next_result = js_mkundef();
  ant_value_t on_resolve = js_heavy_mkfun(js, stream_readable_from_handle_result, state_obj);
  ant_value_t on_reject = js_heavy_mkfun(js, stream_readable_from_reject, state_obj);
  ant_value_t then_result = 0;

  if (stream_is_destroyed(js, readable)) {
    stream_release_reader(js, state_obj);
    return js_mkundef();
  }

  if (stream_key_is_cstr(js, mode, "reader")) next_result = stream_call_prop(js, iterator, "read", NULL, 0);
  else next_result = stream_call_prop(js, iterator, "next", NULL, 0);

  if (is_err(next_result)) return stream_readable_from_fail(js, state_obj, next_result);
  if (vtype(next_result) == kTypePromise) {
    then_result = js_promise_then(js, next_result, on_resolve, on_reject);
    if (is_err(then_result)) return stream_readable_from_fail(js, state_obj, then_result);
    promise_mark_handled(then_result);
    return js_mkundef();
  }

  ant_value_t one_arg[1] = { next_result };
  return stream_readable_from_handle_result(js, one_arg, 1, js_mkundef());
}

static ant_value_t stream_readable_from_start(ant_params_t) {
  ant_value_t state_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t readable = js_get(js, state_obj, "readable");
  ant_value_t source = js_get(js, state_obj, "source");
  
  ant_value_t async_iter_fn = 0;
  ant_value_t reader_fn = 0;
  iterator_t it;

  if (stream_is_destroyed(js, readable)) return js_mkundef();

  if (vtype(source) == kTypeString || (is_object_type(source) && js_instance_of_proto(js, source, js->builtins.buffer_proto))) {
    stream_readable_push_value(js, readable, source, js_mkundef());
    stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
    return js_mkundef();
  }

  async_iter_fn = is_object_type(source) ? js_get_sym(js, source, js->sym.asyncIterator_sym) : js_mkundef();
  if (is_err(async_iter_fn)) return stream_readable_from_fail(js, state_obj, async_iter_fn);
  if (is_callable(async_iter_fn)) {
    ant_value_t iterator = stream_call(js, async_iter_fn, source, NULL, 0);
    if (is_err(iterator)) return stream_readable_from_fail(js, state_obj, iterator);
    js_set(js, state_obj, "iterator", iterator);
    js_set(js, state_obj, "mode", js_mkstr(js, "async", 5));
    stream_readable_from_schedule(js, state_obj);
    return js_mkundef();
  }

  if (js_iter_open(js, source, &it)) {
    ant_value_t value = 0;
    while (js_iter_next(js, &it, &value)) {
      if (stream_is_destroyed(js, readable)) break;
      stream_readable_push_value(js, readable, value, js_mkundef());
    }
    js_iter_close(js, &it);
    if (Ant_Exception_Pending(js)) return stream_readable_from_fail(js, state_obj, Ant_Exception_Current(js));
    if (!stream_is_destroyed(js, readable))
      stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
    return js_mkundef();
  }

  if (Ant_Exception_Pending(js)) return stream_readable_from_fail(js, state_obj, Ant_Exception_Current(js));
  reader_fn = is_object_type(source) ? js_get(js, source, "getReader") : js_mkundef();
  if (is_err(reader_fn)) return stream_readable_from_fail(js, state_obj, reader_fn);
  if (is_callable(reader_fn)) {
    ant_value_t reader = stream_call(js, reader_fn, source, NULL, 0);
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

static ant_value_t stream_readable_from_options(ant_t *js, ant_value_t opts) {
  ant_value_t options = Ant_Object_CreateBare(js);
  if (is_object_type(opts)) js_set_proto_init(options, opts);
  
  if (vtype(stream_get_option(js, opts, "objectMode")) == kTypeUndefined) js_set(js, options, "objectMode", js_true);
  if (vtype(stream_get_option(js, opts, "highWaterMark")) == kTypeUndefined) js_set(js, options, "highWaterMark", js_mknum(1));
  
  return options;
}

static ant_value_t stream_readable_from_source(ant_t *js, ant_value_t source, ant_value_t options) {
  ant_value_t readable = stream_construct(js, js->builtins.readable_proto, options, stream_init_readable, js_mkundef());
  if (is_err(readable)) return readable;

  ant_value_t state_obj = Ant_Object_CreateBare(js);
  js_set(js, state_obj, "readable", readable);
  js_set(js, state_obj, "source", source);
  js_set(js, state_obj, "iterator", js_mkundef());
  js_set(js, state_obj, "reader", js_mkundef());
  js_set(js, state_obj, "mode", js_mkundef());
  stream_schedule_microtask(js, stream_readable_from_start, state_obj);

  return readable;
}

static ant_value_t js_readable_from(ant_params_t) {
  ant_value_t options = stream_readable_from_options(js, nargs > 1 ? args[1] : js_mkundef());
  return stream_readable_from_source(js, nargs > 0 ? args[0] : js_mkundef(), options);
}

static ant_value_t js_readable_from_web(ant_params_t) {
  return stream_readable_from_source(js, nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef());
}

static bool stream_to_web_closed(ant_t *js, ant_value_t state_obj) {
  return js_truthy(js, js_get(js, state_obj, "closed"));
}

static void stream_to_web_cleanup(ant_t *js, ant_value_t state_obj) {
  ant_value_t source = js_get(js, state_obj, "source");
  ant_value_t on_data = js_get(js, state_obj, "onData");
  ant_value_t on_end = js_get(js, state_obj, "onEnd");
  ant_value_t on_error = js_get(js, state_obj, "onError");

  if (!is_object_type(source)) return;
  if (is_callable(on_data)) stream_remove_listener(js, source, "data", on_data);
  if (is_callable(on_end)) stream_remove_listener(js, source, "end", on_end);
  if (is_callable(on_error)) stream_remove_listener(js, source, "error", on_error);
}

static ant_value_t stream_to_web_on_data(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t controller = js_get(js, state_obj, "controller");
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();

  if (stream_to_web_closed(js, state_obj)) return js_mkundef();
  return rs_controller_enqueue(js, controller, chunk);
}

static ant_value_t stream_to_web_on_end(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t controller = js_get(js, state_obj, "controller");

  if (stream_to_web_closed(js, state_obj)) return js_mkundef();
  js_set(js, state_obj, "closed", js_true);
  stream_to_web_cleanup(js, state_obj);
  rs_controller_close(js, controller);
  return js_mkundef();
}

static ant_value_t stream_to_web_on_error(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t web_stream = js_get(js, state_obj, "webStream");
  ant_value_t error = nargs > 0 ? args[0] : Ant_Error_Create(js, JS_ERR_GENERIC, "stream error");

  if (stream_to_web_closed(js, state_obj)) return js_mkundef();
  js_set(js, state_obj, "closed", js_true);
  stream_to_web_cleanup(js, state_obj);
  readable_stream_error(js, web_stream, error);
  return js_mkundef();
}

static ant_value_t stream_to_web_cancel(ant_params_t) {
  ant_value_t state_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t source = js_get(js, state_obj, "source");
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t destroy_fn = 0;
  ant_value_t cancel_result = 0;

  if (stream_to_web_closed(js, state_obj)) return js_mkundef();
  js_set(js, state_obj, "closed", js_true);
  stream_to_web_cleanup(js, state_obj);

  if (!is_object_type(source)) return js_mkundef();
  destroy_fn = js_getprop_fallback(js, source, "destroy");
  if (!is_callable(destroy_fn)) return js_mkundef();

  cancel_result = stream_call(js, destroy_fn, source, &reason, 1);
  return is_err(cancel_result) ? cancel_result : js_mkundef();
}

static ant_value_t js_readable_to_web(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t state_obj = 0;
  ant_value_t cancel_fn = 0;
  ant_value_t web_stream = 0;
  ant_value_t controller = 0;
  ant_value_t on_data = 0;
  ant_value_t on_end = 0;
  ant_value_t on_error = 0;

  if (!is_object_type(source))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Readable.toWeb requires a stream");

  state_obj = Ant_Object_CreateBare(js);
  js_set(js, state_obj, "source", source);
  js_set(js, state_obj, "closed", js_false);

  cancel_fn = js_heavy_mkfun(js, stream_to_web_cancel, state_obj);
  web_stream = rs_create_stream(js, js_mkundef(), cancel_fn, 1.0);
  if (is_err(web_stream)) return web_stream;

  controller = rs_stream_controller(js, web_stream);
  js_set(js, state_obj, "webStream", web_stream);
  js_set(js, state_obj, "controller", controller);

  on_data = js_heavy_mkfun(js, stream_to_web_on_data, state_obj);
  on_end = js_heavy_mkfun(js, stream_to_web_on_end, state_obj);
  on_error = js_heavy_mkfun(js, stream_to_web_on_error, state_obj);

  js_set(js, state_obj, "onData", on_data);
  js_set(js, state_obj, "onEnd", on_end);
  js_set(js, state_obj, "onError", on_error);

  stream_add_listener(js, source, "data", on_data, false);
  stream_add_listener(js, source, "end", on_end, true);
  stream_add_listener(js, source, "error", on_error, true);

  stream_call_prop(js, source, "resume", NULL, 0);
  return web_stream;
}

static constexpr double STREAM_ITER_PAUSE_AT = 16;
static constexpr double STREAM_ITER_RESUME_AT = 4;

static ant_value_t stream_iter_resolved(ant_t *js, ant_value_t value) {
  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, value);
  return promise;
}

static ant_value_t stream_iter_rejected(ant_t *js, ant_value_t error) {
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, error);
  return promise;
}

static double stream_iter_queued(ant_t *js, ant_value_t state) {
  ant_value_t queue = js_get(js, state, "queue");
  ant_value_t head = js_get(js, state, "head");
  
  double len = vtype(queue) == kTypeArray ? (double)js_arr_len(js, queue) : 0;
  return len - (vtype(head) == kTypeNumber ? tod(head) : 0);
}

static void stream_iter_detach(ant_t *js, ant_value_t state) {
  if (js_truthy(js, js_get(js, state, "detached"))) return;
  js_set(js, state, "detached", js_true);

  ant_value_t stream_obj = js_get(js, state, "stream");
  static const char *const events[] = { "data", "end", "error", "close" };
  static const char *const keys[] = { "onData", "onEnd", "onError", "onClose" };
  
  for (size_t i = 0; i < 4; i++) stream_remove_listener(js, stream_obj, events[i], js_get(js, state, keys[i]));
}

static void stream_iter_settle(ant_t *js, ant_value_t state, ant_value_t result, bool reject) {
  ant_value_t pending = js_get(js, state, "pending");
  if (vtype(pending) != kTypePromise) return;
  
  js_set(js, state, "pending", js_mkundef());
  if (reject) js_reject_promise(js, pending, result);
  
  else js_resolve_promise(js, pending, result);
}

static ant_value_t stream_iter_on_data(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();

  if (vtype(js_get(js, state, "pending")) == kTypePromise) {
    stream_iter_settle(js, state, js_iter_result(js, true, chunk), false);
    return js_mkundef();
  }

  js_arr_push(js, js_get(js, state, "queue"), chunk);
  if (stream_iter_queued(js, state) >= STREAM_ITER_PAUSE_AT && !js_truthy(js, js_get(js, state, "paused"))) {
    js_set(js, state, "paused", js_true);
    stream_call_prop(js, js_get(js, state, "stream"), "pause", NULL, 0);
  }
  
  return js_mkundef();
}

static ant_value_t stream_iter_on_end(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  js_set(js, state, "ended", js_true);
  
  stream_iter_detach(js, state);
  stream_iter_settle(js, state, js_iter_result(js, false, js_mkundef()), false);
  
  return js_mkundef();
}

static ant_value_t stream_iter_on_error(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();
  stream_iter_detach(js, state);

  if (vtype(js_get(js, state, "pending")) == kTypePromise) {
    js_set(js, state, "ended", js_true);
    stream_iter_settle(js, state, error, true);
  } else {
    js_set(js, state, "error", error);
    js_set(js, state, "hasError", js_true);
  }
  
  return js_mkundef();
}

static ant_value_t stream_iter_next(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);

  if (stream_iter_queued(js, state) > 0) {
    ant_value_t queue = js_get(js, state, "queue");
    ant_offset_t head = (ant_offset_t)tod(js_get(js, state, "head"));
    ant_value_t chunk = js_arr_get(js, queue, head);
    head++;

    if (head >= js_arr_len(js, queue)) {
      js_set(js, state, "queue", js_mkarr(js));
      head = 0;
    }
    js_set(js, state, "head", js_mknum((double)head));

    if (
      js_truthy(js, js_get(js, state, "paused")) &&
      stream_iter_queued(js, state) <= STREAM_ITER_RESUME_AT &&
      !js_truthy(js, js_get(js, state, "detached"))
    ) {
      js_set(js, state, "paused", js_false);
      stream_call_prop(js, js_get(js, state, "stream"), "resume", NULL, 0);
    }
    
    return stream_iter_resolved(js, js_iter_result(js, true, chunk));
  }

  if (js_truthy(js, js_get(js, state, "hasError"))) {
    ant_value_t error = js_get(js, state, "error");
    js_set(js, state, "hasError", js_false);
    js_set(js, state, "error", js_mkundef());
    js_set(js, state, "ended", js_true);
    
    return stream_iter_rejected(js, error);
  }

  if (js_truthy(js, js_get(js, state, "ended")))
    return stream_iter_resolved(js, js_iter_result(js, false, js_mkundef()));

  ant_value_t pending = js_get(js, state, "pending");
  if (vtype(pending) == kTypePromise) return pending;

  pending = js_mkpromise(js);
  js_set(js, state, "pending", pending);
  
  return pending;
}

static ant_value_t stream_iter_return(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t done = js_iter_result(js, false, nargs > 0 ? args[0] : js_mkundef());
  ant_value_t stream_obj = js_get(js, state, "stream");

  js_set(js, state, "ended", js_true);
  js_set(js, state, "queue", js_mkarr(js));
  js_set(js, state, "head", js_mknum(0));

  stream_iter_detach(js, state);
  stream_iter_settle(js, state, js_iter_result(js, false, js_mkundef()), false);

  if (!stream_is_destroyed(js, stream_obj)) {
    ant_value_t result = stream_call_prop(js, stream_obj, "destroy", NULL, 0);
    if (is_err(result)) return stream_iter_rejected(js, js_take_thrown(js, result));
  }

  return stream_iter_resolved(js, done);
}

static ant_value_t js_readable_async_iterator(ant_params_t) {
  ant_value_t stream_obj = js_getthis(js);
  if (!is_object_type(stream_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Readable[Symbol.asyncIterator] called on incompatible receiver");

  ant_value_t state = Ant_Object_CreateBare(js);
  ant_value_t iterator = Ant_Object_CreateBare(js);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, state);
  GC_ROOT_PIN(js, iterator);

  js_set(js, state, "stream", stream_obj);
  js_set(js, state, "queue", js_mkarr(js));
  js_set(js, state, "head", js_mknum(0));
  js_set(js, state, "pending", js_mkundef());
  js_set(js, state, "error", js_mkundef());
  js_set(js, state, "hasError", js_false);
  js_set(js, state, "paused", js_false);
  js_set(js, state, "detached", js_false);
  js_set(js, state, "onData", js_heavy_mkfun(js, stream_iter_on_data, state));
  js_set(js, state, "onEnd", js_heavy_mkfun(js, stream_iter_on_end, state));
  js_set(js, state, "onError", js_heavy_mkfun(js, stream_iter_on_error, state));
  js_set(js, state, "onClose", js_get(js, state, "onEnd"));

  bool ended = js_truthy(js, js_get(js, stream_obj, "readableEnded")) || stream_is_destroyed(js, stream_obj);
  js_set(js, state, "ended", js_bool(ended));

  js_set(js, iterator, "next", js_heavy_mkfun(js, stream_iter_next, state));
  js_set(js, iterator, "return", js_heavy_mkfun(js, stream_iter_return, state));
  js_set_sym(js, iterator, js->sym.asyncIterator_sym, Ant_Function_Create(sym_this_cb));

  if (!ended) {
    static const char *const events[] = { "error", "end", "close", "data" };
    static const char *const keys[] = { "onError", "onEnd", "onClose", "onData" };
    for (size_t i = 0; i < 4; i++) {
      ant_value_t added = stream_add_listener(js, stream_obj, events[i], js_get(js, state, keys[i]), false);
      if (is_err(added)) { GC_ROOT_RESTORE(js, root_mark); return added; }
    }
  } else js_set(js, state, "detached", js_true);

  GC_ROOT_RESTORE(js, root_mark);
  return iterator;
}

static ant_value_t js_stream_ctor(ant_params_t) {
  return stream_construct(js, js->builtins.stream_proto, nargs > 0 ? args[0] : js_mkundef(), stream_init_base, call_new_target);
}

static ant_value_t js_readable_ctor(ant_params_t) {
  return stream_construct(js, js->builtins.readable_proto, nargs > 0 ? args[0] : js_mkundef(), stream_init_readable, call_new_target);
}

static ant_value_t js_writable_ctor(ant_params_t) {
  return stream_construct(js, js->builtins.writable_proto, nargs > 0 ? args[0] : js_mkundef(), stream_init_writable, call_new_target);
}

static ant_value_t js_writable_has_instance(ant_params_t) {
  ant_value_t obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t ctor = js_getthis(js);
  if (!is_object_type(obj)) return js_false;

  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto) && js_instance_of_proto(js, obj, proto)) return js_true;
  if (ctor != js->builtins.writable_ctor) return js_false;
  
  return js_bool(js_instance_of_proto(js, obj, js->builtins.duplex_proto));
}

static void stream_init_duplex(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_readable(js, obj, options);
  stream_init_writable(js, obj, options);
  js_set(js, obj, "readable", js_true);
}

static ant_value_t stream_construct_duplex_like(ant_t *js, ant_value_t base_proto, ant_value_t options, ant_value_t new_target) {
  ant_value_t obj = stream_construct(js, base_proto, options, stream_init_duplex, new_target);
  js_set(js, obj, "allowHalfOpen", js_bool(stream_get_option(js, options, "allowHalfOpen") != js_false));
  return obj;
}

static ant_value_t js_duplex_ctor(ant_params_t) {
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  return stream_construct_duplex_like(js, js->builtins.duplex_proto, options, call_new_target);
}

static ant_value_t js_transform_ctor(ant_params_t) {
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t obj = stream_construct_duplex_like(js, js->builtins.stream_transform_proto, options, call_new_target);
  ant_value_t transform_fn = stream_get_option(js, options, "transform");
  ant_value_t flush_fn = stream_get_option(js, options, "flush");
  
  if (is_callable(transform_fn)) js_set(js, obj, "_transform", transform_fn);
  if (is_callable(flush_fn)) js_set(js, obj, "_flush", flush_fn);
  
  return obj;
}

static ant_value_t js_passthrough_ctor(ant_params_t) {
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  return stream_construct_duplex_like(js, js->builtins.passthrough_proto, options, call_new_target);
}

void stream_init_constructors(ant_t *js) {
  ant_value_t events = 0;
  ant_value_t ee_ctor = 0;
  ant_value_t ee_proto = 0;

  if (js->builtins.stream_ctor) return;

  events = events_library(js);
  ee_ctor = js_get(js, events, "EventEmitter");
  ee_proto = js_get(js, ee_ctor, "prototype");

  js->builtins.readable_state_proto = stream_make_state_proto(js, STREAM_READABLE_STATE_TAG);
  js->builtins.writable_state_proto = stream_make_state_proto(js, STREAM_WRITABLE_STATE_TAG);

  js->builtins.stream_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.stream_proto, ee_proto);
  js_set(js, js->builtins.stream_proto, "pipe", Ant_Function_CreateWithArity(js_stream_pipe, 2));
  js_set(js, js->builtins.stream_proto, "unpipe", Ant_Function_Create(js_stream_unpipe));
  js_set(js, js->builtins.stream_proto, "pause", Ant_Function_Create(js_stream_pause));
  js_set(js, js->builtins.stream_proto, "resume", Ant_Function_Create(js_stream_resume));
  js_set(js, js->builtins.stream_proto, "isPaused", Ant_Function_Create(js_stream_is_paused));
  js_set(js, js->builtins.stream_proto, "destroy", Ant_Function_Create(js_stream_destroy));
  js_set_tostringtag(js, js->builtins.stream_proto, js_mkstr(js, "Stream", 6));
  js->builtins.stream_ctor = Ant_Function_CreateConstructor(js, js_stream_ctor, js->builtins.stream_proto, "Stream", 1);

  js->builtins.readable_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.readable_proto, js->builtins.stream_proto);
  js_set(js, js->builtins.readable_proto, "_read", Ant_Function_CreateWithArity(js_readable__read, 1));
  js_set(js, js->builtins.readable_proto, "_destroy", Ant_Function_CreateWithArity(js_stream__destroy, 2));
  js->builtins.stream_default_destroy = js_get(js, js->builtins.readable_proto, "_destroy");
  js_set(js, js->builtins.readable_proto, "push", Ant_Function_CreateWithArity(js_readable_push, 2));
  js_set(js, js->builtins.readable_proto, "read", Ant_Function_CreateWithArity(js_readable_read, 1));
  js_set(js, js->builtins.readable_proto, "setEncoding", Ant_Function_CreateWithArity(js_readable_set_encoding, 1));
  js_set(js, js->builtins.readable_proto, "on", Ant_Function_CreateWithArity(js_readable_on, 2));
  js_set_exact(js, js->builtins.readable_proto, "addListener", js_get(js, js->builtins.readable_proto, "on"));
  js_set(js, js->builtins.readable_proto, "once", Ant_Function_Create(js_readable_once));
  js_set(js, js->builtins.readable_proto, "resume", Ant_Function_Create(js_readable_resume));
  js_set(js, js->builtins.readable_proto, "pause", Ant_Function_Create(js_readable_pause));
  js_set_sym(js, js->builtins.readable_proto, js->sym.asyncIterator_sym, Ant_Function_Create(js_readable_async_iterator));
  js_set_tostringtag(js, js->builtins.readable_proto, js_mkstr(js, "Readable", 8));
  js->builtins.readable_ctor = Ant_Function_CreateConstructor(js, js_readable_ctor, js->builtins.readable_proto, "Readable", 1);
  js_set(js, js->builtins.readable_ctor, "from", Ant_Function_Create(js_readable_from));
  js_set(js, js->builtins.readable_ctor, "fromWeb", Ant_Function_Create(js_readable_from_web));
  js_set(js, js->builtins.readable_ctor, "toWeb", Ant_Function_Create(js_readable_to_web));

  js->builtins.writable_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.writable_proto, js->builtins.stream_proto);
  js_set(js, js->builtins.writable_proto, "_write", Ant_Function_CreateWithArity(js_writable__write, 3));
  js_set(js, js->builtins.writable_proto, "_final", Ant_Function_Create(js_writable__final));
  js_set(js, js->builtins.writable_proto, "_destroy", js->builtins.stream_default_destroy);
  js_set(js, js->builtins.writable_proto, "write", Ant_Function_CreateWithArity(js_writable_write, 3));
  stream_define_writable_accessors(js, js->builtins.writable_proto);
  js_set(js, js->builtins.writable_proto, "end", Ant_Function_CreateWithArity(js_writable_end, 3));
  js_set(js, js->builtins.writable_proto, "cork", Ant_Function_Create(stream_noop));
  js_set(js, js->builtins.writable_proto, "uncork", Ant_Function_Create(stream_noop));
  js_set_tostringtag(js, js->builtins.writable_proto, js_mkstr(js, "Writable", 8));
  js->builtins.writable_ctor = Ant_Function_CreateConstructor(js, js_writable_ctor, js->builtins.writable_proto, "Writable", 1);
  js_set_sym(js, js->builtins.writable_ctor, js->sym.hasInstance_sym, Ant_Function_CreateWithArity(js_writable_has_instance, 1));

  js->builtins.duplex_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.duplex_proto, js->builtins.readable_proto);
  js_set(js, js->builtins.duplex_proto, "_write", Ant_Function_CreateWithArity(js_writable__write, 3));
  js_set(js, js->builtins.duplex_proto, "_final", Ant_Function_Create(js_writable__final));
  js_set(js, js->builtins.duplex_proto, "write", Ant_Function_CreateWithArity(js_writable_write, 3));
  stream_define_writable_accessors(js, js->builtins.duplex_proto);
  js_set(js, js->builtins.duplex_proto, "end", Ant_Function_CreateWithArity(js_writable_end, 3));
  js_set(js, js->builtins.duplex_proto, "cork", Ant_Function_Create(stream_noop));
  js_set(js, js->builtins.duplex_proto, "uncork", Ant_Function_Create(stream_noop));
  js_set_tostringtag(js, js->builtins.duplex_proto, js_mkstr(js, "Duplex", 6));
  js->builtins.duplex_ctor = Ant_Function_CreateConstructor(js, js_duplex_ctor, js->builtins.duplex_proto, "Duplex", 1);

  js->builtins.stream_transform_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.stream_transform_proto, js->builtins.duplex_proto);
  js_set(js, js->builtins.stream_transform_proto, "_transform", Ant_Function_CreateWithArity(js_transform__transform, 3));
  js_set(js, js->builtins.stream_transform_proto, "_write", Ant_Function_CreateWithArity(js_transform__write, 3));
  js_set(js, js->builtins.stream_transform_proto, "_final", Ant_Function_CreateWithArity(js_transform__final, 1));
  js_set_tostringtag(js, js->builtins.stream_transform_proto, js_mkstr(js, "Transform", 9));
  js->builtins.stream_transform_ctor = Ant_Function_CreateConstructor(js, js_transform_ctor, js->builtins.stream_transform_proto, "Transform", 1);

  js->builtins.passthrough_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.passthrough_proto, js->builtins.stream_transform_proto);
  js_set(js, js->builtins.passthrough_proto, "_transform", Ant_Function_CreateWithArity(js_passthrough__transform, 3));
  js_set_tostringtag(js, js->builtins.passthrough_proto, js_mkstr(js, "PassThrough", 11));
  js->builtins.passthrough_ctor = Ant_Function_CreateConstructor(js, js_passthrough_ctor, js->builtins.passthrough_proto, "PassThrough", 1);
  js->builtins.stream_utf8 = js_mkstr(js, "utf8", 4);
}

ant_value_t stream_readable_constructor(ant_t *js) {
  stream_init_constructors(js);
  return js->builtins.readable_ctor;
}

ant_value_t stream_writable_constructor(ant_t *js) {
  stream_init_constructors(js);
  return js->builtins.writable_ctor;
}

ant_value_t stream_readable_prototype(ant_t *js) {
  stream_init_constructors(js);
  return js->builtins.readable_proto;
}

ant_value_t stream_writable_prototype(ant_t *js) {
  stream_init_constructors(js);
  return js->builtins.writable_proto;
}

ant_value_t stream_duplex_prototype(ant_t *js) {
  stream_init_constructors(js);
  return js->builtins.duplex_proto;
}

ant_value_t stream_transform_prototype(ant_t *js) {
  stream_init_constructors(js);
  return js->builtins.stream_transform_proto;
}

ant_value_t stream_construct_readable(ant_t *js, ant_value_t base_proto, ant_value_t options, ant_value_t new_target) {
  stream_init_constructors(js);
  return stream_construct(js, base_proto, options, stream_init_readable, new_target);
}

ant_value_t stream_construct_writable(ant_t *js, ant_value_t base_proto, ant_value_t options, ant_value_t new_target) {
  stream_init_constructors(js);
  return stream_construct(js, base_proto, options, stream_init_writable, new_target);
}


ant_value_t stream_construct_duplex(ant_t *js, ant_value_t base_proto, ant_value_t options, ant_value_t new_target) {
  stream_init_constructors(js);
  return stream_construct(js, base_proto, options, stream_init_duplex, new_target);
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

static ant_value_t stream_construct_done(ant_params_t) {
  ant_value_t stream_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  stream_private_state_t *priv = stream_private_state(stream_obj);
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  bool failed = !is_undefined(err) && !is_null(err);
  if (priv) priv->constructing = false;

  if (priv && priv->destroy_pending) {
    ant_value_t pending_err = js_get_slot(stream_obj, SLOT_STREAM_DESTROY_ERROR);
    priv->destroy_pending = false;
    js_set_slot(stream_obj, SLOT_STREAM_DESTROY_ERROR, js_mkundef());
    if (failed) stream_set_errored(js, stream_obj, err);
    return stream_run_destroy(js, stream_obj, failed ? err : pending_err);
  }

  if (failed) {
    sv_call_native_this(js, js_stream_destroy, stream_obj, &err, 1);
    return js_mkundef();
  }

  stream_readable_state_t *rs = stream_rs(stream_obj);
  if (rs && (rs->flowing || rs->readable_listening)) stream_readable_maybe_read(js, stream_obj);

  if (stream_ws(stream_obj)) return stream_writable_drain(js, stream_obj);
  return js_mkundef();
}

static ant_value_t stream_construct_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t done = stream_make_once(js, js_heavy_mkfun(js, stream_construct_done, stream_obj), js_mkundef());
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, done);

  ant_value_t result = stream_call_prop(js, stream_obj, "_construct", &done, 1);
  if (is_err(result)) {
    ant_value_t err = js_take_thrown(js, result);
    stream_call_callback(js, done, &err, 1);
  }

  GC_ROOT_RESTORE(js, mark);
  return js_mkundef();
}

void stream_begin_construct(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv || priv->constructing) return;
  priv->constructing = true;
  stream_schedule_microtask(js, stream_construct_tick, stream_obj);
}

ant_value_t stream_construct(
  ant_t *js, ant_value_t base_proto, ant_value_t raw_options,
  void (*init_fn)(ant_t *, ant_value_t, ant_value_t), ant_value_t new_target
) {
  ant_value_t proto = js_instance_proto_from_new_target(js, base_proto, new_target);
  bool subclassed = is_object_type(proto) && proto != base_proto;
  ant_value_t obj = stream_make_base_object(js, subclassed ? proto : base_proto);

  init_fn(js, obj, raw_options);
  if (subclassed && is_callable(js_getprop_fallback(js, obj, "_construct"))) stream_begin_construct(js, obj);
  
  return obj;
}

ant_value_t stream_destroy(ant_t *js, ant_value_t stream_obj, ant_value_t error) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, error);
  ant_value_t result = stream_call_prop(js, stream_obj, "destroy", &error, 1);
  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t js_stream_get_default_high_water_mark(ant_params_t) {
  bool object_mode = nargs > 0 && js_truthy(js, args[0]);
  return js_mknum(stream_default_high_water_mark(js, object_mode));
}

static ant_value_t js_stream_set_default_high_water_mark(ant_params_t) {
  if (nargs < 2 || vtype(args[1]) != kTypeNumber || js_getnum(args[1]) < 0)
    return js_mkerr_typed(js, JS_ERR_RANGE, "setDefaultHighWaterMark requires a non-negative number");

  bool object_mode = js_truthy(js, args[0]);
  if (object_mode) js->defaults.stream_object_high_water_mark = js_getnum(args[1]);
  else js->defaults.stream_high_water_mark = js_getnum(args[1]);

  return js_mkundef();
}

static ant_value_t js_stream_is_destroyed(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(stream_obj)) return js_false;
  return js_bool(stream_is_destroyed(js, stream_obj));
}

static bool stream_state_flag(ant_t *js, ant_value_t stream_obj, const char *state_key, const char *field) {
  return js_truthy(js, stream_state_field(js, stream_obj, state_key, field));
}

static ant_value_t js_stream_is_disturbed(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(stream_obj)) return js_false;
  
  if (stream_state_flag(js, stream_obj, "_readableState", "dataEmitted")) return js_true;
  if (stream_state_flag(js, stream_obj, "_readableState", "endEmitted")) return js_false;
  
  return js_bool(stream_is_destroyed(js, stream_obj));
}

static ant_value_t js_stream_is_errored(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(stream_obj)) return js_false;
  
  return js_bool(
    stream_state_flag(js, stream_obj, "_readableState", "errored") ||
    stream_state_flag(js, stream_obj, "_writableState", "errored")
  );
}

static ant_value_t js_stream_is_readable(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();

  if (!is_object_type(stream_obj)) return js_false;
  if (stream_is_destroyed(js, stream_obj)) return js_false;
  if (js_truthy(js, js_get(js, stream_obj, "readableEnded"))) return js_false;
  
  if (stream_state_flag(js, stream_obj, "_readableState", "ended")) return js_false;
  if (stream_state_flag(js, stream_obj, "_readableState", "errored")) return js_false;
  if (stream_state_flag(js, stream_obj, "_writableState", "errored")) return js_false;
  
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
  ant_value_t lib = Ant_Object_CreateBare(js);
  ant_value_t promises = Ant_Object_CreateBare(js);
  stream_init_constructors(js);

  js_set(js, promises, "pipeline", Ant_Function_Create(js_stream_promises_pipeline));
  js_set(js, promises, "finished", Ant_Function_Create(js_stream_promises_finished));

  js_set_module_default(js, lib, js->builtins.stream_ctor, "Stream");
  js_set(js, lib, "Readable", js->builtins.readable_ctor);
  js_set(js, lib, "Writable", js->builtins.writable_ctor);
  js_set(js, lib, "Duplex", js->builtins.duplex_ctor);
  js_set(js, lib, "Transform", js->builtins.stream_transform_ctor);
  js_set(js, lib, "PassThrough", js->builtins.passthrough_ctor);
  js_set(js, lib, "pipeline", Ant_Function_Create(js_stream_pipeline));
  js_set(js, lib, "finished", Ant_Function_CreateWithArity(js_stream_finished, 3));
  js_set(js, lib, "getDefaultHighWaterMark", Ant_Function_CreateWithArity(js_stream_get_default_high_water_mark, 1));
  js_set(js, lib, "setDefaultHighWaterMark", Ant_Function_CreateWithArity(js_stream_set_default_high_water_mark, 2));
  js_set(js, lib, "isDestroyed", Ant_Function_CreateWithArity(js_stream_is_destroyed, 1));
  js_set(js, lib, "isDisturbed", Ant_Function_CreateWithArity(js_stream_is_disturbed, 1));
  js_set(js, lib, "isErrored", Ant_Function_CreateWithArity(js_stream_is_errored, 1));
  js_set(js, lib, "isReadable", Ant_Function_CreateWithArity(js_stream_is_readable, 1));
  js_set(js, lib, "promises", promises);

  js_set(js, js->builtins.stream_ctor, "Readable", js->builtins.readable_ctor);
  js_set(js, js->builtins.stream_ctor, "Writable", js->builtins.writable_ctor);
  js_set(js, js->builtins.stream_ctor, "Duplex", js->builtins.duplex_ctor);
  js_set(js, js->builtins.stream_ctor, "Transform", js->builtins.stream_transform_ctor);
  js_set(js, js->builtins.stream_ctor, "PassThrough", js->builtins.passthrough_ctor);
  js_set(js, js->builtins.stream_ctor, "pipeline", js_get(js, lib, "pipeline"));
  js_set(js, js->builtins.stream_ctor, "finished", js_get(js, lib, "finished"));
  js_set(js, js->builtins.stream_ctor, "getDefaultHighWaterMark", js_get(js, lib, "getDefaultHighWaterMark"));
  js_set(js, js->builtins.stream_ctor, "setDefaultHighWaterMark", js_get(js, lib, "setDefaultHighWaterMark"));
  js_set(js, js->builtins.stream_ctor, "isDestroyed", js_get(js, lib, "isDestroyed"));
  js_set(js, js->builtins.stream_ctor, "isDisturbed", js_get(js, lib, "isDisturbed"));
  js_set(js, js->builtins.stream_ctor, "isErrored", js_get(js, lib, "isErrored"));
  js_set(js, js->builtins.stream_ctor, "isReadable", js_get(js, lib, "isReadable"));
  js_set(js, js->builtins.stream_ctor, "promises", promises);

  js_set(js, promises, "default", promises);
  js_set_slot_wb(js, promises, SLOT_DEFAULT, promises);
  js_set_tostringtag(js, lib, js_mkstr(js, "stream", 6));
  
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
  ant_value_t lib = Ant_Object_CreateBare(js);

  stream_web_define_common(js, lib);
  js_set(js, lib, "default", lib);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, lib);
  js_set_tostringtag(js, lib, js_mkstr(js, "stream/web", 10));

  return lib;
}
