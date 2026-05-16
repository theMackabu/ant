#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/symbol.h"
#include "modules/buffer.h"
#include "streams/brotli.h"
#include "streams/compression.h"
#include "streams/transform.h"


enum {
  CS_Z_NATIVE_TAG = 0x43535a53u, // CSZS
  DS_Z_NATIVE_TAG = 0x44535a53u, // DSZS
  
  CS_BROTLI_NATIVE_TAG = 0x43534252u, // CSBR
  DS_BROTLI_NATIVE_TAG = 0x44534252u  // DSBR
};

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
    case ZFMT_BROTLI:      return 15;
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
  if (len == 6 && !memcmp(s, "brotli", 6)) { *out = ZFMT_BROTLI; return 0; }
  return -1;
}

static ant_value_t get_ts(ant_value_t obj) {
  return js_get_slot(obj, SLOT_ENTRIES);
}

bool cs_is_stream(ant_value_t obj) {
  return is_object_type(obj)
    && (js_check_native_tag(obj, CS_Z_NATIVE_TAG) || js_check_native_tag(obj, CS_BROTLI_NATIVE_TAG))
    && ts_is_stream(get_ts(obj));
}

bool ds_is_stream(ant_value_t obj) {
  return is_object_type(obj)
    && (js_check_native_tag(obj, DS_Z_NATIVE_TAG) || js_check_native_tag(obj, DS_BROTLI_NATIVE_TAG))
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
  ant_value_t value = js_obj_from_ptr(obj);
  zstate_t *st = (zstate_t *)js_get_native(value, CS_Z_NATIVE_TAG);
  if (!st) return;
  if (st->initialized) deflateEnd(&st->strm);
  free(st);
  js_clear_native(value, CS_Z_NATIVE_TAG);
}

static void zstate_inflate_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  zstate_t *st = (zstate_t *)js_get_native(value, DS_Z_NATIVE_TAG);
  if (!st) return;
  if (st->initialized) inflateEnd(&st->strm);
  free(st);
  js_clear_native(value, DS_Z_NATIVE_TAG);
}

static void brotli_state_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  uint32_t tag = CS_BROTLI_NATIVE_TAG;
  brotli_stream_state_t *st = (brotli_stream_state_t *)js_get_native(value, tag);
  if (!st) {
    tag = DS_BROTLI_NATIVE_TAG;
    st = (brotli_stream_state_t *)js_get_native(value, tag);
  }
  if (!st) return;
  brotli_stream_state_destroy(st);
  js_clear_native(value, tag);
}

static ant_value_t enqueue_buffer(ant_t *js, ant_value_t ctrl_obj, const uint8_t *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr(js, "out of memory");
  memcpy(ab->data, data, len);
  ant_value_t arr = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Uint8Array");
  if (!ts_is_controller(ctrl_obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStreamDefaultController");
  return ts_ctrl_enqueue(js, ctrl_obj, arr);
}

#define ZCHUNK_SIZE 16384

static ant_value_t cs_transform(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = (nargs > 1) ? args[1] : js_mkundef();
  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  
  const uint8_t *input = NULL;
  size_t input_len = 0;

  if (!is_object_type(chunk) || !buffer_source_get_bytes(js, chunk, &input, &input_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");

  if (!input || input_len == 0) return js_mkundef();

  brotli_stream_state_t *brotli_st = (brotli_stream_state_t *)js_get_native(js->current_func, CS_BROTLI_NATIVE_TAG);
  if (brotli_st)
    return brotli_stream_transform(js, brotli_st, ctrl_obj, input, input_len);
  
  zstate_t *st = (zstate_t *)js_get_native(js->current_func, CS_Z_NATIVE_TAG);
  if (!st) 
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid CompressionStream");

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
      ant_value_t r = enqueue_buffer(js, ctrl_obj, out_buf, have);
      if (is_err(r)) return r;
    }
  } while (st->strm.avail_out == 0);

  return js_mkundef();
}

static ant_value_t cs_flush(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = (nargs > 0) ? args[0] : js_mkundef();

  brotli_stream_state_t *brotli_st = (brotli_stream_state_t *)js_get_native(js->current_func, CS_BROTLI_NATIVE_TAG);
  if (brotli_st)
    return brotli_stream_flush(js, brotli_st, ctrl_obj);
  
  zstate_t *st = (zstate_t *)js_get_native(js->current_func, CS_Z_NATIVE_TAG);
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid CompressionStream");

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
      ant_value_t r = enqueue_buffer(js, ctrl_obj, out_buf, have);
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
  brotli_stream_state_t *brotli = NULL;
  if (fmt == ZFMT_BROTLI) {
    brotli = brotli_stream_state_new(false);
    if (!brotli) return js_mkerr(js, "Failed to initialize compression");
  } else {
    if (!st) return js_mkerr(js, "out of memory");
    st->format = fmt;
    int ret = deflateInit2(&st->strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
      zfmt_window_bits(fmt, false), 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) { free(st); return js_mkerr(js, "Failed to initialize compression"); }
    st->initialized = true;
  }

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.cs_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  
  js_set_native(obj, fmt == ZFMT_BROTLI ? (void *)brotli : (void *)st, fmt == ZFMT_BROTLI ? CS_BROTLI_NATIVE_TAG : CS_Z_NATIVE_TAG);
  js_set_finalizer(obj, fmt == ZFMT_BROTLI ? brotli_state_finalize : zstate_finalize);

  ant_value_t transformer = js_mkobj(js);
  
  ant_value_t transform_fn = js_heavy_mkfun_native(js, cs_transform, fmt == ZFMT_BROTLI 
    ? (void *)brotli : (void *)st, fmt == ZFMT_BROTLI
    ? CS_BROTLI_NATIVE_TAG : CS_Z_NATIVE_TAG
  );
  
  ant_value_t flush_fn = js_heavy_mkfun_native(js, cs_flush, fmt == ZFMT_BROTLI 
    ? (void *)brotli : (void *)st, fmt == ZFMT_BROTLI 
    ? CS_BROTLI_NATIVE_TAG : CS_Z_NATIVE_TAG
  );
  
  js_set(js, transformer, "transform", transform_fn);
  js_set(js, transformer, "flush", flush_fn);

  ant_value_t ctor_args[1] = { transformer };
  ant_value_t saved_new_target = js->new_target;
  ant_value_t saved_this = js->this_val;
  js->new_target = js_mknum(1);

  ant_value_t ts_obj = js_ts_ctor(js, ctor_args, 1);
  js->new_target = saved_new_target;
  js->this_val = saved_this;

  if (is_err(ts_obj)) {
    if (fmt == ZFMT_BROTLI) brotli_stream_state_destroy(brotli);
    else { deflateEnd(&st->strm); free(st); }
    return ts_obj;
  }
  js_set_slot(obj, SLOT_ENTRIES, ts_obj);
  js_set_slot_wb(js, transform_fn, SLOT_ENTRIES, obj);
  js_set_slot_wb(js, flush_fn, SLOT_ENTRIES, obj);

  return obj;
}

static ant_value_t ds_transform(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = (nargs > 1) ? args[1] : js_mkundef();

  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  const uint8_t *input = NULL;
  size_t input_len = 0;

  if (!is_object_type(chunk) || !buffer_source_get_bytes(js, chunk, &input, &input_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");

  if (!input || input_len == 0) return js_mkundef();
  brotli_stream_state_t *brotli_st = (brotli_stream_state_t *)js_get_native(js->current_func, DS_BROTLI_NATIVE_TAG);
  if (brotli_st)
    return brotli_stream_transform(js, brotli_st, ctrl_obj, input, input_len);
  
  zstate_t *st = (zstate_t *)js_get_native(js->current_func, DS_Z_NATIVE_TAG);
  if (!st)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid DecompressionStream");

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
      ant_value_t r = enqueue_buffer(js, ctrl_obj, out_buf, have);
      if (is_err(r)) return r;
    }
    if (ret == Z_STREAM_END) break;
  } while (st->strm.avail_out == 0);

  return js_mkundef();
}

static ant_value_t ds_flush(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctrl_obj = (nargs > 0) ? args[0] : js_mkundef();
  brotli_stream_state_t *st = (brotli_stream_state_t *)js_get_native(js->current_func, DS_BROTLI_NATIVE_TAG);
  if (st)
    return brotli_stream_flush(js, st, ctrl_obj);
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
  brotli_stream_state_t *brotli = NULL;
  if (fmt == ZFMT_BROTLI) {
    brotli = brotli_stream_state_new(true);
    if (!brotli) return js_mkerr(js, "Failed to initialize decompression");
  } else {
    if (!st) return js_mkerr(js, "out of memory");
    st->format = fmt;
    int ret = inflateInit2(&st->strm, zfmt_window_bits(fmt, true));
    if (ret != Z_OK) { free(st); return js_mkerr(js, "Failed to initialize decompression"); }
    st->initialized = true;
  }

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.ds_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  
  js_set_native(obj, fmt == ZFMT_BROTLI ? (void *)brotli : (void *)st, fmt == ZFMT_BROTLI ? DS_BROTLI_NATIVE_TAG : DS_Z_NATIVE_TAG);
  js_set_finalizer(obj, fmt == ZFMT_BROTLI ? brotli_state_finalize : zstate_inflate_finalize);

  ant_value_t transformer = js_mkobj(js);
  
  ant_value_t transform_fn = js_heavy_mkfun_native(js, ds_transform, fmt == ZFMT_BROTLI 
    ? (void *)brotli : (void *)st, fmt == ZFMT_BROTLI 
    ? DS_BROTLI_NATIVE_TAG : DS_Z_NATIVE_TAG
  );
  
  ant_value_t flush_fn = js_heavy_mkfun_native(js, ds_flush, fmt == ZFMT_BROTLI 
    ? (void *)brotli : (void *)st, fmt == ZFMT_BROTLI 
    ? DS_BROTLI_NATIVE_TAG : DS_Z_NATIVE_TAG
  );
  
  js_set(js, transformer, "transform", transform_fn);
  js_set(js, transformer, "flush", flush_fn);

  ant_value_t ctor_args[1] = { transformer };
  ant_value_t saved_new_target = js->new_target;
  ant_value_t saved_this = js->this_val;
  js->new_target = js_mknum(1);

  ant_value_t ts_obj = js_ts_ctor(js, ctor_args, 1);

  js->new_target = saved_new_target;
  js->this_val = saved_this;
  if (is_err(ts_obj)) {
    if (fmt == ZFMT_BROTLI) brotli_stream_state_destroy(brotli);
    else { inflateEnd(&st->strm); free(st); }
    return ts_obj;
  }

  js_set_slot(obj, SLOT_ENTRIES, ts_obj);
  js_set_slot_wb(js, transform_fn, SLOT_ENTRIES, obj);
  js_set_slot_wb(js, flush_fn, SLOT_ENTRIES, obj);
  return obj;
}

void init_compression_stream_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);

  js->sym.cs_proto = js_mkobj(js);
  js_set_getter_desc(js, js->sym.cs_proto, "readable", 8, js_mkfun(js_cs_get_readable), JS_DESC_C);
  js_set_getter_desc(js, js->sym.cs_proto, "writable", 8, js_mkfun(js_cs_get_writable), JS_DESC_C);
  js_set_sym(js, js->sym.cs_proto, get_toStringTag_sym(), js_mkstr(js, "CompressionStream", 17));

  ant_value_t cs_ctor = js_make_ctor(js, js_cs_ctor, js->sym.cs_proto, "CompressionStream", 17);
  js_set(js, g, "CompressionStream", cs_ctor);
  js_set_descriptor(js, g, "CompressionStream", 17, JS_DESC_W | JS_DESC_C);

  js->sym.ds_proto = js_mkobj(js);
  js_set_getter_desc(js, js->sym.ds_proto, "readable", 8, js_mkfun(js_ds_get_readable), JS_DESC_C);
  js_set_getter_desc(js, js->sym.ds_proto, "writable", 8, js_mkfun(js_ds_get_writable), JS_DESC_C);
  js_set_sym(js, js->sym.ds_proto, get_toStringTag_sym(), js_mkstr(js, "DecompressionStream", 19));

  ant_value_t ds_ctor = js_make_ctor(js, js_ds_ctor, js->sym.ds_proto, "DecompressionStream", 19);
  js_set(js, g, "DecompressionStream", ds_ctor);
  js_set_descriptor(js, g, "DecompressionStream", 19, JS_DESC_W | JS_DESC_C);
}

void gc_mark_compression_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  mark(js, js->sym.cs_proto);
  mark(js, js->sym.ds_proto);
}
