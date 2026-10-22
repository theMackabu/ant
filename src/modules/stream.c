#include <math.h>
#include <stdarg.h>
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

enum {
  STREAM_WRITE_ENTRY_CHUNK,
  STREAM_WRITE_ENTRY_ENCODING,
  STREAM_WRITE_ENTRY_CALLBACK,
  STREAM_WRITE_ENTRY_SIZE,
  STREAM_WRITE_ENTRY_STRIDE,
};

enum { STREAM_ONCE_FN, STREAM_ONCE_THIS, STREAM_ONCE_CALLED };
enum { STREAM_DESTROY_STREAM, STREAM_DESTROY_CALLED };
enum { STREAM_CALL_STREAM, STREAM_CALL_CALLBACK };
enum { STREAM_PIPELINE_CALLBACK, STREAM_PIPELINE_CALLED };

enum {
  STREAM_PIPE_SOURCE,
  STREAM_PIPE_DEST,
  STREAM_PIPE_END,
  STREAM_PIPE_CLEANED,
  STREAM_PIPE_ON_DATA,
  STREAM_PIPE_ON_DRAIN,
  STREAM_PIPE_ON_END,
  STREAM_PIPE_ON_CLOSE,
  STREAM_PIPE_ON_ERROR,
};

enum {
  STREAM_FINISHED_STREAM,
  STREAM_FINISHED_CALLBACK,
  STREAM_FINISHED_CALLED,
  STREAM_FINISHED_ON_FINISH,
  STREAM_FINISHED_ON_ERROR,
};

enum {
  STREAM_FROM_READABLE,
  STREAM_FROM_SOURCE,
  STREAM_FROM_ITERATOR,
  STREAM_FROM_READER,
  STREAM_FROM_READER_MODE,
};

enum {
  STREAM_WEB_SOURCE,
  STREAM_WEB_STREAM,
  STREAM_WEB_CONTROLLER,
  STREAM_WEB_CLOSED,
  STREAM_WEB_ON_DATA,
  STREAM_WEB_ON_END,
  STREAM_WEB_ON_ERROR,
};

enum {
  STREAM_ITER_STREAM,
  STREAM_ITER_QUEUE,
  STREAM_ITER_HEAD,
  STREAM_ITER_PENDING,
  STREAM_ITER_ERROR,
  STREAM_ITER_HAS_ERROR,
  STREAM_ITER_PAUSED,
  STREAM_ITER_DETACHED,
  STREAM_ITER_ENDED,
  STREAM_ITER_ON_DATA,
  STREAM_ITER_ON_END,
  STREAM_ITER_ON_ERROR,
};

typedef enum {
  STREAM_WRITE_THROWN = -1,
  STREAM_WRITE_PENDING = 0,
  STREAM_WRITE_COMPLETE = 1,
} stream_write_status_t;

typedef struct {
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
  ant_value_t buffer;
  ant_value_t errored;
  ant_value_t onwrite;
  ant_value_t write_callback;
  ant_value_t end_callback;
  double length;
  double high_water_mark;
  double write_size;
  ant_offset_t buffer_head;
  uint32_t write_seq;
  uint32_t write_done_seq;
  bool object_mode;
  bool auto_destroy;
  bool ended;
  bool finished;
  bool need_drain;
  bool write_pending;
  bool writing;
  bool pending_final;
  bool final_started;
  bool draining;
  bool sync;
} stream_writable_state_t;

typedef struct {
  ant_object_t *obj;
  stream_readable_state_t readable;
  stream_writable_state_t writable;
  ant_value_t pipes;
  ant_value_t destroy_error;
  ant_value_t readable_state_obj;
  ant_value_t writable_state_obj;
  ant_value_t transform_done;
  ant_value_t transform_callback;
  void *attached_state;
  stream_finalize_fn attached_state_finalize;
  bool has_readable;
  bool has_writable;
  bool is_readable;
  bool destroyed;
  bool paused;
  bool constructing;
  bool destroy_pending;
  bool transform_pending;
} stream_private_state_t;

typedef enum {
  STREAM_FIELD_BOOL,
  STREAM_FIELD_NUM,
  STREAM_FIELD_VALUE,
  STREAM_FIELD_BUFFER,
} stream_field_kind_t;

typedef enum {
  STREAM_ON_STREAM,
  STREAM_ON_READABLE,
  STREAM_ON_WRITABLE,
  STREAM_ON_READABLE_STATE,
  STREAM_ON_WRITABLE_STATE,
} stream_field_target_t;

typedef struct {
  const char *name;
  stream_field_target_t target;
  stream_field_kind_t kind;
  size_t offset;
  bool readonly;
} stream_field_t;

#define STREAM_FIELD(target, name, kind, field, readonly) \
  { name, STREAM_ON_##target, STREAM_FIELD_##kind, offsetof(stream_private_state_t, field), readonly }

static const stream_field_t stream_fields[] = {
  STREAM_FIELD(STREAM,         "destroyed",             BOOL,   destroyed,                   false),
  STREAM_FIELD(STREAM,         "readable",              BOOL,   is_readable,                 false),
  STREAM_FIELD(READABLE,       "readableEnded",         BOOL,   readable.end_emitted,        true),
  STREAM_FIELD(WRITABLE,       "writableEnded",         BOOL,   writable.ended,              true),
  STREAM_FIELD(WRITABLE,       "writableFinished",      BOOL,   writable.finished,           true),
  STREAM_FIELD(WRITABLE,       "writableHighWaterMark", NUM,    writable.high_water_mark,    true),
  STREAM_FIELD(WRITABLE,       "writableLength",        NUM,    writable.length,             true),
  STREAM_FIELD(WRITABLE,       "writableNeedDrain",     BOOL,   writable.need_drain,         true),
  STREAM_FIELD(READABLE_STATE, "objectMode",            BOOL,   readable.object_mode,        false),
  STREAM_FIELD(READABLE_STATE, "highWaterMark",         NUM,    readable.high_water_mark,    false),
  STREAM_FIELD(READABLE_STATE, "buffer",                BUFFER, readable.buffer,             false),
  STREAM_FIELD(READABLE_STATE, "length",                NUM,    readable.length,             false),
  STREAM_FIELD(READABLE_STATE, "flowing",               BOOL,   readable.flowing,            false),
  STREAM_FIELD(READABLE_STATE, "ended",                 BOOL,   readable.ended,              false),
  STREAM_FIELD(READABLE_STATE, "endEmitted",            BOOL,   readable.end_emitted,        false),
  STREAM_FIELD(READABLE_STATE, "reading",               BOOL,   readable.reading,            false),
  STREAM_FIELD(READABLE_STATE, "readableListening",     BOOL,   readable.readable_listening, false),
  STREAM_FIELD(READABLE_STATE, "dataEmitted",           BOOL,   readable.data_emitted,       false),
  STREAM_FIELD(READABLE_STATE, "autoDestroy",           BOOL,   readable.auto_destroy,       false),
  STREAM_FIELD(READABLE_STATE, "destroyed",             BOOL,   destroyed,                   false),
  STREAM_FIELD(READABLE_STATE, "errored",               VALUE,  readable.errored,            false),
  STREAM_FIELD(READABLE_STATE, "decoder",               VALUE,  readable.decoder,            false),
  STREAM_FIELD(WRITABLE_STATE, "objectMode",            BOOL,   writable.object_mode,        false),
  STREAM_FIELD(WRITABLE_STATE, "highWaterMark",         NUM,    writable.high_water_mark,    false),
  STREAM_FIELD(WRITABLE_STATE, "length",                NUM,    writable.length,             false),
  STREAM_FIELD(WRITABLE_STATE, "needDrain",             BOOL,   writable.need_drain,         false),
  STREAM_FIELD(WRITABLE_STATE, "ended",                 BOOL,   writable.ended,              false),
  STREAM_FIELD(WRITABLE_STATE, "finished",              BOOL,   writable.finished,           false),
  STREAM_FIELD(WRITABLE_STATE, "autoDestroy",           BOOL,   writable.auto_destroy,       false),
  STREAM_FIELD(WRITABLE_STATE, "destroyed",             BOOL,   destroyed,                   false),
  STREAM_FIELD(WRITABLE_STATE, "errored",               VALUE,  writable.errored,            false),
};

static ant_value_t stream_noop(ant_params_t) {
  return js_mkundef();
}

static bool stream_is_instance(ant_value_t value) {
  return is_object_type(value) && js_check_native_tag(value, STREAM_NATIVE_TAG);
}

static stream_private_state_t *stream_private_state(ant_value_t value) {
  return is_object_type(value) ? js_get_native(value, STREAM_NATIVE_TAG) : NULL;
}

static stream_private_state_t *stream_readable_of(ant_value_t value) {
  stream_private_state_t *priv = stream_private_state(value);
  return priv && priv->has_readable ? priv : NULL;
}

static stream_private_state_t *stream_writable_of(ant_value_t value) {
  stream_private_state_t *priv = stream_private_state(value);
  return priv && priv->has_writable ? priv : NULL;
}

static ant_value_t stream_value(stream_private_state_t *priv) {
  return js_obj_from_ptr(priv->obj);
}

static ant_value_t stream_require_this(ant_t *js, ant_value_t value, const char *label) {
  if (!stream_is_instance(value))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid %s", label);
  return value;
}

static inline void stream_store(ant_t *js, stream_private_state_t *priv, ant_value_t *field, ant_value_t value) {
  *field = value;
  gc_write_barrier(js, priv->obj, value);
}

static bool stream_is_destroyed(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  return priv ? priv->destroyed : js_truthy(js, js_get(js, stream_obj, "destroyed"));
}

static ant_value_t stream_record(ant_t *js, int count, ...) {
  ant_value_t record = js_mkarr(js);
  va_list ap;

  va_start(ap, count);
  for (int i = 0; i < count; i++) js_arr_push(js, record, va_arg(ap, ant_value_t));
  va_end(ap);

  return record;
}

static inline ant_value_t stream_record_get(ant_t *js, ant_value_t record, int field) {
  return js_arr_get(js, record, (ant_offset_t)field);
}

static inline void stream_record_set(ant_t *js, ant_value_t record, int field, ant_value_t value) {
  js_arr_set_index(js, record, (ant_offset_t)field, value);
}

static void stream_tick(ant_t *js, ant_cfunc_t fn, int count, ...) {
  ant_value_t args[2];
  va_list ap;

  va_start(ap, count);
  for (int i = 0; i < count; i++) args[i] = va_arg(ap, ant_value_t);
  va_end(ap);

  queue_microtask_with_args(js, js_mkfun_dyn(fn), args, count);
}

static stream_private_state_t *stream_attach(ant_t *js, ant_value_t obj) {
  stream_private_state_t *priv = stream_private_state(obj);
  ant_value_t readable_state_obj = js_mkundef();
  ant_value_t writable_state_obj = js_mkundef();
  void *attached_state = NULL;
  stream_finalize_fn attached_state_finalize = NULL;

  if (priv) {
    readable_state_obj = priv->readable_state_obj;
    writable_state_obj = priv->writable_state_obj;
    attached_state = priv->attached_state;
    attached_state_finalize = priv->attached_state_finalize;
    memset(priv, 0, sizeof(*priv));
  } else {
    if (!(priv = calloc(1, sizeof(*priv)))) return NULL;
    js_set_native(obj, priv, STREAM_NATIVE_TAG);
  }

  priv->obj = js_obj_ptr(obj);
  priv->attached_state = attached_state;
  priv->attached_state_finalize = attached_state_finalize;
  priv->readable_state_obj = readable_state_obj;
  priv->writable_state_obj = writable_state_obj;
  priv->pipes = js_mkundef();
  priv->destroy_error = js_mkundef();
  priv->transform_done = js_mkundef();
  priv->transform_callback = js_mkundef();
  priv->readable.buffer = js_mkundef();
  priv->readable.decoder = js_mkundef();
  priv->readable.errored = js_mkundef();
  priv->writable.buffer = js_mkundef();
  priv->writable.errored = js_mkundef();
  priv->writable.onwrite = js_mkundef();
  priv->writable.write_callback = js_mkundef();
  priv->writable.end_callback = js_mkundef();
  priv->is_readable = true;

  return priv;
}

void gc_mark_stream_object(ant_t *js, ant_value_t value, gc_mark_fn mark) {
  stream_private_state_t *priv = js_get_native(value, STREAM_NATIVE_TAG);
  if (!priv) return;

  mark(js, priv->pipes);
  mark(js, priv->destroy_error);
  mark(js, priv->readable_state_obj);
  mark(js, priv->writable_state_obj);
  mark(js, priv->transform_done);
  mark(js, priv->transform_callback);
  mark(js, priv->readable.buffer);
  mark(js, priv->readable.decoder);
  mark(js, priv->readable.errored);
  mark(js, priv->writable.buffer);
  mark(js, priv->writable.errored);
  mark(js, priv->writable.onwrite);
  mark(js, priv->writable.write_callback);
  mark(js, priv->writable.end_callback);
}

void gc_finalize_stream_object(ant_t *js, ant_value_t value) {
  stream_private_state_t *priv = js_get_native(value, STREAM_NATIVE_TAG);
  if (!priv) return;

  js_clear_native(value, STREAM_NATIVE_TAG);
  if (priv->attached_state && priv->attached_state_finalize)
    priv->attached_state_finalize(js, value, priv->attached_state);
  free(priv);
}

void *stream_get_attached_state(ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  return priv ? priv->attached_state : NULL;
}

void stream_set_attached_state(ant_value_t stream_obj, void *state, stream_finalize_fn finalize) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return;
  priv->attached_state = state;
  priv->attached_state_finalize = finalize;
}

void stream_clear_attached_state(ant_value_t stream_obj) {
  stream_set_attached_state(stream_obj, NULL, NULL);
}

void stream_set_destroyed(ant_t *js, ant_value_t stream_obj, bool destroyed) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (priv) priv->destroyed = destroyed;
  else if (is_object_type(stream_obj)) js_set(js, stream_obj, "destroyed", js_bool(destroyed));
}

void stream_set_readable(ant_t *js, ant_value_t stream_obj, bool readable) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (priv) priv->is_readable = readable;
  else if (is_object_type(stream_obj)) js_set(js, stream_obj, "readable", js_bool(readable));
}

static uint32_t stream_field_tag(stream_field_target_t target) {
  if (target == STREAM_ON_READABLE_STATE) return STREAM_READABLE_STATE_TAG;
  if (target == STREAM_ON_WRITABLE_STATE) return STREAM_WRITABLE_STATE_TAG;
  return STREAM_NATIVE_TAG;
}

static const stream_field_t *stream_current_field(ant_t *js) {
  ant_value_t index = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  return &stream_fields[(size_t)js_getnum(index)];
}

static stream_private_state_t *stream_field_owner(ant_value_t receiver, const stream_field_t *field) {
  return is_object_type(receiver) ? js_get_native(receiver, stream_field_tag(field->target)) : NULL;
}

static ant_value_t stream_readable_buffer(ant_t *js, stream_private_state_t *priv) {
  if (vtype(priv->readable.buffer) != kTypeArray) stream_store(js, priv, &priv->readable.buffer, js_mkarr(js));
  return priv->readable.buffer;
}

static ant_value_t stream_field_get(ant_params_t) {
  const stream_field_t *field = stream_current_field(js);
  stream_private_state_t *priv = stream_field_owner(js_getthis(js), field);
  if (!priv) return js_mkundef();

  void *ptr = (char *)priv + field->offset;
  switch (field->kind) {
    case STREAM_FIELD_BOOL: return js_bool(*(bool *)ptr);
    case STREAM_FIELD_NUM: return js_mknum(*(double *)ptr);
    case STREAM_FIELD_VALUE: return *(ant_value_t *)ptr;
    case STREAM_FIELD_BUFFER: return stream_readable_buffer(js, priv);
  }

  return js_mkundef();
}

static ant_value_t stream_field_set(ant_params_t) {
  const stream_field_t *field = stream_current_field(js);
  ant_value_t receiver = js_getthis(js);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  stream_private_state_t *priv = stream_field_owner(receiver, field);

  if (!priv) {
    if (field->target == STREAM_ON_STREAM && is_object_type(receiver)) js_set(js, receiver, field->name, value);
    return js_mkundef();
  }

  void *ptr = (char *)priv + field->offset;
  switch (field->kind) {
    case STREAM_FIELD_BOOL: *(bool *)ptr = js_truthy(js, value); break;
    case STREAM_FIELD_NUM: {
      double number = js_to_number(js, value);
      if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
      if (!isnan(number)) *(double *)ptr = number;
      break;
    }
    case STREAM_FIELD_VALUE: stream_store(js, priv, ptr, value); break;
    case STREAM_FIELD_BUFFER:
      stream_store(js, priv, ptr, value);
      priv->readable.buffer_head = 0;
      break;
  }

  return js_mkundef();
}

static void stream_define_fields(ant_t *js, ant_value_t target, stream_field_target_t on) {
  int flags = on == STREAM_ON_READABLE_STATE || on == STREAM_ON_WRITABLE_STATE ? JS_DESC_E | JS_DESC_C : JS_DESC_C;

  for (size_t i = 0; i < countof(stream_fields); i++) {
    const stream_field_t *field = &stream_fields[i];
    if (field->target != on) continue;

    ant_value_t getter = js_heavy_mkfun(js, stream_field_get, js_mknum((double)i));
    ant_value_t setter = field->readonly ? js_mkundef() : js_heavy_mkfun(js, stream_field_set, js_mknum((double)i));
    js_set_accessor_desc(js, target, field->name, strlen(field->name), getter, setter, flags);
  }
}

static ant_value_t stream_state_object(ant_t *js, stream_private_state_t *priv, bool readable) {
  ant_value_t *cached = readable ? &priv->readable_state_obj : &priv->writable_state_obj;
  if (is_object_type(*cached)) return *cached;

  ant_value_t state = Ant_Object_CreateBare(js);
  js_set_proto_init(state, readable ? js->builtins.readable_state_proto : js->builtins.writable_state_proto);
  js_set_native(state, priv, readable ? STREAM_READABLE_STATE_TAG : STREAM_WRITABLE_STATE_TAG);
  js_set_slot_wb(js, state, SLOT_DATA, stream_value(priv));
  stream_store(js, priv, cached, state);

  return state;
}

static ant_value_t js_stream_readable_state_getter(ant_params_t) {
  stream_private_state_t *priv = stream_readable_of(js_getthis(js));
  return priv ? stream_state_object(js, priv, true) : js_mkundef();
}

static ant_value_t js_stream_writable_state_getter(ant_params_t) {
  stream_private_state_t *priv = stream_writable_of(js_getthis(js));
  return priv ? stream_state_object(js, priv, false) : js_mkundef();
}

static ant_value_t js_stream_readable_state_setter(ant_params_t) {
  ant_value_t receiver = js_getthis(js);
  if (is_object_type(receiver)) js_set(js, receiver, "_readableState", nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t js_stream_writable_state_setter(ant_params_t) {
  ant_value_t receiver = js_getthis(js);
  if (is_object_type(receiver)) js_set(js, receiver, "_writableState", nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t js_writable_writable_getter(ant_params_t) {
  stream_private_state_t *priv = stream_writable_of(js_getthis(js));
  return js_bool(priv && !priv->destroyed && !priv->writable.ended);
}

static void stream_define_stream_accessors(ant_t *js, ant_value_t target) {
  stream_define_fields(js, target, STREAM_ON_STREAM);

  js_set_accessor_desc(
    js, target, "_readableState", 14,
    Ant_Function_Create(js_stream_readable_state_getter),
    Ant_Function_Create(js_stream_readable_state_setter), JS_DESC_C
  );

  js_set_accessor_desc(
    js, target, "_writableState", 14,
    Ant_Function_Create(js_stream_writable_state_getter),
    Ant_Function_Create(js_stream_writable_state_setter), JS_DESC_C
  );
}

static void stream_define_writable_accessors(ant_t *js, ant_value_t target) {
  stream_define_fields(js, target, STREAM_ON_WRITABLE);
  js_set_getter_desc(js, target, "writable", 8, Ant_Function_Create(js_writable_writable_getter), JS_DESC_C);
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

static ant_value_t stream_buffer_ctor(ant_t *js) {
  ant_value_t ns = js_esm_import_sync_cstr(js, "buffer", 6);
  if (is_err(ns)) return ns;
  return js_get(js, ns, "Buffer");
}

static ant_value_t stream_readable_decode_chunk(
  ant_t *js, stream_readable_state_t *rs,
  ant_value_t chunk, bool flush
) {
  if (!is_object_type(rs->decoder)) return chunk;
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

static double stream_chunk_size(ant_t *js, ant_value_t chunk, bool object_mode) {
  const uint8_t *bytes = NULL;
  size_t byte_len = 0;
  if (object_mode) return 1;
  if (buffer_source_get_bytes(js, chunk, &bytes, &byte_len)) return (double)byte_len;
  return 1;
}

static void stream_queue_compact(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t *queue, ant_offset_t *head, ant_offset_t min_head
) {
  if (vtype(*queue) != kTypeArray || *head == 0) return;
  ant_offset_t len = js_arr_len(js, *queue);

  if (*head >= len) {
    if (js_dense_array_obj(*queue)) js_arr_set_dense_length(js, *queue, 0);
    else stream_store(js, priv, queue, js_mkarr(js));
    *head = 0;
    return;
  }

  if (*head < min_head || *head * 2 < len) return;
  ant_value_t compact = js_mkarr(js);
  for (ant_offset_t i = *head; i < len; i++) js_arr_push(js, compact, js_arr_get(js, *queue, i));
  
  stream_store(js, priv, queue, compact);
  *head = 0;
}

static ant_offset_t stream_readable_buffer_len(ant_t *js, stream_readable_state_t *rs) {
  if (vtype(rs->buffer) != kTypeArray) return 0;
  ant_offset_t len = js_arr_len(js, rs->buffer);
  return len > rs->buffer_head ? len - rs->buffer_head : 0;
}

static void stream_buffer_push(ant_t *js, stream_private_state_t *priv, ant_value_t value) {
  js_arr_push(js, stream_readable_buffer(js, priv), value);
  priv->readable.length += stream_chunk_size(js, value, priv->readable.object_mode);
  }
  
static ant_value_t stream_buffer_shift(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (stream_readable_buffer_len(js, rs) == 0) return js_mkundef();

  ant_value_t value = js_arr_get(js, rs->buffer, rs->buffer_head++);
  double next_length = rs->length - stream_chunk_size(js, value, rs->object_mode);

  rs->length = next_length > 0 ? next_length : 0;
  rs->data_emitted = true;
  stream_queue_compact(js, priv, &rs->buffer, &rs->buffer_head, 32);
  
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

static stream_private_state_t *stream_init_base(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_private_state_t *priv = stream_attach(js, obj);
  if (!priv) return NULL;

  ant_value_t construct_fn = stream_get_option(js, options, "construct");
  ant_value_t destroy_fn = stream_get_option(js, options, "destroy");
  
  if (is_callable(construct_fn)) {
    js_set(js, obj, "_construct", construct_fn);
    stream_begin_construct(js, obj);
  }
  
  if (is_callable(destroy_fn)) js_set(js, obj, "_destroy", destroy_fn);
  return priv;
}

static void stream_init_readable_side(ant_t *js, stream_private_state_t *priv, ant_value_t options) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t read_fn = stream_get_option(js, options, "read");

  priv->has_readable = true;
  rs->object_mode = js_truthy(js, stream_get_option(js, options, "objectMode"));
  rs->high_water_mark = stream_high_water_mark_from_options(js, options, rs->object_mode);
  rs->auto_destroy = stream_auto_destroy_from_options(js, options);

  if (is_callable(read_fn)) js_set(js, stream_value(priv), "_read", read_fn);
}

static void stream_init_writable_side(ant_t *js, stream_private_state_t *priv, ant_value_t options) {
  stream_writable_state_t *ws = &priv->writable;
  ant_value_t write_fn = stream_get_option(js, options, "write");
  ant_value_t final_fn = stream_get_option(js, options, "final");

  priv->has_writable = true;
  ws->object_mode =
    js_truthy(js, stream_get_option(js, options, "objectMode")) ||
    js_truthy(js, stream_get_option(js, options, "writableObjectMode"));
  ws->high_water_mark = stream_high_water_mark_from_options(js, options, ws->object_mode);
  ws->auto_destroy = stream_auto_destroy_from_options(js, options);

  if (is_callable(write_fn)) js_set(js, stream_value(priv), "_write", write_fn);
  if (is_callable(final_fn)) js_set(js, stream_value(priv), "_final", final_fn);
}

static void stream_init_legacy(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_base(js, obj, options);
}

static void stream_init_readable(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_private_state_t *priv = stream_init_base(js, obj, options);
  if (priv) stream_init_readable_side(js, priv, options);
}

static void stream_init_writable(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_private_state_t *priv = stream_init_base(js, obj, options);
  if (!priv) return;
  priv->is_readable = false;
  stream_init_writable_side(js, priv, options);
}

static void stream_init_duplex(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_private_state_t *priv = stream_init_base(js, obj, options);
  if (!priv) return;
  stream_init_readable_side(js, priv, options);
  stream_init_writable_side(js, priv, options);
}

static ant_value_t stream_emit_named(ant_t *js, ant_value_t stream_obj, const char *event_name) {
  return js_bool(eventemitter_emit_args(js, stream_obj, event_name, NULL, 0));
}

static void stream_set_errored(ant_t *js, stream_private_state_t *priv, ant_value_t error) {
  if (priv->has_readable) stream_store(js, priv, &priv->readable.errored, error);
  if (priv->has_writable) stream_store(js, priv, &priv->writable.errored, error);
}

static void stream_emit_error(ant_t *js, stream_private_state_t *priv, ant_value_t error) {
  ant_value_t args[1] = { error };
  stream_set_errored(js, priv, error);
  eventemitter_emit_args(js, stream_value(priv), "error", args, 1);
}

static bool stream_auto_destroy(stream_private_state_t *priv) {
  if (priv->has_readable && !priv->readable.auto_destroy) return false;
  if (priv->has_writable && !priv->writable.auto_destroy) return false;
  return true;
}

static ant_value_t stream_set_paused(ant_t *js, stream_private_state_t *priv, bool paused) {
  ant_value_t stream_obj = stream_value(priv);
  priv->paused = paused;
  stream_emit_named(js, stream_obj, paused ? "pause" : "resume");
  return stream_obj;
}

static ant_value_t js_stream_pause(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  if (is_err(stream_obj)) return stream_obj;
  return stream_set_paused(js, stream_private_state(stream_obj), true);
}

static ant_value_t js_stream_resume(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  if (is_err(stream_obj)) return stream_obj;
  return stream_set_paused(js, stream_private_state(stream_obj), false);
}

static ant_value_t js_stream_is_paused(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  if (is_err(stream_obj)) return stream_obj;
  return js_bool(stream_private_state(stream_obj)->paused);
}

static void stream_pipe_remove_state(ant_t *js, stream_private_state_t *priv, ant_value_t state) {
  ant_value_t pipes = priv->pipes;
  ant_offset_t len = vtype(pipes) == kTypeArray ? js_arr_len(js, pipes) : 0;
  ant_value_t next = js_mkarr(js);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = js_arr_get(js, pipes, i);
    if (item != state) js_arr_push(js, next, item);
  }

  stream_store(js, priv, &priv->pipes, next);
}

static void stream_pipe_cleanup(ant_t *js, ant_value_t state) {
  if (stream_record_get(js, state, STREAM_PIPE_CLEANED) == js_true) return;
  stream_record_set(js, state, STREAM_PIPE_CLEANED, js_true);

  ant_value_t source = stream_record_get(js, state, STREAM_PIPE_SOURCE);
  ant_value_t dest = stream_record_get(js, state, STREAM_PIPE_DEST);
  stream_private_state_t *priv = stream_private_state(source);

  if (priv) {
    stream_remove_listener(js, source, "data", stream_record_get(js, state, STREAM_PIPE_ON_DATA));
    stream_remove_listener(js, source, "end", stream_record_get(js, state, STREAM_PIPE_ON_END));
    stream_remove_listener(js, source, "close", stream_record_get(js, state, STREAM_PIPE_ON_CLOSE));
    stream_remove_listener(js, source, "error", stream_record_get(js, state, STREAM_PIPE_ON_ERROR));
    stream_pipe_remove_state(js, priv, state);
  }

  if (is_object_type(dest))
    stream_remove_listener(js, dest, "drain", stream_record_get(js, state, STREAM_PIPE_ON_DRAIN));
}

static ant_value_t stream_pipe_on_data(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t dest = stream_record_get(js, state, STREAM_PIPE_DEST);
  if (!is_object_type(dest)) return js_mkundef();
  
  ant_value_t result = stream_call_prop(js, dest, "write", nargs > 0 ? &args[0] : NULL, nargs > 0 ? 1 : 0);
  if (is_err(result)) return result;

  if (result == js_false) stream_call_prop(js, stream_record_get(js, state, STREAM_PIPE_SOURCE), "pause", NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_drain(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  stream_call_prop(js, stream_record_get(js, state, STREAM_PIPE_SOURCE), "resume", NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_end(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t dest = stream_record_get(js, state, STREAM_PIPE_DEST);
  bool end_dest = stream_record_get(js, state, STREAM_PIPE_END) == js_true;

  stream_pipe_cleanup(js, state);
  if (end_dest) stream_call_prop(js, dest, "end", NULL, 0);

  return js_mkundef();
}

static ant_value_t stream_pipe_on_close(ant_params_t) {
  stream_pipe_cleanup(js, js_get_slot(js_getcurrentfunc(js), SLOT_DATA));
  return js_mkundef();
}

static ant_value_t stream_pipe_on_error(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t dest = stream_record_get(js, state, STREAM_PIPE_DEST);

  stream_pipe_cleanup(js, state);
  if (is_object_type(dest) && stream_listener_count_positive(js, dest, "error") && nargs > 0)
    eventemitter_emit_args(js, dest, "error", &args[0], 1);

  return js_mkundef();
}

static ant_value_t js_stream_pipe(ant_params_t) {
  ant_value_t source = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();
  bool end_dest = true;

  if (is_err(source)) return source;
  if (nargs < 1 || !is_object_type(args[0])) return js_mkerr(js, "pipe requires a destination stream");
  if (is_object_type(options)) {
    ant_value_t end_val = js_get(js, options, "end");
    if (!is_undefined(end_val)) end_dest = end_val != js_false;
  }

  ant_value_t dest = args[0];
  ant_value_t state = stream_record(
    js, 9, source, dest, js_bool(end_dest), js_false,
    js_mkundef(), js_mkundef(), js_mkundef(), js_mkundef(), js_mkundef()
  );

  ant_value_t on_data = js_heavy_mkfun(js, stream_pipe_on_data, state);
  ant_value_t on_drain = js_heavy_mkfun(js, stream_pipe_on_drain, state);
  ant_value_t on_end = js_heavy_mkfun(js, stream_pipe_on_end, state);
  ant_value_t on_close = js_heavy_mkfun(js, stream_pipe_on_close, state);
  ant_value_t on_error = js_heavy_mkfun(js, stream_pipe_on_error, state);

  stream_record_set(js, state, STREAM_PIPE_ON_DATA, on_data);
  stream_record_set(js, state, STREAM_PIPE_ON_DRAIN, on_drain);
  stream_record_set(js, state, STREAM_PIPE_ON_END, on_end);
  stream_record_set(js, state, STREAM_PIPE_ON_CLOSE, on_close);
  stream_record_set(js, state, STREAM_PIPE_ON_ERROR, on_error);

  stream_private_state_t *priv = stream_private_state(source);
  if (vtype(priv->pipes) != kTypeArray) stream_store(js, priv, &priv->pipes, js_mkarr(js));
  js_arr_push(js, priv->pipes, state);

  stream_add_listener(js, source, "data", on_data, false);
  stream_add_listener(js, source, "end", on_end, true);
  stream_add_listener(js, source, "close", on_close, true);
  stream_add_listener(js, source, "error", on_error, false);
  stream_add_listener(js, dest, "drain", on_drain, false);
  eventemitter_emit_args(js, dest, "pipe", &source, 1);

  if (priv->has_readable) priv->readable.flowing = true;
  stream_call_prop(js, source, "resume", NULL, 0);

  return dest;
}

static ant_value_t js_stream_unpipe(ant_params_t) {
  ant_value_t source = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t dest = nargs > 0 ? args[0] : js_mkundef();

  if (is_err(source)) return source;
  ant_value_t pipes = stream_private_state(source)->pipes;
  if (vtype(pipes) != kTypeArray) return source;

  ant_value_t matches = js_mkarr(js);
  ant_offset_t len = js_arr_len(js, pipes);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t state = js_arr_get(js, pipes, i);
    if (!is_object_type(dest) || stream_record_get(js, state, STREAM_PIPE_DEST) == dest) js_arr_push(js, matches, state);
  }

  len = js_arr_len(js, matches);
  for (ant_offset_t i = 0; i < len; i++) stream_pipe_cleanup(js, js_arr_get(js, matches, i));

  return source;
}

static ant_value_t stream_destroy_emit_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t destroyed_err = nargs > 1 ? args[1] : js_mkundef();
  stream_private_state_t *priv = stream_private_state(stream_obj);

  if (priv && !is_null(destroyed_err) && !is_undefined(destroyed_err)) stream_emit_error(js, priv, destroyed_err);
  stream_emit_named(js, stream_obj, "close");

  return js_mkundef();
}

static ant_value_t stream_destroy_done(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (stream_record_get(js, state, STREAM_DESTROY_CALLED) == js_true) return js_mkundef();

  stream_record_set(js, state, STREAM_DESTROY_CALLED, js_true);
  stream_tick(
    js, stream_destroy_emit_tick, 2,
    stream_record_get(js, state, STREAM_DESTROY_STREAM),
    nargs > 0 ? args[0] : js_mkundef()
  );

  return js_mkundef();
}

static ant_value_t stream_once_call(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (stream_record_get(js, state, STREAM_ONCE_CALLED) == js_true) return js_mkundef();

  stream_record_set(js, state, STREAM_ONCE_CALLED, js_true);
  return stream_call(
    js, stream_record_get(js, state, STREAM_ONCE_FN),
    stream_record_get(js, state, STREAM_ONCE_THIS), args, nargs
  );
}

static ant_value_t stream_make_once(ant_t *js, ant_value_t fn, ant_value_t this_val) {
  return js_heavy_mkfun(js, stream_once_call, stream_record(js, 3, fn, this_val, js_false));
}

static ant_value_t js_stream__destroy(ant_params_t) {
  ant_value_t error = nargs > 0 ? args[0] : js_mknull();
  stream_call_callback(js, nargs > 1 ? args[1] : js_mkundef(), &error, 1);
  return js_mkundef();
  }

static ant_value_t stream_run_destroy(ant_t *js, stream_private_state_t *priv, ant_value_t error) {
  ant_value_t stream_obj = stream_value(priv);
  ant_value_t destroy_fn = js_getprop_fallback(js, stream_obj, "_destroy");
  ant_value_t destroy_error = is_undefined(error) ? js_mknull() : error;
  
  if (!is_callable(destroy_fn) || destroy_fn == js->builtins.stream_default_destroy) {
    stream_tick(js, stream_destroy_emit_tick, 2, stream_obj, destroy_error);
  return stream_obj;
  }

  ant_value_t done_state = stream_record(js, 2, stream_obj, js_false);
  ant_value_t destroy_args[2] = { destroy_error, js_heavy_mkfun(js, stream_destroy_done, done_state) };
  ant_value_t result = stream_call(js, destroy_fn, stream_obj, destroy_args, 2);

  return is_err(result) ? result : stream_obj;
}

static ant_value_t js_stream_destroy(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();

  if (is_err(stream_obj)) return stream_obj;
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (priv->destroyed) return stream_obj;

  priv->destroyed = true;
  if (priv->has_readable) priv->is_readable = false;
  if (!is_undefined(error) && !is_null(error)) stream_set_errored(js, priv, error);
  
  if (priv->constructing) {
    priv->destroy_pending = true;
    stream_store(js, priv, &priv->destroy_error, error);
    return stream_obj;
  }

  return stream_run_destroy(js, priv, error);
}

static ant_value_t js_readable__read(ant_params_t) {
  return js_mkundef();
}

static void stream_readable_schedule_flowing_tick(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (!rs->flowing || priv->destroyed || rs->flowing_read_scheduled) return;

  rs->flowing_read_scheduled = true;
  stream_tick(js, stream_readable_continue_flowing, 1, stream_value(priv));
}

static void stream_readable_schedule_continue_flowing(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (!rs->flowing || priv->destroyed || rs->ended) return;
  if (stream_readable_buffer_len(js, rs) > 0) return;
  stream_readable_schedule_flowing_tick(js, priv);
}

static ant_value_t stream_readable_emit_readable(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return js_mkundef();

  stream_readable_state_t *rs = &priv->readable;
  rs->readable_scheduled = false;

  if (rs->flowing || rs->end_emitted || priv->destroyed) return js_mkundef();
  if (stream_readable_buffer_len(js, rs) == 0 && !rs->ended) return js_mkundef();

  stream_emit_named(js, stream_obj, "readable");
  return js_mkundef();
}

static void stream_readable_schedule_readable(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (!rs->readable_listening || rs->flowing || rs->readable_scheduled) return;

  rs->readable_scheduled = true;
  stream_tick(js, stream_readable_emit_readable, 1, stream_value(priv));
}

static ant_value_t stream_rs_maybe_read(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;

  if (priv->constructing || priv->destroyed) return js_mkundef();
  if (rs->reading || rs->ended) return js_mkundef();
  if (rs->length >= rs->high_water_mark) return js_mkundef();

  ant_value_t stream_obj = stream_value(priv);
  ant_value_t read_fn = js_getprop_fallback(js, stream_obj, "_read");
  ant_value_t args[1] = { js_mknum(rs->high_water_mark) };

  rs->reading = true;
  if (is_callable(read_fn)) stream_call(js, read_fn, stream_obj, args, 1);
  rs->reading = false;

  return js_mkundef();
}

static ant_value_t stream_half_open_end_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_private_state_t *priv = stream_private_state(stream_obj);

  if (!priv || priv->destroyed || (priv->has_writable && priv->writable.ended)) return js_mkundef();
  return stream_call_prop(js, stream_obj, "end", NULL, 0);
}

static ant_value_t stream_readable_emit_end(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return js_mkundef();

  stream_readable_state_t *rs = &priv->readable;
  rs->end_scheduled = false;

  if (!rs->flowing && !rs->end_requested) return js_mkundef();
  if (!rs->ended || rs->end_emitted || priv->destroyed) return js_mkundef();
  if (stream_readable_buffer_len(js, rs) != 0) return js_mkundef();

  ant_value_t tail = stream_readable_decode_chunk(js, rs, js_mkundef(), true);
  if (is_err(tail)) return tail;

  if (!is_undefined(tail) && !stream_value_is_empty_string(js, tail)) {
    rs->data_emitted = true;
    eventemitter_emit_args(js, stream_obj, "data", &tail, 1);
  }

  rs->end_emitted = true;
  priv->is_readable = false;
  stream_emit_named(js, stream_obj, "end");

  bool writable_done = !priv->has_writable || priv->writable.finished;
  if (writable_done && stream_auto_destroy(priv))
    sv_call_native_this(js, js_stream_destroy, stream_obj, NULL, 0);
  else if (!writable_done && js_get(js, stream_obj, "allowHalfOpen") == js_false)
    stream_tick(js, stream_half_open_end_tick, 1, stream_obj);

  return js_mkundef();
}

static void stream_readable_schedule_end(ant_t *js, stream_private_state_t *priv) {
  if (priv->readable.end_scheduled) return;
  priv->readable.end_scheduled = true;
  stream_tick(js, stream_readable_emit_end, 1, stream_value(priv));
}

static void stream_readable_end_if_drained(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (!rs->ended || stream_readable_buffer_len(js, rs) != 0) return;

  rs->end_requested = true;
  stream_readable_schedule_end(js, priv);
}

static void stream_readable_after_data(ant_t *js, stream_private_state_t *priv, bool emitted_data) {
  stream_readable_state_t *rs = &priv->readable;

  if (rs->flowing && rs->ended && !rs->end_emitted && stream_readable_buffer_len(js, rs) == 0)
    stream_readable_schedule_end(js, priv);
  else if (emitted_data) stream_readable_schedule_continue_flowing(js, priv);
}

static ant_value_t stream_rs_flush(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t stream_obj = stream_value(priv);
  bool emitted_data = false;

  while (rs->flowing && stream_readable_buffer_len(js, rs) > 0) {
    ant_value_t chunk = stream_buffer_shift(js, priv);
    chunk = stream_readable_decode_chunk(js, rs, chunk, false);
    if (is_err(chunk)) return chunk;
    emitted_data = true;
    eventemitter_emit_args(js, stream_obj, "data", &chunk, 1);
  }

  stream_readable_after_data(js, priv, emitted_data);
  return js_mkundef();
}

static ant_value_t stream_rs_begin_flowing(ant_t *js, stream_private_state_t *priv) {
  if (!priv->readable.flowing) return js_mkundef();

  stream_set_paused(js, priv, false);
  stream_rs_maybe_read(js, priv);
  stream_rs_flush(js, priv);

  return js_mkundef();
}

ant_value_t stream_readable_continue_flowing(ant_params_t) {
  stream_private_state_t *priv = stream_readable_of(nargs > 0 ? args[0] : js_mkundef());
  if (!priv) return js_mkundef();

  priv->readable.flowing_read_scheduled = false;
  if (!priv->readable.flowing || priv->destroyed) return js_mkundef();

  stream_rs_maybe_read(js, priv);
  stream_rs_flush(js, priv);
  
  return js_mkundef();
}

ant_value_t stream_readable_begin_flowing(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  return priv ? stream_rs_begin_flowing(js, priv) : js_mkundef();
}

ant_value_t stream_readable_flush(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  return priv ? stream_rs_flush(js, priv) : js_mkundef();
}

ant_value_t stream_readable_maybe_read(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  return priv ? stream_rs_maybe_read(js, priv) : js_mkundef();
}
  
static ant_value_t stream_readable_start_flowing(ant_params_t) {
  return stream_readable_begin_flowing(js, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t stream_readable_read_tick(ant_params_t) {
  return stream_readable_maybe_read(js, nargs > 0 ? args[0] : js_mkundef());
}

ant_value_t stream_readable_push_value(
  ant_t *js,
  ant_value_t stream_obj,
  ant_value_t chunk,
  ant_value_t encoding
) {
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv || priv->destroyed) return js_false;

  stream_readable_state_t *rs = &priv->readable;
  if (is_null(chunk)) {
    rs->ended = true;
    stream_rs_flush(js, priv);
    stream_readable_schedule_readable(js, priv);
    return js_false;
  }

  ant_value_t normalized = stream_normalize_chunk(js, chunk, rs->object_mode, encoding);
  if (is_err(normalized)) return normalized;
  
  if (rs->flowing && stream_readable_buffer_len(js, rs) == 0) {
    ant_value_t data = stream_readable_decode_chunk(js, rs, normalized, false);
    if (is_err(data)) return data;

    rs->data_emitted = true;
    eventemitter_emit_args(js, stream_obj, "data", &data, 1);
    stream_readable_after_data(js, priv, true);
  } else {
    stream_buffer_push(js, priv, normalized);
    if (rs->flowing) stream_rs_flush(js, priv);
    else stream_readable_schedule_readable(js, priv);
  }
  
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
  if (is_err(stream_obj)) return stream_obj;

  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return js_mknull();
  stream_readable_state_t *rs = &priv->readable;

  if (nargs > 0 && vtype(args[0]) == kTypeNumber && tod(args[0]) == 0.0) {
    stream_rs_maybe_read(js, priv);
    return js_mknull();
  }

  if (stream_readable_buffer_len(js, rs) == 0) stream_rs_maybe_read(js, priv);
  if (stream_readable_buffer_len(js, rs) == 0) {
    stream_readable_end_if_drained(js, priv);
    return js_mknull();
  }

  ant_value_t chunk = stream_buffer_shift(js, priv);
  chunk = stream_readable_decode_chunk(js, rs, chunk, false);
  if (is_err(chunk)) return chunk;

  if (rs->flowing) stream_rs_flush(js, priv);
  else stream_readable_end_if_drained(js, priv);

  return chunk;
}

static ant_value_t js_readable_set_encoding(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t encoding = nargs > 0 && !is_undefined(args[0]) ? args[0] : js->builtins.stream_utf8;

  if (is_err(stream_obj)) return stream_obj;
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return stream_obj;

  ant_value_t decoder = string_decoder_create(js, encoding, js_mkundef());
  if (is_err(decoder)) return decoder;
  ant_value_t encoding_str = js_tostring_val(js, encoding);
  if (is_err(encoding_str)) return encoding_str;

  stream_store(js, priv, &priv->readable.decoder, decoder);
  js_set(js, stream_obj, "encoding", encoding_str);
  js_set(js, stream_obj, "readableEncoding", encoding_str);
  
  return stream_obj;
}

static ant_value_t js_readable_add(ant_native_params_t, bool once) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  if (is_err(stream_obj)) return stream_obj;
  if (nargs < 2) return js_mkerr(js, "on requires 2 arguments (event, listener)");

  ant_value_t key = stream_event_key(js, args[0]);
  if (is_err(key)) return key;
  if (!eventemitter_add_listener_val(js, stream_obj, key, args[1], once))
    return js_mkerr(js, "listener must be a function");

  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return stream_obj;
  stream_readable_state_t *rs = &priv->readable;

  if (stream_key_is_cstr(js, key, "data") && !rs->readable_listening) {
    rs->flowing = true;
    stream_tick(js, stream_readable_start_flowing, 1, stream_obj);
  } else if (stream_key_is_cstr(js, key, "readable")) {
    rs->readable_listening = true;
    rs->flowing = false;
    if (stream_readable_buffer_len(js, rs) > 0 || rs->ended) stream_readable_schedule_readable(js, priv);
    else stream_tick(js, stream_readable_read_tick, 1, stream_obj);
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
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return stream_obj;

  if (priv->has_readable) priv->readable.flowing = true;
  stream_set_paused(js, priv, false);
  if (priv->has_readable) stream_readable_schedule_flowing_tick(js, priv);

  return stream_obj;
}

ant_value_t stream_readable_pause(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return stream_obj;

  if (priv->has_readable) priv->readable.flowing = false;
  return stream_set_paused(js, priv, true);
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

static stream_write_status_t stream_writable_start(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding,
  ant_value_t callback, double chunk_size
);

static ant_value_t stream_ws_begin_end(ant_t *js, stream_private_state_t *priv, ant_value_t callback);

static void stream_writable_set_length(stream_writable_state_t *ws, double length) {
  ws->length = length < 0 ? 0 : length;
}

static bool stream_writable_has_buffered(ant_t *js, stream_writable_state_t *ws) {
  return vtype(ws->buffer) == kTypeArray && ws->buffer_head < js_arr_len(js, ws->buffer);
}

static void stream_writable_enqueue(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding,
  ant_value_t callback, double chunk_size
) {
  stream_writable_state_t *ws = &priv->writable;

  if (vtype(ws->buffer) != kTypeArray) {
    stream_store(js, priv, &ws->buffer, js_mkarr(js));
    ws->buffer_head = 0;
  }

  js_arr_push(js, ws->buffer, chunk);
  js_arr_push(js, ws->buffer, encoding);
  js_arr_push(js, ws->buffer, callback);
  js_arr_push(js, ws->buffer, js_mknum(chunk_size));
}

static bool stream_writable_dequeue(ant_t *js, stream_private_state_t *priv, ant_value_t entry[STREAM_WRITE_ENTRY_STRIDE]) {
  stream_writable_state_t *ws = &priv->writable;
  if (!stream_writable_has_buffered(js, ws)) return false;

  for (int i = 0; i < STREAM_WRITE_ENTRY_STRIDE; i++)
    entry[i] = js_arr_get(js, ws->buffer, ws->buffer_head + (ant_offset_t)i);
  
  ws->buffer_head += STREAM_WRITE_ENTRY_STRIDE;
  stream_queue_compact(js, priv, &ws->buffer, &ws->buffer_head, 32 * STREAM_WRITE_ENTRY_STRIDE);

  return true;
}

static void stream_writable_fail_buffered(ant_t *js, stream_private_state_t *priv, ant_value_t err) {
  ant_value_t entry[STREAM_WRITE_ENTRY_STRIDE];

  while (stream_writable_dequeue(js, priv, entry)) {
    ant_value_t entry_cb = entry[STREAM_WRITE_ENTRY_CALLBACK];
    if (is_callable(entry_cb)) stream_call_callback(js, entry_cb, &err, 1);
  }

  stream_writable_set_length(&priv->writable, 0);
  priv->writable.need_drain = false;
}

static ant_value_t stream_writable_emit_drain(ant_params_t) {
  stream_emit_named(js, nargs > 0 ? args[0] : js_mkundef(), "drain");
  return js_mkundef();
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

static ant_value_t stream_writable_drain(ant_t *js, stream_private_state_t *priv) {
  stream_writable_state_t *ws = &priv->writable;
  ant_value_t stream_obj = stream_value(priv);
  ant_value_t entry[STREAM_WRITE_ENTRY_STRIDE];

  if (ws->draining) return js_mkundef();
  ws->draining = true;

  while (!priv->destroyed && stream_writable_dequeue(js, priv, entry)) {
    ant_value_t size = entry[STREAM_WRITE_ENTRY_SIZE];
    stream_write_status_t status = stream_writable_start(
      js, priv,
      entry[STREAM_WRITE_ENTRY_CHUNK],
      entry[STREAM_WRITE_ENTRY_ENCODING],
      entry[STREAM_WRITE_ENTRY_CALLBACK],
      vtype(size) == kTypeNumber ? js_getnum(size) : 0
    );

    if (status == STREAM_WRITE_COMPLETE) continue;
    if (status == STREAM_WRITE_THROWN) {
      ws->draining = false;
      return Ant_Exception_Current(js);
    }

    break;
  }

  ws->draining = false;

  if (!ws->writing && !stream_writable_has_buffered(js, ws) && ws->length <= 0 && ws->need_drain) {
    ws->need_drain = false;
    if (ws->sync) stream_tick(js, stream_writable_emit_drain, 1, stream_obj);
    else stream_emit_named(js, stream_obj, "drain");
  }

  if (!ws->writing && ws->pending_final && !ws->final_started) {
    ant_value_t end_callback = ws->end_callback;
    ws->pending_final = false;
    stream_store(js, priv, &ws->end_callback, js_mkundef());
    return stream_ws_begin_end(js, priv, end_callback);
  }

  return js_mkundef();
}

static ant_value_t stream_writable_onwrite(ant_params_t) {
  ant_value_t stream_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  stream_private_state_t *priv = stream_writable_of(stream_obj);
  if (!priv || !priv->writable.write_pending) return js_mkundef();

  stream_writable_state_t *ws = &priv->writable;
  ant_value_t callback = ws->write_callback;
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  ws->write_pending = false;
  ws->write_done_seq = ws->write_seq;
  stream_store(js, priv, &ws->write_callback, js_mkundef());
  stream_writable_set_length(ws, ws->length - ws->write_size);
  if (!stream_writable_has_buffered(js, ws)) ws->writing = false;

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };

    ws->pending_final = false;
    ws->writing = false;

    stream_set_errored(js, priv, err);
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);

    stream_writable_fail_buffered(js, priv, err);
    sv_call_native_this(js, js_stream_destroy, stream_obj, destroy_args, 1);

    return js_mkundef();
  }

  if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);

  if (priv->destroyed && stream_writable_has_buffered(js, ws)) {
    stream_writable_fail_buffered(js, priv, stream_write_state_error(js, false));
    return js_mkundef();
  }

  return stream_writable_drain(js, priv);
}

static stream_write_status_t stream_writable_start(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding,
  ant_value_t callback, double chunk_size
) {
  stream_writable_state_t *ws = &priv->writable;
  ant_value_t stream_obj = stream_value(priv);

  if (!is_callable(ws->onwrite))
    stream_store(js, priv, &ws->onwrite, js_heavy_mkfun(js, stream_writable_onwrite, stream_obj));

  stream_store(js, priv, &ws->write_callback, callback);
  ws->write_size = chunk_size;
  ws->write_pending = true;
  ws->writing = true;
  uint32_t seq = ++ws->write_seq;

  ant_value_t write_fn = js_getprop_fallback(js, stream_obj, "_write");
  ant_value_t write_args[3] = { chunk, encoding, ws->onwrite };

  bool prev_sync = ws->sync;
  ws->sync = true;

  if (is_callable(write_fn)) {
    ant_value_t result = stream_call(js, write_fn, stream_obj, write_args, 3);
    if (is_err(result)) {
      ws->sync = prev_sync;
      return STREAM_WRITE_THROWN;
    }
  } else stream_call_callback(js, ws->onwrite, NULL, 0);

  ws->sync = prev_sync;
  return (int32_t)(ws->write_done_seq - seq) >= 0 ? STREAM_WRITE_COMPLETE : STREAM_WRITE_PENDING;
}

static ant_value_t stream_errored_write_cb_tick(ant_params_t) {
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t error = nargs > 1 ? args[1] : js_mkundef();
  if (is_callable(callback)) stream_call_callback(js, callback, &error, 1);
  return js_mkundef();
}

static ant_value_t stream_writable_write_impl(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding,
  ant_value_t callback, bool allow_after_end
) {
  stream_writable_state_t *ws = &priv->writable;
  
  if (!is_undefined(ws->errored) && !is_null(ws->errored)) {
    if (is_callable(callback)) stream_tick(js, stream_errored_write_cb_tick, 2, callback, ws->errored);
    return js_false;
  }

  bool ending = !allow_after_end && ws->ended;
  if (ending || priv->destroyed) {
    ant_value_t err = stream_write_state_error(js, ending);
    stream_set_errored(js, priv, err);
    
    if (is_callable(callback)) stream_tick(js, stream_errored_write_cb_tick, 2, callback, err);
    else stream_emit_error(js, priv, err);
    
    return js_false;
  }

  ant_value_t normalized = stream_normalize_chunk(js, chunk, ws->object_mode, encoding);
  if (is_err(normalized)) return normalized;

  double chunk_size = stream_chunk_size(js, normalized, ws->object_mode);
  double length = ws->length + chunk_size;
  bool accepted = length < ws->high_water_mark;
  
  stream_writable_set_length(ws, length);
  if (!accepted) ws->need_drain = true;

  if (ws->writing || priv->constructing) stream_writable_enqueue(js, priv, normalized, encoding, callback, chunk_size);
  else if (stream_writable_start(js, priv, normalized, encoding, callback, chunk_size) == STREAM_WRITE_THROWN)
    return Ant_Exception_Current(js);

  return js_bool(accepted && !priv->destroyed);
}

static ant_value_t js_writable_write(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Writable");
  ant_value_t callback = js_mkundef();
  ant_value_t encoding = js->builtins.stream_utf8;

  if (is_err(stream_obj)) return stream_obj;
  stream_private_state_t *priv = stream_writable_of(stream_obj);
  if (!priv) return js_false;

  if (nargs > 1 && is_callable(args[1])) callback = args[1];
  else if (nargs > 1 && !is_undefined(args[1])) encoding = args[1];
  if (nargs > 2 && is_callable(args[2])) callback = args[2];

  return stream_writable_write_impl(js, priv, nargs > 0 ? args[0] : js_mkundef(), encoding, callback, false);
}

static ant_value_t stream_writable_finish_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t callback = nargs > 1 ? args[1] : js_mkundef();
  stream_private_state_t *priv = stream_writable_of(stream_obj);
  if (!priv) return js_mkundef();

  if (priv->destroyed) {
    if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);
    return js_mkundef();
  }
  
  priv->writable.finished = true;
  if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);
  stream_emit_named(js, stream_obj, "finish");

  if (!priv->is_readable && stream_auto_destroy(priv))
    sv_call_native_this(js, js_stream_destroy, stream_obj, NULL, 0);

  return js_mkundef();
}

static ant_value_t stream_writable_end_done(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = stream_record_get(js, state, STREAM_CALL_STREAM);
  ant_value_t callback = stream_record_get(js, state, STREAM_CALL_CALLBACK);
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };
    sv_call_native_this(js, js_stream_destroy, stream_obj, destroy_args, 1);
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    return js_mkundef();
  }

  stream_tick(js, stream_writable_finish_tick, 2, stream_obj, callback);
  return js_mkundef();
}

static ant_value_t stream_ws_begin_end(ant_t *js, stream_private_state_t *priv, ant_value_t callback) {
  stream_writable_state_t *ws = &priv->writable;
  ant_value_t stream_obj = stream_value(priv);
  ant_value_t state = stream_record(js, 2, stream_obj, callback);
  ant_value_t done = stream_make_once(js, js_heavy_mkfun(js, stream_writable_end_done, state), js_mkundef());

  ws->final_started = true;
  ws->pending_final = false;
  stream_store(js, priv, &ws->end_callback, js_mkundef());
  
  ant_value_t final_fn = js_getprop_fallback(js, stream_obj, "_final");
  if (is_callable(final_fn)) stream_call(js, final_fn, stream_obj, &done, 1);
  else stream_call_callback(js, done, NULL, 0);

  return stream_obj;
}

ant_value_t stream_writable_begin_end(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  stream_private_state_t *priv = stream_writable_of(stream_obj);
  return priv ? stream_ws_begin_end(js, priv, callback) : stream_obj;
}

static ant_value_t stream_writable_end_after_write(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = stream_record_get(js, state, STREAM_CALL_STREAM);
  ant_value_t callback = stream_record_get(js, state, STREAM_CALL_CALLBACK);
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();
  stream_private_state_t *priv = stream_writable_of(stream_obj);

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };
    sv_call_native_this(js, js_stream_destroy, stream_obj, destroy_args, 1);
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    return js_mkundef();
  }

  return priv ? stream_ws_begin_end(js, priv, callback) : js_mkundef();
}

static ant_value_t js_writable_end(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Writable");
  ant_value_t callback = js_mkundef();
  ant_value_t chunk = js_mkundef();
  ant_value_t encoding = js_mkundef();

  if (is_err(stream_obj)) return stream_obj;
  if (nargs > 0 && is_callable(args[0])) callback = args[0];
  else {
    if (nargs > 0) chunk = args[0];
    if (nargs > 1 && is_callable(args[1])) callback = args[1];
    else if (nargs > 1) encoding = args[1];
    if (nargs > 2 && is_callable(args[2])) callback = args[2];
  }

  stream_private_state_t *priv = stream_writable_of(stream_obj);
  if (!priv || priv->writable.ended) {
    if (is_callable(callback)) stream_call_callback(js, callback, NULL, 0);
    return stream_obj;
  }

  stream_writable_state_t *ws = &priv->writable;
  ws->ended = true;

  if (!is_undefined(chunk) && !is_null(chunk)) {
    ant_value_t state = stream_record(js, 2, stream_obj, callback);
    ant_value_t after_write = stream_make_once(js, js_heavy_mkfun(js, stream_writable_end_after_write, state), js_mkundef());
    ant_value_t result = stream_writable_write_impl(js, priv, chunk, encoding, after_write, true);
    return is_err(result) ? result : stream_obj;
  }

  if ((ws->writing || priv->constructing) && !ws->final_started) {
    ws->pending_final = true;
    stream_store(js, priv, &ws->end_callback, callback);
    return stream_obj;
  }

  return stream_ws_begin_end(js, priv, callback);
}

static ant_value_t js_transform__transform(ant_params_t) {
  ant_value_t callback = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t cb_args[2];
  cb_args[0] = js_mknull();
  cb_args[1] = nargs > 0 ? args[0] : js_mkundef();
  stream_call_callback(js, callback, cb_args, 2);
  return js_mkundef();
}

static ant_value_t stream_transform_done(ant_params_t) {
  ant_value_t stream_obj = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv || !priv->transform_pending) return js_mkundef();

  ant_value_t callback = priv->transform_callback;
  priv->transform_pending = false;
  stream_store(js, priv, &priv->transform_callback, js_mkundef());

  if (nargs > 0 && !is_null(args[0]) && !is_undefined(args[0])) {
    if (is_callable(callback)) stream_call(js, callback, stream_obj, &args[0], 1);
    return js_mkundef();
  }

  if (nargs > 1 && !is_null(args[1]) && !is_undefined(args[1]))
    stream_readable_push_value(js, stream_obj, args[1], js_mkundef());

  if (is_callable(callback)) stream_call(js, callback, stream_obj, NULL, 0);
  return js_mkundef();
}

static ant_value_t js_transform__write(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Transform");
  if (is_err(stream_obj)) return stream_obj;

  stream_private_state_t *priv = stream_private_state(stream_obj);
  ant_value_t transform_fn = js_getprop_fallback(js, stream_obj, "_transform");

  if (!is_callable(priv->transform_done))
    stream_store(js, priv, &priv->transform_done, js_heavy_mkfun(js, stream_transform_done, stream_obj));

  stream_store(js, priv, &priv->transform_callback, nargs > 2 ? args[2] : js_mkundef());
  priv->transform_pending = true;

  ant_value_t call_args[3] = {
    nargs > 0 ? args[0] : js_mkundef(),
    nargs > 1 ? args[1] : js->builtins.stream_utf8,
    priv->transform_done,
  };

  return stream_call(js, transform_fn, stream_obj, call_args, 3);
}

static ant_value_t stream_transform_final_callback(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t stream_obj = stream_record_get(js, state, STREAM_CALL_STREAM);
  ant_value_t callback = stream_record_get(js, state, STREAM_CALL_CALLBACK);

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
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();

  if (is_err(stream_obj)) return stream_obj;
  ant_value_t flush_fn = js_getprop_fallback(js, stream_obj, "_flush");
  
  if (!is_callable(flush_fn)) {
    stream_readable_push_value(js, stream_obj, js_mknull(), js_mkundef());
    stream_call_callback(js, callback, NULL, 0);
    return js_mkundef();
  }

  ant_value_t state = stream_record(js, 2, stream_obj, callback);
  ant_value_t cb = js_heavy_mkfun(js, stream_transform_final_callback, state);
  return stream_call(js, flush_fn, stream_obj, &cb, 1);
}

static ant_value_t js_passthrough__transform(ant_params_t) {
  return js_transform__transform(js, args, nargs, js_mkundef());
}

static void stream_finished_cleanup(ant_t *js, ant_value_t state) {
  ant_value_t stream_obj = stream_record_get(js, state, STREAM_FINISHED_STREAM);
  ant_value_t on_finish = stream_record_get(js, state, STREAM_FINISHED_ON_FINISH);
  if (!is_object_type(stream_obj)) return;

  stream_remove_listener(js, stream_obj, "end", on_finish);
  stream_remove_listener(js, stream_obj, "finish", on_finish);
  stream_remove_listener(js, stream_obj, "close", on_finish);
  stream_remove_listener(js, stream_obj, "error", stream_record_get(js, state, STREAM_FINISHED_ON_ERROR));
}

static ant_value_t stream_finished_fire(ant_t *js, ant_value_t state, ant_value_t error) {
  if (stream_record_get(js, state, STREAM_FINISHED_CALLED) == js_true) return js_mkundef();

  stream_record_set(js, state, STREAM_FINISHED_CALLED, js_true);
  stream_finished_cleanup(js, state);

  ant_value_t callback = stream_record_get(js, state, STREAM_FINISHED_CALLBACK);
  if (is_undefined(error)) stream_call_callback(js, callback, NULL, 0);
  else stream_call_callback(js, callback, &error, 1);

  return js_mkundef();
}

static ant_value_t stream_finished_on_finish(ant_params_t) {
  return stream_finished_fire(js, js_get_slot(js_getcurrentfunc(js), SLOT_DATA), js_mkundef());
}

static ant_value_t stream_finished_on_error(ant_params_t) {
  return stream_finished_fire(js, js_get_slot(js_getcurrentfunc(js), SLOT_DATA), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t stream_finished_register(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  if (!is_callable(callback)) callback = Ant_Function_Create(stream_noop);

  ant_value_t state = stream_record(js, 5, stream_obj, callback, js_false, js_mkundef(), js_mkundef());
  ant_value_t on_finish = js_heavy_mkfun(js, stream_finished_on_finish, state);
  ant_value_t on_error = js_heavy_mkfun(js, stream_finished_on_error, state);

  stream_record_set(js, state, STREAM_FINISHED_ON_FINISH, on_finish);
  stream_record_set(js, state, STREAM_FINISHED_ON_ERROR, on_error);

  static const char *const events[] = { "end", "finish", "close" };
  for (size_t i = 0; i < countof(events); i++) {
    ant_value_t added = stream_add_listener(js, stream_obj, events[i], on_finish, false);
  if (is_err(added)) return added;
  }
  
  ant_value_t added = stream_add_listener(js, stream_obj, "error", on_error, false);
  return is_err(added) ? added : stream_obj;
}

static ant_value_t js_stream_finished(ant_params_t) {
  ant_value_t callback = nargs > 1 ? args[1] : js_mkundef();
  if (nargs < 1 || !is_object_type(args[0])) return js_mkerr(js, "finished requires a stream");
  return stream_finished_register(js, args[0], callback);
}

static ant_value_t stream_pipeline_done(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (stream_record_get(js, state, STREAM_PIPELINE_CALLED) == js_true) return js_mkundef();

  stream_record_set(js, state, STREAM_PIPELINE_CALLED, js_true);
  ant_value_t callback = stream_record_get(js, state, STREAM_PIPELINE_CALLBACK);

  if (nargs > 0 && !is_undefined(args[0])) stream_call_callback(js, callback, &args[0], 1);
  else stream_call_callback(js, callback, NULL, 0);

  return js_mkundef();
}

static ant_value_t stream_pipeline_error(ant_params_t) {
  ant_value_t done = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (nargs > 0 && !is_undefined(args[0])) stream_call_callback(js, done, &args[0], 1);
  return js_mkundef();
}

static ant_value_t stream_pipeline_schedule_done(ant_params_t) {
  stream_call_callback(js, nargs > 0 ? args[0] : js_mkundef(), NULL, 0);
  return js_mkundef();
}

static ant_value_t js_stream_pipeline(ant_params_t) {
  int stream_count = nargs;
  ant_value_t callback = js_mkundef();

  if (nargs > 0 && is_callable(args[nargs - 1])) {
    callback = args[nargs - 1];
    stream_count--;
  }
  
  if (!is_callable(callback)) callback = Ant_Function_Create(stream_noop);
  if (stream_count <= 0) return js_mkundef();

  ant_value_t done = js_heavy_mkfun(js, stream_pipeline_done, stream_record(js, 2, callback, js_false));

  if (stream_count < 2) {
    stream_tick(js, stream_pipeline_schedule_done, 1, done);
    return args[0];
  }

  for (int i = 0; i < stream_count - 1; i++) {
    ant_value_t finished_args[2] = { args[i], js_heavy_mkfun(js, stream_pipeline_error, done) };
    ant_value_t registered = js_stream_finished(js, finished_args, 2, js_mkundef());
    if (is_err(registered)) return registered;
    
    ant_value_t pipe_args[2] = { args[i + 1], js_mkundef() };
    ant_value_t piped = stream_call_prop(js, args[i], "pipe", pipe_args, 2);
    if (is_err(piped)) return piped;
  }

  ant_value_t finished_args[2] = { args[stream_count - 1], done };
    ant_value_t registered = js_stream_finished(js, finished_args, 2, js_mkundef());
    if (is_err(registered)) return registered;

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

  if (nargs < 1 || !is_object_type(args[0])) {
    js_reject_promise(js, promise, js_mkerr(js, "finished requires a stream"));
    return promise;
  }

  ant_value_t result = stream_finished_register(js, args[0], js_heavy_mkfun(js, stream_promise_callback, promise));
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

static void stream_release_reader(ant_t *js, ant_value_t state) {
  ant_value_t reader = stream_record_get(js, state, STREAM_FROM_READER);
  if (is_object_type(reader)) stream_call_prop(js, reader, "releaseLock", NULL, 0);
}

static ant_value_t stream_readable_from_step(ant_params_t);

static ant_value_t stream_readable_from_fail(ant_t *js, ant_value_t state, ant_value_t error) {
  GC_ROOT_SAVE(root_mark, js);
  if (is_err(error)) error = js_take_thrown(js, error);
  GC_ROOT_PIN(js, error);

  ant_value_t readable = stream_record_get(js, state, STREAM_FROM_READABLE);
  stream_release_reader(js, state);

  if (stream_is_instance(readable)) {
    ant_value_t destroy_args[1] = { error };
    sv_call_native_this(js, js_stream_destroy, readable, destroy_args, 1);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t stream_readable_from_handle(ant_t *js, ant_value_t state, ant_value_t result) {
  ant_value_t readable = stream_record_get(js, state, STREAM_FROM_READABLE);

  if (stream_is_destroyed(js, readable)) {
    stream_release_reader(js, state);
    return js_mkundef();
  }

  if (!is_object_type(result)) return stream_readable_from_fail(
    js, state, Ant_Error_Create(js, JS_ERR_TYPE, "iterator step must be an object"));

  ant_value_t done = js_get(js, result, "done");
  if (is_err(done)) return stream_readable_from_fail(js, state, done);
  ant_value_t value = js_get(js, result, "value");
  if (is_err(value)) return stream_readable_from_fail(js, state, value);

  if (js_truthy(js, done)) {
    stream_release_reader(js, state);
    stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
    return js_mkundef();
  }

  stream_readable_push_value(js, readable, value, js_mkundef());
  if (!stream_is_destroyed(js, readable)) stream_tick(js, stream_readable_from_step, 1, state);

  return js_mkundef();
}

static ant_value_t stream_readable_from_on_result(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  return stream_readable_from_handle(js, state, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t stream_readable_from_reject(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t error = nargs > 0 ? args[0] : Ant_Error_Create(js, JS_ERR_GENERIC, "stream iteration failed");
  return stream_readable_from_fail(js, state, error);
}

static ant_value_t stream_readable_from_step(ant_params_t) {
  ant_value_t state = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t readable = stream_record_get(js, state, STREAM_FROM_READABLE);
  ant_value_t iterator = stream_record_get(js, state, STREAM_FROM_ITERATOR);
  bool reader_mode = stream_record_get(js, state, STREAM_FROM_READER_MODE) == js_true;
  
  if (stream_is_destroyed(js, readable)) {
    stream_release_reader(js, state);
    return js_mkundef();
  }

  ant_value_t next_result = stream_call_prop(js, iterator, reader_mode ? "read" : "next", NULL, 0);
  if (is_err(next_result)) return stream_readable_from_fail(js, state, next_result);

  bool native_promise = vtype(next_result) == kTypePromise;
  ant_value_t then_fn = !native_promise && is_object_type(next_result) ? js_getprop_fallback(js, next_result, "then") : js_mkundef();
  if (is_err(then_fn)) return stream_readable_from_fail(js, state, then_fn);
  if (!native_promise && !is_callable(then_fn)) return stream_readable_from_handle(js, state, next_result);

  ant_value_t callbacks[2] = {
    js_heavy_mkfun(js, stream_readable_from_on_result, state),
    js_heavy_mkfun(js, stream_readable_from_reject, state),
  };

  if (!native_promise) {
    ant_value_t result = stream_call(js, then_fn, next_result, callbacks, 2);
    return is_err(result) ? stream_readable_from_fail(js, state, result) : js_mkundef();
  }

  ant_value_t then_result = js_promise_then(js, next_result, callbacks[0], callbacks[1]);
  if (is_err(then_result)) return stream_readable_from_fail(js, state, then_result);
    promise_mark_handled(then_result);

    return js_mkundef();
  }

static ant_value_t stream_readable_from_start(ant_params_t) {
  ant_value_t state = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t readable = stream_record_get(js, state, STREAM_FROM_READABLE);
  ant_value_t source = stream_record_get(js, state, STREAM_FROM_SOURCE);
  iterator_t it;

  if (stream_is_destroyed(js, readable)) return js_mkundef();

  if (vtype(source) == kTypeString || (is_object_type(source) && js_instance_of_proto(js, source, js->builtins.buffer_proto))) {
    stream_readable_push_value(js, readable, source, js_mkundef());
    stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
    return js_mkundef();
  }

  ant_value_t async_iter_fn = is_object_type(source) ? js_get_sym(js, source, js->sym.asyncIterator_sym) : js_mkundef();
  if (is_err(async_iter_fn)) return stream_readable_from_fail(js, state, async_iter_fn);

  if (is_callable(async_iter_fn)) {
    ant_value_t iterator = stream_call(js, async_iter_fn, source, NULL, 0);
    if (is_err(iterator)) return stream_readable_from_fail(js, state, iterator);
    stream_record_set(js, state, STREAM_FROM_ITERATOR, iterator);
    stream_tick(js, stream_readable_from_step, 1, state);
    return js_mkundef();
  }

  if (js_iter_open(js, source, &it)) {
    ant_value_t value = 0;
    while (js_iter_next(js, &it, &value)) {
      if (stream_is_destroyed(js, readable)) break;
      stream_readable_push_value(js, readable, value, js_mkundef());
    }
    js_iter_close(js, &it);
    if (Ant_Exception_Pending(js)) return stream_readable_from_fail(js, state, Ant_Exception_Current(js));
    if (!stream_is_destroyed(js, readable)) stream_readable_push_value(js, readable, js_mknull(), js_mkundef());
    return js_mkundef();
  }

  if (Ant_Exception_Pending(js)) return stream_readable_from_fail(js, state, Ant_Exception_Current(js));
  ant_value_t reader_fn = is_object_type(source) ? js_get(js, source, "getReader") : js_mkundef();
  if (is_err(reader_fn)) return stream_readable_from_fail(js, state, reader_fn);

  if (is_callable(reader_fn)) {
    ant_value_t reader = stream_call(js, reader_fn, source, NULL, 0);
    if (is_err(reader)) return stream_readable_from_fail(js, state, reader);
    stream_record_set(js, state, STREAM_FROM_READER, reader);
    stream_record_set(js, state, STREAM_FROM_ITERATOR, reader);
    stream_record_set(js, state, STREAM_FROM_READER_MODE, js_true);
    stream_tick(js, stream_readable_from_step, 1, state);
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

  ant_value_t state = stream_record(js, 5, readable, source, js_mkundef(), js_mkundef(), js_false);
  stream_tick(js, stream_readable_from_start, 1, state);

  return readable;
}

static ant_value_t js_readable_from(ant_params_t) {
  ant_value_t options = stream_readable_from_options(js, nargs > 1 ? args[1] : js_mkundef());
  return stream_readable_from_source(js, nargs > 0 ? args[0] : js_mkundef(), options);
}

static ant_value_t js_readable_from_web(ant_params_t) {
  return stream_readable_from_source(js, nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef());
}

static bool stream_to_web_close(ant_t *js, ant_value_t state) {
  if (stream_record_get(js, state, STREAM_WEB_CLOSED) == js_true) return false;
  stream_record_set(js, state, STREAM_WEB_CLOSED, js_true);

  ant_value_t source = stream_record_get(js, state, STREAM_WEB_SOURCE);
  if (!is_object_type(source)) return true;

  stream_remove_listener(js, source, "data", stream_record_get(js, state, STREAM_WEB_ON_DATA));
  stream_remove_listener(js, source, "end", stream_record_get(js, state, STREAM_WEB_ON_END));
  stream_remove_listener(js, source, "error", stream_record_get(js, state, STREAM_WEB_ON_ERROR));

  return true;
}

static ant_value_t stream_to_web_on_data(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (stream_record_get(js, state, STREAM_WEB_CLOSED) == js_true) return js_mkundef();
  return rs_controller_enqueue(js, stream_record_get(js, state, STREAM_WEB_CONTROLLER), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t stream_to_web_on_end(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (stream_to_web_close(js, state)) rs_controller_close(js, stream_record_get(js, state, STREAM_WEB_CONTROLLER));
  return js_mkundef();
}

static ant_value_t stream_to_web_on_error(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t error = nargs > 0 ? args[0] : Ant_Error_Create(js, JS_ERR_GENERIC, "stream error");
  if (stream_to_web_close(js, state)) readable_stream_error(js, stream_record_get(js, state, STREAM_WEB_STREAM), error);
  return js_mkundef();
}

static ant_value_t stream_to_web_cancel(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t source = stream_record_get(js, state, STREAM_WEB_SOURCE);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();

  if (!stream_to_web_close(js, state) || !is_object_type(source)) return js_mkundef();

  ant_value_t destroy_fn = js_getprop_fallback(js, source, "destroy");
  if (!is_callable(destroy_fn)) return js_mkundef();

  ant_value_t cancel_result = stream_call(js, destroy_fn, source, &reason, 1);
  return is_err(cancel_result) ? cancel_result : js_mkundef();
}

static ant_value_t js_readable_to_web(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(source))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Readable.toWeb requires a stream");

  ant_value_t state = stream_record(
    js, 7, source, js_mkundef(), js_mkundef(), js_false,
    js_mkundef(), js_mkundef(), js_mkundef()
  );

  ant_value_t web_stream = rs_create_stream(js, js_mkundef(), js_heavy_mkfun(js, stream_to_web_cancel, state), 1.0);
  if (is_err(web_stream)) return web_stream;

  ant_value_t on_data = js_heavy_mkfun(js, stream_to_web_on_data, state);
  ant_value_t on_end = js_heavy_mkfun(js, stream_to_web_on_end, state);
  ant_value_t on_error = js_heavy_mkfun(js, stream_to_web_on_error, state);

  stream_record_set(js, state, STREAM_WEB_STREAM, web_stream);
  stream_record_set(js, state, STREAM_WEB_CONTROLLER, rs_stream_controller(js, web_stream));
  stream_record_set(js, state, STREAM_WEB_ON_DATA, on_data);
  stream_record_set(js, state, STREAM_WEB_ON_END, on_end);
  stream_record_set(js, state, STREAM_WEB_ON_ERROR, on_error);

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

static ant_offset_t stream_iter_head(ant_t *js, ant_value_t state) {
  return (ant_offset_t)js_getnum(stream_record_get(js, state, STREAM_ITER_HEAD));
}

static double stream_iter_queued(ant_t *js, ant_value_t state) {
  ant_value_t queue = stream_record_get(js, state, STREAM_ITER_QUEUE);
  return (double)js_arr_len(js, queue) - (double)stream_iter_head(js, state);
}
  
static void stream_iter_clear_queue(ant_t *js, ant_value_t state) {
  js_arr_set_dense_length(js, stream_record_get(js, state, STREAM_ITER_QUEUE), 0);
  stream_record_set(js, state, STREAM_ITER_HEAD, js_mknum(0));
}

static void stream_iter_detach(ant_t *js, ant_value_t state) {
  if (stream_record_get(js, state, STREAM_ITER_DETACHED) == js_true) return;
  stream_record_set(js, state, STREAM_ITER_DETACHED, js_true);

  ant_value_t stream_obj = stream_record_get(js, state, STREAM_ITER_STREAM);
  ant_value_t on_end = stream_record_get(js, state, STREAM_ITER_ON_END);
  
  stream_remove_listener(js, stream_obj, "data", stream_record_get(js, state, STREAM_ITER_ON_DATA));
  stream_remove_listener(js, stream_obj, "end", on_end);
  stream_remove_listener(js, stream_obj, "error", stream_record_get(js, state, STREAM_ITER_ON_ERROR));
  stream_remove_listener(js, stream_obj, "close", on_end);
}

static void stream_iter_settle(ant_t *js, ant_value_t state, ant_value_t result, bool reject) {
  ant_value_t pending = stream_record_get(js, state, STREAM_ITER_PENDING);
  if (vtype(pending) != kTypePromise) return;
  
  stream_record_set(js, state, STREAM_ITER_PENDING, js_mkundef());
  if (reject) js_reject_promise(js, pending, result);
  else js_resolve_promise(js, pending, result);
}

static ant_value_t stream_iter_on_data(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();

  if (vtype(stream_record_get(js, state, STREAM_ITER_PENDING)) == kTypePromise) {
    stream_iter_settle(js, state, js_iter_result(js, true, chunk), false);
    return js_mkundef();
  }

  js_arr_push(js, stream_record_get(js, state, STREAM_ITER_QUEUE), chunk);
  if (stream_iter_queued(js, state) >= STREAM_ITER_PAUSE_AT && stream_record_get(js, state, STREAM_ITER_PAUSED) != js_true) {
    stream_record_set(js, state, STREAM_ITER_PAUSED, js_true);
    stream_call_prop(js, stream_record_get(js, state, STREAM_ITER_STREAM), "pause", NULL, 0);
  }
  
  return js_mkundef();
}

static ant_value_t stream_iter_on_end(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  stream_record_set(js, state, STREAM_ITER_ENDED, js_true);
  
  stream_iter_detach(js, state);
  stream_iter_settle(js, state, js_iter_result(js, false, js_mkundef()), false);
  
  return js_mkundef();
}

static ant_value_t stream_iter_on_error(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();
  stream_iter_detach(js, state);

  if (vtype(stream_record_get(js, state, STREAM_ITER_PENDING)) == kTypePromise) {
    stream_record_set(js, state, STREAM_ITER_ENDED, js_true);
    stream_iter_settle(js, state, error, true);
  } else {
    stream_record_set(js, state, STREAM_ITER_ERROR, error);
    stream_record_set(js, state, STREAM_ITER_HAS_ERROR, js_true);
  }
  
  return js_mkundef();
}

static ant_value_t stream_iter_next(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);

  if (stream_iter_queued(js, state) > 0) {
    ant_value_t queue = stream_record_get(js, state, STREAM_ITER_QUEUE);
    ant_offset_t head = stream_iter_head(js, state);
    ant_value_t chunk = js_arr_get(js, queue, head);

    if (head + 1 >= js_arr_len(js, queue)) stream_iter_clear_queue(js, state);
    else stream_record_set(js, state, STREAM_ITER_HEAD, js_mknum((double)(head + 1)));

    if (
      stream_record_get(js, state, STREAM_ITER_PAUSED) == js_true &&
      stream_iter_queued(js, state) <= STREAM_ITER_RESUME_AT &&
      stream_record_get(js, state, STREAM_ITER_DETACHED) != js_true
    ) {
      stream_record_set(js, state, STREAM_ITER_PAUSED, js_false);
      stream_call_prop(js, stream_record_get(js, state, STREAM_ITER_STREAM), "resume", NULL, 0);
    }
    
    return stream_iter_resolved(js, js_iter_result(js, true, chunk));
  }

  if (stream_record_get(js, state, STREAM_ITER_HAS_ERROR) == js_true) {
    ant_value_t error = stream_record_get(js, state, STREAM_ITER_ERROR);
    stream_record_set(js, state, STREAM_ITER_HAS_ERROR, js_false);
    stream_record_set(js, state, STREAM_ITER_ERROR, js_mkundef());
    stream_record_set(js, state, STREAM_ITER_ENDED, js_true);
    return stream_iter_rejected(js, error);
  }

  if (stream_record_get(js, state, STREAM_ITER_ENDED) == js_true)
    return stream_iter_resolved(js, js_iter_result(js, false, js_mkundef()));

  ant_value_t pending = stream_record_get(js, state, STREAM_ITER_PENDING);
  if (vtype(pending) == kTypePromise) return pending;

  pending = js_mkpromise(js);
  stream_record_set(js, state, STREAM_ITER_PENDING, pending);
  
  return pending;
}

static ant_value_t stream_iter_return(ant_params_t) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t done = js_iter_result(js, false, nargs > 0 ? args[0] : js_mkundef());
  ant_value_t stream_obj = stream_record_get(js, state, STREAM_ITER_STREAM);

  stream_record_set(js, state, STREAM_ITER_ENDED, js_true);
  stream_iter_clear_queue(js, state);
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

  bool ended = js_truthy(js, js_get(js, stream_obj, "readableEnded")) || stream_is_destroyed(js, stream_obj);

  ant_value_t state = stream_record(
    js, 12, stream_obj, js_mkarr(js), js_mknum(0), js_mkundef(), js_mkundef(),
    js_false, js_false, js_bool(ended), js_bool(ended), js_mkundef(), js_mkundef(), js_mkundef()
  );
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, state);

  ant_value_t on_data = js_heavy_mkfun(js, stream_iter_on_data, state);
  ant_value_t on_end = js_heavy_mkfun(js, stream_iter_on_end, state);
  ant_value_t on_error = js_heavy_mkfun(js, stream_iter_on_error, state);

  stream_record_set(js, state, STREAM_ITER_ON_DATA, on_data);
  stream_record_set(js, state, STREAM_ITER_ON_END, on_end);
  stream_record_set(js, state, STREAM_ITER_ON_ERROR, on_error);

  ant_value_t iterator = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, iterator);

  js_set(js, iterator, "next", js_heavy_mkfun(js, stream_iter_next, state));
  js_set(js, iterator, "return", js_heavy_mkfun(js, stream_iter_return, state));
  js_set_sym(js, iterator, js->sym.asyncIterator_sym, Ant_Function_Create(sym_this_cb));

  if (!ended) {
    const char *const events[] = { "error", "end", "close", "data" };
    const ant_value_t listeners[] = { on_error, on_end, on_end, on_data };
    for (size_t i = 0; i < countof(events); i++) {
      ant_value_t added = stream_add_listener(js, stream_obj, events[i], listeners[i], false);
      if (is_err(added)) { GC_ROOT_RESTORE(js, root_mark); return added; }
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return iterator;
}

static ant_value_t js_stream_ctor(ant_params_t) {
  return stream_construct(js, js->builtins.stream_proto, nargs > 0 ? args[0] : js_mkundef(), stream_init_legacy, call_new_target);
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

static void stream_define_writable_methods(ant_t *js, ant_value_t proto) {
  js_set(js, proto, "_write", Ant_Function_CreateWithArity(js_writable__write, 3));
  js_set(js, proto, "_final", Ant_Function_Create(js_writable__final));
  js_set(js, proto, "write", Ant_Function_CreateWithArity(js_writable_write, 3));
  js_set(js, proto, "end", Ant_Function_CreateWithArity(js_writable_end, 3));
  js_set(js, proto, "cork", Ant_Function_Create(stream_noop));
  js_set(js, proto, "uncork", Ant_Function_Create(stream_noop));
  stream_define_writable_accessors(js, proto);
}

void stream_init_constructors(ant_t *js) {
  if (js->builtins.stream_ctor) return;

  ant_value_t events = events_library(js);
  ant_value_t ee_ctor = js_get(js, events, "EventEmitter");
  ant_value_t ee_proto = js_get(js, ee_ctor, "prototype");

  js->builtins.readable_state_proto = Ant_Object_CreateBare(js);
  js->builtins.writable_state_proto = Ant_Object_CreateBare(js);
  stream_define_fields(js, js->builtins.readable_state_proto, STREAM_ON_READABLE_STATE);
  stream_define_fields(js, js->builtins.writable_state_proto, STREAM_ON_WRITABLE_STATE);
  js->builtins.stream_default_destroy = Ant_Function_CreateWithMeta(js_stream__destroy, "_destroy", 2, 0);

  js->builtins.stream_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.stream_proto, ee_proto);
  js_set(js, js->builtins.stream_proto, "pipe", Ant_Function_CreateWithArity(js_stream_pipe, 2));
  js_set(js, js->builtins.stream_proto, "unpipe", Ant_Function_Create(js_stream_unpipe));
  js_set(js, js->builtins.stream_proto, "pause", Ant_Function_Create(js_stream_pause));
  js_set(js, js->builtins.stream_proto, "resume", Ant_Function_Create(js_stream_resume));
  js_set(js, js->builtins.stream_proto, "isPaused", Ant_Function_Create(js_stream_is_paused));
  js_set(js, js->builtins.stream_proto, "destroy", Ant_Function_Create(js_stream_destroy));
  stream_define_stream_accessors(js, js->builtins.stream_proto);
  js_set_tostringtag(js, js->builtins.stream_proto, js_mkstr(js, "Stream", 6));
  js->builtins.stream_ctor = Ant_Function_CreateConstructor(js, js_stream_ctor, js->builtins.stream_proto, "Stream", 1);

  js->builtins.readable_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.readable_proto, js->builtins.stream_proto);
  js_set(js, js->builtins.readable_proto, "_read", Ant_Function_CreateWithArity(js_readable__read, 1));
  js_set(js, js->builtins.readable_proto, "_destroy", js->builtins.stream_default_destroy);
  js_set(js, js->builtins.readable_proto, "push", Ant_Function_CreateWithArity(js_readable_push, 2));
  js_set(js, js->builtins.readable_proto, "read", Ant_Function_CreateWithArity(js_readable_read, 1));
  js_set(js, js->builtins.readable_proto, "setEncoding", Ant_Function_CreateWithArity(js_readable_set_encoding, 1));
  js_set(js, js->builtins.readable_proto, "on", Ant_Function_CreateWithArity(js_readable_on, 2));
  js_set_exact(js, js->builtins.readable_proto, "addListener", js_get(js, js->builtins.readable_proto, "on"));
  js_set(js, js->builtins.readable_proto, "once", Ant_Function_Create(js_readable_once));
  js_set(js, js->builtins.readable_proto, "resume", Ant_Function_Create(js_readable_resume));
  js_set(js, js->builtins.readable_proto, "pause", Ant_Function_Create(js_readable_pause));
  stream_define_fields(js, js->builtins.readable_proto, STREAM_ON_READABLE);
  js_set_sym(js, js->builtins.readable_proto, js->sym.asyncIterator_sym, Ant_Function_Create(js_readable_async_iterator));
  js_set_tostringtag(js, js->builtins.readable_proto, js_mkstr(js, "Readable", 8));
  js->builtins.readable_ctor = Ant_Function_CreateConstructor(js, js_readable_ctor, js->builtins.readable_proto, "Readable", 1);
  js_set(js, js->builtins.readable_ctor, "from", Ant_Function_Create(js_readable_from));
  js_set(js, js->builtins.readable_ctor, "fromWeb", Ant_Function_Create(js_readable_from_web));
  js_set(js, js->builtins.readable_ctor, "toWeb", Ant_Function_Create(js_readable_to_web));

  js->builtins.writable_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.writable_proto, js->builtins.stream_proto);
  js_set(js, js->builtins.writable_proto, "_destroy", js->builtins.stream_default_destroy);
  stream_define_writable_methods(js, js->builtins.writable_proto);
  js_set_tostringtag(js, js->builtins.writable_proto, js_mkstr(js, "Writable", 8));
  js->builtins.writable_ctor = Ant_Function_CreateConstructor(js, js_writable_ctor, js->builtins.writable_proto, "Writable", 1);
  js_set_sym(js, js->builtins.writable_ctor, js->sym.hasInstance_sym, Ant_Function_CreateWithArity(js_writable_has_instance, 1));

  js->builtins.duplex_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.duplex_proto, js->builtins.readable_proto);
  stream_define_writable_methods(js, js->builtins.duplex_proto);
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

static void stream_init_object(
  ant_t *js, ant_value_t obj, ant_value_t options,
  void (*init_fn)(ant_t *, ant_value_t, ant_value_t)
) {
  stream_init_constructors(js);
  if (!is_object_type(obj)) return;

  init_fn(js, obj, options);
  if (js_instance_of_proto(js, obj, js->builtins.stream_proto)) return;

  stream_private_state_t *priv = stream_private_state(obj);
  stream_define_stream_accessors(js, obj);
  if (priv && priv->has_readable) stream_define_fields(js, obj, STREAM_ON_READABLE);
  if (priv && priv->has_writable) stream_define_writable_accessors(js, obj);
}

void stream_init_readable_object(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_object(js, obj, options, stream_init_readable);
}

void stream_init_writable_object(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_object(js, obj, options, stream_init_writable);
}

void stream_init_duplex_object(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_object(js, obj, options, stream_init_duplex);
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

  if (!priv) return js_mkundef();
  priv->constructing = false;

  if (priv->destroy_pending) {
    ant_value_t pending_err = priv->destroy_error;
    priv->destroy_pending = false;
    stream_store(js, priv, &priv->destroy_error, js_mkundef());
    if (failed) stream_set_errored(js, priv, err);
    return stream_run_destroy(js, priv, failed ? err : pending_err);
  }

  if (failed) {
    sv_call_native_this(js, js_stream_destroy, stream_obj, &err, 1);
    return js_mkundef();
  }

  if (priv->has_readable && (priv->readable.flowing || priv->readable.readable_listening)) stream_rs_maybe_read(js, priv);
  if (priv->has_writable) return stream_writable_drain(js, priv);

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
  stream_tick(js, stream_construct_tick, 1, stream_obj);
}

ant_value_t stream_construct(
  ant_t *js, ant_value_t base_proto, ant_value_t raw_options,
  void (*init_fn)(ant_t *, ant_value_t, ant_value_t), ant_value_t new_target
) {
  ant_value_t proto = js_instance_proto_from_new_target(js, base_proto, new_target);
  bool subclassed = is_object_type(proto) && proto != base_proto;
  ant_value_t obj = Ant_Object_CreateBare(js);

  js_set_proto_init(obj, subclassed ? proto : base_proto);
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

static bool stream_state_flag(ant_t *js, ant_value_t stream_obj, const char *state_key, const char *field) {
  ant_value_t state = js_get(js, stream_obj, state_key);
  return is_object_type(state) && js_truthy(js, js_get(js, state, field));
}

static ant_value_t js_stream_is_destroyed(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(stream_obj)) return js_false;
  return js_bool(stream_is_destroyed(js, stream_obj));
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
