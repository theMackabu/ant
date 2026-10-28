// TODO: split into multiple files!

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

#include "modules/abort.h"
#include "modules/assert.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/stream.h"
#include "modules/iterator.h"
#include "modules/string_decoder.h"
#include "modules/symbol.h"
#include "streams/readable.h"
#include "tokens.h"

static constexpr uint32_t STREAM_OP_TAG = Ant_NativeTag("STRO");
static constexpr uint32_t STREAM_NATIVE_TAG = Ant_NativeTag("STRM");
static constexpr uint32_t STREAM_READABLE_STATE_TAG = Ant_NativeTag("STRR");
static constexpr uint32_t STREAM_WRITABLE_STATE_TAG = Ant_NativeTag("STRW");

typedef struct {
  ant_value_t chunk;
  ant_value_t encoding;
  ant_value_t callback;
  double size;
} stream_write_entry_t;

#define STREAM_OP(name, ...) typedef struct { ant_value_t __VA_ARGS__; } stream_##name##_op_t;
#include "stream/ops.h"

typedef union {
#define STREAM_OP(name, ...) stream_##name##_op_t name;
#include "stream/ops.h"
} stream_op_fields_t;

static constexpr int STREAM_OP_VALUES = sizeof(stream_op_fields_t) / sizeof(ant_value_t);

typedef enum {
  STREAM_FROM_VALUE,
  STREAM_FROM_ASYNC,
  STREAM_FROM_SYNC,
  STREAM_FROM_READER,
} stream_from_mode_t;

typedef struct {
  ant_object_t *holder;
  union {
    ant_value_t values[STREAM_OP_VALUES];
#define STREAM_OP(name, ...) stream_##name##_op_t name;
#include "stream/ops.h"
  };
  ant_offset_t iter_head;
  ant_get_cache_t write_cache;
  ant_get_cache_t pause_cache;
  ant_get_cache_t resume_cache;
  js_iter_advance_fn from_advance;
  stream_from_mode_t from_mode;
  bool called;
  bool pipe_end_dest;
  bool pipe_cleaned;
  bool web_closed;
  bool from_reading;
  bool iter_has_error;
  bool iter_paused;
  bool iter_detached;
  bool iter_ended;
} stream_op_t;

typedef enum {
  STREAM_WRITE_THROWN = -1,
  STREAM_WRITE_PENDING = 0,
  STREAM_WRITE_COMPLETE = 1,
} stream_write_status_t;

typedef enum {
#define STREAM_METHOD(id, name) STREAM_METHOD_##id,
#include "stream/methods.h"
  STREAM_METHOD_COUNT
} stream_method_t;

static const struct { const char *name; size_t len; } stream_method_keys[] = {
#define STREAM_METHOD(id, name) { name, sizeof(name) - 1 },
#include "stream/methods.h"
};

typedef enum {
  STREAM_TRI_NULL = -1,
  STREAM_TRI_FALSE = 0,
  STREAM_TRI_TRUE = 1,
} stream_tristate_t;

typedef struct {
  ant_value_t value;
  double size;
} stream_chunk_t;

typedef struct {
  stream_chunk_t *chunks;
  ant_value_t decoder;
  ant_value_t encoding;
  ant_value_t default_encoding;
  ant_value_t errored;
  double length;
  double high_water_mark;
  uint32_t chunk_head;
  uint32_t chunk_count;
  uint32_t chunk_cap;
  size_t head_offset;
  stream_tristate_t flowing;
  stream_tristate_t paused;
  bool object_mode;
  bool auto_destroy;
  bool ended;
  bool end_emitted;
  bool end_scheduled;
  bool readable_listening;
  bool need_readable;
  bool emitted_readable;
  bool resume_scheduled;
  bool reading_more;
  bool data_emitted;
  bool reading;
  bool sync;
  bool in_read;
} stream_readable_state_t;

typedef struct {
  stream_write_entry_t *queue;
  ant_value_t errored;
  ant_value_t onwrite;
  ant_value_t write_callback;
  ant_value_t end_callback;
  double length;
  double high_water_mark;
  double write_size;
  uint32_t queue_head;
  uint32_t queue_len;
  uint32_t queue_cap;
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
  ant_value_t from_read;
  void *attached_state;
  stream_finalize_fn attached_state_finalize;
  stream_end_fn end_hook;
  ant_get_cache_t methods[STREAM_METHOD_COUNT];
  eventemitter_ref_t data_event;
  eventemitter_ref_t readable_event;
  alignas(max_align_t) unsigned char inline_state[STREAM_INLINE_STATE_SIZE];
  bool has_readable;
  bool has_writable;
  bool is_readable;
  bool destroyed;
  bool paused;
  bool emit_close;
  bool error_emitted;
  bool close_emitted;
  bool closed;
  bool constructing;
  bool destroy_pending;
  bool transform_pending;
} stream_private_state_t;

typedef enum {
  STREAM_FIELD_BOOL,
  STREAM_FIELD_NUM,
  STREAM_FIELD_VALUE,
  STREAM_FIELD_BUFFER,
  STREAM_FIELD_TRI,
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
  STREAM_FIELD(STREAM,         "closed",                BOOL,   closed,                      true),
  STREAM_FIELD(STREAM,         "readable",              BOOL,   is_readable,                 false),
  STREAM_FIELD(READABLE,       "readableEnded",         BOOL,   readable.end_emitted,        true),
  STREAM_FIELD(READABLE,       "readableHighWaterMark", NUM,    readable.high_water_mark,    true),
  STREAM_FIELD(READABLE,       "readableLength",        NUM,    readable.length,             true),
  STREAM_FIELD(READABLE,       "readableObjectMode",    BOOL,   readable.object_mode,        true),
  STREAM_FIELD(READABLE,       "readableFlowing",       TRI,    readable.flowing,            false),
  STREAM_FIELD(READABLE,       "readableEncoding",      VALUE,  readable.encoding,           true),
  STREAM_FIELD(READABLE,       "readableDidRead",       BOOL,   readable.data_emitted,       true),
  STREAM_FIELD(READABLE,       "errored",               VALUE,  readable.errored,            true),
  STREAM_FIELD(WRITABLE,       "writableEnded",         BOOL,   writable.ended,              true),
  STREAM_FIELD(WRITABLE,       "writableFinished",      BOOL,   writable.finished,           true),
  STREAM_FIELD(WRITABLE,       "writableHighWaterMark", NUM,    writable.high_water_mark,    true),
  STREAM_FIELD(WRITABLE,       "writableLength",        NUM,    writable.length,             true),
  STREAM_FIELD(WRITABLE,       "writableNeedDrain",     BOOL,   writable.need_drain,         true),
  STREAM_FIELD(READABLE_STATE, "objectMode",            BOOL,   readable.object_mode,        false),
  STREAM_FIELD(READABLE_STATE, "highWaterMark",         NUM,    readable.high_water_mark,    false),
  STREAM_FIELD(READABLE_STATE, "buffer",                BUFFER, readable.chunks,             false),
  STREAM_FIELD(READABLE_STATE, "length",                NUM,    readable.length,             false),
  STREAM_FIELD(READABLE_STATE, "flowing",               TRI,    readable.flowing,            false),
  STREAM_FIELD(READABLE_STATE, "ended",                 BOOL,   readable.ended,              false),
  STREAM_FIELD(READABLE_STATE, "endEmitted",            BOOL,   readable.end_emitted,        false),
  STREAM_FIELD(READABLE_STATE, "reading",               BOOL,   readable.reading,            false),
  STREAM_FIELD(READABLE_STATE, "sync",                  BOOL,   readable.sync,               false),
  STREAM_FIELD(READABLE_STATE, "needReadable",          BOOL,   readable.need_readable,      false),
  STREAM_FIELD(READABLE_STATE, "emittedReadable",       BOOL,   readable.emitted_readable,   false),
  STREAM_FIELD(READABLE_STATE, "readableListening",     BOOL,   readable.readable_listening, false),
  STREAM_FIELD(READABLE_STATE, "resumeScheduled",       BOOL,   readable.resume_scheduled,   false),
  STREAM_FIELD(READABLE_STATE, "readingMore",           BOOL,   readable.reading_more,       false),
  STREAM_FIELD(READABLE_STATE, "dataEmitted",           BOOL,   readable.data_emitted,       false),
  STREAM_FIELD(READABLE_STATE, "autoDestroy",           BOOL,   readable.auto_destroy,       false),
  STREAM_FIELD(READABLE_STATE, "emitClose",             BOOL,   emit_close,                  false),
  STREAM_FIELD(READABLE_STATE, "errorEmitted",          BOOL,   error_emitted,               false),
  STREAM_FIELD(READABLE_STATE, "closeEmitted",          BOOL,   close_emitted,               false),
  STREAM_FIELD(READABLE_STATE, "closed",                BOOL,   closed,                      false),
  STREAM_FIELD(READABLE_STATE, "destroyed",             BOOL,   destroyed,                   false),
  STREAM_FIELD(READABLE_STATE, "errored",               VALUE,  readable.errored,            false),
  STREAM_FIELD(READABLE_STATE, "decoder",               VALUE,  readable.decoder,            false),
  STREAM_FIELD(READABLE_STATE, "encoding",              VALUE,  readable.encoding,           false),
  STREAM_FIELD(WRITABLE_STATE, "objectMode",            BOOL,   writable.object_mode,        false),
  STREAM_FIELD(WRITABLE_STATE, "highWaterMark",         NUM,    writable.high_water_mark,    false),
  STREAM_FIELD(WRITABLE_STATE, "length",                NUM,    writable.length,             false),
  STREAM_FIELD(WRITABLE_STATE, "needDrain",             BOOL,   writable.need_drain,         false),
  STREAM_FIELD(WRITABLE_STATE, "ended",                 BOOL,   writable.ended,              false),
  STREAM_FIELD(WRITABLE_STATE, "finished",              BOOL,   writable.finished,           false),
  STREAM_FIELD(WRITABLE_STATE, "autoDestroy",           BOOL,   writable.auto_destroy,       false),
  STREAM_FIELD(WRITABLE_STATE, "destroyed",             BOOL,   destroyed,                   false),
  STREAM_FIELD(WRITABLE_STATE, "closed",                BOOL,   closed,                      false),
  STREAM_FIELD(WRITABLE_STATE, "errored",               VALUE,  writable.errored,            false),
};

static bool stream_is_instance(ant_value_t value) {
  return is_object_type(value) && js_check_native_tag(value, STREAM_NATIVE_TAG);
}

static inline stream_private_state_t *stream_private_state(ant_value_t value) {
  return js_get_native(value, STREAM_NATIVE_TAG);
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

bool stream_is_destroyed(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  return priv ? priv->destroyed : js_truthy(js, js_get(js, stream_obj, "destroyed"));
}

static stream_op_t *stream_op_new(ant_t *js, ant_value_t *holder_out) {
  stream_op_t *op = calloc(1, sizeof(*op));
  if (!op) return NULL;

  ant_value_t holder = Ant_Object_CreateBare(js);
  if (is_err(holder)) {
    free(op);
    return NULL;
  }

  for (int i = 0; i < STREAM_OP_VALUES; i++) op->values[i] = js_mkundef();
  op->holder = js_obj_ptr(holder);
  js_set_native(holder, op, STREAM_OP_TAG);

  *holder_out = holder;
  return op;
}

static inline stream_op_t *stream_op(ant_value_t holder) {
  ant_object_t *obj = js_obj_ptr(holder);
  return obj && obj->native_tag == STREAM_OP_TAG ? obj->native_ptr : NULL;
}

static inline stream_op_t *stream_op_current(ant_t *js) {
  return stream_op(js_func_data(js->current_func));
}

static inline ant_value_t stream_op_value(stream_op_t *op) {
  return js_obj_from_ptr(op->holder);
}

static void stream_op_barrier(ant_t *js, stream_op_t *op) {
  for (int i = 0; i < STREAM_OP_VALUES; i++) gc_write_barrier(js, op->holder, op->values[i]);
}

static inline void stream_op_store(ant_t *js, stream_op_t *op, ant_value_t *field, ant_value_t value) {
  *field = value;
  gc_write_barrier(js, op->holder, value);
}

static void stream_next_tick(ant_t *js, ant_cfunc_t fn, int count, ...) {
  ant_value_t args[2];
  va_list ap;

  va_start(ap, count);
  for (int i = 0; i < count; i++) args[i] = va_arg(ap, ant_value_t);
  va_end(ap);

  queue_next_tick_with_args(js, js_mkfun_dyn(fn), args, count);
}

static stream_private_state_t *stream_attach(ant_t *js, ant_value_t obj) {
  stream_private_state_t *priv = stream_private_state(obj);
  ant_value_t readable_state_obj = js_mkundef();
  ant_value_t writable_state_obj = js_mkundef();
  void *attached_state = NULL;
  stream_finalize_fn attached_state_finalize = NULL;
  unsigned char inline_state[STREAM_INLINE_STATE_SIZE] = {0};

  if (priv) {
    readable_state_obj = priv->readable_state_obj;
    writable_state_obj = priv->writable_state_obj;
    attached_state = priv->attached_state;
    attached_state_finalize = priv->attached_state_finalize;
    memcpy(inline_state, priv->inline_state, sizeof(inline_state));
    free(priv->writable.queue);
    free(priv->readable.chunks);
    for (int i = 0; i < STREAM_METHOD_COUNT; i++) js_get_cache_release(&priv->methods[i]);
    memset(priv, 0, sizeof(*priv));
  } else {
    if (!(priv = calloc(1, sizeof(*priv)))) return NULL;
    js_set_native(obj, priv, STREAM_NATIVE_TAG);
  }

  priv->obj = js_obj_ptr(obj);
  priv->attached_state = attached_state;
  priv->attached_state_finalize = attached_state_finalize;
  memcpy(priv->inline_state, inline_state, sizeof(inline_state));
  priv->readable_state_obj = readable_state_obj;
  priv->writable_state_obj = writable_state_obj;
  priv->pipes = js_mkundef();
  priv->destroy_error = js_mkundef();
  priv->transform_done = js_mkundef();
  priv->transform_callback = js_mkundef();
  priv->from_read = js_mkundef();
  priv->data_event = (eventemitter_ref_t){ .name = "data" };
  priv->readable_event = (eventemitter_ref_t){ .name = "readable" };
  priv->readable.decoder = js_mkundef();
  priv->readable.encoding = js_mknull();
  priv->readable.default_encoding = js_mkundef();
  priv->readable.errored = js_mknull();
  priv->readable.flowing = STREAM_TRI_NULL;
  priv->readable.paused = STREAM_TRI_NULL;
  priv->readable.sync = true;
  priv->writable.errored = js_mknull();
  priv->writable.onwrite = js_mkundef();
  priv->writable.write_callback = js_mkundef();
  priv->writable.end_callback = js_mkundef();
  priv->is_readable = true;
  priv->emit_close = true;

  return priv;
}

void gc_mark_stream_object(ant_t *js, ant_value_t value, gc_mark_fn mark) {
  stream_op_t *op = stream_op(value);
  if (op) {
    for (int i = 0; i < STREAM_OP_VALUES; i++) mark(js, op->values[i]);
    return;
  }

  stream_private_state_t *priv = js_get_native(value, STREAM_NATIVE_TAG);
  if (!priv) return;

  mark(js, priv->pipes);
  mark(js, priv->destroy_error);
  mark(js, priv->readable_state_obj);
  mark(js, priv->writable_state_obj);
  mark(js, priv->transform_done);
  mark(js, priv->transform_callback);
  mark(js, priv->from_read);
  stream_readable_state_t *rs = &priv->readable;
  for (uint32_t i = 0; i < rs->chunk_count; i++)
    mark(js, rs->chunks[(rs->chunk_head + i) & (rs->chunk_cap - 1)].value);
  mark(js, priv->readable.decoder);
  mark(js, priv->readable.encoding);
  mark(js, priv->readable.default_encoding);
  mark(js, priv->readable.errored);
  
  stream_writable_state_t *ws = &priv->writable;
  for (uint32_t i = ws->queue_head; i < ws->queue_len; i++) {
    mark(js, ws->queue[i].chunk);
    mark(js, ws->queue[i].encoding);
    mark(js, ws->queue[i].callback);
  }
  
  mark(js, priv->writable.errored);
  mark(js, priv->writable.onwrite);
  mark(js, priv->writable.write_callback);
  mark(js, priv->writable.end_callback);
}

void gc_finalize_stream_object(ant_t *js, ant_value_t value) {
  stream_op_t *op = stream_op(value);
  if (op) {
    js_clear_native(value, STREAM_OP_TAG);
    js_get_cache_release(&op->write_cache);
    js_get_cache_release(&op->pause_cache);
    js_get_cache_release(&op->resume_cache);
    free(op);
    return;
  }

  stream_private_state_t *priv = js_get_native(value, STREAM_NATIVE_TAG);
  if (!priv) return;

  js_clear_native(value, STREAM_NATIVE_TAG);
  if (priv->attached_state && priv->attached_state_finalize)
    priv->attached_state_finalize(js, value, priv->attached_state);
  for (int i = 0; i < STREAM_METHOD_COUNT; i++) js_get_cache_release(&priv->methods[i]);
  free(priv->readable.chunks);
  free(priv->writable.queue);
  free(priv);
}

void *stream_get_attached_state(ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  return priv ? priv->attached_state : NULL;
}

void *stream_get_inline_state(ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  return priv ? priv->inline_state : NULL;
}

void stream_set_end_hook(ant_value_t stream_obj, stream_end_fn hook) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (priv) priv->end_hook = hook;
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

bool stream_is_closed(ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  return priv && priv->closed;
}

bool stream_get_auto_destroy(ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return false;
  return priv->has_readable ? priv->readable.auto_destroy : priv->writable.auto_destroy;
}

void stream_set_auto_destroy(ant_value_t stream_obj, bool auto_destroy) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return;
  if (priv->has_readable) priv->readable.auto_destroy = auto_destroy;
  if (priv->has_writable) priv->writable.auto_destroy = auto_destroy;
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
  ant_value_t index = js_func_data(js->current_func);
  return &stream_fields[(size_t)js_getnum(index)];
}

static inline stream_private_state_t *stream_field_owner(ant_value_t receiver, const stream_field_t *field) {
  return js_get_native(receiver, stream_field_tag(field->target));
}

static inline stream_chunk_t *stream_chunk_at(stream_readable_state_t *rs, uint32_t i) {
  return &rs->chunks[(rs->chunk_head + i) & (rs->chunk_cap - 1)];
}

static bool stream_chunks_reserve(stream_readable_state_t *rs) {
  if (rs->chunk_count < rs->chunk_cap) return true;

  uint32_t cap = rs->chunk_cap ? rs->chunk_cap * 2 : 16;
  stream_chunk_t *next = malloc(cap * sizeof(*next));
  if (!next) return false;

  for (uint32_t i = 0; i < rs->chunk_count; i++) next[i] = *stream_chunk_at(rs, i);
  free(rs->chunks);
  rs->chunks = next;
  rs->chunk_cap = cap;
  rs->chunk_head = 0;

  return true;
}

static bool stream_chunks_add(ant_t *js, stream_private_state_t *priv, ant_value_t value, double size, bool front) {
  stream_readable_state_t *rs = &priv->readable;
  if (!stream_chunks_reserve(rs)) return false;

  if (front) {
    rs->chunk_head = (rs->chunk_head - 1) & (rs->chunk_cap - 1);
    *stream_chunk_at(rs, 0) = (stream_chunk_t){ value, size };
  } else *stream_chunk_at(rs, rs->chunk_count) = (stream_chunk_t){ value, size };

  rs->chunk_count++;
  gc_write_barrier(js, priv->obj, value);
  return true;
}

static void stream_chunks_set_head(ant_t *js, stream_private_state_t *priv, ant_value_t value, double size) {
  *stream_chunk_at(&priv->readable, 0) = (stream_chunk_t){ value, size };
  gc_write_barrier(js, priv->obj, value);
}

static void stream_chunks_clear(stream_readable_state_t *rs) {
  rs->chunk_head = 0;
  rs->chunk_count = 0;
  rs->head_offset = 0;
}

static ant_value_t stream_readable_buffer_array(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t arr = js_mkarr(js);

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, arr);
  for (uint32_t i = 0; i < rs->chunk_count; i++) js_arr_push(js, arr, stream_chunk_at(rs, i)->value);
  GC_ROOT_RESTORE(js, root_mark);

  return arr;
}

static double stream_chunk_size(ant_t *js, ant_value_t chunk, bool object_mode);

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
    case STREAM_FIELD_TRI:
      *(stream_tristate_t *)ptr = is_null(value) || is_undefined(value)
        ? STREAM_TRI_NULL
        : js_truthy(js, value) ? STREAM_TRI_TRUE : STREAM_TRI_FALSE;
      break;
    case STREAM_FIELD_BUFFER: {
      stream_readable_state_t *rs = &priv->readable;
      stream_chunks_clear(rs);
      ant_offset_t len = vtype(value) == kTypeArray ? js_arr_len(js, value) : 0;
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t chunk = js_arr_get(js, value, i);
        if (!stream_chunks_add(js, priv, chunk, stream_chunk_size(js, chunk, rs->object_mode), false))
          return Ant_Error_CreateOutOfMemory(js);
      }
      break;
    }
  }

  return js_mkundef();
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

  if (vtype(fn) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(fn);
    if (sv_closure_fast_callable(closure)) return sv_call_closure_fast(js, fn, closure, this_val, args, nargs);
  }

  const ant_cfunc_meta_t *meta = sv_native_closure_meta(fn);
  if (meta && sv_native_direct_allowed(js))
    return Ant_Silver_FinishNativeCall(js, sv_call_native_closure(js, fn, meta, this_val, args, nargs));

  return sv_vm_call_mode(
    js->vm, js, fn, this_val, args, nargs, NULL,
    SV_CALL_MODE_NORMAL, js_mkundef()
  );
}

static ant_value_t stream_method(ant_t *js, ant_value_t stream_obj, stream_method_t id) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  const char *name = stream_method_keys[id].name;
  size_t len = stream_method_keys[id].len;
  return priv ? js_get_cached(js, stream_obj, name, len, &priv->methods[id]) : js_getprop_fallback_len(js, stream_obj, name, len);
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
  if (vtype(value) == kTypeString) return Ant_Buffer_FromString(js, value, encoding);
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

static ant_value_t stream_coded_error(ant_t *js, const char *code, const char *message) {
  ant_value_t err = Ant_Error_Create(js, JS_ERR_GENERIC, message);
  if (is_object_type(err)) js_set(js, err, "code", js_mkstr(js, code, strlen(code)));
  return err;
}

static double stream_chunk_size(ant_t *js, ant_value_t chunk, bool object_mode) {
  const uint8_t *bytes = NULL;
  size_t byte_len = 0;
  if (object_mode) return 1;
  if (vtype(chunk) == kTypeString) return (double)str_utf16_len(js, chunk);
  if (buffer_source_get_bytes(js, chunk, &bytes, &byte_len)) return (double)byte_len;
  return 1;
}

static inline uint32_t stream_readable_buffer_len(stream_readable_state_t *rs) {
  return rs->chunk_count;
}

static ant_value_t stream_buffer_push_sized(ant_t *js, stream_private_state_t *priv, ant_value_t value, double size) {
  if (!stream_chunks_add(js, priv, value, size, false)) return Ant_Error_CreateOutOfMemory(js);
  priv->readable.length += size;
  return js_mkundef();
}

static ant_value_t stream_buffer_push(ant_t *js, stream_private_state_t *priv, ant_value_t value) {
  return stream_buffer_push_sized(js, priv, value, stream_chunk_size(js, value, priv->readable.object_mode));
}

static ant_value_t stream_bytes_view(ant_t *js, ant_value_t chunk, double start, double end) {
  if (!buffer_get_typedarray_data(chunk)) {
    chunk = stream_make_buffer(js, chunk, js_mkundef());
    if (is_err(chunk)) return chunk;
  }
  
  return buffer_byte_subarray(js, chunk, (size_t)start, (size_t)end);
}

static inline ant_value_t stream_readable_head(stream_readable_state_t *rs) {
  return stream_chunk_at(rs, 0)->value;
}

static inline double stream_rs_head_size(stream_readable_state_t *rs) {
  return stream_chunk_at(rs, 0)->size - (double)rs->head_offset;
}

static ant_value_t stream_readable_head_rest(ant_t *js, stream_readable_state_t *rs) {
  stream_chunk_t *chunk = stream_chunk_at(rs, 0);
  ant_value_t head = chunk->value;
  if (rs->head_offset == 0) return head;

  double start = (double)rs->head_offset;
  double end = chunk->size;

  return vtype(head) == kTypeString
    ? js_string_slice(js, head, start, end)
    : stream_bytes_view(js, head, start, end);
}

static ant_value_t stream_readable_settle_head(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (rs->head_offset == 0) return js_mkundef();

  if (stream_readable_buffer_len(rs) == 0) {
    rs->head_offset = 0;
    return js_mkundef();
  }

  double rest_size = stream_rs_head_size(rs);
  ant_value_t rest = stream_readable_head_rest(js, rs);
  if (is_err(rest)) return rest;

  stream_chunks_set_head(js, priv, rest, rest_size);
  rs->head_offset = 0;

  return js_mkundef();
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
    case STREAM_FIELD_TRI: {
      stream_tristate_t tri = *(stream_tristate_t *)ptr;
      return tri == STREAM_TRI_NULL ? js_mknull() : js_bool(tri == STREAM_TRI_TRUE);
    }
    case STREAM_FIELD_BUFFER: {
      ant_value_t settled = stream_readable_settle_head(js, priv);
      return is_err(settled) ? settled : stream_readable_buffer_array(js, priv);
    }
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

static void stream_define_writable_accessors(ant_t *js, ant_value_t target) {
  stream_define_fields(js, target, STREAM_ON_WRITABLE);
  js_set_getter_desc(js, target, "writable", 8, Ant_Function_Create(js_writable_writable_getter), JS_DESC_C);
}

static ant_value_t js_readable_aborted_getter(ant_params_t) {
  stream_private_state_t *priv = stream_readable_of(js_getthis(js));
  if (!priv) return js_false;

  stream_readable_state_t *rs = &priv->readable;
  bool errored = !is_undefined(rs->errored) && !is_null(rs->errored);
  return js_bool((priv->destroyed || errored) && !rs->end_emitted);
}

static void stream_define_readable_accessors(ant_t *js, ant_value_t target) {
  stream_define_fields(js, target, STREAM_ON_READABLE);
  js_set_getter_desc(js, target, "readableAborted", 15, Ant_Function_Create(js_readable_aborted_getter), JS_DESC_C);
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

static void stream_buffer_consume(stream_readable_state_t *rs, double n) {
  rs->length = rs->length > n ? rs->length - n : 0;
  rs->data_emitted = true;
}

static void stream_buffer_drop_head(stream_private_state_t *priv, double rest_size) {
  stream_readable_state_t *rs = &priv->readable;
  rs->chunk_head = (rs->chunk_head + 1) & (rs->chunk_cap - 1);
  rs->chunk_count--;
  rs->head_offset = 0;
  stream_buffer_consume(rs, rest_size);
}

static ant_value_t stream_buffer_shift(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (stream_readable_buffer_len(rs) == 0) return js_mkundef();

  double size = stream_rs_head_size(rs);
  ant_value_t value = stream_readable_head_rest(js, rs);
  if (is_err(value)) return value;

  stream_buffer_drop_head(priv, size);
  return value;
}

static ant_value_t stream_buffer_unshift(ant_t *js, stream_private_state_t *priv, ant_value_t value) {
  stream_readable_state_t *rs = &priv->readable;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);
  ant_value_t settled = stream_readable_settle_head(js, priv);
  GC_ROOT_RESTORE(js, root_mark);
  if (is_err(settled)) return settled;

  double size = stream_chunk_size(js, value, rs->object_mode);
  if (!stream_chunks_add(js, priv, value, size, true)) return Ant_Error_CreateOutOfMemory(js);
  rs->length += size;

  return js_mkundef();
}

static ant_value_t stream_buffer_take_bytes(ant_t *js, stream_private_state_t *priv, double n) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t head = stream_readable_head(rs);
  double head_rest = stream_rs_head_size(rs);

  if (n == head_rest) return stream_buffer_shift(js, priv);
  if (n < head_rest) {
    double start = (double)rs->head_offset;
    ant_value_t out = stream_bytes_view(js, head, start, start + n);
    if (is_err(out)) return out;

    rs->head_offset += (size_t)n;
    stream_buffer_consume(rs, n);

    return out;
  }

  ArrayBufferData *ab = create_array_buffer_data(js, (size_t)n);
  if (!ab) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t out = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, (size_t)n, js->builtins.buffer_proto);
  if (is_err(out)) return out;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);

  size_t filled = 0;
  while (filled < (size_t)n && stream_readable_buffer_len(rs) > 0) {
    ant_value_t chunk = stream_readable_head(rs);
    const uint8_t *bytes = NULL;
    size_t len = 0;

    if (!buffer_source_get_bytes(js, chunk, &bytes, &len)) {
      ant_value_t settled = stream_readable_settle_head(js, priv);
      chunk = is_err(settled) ? settled : stream_make_buffer(js, stream_readable_head(rs), js_mkundef());
      if (is_err(chunk) || !buffer_source_get_bytes(js, chunk, &bytes, &len)) {
        GC_ROOT_RESTORE(js, root_mark);
        return is_err(chunk) ? chunk : js_mkerr(js, "readable chunk is not binary");
      }
    } else {
      bytes += rs->head_offset;
      len -= rs->head_offset;
    }

    size_t take = len < (size_t)n - filled ? len : (size_t)n - filled;
    if (take > 0) memcpy(ab->data + filled, bytes, take);
    filled += take;

    if (take == len) stream_buffer_drop_head(priv, (double)len);
    else {
      rs->head_offset += take;
      stream_buffer_consume(rs, (double)take);
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t stream_buffer_take_chars(ant_t *js, stream_private_state_t *priv, double n) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t out = js_mkundef();
  double filled = 0;

  GC_ROOT_SAVE(root_mark, js);
  while (filled < n && stream_readable_buffer_len(rs) > 0) {
    GC_ROOT_RESTORE(js, root_mark);
    GC_ROOT_PIN(js, out);

    ant_value_t head = stream_readable_head(rs);
    if (vtype(head) != kTypeString) {
      ant_value_t settled = stream_readable_settle_head(js, priv);
      head = is_err(settled) ? settled : js_tostring_val(js, stream_readable_head(rs));
      if (is_err(head)) { out = head; break; }
      stream_chunks_set_head(js, priv, head, stream_chunk_size(js, head, false));
    }

    double start = (double)rs->head_offset;
    double rest = stream_rs_head_size(rs);
    double take = rest < n - filled ? rest : n - filled;
    
    ant_value_t piece = start == 0 && take == rest ? head : js_string_slice(js, head, start, start + take);
    if (is_err(piece)) { out = piece; break; }
    GC_ROOT_PIN(js, piece);

    if (take == rest) stream_buffer_drop_head(priv, rest);
    else {
      rs->head_offset += (size_t)take;
      stream_buffer_consume(rs, take);
    }

    out = is_undefined(out) ? piece : do_string_op(js, TOK_PLUS, out, piece);
    if (is_err(out)) break;
    filled += take;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return is_undefined(out) ? js_mkstr(js, "", 0) : out;
}

static ant_value_t stream_readable_redecode_buffer(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (rs->object_mode || stream_readable_buffer_len(rs) == 0) return js_mkundef();

  ant_value_t settled = stream_readable_settle_head(js, priv);
  if (is_err(settled)) return settled;

  ant_value_t content = js_mkstr(js, "", 0);
  uint32_t len = stream_readable_buffer_len(rs);

  GC_ROOT_SAVE(root_mark, js);
  for (uint32_t i = 0; i < len; i++) {
    GC_ROOT_RESTORE(js, root_mark);
    GC_ROOT_PIN(js, content);

    ant_value_t chunk = stream_chunk_at(rs, i)->value;
    ant_value_t part = vtype(chunk) == kTypeString ? chunk : stream_readable_decode_chunk(js, rs, chunk, false);
    if (is_err(part)) {
      GC_ROOT_RESTORE(js, root_mark);
      return part;
    }

    content = do_string_op(js, TOK_PLUS, content, part);
    if (is_err(content)) {
      GC_ROOT_RESTORE(js, root_mark);
      return content;
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  GC_ROOT_PIN(js, content);

  stream_chunks_clear(rs);
  rs->length = 0;

  ant_value_t pushed = stream_value_is_empty_string(js, content) ? js_mkundef() : stream_buffer_push(js, priv, content);
  GC_ROOT_RESTORE(js, root_mark);

  return pushed;
}

static double stream_grow_high_water_mark(double n) {
  if (n >= 1073741824.0) return 1073741824.0;
  double hwm = 1;
  while (hwm < n) hwm *= 2;
  return hwm;
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

static double stream_high_water_mark_from_options(
  ant_t *js, ant_value_t options,
  const char *side_key, bool object_mode
) {
  ant_value_t hwm = stream_get_option(js, options, "highWaterMark");
  if ((is_undefined(hwm) || is_null(hwm)) && side_key) hwm = stream_get_option(js, options, side_key);

  return (vtype(hwm) == kTypeNumber && js_getnum(hwm) >= 0)
    ? floor(js_getnum(hwm))
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
  priv->emit_close = stream_get_option(js, options, "emitClose") != js_false;
  
  if (is_callable(construct_fn)) {
    js_set(js, obj, "_construct", construct_fn);
    stream_begin_construct(js, obj);
  }
  
  if (is_callable(destroy_fn)) js_set(js, obj, "_destroy", destroy_fn);
  return priv;
}

static ant_value_t stream_readable_apply_encoding(ant_t *js, stream_private_state_t *priv, ant_value_t encoding);

static void stream_init_readable_side(ant_t *js, stream_private_state_t *priv, ant_value_t options, bool duplex) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t read_fn = stream_get_option(js, options, "read");
  ant_value_t encoding = stream_get_option(js, options, "encoding");
  ant_value_t default_encoding = stream_get_option(js, options, "defaultEncoding");

  priv->has_readable = true;
  rs->object_mode =
    js_truthy(js, stream_get_option(js, options, "objectMode")) ||
    (duplex && js_truthy(js, stream_get_option(js, options, "readableObjectMode")));
  rs->high_water_mark = stream_high_water_mark_from_options(
    js, options, duplex ? "readableHighWaterMark" : NULL, rs->object_mode
  );
  rs->auto_destroy = stream_auto_destroy_from_options(js, options);
  if (vtype(default_encoding) == kTypeString) stream_store(js, priv, &rs->default_encoding, default_encoding);

  if (is_callable(read_fn)) js_set(js, stream_value(priv), "_read", read_fn);
  if (vtype(encoding) == kTypeString) {
    ant_value_t applied = stream_readable_apply_encoding(js, priv, encoding);
    if (is_err(applied)) Ant_Error_ConsumeMarker(js, applied);
  }
}

static void stream_init_writable_side(ant_t *js, stream_private_state_t *priv, ant_value_t options, bool duplex) {
  stream_writable_state_t *ws = &priv->writable;
  ant_value_t write_fn = stream_get_option(js, options, "write");
  ant_value_t final_fn = stream_get_option(js, options, "final");

  priv->has_writable = true;
  ws->object_mode =
    js_truthy(js, stream_get_option(js, options, "objectMode")) ||
    (duplex && js_truthy(js, stream_get_option(js, options, "writableObjectMode")));
  ws->high_water_mark = stream_high_water_mark_from_options(
    js, options, duplex ? "writableHighWaterMark" : NULL, ws->object_mode
  );
  ws->auto_destroy = stream_auto_destroy_from_options(js, options);

  if (is_callable(write_fn)) js_set(js, stream_value(priv), "_write", write_fn);
  if (is_callable(final_fn)) js_set(js, stream_value(priv), "_final", final_fn);
}

static void stream_init_legacy(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_init_base(js, obj, options);
}

static void stream_init_readable(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_private_state_t *priv = stream_init_base(js, obj, options);
  if (priv) stream_init_readable_side(js, priv, options, false);
}

static void stream_init_writable(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_private_state_t *priv = stream_init_base(js, obj, options);
  if (!priv) return;
  priv->is_readable = false;
  stream_init_writable_side(js, priv, options, false);
}

static void stream_init_duplex(ant_t *js, ant_value_t obj, ant_value_t options) {
  stream_private_state_t *priv = stream_init_base(js, obj, options);
  if (!priv) return;
  stream_init_readable_side(js, priv, options, true);
  stream_init_writable_side(js, priv, options, true);
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

static void stream_pipe_cleanup(ant_t *js, stream_op_t *op) {
  if (op->pipe_cleaned) return;
  op->pipe_cleaned = true;

  stream_pipe_op_t *pipe = &op->pipe;
  stream_private_state_t *priv = stream_private_state(pipe->source);

  if (priv) {
    stream_remove_listener(js, pipe->source, "data", pipe->on_data);
    stream_remove_listener(js, pipe->source, "end", pipe->on_end);
    stream_remove_listener(js, pipe->source, "close", pipe->on_close);
    stream_remove_listener(js, pipe->source, "error", pipe->on_error);
    stream_pipe_remove_state(js, priv, stream_op_value(op));
  }

  if (is_object_type(pipe->dest)) stream_remove_listener(js, pipe->dest, "drain", pipe->on_drain);
}

static ant_value_t stream_writable_write_impl(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding,
  ant_value_t callback, bool allow_after_end
);
static ant_value_t js_writable_write(ant_params_t);
static ant_value_t js_readable_pause(ant_params_t);
static ant_value_t js_readable_resume(ant_params_t);

static void stream_pipe_flow(ant_t *js, stream_op_t *op, bool pause) {
  ant_value_t source = op->pipe.source;
  
  ant_value_t fn = pause
    ? js_get_cached(js, source, "pause", 5, &op->pause_cache)
    : js_get_cached(js, source, "resume", 6, &op->resume_cache);

  if (vtype(fn) == kTypeBuiltin && js_cfunc_same_entrypoint(fn, pause ? js_readable_pause : js_readable_resume) && stream_readable_of(source)) {
    if (pause) stream_readable_pause(js, source);
    else stream_readable_resume(js, source);
    return;
  }

  if (is_callable(fn)) stream_call(js, fn, source, NULL, 0);
}

static ant_value_t stream_pipe_deliver(ant_t *js, stream_op_t *op, ant_value_t *args, int nargs) {
  ant_value_t dest = op->pipe.dest;
  
  if (!is_object_type(dest)) return js_mkundef();
  ant_value_t write_fn = js_get_cached(js, dest, "write", 5, &op->write_cache);
  
  stream_private_state_t *dest_priv = vtype(write_fn) == kTypeBuiltin && 
    js_cfunc_same_entrypoint(write_fn, js_writable_write)
    ? stream_writable_of(dest) : NULL;
  
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();

  ant_value_t result = dest_priv
    ? stream_writable_write_impl(js, dest_priv, chunk, js->builtins.stream_utf8, js_mkundef(), false)
    : is_err(write_fn) ? write_fn
    : !is_callable(write_fn) ? js_mkundef()
    : stream_call(js, write_fn, dest, nargs > 0 ? &args[0] : NULL, nargs > 0 ? 1 : 0);
  
  if (is_err(result) || result != js_false) return is_err(result) ? result : js_mkundef();
  bool need_drain = dest_priv ? dest_priv->writable.need_drain : js_get(js, op->pipe.dest, "writableNeedDrain") != js_false;
  if (need_drain || stream_is_destroyed(js, op->pipe.dest)) stream_pipe_flow(js, op, true);

  return js_mkundef();
}

static ant_value_t stream_pipe_on_data(ant_params_t) {
  return stream_pipe_deliver(js, stream_op_current(js), args, nargs);
}

static void stream_emit_data(ant_t *js, stream_private_state_t *priv, ant_value_t chunk) {
  ant_value_t stream_obj = stream_value(priv);
  ant_value_t sole = vtype(priv->pipes) == kTypeArray ? eventemitter_sole_listener_ref(stream_obj, &priv->data_event) : 0;

  // TODO: reduce nesting
  if (sole) {
    ant_offset_t len = js_arr_len(js, priv->pipes);
    for (ant_offset_t i = 0; i < len; i++) {
      stream_op_t *op = stream_op(js_arr_get(js, priv->pipes, i));
      if (!op || op->pipe.on_data != sole) continue;
      if (sv_check_c_stack_overflow(js)) {
        Ant_Exception_Set(js, Ant_Error_CreateStackOverflow(js));
        return;
      }
      Ant_Silver_FinishNativeCall(js, stream_pipe_deliver(js, op, &chunk, 1));
      return;
    }
  }

  eventemitter_emit_ref(js, stream_obj, &priv->data_event, &chunk, 1);
}

static ant_value_t stream_pipe_on_drain(ant_params_t) {
  stream_pipe_flow(js, stream_op_current(js), false);
  return js_mkundef();
}

static ant_value_t stream_pipe_on_end(ant_params_t) {
  stream_op_t *op = stream_op_current(js);

  stream_pipe_cleanup(js, op);
  if (op->pipe_end_dest) stream_call_prop(js, op->pipe.dest, "end", NULL, 0);

  return js_mkundef();
}

static ant_value_t stream_pipe_on_close(ant_params_t) {
  stream_pipe_cleanup(js, stream_op_current(js));
  return js_mkundef();
}

static ant_value_t stream_pipe_on_error(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t dest = op->pipe.dest;

  stream_pipe_cleanup(js, op);
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
  ant_value_t state;
  stream_op_t *op = stream_op_new(js, &state);
  if (!op) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t on_data = js_heavy_mkfun(js, stream_pipe_on_data, state);
  ant_value_t on_drain = js_heavy_mkfun(js, stream_pipe_on_drain, state);
  ant_value_t on_end = js_heavy_mkfun(js, stream_pipe_on_end, state);
  ant_value_t on_close = js_heavy_mkfun(js, stream_pipe_on_close, state);
  ant_value_t on_error = js_heavy_mkfun(js, stream_pipe_on_error, state);

  op->pipe_end_dest = end_dest;
  op->pipe.source = source;
  op->pipe.dest = dest;
  op->pipe.on_data = on_data;
  op->pipe.on_drain = on_drain;
  op->pipe.on_end = on_end;
  op->pipe.on_close = on_close;
  op->pipe.on_error = on_error;
  
  stream_op_barrier(js, op);
  stream_private_state_t *priv = stream_private_state(source);
  
  if (vtype(priv->pipes) != kTypeArray) stream_store(js, priv, &priv->pipes, js_mkarr(js));
  js_arr_push(js, priv->pipes, state);

  stream_add_listener(js, source, "data", on_data, false);
  stream_add_listener(js, source, "end", on_end, true);
  stream_add_listener(js, source, "close", on_close, true);
  stream_add_listener(js, source, "error", on_error, false);
  stream_add_listener(js, dest, "drain", on_drain, false);
  eventemitter_emit_args(js, dest, "pipe", &source, 1);

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
    if (!is_object_type(dest) || stream_op(state)->pipe.dest == dest) js_arr_push(js, matches, state);
  }

  len = js_arr_len(js, matches);
  for (ant_offset_t i = 0; i < len; i++) stream_pipe_cleanup(js, stream_op(js_arr_get(js, matches, i)));

  return source;
}

static ant_value_t stream_destroy_emit_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t destroyed_err = nargs > 1 ? args[1] : js_mkundef();
  stream_private_state_t *priv = stream_private_state(stream_obj);
  bool failed = !is_null(destroyed_err) && !is_undefined(destroyed_err);

  if (!priv) {
    stream_emit_named(js, stream_obj, "close");
    return js_mkundef();
  }

  if (failed && !priv->error_emitted) {
    priv->error_emitted = true;
    stream_emit_error(js, priv, destroyed_err);
  }

  priv->close_emitted = true;
  if (priv->emit_close) stream_emit_named(js, stream_obj, "close");

  return js_mkundef();
}

static ant_value_t stream_destroy_done(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  if (op->called) return js_mkundef();

  op->called = true;
  stream_private_state_t *priv = stream_private_state(op->destroy.stream);
  if (priv) priv->closed = true;
  stream_next_tick(js, stream_destroy_emit_tick, 2, op->destroy.stream, nargs > 0 ? args[0] : js_mkundef());

  return js_mkundef();
}

static ant_value_t stream_once_call(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  if (op->called) return js_mkundef();

  op->called = true;
  return stream_call(js, op->once.fn, op->once.this_val, args, nargs);
}

static ant_value_t stream_make_once(ant_t *js, ant_value_t fn, ant_value_t this_val) {
  if (is_err(fn)) return fn;

  ant_value_t state;
  stream_op_t *op = stream_op_new(js, &state);
  if (!op) return Ant_Error_CreateOutOfMemory(js);

  op->once.fn = fn;
  op->once.this_val = this_val;
  stream_op_barrier(js, op);
  return js_heavy_mkfun(js, stream_once_call, state);
}

static ant_value_t stream_make_call(ant_t *js, ant_value_t (*fn)(ant_params_t), ant_value_t stream_obj, ant_value_t callback) {
  ant_value_t state;
  stream_op_t *op = stream_op_new(js, &state);
  if (!op) return Ant_Error_CreateOutOfMemory(js);

  op->call.stream = stream_obj;
  op->call.callback = callback;
  stream_op_barrier(js, op);
  return js_heavy_mkfun(js, fn, state);
}

static ant_value_t js_stream__destroy(ant_params_t) {
  ant_value_t error = nargs > 0 ? args[0] : js_mknull();
  stream_call_callback(js, nargs > 1 ? args[1] : js_mkundef(), &error, 1);
  return js_mkundef();
  }

static ant_value_t stream_run_destroy(ant_t *js, stream_private_state_t *priv, ant_value_t error) {
  ant_value_t stream_obj = stream_value(priv);
  ant_value_t destroy_fn = stream_method(js, stream_obj, STREAM_METHOD_DESTROY);
  ant_value_t destroy_error = is_undefined(error) ? js_mknull() : error;
  
  if (!is_callable(destroy_fn) || destroy_fn == js->builtins.stream_default_destroy) {
    priv->closed = true;
    stream_next_tick(js, stream_destroy_emit_tick, 2, stream_obj, destroy_error);
  return stream_obj;
  }

  ant_value_t done_state;
  stream_op_t *op = stream_op_new(js, &done_state);
  if (!op) return Ant_Error_CreateOutOfMemory(js);

  op->destroy.stream = stream_obj;
  stream_op_barrier(js, op);

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

static inline bool stream_rs_flowing(stream_readable_state_t *rs) {
  return rs->flowing == STREAM_TRI_TRUE;
}

static inline bool stream_rs_errored(stream_readable_state_t *rs) {
  return !is_undefined(rs->errored) && !is_null(rs->errored);
}

static ant_value_t stream_error_or_destroy(ant_t *js, stream_private_state_t *priv, ant_value_t error) {
  if (priv->destroyed) return js_mkundef();

  bool auto_destroy =
    (priv->has_readable && priv->readable.auto_destroy) ||
    (priv->has_writable && priv->writable.auto_destroy);

  if (auto_destroy) return stream_destroy(js, stream_value(priv), error);
  if (priv->has_readable && !stream_rs_errored(&priv->readable)) stream_store(js, priv, &priv->readable.errored, error);
  if (priv->has_writable && is_null(priv->writable.errored)) stream_store(js, priv, &priv->writable.errored, error);
  if (priv->error_emitted) return js_mkundef();

  priv->error_emitted = true;
  eventemitter_emit_args(js, stream_value(priv), "error", &error, 1);

  return js_mkundef();
}

static double stream_rs_how_much(ant_t *js, stream_readable_state_t *rs, double n) {
  if (n <= 0 || (rs->length == 0 && rs->ended)) return 0;
  if (rs->object_mode) return 1;

  if (isnan(n)) {
    if (stream_rs_flowing(rs) && rs->length > 0) return stream_rs_head_size(rs);
    return rs->length;
  }

  if (n <= rs->length) return n;
  return rs->ended ? rs->length : 0;
}

static ant_value_t stream_rs_from_list(ant_t *js, stream_private_state_t *priv, double n) {
  stream_readable_state_t *rs = &priv->readable;

  if (rs->length <= 0 || stream_readable_buffer_len(rs) == 0) return js_mknull();
  if (rs->object_mode) return stream_buffer_shift(js, priv);

  if (isnan(n) || n <= 0 || n >= rs->length) {
    if (stream_readable_buffer_len(rs) == 1) return stream_buffer_shift(js, priv);
    n = rs->length;
  }

  return is_object_type(rs->decoder)
    ? stream_buffer_take_chars(js, priv, n)
    : stream_buffer_take_bytes(js, priv, n);
}

static ant_value_t stream_rs_read(ant_t *js, stream_private_state_t *priv, double n);
static ant_value_t stream_readable_from_read_state(ant_t *js, ant_value_t state);
static ant_value_t stream_readable_emit_readable(ant_params_t);
static ant_value_t stream_readable_emit_end(ant_params_t);
static ant_value_t stream_readable_read_more_tick(ant_params_t);
static ant_value_t stream_readable_update_listening_tick(ant_params_t);

static void stream_rs_emit_readable(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  rs->need_readable = false;
  if (rs->emitted_readable) return;

  rs->emitted_readable = true;
  stream_next_tick(js, stream_readable_emit_readable, 1, stream_value(priv));
}

static void stream_rs_end(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (rs->end_emitted || rs->end_scheduled) return;

  rs->ended = true;
  rs->end_scheduled = true;
  stream_next_tick(js, stream_readable_emit_end, 1, stream_value(priv));
}

static void stream_rs_maybe_read_more(ant_t *js, stream_private_state_t *priv) {
  if (priv->readable.reading_more || priv->constructing) return;
  priv->readable.reading_more = true;
  stream_next_tick(js, stream_readable_read_more_tick, 1, stream_value(priv));
}

static ant_value_t stream_rs_flow(ant_t *js, stream_private_state_t *priv) {
  while (stream_rs_flowing(&priv->readable)) {
    ant_value_t chunk = stream_rs_read(js, priv, (double)NAN);
    if (is_err(chunk)) return chunk;
    if (is_null(chunk)) break;
  }

  return js_mkundef();
}

static ant_value_t stream_rs_emit_readable_now(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t stream_obj = stream_value(priv);

  if (!priv->destroyed && !stream_rs_errored(rs) && (rs->length > 0 || rs->ended)) {
    ant_offset_t before = 0, after = 0;
    eventemitter_emit_ref_counted(js, stream_obj, &priv->readable_event, NULL, 0, &before, &after);
    rs->emitted_readable = false;
    if (after < before) stream_next_tick(js, stream_readable_update_listening_tick, 1, stream_obj);
  }

  rs->need_readable = !stream_rs_flowing(rs) && !rs->ended && rs->length <= rs->high_water_mark;
  return stream_rs_flow(js, priv);
}

static ant_value_t stream_readable_emit_readable(ant_params_t) {
  stream_private_state_t *priv = stream_readable_of(nargs > 0 ? args[0] : js_mkundef());
  return priv ? stream_rs_emit_readable_now(js, priv) : js_mkundef();
}

static ant_value_t stream_rs_read(ant_t *js, stream_private_state_t *priv, double n) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t stream_obj = stream_value(priv);
  double n_orig = n;

  if (n > rs->high_water_mark) {
    if (n > 1073741824.0) return Ant_Error_CreateOutOfRange(js, "size", "<= 1GiB", js_mknum(n));
    rs->high_water_mark = stream_grow_high_water_mark(n);
  }

  if (n != 0) rs->emitted_readable = false;

  if (
    n == 0 && rs->need_readable &&
    ((rs->high_water_mark != 0 ? rs->length >= rs->high_water_mark : rs->length > 0) || rs->ended)
  ) {
    if (rs->length == 0 && rs->ended) stream_rs_end(js, priv);
    else stream_rs_emit_readable(js, priv);
    return js_mknull();
  }

  n = stream_rs_how_much(js, rs, n);
  if (n == 0 && rs->ended) {
    if (rs->length == 0) stream_rs_end(js, priv);
    return js_mknull();
  }

  bool do_read = rs->need_readable || rs->length == 0 || rs->length - n < rs->high_water_mark;

  if (rs->ended || rs->reading || priv->destroyed || stream_rs_errored(rs) || priv->constructing) do_read = false;
  else if (do_read) {
    rs->reading = true;
    rs->sync = true;
    if (rs->length == 0) rs->need_readable = true;

    ant_value_t read_fn = stream_method(js, stream_obj, STREAM_METHOD_READ);
    ant_value_t read_args[1] = { js_mknum(rs->high_water_mark) };
    bool prev_in_read = rs->in_read;

    rs->in_read = true;
    ant_value_t result = is_err(read_fn) ? read_fn
      : read_fn == priv->from_read ? stream_readable_from_read_state(js, js_get_slot(read_fn, SLOT_DATA))
      : stream_call(js, read_fn, stream_obj, read_args, 1);
    rs->in_read = prev_in_read;
    rs->sync = false;

    if (is_err(result)) {
      ant_value_t destroyed = stream_error_or_destroy(js, priv, Ant_Error_ConsumeMarker(js, result));
      if (is_err(destroyed)) return destroyed;
    }

    if (!rs->reading) n = stream_rs_how_much(js, rs, n_orig);
  }

  ant_value_t ret = n > 0 ? stream_rs_from_list(js, priv, n) : js_mknull();
  if (is_err(ret)) return ret;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ret);

  if (is_null(ret)) {
    rs->need_readable = rs->length <= rs->high_water_mark;
    n = 0;
  }

  if (rs->length == 0) {
    if (!rs->ended) rs->need_readable = true;
    if (n_orig != n && rs->ended) stream_rs_end(js, priv);
  }

  if (!is_null(ret) && !priv->error_emitted && !priv->close_emitted) {
    rs->data_emitted = true;
    stream_emit_data(js, priv, ret);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return ret;
}

static ant_value_t stream_readable_read_more_tick(ant_params_t) {
  stream_private_state_t *priv = stream_readable_of(nargs > 0 ? args[0] : js_mkundef());
  if (!priv) return js_mkundef();

  stream_readable_state_t *rs = &priv->readable;
  while (
    !rs->reading && !rs->ended &&
    (rs->length < rs->high_water_mark || (stream_rs_flowing(rs) && rs->length == 0))
  ) {
    double length = rs->length;
    ant_value_t result = stream_rs_read(js, priv, 0);
    if (is_err(result)) {
      rs->reading_more = false;
      return result;
    }
    if (length == rs->length) break;
  }

  rs->reading_more = false;
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

  if (stream_rs_errored(rs) || priv->close_emitted || rs->end_emitted || rs->length > 0) return js_mkundef();

  rs->end_emitted = true;
  priv->is_readable = false;
  stream_emit_named(js, stream_obj, "end");
  if (priv->end_hook) priv->end_hook(js, stream_obj);

  bool writable_done = !priv->has_writable || priv->writable.finished;
  if (writable_done && stream_auto_destroy(priv))
    sv_call_native_this(js, js_stream_destroy, stream_obj, NULL, 0);
  else if (!writable_done && js_get(js, stream_obj, "allowHalfOpen") == js_false)
    stream_next_tick(js, stream_half_open_end_tick, 1, stream_obj);

  return js_mkundef();
}

static ant_value_t stream_rs_add_chunk(ant_t *js, stream_private_state_t *priv, ant_value_t chunk, double size, bool add_to_front) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t stream_obj = stream_value(priv);

  if (
    stream_rs_flowing(rs) && rs->length == 0 && !rs->sync &&
    eventemitter_listener_count_ref(stream_obj, &priv->data_event) > 0
  ) {
    rs->data_emitted = true;
    stream_emit_data(js, priv, chunk);
  } else {
    ant_value_t added = add_to_front ? stream_buffer_unshift(js, priv, chunk) : stream_buffer_push_sized(js, priv, chunk, size);
    if (is_err(added)) return added;
    if (rs->need_readable) stream_rs_emit_readable(js, priv);
  }

  stream_rs_maybe_read_more(js, priv);
  return js_mkundef();
}

static ant_value_t stream_rs_on_eof(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;
  if (rs->ended) return js_mkundef();

  if (is_object_type(rs->decoder)) {
    ant_value_t tail = stream_readable_decode_chunk(js, rs, js_mkundef(), true);
    if (is_err(tail)) return tail;
    if (!is_undefined(tail) && !stream_value_is_empty_string(js, tail)) {
      ant_value_t pushed = stream_buffer_push(js, priv, tail);
      if (is_err(pushed)) return pushed;
    }
  }

  rs->ended = true;
  if (rs->sync) {
    stream_rs_emit_readable(js, priv);
    return js_mkundef();
  }

  rs->need_readable = false;
  rs->emitted_readable = true;
  return stream_rs_emit_readable_now(js, priv);
}

static bool stream_encoding_is_utf8(const char *s, size_t len) {
  return (len == 4 && strncasecmp(s, "utf8", 4) == 0) || (len == 5 && strncasecmp(s, "utf-8", 5) == 0);
}

static bool stream_encoding_matches(ant_t *js, ant_value_t a, ant_value_t b) {
  size_t alen = 0, blen = 0;
  const char *as = vtype(a) == kTypeString ? js_getstr(js, a, &alen) : NULL;
  const char *bs = vtype(b) == kTypeString ? js_getstr(js, b, &blen) : NULL;

  if (!as || !bs) return false;
  if (alen == blen && strncasecmp(as, bs, alen) == 0) return true;
  return stream_encoding_is_utf8(as, alen) && stream_encoding_is_utf8(bs, blen);
}

static ant_value_t stream_view_to_buffer(ant_t *js, ant_value_t chunk) {
  if (vtype(chunk) == kTypeObject && js_obj_ptr(chunk)->proto == js->builtins.buffer_proto) return chunk;
  if (buffer_get_typedarray_data(chunk)) return buffer_byte_subarray(js, chunk, 0, SIZE_MAX);
  return stream_make_buffer(js, chunk, js_mkundef());
}

static ant_value_t stream_rs_add(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding, bool add_to_front
) {
  stream_readable_state_t *rs = &priv->readable;
  ant_value_t result = js_mkundef();
  bool raw_string = false;
  bool refused = false;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, chunk);
  GC_ROOT_PIN(js, encoding);

  if (!rs->object_mode && !is_null(chunk) && !is_undefined(chunk)) {
    if (vtype(chunk) == kTypeString) {
      if (is_undefined(encoding) || is_null(encoding))
        encoding = is_undefined(rs->default_encoding) ? js->builtins.stream_utf8 : rs->default_encoding;

      if (stream_encoding_matches(js, rs->encoding, encoding)) raw_string = true;
      else {
        chunk = stream_make_buffer(js, chunk, encoding);
        if (is_err(chunk)) { result = chunk; goto done; }
        if (add_to_front && vtype(rs->encoding) == kTypeString) {
          chunk = stream_call_prop(js, chunk, "toString", &rs->encoding, 1);
          if (is_err(chunk)) { result = chunk; goto done; }
          raw_string = true;
        }
      }
    } else if (vtype(chunk) == kTypeTypedArray || buffer_is_binary_source(chunk)) {
      chunk = stream_view_to_buffer(js, chunk);
      if (is_err(chunk)) { result = chunk; goto done; }
    } else {
      ant_value_t err = Ant_Error_ConsumeMarker(js, Ant_Error_CreateInvalidArgType(
        js, "chunk", "of type string or an instance of Buffer, TypedArray, or DataView", chunk
      ));
      result = stream_error_or_destroy(js, priv, err);
      goto done;
    }
  }

  if (is_null(chunk)) {
    rs->reading = false;
    result = stream_rs_on_eof(js, priv);
    goto done;
  }

  double size = rs->object_mode ? 1 : is_undefined(chunk) ? 0 : stream_chunk_size(js, chunk, false);
  if (!rs->object_mode && size <= 0) {
    if (!add_to_front) {
      rs->reading = false;
      stream_rs_maybe_read_more(js, priv);
    }
    goto done;
  }

  if (add_to_front) {
    if (rs->end_emitted) {
      result = stream_error_or_destroy(js, priv, stream_coded_error(
        js, "ERR_STREAM_UNSHIFT_AFTER_END_EVENT", "stream.unshift() after end event"
      ));
      goto done;
    }

    if (priv->destroyed || stream_rs_errored(rs)) { refused = true; goto done; }
    if (!rs->object_mode && is_object_type(rs->decoder) && vtype(chunk) != kTypeString) {
      chunk = stream_call_prop(js, chunk, "toString", &rs->encoding, 1);
      if (is_err(chunk)) { result = chunk; goto done; }
    }

    result = stream_rs_add_chunk(js, priv, chunk, size, true);
    goto done;
  }

  if (rs->ended) {
    result = stream_error_or_destroy(js, priv, stream_coded_error(
      js, "ERR_STREAM_PUSH_AFTER_EOF", "stream.push() after EOF"
    ));
    goto done;
  }

  if (priv->destroyed || stream_rs_errored(rs)) { refused = true; goto done; }
  rs->reading = false;

  if (is_object_type(rs->decoder) && !raw_string) {
    chunk = stream_readable_decode_chunk(js, rs, chunk, false);
    if (is_err(chunk)) { result = chunk; goto done; }
    if (!rs->object_mode && stream_value_is_empty_string(js, chunk)) {
      stream_rs_maybe_read_more(js, priv);
      goto done;
    }
    size = stream_chunk_size(js, chunk, rs->object_mode);
  }

  result = stream_rs_add_chunk(js, priv, chunk, size, false);

done:
  GC_ROOT_RESTORE(js, root_mark);
  if (is_err(result)) return result;
  if (refused) return js_false;
  return js_bool(!rs->ended && (rs->length < rs->high_water_mark || rs->length == 0));
}

ant_value_t stream_readable_push_value(
  ant_t *js,
  ant_value_t stream_obj,
  ant_value_t chunk,
  ant_value_t encoding
) {
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  return priv ? stream_rs_add(js, priv, chunk, encoding, false) : js_false;
}

static ant_value_t js_readable_push(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t encoding = nargs > 1 ? args[1] : js_mkundef();
  if (is_err(stream_obj)) return stream_obj;
  return stream_readable_push_value(js, stream_obj, chunk, encoding);
}

static ant_value_t js_readable_unshift(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  if (is_err(stream_obj)) return stream_obj;

  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return js_false;

  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t encoding = nargs > 1 ? args[1] : js_mkundef();
  return stream_rs_add(js, priv, chunk, encoding, true);
}

static ant_value_t js_readable_read(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  if (is_err(stream_obj)) return stream_obj;

  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return js_mknull();

  double n = (double)NAN;
  if (nargs > 0 && !is_undefined(args[0])) {
    n = vtype(args[0]) == kTypeNumber ? tod(args[0]) : js_to_number(js, args[0]);
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    if (!isnan(n)) n = trunc(n);
  }

  return stream_rs_read(js, priv, n);
}

static ant_value_t stream_readable_apply_encoding(ant_t *js, stream_private_state_t *priv, ant_value_t encoding) {
  ant_value_t stream_obj = stream_value(priv);
  ant_value_t decoder = string_decoder_create(js, encoding, js_mkundef());
  if (is_err(decoder)) return decoder;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, decoder);

  ant_value_t normalized = js_get(js, decoder, "encoding");
  if (vtype(normalized) != kTypeString) normalized = js_tostring_val(js, encoding);
  if (is_err(normalized)) {
    GC_ROOT_RESTORE(js, root_mark);
    return normalized;
  }

  stream_store(js, priv, &priv->readable.decoder, decoder);
  stream_store(js, priv, &priv->readable.encoding, normalized);
  GC_ROOT_RESTORE(js, root_mark);

  ant_value_t redecoded = stream_readable_redecode_buffer(js, priv);
  if (is_err(redecoded)) return redecoded;

  return stream_obj;
}

static ant_value_t js_readable_set_encoding(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  ant_value_t encoding = nargs > 0 && !is_undefined(args[0]) ? args[0] : js->builtins.stream_utf8;

  if (is_err(stream_obj)) return stream_obj;
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return stream_obj;

  ant_value_t applied = stream_readable_apply_encoding(js, priv, encoding);
  return is_err(applied) ? applied : stream_obj;
}

static ant_value_t stream_readable_resume_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return js_mkundef();

  stream_readable_state_t *rs = &priv->readable;
  if (!rs->reading) {
    ant_value_t result = stream_rs_read(js, priv, 0);
    if (is_err(result)) return result;
  }

  rs->resume_scheduled = false;
  stream_emit_named(js, stream_obj, "resume");

  ant_value_t flowed = stream_rs_flow(js, priv);
  if (is_err(flowed)) return flowed;
  if (stream_rs_flowing(rs) && !rs->reading) return stream_rs_read(js, priv, 0);

  return js_mkundef();
}

static void stream_rs_resume(ant_t *js, stream_private_state_t *priv) {
  stream_readable_state_t *rs = &priv->readable;

  if (!stream_rs_flowing(rs)) {
    rs->flowing = rs->readable_listening ? STREAM_TRI_FALSE : STREAM_TRI_TRUE;
    if (!rs->resume_scheduled) {
      rs->resume_scheduled = true;
      stream_next_tick(js, stream_readable_resume_tick, 1, stream_value(priv));
    }
  }

  rs->paused = STREAM_TRI_FALSE;
  priv->paused = false;
}

static ant_value_t stream_readable_update_listening_tick(ant_params_t) {
  ant_value_t stream_obj = nargs > 0 ? args[0] : js_mkundef();
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return js_mkundef();

  stream_readable_state_t *rs = &priv->readable;
  rs->readable_listening = eventemitter_listener_count(js, stream_obj, "readable") > 0;

  if (rs->resume_scheduled && rs->paused == STREAM_TRI_FALSE) rs->flowing = STREAM_TRI_TRUE;
  else if (eventemitter_listener_count(js, stream_obj, "data") > 0) stream_rs_resume(js, priv);
  else if (!rs->readable_listening) rs->flowing = STREAM_TRI_NULL;

  return js_mkundef();
}

static ant_value_t stream_readable_read_zero_tick(ant_params_t) {
  stream_private_state_t *priv = stream_readable_of(nargs > 0 ? args[0] : js_mkundef());
  return priv ? stream_rs_read(js, priv, 0) : js_mkundef();
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

  if (stream_key_is_cstr(js, key, "data")) {
    rs->readable_listening = eventemitter_listener_count(js, stream_obj, "readable") > 0;
    if (rs->flowing != STREAM_TRI_FALSE) stream_rs_resume(js, priv);
  } else if (stream_key_is_cstr(js, key, "readable") && !rs->end_emitted && !rs->readable_listening) {
    rs->readable_listening = true;
    rs->need_readable = true;
    rs->flowing = STREAM_TRI_FALSE;
    rs->emitted_readable = false;
    if (rs->length > 0) stream_rs_emit_readable(js, priv);
    else if (!rs->reading) stream_next_tick(js, stream_readable_read_zero_tick, 1, stream_obj);
  }

  return stream_obj;
}

static ant_value_t js_readable_on(ant_params_t) {
  return js_readable_add(js, args, nargs, false);
}

static ant_value_t js_readable_once(ant_params_t) {
  return js_readable_add(js, args, nargs, true);
}

static ant_value_t stream_readable_remove(ant_native_params_t, const char *method, bool all) {
  ant_value_t stream_obj = js_getthis(js);
  ant_value_t remove = js_get(js, eventemitter_prototype(js), method);
  ant_value_t result = stream_call(js, remove, stream_obj, args, nargs);
  if (is_err(result)) return result;

  ant_value_t key = nargs > 0 ? args[0] : js_mkundef();
  if (stream_readable_of(stream_obj) && ((all && is_undefined(key)) || stream_key_is_cstr(js, key, "readable")))
    stream_next_tick(js, stream_readable_update_listening_tick, 1, stream_obj);

  return result;
}

static ant_value_t js_readable_remove_listener(ant_params_t) {
  return stream_readable_remove(js, args, nargs, "removeListener", false);
}

static ant_value_t js_readable_remove_all_listeners(ant_params_t) {
  return stream_readable_remove(js, args, nargs, "removeAllListeners", true);
}

ant_value_t stream_readable_resume(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return stream_obj;
  if (!priv->has_readable) return stream_set_paused(js, priv, false);

  stream_rs_resume(js, priv);
  return stream_obj;
}

ant_value_t stream_readable_pause(ant_t *js, ant_value_t stream_obj) {
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv) return stream_obj;
  if (!priv->has_readable) return stream_set_paused(js, priv, true);

  stream_readable_state_t *rs = &priv->readable;
  if (rs->flowing != STREAM_TRI_FALSE) {
    rs->flowing = STREAM_TRI_FALSE;
    stream_emit_named(js, stream_obj, "pause");
  }

  rs->paused = STREAM_TRI_TRUE;
  priv->paused = true;

  return stream_obj;
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

static ant_value_t js_readable_is_paused(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Readable");
  if (is_err(stream_obj)) return stream_obj;

  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv->has_readable) return js_bool(priv->paused);
  return js_bool(priv->readable.paused == STREAM_TRI_TRUE || priv->readable.flowing == STREAM_TRI_FALSE);
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

static bool stream_writable_has_buffered(stream_writable_state_t *ws) {
  return ws->queue_head < ws->queue_len;
}

static bool stream_writable_enqueue(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding,
  ant_value_t callback, double chunk_size
) {
  stream_writable_state_t *ws = &priv->writable;

  if (ws->queue_len == ws->queue_cap && ws->queue_head > 0) {
    ws->queue_len -= ws->queue_head;
    memmove(ws->queue, ws->queue + ws->queue_head, ws->queue_len * sizeof(*ws->queue));
    ws->queue_head = 0;
  }

  if (ws->queue_len == ws->queue_cap) {
    uint32_t cap = ws->queue_cap ? ws->queue_cap * 2 : 8;
    stream_write_entry_t *queue = realloc(ws->queue, cap * sizeof(*queue));
    if (!queue) return false;
    ws->queue = queue;
    ws->queue_cap = cap;
  }

  ws->queue[ws->queue_len++] = (stream_write_entry_t){
    .chunk = chunk, .encoding = encoding,
    .callback = callback, .size = chunk_size,
  };

  gc_write_barrier(js, priv->obj, chunk);
  gc_write_barrier(js, priv->obj, encoding);
  gc_write_barrier(js, priv->obj, callback);
  
  return true;
}

static bool stream_writable_dequeue(stream_writable_state_t *ws, stream_write_entry_t *entry) {
  if (!stream_writable_has_buffered(ws)) return false;

  *entry = ws->queue[ws->queue_head++];
  if (ws->queue_head == ws->queue_len) ws->queue_head = ws->queue_len = 0;

  return true;
}

static void stream_writable_fail_buffered(ant_t *js, stream_private_state_t *priv, ant_value_t err) {
  stream_write_entry_t entry;

  while (stream_writable_dequeue(&priv->writable, &entry))
    if (is_callable(entry.callback)) stream_call_callback(js, entry.callback, &err, 1);

  stream_writable_set_length(&priv->writable, 0);
  priv->writable.need_drain = false;
}

static ant_value_t stream_writable_emit_drain(ant_params_t) {
  stream_emit_named(js, nargs > 0 ? args[0] : js_mkundef(), "drain");
  return js_mkundef();
}

static ant_value_t stream_write_state_error(ant_t *js, bool ending) {
  return ending
    ? stream_coded_error(js, "ERR_STREAM_WRITE_AFTER_END", "write after end")
    : stream_coded_error(js, "ERR_STREAM_DESTROYED", "Cannot call write after a stream was destroyed");
}

static ant_value_t stream_writable_drain(ant_t *js, stream_private_state_t *priv) {
  stream_writable_state_t *ws = &priv->writable;
  ant_value_t stream_obj = stream_value(priv);
  stream_write_entry_t entry;

  if (ws->draining) return js_mkundef();
  ws->draining = true;

  while (!priv->destroyed && stream_writable_dequeue(ws, &entry)) {
    stream_write_status_t status = stream_writable_start(
      js, priv, entry.chunk, entry.encoding, entry.callback, entry.size
    );

    if (status == STREAM_WRITE_COMPLETE) continue;
    if (status == STREAM_WRITE_THROWN) {
      ws->draining = false;
      return Ant_Exception_Current(js);
    }

    break;
  }

  ws->draining = false;

  if (!ws->writing && !stream_writable_has_buffered(ws) && ws->length <= 0 && ws->need_drain) {
    ws->need_drain = false;
    if (ws->sync) stream_next_tick(js, stream_writable_emit_drain, 1, stream_obj);
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

static ant_value_t stream_writable_after_write(ant_t *js, stream_private_state_t *priv, ant_value_t err) {
  if (!priv->writable.write_pending) return js_mkundef();

  ant_value_t stream_obj = stream_value(priv);
  stream_writable_state_t *ws = &priv->writable;
  ant_value_t callback = ws->write_callback;

  ws->write_pending = false;
  ws->write_done_seq = ws->write_seq;
  stream_store(js, priv, &ws->write_callback, js_mkundef());
  stream_writable_set_length(ws, ws->length - ws->write_size);
  if (!stream_writable_has_buffered(ws)) ws->writing = false;

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

  if (priv->destroyed && stream_writable_has_buffered(ws)) {
    stream_writable_fail_buffered(js, priv, stream_write_state_error(js, false));
    return js_mkundef();
  }

  return stream_writable_drain(js, priv);
}

static ant_value_t stream_writable_onwrite(ant_params_t) {
  stream_private_state_t *priv = stream_writable_of(js_func_data(js->current_func));
  return priv ? stream_writable_after_write(js, priv, nargs > 0 ? args[0] : js_mkundef()) : js_mkundef();
}

static ant_value_t stream_transform_write(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding, ant_value_t callback
);
static ant_value_t js_transform__write(ant_params_t);

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

  ant_value_t write_fn = stream_method(js, stream_obj, STREAM_METHOD_WRITE);
  ant_value_t write_args[3] = { chunk, encoding, ws->onwrite };

  bool prev_sync = ws->sync;
  ws->sync = true;

  if (vtype(write_fn) == kTypeBuiltin && js_cfunc_same_entrypoint(write_fn, js_transform__write)) {
    ant_value_t result = stream_transform_write(js, priv, chunk, encoding, ws->onwrite);
    if (is_err(result)) {
      ws->sync = prev_sync;
      return STREAM_WRITE_THROWN;
    }
  } else if (is_callable(write_fn)) {
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
    if (is_callable(callback)) stream_next_tick(js, stream_errored_write_cb_tick, 2, callback, ws->errored);
    return js_false;
  }

  bool ending = !allow_after_end && ws->ended;
  if (ending || priv->destroyed) {
    ant_value_t err = stream_write_state_error(js, ending);
    stream_set_errored(js, priv, err);
    
    if (is_callable(callback)) stream_next_tick(js, stream_errored_write_cb_tick, 2, callback, err);
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

  if (ws->writing || priv->constructing) {
    if (!stream_writable_enqueue(js, priv, normalized, encoding, callback, chunk_size))
      return Ant_Error_CreateOutOfMemory(js);
  } else if (stream_writable_start(js, priv, normalized, encoding, callback, chunk_size) == STREAM_WRITE_THROWN)
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
  stream_op_t *op = stream_op_current(js);
  if (op->called) return js_mkundef();

  op->called = true;
  ant_value_t stream_obj = op->call.stream;
  ant_value_t callback = op->call.callback;
  ant_value_t err = nargs > 0 ? args[0] : js_mkundef();

  if (!is_undefined(err) && !is_null(err)) {
    ant_value_t destroy_args[1] = { err };
    sv_call_native_this(js, js_stream_destroy, stream_obj, destroy_args, 1);
    if (is_callable(callback)) stream_call_callback(js, callback, &err, 1);
    return js_mkundef();
  }

  stream_next_tick(js, stream_writable_finish_tick, 2, stream_obj, callback);
  return js_mkundef();
}

static ant_value_t stream_ws_begin_end(ant_t *js, stream_private_state_t *priv, ant_value_t callback) {
  stream_writable_state_t *ws = &priv->writable;
  ant_value_t stream_obj = stream_value(priv);
  ant_value_t done = stream_make_call(js, stream_writable_end_done, stream_obj, callback);
  if (is_err(done)) return done;

  ws->final_started = true;
  ws->pending_final = false;
  stream_store(js, priv, &ws->end_callback, js_mkundef());
  
  ant_value_t final_fn = stream_method(js, stream_obj, STREAM_METHOD_FINAL);
  if (is_callable(final_fn)) stream_call(js, final_fn, stream_obj, &done, 1);
  else stream_call_callback(js, done, NULL, 0);

  return stream_obj;
}

ant_value_t stream_writable_begin_end(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  stream_private_state_t *priv = stream_writable_of(stream_obj);
  return priv ? stream_ws_begin_end(js, priv, callback) : stream_obj;
}

static ant_value_t stream_writable_end_after_write(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  if (op->called) return js_mkundef();

  op->called = true;
  ant_value_t stream_obj = op->call.stream;
  ant_value_t callback = op->call.callback;
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
    ant_value_t after_write = stream_make_call(js, stream_writable_end_after_write, stream_obj, callback);
    if (is_err(after_write)) return after_write;
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
  ant_value_t stream_obj = js_func_data(js->current_func);
  stream_private_state_t *priv = stream_private_state(stream_obj);
  if (!priv || !priv->transform_pending) return js_mkundef();

  ant_value_t callback = priv->transform_callback;
  priv->transform_pending = false;
  stream_store(js, priv, &priv->transform_callback, js_mkundef());

  bool own_onwrite = callback == priv->writable.onwrite && priv->has_writable;
  if (nargs > 0 && !is_null(args[0]) && !is_undefined(args[0])) {
    if (own_onwrite) stream_writable_after_write(js, priv, args[0]);
    else if (is_callable(callback)) stream_call(js, callback, stream_obj, &args[0], 1);
    return js_mkundef();
  }

  if (nargs > 1 && !is_null(args[1]) && !is_undefined(args[1]))
    stream_readable_push_value(js, stream_obj, args[1], js_mkundef());

  if (own_onwrite) stream_writable_after_write(js, priv, js_mkundef());
  else if (is_callable(callback)) stream_call(js, callback, stream_obj, NULL, 0);
  return js_mkundef();
}

static ant_value_t stream_transform_write(
  ant_t *js, stream_private_state_t *priv,
  ant_value_t chunk, ant_value_t encoding, ant_value_t callback
) {
  ant_value_t stream_obj = stream_value(priv);
  ant_value_t transform_fn = stream_method(js, stream_obj, STREAM_METHOD_TRANSFORM);

  if (!is_callable(priv->transform_done))
    stream_store(js, priv, &priv->transform_done, js_heavy_mkfun(js, stream_transform_done, stream_obj));

  stream_store(js, priv, &priv->transform_callback, callback);
  priv->transform_pending = true;

  ant_value_t call_args[3] = { chunk, encoding, priv->transform_done };
  return stream_call(js, transform_fn, stream_obj, call_args, 3);
}

static ant_value_t js_transform__write(ant_params_t) {
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "Transform");
  if (is_err(stream_obj)) return stream_obj;

  return stream_transform_write(
    js, stream_private_state(stream_obj),
    nargs > 0 ? args[0] : js_mkundef(),
    nargs > 1 ? args[1] : js->builtins.stream_utf8,
    nargs > 2 ? args[2] : js_mkundef()
  );
}

static ant_value_t stream_transform_final_callback(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t stream_obj = op->call.stream;
  ant_value_t callback = op->call.callback;

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
  ant_value_t flush_fn = stream_method(js, stream_obj, STREAM_METHOD_FLUSH);
  
  if (!is_callable(flush_fn)) {
    stream_readable_push_value(js, stream_obj, js_mknull(), js_mkundef());
    stream_call_callback(js, callback, NULL, 0);
    return js_mkundef();
  }

  ant_value_t cb = stream_make_call(js, stream_transform_final_callback, stream_obj, callback);
  return is_err(cb) ? cb : stream_call(js, flush_fn, stream_obj, &cb, 1);
}

static ant_value_t js_passthrough__transform(ant_params_t) {
  return js_transform__transform(js, args, nargs, js_mkundef());
}

static void stream_finished_cleanup(ant_t *js, stream_finished_op_t *finished) {
  ant_value_t stream_obj = finished->stream;
  if (!is_object_type(stream_obj)) return;

  stream_remove_listener(js, stream_obj, "end", finished->on_finish);
  stream_remove_listener(js, stream_obj, "finish", finished->on_finish);
  stream_remove_listener(js, stream_obj, "close", finished->on_finish);
  stream_remove_listener(js, stream_obj, "error", finished->on_error);
}

static ant_value_t stream_finished_fire(ant_t *js, stream_op_t *op, ant_value_t error) {
  if (op->called) return js_mkundef();

  op->called = true;
  stream_finished_cleanup(js, &op->finished);

  if (is_undefined(error)) stream_call_callback(js, op->finished.callback, NULL, 0);
  else stream_call_callback(js, op->finished.callback, &error, 1);

  return js_mkundef();
}

static ant_value_t stream_finished_on_finish(ant_params_t) {
  return stream_finished_fire(js, stream_op_current(js), js_mkundef());
}

static ant_value_t stream_finished_on_error(ant_params_t) {
  return stream_finished_fire(js, stream_op_current(js), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t stream_finished_register(ant_t *js, ant_value_t stream_obj, ant_value_t callback) {
  if (!is_callable(callback)) callback = Ant_Function_Create(js_cfunc_noop);

  ant_value_t state;
  stream_op_t *op = stream_op_new(js, &state);
  if (!op) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t on_finish = js_heavy_mkfun(js, stream_finished_on_finish, state);
  ant_value_t on_error = js_heavy_mkfun(js, stream_finished_on_error, state);

  op->finished.stream = stream_obj;
  op->finished.callback = callback;
  op->finished.on_finish = on_finish;
  op->finished.on_error = on_error;
  stream_op_barrier(js, op);

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
  stream_op_t *op = stream_op_current(js);
  if (op->called) return js_mkundef();

  op->called = true;
  ant_value_t callback = op->pipeline.callback;

  if (nargs > 0 && !is_undefined(args[0])) stream_call_callback(js, callback, &args[0], 1);
  else stream_call_callback(js, callback, NULL, 0);

  return js_mkundef();
}

static ant_value_t stream_pipeline_error(ant_params_t) {
  ant_value_t done = js_func_data(js->current_func);
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
  
  if (!is_callable(callback)) callback = Ant_Function_Create(js_cfunc_noop);
  if (stream_count <= 0) return js_mkundef();

  ant_value_t state;
  stream_op_t *op = stream_op_new(js, &state);
  if (!op) return Ant_Error_CreateOutOfMemory(js);

  op->pipeline.callback = callback;
  stream_op_barrier(js, op);
  ant_value_t done = js_heavy_mkfun(js, stream_pipeline_done, state);

  if (stream_count < 2) {
    stream_next_tick(js, stream_pipeline_schedule_done, 1, done);
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
  ant_value_t promise = js_func_data(js->current_func);
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

static ant_value_t stream_async_dispose_done(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t promise = op->dispose.promise;
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();

  if (is_undefined(error) || is_null(error) || error == op->dispose.error)
    js_resolve_promise(js, promise, js_mkundef());
  else js_reject_promise(js, promise, error);

  return js_mkundef();
}

static ant_value_t js_stream_async_dispose(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t stream_obj = stream_require_this(js, js_getthis(js), "stream");
  
  if (is_err(stream_obj)) {
    js_reject_promise(js, promise, stream_obj);
    return promise;
  }

  stream_private_state_t *priv = stream_private_state(stream_obj);
  bool ended = priv->has_readable ? priv->readable.end_emitted : priv->writable.finished;

  if (priv->destroyed) {
    ant_value_t errored = priv->has_readable ? priv->readable.errored : priv->writable.errored;
    if (!is_undefined(errored) && !is_null(errored)) js_reject_promise(js, promise, errored);
    else if (ended) js_resolve_promise(js, promise, js_mkundef());
    else js_reject_promise(js, promise, stream_coded_error(js, "ERR_STREAM_PREMATURE_CLOSE", "Premature close"));
    
    return promise;
  }

  ant_value_t error = ended ? js_mknull() : abort_signal_make_error(js, js_mkundef());
  ant_value_t state;
  
  stream_op_t *op = stream_op_new(js, &state);
  if (!op) {
    js_reject_promise(js, promise, Ant_Error_ConsumeMarker(js, Ant_Error_CreateOutOfMemory(js)));
    return promise;
  }

  op->dispose.promise = promise;
  op->dispose.error = error;
  stream_op_barrier(js, op);
  
  ant_value_t done = js_heavy_mkfun(js, stream_async_dispose_done, state);
  ant_value_t registered = stream_finished_register(js, stream_obj, done);
  
  if (is_err(registered)) {
    js_reject_promise(js, promise, registered);
    return promise;
  }

  ant_value_t destroyed = stream_call_prop(js, stream_obj, "destroy", &error, 1);
  if (is_err(destroyed)) js_reject_promise(js, promise, destroyed);

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
    js_reject_promise(js, promise, Ant_Error_ConsumeMarker(js, Ant_Error_CreateOutOfMemory(js)));
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

static void stream_release_reader(ant_t *js, stream_op_t *op) {
  ant_value_t reader = op->from.reader;
  if (!is_object_type(reader)) return;

  stream_op_store(js, op, &op->from.reader, js_mkundef());
  stream_call_prop(js, reader, "releaseLock", NULL, 0);
}

static void stream_readable_from_fail(ant_t *js, stream_op_t *op, ant_value_t error) {
  GC_ROOT_SAVE(root_mark, js);
  error = Ant_Error_ConsumeMarker(js, error);
  GC_ROOT_PIN(js, error);

  op->from_reading = false;
  stream_release_reader(js, op);
  if (stream_is_instance(op->from.readable)) stream_destroy(js, op->from.readable, error);

  GC_ROOT_RESTORE(js, root_mark);
}

static bool stream_readable_from_push(ant_t *js, stream_op_t *op, bool done, ant_value_t value) {
  stream_private_state_t *priv = stream_readable_of(op->from.readable);

  if (!priv || priv->destroyed) {
    stream_release_reader(js, op);
    return false;
  }

  if (done) {
    op->from_reading = false;
    stream_release_reader(js, op);
    stream_rs_add(js, priv, js_mknull(), js_mkundef(), false);
    return false;
  }

  if (is_null(value)) {
    ant_value_t err = js_mkerr_code(js, JS_ERR_TYPE, "ERR_STREAM_NULL_VALUES", "May not write null values to stream");
    stream_readable_from_fail(js, op, err);
    return false;
  }

  ant_value_t pushed = stream_rs_add(js, priv, value, js_mkundef(), false);
  if (is_err(pushed)) {
    stream_readable_from_fail(js, op, pushed);
    return false;
  }

  if (pushed == js_true) return true;
  op->from_reading = false;
  return false;
}

static bool stream_readable_from_accept(ant_t *js, stream_op_t *op, ant_value_t result) {
  if (stream_is_destroyed(js, op->from.readable)) {
    stream_release_reader(js, op);
    return false;
  }

  if (!is_object_type(result)) {
    stream_readable_from_fail(js, op, Ant_Error_Create(js, JS_ERR_TYPE, "iterator result is not an object"));
    return false;
  }

  ant_value_t done = js_get(js, result, "done");
  if (is_err(done)) {
    stream_readable_from_fail(js, op, done);
    return false;
  }

  ant_value_t value = js_truthy(js, done) ? js_mkundef() : js_get(js, result, "value");
  if (is_err(value)) {
    stream_readable_from_fail(js, op, value);
    return false;
  }

  return stream_readable_from_push(js, op, js_truthy(js, done), value);
}

static void stream_readable_from_step(ant_t *js, ant_value_t state);

static ant_value_t stream_readable_from_on_result(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  if (stream_readable_from_accept(js, stream_op(state), nargs > 0 ? args[0] : js_mkundef()))
    stream_readable_from_step(js, state);
  return js_mkundef();
}

static ant_value_t stream_readable_from_on_value(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  if (stream_readable_from_push(js, stream_op(state), false, nargs > 0 ? args[0] : js_mkundef()))
    stream_readable_from_step(js, state);
  return js_mkundef();
}

static ant_value_t stream_readable_from_reject(ant_params_t) {
  ant_value_t error = nargs > 0 ? args[0] : Ant_Error_Create(js, JS_ERR_GENERIC, "stream iteration failed");
  stream_readable_from_fail(js, stream_op_current(js), error);
  return js_mkundef();
}

static int stream_readable_from_await(ant_t *js, ant_value_t state, ant_value_t value, ant_cfunc_t on_value) {
  stream_op_t *op = stream_op(state);
  bool native_promise = vtype(value) == kTypePromise;

  ant_value_t then_fn = !native_promise && is_object_type(value) ? js_getprop_fallback(js, value, "then") : js_mkundef();
  if (is_err(then_fn)) {
    stream_readable_from_fail(js, op, then_fn);
    return -1;
  }
  if (!native_promise && !is_callable(then_fn)) return 0;

  ant_value_t callbacks[2] = {
    js_heavy_mkfun(js, on_value, state),
    js_heavy_mkfun(js, stream_readable_from_reject, state),
  };

  ant_value_t result = native_promise
    ? js_promise_then(js, value, callbacks[0], callbacks[1])
    : stream_call(js, then_fn, value, callbacks, 2);

  if (is_err(result)) {
    stream_readable_from_fail(js, op, result);
    return -1;
  }

  if (native_promise) promise_mark_handled(result);
  return 1;
}

static void stream_readable_from_step(ant_t *js, ant_value_t state) {
  stream_op_t *op = stream_op(state);

  for (;;) {
    stream_private_state_t *priv = stream_readable_of(op->from.readable);
    if (!priv || priv->destroyed) {
      stream_release_reader(js, op);
      return;
    }

    if (op->from_mode == STREAM_FROM_VALUE) {
      op->from_reading = false;
      stream_rs_add(js, priv, op->from.source, js_mkundef(), false);
      stream_rs_add(js, priv, js_mknull(), js_mkundef(), false);
      return;
    }

    if (op->from_mode == STREAM_FROM_SYNC && op->from_advance) {
      iterator_t it = { op->from.iterator, op->from.next, op->from_advance };
      ant_value_t value = js_mkundef();

      if (!js_iter_next(js, &it, &value)) {
        if (Ant_Exception_Pending(js)) stream_readable_from_fail(js, op, Ant_Exception_Current(js));
        else stream_readable_from_push(js, op, true, js_mkundef());
        return;
      }

      int awaited = stream_readable_from_await(js, state, value, stream_readable_from_on_value);
      if (awaited != 0) return;
      if (!stream_readable_from_push(js, op, false, value)) return;
      continue;
    }

    const char *method = op->from_mode == STREAM_FROM_READER ? "read" : "next";
    ant_value_t next_result = stream_call_prop(js, op->from.iterator, method, NULL, 0);
    if (is_err(next_result)) {
      stream_readable_from_fail(js, op, next_result);
      return;
    }

    if (op->from_mode != STREAM_FROM_SYNC) {
      int awaited = stream_readable_from_await(js, state, next_result, stream_readable_from_on_result);
      if (awaited != 0) return;
      if (!stream_readable_from_accept(js, op, next_result)) return;
      continue;
    }

    if (!is_object_type(next_result)) {
      stream_readable_from_accept(js, op, next_result);
      return;
    }

    ant_value_t done = js_get(js, next_result, "done");
    if (is_err(done)) {
      stream_readable_from_fail(js, op, done);
      return;
    }

    ant_value_t value = js_truthy(js, done) ? js_mkundef() : js_get(js, next_result, "value");
    if (is_err(value)) {
      stream_readable_from_fail(js, op, value);
      return;
    }

    if (!js_truthy(js, done)) {
      int awaited = stream_readable_from_await(js, state, value, stream_readable_from_on_value);
      if (awaited != 0) return;
    }

    if (!stream_readable_from_push(js, op, js_truthy(js, done), value)) return;
  }
}

static ant_value_t stream_readable_from_read_state(ant_t *js, ant_value_t state) {
  stream_op_t *op = stream_op(state);
  if (!op || op->from_reading) return js_mkundef();

  op->from_reading = true;
  stream_readable_from_step(js, state);

  return js_mkundef();
}

static ant_value_t stream_readable_from_read(ant_params_t) {
  return stream_readable_from_read_state(js, js_func_data(js->current_func));
}

static ant_value_t stream_readable_from_destroy(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t error = nargs > 0 ? args[0] : js_mknull();
  ant_value_t callback = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t iterator = op->from.iterator;

  stream_release_reader(js, op);
  if (is_object_type(iterator) && op->from_mode != STREAM_FROM_READER) {
    ant_value_t returned = stream_call_prop(js, iterator, "return", NULL, 0);
    if (is_err(returned)) Ant_Error_ConsumeMarker(js, returned);
    else if (vtype(returned) == kTypePromise) promise_mark_handled(returned);
  }

  stream_call_callback(js, callback, &error, 1);
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
  stream_from_mode_t mode = STREAM_FROM_VALUE;
  ant_value_t iterator = js_mkundef();
  ant_value_t reader = js_mkundef();
  ant_value_t readable = js_mkundef();
  ant_value_t state = js_mkundef();
  ant_value_t result = js_mkundef();
  ant_value_t next_fn = js_mkundef();
  js_iter_advance_fn advance = NULL;
  stream_op_t *op = NULL;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, source);
  GC_ROOT_PIN(js, options);
  GC_ROOT_PIN(js, iterator);
  GC_ROOT_PIN(js, next_fn);
  GC_ROOT_PIN(js, reader);
  GC_ROOT_PIN(js, readable);

  bool is_value = vtype(source) == kTypeString ||
    (is_object_type(source) && js_instance_of_proto(js, source, js->builtins.buffer_proto));

  if (!is_value) {
    ant_value_t async_fn = is_object_type(source) ? js_get_sym(js, source, js->sym.asyncIterator_sym) : js_mkundef();
    if (is_err(async_fn)) { result = async_fn; goto done; }

    ant_value_t sync_fn = !is_callable(async_fn) && is_object_type(source)
      ? js_get_sym(js, source, js->sym.iterator_sym)
      : js_mkundef();
    if (is_err(sync_fn)) { result = sync_fn; goto done; }

    ant_value_t reader_fn = !is_callable(async_fn) && !is_callable(sync_fn) && is_object_type(source)
      ? js_get(js, source, "getReader")
      : js_mkundef();
    if (is_err(reader_fn)) { result = reader_fn; goto done; }

    if (is_callable(async_fn) || is_callable(sync_fn)) {
      mode = is_callable(async_fn) ? STREAM_FROM_ASYNC : STREAM_FROM_SYNC;
      iterator = stream_call(js, is_callable(async_fn) ? async_fn : sync_fn, source, NULL, 0);
      if (is_err(iterator)) { result = iterator; goto done; }
      if (mode == STREAM_FROM_SYNC && is_object_type(iterator)) {
        next_fn = js_get(js, iterator, "next");
        if (is_err(next_fn)) { result = next_fn; goto done; }
        advance = js_iter_find_advance(js, iterator, next_fn);
      }
    } else if (is_callable(reader_fn)) {
      mode = STREAM_FROM_READER;
      reader = stream_call(js, reader_fn, source, NULL, 0);
      if (is_err(reader)) { result = reader; goto done; }
      iterator = reader;
    } else {
      result = Ant_Error_CreateInvalidArgType(js, "iterable", "an instance of Iterable", source);
      goto done;
    }
  }

  readable = stream_construct(js, js->builtins.readable_proto, options, stream_init_readable, js_mkundef());
  if (is_err(readable)) { result = readable; goto done; }

  op = stream_op_new(js, &state);
  if (!op) { result = Ant_Error_CreateOutOfMemory(js); goto done; }

  op->from_mode = mode;
  op->from.readable = readable;
  op->from.source = source;
  op->from.iterator = iterator;
  op->from.reader = reader;
  op->from.next = next_fn;
  op->from_advance = advance;
  stream_op_barrier(js, op);

  ant_value_t read_fn = js_heavy_mkfun(js, stream_readable_from_read, state);
  stream_private_state_t *readable_priv = stream_private_state(readable);
  if (readable_priv) stream_store(js, readable_priv, &readable_priv->from_read, read_fn);
  js_set(js, readable, "_read", read_fn);
  js_set(js, readable, "_destroy", js_heavy_mkfun(js, stream_readable_from_destroy, state));
  result = readable;

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t js_readable_from(ant_params_t) {
  ant_value_t options = stream_readable_from_options(js, nargs > 1 ? args[1] : js_mkundef());
  return stream_readable_from_source(js, nargs > 0 ? args[0] : js_mkundef(), options);
}

static ant_value_t js_readable_from_web(ant_params_t) {
  return stream_readable_from_source(js, nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef());
}

static bool stream_to_web_close(ant_t *js, stream_op_t *op) {
  if (op->web_closed) return false;
  op->web_closed = true;

  stream_web_op_t *web = &op->web;
  if (!is_object_type(web->source)) return true;

  stream_remove_listener(js, web->source, "data", web->on_data);
  stream_remove_listener(js, web->source, "end", web->on_end);
  stream_remove_listener(js, web->source, "error", web->on_error);

  return true;
}

static ant_value_t stream_to_web_on_data(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  if (op->web_closed) return js_mkundef();
  return rs_controller_enqueue(js, op->web.controller, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t stream_to_web_on_end(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  if (stream_to_web_close(js, op)) rs_controller_close(js, op->web.controller);
  return js_mkundef();
}

static ant_value_t stream_to_web_on_error(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t error = nargs > 0 ? args[0] : Ant_Error_Create(js, JS_ERR_GENERIC, "stream error");
  if (stream_to_web_close(js, op)) readable_stream_error(js, op->web.stream, error);
  return js_mkundef();
}

static ant_value_t stream_to_web_cancel(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t source = op->web.source;
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();

  if (!stream_to_web_close(js, op) || !is_object_type(source)) return js_mkundef();

  ant_value_t destroy_fn = js_getprop_fallback(js, source, "destroy");
  if (!is_callable(destroy_fn)) return js_mkundef();

  ant_value_t cancel_result = stream_call(js, destroy_fn, source, &reason, 1);
  return is_err(cancel_result) ? cancel_result : js_mkundef();
}

static ant_value_t js_readable_to_web(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : js_mkundef();
  if (!is_object_type(source))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Readable.toWeb requires a stream");

  ant_value_t state;
  stream_op_t *op = stream_op_new(js, &state);
  if (!op) return Ant_Error_CreateOutOfMemory(js);

  stream_op_store(js, op, &op->web.source, source);
  ant_value_t web_stream = rs_create_stream(js, js_mkundef(), js_heavy_mkfun(js, stream_to_web_cancel, state), 1.0);
  if (is_err(web_stream)) return web_stream;

  ant_value_t controller = rs_stream_controller(js, web_stream);
  ant_value_t on_data = js_heavy_mkfun(js, stream_to_web_on_data, state);
  ant_value_t on_end = js_heavy_mkfun(js, stream_to_web_on_end, state);
  ant_value_t on_error = js_heavy_mkfun(js, stream_to_web_on_error, state);

  op->web.stream = web_stream;
  op->web.controller = controller;
  op->web.on_data = on_data;
  op->web.on_end = on_end;
  op->web.on_error = on_error;
  stream_op_barrier(js, op);

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

static double stream_iter_queued(ant_t *js, stream_op_t *op) {
  return (double)js_arr_len(js, op->iter.queue) - (double)op->iter_head;
}
  
static void stream_iter_clear_queue(ant_t *js, stream_op_t *op) {
  js_arr_set_dense_length(js, op->iter.queue, 0);
  op->iter_head = 0;
}

static void stream_iter_detach(ant_t *js, stream_op_t *op) {
  if (op->iter_detached) return;
  op->iter_detached = true;

  stream_iter_op_t *iter = &op->iter;
  stream_remove_listener(js, iter->stream, "data", iter->on_data);
  stream_remove_listener(js, iter->stream, "end", iter->on_end);
  stream_remove_listener(js, iter->stream, "error", iter->on_error);
  stream_remove_listener(js, iter->stream, "close", iter->on_end);
}

static void stream_iter_settle(ant_t *js, stream_op_t *op, ant_value_t result, bool reject) {
  ant_value_t pending = op->iter.pending;
  if (vtype(pending) != kTypePromise) return;
  
  op->iter.pending = js_mkundef();
  if (reject) js_reject_promise(js, pending, result);
  else js_resolve_promise(js, pending, result);
}

static ant_value_t stream_iter_on_data(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t chunk = nargs > 0 ? args[0] : js_mkundef();

  if (vtype(op->iter.pending) == kTypePromise) {
    stream_iter_settle(js, op, js_iter_result(js, true, chunk), false);
    return js_mkundef();
  }

  js_arr_push(js, op->iter.queue, chunk);
  if (stream_iter_queued(js, op) >= STREAM_ITER_PAUSE_AT && !op->iter_paused) {
    op->iter_paused = true;
    stream_call_prop(js, op->iter.stream, "pause", NULL, 0);
  }
  
  return js_mkundef();
}

static ant_value_t stream_iter_on_end(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  op->iter_ended = true;
  
  stream_iter_detach(js, op);
  stream_iter_settle(js, op, js_iter_result(js, false, js_mkundef()), false);
  
  return js_mkundef();
}

static ant_value_t stream_iter_on_error(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();
  stream_iter_detach(js, op);

  if (vtype(op->iter.pending) == kTypePromise) {
    op->iter_ended = true;
    stream_iter_settle(js, op, error, true);
  } else {
    stream_op_store(js, op, &op->iter.error, error);
    op->iter_has_error = true;
  }
  
  return js_mkundef();
}

static ant_value_t stream_iter_next(ant_params_t) {
  stream_op_t *op = stream_op_current(js);

  if (stream_iter_queued(js, op) > 0) {
    ant_value_t queue = op->iter.queue;
    ant_value_t chunk = js_arr_get(js, queue, op->iter_head);

    if (op->iter_head + 1 >= js_arr_len(js, queue)) stream_iter_clear_queue(js, op);
    else op->iter_head++;

    if (op->iter_paused && stream_iter_queued(js, op) <= STREAM_ITER_RESUME_AT && !op->iter_detached) {
      op->iter_paused = false;
      stream_call_prop(js, op->iter.stream, "resume", NULL, 0);
    }
    
    return stream_iter_resolved(js, js_iter_result(js, true, chunk));
  }

  if (op->iter_has_error) {
    ant_value_t error = op->iter.error;
    op->iter_has_error = false;
    op->iter.error = js_mkundef();
    op->iter_ended = true;
    return stream_iter_rejected(js, error);
  }

  if (op->iter_ended) return stream_iter_resolved(js, js_iter_result(js, false, js_mkundef()));
  if (vtype(op->iter.pending) == kTypePromise) return op->iter.pending;

  ant_value_t pending = js_mkpromise(js);
  stream_op_store(js, op, &op->iter.pending, pending);
  
  return pending;
}

static ant_value_t stream_iter_return(ant_params_t) {
  stream_op_t *op = stream_op_current(js);
  ant_value_t done = js_iter_result(js, false, nargs > 0 ? args[0] : js_mkundef());
  ant_value_t stream_obj = op->iter.stream;

  op->iter_ended = true;
  stream_iter_clear_queue(js, op);
  stream_iter_detach(js, op);
  stream_iter_settle(js, op, js_iter_result(js, false, js_mkundef()), false);

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

  ant_value_t state;
  stream_op_t *op = stream_op_new(js, &state);
  if (!op) return Ant_Error_CreateOutOfMemory(js);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, state);

  ant_value_t queue = js_mkarr(js);
  ant_value_t on_data = js_heavy_mkfun(js, stream_iter_on_data, state);
  ant_value_t on_end = js_heavy_mkfun(js, stream_iter_on_end, state);
  ant_value_t on_error = js_heavy_mkfun(js, stream_iter_on_error, state);

  op->iter_detached = ended;
  op->iter_ended = ended;
  op->iter.stream = stream_obj;
  op->iter.queue = queue;
  op->iter.on_data = on_data;
  op->iter.on_end = on_end;
  op->iter.on_error = on_error;
  
  stream_op_barrier(js, op);
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

static ant_value_t stream_construct_transform_like(ant_t *js, ant_value_t base_proto, ant_value_t options, ant_value_t new_target) {
  ant_value_t obj = stream_construct_duplex_like(js, base_proto, options, new_target);
  stream_private_state_t *priv = stream_readable_of(obj);
  if (priv) priv->readable.sync = false;
  return obj;
}

static ant_value_t js_transform_ctor(ant_params_t) {
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t obj = stream_construct_transform_like(js, js->builtins.stream_transform_proto, options, call_new_target);
  ant_value_t transform_fn = stream_get_option(js, options, "transform");
  ant_value_t flush_fn = stream_get_option(js, options, "flush");
  
  if (is_callable(transform_fn)) js_set(js, obj, "_transform", transform_fn);
  if (is_callable(flush_fn)) js_set(js, obj, "_flush", flush_fn);
  
  return obj;
}

static ant_value_t js_passthrough_ctor(ant_params_t) {
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  return stream_construct_transform_like(js, js->builtins.passthrough_proto, options, call_new_target);
}

static void stream_define_writable_methods(ant_t *js, ant_value_t proto) {
  js_set(js, proto, "_write", Ant_Function_CreateWithArity(js_writable__write, 3));
  js_set(js, proto, "_final", Ant_Function_Create(js_writable__final));
  js_set(js, proto, "write", Ant_Function_CreateWithArity(js_writable_write, 3));
  js_set(js, proto, "end", Ant_Function_CreateWithArity(js_writable_end, 3));
  js_set(js, proto, "cork", Ant_Function_Create(js_cfunc_noop));
  js_set(js, proto, "uncork", Ant_Function_Create(js_cfunc_noop));
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
  js_set(js, js->builtins.readable_proto, "unshift", Ant_Function_CreateWithArity(js_readable_unshift, 2));
  js_set(js, js->builtins.readable_proto, "setEncoding", Ant_Function_CreateWithArity(js_readable_set_encoding, 1));
  js_set(js, js->builtins.readable_proto, "on", Ant_Function_CreateWithArity(js_readable_on, 2));
  js_set_exact(js, js->builtins.readable_proto, "addListener", js_get(js, js->builtins.readable_proto, "on"));
  js_set(js, js->builtins.readable_proto, "once", Ant_Function_Create(js_readable_once));
  js_set(js, js->builtins.readable_proto, "resume", Ant_Function_Create(js_readable_resume));
  js_set(js, js->builtins.readable_proto, "pause", Ant_Function_Create(js_readable_pause));
  js_set(js, js->builtins.readable_proto, "isPaused", Ant_Function_Create(js_readable_is_paused));
  js_set(js, js->builtins.readable_proto, "removeListener", Ant_Function_CreateWithArity(js_readable_remove_listener, 2));
  js_set_exact(js, js->builtins.readable_proto, "off", js_get(js, js->builtins.readable_proto, "removeListener"));
  js_set(js, js->builtins.readable_proto, "removeAllListeners", Ant_Function_CreateWithArity(js_readable_remove_all_listeners, 1));
  stream_define_readable_accessors(js, js->builtins.readable_proto);
  js_set_sym(js, js->builtins.readable_proto, js->sym.asyncIterator_sym, Ant_Function_Create(js_readable_async_iterator));
  js_set_sym(js, js->builtins.readable_proto, js->sym.asyncDispose_sym, Ant_Function_Create(js_stream_async_dispose));
  js_set_tostringtag(js, js->builtins.readable_proto, js_mkstr(js, "Readable", 8));
  js->builtins.readable_ctor = Ant_Function_CreateConstructor(js, js_readable_ctor, js->builtins.readable_proto, "Readable", 1);
  js_set(js, js->builtins.readable_ctor, "from", Ant_Function_Create(js_readable_from));
  js_set(js, js->builtins.readable_ctor, "fromWeb", Ant_Function_Create(js_readable_from_web));
  js_set(js, js->builtins.readable_ctor, "toWeb", Ant_Function_Create(js_readable_to_web));

  js->builtins.writable_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.writable_proto, js->builtins.stream_proto);
  js_set(js, js->builtins.writable_proto, "_destroy", js->builtins.stream_default_destroy);
  stream_define_writable_methods(js, js->builtins.writable_proto);
  js_set_sym(js, js->builtins.writable_proto, js->sym.asyncDispose_sym, Ant_Function_Create(js_stream_async_dispose));
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
  stream_init_fn init_fn
) {
  stream_init_constructors(js);
  if (!is_object_type(obj)) return;

  init_fn(js, obj, options);
  if (js_instance_of_proto(js, obj, js->builtins.stream_proto)) return;

  stream_private_state_t *priv = stream_private_state(obj);
  stream_define_stream_accessors(js, obj);
  if (priv && priv->has_readable) stream_define_readable_accessors(js, obj);
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
  stream_private_state_t *priv = stream_readable_of(stream_obj);
  if (!priv) return js_false;

  if (!priv->readable.in_read) priv->readable.sync = false;
  return stream_rs_add(js, priv, chunk, encoding, false);
}

ant_value_t stream_readable_end(ant_t *js, ant_value_t stream_obj) {
  ant_value_t result = stream_readable_push(js, stream_obj, js_mknull(), js_mkundef());
  if (is_err(result)) return result;

  stream_private_state_t *priv = stream_readable_of(stream_obj);
  ant_value_t read = priv ? stream_rs_read(js, priv, 0) : js_mknull();

  return is_err(read) ? read : result;
}

static ant_value_t stream_construct_done(ant_params_t) {
  ant_value_t stream_obj = js_func_data(js->current_func);
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

  if (priv->has_readable && priv->readable.need_readable) stream_rs_maybe_read_more(js, priv);
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
  stream_next_tick(js, stream_construct_tick, 1, stream_obj);
}

ant_value_t stream_construct(
  ant_t *js, ant_value_t base_proto, ant_value_t raw_options,
  stream_init_fn init_fn, ant_value_t new_target
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
