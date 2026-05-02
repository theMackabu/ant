#include <compat.h>

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"

#include "silver/engine.h"
#include "streams/brotli.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/symbol.h"
#include "modules/zlib.h"

#define ZLIB_CHUNK      16384
#define ZLIB_STREAM_TAG 0x5A4C4942u

typedef enum {
  ZLIB_KIND_GZIP,
  ZLIB_KIND_GUNZIP,
  ZLIB_KIND_DEFLATE,
  ZLIB_KIND_INFLATE,
  ZLIB_KIND_DEFLATE_RAW,
  ZLIB_KIND_INFLATE_RAW,
  ZLIB_KIND_UNZIP,
  ZLIB_KIND_BROTLI_COMPRESS,
  ZLIB_KIND_BROTLI_DECOMPRESS,
} zlib_kind_t;

typedef struct zlib_stream_s {
  z_stream strm;
  brotli_stream_state_t *brotli;
  ant_value_t obj;
  ant_t *js;
  zlib_kind_t kind;
  uint32_t bytes_written;
  bool initialized;
  bool ended;
  bool destroyed;
  struct zlib_stream_s *next_active;
} zlib_stream_t;

static ant_value_t g_transform_proto = 0;
static zlib_stream_t *g_active_streams = NULL;

static bool zlib_kind_is_compress(zlib_kind_t k) {
  return 
    k == ZLIB_KIND_GZIP 
    || k == ZLIB_KIND_DEFLATE
    || k == ZLIB_KIND_DEFLATE_RAW 
    || k == ZLIB_KIND_BROTLI_COMPRESS;
}

static int zlib_window_bits(zlib_kind_t k) {
  switch (k) {
    case ZLIB_KIND_GZIP:        return 15 + 16;
    case ZLIB_KIND_GUNZIP:      return 15 + 32;
    case ZLIB_KIND_UNZIP:       return 15 + 32;
    case ZLIB_KIND_DEFLATE:     return 15;
    case ZLIB_KIND_INFLATE:     return 15;
    case ZLIB_KIND_DEFLATE_RAW: return -15;
    case ZLIB_KIND_INFLATE_RAW: return -15;
    default:                    return 15;
  }
}

static ant_value_t pick_callback(ant_value_t *args, int nargs) {
  if (nargs > 2 && is_callable(args[2])) return args[2];
  if (nargs > 1 && is_callable(args[1])) return args[1];
  return js_mkundef();
}

static zlib_stream_t *zlib_stream_ptr(ant_value_t obj) {
  if (!js_check_native_tag(obj, ZLIB_STREAM_TAG)) return NULL;
  return (zlib_stream_t *)js_get_native(obj, ZLIB_STREAM_TAG);
}

static void zlib_add_active(zlib_stream_t *st) {
  st->next_active = g_active_streams;
  g_active_streams = st;
}

static void zlib_remove_active(zlib_stream_t *st) {
  zlib_stream_t **it;
  for (it = &g_active_streams; *it; it = &(*it)->next_active) {
  if (*it == st) {
    *it = st->next_active;
    st->next_active = NULL;
    return;
  }}
}

static void zlib_stream_release(zlib_stream_t *st) {
  if (!st) return;
  if (st->brotli) {
    brotli_stream_state_destroy(st->brotli);
    st->brotli = NULL;
  } else if (st->initialized) {
    if (zlib_kind_is_compress(st->kind)) deflateEnd(&st->strm);
    else inflateEnd(&st->strm);
    st->initialized = false;
  }
}

static ant_value_t zlib_make_buffer(ant_t *js, const uint8_t *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr(js, "out of memory");
  if (data && len > 0) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Buffer");
}

static void zlib_emit_data(ant_t *js, ant_value_t obj, const uint8_t *data, size_t len) {
  ant_value_t buf = zlib_make_buffer(js, data, len);
  if (!is_err(buf)) eventemitter_emit_args(js, obj, "data", &buf, 1);
}

typedef struct { ant_t *js; ant_value_t obj; } brotli_emit_ctx_t;
typedef struct { ant_t *js; ant_value_t arr; bool error; } brotli_collect_ctx_t;

static int brotli_emit_cb(void *ctx, const uint8_t *chunk, size_t len) {
  brotli_emit_ctx_t *ec = (brotli_emit_ctx_t *)ctx;
  zlib_emit_data(ec->js, ec->obj, chunk, len);
  return 0;
}

static int brotli_collect_cb(void *ctx, const uint8_t *chunk, size_t len) {
  brotli_collect_ctx_t *ec = (brotli_collect_ctx_t *)ctx;
  ant_value_t buf = zlib_make_buffer(ec->js, chunk, len);
  if (is_err(buf)) { ec->error = true; return -1; }
  js_arr_push(ec->js, ec->arr, buf);
  return 0;
}

static ant_value_t zlib_do_process(
  ant_t *js, zlib_stream_t *st,
  const uint8_t *input, size_t input_len,
  bool finish
) {
  if (st->destroyed || st->ended) return js_mkundef();

  if (st->brotli) {
    brotli_emit_ctx_t ctx = { js, st->obj };
    int rc;
    if (finish) {
      rc = brotli_stream_finish(st->brotli, brotli_emit_cb, &ctx);
    } else {
      rc = brotli_stream_process(st->brotli, input, input_len, brotli_emit_cb, &ctx);
    }
    if (rc < 0) return js_mkerr(js, "brotli operation failed");
    return js_mkundef();
  }

  bool compress = zlib_kind_is_compress(st->kind);
  int flush = finish ? Z_FINISH : Z_NO_FLUSH;
  uint8_t out[ZLIB_CHUNK];

  if (!finish) {
    st->strm.next_in = (Bytef *)input;
    st->strm.avail_in = (uInt)input_len;
  } else {
    st->strm.next_in = NULL;
    st->strm.avail_in = 0;
  }

  int ret;
  do {
    st->strm.next_out = out;
    st->strm.avail_out = ZLIB_CHUNK;

    if (compress) {
      ret = deflate(&st->strm, flush);
      if (ret == Z_STREAM_ERROR) return js_mkerr(js, "zlib deflate error");
    } else {
      ret = inflate(&st->strm, flush == Z_FINISH ? Z_SYNC_FLUSH : flush);
      if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
        return js_mkerr(js, "zlib inflate error");
    }

    size_t have = ZLIB_CHUNK - st->strm.avail_out;
    if (have > 0) zlib_emit_data(js, st->obj, out, have);
    if (ret == Z_STREAM_END) break;
  } while (st->strm.avail_out == 0);

  return js_mkundef();
}

static ant_value_t zlib_process_chunk_sync(
  ant_t *js, zlib_stream_t *st,
  const uint8_t *input, size_t input_len,
  int flush_flag
) {
  ant_value_t arr = js_mkarr(js);

  if (st->brotli) {
    brotli_collect_ctx_t ctx = { js, arr, false };
    int rc = 0;
    if (input && input_len > 0)
      rc = brotli_stream_process(st->brotli, input, input_len, brotli_collect_cb, &ctx);
    if (rc >= 0 && flush_flag == Z_FINISH)
      rc = brotli_stream_finish(st->brotli, brotli_collect_cb, &ctx);
    if (rc < 0 || ctx.error) return js_mkerr(js, "brotli operation failed");
    return arr;
  }

  bool compress = zlib_kind_is_compress(st->kind);
  uint8_t out[ZLIB_CHUNK];

  st->strm.next_in  = input ? (Bytef *)input : NULL;
  st->strm.avail_in = input ? (uInt)input_len : 0;

  int ret;
  do {
    st->strm.next_out  = out;
    st->strm.avail_out = ZLIB_CHUNK;

    if (compress) {
      ret = deflate(&st->strm, flush_flag);
      if (ret == Z_STREAM_ERROR) return js_mkerr(js, "zlib deflate error");
    } else {
      ret = inflate(&st->strm, flush_flag);
      if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
        return js_mkerr(js, "zlib inflate error");
    }

    size_t have = ZLIB_CHUNK - st->strm.avail_out;
    if (have > 0) {
      ant_value_t buf = zlib_make_buffer(js, out, have);
      if (is_err(buf)) return buf;
      js_arr_push(js, arr, buf);
    }
    if (ret == Z_STREAM_END) break;
  } while (st->strm.avail_out == 0);

  return arr;
}

static ant_value_t js_zlib_process_chunk(ant_t *js, ant_value_t *args, int nargs) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  if (st->destroyed || st->ended) return js_mkarr(js);

  const uint8_t *input = NULL;
  size_t input_len = 0;
  if (nargs >= 1 && is_object_type(args[0]))
    buffer_source_get_bytes(js, args[0], &input, &input_len);

  int flush_flag = Z_NO_FLUSH;
  if (nargs >= 2 && vtype(args[1]) == T_NUM)
    flush_flag = (int)js_getnum(args[1]);

  return zlib_process_chunk_sync(js, st, input, input_len, flush_flag);
}

static ant_value_t js_zlib_write(ant_t *js, ant_value_t *args, int nargs) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  if (st->destroyed) return js_false;
  if (st->ended) return js_false;

  if (nargs < 1 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL)
    return js_true;

  const uint8_t *bytes = NULL;
  size_t len = 0;

  if (is_object_type(args[0])) {
    buffer_source_get_bytes(js, args[0], &bytes, &len);
  }

  if (!bytes) {
    size_t slen = 0;
    char *s = js_getstr(js, args[0], &slen);
    if (s) { bytes = (const uint8_t *)s; len = slen; }
  }

  if (!bytes || len == 0) return js_true;
  st->bytes_written += (uint32_t)len;

  ant_value_t r = zlib_do_process(js, st, bytes, len, false);
  if (is_err(r)) {
    eventemitter_emit_args(js, st->obj, "error", &r, 1);
    return js_false;
  }

  ant_value_t cb = pick_callback(args, nargs);
  if (is_callable(cb)) {
    ant_value_t null_val = js_mknull();
    sv_vm_call(js->vm, js, cb, js_mkundef(), &null_val, 1, NULL, false);
  }

  return js_true;
}

static ant_value_t js_zlib_end(ant_t *js, ant_value_t *args, int nargs) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  ant_value_t self = js_getthis(js);

  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  if (st->destroyed || st->ended) return self;

  if (nargs > 0 && vtype(args[0]) != T_UNDEF && vtype(args[0]) != T_NULL) {
    ant_value_t write_args[1] = { args[0] };
    js_zlib_write(js, write_args, 1);
  }

  st->ended = true;
  ant_value_t r = zlib_do_process(js, st, NULL, 0, true);

  if (is_err(r)) {
    eventemitter_emit_args(js, st->obj, "error", &r, 1);
    return self;
  }

  eventemitter_emit_args(js, st->obj, "end", NULL, 0);
  eventemitter_emit_args(js, st->obj, "finish", NULL, 0);
  eventemitter_emit_args(js, st->obj, "close", NULL, 0);

  ant_value_t cb = pick_callback(args, nargs);
  if (is_callable(cb))
    sv_vm_call(js->vm, js, cb, js_mkundef(), NULL, 0, NULL, false);

  return self;
}

static ant_value_t js_zlib_destroy(ant_t *js, ant_value_t *args, int nargs) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  ant_value_t self = js_getthis(js);

  if (!st || st->destroyed) return self;
  st->destroyed = true;
  zlib_stream_release(st);

  if (nargs > 0 && !js_truthy(js, args[0])) {
    ant_value_t null_val = js_mknull();
    eventemitter_emit_args(js, st->obj, "error", &null_val, 1);
  }

  eventemitter_emit_args(js, st->obj, "close", NULL, 0);
  return self;
}

static ant_value_t js_zlib_pause(ant_t *js, ant_value_t *args, int nargs) {
  return js_getthis(js);
}

static ant_value_t js_zlib_resume(ant_t *js, ant_value_t *args, int nargs) {
  return js_getthis(js);
}

static ant_value_t js_zlib_unpipe(ant_t *js, ant_value_t *args, int nargs) {
  return js_getthis(js);
}

static ant_value_t pipe_on_data(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t dest = js_get_slot(fn, SLOT_DATA);
  ant_value_t write_fn = js_get(js, dest, "write");
  
  if (is_callable(write_fn) && nargs > 0)
    sv_vm_call(js->vm, js, write_fn, dest, args, 1, NULL, false);
    
  return js_mkundef();
}

static ant_value_t pipe_on_end(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t fn = js_getcurrentfunc(js);
  ant_value_t dest = js_get_slot(fn, SLOT_DATA);
  ant_value_t end_fn = js_get(js, dest, "end");
  
  if (is_callable(end_fn))
    sv_vm_call(js->vm, js, end_fn, dest, NULL, 0, NULL, false);
    
  return js_mkundef();
}

static ant_value_t js_zlib_pipe(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) return js_mkundef();
  ant_value_t self = js_getthis(js);
  ant_value_t dest = args[0];

  ant_value_t data_handler = js_heavy_mkfun(js, pipe_on_data, dest);
  ant_value_t end_handler = js_heavy_mkfun(js, pipe_on_end, dest);

  eventemitter_add_listener(js, self, "data", data_handler, false);
  eventemitter_add_listener(js, self, "end", end_handler, true);

  return dest;
}

static ant_value_t js_zlib_get_bytes_written(ant_t *js, ant_value_t *args, int nargs) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mknum(0);
  return js_mknum((double)st->bytes_written);
}

static ant_value_t zlib_create_stream(ant_t *js, zlib_kind_t kind) {
  bool compress = zlib_kind_is_compress(kind);

  zlib_stream_t *st = calloc(1, sizeof(zlib_stream_t));
  if (!st) return js_mkerr(js, "out of memory");

  st->kind = kind;
  st->js = js;

  if (kind == ZLIB_KIND_BROTLI_COMPRESS || kind == ZLIB_KIND_BROTLI_DECOMPRESS) {
    st->brotli = brotli_stream_state_new(kind == ZLIB_KIND_BROTLI_DECOMPRESS);
    if (!st->brotli) { free(st); return js_mkerr(js, "brotli init failed"); }
    st->initialized = true;
  } else {
    int wbits = zlib_window_bits(kind);
    int ret;
    if (compress) {
      ret = deflateInit2(&st->strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY);
    } else ret = inflateInit2(&st->strm, wbits);

    if (ret != Z_OK) { free(st); return js_mkerr(js, "zlib init failed"); }
    st->initialized = true;
  }

  ant_value_t obj = js_mkobj(js);
  if (is_object_type(g_transform_proto)) js_set_proto_init(obj, g_transform_proto);

  js_set_native(obj, st, ZLIB_STREAM_TAG);

  js_set(js, obj, "readable", js_true);
  js_set(js, obj, "writable", js_true);
  js_set(js, obj, "_processChunk", js_mkfun(js_zlib_process_chunk));

  ant_value_t handle_obj = js_mkobj(js);
  js_set(js, handle_obj, "close", js_mkfun(js_zlib_destroy));
  js_set(js, obj, "_handle", handle_obj);

  st->obj = obj;
  zlib_add_active(st);
  return obj;
}

static ant_value_t js_create_gzip(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_GZIP);
}

static ant_value_t js_create_gunzip(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_GUNZIP);
}

static ant_value_t js_create_deflate(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_DEFLATE);
}

static ant_value_t js_create_inflate(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_INFLATE);
}

static ant_value_t js_create_deflate_raw(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_DEFLATE_RAW);
}

static ant_value_t js_create_inflate_raw(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_INFLATE_RAW);
}

static ant_value_t js_create_unzip(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_UNZIP);
}

static ant_value_t js_create_brotli_compress(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_BROTLI_COMPRESS);
}

static ant_value_t js_create_brotli_decompress(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_BROTLI_DECOMPRESS);
}

typedef struct { 
  uint8_t *data;
  size_t len;
  size_t cap;
  int error;
} zbuf_t;

static int zbuf_append(zbuf_t *b, const uint8_t *chunk, size_t n) {
  if (b->len + n > b->cap) {
    size_t newcap = b->cap ? b->cap * 2 : 4096;
    while (newcap < b->len + n) newcap *= 2;
    uint8_t *p = realloc(b->data, newcap);
    
    if (!p) { b->error = 1; return -1; }
    b->data = p;
    b->cap = newcap;
  }
  
  memcpy(b->data + b->len, chunk, n);
  b->len += n;
  
  return 0;
}

static int zbuf_brotli_cb(void *ctx, const uint8_t *chunk, size_t n) {
  return zbuf_append((zbuf_t *)ctx, chunk, n);
}

static ant_value_t zlib_sync_op(
  ant_t *js, zlib_kind_t kind,
  const uint8_t *input, size_t input_len
) {
  zbuf_t out = {0};
  uint8_t tmp[ZLIB_CHUNK];
  bool compress = zlib_kind_is_compress(kind);

  if (kind == ZLIB_KIND_BROTLI_COMPRESS || kind == ZLIB_KIND_BROTLI_DECOMPRESS) {
    brotli_stream_state_t *bs = brotli_stream_state_new(kind == ZLIB_KIND_BROTLI_DECOMPRESS);
    if (!bs) return js_mkerr(js, "brotli init failed");
    
    int rc = brotli_stream_process(bs, input, input_len, zbuf_brotli_cb, &out);
    if (rc >= 0) rc = brotli_stream_finish(bs, zbuf_brotli_cb, &out);
    
    brotli_stream_state_destroy(bs);
    if (rc < 0 || out.error) { free(out.data); return js_mkerr(js, "brotli operation failed"); }
    ant_value_t buf = zlib_make_buffer(js, out.data, out.len);
    free(out.data);
    
    return buf;
  }

  z_stream strm = {0};
  int wbits = zlib_window_bits(kind);
  int ret;

  if (compress) ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY);
  else ret = inflateInit2(&strm, wbits);
  if (ret != Z_OK) return js_mkerr(js, "zlib init failed");

  strm.next_in = (Bytef *)input;
  strm.avail_in = (uInt)input_len;

  do {
    strm.next_out = tmp;
    strm.avail_out = ZLIB_CHUNK;
    if (compress) ret = deflate(&strm, Z_FINISH);
    else ret = inflate(&strm, Z_SYNC_FLUSH);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
      if (compress) deflateEnd(&strm);
      else inflateEnd(&strm);
      free(out.data);
      return js_mkerr(js, "zlib operation failed");
    }
    size_t have = ZLIB_CHUNK - strm.avail_out;
    if (have > 0 && zbuf_append(&out, tmp, have) < 0) {
      if (compress) deflateEnd(&strm);
      else inflateEnd(&strm);
      free(out.data);
      return js_mkerr(js, "out of memory");
    }
    if (ret == Z_STREAM_END) break;
  } while (strm.avail_out == 0 || strm.avail_in > 0);

  if (compress) deflateEnd(&strm);
  else inflateEnd(&strm);

  ant_value_t buf = zlib_make_buffer(js, out.data, out.len);
  free(out.data);
  return buf;
}

static bool get_input_bytes(ant_t *js, ant_value_t val, const uint8_t **out_bytes, size_t *out_len) {
  if (is_object_type(val) && buffer_source_get_bytes(js, val, out_bytes, out_len)) return true;
  size_t slen = 0;
  char *s = js_getstr(js, val, &slen);
  if (s) { *out_bytes = (const uint8_t *)s; *out_len = slen; return true; }
  return false;
}

static ant_value_t zlib_sync_fn(ant_t *js, ant_value_t *args, int nargs, zlib_kind_t kind) {
  if (nargs < 1) return js_mkerr(js, "argument required");
  const uint8_t *bytes = NULL;
  size_t len = 0;
  if (!get_input_bytes(js, args[0], &bytes, &len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "argument must be a string or Buffer");
  return zlib_sync_op(js, kind, bytes, len);
}

static ant_value_t zlib_async_fn(ant_t *js, ant_value_t *args, int nargs, zlib_kind_t kind) {
  if (nargs < 1) return js_mkerr(js, "argument required");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t cb = pick_callback(args, nargs);

  if (!get_input_bytes(js, args[0], &bytes, &len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "argument must be a string or Buffer");

  ant_value_t result = zlib_sync_op(js, kind, bytes, len);

  if (is_callable(cb)) {
    if (is_err(result)) {
      ant_value_t argv[1] = { result };
      sv_vm_call(js->vm, js, cb, js_mkundef(), argv, 1, NULL, false);
    } else {
      ant_value_t null_val = js_mknull();
      ant_value_t argv[2] = { null_val, result };
      sv_vm_call(js->vm, js, cb, js_mkundef(), argv, 2, NULL, false);
    }
    return js_mkundef();
  }

  return result;
}

#define ZLIB_SYNC_FN(name, kind) \
  static ant_value_t js_##name##Sync(ant_t *js, ant_value_t *a, int n) { return zlib_sync_fn(js, a, n, kind); }

#define ZLIB_ASYNC_FN(name, kind) \
  static ant_value_t js_##name(ant_t *js, ant_value_t *a, int n) { return zlib_async_fn(js, a, n, kind); }

ZLIB_SYNC_FN(gzip,             ZLIB_KIND_GZIP)
ZLIB_SYNC_FN(gunzip,           ZLIB_KIND_GUNZIP)
ZLIB_SYNC_FN(deflate,          ZLIB_KIND_DEFLATE)
ZLIB_SYNC_FN(inflate,          ZLIB_KIND_INFLATE)
ZLIB_SYNC_FN(deflateRaw,       ZLIB_KIND_DEFLATE_RAW)
ZLIB_SYNC_FN(inflateRaw,       ZLIB_KIND_INFLATE_RAW)
ZLIB_SYNC_FN(unzip,            ZLIB_KIND_UNZIP)
ZLIB_SYNC_FN(brotliCompress,   ZLIB_KIND_BROTLI_COMPRESS)
ZLIB_SYNC_FN(brotliDecompress, ZLIB_KIND_BROTLI_DECOMPRESS)

ZLIB_ASYNC_FN(gzip,             ZLIB_KIND_GZIP)
ZLIB_ASYNC_FN(gunzip,           ZLIB_KIND_GUNZIP)
ZLIB_ASYNC_FN(deflate,          ZLIB_KIND_DEFLATE)
ZLIB_ASYNC_FN(inflate,          ZLIB_KIND_INFLATE)
ZLIB_ASYNC_FN(deflateRaw,       ZLIB_KIND_DEFLATE_RAW)
ZLIB_ASYNC_FN(inflateRaw,       ZLIB_KIND_INFLATE_RAW)
ZLIB_ASYNC_FN(unzip,            ZLIB_KIND_UNZIP)
ZLIB_ASYNC_FN(brotliCompress,   ZLIB_KIND_BROTLI_COMPRESS)
ZLIB_ASYNC_FN(brotliDecompress, ZLIB_KIND_BROTLI_DECOMPRESS)

static ant_value_t js_zlib_crc32(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "argument required");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  uLong init_val = 0;

  if (nargs >= 2 && vtype(args[1]) == T_NUM)
    init_val = (uLong)js_getnum(args[1]);

  if (!get_input_bytes(js, args[0], &bytes, &len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "argument must be a string or Buffer");

  uLong crc = crc32(init_val, bytes, (uInt)len);
  return js_mknum((double)(uint32_t)crc);
}

static ant_value_t make_constants(ant_t *js) {
  ant_value_t c = js_mkobj(js);

  js_set(js, c, "Z_NO_FLUSH",          js_mknum(Z_NO_FLUSH));
  js_set(js, c, "Z_PARTIAL_FLUSH",     js_mknum(Z_PARTIAL_FLUSH));
  js_set(js, c, "Z_SYNC_FLUSH",        js_mknum(Z_SYNC_FLUSH));
  js_set(js, c, "Z_FULL_FLUSH",        js_mknum(Z_FULL_FLUSH));
  js_set(js, c, "Z_FINISH",            js_mknum(Z_FINISH));
  js_set(js, c, "Z_BLOCK",             js_mknum(Z_BLOCK));

  js_set(js, c, "Z_OK",                js_mknum(Z_OK));
  js_set(js, c, "Z_STREAM_END",        js_mknum(Z_STREAM_END));
  js_set(js, c, "Z_NEED_DICT",         js_mknum(Z_NEED_DICT));
  js_set(js, c, "Z_ERRNO",             js_mknum(Z_ERRNO));
  js_set(js, c, "Z_STREAM_ERROR",      js_mknum(Z_STREAM_ERROR));
  js_set(js, c, "Z_DATA_ERROR",        js_mknum(Z_DATA_ERROR));
  js_set(js, c, "Z_MEM_ERROR",         js_mknum(Z_MEM_ERROR));
  js_set(js, c, "Z_BUF_ERROR",         js_mknum(Z_BUF_ERROR));
  js_set(js, c, "Z_VERSION_ERROR",     js_mknum(Z_VERSION_ERROR));

  js_set(js, c, "Z_NO_COMPRESSION",    js_mknum(Z_NO_COMPRESSION));
  js_set(js, c, "Z_BEST_SPEED",        js_mknum(Z_BEST_SPEED));
  js_set(js, c, "Z_BEST_COMPRESSION",  js_mknum(Z_BEST_COMPRESSION));
  js_set(js, c, "Z_DEFAULT_COMPRESSION", js_mknum(Z_DEFAULT_COMPRESSION));

  js_set(js, c, "Z_FILTERED",          js_mknum(Z_FILTERED));
  js_set(js, c, "Z_HUFFMAN_ONLY",      js_mknum(Z_HUFFMAN_ONLY));
  js_set(js, c, "Z_RLE",               js_mknum(Z_RLE));
  js_set(js, c, "Z_FIXED",             js_mknum(Z_FIXED));
  js_set(js, c, "Z_DEFAULT_STRATEGY",  js_mknum(Z_DEFAULT_STRATEGY));

  js_set(js, c, "BROTLI_OPERATION_PROCESS",         js_mknum(0));
  js_set(js, c, "BROTLI_OPERATION_FLUSH",           js_mknum(1));
  js_set(js, c, "BROTLI_OPERATION_FINISH",          js_mknum(2));
  js_set(js, c, "BROTLI_OPERATION_EMIT_METADATA",   js_mknum(3));

  js_set(js, c, "BROTLI_PARAM_MODE",                js_mknum(0));
  js_set(js, c, "BROTLI_MODE_GENERIC",              js_mknum(0));
  js_set(js, c, "BROTLI_MODE_TEXT",                 js_mknum(1));
  js_set(js, c, "BROTLI_MODE_FONT",                 js_mknum(2));
  js_set(js, c, "BROTLI_PARAM_QUALITY",             js_mknum(1));
  js_set(js, c, "BROTLI_MIN_QUALITY",               js_mknum(0));
  js_set(js, c, "BROTLI_MAX_QUALITY",               js_mknum(11));
  js_set(js, c, "BROTLI_DEFAULT_QUALITY",           js_mknum(11));
  js_set(js, c, "BROTLI_PARAM_LGWIN",               js_mknum(2));
  js_set(js, c, "BROTLI_MIN_WINDOW_BITS",           js_mknum(10));
  js_set(js, c, "BROTLI_MAX_WINDOW_BITS",           js_mknum(24));
  js_set(js, c, "BROTLI_DEFAULT_WINDOW",            js_mknum(22));
  js_set(js, c, "BROTLI_PARAM_SIZE_HINT",           js_mknum(3));
  js_set(js, c, "BROTLI_PARAM_LARGE_WINDOW",        js_mknum(4));
  js_set(js, c, "BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION", js_mknum(0));
  js_set(js, c, "BROTLI_DECODER_PARAM_LARGE_WINDOW", js_mknum(1));

  js_set(js, c, "ZSTD_e_continue", js_mknum(0));
  js_set(js, c, "ZSTD_e_flush",    js_mknum(1));
  js_set(js, c, "ZSTD_e_end",      js_mknum(2));

  return c;
}

static void zlib_init_proto(ant_t *js) {
  if (g_transform_proto) return;

  ant_value_t events = events_library(js);
  ant_value_t ee_ctor = js_get(js, events, "EventEmitter");
  ant_value_t ee_proto = js_get(js, ee_ctor, "prototype");

  g_transform_proto = js_mkobj(js);
  js_set_proto_init(g_transform_proto, ee_proto);

  js_set(js, g_transform_proto, "write",    js_mkfun(js_zlib_write));
  js_set(js, g_transform_proto, "end",      js_mkfun(js_zlib_end));
  js_set(js, g_transform_proto, "destroy",  js_mkfun(js_zlib_destroy));
  js_set(js, g_transform_proto, "pause",    js_mkfun(js_zlib_pause));
  js_set(js, g_transform_proto, "resume",   js_mkfun(js_zlib_resume));
  js_set(js, g_transform_proto, "unpipe",   js_mkfun(js_zlib_unpipe));
  js_set(js, g_transform_proto, "pipe",     js_mkfun(js_zlib_pipe));

  js_set_getter_desc(js, g_transform_proto, "bytesWritten", 12,
  js_mkfun(js_zlib_get_bytes_written), JS_DESC_C);
}

static ant_value_t zlib_mkctor(ant_t *js, ant_value_t (*fn)(ant_t *, ant_value_t *, int)) {
  ant_value_t ctor = js_heavy_mkfun(js, fn, js_mkundef());
  js_mark_constructor(ctor, true);
  return ctor;
}

ant_value_t zlib_library(ant_t *js) {
  zlib_init_proto(js);

  ant_value_t lib = js_mkobj(js);
  ant_value_t consts = make_constants(js);

  js_set(js, lib, "gzip", js_mkfun(js_gzip));
  js_set(js, lib, "gzipSync", js_mkfun(js_gzipSync));
  js_set(js, lib, "Gzip", zlib_mkctor(js, js_create_gzip));
  js_set(js, lib, "createGzip", js_mkfun(js_create_gzip));

  js_set(js, lib, "gunzip", js_mkfun(js_gunzip));
  js_set(js, lib, "gunzipSync", js_mkfun(js_gunzipSync));
  js_set(js, lib, "Gunzip", zlib_mkctor(js, js_create_gunzip));
  js_set(js, lib, "createGunzip", js_mkfun(js_create_gunzip));

  js_set(js, lib, "deflate", js_mkfun(js_deflate));
  js_set(js, lib, "deflateSync", js_mkfun(js_deflateSync));
  js_set(js, lib, "Deflate", zlib_mkctor(js, js_create_deflate));
  js_set(js, lib, "createDeflate", js_mkfun(js_create_deflate));

  js_set(js, lib, "inflate", js_mkfun(js_inflate));
  js_set(js, lib, "inflateSync", js_mkfun(js_inflateSync));
  js_set(js, lib, "Inflate", zlib_mkctor(js, js_create_inflate));
  js_set(js, lib, "createInflate", js_mkfun(js_create_inflate));

  js_set(js, lib, "deflateRaw", js_mkfun(js_deflateRaw));
  js_set(js, lib, "deflateRawSync", js_mkfun(js_deflateRawSync));
  js_set(js, lib, "DeflateRaw", zlib_mkctor(js, js_create_deflate_raw));
  js_set(js, lib, "createDeflateRaw", js_mkfun(js_create_deflate_raw));

  js_set(js, lib, "inflateRaw", js_mkfun(js_inflateRaw));
  js_set(js, lib, "inflateRawSync", js_mkfun(js_inflateRawSync));
  js_set(js, lib, "InflateRaw", zlib_mkctor(js, js_create_inflate_raw));
  js_set(js, lib, "createInflateRaw", js_mkfun(js_create_inflate_raw));

  js_set(js, lib, "unzip", js_mkfun(js_unzip));
  js_set(js, lib, "unzipSync", js_mkfun(js_unzipSync));
  js_set(js, lib, "Unzip", zlib_mkctor(js, js_create_unzip));
  js_set(js, lib, "createUnzip", js_mkfun(js_create_unzip));

  js_set(js, lib, "brotliCompress", js_mkfun(js_brotliCompress));
  js_set(js, lib, "brotliCompressSync", js_mkfun(js_brotliCompressSync));
  js_set(js, lib, "BrotliCompress", zlib_mkctor(js, js_create_brotli_compress));
  js_set(js, lib, "createBrotliCompress", js_mkfun(js_create_brotli_compress));

  js_set(js, lib, "brotliDecompress", js_mkfun(js_brotliDecompress));
  js_set(js, lib, "brotliDecompressSync", js_mkfun(js_brotliDecompressSync));
  js_set(js, lib, "BrotliDecompress", zlib_mkctor(js, js_create_brotli_decompress));
  js_set(js, lib, "createBrotliDecompress", js_mkfun(js_create_brotli_decompress));
  
  js_set(js, lib, "default", lib);
  js_set(js, lib, "constants", consts);
  js_set(js, lib, "crc32", js_mkfun(js_zlib_crc32));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "zlib", 4));
  
  return lib;
}

void gc_mark_zlib(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  if (g_transform_proto) mark(js, g_transform_proto);
  for (zlib_stream_t *st = g_active_streams; st; st = st->next_active) mark(js, st->obj);
}
