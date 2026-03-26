#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/symbol.h"
#include "modules/buffer.h"
#include "streams/compression.h"
#include "streams/transform.h"
#include "streams/readable.h"

ant_value_t g_cs_proto;
ant_value_t g_ds_proto;

typedef struct {
  z_stream strm;
  zformat_t format;
  bool initialized;
} zstate_t;

static int zfmt_window_bits(zformat_t fmt, bool decompress) {
  switch (fmt) {
    case ZFMT_GZIP:        return decompress ? (15 + 32) : (15 + 16);
    case ZFMT_DEFLATE:     return 15;
    case ZFMT_DEFLATE_RAW: return -15;
  }
  return 15;
}

static int parse_format(ant_t *js, ant_value_t arg, zformat_t *out) {
  if (vtype(arg) != T_STR) return -1;
  size_t len;
  const char *s = js_getstr(js, arg, &len);
  if (!s) return -1;
  if (len == 4 && !memcmp(s, "gzip", 4)) { *out = ZFMT_GZIP; return 0; }
  if (len == 7 && !memcmp(s, "deflate", 7)) { *out = ZFMT_DEFLATE; return 0; }
  if (len == 11 && !memcmp(s, "deflate-raw", 11)) { *out = ZFMT_DEFLATE_RAW; return 0; }
  return -1;
}

static ant_value_t get_ts(ant_value_t obj) {
  return js_get_slot(obj, SLOT_ENTRIES);
}

bool cs_is_stream(ant_value_t obj) {
  return is_object_type(obj)
    && vtype(js_get_slot(obj, SLOT_DATA)) == T_NUM
    && ts_is_stream(get_ts(obj));
}

bool ds_is_stream(ant_value_t obj) {
  return is_object_type(obj)
    && vtype(js_get_slot(obj, SLOT_DATA)) == T_NUM
    && ts_is_stream(get_ts(obj));
}

ant_value_t cs_stream_readable(ant_value_t obj) {
  return ts_stream_readable(get_ts(obj));
}

ant_value_t cs_stream_writable(ant_value_t obj) {
  return ts_stream_writable(get_ts(obj));
}

ant_value_t ds_stream_readable(ant_value_t obj) {
  return ts_stream_readable(get_ts(obj));
}

ant_value_t ds_stream_writable(ant_value_t obj) {
  return ts_stream_writable(get_ts(obj));
}

static void zstate_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    zstate_t *st = (zstate_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
    if (st->initialized) deflateEnd(&st->strm);
    free(st);
    return;
  }}
}

static void zstate_inflate_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    zstate_t *st = (zstate_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
    if (st->initialized) inflateEnd(&st->strm);
    free(st);
    return;
  }}
}

static bool get_chunk_bytes(ant_t *js, ant_value_t chunk, const uint8_t **out, size_t *len) {
  ant_value_t slot = js_get_slot(chunk, SLOT_BUFFER);
  TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(slot);
  if (ta) {
    if (!ta->buffer || ta->buffer->is_detached) { *out = NULL; *len = 0; return true; }
    *out = ta->buffer->data + ta->byte_offset;
    *len = ta->byte_length;
    return true;
  }

  if (vtype(slot) == T_NUM) {
    ArrayBufferData *ab = (ArrayBufferData *)(uintptr_t)(size_t)js_getnum(slot);
    if (!ab || ab->is_detached) { *out = NULL; *len = 0; return true; }
    *out = ab->data;
    *len = ab->length;
    return true;
  }

  ant_value_t buf_prop = js_get(js, chunk, "buffer");
  if (is_object_type(buf_prop)) {
  ant_value_t buf_slot = js_get_slot(buf_prop, SLOT_BUFFER);
  if (vtype(buf_slot) == T_NUM) {
    ArrayBufferData *ab = (ArrayBufferData *)(uintptr_t)(size_t)js_getnum(buf_slot);
    if (!ab || ab->is_detached) { *out = NULL; *len = 0; return true; }
    ant_value_t off_v = js_get(js, chunk, "byteOffset");
    ant_value_t len_v = js_get(js, chunk, "byteLength");
    size_t off  = (vtype(off_v) == T_NUM) ? (size_t)js_getnum(off_v) : 0;
    size_t blen = (vtype(len_v) == T_NUM) ? (size_t)js_getnum(len_v) : ab->length - off;
    *out = ab->data + off;
    *len = blen;
    return true;
  }}

  return false;
}

static ant_value_t enqueue_buffer(ant_t *js, ant_value_t ts_obj, const uint8_t *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr(js, "out of memory");
  memcpy(ab->data, data, len);
  ant_value_t arr = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Uint8Array");
  ant_value_t readable = ts_stream_readable(ts_obj);
  ant_value_t rs_ctrl = rs_stream_controller(js, readable);
  return rs_controller_enqueue(js, rs_ctrl, arr);
}

#define ZCHUNK_SIZE 16384

static ant_value_t cs_transform(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t state_val = js_get_slot(wrapper, SLOT_DATA);
  zstate_t *st = (zstate_t *)(uintptr_t)(size_t)js_getnum(state_val);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);

  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  const uint8_t *input = NULL;
  size_t input_len = 0;

  if (!is_object_type(chunk) || !get_chunk_bytes(js, chunk, &input, &input_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");

  if (!input || input_len == 0) return js_mkundef();

  st->strm.next_in = (Bytef *)input;
  st->strm.avail_in = (uInt)input_len;

  uint8_t out_buf[ZCHUNK_SIZE];
  do {
    st->strm.next_out = out_buf;
    st->strm.avail_out = ZCHUNK_SIZE;
    int ret = deflate(&st->strm, Z_NO_FLUSH);
    if (ret == Z_STREAM_ERROR)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Compression failed");
    size_t have = ZCHUNK_SIZE - st->strm.avail_out;
    if (have > 0) {
      ant_value_t r = enqueue_buffer(js, ts_obj, out_buf, have);
      if (is_err(r)) return r;
    }
  } while (st->strm.avail_out == 0);

  return js_mkundef();
}

static ant_value_t cs_flush(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t state_val = js_get_slot(wrapper, SLOT_DATA);
  zstate_t *st = (zstate_t *)(uintptr_t)(size_t)js_getnum(state_val);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);

  st->strm.next_in = NULL;
  st->strm.avail_in = 0;

  uint8_t out_buf[ZCHUNK_SIZE];
  int ret;
  do {
    st->strm.next_out = out_buf;
    st->strm.avail_out = ZCHUNK_SIZE;
    ret = deflate(&st->strm, Z_FINISH);
    size_t have = ZCHUNK_SIZE - st->strm.avail_out;
    if (have > 0) {
      ant_value_t r = enqueue_buffer(js, ts_obj, out_buf, have);
      if (is_err(r)) return r;
    }
  } while (ret != Z_STREAM_END);

  return js_mkundef();
}

static ant_value_t js_cs_get_readable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = get_ts(js->this_val);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid CompressionStream");
  return ts_stream_readable(ts_obj);
}

static ant_value_t js_cs_get_writable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = get_ts(js->this_val);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid CompressionStream");
  return ts_stream_writable(ts_obj);
}

static ant_value_t js_cs_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "CompressionStream constructor requires 'new'");

  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'CompressionStream': 1 argument required");

  zformat_t fmt;
  if (parse_format(js, args[0], &fmt) < 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'CompressionStream': Unsupported compression format");

  zstate_t *st = calloc(1, sizeof(zstate_t));
  if (!st) return js_mkerr(js, "out of memory");
  st->format = fmt;

  int ret = deflateInit2(&st->strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
    zfmt_window_bits(fmt, false), 8, Z_DEFAULT_STRATEGY);
  if (ret != Z_OK) { free(st); return js_mkerr(js, "Failed to initialize compression"); }
  st->initialized = true;

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_cs_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_DATA, ANT_PTR(st));
  js_set_finalizer(obj, zstate_finalize);

  ant_value_t wrapper = js_mkobj(js);
  js_set_slot(wrapper, SLOT_DATA, ANT_PTR(st));

  ant_value_t transformer = js_mkobj(js);
  ant_value_t transform_fn = js_heavy_mkfun(js, cs_transform, wrapper);
  ant_value_t flush_fn = js_heavy_mkfun(js, cs_flush, wrapper);
  js_set(js, transformer, "transform", transform_fn);
  js_set(js, transformer, "flush", flush_fn);

  ant_value_t ctor_args[1] = { transformer };
  ant_value_t saved_new_target = js->new_target;
  ant_value_t saved_this = js->this_val;
  js->new_target = js_mknum(1);

  ant_value_t ts_obj = js_ts_ctor(js, ctor_args, 1);
  js->new_target = saved_new_target;
  js->this_val = saved_this;

  if (is_err(ts_obj)) { deflateEnd(&st->strm); free(st); return ts_obj; }
  js_set_slot(obj, SLOT_ENTRIES, ts_obj);
  js_set_slot(wrapper, SLOT_ENTRIES, ts_obj);

  return obj;
}

static ant_value_t ds_transform(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t wrapper = js_get_slot(js->current_func, SLOT_DATA);
  ant_value_t state_val = js_get_slot(wrapper, SLOT_DATA);
  zstate_t *st = (zstate_t *)(uintptr_t)(size_t)js_getnum(state_val);
  ant_value_t ts_obj = js_get_slot(wrapper, SLOT_ENTRIES);

  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  const uint8_t *input = NULL;
  size_t input_len = 0;

  if (!is_object_type(chunk) || !get_chunk_bytes(js, chunk, &input, &input_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");

  if (!input || input_len == 0) return js_mkundef();

  st->strm.next_in = (Bytef *)input;
  st->strm.avail_in = (uInt)input_len;

  uint8_t out_buf[ZCHUNK_SIZE];
  do {
    st->strm.next_out = out_buf;
    st->strm.avail_out = ZCHUNK_SIZE;
    int ret = inflate(&st->strm, Z_NO_FLUSH);
    if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Decompression failed");
    size_t have = ZCHUNK_SIZE - st->strm.avail_out;
    if (have > 0) {
      ant_value_t r = enqueue_buffer(js, ts_obj, out_buf, have);
      if (is_err(r)) return r;
    }
    if (ret == Z_STREAM_END) break;
  } while (st->strm.avail_out == 0);

  return js_mkundef();
}

static ant_value_t ds_flush(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static ant_value_t js_ds_get_readable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = get_ts(js->this_val);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid DecompressionStream");
  return ts_stream_readable(ts_obj);
}

static ant_value_t js_ds_get_writable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = get_ts(js->this_val);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid DecompressionStream");
  return ts_stream_writable(ts_obj);
}

static ant_value_t js_ds_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "DecompressionStream constructor requires 'new'");

  if (nargs < 1)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'DecompressionStream': 1 argument required");

  zformat_t fmt;
  if (parse_format(js, args[0], &fmt) < 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'DecompressionStream': Unsupported compression format");

  zstate_t *st = calloc(1, sizeof(zstate_t));
  if (!st) return js_mkerr(js, "out of memory");
  st->format = fmt;

  int ret = inflateInit2(&st->strm, zfmt_window_bits(fmt, true));
  if (ret != Z_OK) { free(st); return js_mkerr(js, "Failed to initialize decompression"); }
  st->initialized = true;

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_ds_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_DATA, ANT_PTR(st));
  js_set_finalizer(obj, zstate_inflate_finalize);

  ant_value_t wrapper = js_mkobj(js);
  js_set_slot(wrapper, SLOT_DATA, ANT_PTR(st));

  ant_value_t transformer = js_mkobj(js);
  ant_value_t transform_fn = js_heavy_mkfun(js, ds_transform, wrapper);
  ant_value_t flush_fn = js_heavy_mkfun(js, ds_flush, wrapper);
  
  js_set(js, transformer, "transform", transform_fn);
  js_set(js, transformer, "flush", flush_fn);

  ant_value_t ctor_args[1] = { transformer };
  ant_value_t saved_new_target = js->new_target;
  ant_value_t saved_this = js->this_val;
  js->new_target = js_mknum(1);

  ant_value_t ts_obj = js_ts_ctor(js, ctor_args, 1);

  js->new_target = saved_new_target;
  js->this_val = saved_this;
  if (is_err(ts_obj)) { inflateEnd(&st->strm); free(st); return ts_obj; }

  js_set_slot(obj, SLOT_ENTRIES, ts_obj);
  js_set_slot(wrapper, SLOT_ENTRIES, ts_obj);

  return obj;
}

void init_compression_stream_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);

  g_cs_proto = js_mkobj(js);
  js_set_getter_desc(js, g_cs_proto, "readable", 8, js_mkfun(js_cs_get_readable), JS_DESC_C);
  js_set_getter_desc(js, g_cs_proto, "writable", 8, js_mkfun(js_cs_get_writable), JS_DESC_C);
  js_set_sym(js, g_cs_proto, get_toStringTag_sym(), js_mkstr(js, "CompressionStream", 17));

  ant_value_t cs_ctor = js_make_ctor(js, js_cs_ctor, g_cs_proto, "CompressionStream", 17);
  js_set(js, g, "CompressionStream", cs_ctor);
  js_set_descriptor(js, g, "CompressionStream", 17, JS_DESC_W | JS_DESC_C);

  g_ds_proto = js_mkobj(js);
  js_set_getter_desc(js, g_ds_proto, "readable", 8, js_mkfun(js_ds_get_readable), JS_DESC_C);
  js_set_getter_desc(js, g_ds_proto, "writable", 8, js_mkfun(js_ds_get_writable), JS_DESC_C);
  js_set_sym(js, g_ds_proto, get_toStringTag_sym(), js_mkstr(js, "DecompressionStream", 19));

  ant_value_t ds_ctor = js_make_ctor(js, js_ds_ctor, g_ds_proto, "DecompressionStream", 19);
  js_set(js, g, "DecompressionStream", ds_ctor);
  js_set_descriptor(js, g, "DecompressionStream", 19, JS_DESC_W | JS_DESC_C);
}

void gc_mark_compression_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  mark(js, g_cs_proto);
  mark(js, g_ds_proto);
}
