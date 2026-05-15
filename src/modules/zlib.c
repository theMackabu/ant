#include <compat.h>

#include <math.h>
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
  ZLIB_KIND_COUNT,
} zlib_kind_t;

typedef struct {
  int level;
  int window_bits;
  int mem_level;
  int strategy;
  int chunk_size;
  int finish_flush;
} zlib_options_t;

typedef struct zlib_stream_s {
  z_stream strm;
  brotli_stream_state_t *brotli;
  ant_value_t obj;
  ant_t *js;
  zlib_kind_t kind;
  zlib_options_t opts;
  uint32_t bytes_written;
  bool initialized;
  bool ended;
  bool destroyed;
  struct zlib_stream_s *next_active;
} zlib_stream_t;

static ant_value_t g_transform_proto = 0;
static ant_value_t g_zlib_protos[ZLIB_KIND_COUNT] = {0};
static zlib_stream_t *g_active_streams = NULL;

static ant_value_t js_zlib_destroy(ant_t *js, ant_value_t *args, int nargs);

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

static zlib_options_t zlib_default_options(zlib_kind_t kind) {
  return (zlib_options_t){
    .level = Z_DEFAULT_COMPRESSION,
    .window_bits = zlib_window_bits(kind),
    .mem_level = 8,
    .strategy = Z_DEFAULT_STRATEGY,
    .chunk_size = ZLIB_CHUNK,
    .finish_flush = Z_FINISH,
  };
}

static bool zlib_is_brotli_kind(zlib_kind_t kind) {
  return kind == ZLIB_KIND_BROTLI_COMPRESS || kind == ZLIB_KIND_BROTLI_DECOMPRESS;
}

static bool zlib_option_int(ant_t *js, ant_value_t opts, const char *key, int *out) {
  ant_value_t v = js_get(js, opts, key);
  if (vtype(v) == T_UNDEF || vtype(v) == T_NULL) return true;
  if (vtype(v) != T_NUM) return false;
  *out = (int)js_to_number(js, v);
  return true;
}

static ant_value_t zlib_read_options(
  ant_t *js,
  zlib_kind_t kind,
  ant_value_t opts_val,
  zlib_options_t *out
) {
  *out = zlib_default_options(kind);
  if (!is_object_type(opts_val)) return js_mkundef();

  if (!zlib_option_int(js, opts_val, "level", &out->level) ||
      !zlib_option_int(js, opts_val, "windowBits", &out->window_bits) ||
      !zlib_option_int(js, opts_val, "memLevel", &out->mem_level) ||
      !zlib_option_int(js, opts_val, "strategy", &out->strategy) ||
      !zlib_option_int(js, opts_val, "chunkSize", &out->chunk_size) ||
      !zlib_option_int(js, opts_val, "finishFlush", &out->finish_flush)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "zlib options must be numeric");
  }

  if (out->chunk_size < 64)
    return js_mkerr_typed(js, JS_ERR_RANGE, "chunkSize must be >= 64");

  if (zlib_is_brotli_kind(kind)) return js_mkundef();

  if (out->level < Z_DEFAULT_COMPRESSION || out->level > Z_BEST_COMPRESSION)
    return js_mkerr_typed(js, JS_ERR_RANGE, "level out of range");
  if (out->mem_level < 1 || out->mem_level > 9)
    return js_mkerr_typed(js, JS_ERR_RANGE, "memLevel out of range");

  if (kind == ZLIB_KIND_GZIP && out->window_bits > 0 && out->window_bits <= 15)
    out->window_bits += 16;
  else if ((kind == ZLIB_KIND_GUNZIP || kind == ZLIB_KIND_UNZIP) &&
           out->window_bits > 0 && out->window_bits <= 15)
    out->window_bits += 32;
  else if ((kind == ZLIB_KIND_DEFLATE_RAW || kind == ZLIB_KIND_INFLATE_RAW) &&
           out->window_bits > 0)
    out->window_bits = -out->window_bits;

  int base_window = out->window_bits < 0 ? -out->window_bits : out->window_bits;
  if (base_window > 32) base_window -= 32;
  if (base_window > 16) base_window -= 16;
  if (base_window < 8 || base_window > 15)
    return js_mkerr_typed(js, JS_ERR_RANGE, "windowBits out of range");

  return js_mkundef();
}

static ant_value_t pick_callback(ant_value_t *args, int nargs) {
  if (nargs > 2 && is_callable(args[2])) return args[2];
  if (nargs > 1 && is_callable(args[1])) return args[1];
  return js_mkundef();
}

static zlib_stream_t *zlib_stream_ptr(ant_value_t obj) {
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
  int flush
) {
  if (st->destroyed || st->ended) return js_mkundef();

  if (st->brotli) {
    brotli_emit_ctx_t ctx = { js, st->obj };
    int rc;
    if (flush == Z_FINISH) {
      rc = brotli_stream_finish(st->brotli, brotli_emit_cb, &ctx);
    } else {
      rc = brotli_stream_process(st->brotli, input, input_len, brotli_emit_cb, &ctx);
    }
    if (rc < 0) return js_mkerr(js, "brotli operation failed");
    return js_mkundef();
  }

  bool compress = zlib_kind_is_compress(st->kind);
  size_t chunk_size = (st->opts.chunk_size > 0) ? (size_t)st->opts.chunk_size : ZLIB_CHUNK;
  uint8_t *out = malloc(chunk_size);
  if (!out) return js_mkerr(js, "out of memory");

  st->strm.next_in = input ? (Bytef *)input : NULL;
  st->strm.avail_in = input ? (uInt)input_len : 0;

  int ret;
  ant_value_t result = js_mkundef();
  do {
    st->strm.next_out = out;
    st->strm.avail_out = (uInt)chunk_size;

    if (compress) {
      ret = deflate(&st->strm, flush);
      if (ret == Z_STREAM_ERROR) { result = js_mkerr(js, "zlib deflate error"); break; }
    } else {
      ret = inflate(&st->strm, flush);
      if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        result = js_mkerr(js, "zlib inflate error");
        break;
      }
    }

    size_t have = chunk_size - st->strm.avail_out;
    if (have > 0) zlib_emit_data(js, st->obj, out, have);
    if (ret == Z_STREAM_END) break;
  } while (st->strm.avail_out == 0);

  free(out);
  return result;
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
  size_t chunk_size = (st->opts.chunk_size > 0) ? (size_t)st->opts.chunk_size : ZLIB_CHUNK;
  uint8_t *out = malloc(chunk_size);
  if (!out) return js_mkerr(js, "out of memory");

  st->strm.next_in  = input ? (Bytef *)input : NULL;
  st->strm.avail_in = input ? (uInt)input_len : 0;

  int ret;
  ant_value_t result = arr;
  do {
    st->strm.next_out  = out;
    st->strm.avail_out = (uInt)chunk_size;

    if (compress) {
      ret = deflate(&st->strm, flush_flag);
      if (ret == Z_STREAM_ERROR) { result = js_mkerr(js, "zlib deflate error"); break; }
    } else {
      ret = inflate(&st->strm, flush_flag);
      if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        result = js_mkerr(js, "zlib inflate error");
        break;
      }
    }

    size_t have = chunk_size - st->strm.avail_out;
    if (have > 0) {
      ant_value_t buf = zlib_make_buffer(js, out, have);
      if (is_err(buf)) { result = buf; break; }
      js_arr_push(js, arr, buf);
    }
    if (ret == Z_STREAM_END) break;
  } while (st->strm.avail_out == 0);

  free(out);
  return result;
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

  ant_value_t r = zlib_do_process(js, st, bytes, len, Z_NO_FLUSH);
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

  ant_value_t r = zlib_do_process(js, st, NULL, 0, Z_FINISH);

  if (is_err(r)) {
    eventemitter_emit_args(js, st->obj, "error", &r, 1);
    return self;
  }

  st->ended = true;
  eventemitter_emit_args(js, st->obj, "end", NULL, 0);
  eventemitter_emit_args(js, st->obj, "finish", NULL, 0);
  eventemitter_emit_args(js, st->obj, "close", NULL, 0);

  ant_value_t cb = pick_callback(args, nargs);
  if (is_callable(cb))
    sv_vm_call(js->vm, js, cb, js_mkundef(), NULL, 0, NULL, false);

  return self;
}

static ant_value_t js_zlib_flush(ant_t *js, ant_value_t *args, int nargs) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  ant_value_t self = js_getthis(js);
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  if (st->destroyed || st->ended) return self;

  int flush = st->brotli ? 1 : Z_FULL_FLUSH;
  ant_value_t cb = js_mkundef();
  if (nargs > 0 && is_callable(args[0])) cb = args[0];
  else {
    if (nargs > 0 && vtype(args[0]) == T_NUM) flush = (int)js_getnum(args[0]);
    if (nargs > 1 && is_callable(args[1])) cb = args[1];
  }

  ant_value_t r = st->brotli ? js_mkundef() : zlib_do_process(js, st, NULL, 0, flush);
  if (is_err(r)) {
    eventemitter_emit_args(js, st->obj, "error", &r, 1);
    if (is_callable(cb)) {
      ant_value_t argv[1] = { r };
      sv_vm_call(js->vm, js, cb, js_mkundef(), argv, 1, NULL, false);
    }
    return self;
  }

  if (is_callable(cb)) {
    ant_value_t null_val = js_mknull();
    sv_vm_call(js->vm, js, cb, js_mkundef(), &null_val, 1, NULL, false);
  }
  return self;
}

static ant_value_t js_zlib_reset(ant_t *js, ant_value_t *args, int nargs) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  if (st->destroyed) return js_getthis(js);

  if (st->brotli) {
    bool decompress = st->kind == ZLIB_KIND_BROTLI_DECOMPRESS;
    brotli_stream_state_destroy(st->brotli);
    st->brotli = brotli_stream_state_new(decompress);
    if (!st->brotli) return js_mkerr(js, "brotli init failed");
  } else if (zlib_kind_is_compress(st->kind)) {
    if (deflateReset(&st->strm) != Z_OK) return js_mkerr(js, "zlib reset failed");
  } else if (inflateReset(&st->strm) != Z_OK) return js_mkerr(js, "zlib reset failed");

  st->ended = false;
  st->bytes_written = 0;
  return js_getthis(js);
}

static ant_value_t js_zlib_params(ant_t *js, ant_value_t *args, int nargs) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  if (!zlib_kind_is_compress(st->kind) || st->brotli)
    return js_mkerr_typed(js, JS_ERR_TYPE, "params() is only available for deflate streams");
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "level and strategy are required");

  int level = (int)js_to_number(js, args[0]);
  int strategy = (int)js_to_number(js, args[1]);
  if (deflateParams(&st->strm, level, strategy) != Z_OK)
    return js_mkerr(js, "zlib params failed");

  ant_value_t cb = nargs > 2 && is_callable(args[2]) ? args[2] : js_mkundef();
  if (is_callable(cb)) {
    ant_value_t null_val = js_mknull();
    sv_vm_call(js->vm, js, cb, js_mkundef(), &null_val, 1, NULL, false);
  }
  return js_getthis(js);
}

static ant_value_t js_zlib_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js_zlib_destroy(js, NULL, 0);
  if (nargs > 0 && is_callable(args[0]))
    sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, false);
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

static ant_value_t zlib_create_stream(ant_t *js, zlib_kind_t kind, ant_value_t opts_val) {
  bool compress = zlib_kind_is_compress(kind);
  zlib_options_t opts;
  ant_value_t opt_err = zlib_read_options(js, kind, opts_val, &opts);
  if (is_err(opt_err)) return opt_err;

  zlib_stream_t *st = calloc(1, sizeof(zlib_stream_t));
  if (!st) return js_mkerr(js, "out of memory");

  st->kind = kind;
  st->js = js;
  st->opts = opts;

  if (kind == ZLIB_KIND_BROTLI_COMPRESS || kind == ZLIB_KIND_BROTLI_DECOMPRESS) {
    st->brotli = brotli_stream_state_new(kind == ZLIB_KIND_BROTLI_DECOMPRESS);
    if (!st->brotli) { free(st); return js_mkerr(js, "brotli init failed"); }
    st->initialized = true;
  } else {
    int ret;
    if (compress) {
      ret = deflateInit2(&st->strm, opts.level, Z_DEFLATED, opts.window_bits, opts.mem_level, opts.strategy);
    } else ret = inflateInit2(&st->strm, opts.window_bits);

    if (ret != Z_OK) { free(st); return js_mkerr(js, "zlib init failed"); }
    st->initialized = true;
  }

  ant_value_t obj = js_mkobj(js);
  if (kind >= 0 && kind < ZLIB_KIND_COUNT && is_object_type(g_zlib_protos[kind]))
    js_set_proto_init(obj, g_zlib_protos[kind]);
  else if (is_object_type(g_transform_proto)) js_set_proto_init(obj, g_transform_proto);

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
  return zlib_create_stream(js, ZLIB_KIND_GZIP, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_gunzip(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_GUNZIP, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_deflate(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_DEFLATE, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_inflate(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_INFLATE, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_deflate_raw(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_DEFLATE_RAW, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_inflate_raw(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_INFLATE_RAW, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_unzip(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_UNZIP, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_brotli_compress(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_BROTLI_COMPRESS, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_brotli_decompress(ant_t *js, ant_value_t *args, int nargs) {
  return zlib_create_stream(js, ZLIB_KIND_BROTLI_DECOMPRESS, nargs > 0 ? args[0] : js_mkundef());
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
  const uint8_t *input, size_t input_len,
  ant_value_t opts_val
) {
  zbuf_t out = {0};
  bool compress = zlib_kind_is_compress(kind);
  zlib_options_t opts;
  ant_value_t opt_err = zlib_read_options(js, kind, opts_val, &opts);
  if (is_err(opt_err)) return opt_err;
  uint8_t *tmp = malloc((size_t)opts.chunk_size);
  if (!tmp) return js_mkerr(js, "out of memory");

  if (kind == ZLIB_KIND_BROTLI_COMPRESS || kind == ZLIB_KIND_BROTLI_DECOMPRESS) {
    brotli_stream_state_t *bs = brotli_stream_state_new(kind == ZLIB_KIND_BROTLI_DECOMPRESS);
    if (!bs) { free(tmp); return js_mkerr(js, "brotli init failed"); }
    
    int rc = brotli_stream_process(bs, input, input_len, zbuf_brotli_cb, &out);
    if (rc >= 0) rc = brotli_stream_finish(bs, zbuf_brotli_cb, &out);
    
    brotli_stream_state_destroy(bs);
    if (rc < 0 || out.error) { free(tmp); free(out.data); return js_mkerr(js, "brotli operation failed"); }
    ant_value_t buf = zlib_make_buffer(js, out.data, out.len);
    free(tmp);
    free(out.data);
    
    return buf;
  }

  z_stream strm = {0};
  int ret;

  if (compress) ret = deflateInit2(&strm, opts.level, Z_DEFLATED, opts.window_bits, opts.mem_level, opts.strategy);
  else ret = inflateInit2(&strm, opts.window_bits);
  if (ret != Z_OK) { free(tmp); return js_mkerr(js, "zlib init failed"); }

  strm.next_in = (Bytef *)input;
  strm.avail_in = (uInt)input_len;

  do {
    strm.next_out = tmp;
    strm.avail_out = (uInt)opts.chunk_size;
    if (compress) ret = deflate(&strm, Z_FINISH);
    else ret = inflate(&strm, opts.finish_flush);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
      if (compress) deflateEnd(&strm);
      else inflateEnd(&strm);
      free(tmp);
      free(out.data);
      return js_mkerr(js, "zlib operation failed");
    }
    size_t have = (size_t)opts.chunk_size - strm.avail_out;
    if (have > 0 && zbuf_append(&out, tmp, have) < 0) {
      if (compress) deflateEnd(&strm);
      else inflateEnd(&strm);
      free(tmp);
      free(out.data);
      return js_mkerr(js, "out of memory");
    }
    if (ret == Z_STREAM_END) break;
  } while (strm.avail_out == 0 || strm.avail_in > 0);

  if (compress) deflateEnd(&strm);
  else inflateEnd(&strm);

  ant_value_t buf = zlib_make_buffer(js, out.data, out.len);
  free(tmp);
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
  return zlib_sync_op(js, kind, bytes, len, nargs > 1 ? args[1] : js_mkundef());
}

static ant_value_t zlib_async_fn(ant_t *js, ant_value_t *args, int nargs, zlib_kind_t kind) {
  if (nargs < 1) return js_mkerr(js, "argument required");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t cb = pick_callback(args, nargs);

  if (!get_input_bytes(js, args[0], &bytes, &len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "argument must be a string or Buffer");

  ant_value_t opts = (nargs > 1 && is_object_type(args[1])) ? args[1] : js_mkundef();
  ant_value_t result = zlib_sync_op(js, kind, bytes, len, opts);

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

static inline void zlib_set_num(ant_t *js, ant_value_t obj, const char *name, double value) {
  js_set(js, obj, name, js_mknum(value));
}

static void zlib_install_constants(ant_t *js, ant_value_t c) {
  zlib_set_num(js, c, "Z_NO_FLUSH", Z_NO_FLUSH);
  zlib_set_num(js, c, "Z_PARTIAL_FLUSH", Z_PARTIAL_FLUSH);
  zlib_set_num(js, c, "Z_SYNC_FLUSH", Z_SYNC_FLUSH);
  zlib_set_num(js, c, "Z_FULL_FLUSH", Z_FULL_FLUSH);
  zlib_set_num(js, c, "Z_FINISH", Z_FINISH);
  zlib_set_num(js, c, "Z_BLOCK", Z_BLOCK);

  zlib_set_num(js, c, "Z_OK", Z_OK);
  zlib_set_num(js, c, "Z_STREAM_END", Z_STREAM_END);
  zlib_set_num(js, c, "Z_NEED_DICT", Z_NEED_DICT);
  zlib_set_num(js, c, "Z_ERRNO", Z_ERRNO);
  zlib_set_num(js, c, "Z_STREAM_ERROR", Z_STREAM_ERROR);
  zlib_set_num(js, c, "Z_DATA_ERROR", Z_DATA_ERROR);
  zlib_set_num(js, c, "Z_MEM_ERROR", Z_MEM_ERROR);
  zlib_set_num(js, c, "Z_BUF_ERROR", Z_BUF_ERROR);
  zlib_set_num(js, c, "Z_VERSION_ERROR", Z_VERSION_ERROR);

  zlib_set_num(js, c, "Z_NO_COMPRESSION", Z_NO_COMPRESSION);
  zlib_set_num(js, c, "Z_BEST_SPEED", Z_BEST_SPEED);
  zlib_set_num(js, c, "Z_BEST_COMPRESSION", Z_BEST_COMPRESSION);
  zlib_set_num(js, c, "Z_DEFAULT_COMPRESSION", Z_DEFAULT_COMPRESSION);

  zlib_set_num(js, c, "Z_FILTERED", Z_FILTERED);
  zlib_set_num(js, c, "Z_HUFFMAN_ONLY", Z_HUFFMAN_ONLY);
  zlib_set_num(js, c, "Z_RLE", Z_RLE);
  zlib_set_num(js, c, "Z_FIXED", Z_FIXED);
  zlib_set_num(js, c, "Z_DEFAULT_STRATEGY", Z_DEFAULT_STRATEGY);
  zlib_set_num(js, c, "ZLIB_VERNUM", ZLIB_VERNUM);

  zlib_set_num(js, c, "DEFLATE", 1);
  zlib_set_num(js, c, "INFLATE", 2);
  zlib_set_num(js, c, "GZIP", 3);
  zlib_set_num(js, c, "GUNZIP", 4);
  zlib_set_num(js, c, "DEFLATERAW", 5);
  zlib_set_num(js, c, "INFLATERAW", 6);
  zlib_set_num(js, c, "UNZIP", 7);
  zlib_set_num(js, c, "BROTLI_DECODE", 8);
  zlib_set_num(js, c, "BROTLI_ENCODE", 9);
  zlib_set_num(js, c, "Z_MIN_WINDOWBITS", 8);
  zlib_set_num(js, c, "Z_MAX_WINDOWBITS", 15);
  zlib_set_num(js, c, "Z_DEFAULT_WINDOWBITS", 15);
  zlib_set_num(js, c, "Z_MIN_CHUNK", 64);
  zlib_set_num(js, c, "Z_MAX_CHUNK", HUGE_VAL);
  zlib_set_num(js, c, "Z_DEFAULT_CHUNK", ZLIB_CHUNK);
  zlib_set_num(js, c, "Z_MIN_MEMLEVEL", 1);
  zlib_set_num(js, c, "Z_MAX_MEMLEVEL", 9);
  zlib_set_num(js, c, "Z_DEFAULT_MEMLEVEL", 8);
  zlib_set_num(js, c, "Z_MIN_LEVEL", Z_DEFAULT_COMPRESSION);
  zlib_set_num(js, c, "Z_MAX_LEVEL", Z_BEST_COMPRESSION);
  zlib_set_num(js, c, "Z_DEFAULT_LEVEL", Z_DEFAULT_COMPRESSION);

  zlib_set_num(js, c, "BROTLI_OPERATION_PROCESS", 0);
  zlib_set_num(js, c, "BROTLI_OPERATION_FLUSH", 1);
  zlib_set_num(js, c, "BROTLI_OPERATION_FINISH", 2);
  zlib_set_num(js, c, "BROTLI_OPERATION_EMIT_METADATA", 3);
  zlib_set_num(js, c, "BROTLI_PARAM_MODE", 0);
  zlib_set_num(js, c, "BROTLI_MODE_GENERIC", 0);
  zlib_set_num(js, c, "BROTLI_MODE_TEXT", 1);
  zlib_set_num(js, c, "BROTLI_MODE_FONT", 2);
  zlib_set_num(js, c, "BROTLI_DEFAULT_MODE", 0);
  zlib_set_num(js, c, "BROTLI_PARAM_QUALITY", 1);
  zlib_set_num(js, c, "BROTLI_MIN_QUALITY", 0);
  zlib_set_num(js, c, "BROTLI_MAX_QUALITY", 11);
  zlib_set_num(js, c, "BROTLI_DEFAULT_QUALITY", 11);
  zlib_set_num(js, c, "BROTLI_PARAM_LGWIN", 2);
  zlib_set_num(js, c, "BROTLI_MIN_WINDOW_BITS", 10);
  zlib_set_num(js, c, "BROTLI_MAX_WINDOW_BITS", 24);
  zlib_set_num(js, c, "BROTLI_DEFAULT_WINDOW", 22);
  zlib_set_num(js, c, "BROTLI_PARAM_SIZE_HINT", 3);
  zlib_set_num(js, c, "BROTLI_PARAM_LARGE_WINDOW", 4);
  zlib_set_num(js, c, "BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION", 0);
  zlib_set_num(js, c, "BROTLI_DECODER_PARAM_LARGE_WINDOW", 1);

  zlib_set_num(js, c, "ZSTD_e_continue", 0);
  zlib_set_num(js, c, "ZSTD_e_flush", 1);
  zlib_set_num(js, c, "ZSTD_e_end", 2);
}

static ant_value_t make_constants(ant_t *js) {
  ant_value_t c = js_mkobj(js);
  zlib_install_constants(js, c);
  return c;
}

static ant_value_t make_codes(ant_t *js) {
  ant_value_t codes = js_mkobj(js);
  js_set(js, codes, "0", js_mkstr(js, "Z_OK", 4));
  js_set(js, codes, "1", js_mkstr(js, "Z_STREAM_END", 12));
  js_set(js, codes, "2", js_mkstr(js, "Z_NEED_DICT", 11));
  js_set(js, codes, "-1", js_mkstr(js, "Z_ERRNO", 7));
  js_set(js, codes, "-2", js_mkstr(js, "Z_STREAM_ERROR", 14));
  js_set(js, codes, "-3", js_mkstr(js, "Z_DATA_ERROR", 12));
  js_set(js, codes, "-4", js_mkstr(js, "Z_MEM_ERROR", 11));
  js_set(js, codes, "-5", js_mkstr(js, "Z_BUF_ERROR", 11));
  js_set(js, codes, "-6", js_mkstr(js, "Z_VERSION_ERROR", 15));
  zlib_set_num(js, codes, "Z_OK", Z_OK);
  zlib_set_num(js, codes, "Z_STREAM_END", Z_STREAM_END);
  zlib_set_num(js, codes, "Z_NEED_DICT", Z_NEED_DICT);
  zlib_set_num(js, codes, "Z_ERRNO", Z_ERRNO);
  zlib_set_num(js, codes, "Z_STREAM_ERROR", Z_STREAM_ERROR);
  zlib_set_num(js, codes, "Z_DATA_ERROR", Z_DATA_ERROR);
  zlib_set_num(js, codes, "Z_MEM_ERROR", Z_MEM_ERROR);
  zlib_set_num(js, codes, "Z_BUF_ERROR", Z_BUF_ERROR);
  zlib_set_num(js, codes, "Z_VERSION_ERROR", Z_VERSION_ERROR);
  return codes;
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
  js_set(js, g_transform_proto, "close",    js_mkfun(js_zlib_close));
  js_set(js, g_transform_proto, "flush",    js_mkfun(js_zlib_flush));
  js_set(js, g_transform_proto, "params",   js_mkfun(js_zlib_params));
  js_set(js, g_transform_proto, "reset",    js_mkfun(js_zlib_reset));
  js_set(js, g_transform_proto, "pause",    js_mkfun(js_zlib_pause));
  js_set(js, g_transform_proto, "resume",   js_mkfun(js_zlib_resume));
  js_set(js, g_transform_proto, "unpipe",   js_mkfun(js_zlib_unpipe));
  js_set(js, g_transform_proto, "pipe",     js_mkfun(js_zlib_pipe));

  js_set_getter_desc(js, g_transform_proto, "bytesWritten", 12,
  js_mkfun(js_zlib_get_bytes_written), JS_DESC_C);
}

static ant_value_t zlib_mkctor(ant_t *js, zlib_kind_t kind, ant_cfunc_t fn, const char *name) {
  ant_value_t proto = js_mkobj(js);
  if (is_object_type(g_transform_proto)) js_set_proto_init(proto, g_transform_proto);
  g_zlib_protos[kind] = proto;

  ant_value_t ctor = js_make_ctor(js, fn, proto, name, strlen(name));
  js_mark_constructor(ctor, true);
  return ctor;
}

ant_value_t zlib_library(ant_t *js) {
  zlib_init_proto(js);

  ant_value_t lib = js_mkobj(js);
  ant_value_t consts = make_constants(js);
  ant_value_t codes = make_codes(js);

  js_set(js, lib, "gzip", js_mkfun(js_gzip));
  js_set(js, lib, "gzipSync", js_mkfun(js_gzipSync));
  js_set(js, lib, "Gzip", zlib_mkctor(js, ZLIB_KIND_GZIP, js_create_gzip, "Gzip"));
  js_set(js, lib, "createGzip", js_mkfun(js_create_gzip));

  js_set(js, lib, "gunzip", js_mkfun(js_gunzip));
  js_set(js, lib, "gunzipSync", js_mkfun(js_gunzipSync));
  js_set(js, lib, "Gunzip", zlib_mkctor(js, ZLIB_KIND_GUNZIP, js_create_gunzip, "Gunzip"));
  js_set(js, lib, "createGunzip", js_mkfun(js_create_gunzip));

  js_set(js, lib, "deflate", js_mkfun(js_deflate));
  js_set(js, lib, "deflateSync", js_mkfun(js_deflateSync));
  js_set(js, lib, "Deflate", zlib_mkctor(js, ZLIB_KIND_DEFLATE, js_create_deflate, "Deflate"));
  js_set(js, lib, "createDeflate", js_mkfun(js_create_deflate));

  js_set(js, lib, "inflate", js_mkfun(js_inflate));
  js_set(js, lib, "inflateSync", js_mkfun(js_inflateSync));
  js_set(js, lib, "Inflate", zlib_mkctor(js, ZLIB_KIND_INFLATE, js_create_inflate, "Inflate"));
  js_set(js, lib, "createInflate", js_mkfun(js_create_inflate));

  js_set(js, lib, "deflateRaw", js_mkfun(js_deflateRaw));
  js_set(js, lib, "deflateRawSync", js_mkfun(js_deflateRawSync));
  js_set(js, lib, "DeflateRaw", zlib_mkctor(js, ZLIB_KIND_DEFLATE_RAW, js_create_deflate_raw, "DeflateRaw"));
  js_set(js, lib, "createDeflateRaw", js_mkfun(js_create_deflate_raw));

  js_set(js, lib, "inflateRaw", js_mkfun(js_inflateRaw));
  js_set(js, lib, "inflateRawSync", js_mkfun(js_inflateRawSync));
  js_set(js, lib, "InflateRaw", zlib_mkctor(js, ZLIB_KIND_INFLATE_RAW, js_create_inflate_raw, "InflateRaw"));
  js_set(js, lib, "createInflateRaw", js_mkfun(js_create_inflate_raw));

  js_set(js, lib, "unzip", js_mkfun(js_unzip));
  js_set(js, lib, "unzipSync", js_mkfun(js_unzipSync));
  js_set(js, lib, "Unzip", zlib_mkctor(js, ZLIB_KIND_UNZIP, js_create_unzip, "Unzip"));
  js_set(js, lib, "createUnzip", js_mkfun(js_create_unzip));

  js_set(js, lib, "brotliCompress", js_mkfun(js_brotliCompress));
  js_set(js, lib, "brotliCompressSync", js_mkfun(js_brotliCompressSync));
  js_set(js, lib, "BrotliCompress", zlib_mkctor(js, ZLIB_KIND_BROTLI_COMPRESS, js_create_brotli_compress, "BrotliCompress"));
  js_set(js, lib, "createBrotliCompress", js_mkfun(js_create_brotli_compress));

  js_set(js, lib, "brotliDecompress", js_mkfun(js_brotliDecompress));
  js_set(js, lib, "brotliDecompressSync", js_mkfun(js_brotliDecompressSync));
  js_set(js, lib, "BrotliDecompress", zlib_mkctor(js, ZLIB_KIND_BROTLI_DECOMPRESS, js_create_brotli_decompress, "BrotliDecompress"));
  js_set(js, lib, "createBrotliDecompress", js_mkfun(js_create_brotli_decompress));
  
  js_set(js, lib, "default", lib);
  js_set(js, lib, "constants", consts);
  js_set(js, lib, "codes", codes);
  zlib_install_constants(js, lib);
  js_set(js, lib, "crc32", js_mkfun(js_zlib_crc32));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "zlib", 4));
  
  return lib;
}

void gc_mark_zlib(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  if (g_transform_proto) mark(js, g_transform_proto);
  for (int i = 0; i < ZLIB_KIND_COUNT; i++)
    if (g_zlib_protos[i]) mark(js, g_zlib_protos[i]);
  for (zlib_stream_t *st = g_active_streams; st; st = st->next_active) mark(js, st->obj);
}
