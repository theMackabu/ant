#include <compat.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "gc/roots.h"

#include "silver/call.h"
#include "streams/brotli.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/stream.h"
#include "modules/timer.h"
#include "modules/zlib.h"

static constexpr int ZLIB_CHUNK = 16384;
static constexpr uint32_t ZLIB_STREAM_TAG = Ant_NativeTag("ZLIB");

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

static_assert(
  ZLIB_KIND_COUNT == 9,
  "update zlib_protos in include/isolate_values.h when adding zlib kinds"
);

typedef struct {
  int level;
  int window_bits;
  int mem_level;
  int strategy;
  int chunk_size;
  int finish_flush;
  size_t max_output_length;
  const uint8_t *dictionary;
  size_t dictionary_len;
  brotli_params_t brotli_params;
} zlib_options_t;

typedef struct zlib_stream_s {
  z_stream strm;
  brotli_stream_state_t *brotli;
  ant_value_t obj;
  ant_t *js;
  zlib_kind_t kind;
  zlib_options_t opts;
  uint32_t bytes_written;
  uint8_t *owned_dict;
  bool initialized;
  bool ended;
  bool destroyed;
  bool closed;
} zlib_stream_t;

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

static bool zlib_kind_presets_dictionary(zlib_kind_t kind) {
  return kind == ZLIB_KIND_DEFLATE || kind == ZLIB_KIND_DEFLATE_RAW || kind == ZLIB_KIND_INFLATE_RAW;
}

static bool zlib_option_int(ant_t *js, ant_value_t opts, const char *key, int *out) {
  ant_value_t v = js_get(js, opts, key);
  if (vtype(v) == kTypeUndefined || vtype(v) == kTypeNull) return true;
  if (vtype(v) != kTypeNumber) return false;
  *out = (int)js_to_number(js, v);
  return true;
}

static ant_value_t zlib_read_brotli_params(
  ant_t *js, ant_value_t opts_val, brotli_params_t *out
) {
  ant_value_t params = js_get(js, opts_val, "params");
  if (vtype(params) == kTypeUndefined || vtype(params) == kTypeNull)
    return js_mkundef();
  
  if (!is_object_type(params))
    return js_mkerr_typed(js, JS_ERR_TYPE, "options.params must be an object");

  ant_value_t keys = js_for_in_keys(js, params);
  ant_offset_t n = js_arr_len(js, keys);

  for (ant_offset_t i = 0; i < n; i++) {
    ant_value_t key = js_arr_get(js, keys, i);
    size_t klen = 0;
    const char *kstr = js_getstr(js, key, &klen);
    if (!kstr || klen == 0)
      return js_mkerr_typed(js, JS_ERR_RANGE, "invalid brotli param");

    char *end = NULL;
    long id = strtol(kstr, &end, 10);
    if (!end || *end != '\0' || id < 0 || id > BROTLI_MAX_PARAM_ID || out->set[id])
      return js_mkerr_typed(js, JS_ERR_RANGE, "invalid brotli param");

    ant_value_t v = js_get(js, params, kstr);
    uint32_t value;
    
    if (vtype(v) == kTypeNumber) value = (uint32_t)js_to_number(js, v);
    else if (vtype(v) == kTypeBool) value = js_truthy(js, v) ? 1 : 0;
    else return js_mkerr_typed(js, JS_ERR_TYPE, "options.params values must be numbers");

    out->set[id] = true;
    out->value[id] = value;
  }

  return js_mkundef();
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

  ant_value_t max_out = js_get(js, opts_val, "maxOutputLength");
  if (vtype(max_out) == kTypeNumber) {
    double v = js_to_number(js, max_out);
    if (v < 0 || v != v) return js_mkerr_typed(js, JS_ERR_RANGE, "maxOutputLength out of range");
    out->max_output_length = (size_t)v;
  } else if (vtype(max_out) != kTypeUndefined && vtype(max_out) != kTypeNull) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "maxOutputLength must be a number");
  }

  ant_value_t dict = js_get(js, opts_val, "dictionary");
  if (vtype(dict) != kTypeUndefined && vtype(dict) != kTypeNull) {
    size_t dlen = 0;
    const uint8_t *dptr = NULL;
    if (!is_object_type(dict) || !buffer_source_get_bytes(js, dict, &dptr, &dlen))
      return js_mkerr_typed(js, JS_ERR_TYPE, "dictionary must be a buffer");
    out->dictionary = dptr;
    out->dictionary_len = dlen;
  }

  if (zlib_is_brotli_kind(kind))
    return zlib_read_brotli_params(js, opts_val, &out->brotli_params);

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

static ant_value_t pick_callback(ant_args_t) {
  if (nargs > 2 && is_callable(args[2])) return args[2];
  if (nargs > 1 && is_callable(args[1])) return args[1];
  return js_mkundef();
}

static zlib_stream_t *zlib_stream_ptr(ant_value_t obj) {
  return (zlib_stream_t *)js_get_native(obj, ZLIB_STREAM_TAG);
}

static void zlib_stream_release(zlib_stream_t *st) {
  if (!st) return;
  
  free(st->owned_dict);
  st->owned_dict = NULL;
  st->opts.dictionary = NULL;
  st->opts.dictionary_len = 0;
  
  if (st->brotli) {
    brotli_stream_state_destroy(st->brotli);
    st->brotli = NULL;
  } else if (st->initialized) {
    if (zlib_kind_is_compress(st->kind)) deflateEnd(&st->strm);
    else inflateEnd(&st->strm);
  }

  st->initialized = false;
  st->closed = true;
}

static ant_value_t zlib_binding_closed(ant_t *js) {
  return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_ZLIB_BINDING_CLOSED", "zlib binding closed");
}

static ant_value_t zlib_stream_ready(ant_t *js, zlib_stream_t *st) {
  if (st->closed) return zlib_binding_closed(js);
  if (st->initialized) return js_mkundef();

  bool compress = zlib_kind_is_compress(st->kind);
  int ret = compress
    ? deflateInit2(&st->strm, st->opts.level, Z_DEFLATED, st->opts.window_bits, st->opts.mem_level, st->opts.strategy)
    : inflateInit2(&st->strm, st->opts.window_bits);
  
  if (ret != Z_OK) return js_mkerr(js, "zlib init failed");
  st->initialized = true;

  if (!st->opts.dictionary || !zlib_kind_presets_dictionary(st->kind)) return js_mkundef();
  ret = compress
    ? deflateSetDictionary(&st->strm, st->opts.dictionary, (uInt)st->opts.dictionary_len)
    : inflateSetDictionary(&st->strm, st->opts.dictionary, (uInt)st->opts.dictionary_len);

  return ret == Z_OK ? js_mkundef() : js_mkerr(js, "zlib dictionary load failed");
}

static void zlib_stream_finalizer(ant_t *js, ant_value_t value, void *state) {
  zlib_stream_t *st = state;
  if (!st) return;

  js_clear_native(value, ZLIB_STREAM_TAG);
  zlib_stream_release(st);

  free(st);
}

static ant_value_t zlib_make_buffer(ant_t *js, const uint8_t *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(js, len);
  if (!ab) return Ant_Error_CreateOutOfMemory(js);
  if (data && len > 0) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, js->builtins.buffer_proto);
}

static ant_value_t zlib_emit_data(ant_t *js, ant_value_t obj, const uint8_t *data, size_t len) {
  ant_value_t buf = zlib_make_buffer(js, data, len);
  if (is_err(buf)) return buf;
  stream_readable_push(js, obj, buf, js_mkundef());
  return js_mkundef();
}

typedef struct { ant_t *js; ant_value_t obj; ant_value_t error; } brotli_emit_ctx_t;
typedef struct { ant_t *js; ant_value_t arr; bool error; } brotli_collect_ctx_t;

static int brotli_emit_cb(void *ctx, const uint8_t *chunk, size_t len) {
  brotli_emit_ctx_t *ec = (brotli_emit_ctx_t *)ctx;
  ec->error = zlib_emit_data(ec->js, ec->obj, chunk, len);
  return is_err(ec->error) || Ant_Exception_Pending(ec->js) ? -1 : 0;
}

static int brotli_collect_cb(void *ctx, const uint8_t *chunk, size_t len) {
  brotli_collect_ctx_t *ec = (brotli_collect_ctx_t *)ctx;
  ant_value_t buf = zlib_make_buffer(ec->js, chunk, len);
  if (is_err(buf)) { ec->error = true; return -1; }
  js_arr_push(ec->js, ec->arr, buf);
  return 0;
}

static const char *zlib_code_name(int code) {
  switch (code) {
    case Z_NEED_DICT: return "Z_NEED_DICT";
    case Z_ERRNO: return "Z_ERRNO";
    case Z_STREAM_ERROR: return "Z_STREAM_ERROR";
    case Z_DATA_ERROR: return "Z_DATA_ERROR";
    case Z_MEM_ERROR: return "Z_MEM_ERROR";
    case Z_BUF_ERROR: return "Z_BUF_ERROR";
    case Z_VERSION_ERROR: return "Z_VERSION_ERROR";
    default: return "Z_UNKNOWN";
  }
}

static ant_value_t zlib_error(ant_t *js, const char *message, const char *code, int errnum) {
  ant_value_t props = Ant_Object_CreateBare(js);
  js_set(js, props, "code", js_mkstr(js, code, strlen(code)));
  js_set(js, props, "errno", js_mknum((double)errnum));
  return js_mkerr_props(js, JS_ERR_GENERIC, props, "%s", message);
}

static ant_value_t zlib_brotli_error(ant_t *js, int code, const char *name) {
  if (code >= 0) return zlib_error(js, "unexpected end of file", "Z_BUF_ERROR", Z_BUF_ERROR);
  char code_name[96];
  snprintf(code_name, sizeof(code_name), "ERR_%s", name ? name : "BROTLI");
  return zlib_error(js, "Decompression failed", code_name, code);
}

static ant_value_t zlib_stream_error(ant_t *js, int ret, const z_stream *strm) {
  if (ret == Z_BUF_ERROR) return zlib_error(js, "unexpected end of file", "Z_BUF_ERROR", Z_BUF_ERROR);
  if (ret == Z_NEED_DICT) return zlib_error(js, "Missing dictionary", "Z_NEED_DICT", Z_NEED_DICT);
  return zlib_error(js, strm->msg ? strm->msg : "zlib error", zlib_code_name(ret), ret);
}

static int zlib_inflate_with_dictionary(zlib_stream_t *st, int flush) {
  int ret = inflate(&st->strm, flush);
  if (ret != Z_NEED_DICT || !st->opts.dictionary) return ret;
  if (inflateSetDictionary(&st->strm, st->opts.dictionary, (uInt)st->opts.dictionary_len) != Z_OK) return ret;
  return inflate(&st->strm, flush);
}

static ant_value_t zlib_do_process(
  ant_t *js, zlib_stream_t *st,
  const uint8_t *input, size_t input_len,
  int flush
) {
  if (st->ended) return js_mkundef();
  ant_value_t ready = zlib_stream_ready(js, st);
  if (is_err(ready)) return ready;

  if (st->brotli) {
    brotli_emit_ctx_t ctx = { js, st->obj, js_mkundef() };
    int rc;

    if (flush == Z_FINISH) rc = brotli_stream_finish(st->brotli, brotli_emit_cb, &ctx);
    else rc = brotli_stream_process(st->brotli, input, input_len, brotli_emit_cb, &ctx);

    if (is_err(ctx.error)) return ctx.error;
    if (Ant_Exception_Pending(js)) return js_mkundef();
    
    if (rc < 0 && st->kind == ZLIB_KIND_BROTLI_DECOMPRESS) {
      const char *error_name = NULL;
      int error_code = brotli_stream_decoder_error(st->brotli, &error_name);
      return zlib_brotli_error(js, error_code < 0 ? error_code : -1, error_name);
    }
    
    if (rc < 0) return js_mkerr(js, "brotli operation failed");
    return js_mkundef();
  }

  bool compress = zlib_kind_is_compress(st->kind);
  size_t chunk_size = (st->opts.chunk_size > 0) ? (size_t)st->opts.chunk_size : ZLIB_CHUNK;
  uint8_t *out = malloc(chunk_size);
  if (!out) return Ant_Error_CreateOutOfMemory(js);

  st->strm.next_in = input ? (Bytef *)input : NULL;
  st->strm.avail_in = input ? (uInt)input_len : 0;

  int ret;
  ant_value_t result = js_mkundef();
  do {
    st->strm.next_out = out;
    st->strm.avail_out = (uInt)chunk_size;

    if (compress) {
      ret = deflate(&st->strm, flush);
      if (ret == Z_STREAM_ERROR) { result = zlib_stream_error(js, ret, &st->strm); break; }
    } else {
      ret = zlib_inflate_with_dictionary(st, flush);
      if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        result = zlib_stream_error(js, ret, &st->strm);
        break;
      }
    }

    size_t have = chunk_size - st->strm.avail_out;
    if (have > 0) result = zlib_emit_data(js, st->obj, out, have);
    if (is_err(result) || Ant_Exception_Pending(js)) break;
    if (st->destroyed) break;
    
    if (ret == Z_STREAM_END) {
      // Like Node, unused input after DEFLATE EOF ends the readable side;
      // ws appends its trailer here, then checks endEmitted in flush().
      if ((st->kind == ZLIB_KIND_INFLATE || st->kind == ZLIB_KIND_INFLATE_RAW) && st->strm.avail_in > 0)
        stream_readable_push(js, st->obj, js_mknull(), js_mkundef());
      break;
    }
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
  if (!out) return Ant_Error_CreateOutOfMemory(js);

  st->strm.next_in  = input ? (Bytef *)input : NULL;
  st->strm.avail_in = input ? (uInt)input_len : 0;

  int ret;
  ant_value_t result = arr;
  do {
    st->strm.next_out  = out;
    st->strm.avail_out = (uInt)chunk_size;

    if (compress) {
      ret = deflate(&st->strm, flush_flag);
      if (ret == Z_STREAM_ERROR) { result = zlib_stream_error(js, ret, &st->strm); break; }
    } else {
      ret = zlib_inflate_with_dictionary(st, flush_flag);
      if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
        result = zlib_stream_error(js, ret, &st->strm);
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

static ant_value_t js_zlib_process_chunk(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  if (st->ended) return js_mkarr(js);
  
  ant_value_t ready = zlib_stream_ready(js, st);
  if (is_err(ready)) return ready;

  const uint8_t *input = NULL;
  size_t input_len = 0;
  if (nargs >= 1 && is_object_type(args[0]))
    buffer_source_get_bytes(js, args[0], &input, &input_len);

  int flush_flag = Z_NO_FLUSH;
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber)
    flush_flag = (int)js_getnum(args[1]);

  return zlib_process_chunk_sync(js, st, input, input_len, flush_flag);
}

static ant_value_t zlib_finish_callback(ant_t *js, ant_value_t cb, ant_value_t result) {
  if (is_err(result)) {
    GC_ROOT_SAVE(root_mark, js);
    ant_value_t error = js_take_thrown(js, result);

    GC_ROOT_PIN(js, error);
    ant_value_t r = is_callable(cb) ? sv_vm_call(js->vm, js, cb, js_mkundef(), &error, 1, NULL, js_mkundef()) : js_mkundef();

    GC_ROOT_RESTORE(js, root_mark);
    return is_err(r) ? r : js_mkundef();
  }

  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  if (!is_callable(cb)) return js_mkundef();

  ant_value_t r = sv_vm_call(js->vm, js, cb, js_mkundef(), NULL, 0, NULL, js_mkundef());
  return is_err(r) ? r : js_mkundef();
}

static ant_value_t js_zlib__transform(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  ant_value_t cb = nargs > 2 ? args[2] : js_mkundef();
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  
  if (nargs > 0 && is_object_type(args[0])) buffer_source_get_bytes(js, args[0], &bytes, &len);
  if (!bytes && nargs > 0 && vtype(args[0]) == kTypeString) {
    size_t slen = 0;
    char *s = js_getstr(js, args[0], &slen);
    if (s) { bytes = (const uint8_t *)s; len = slen; }
  }

  ant_value_t r = js_mkundef();
  if (bytes) {
    st->bytes_written += (uint32_t)len;
    r = zlib_do_process(js, st, bytes, len, Z_NO_FLUSH);
  }
  return zlib_finish_callback(js, cb, r);
}

static ant_value_t js_zlib__flush(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  ant_value_t cb = nargs > 0 ? args[0] : js_mkundef();
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");

  ant_value_t r = zlib_do_process(js, st, NULL, 0, Z_FINISH);
  st->ended = true;
  
  return zlib_finish_callback(js, cb, r);
}

static ant_value_t js_zlib_flush(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  ant_value_t self = js_getthis(js);
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");

  int flush = st->brotli ? 1 : Z_FULL_FLUSH;
  ant_value_t cb = js_mkundef();
  if (nargs > 0 && is_callable(args[0])) cb = args[0];
  else {
    if (nargs > 0 && vtype(args[0]) == kTypeNumber) flush = (int)js_getnum(args[0]);
    if (nargs > 1 && is_callable(args[1])) cb = args[1];
  }

  if (st->destroyed || st->ended) {
    if (is_callable(cb)) queue_microtask_with_args(js, cb, NULL, 0);
    return self;
  }

  ant_value_t r = st->brotli && !st->closed ? js_mkundef() : zlib_do_process(js, st, NULL, 0, flush);
  if (is_err(r)) {
    GC_ROOT_SAVE(root_mark, js);
    r = js_take_thrown(js, r);
    GC_ROOT_PIN(js, r);
    eventemitter_emit_args(js, st->obj, "error", &r, 1);
    if (!Ant_Exception_Pending(js) && is_callable(cb)) {
      ant_value_t argv[1] = { r };
      sv_vm_call(js->vm, js, cb, js_mkundef(), argv, 1, NULL, js_mkundef());
    }
    GC_ROOT_RESTORE(js, root_mark);
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    return self;
  }

  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);

  if (is_callable(cb)) {
    ant_value_t null_val = js_mknull();
    queue_next_tick_with_args(js, cb, &null_val, 1);
  }
  
  return self;
}

static ant_value_t js_zlib_reset(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  if (st->destroyed) return js_getthis(js);
  if (st->closed) return zlib_binding_closed(js);

  if (st->brotli) {
    bool decompress = st->kind == ZLIB_KIND_BROTLI_DECOMPRESS;
    brotli_stream_state_destroy(st->brotli);
    st->brotli = brotli_stream_state_new_params(decompress, &st->opts.brotli_params,
      st->opts.dictionary, st->opts.dictionary_len, NULL);
    if (!st->brotli) return js_mkerr(js, "brotli init failed");
  } else if (st->initialized) {
    int ret = zlib_kind_is_compress(st->kind) ? deflateReset(&st->strm) : inflateReset(&st->strm);
    if (ret != Z_OK) return js_mkerr(js, "zlib reset failed");
  }

  st->bytes_written = 0;
  
  return js_getthis(js);
}

static ant_value_t js_zlib_params(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");
  
  if (!zlib_kind_is_compress(st->kind) || st->brotli)
    return js_mkerr_typed(js, JS_ERR_TYPE, "params() is only available for deflate streams");
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "level and strategy are required");

  int level = (int)js_to_number(js, args[0]);
  int strategy = (int)js_to_number(js, args[1]);
  ant_value_t ready = zlib_stream_ready(js, st);
  
  if (is_err(ready)) return ready;
  if (deflateParams(&st->strm, level, strategy) != Z_OK)
    return js_mkerr(js, "zlib params failed");

  ant_value_t cb = nargs > 2 && is_callable(args[2]) ? args[2] : js_mkundef();
  if (is_callable(cb)) {
    ant_value_t null_val = js_mknull();
    sv_vm_call(js->vm, js, cb, js_mkundef(), &null_val, 1, NULL, js_mkundef());
  }
  
  return js_getthis(js);
}

static ant_value_t js_zlib_close(ant_params_t) {
  ant_value_t self = js_getthis(js);
  ant_value_t destroy = js_get(js, stream_readable_prototype(js), "destroy");
  ant_value_t result = sv_vm_call(js->vm, js, destroy, self, NULL, 0, NULL, js_mkundef());
  
  if (is_err(result)) return result;
  if (nargs > 0 && is_callable(args[0]))
    sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, js_mkundef());
  
  return self;
}

static ant_value_t js_zlib_handle_close(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_get_slot(js_getthis(js), SLOT_DATA));
  if (st) zlib_stream_release(st);

  if (nargs > 0 && is_callable(args[0]))
    sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, js_mkundef());

  return js_mkundef();
}

static ant_value_t js_zlib__destroy(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid zlib stream");

  st->destroyed = true;
  zlib_stream_release(st);
  js_set(js, js_getthis(js), "_handle", js_mknull());

  if (nargs > 1 && is_callable(args[1]))
    return sv_vm_call(js->vm, js, args[1], js_mkundef(), args, 1, NULL, js_mkundef());
  
  return js_mkundef();
}

static ant_value_t js_zlib_get_bytes_written(ant_params_t) {
  zlib_stream_t *st = zlib_stream_ptr(js_getthis(js));
  if (!st) return js_mknum(0);
  return js_mknum((double)st->bytes_written);
}

static ant_value_t zlib_create_stream(ant_t *js, zlib_kind_t kind, ant_value_t opts_val) {
  zlib_options_t opts;
  ant_value_t opt_err = zlib_read_options(js, kind, opts_val, &opts);
  if (is_err(opt_err)) return opt_err;

  zlib_stream_t *st = calloc(1, sizeof(zlib_stream_t));
  if (!st) return Ant_Error_CreateOutOfMemory(js);

  st->kind = kind;
  st->js = js;
  st->opts = opts;

  if (opts.dictionary && opts.dictionary_len > 0) {
    st->owned_dict = malloc(opts.dictionary_len);
    if (!st->owned_dict) { free(st); return Ant_Error_CreateOutOfMemory(js); }
    memcpy(st->owned_dict, opts.dictionary, opts.dictionary_len);
    st->opts.dictionary = st->owned_dict;
  } else {
    st->opts.dictionary = NULL;
    st->opts.dictionary_len = 0;
  }

  if (kind == ZLIB_KIND_BROTLI_COMPRESS || kind == ZLIB_KIND_BROTLI_DECOMPRESS) {
    int bad_id = -1;
    st->brotli = brotli_stream_state_new_params(
      kind == ZLIB_KIND_BROTLI_DECOMPRESS, &opts.brotli_params,
      opts.dictionary, opts.dictionary_len, &bad_id);
    if (!st->brotli) {
      free(st->owned_dict);
      free(st);
      if (bad_id >= 0) return js_mkerr_typed(js, JS_ERR_RANGE, "invalid brotli param");
      return js_mkerr(js, "brotli init failed");
    }
    st->initialized = true;
  }

  ant_value_t proto = 
    kind >= 0 && kind < ZLIB_KIND_COUNT && 
    is_object_type(js->builtins.zlib_protos[kind])
      ? js->builtins.zlib_protos[kind]
      : js->builtins.zlib_transform_proto;
  
  ant_value_t obj = stream_construct_duplex(js, proto, opts_val, js_mkundef());
  if (is_err(obj)) {
    zlib_stream_release(st);
    free(st);
    return obj;
  }

  js_set_native(obj, st, ZLIB_STREAM_TAG);
  stream_set_attached_state(obj, st, zlib_stream_finalizer);
  js_set(js, obj, "_processChunk", Ant_Function_Create(js_zlib_process_chunk));

  ant_value_t handle_obj = Ant_Object_CreateBare(js);
  js_set_slot(handle_obj, SLOT_DATA, obj);
  
  js_set(js, handle_obj, "close", Ant_Function_Create(js_zlib_handle_close));
  js_set(js, obj, "_handle", handle_obj);

  st->obj = obj;
  return obj;
}

static ant_value_t js_create_gzip(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_GZIP, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_gunzip(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_GUNZIP, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_deflate(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_DEFLATE, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_inflate(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_INFLATE, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_deflate_raw(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_DEFLATE_RAW, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_inflate_raw(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_INFLATE_RAW, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_unzip(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_UNZIP, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_brotli_compress(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_BROTLI_COMPRESS, nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t js_create_brotli_decompress(ant_params_t) {
  return zlib_create_stream(js, ZLIB_KIND_BROTLI_DECOMPRESS, nargs > 0 ? args[0] : js_mkundef());
}

typedef struct { 
  uint8_t *data;
  size_t len;
  size_t cap;
  int error;
  size_t limit;
  int overflow;
} zbuf_t;

static int zbuf_append(zbuf_t *b, const uint8_t *chunk, size_t n) {
  if (b->limit && b->len + n > b->limit) { 
    b->overflow = 1; 
    return -1;
  }
  
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

static ant_value_t zlib_too_large(ant_t *js, size_t limit) {
  ant_value_t props = Ant_Object_CreateBare(js);
  js_set(js, props, "code", js_mkstr(js, "ERR_BUFFER_TOO_LARGE", 20));
  return js_mkerr_props(js, JS_ERR_RANGE, props,
    "Cannot create a Buffer larger than %zu bytes", limit);
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
  out.limit = opts.max_output_length;
  uint8_t *tmp = malloc((size_t)opts.chunk_size);
  if (!tmp) return Ant_Error_CreateOutOfMemory(js);

  if (kind == ZLIB_KIND_BROTLI_COMPRESS || kind == ZLIB_KIND_BROTLI_DECOMPRESS) {
    int bad_id = -1;
    brotli_stream_state_t *bs = brotli_stream_state_new_params(
      kind == ZLIB_KIND_BROTLI_DECOMPRESS, &opts.brotli_params,
      opts.dictionary, opts.dictionary_len, &bad_id);
    if (!bs) {
      free(tmp);
      if (bad_id >= 0) return js_mkerr_typed(js, JS_ERR_RANGE, "invalid brotli param");
      return js_mkerr(js, "brotli init failed");
    }
    
    int rc = brotli_stream_process(bs, input, input_len, zbuf_brotli_cb, &out);
    if (rc >= 0) rc = brotli_stream_finish(bs, zbuf_brotli_cb, &out);
    
    const char *error_name = NULL;
    int error_code = brotli_stream_decoder_error(bs, &error_name);
    brotli_stream_state_destroy(bs);
    
    if (rc < 0 || out.error || out.overflow) {
      int over = out.overflow, failed = out.error;
      free(tmp); free(out.data);
      
      if (over) return zlib_too_large(js, opts.max_output_length);
      if (failed || kind == ZLIB_KIND_BROTLI_COMPRESS) return js_mkerr(js, "brotli operation failed");
      
      return zlib_brotli_error(js, error_code, error_name);
    }
    
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

  if (opts.dictionary && zlib_kind_presets_dictionary(kind)) {
    ret = compress
      ? deflateSetDictionary(&strm, opts.dictionary, (uInt)opts.dictionary_len)
      : inflateSetDictionary(&strm, opts.dictionary, (uInt)opts.dictionary_len);
    if (ret != Z_OK) {
      if (compress) deflateEnd(&strm); else inflateEnd(&strm);
      free(tmp);
      return js_mkerr(js, "zlib dictionary load failed");
    }
  }

  strm.next_in = (Bytef *)input;
  strm.avail_in = (uInt)input_len;

  do {
    strm.next_out = tmp;
    strm.avail_out = (uInt)opts.chunk_size;
    if (compress) ret = deflate(&strm, Z_FINISH);
    else ret = inflate(&strm, opts.finish_flush);
    if (ret == Z_NEED_DICT && opts.dictionary) {
      ret = inflateSetDictionary(&strm, opts.dictionary, (uInt)opts.dictionary_len);
      if (ret == Z_OK) {
        size_t got = (size_t)opts.chunk_size - strm.avail_out;
        if (got > 0 && zbuf_append(&out, tmp, got) < 0) {
          int over = out.overflow;
          inflateEnd(&strm); free(tmp); free(out.data);
          if (over) return zlib_too_large(js, opts.max_output_length);
          return Ant_Error_CreateOutOfMemory(js);
        }
        continue;
      }
    }
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR || ret == Z_NEED_DICT) {
      ant_value_t err = zlib_stream_error(js, ret, &strm);
      if (compress) deflateEnd(&strm);
      else inflateEnd(&strm);
      free(tmp);
      free(out.data);
      return err;
    }
    size_t have = (size_t)opts.chunk_size - strm.avail_out;
    if (have > 0 && zbuf_append(&out, tmp, have) < 0) {
      int over = out.overflow;
      if (compress) deflateEnd(&strm);
      else inflateEnd(&strm);
      free(tmp);
      free(out.data);
      if (over) return zlib_too_large(js, opts.max_output_length);
      return Ant_Error_CreateOutOfMemory(js);
    }
    if (ret == Z_STREAM_END) break;
  } while (strm.avail_out == 0 || strm.avail_in > 0);

  if (compress) deflateEnd(&strm);
  else inflateEnd(&strm);

  if (!compress && ret != Z_STREAM_END && opts.finish_flush == Z_FINISH) {
    free(tmp);
    free(out.data);
    return zlib_stream_error(js, Z_BUF_ERROR, &strm);
  }

  ant_value_t buf = zlib_make_buffer(js, out.data, out.len);
  free(tmp);
  free(out.data);
  return buf;
}

static ant_value_t get_input_bytes(ant_t *js, ant_value_t val, const uint8_t **out_bytes, size_t *out_len) {
  ArrayBufferData *ab = buffer_get_arraybuffer_data(val);
  
  if (ab && ab->is_detached) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform Construct on a detached ArrayBuffer");
  if (is_object_type(val) && buffer_source_get_bytes(js, val, out_bytes, out_len)) return js_mkundef();
  
  size_t slen = 0;
  char *s = js_getstr(js, val, &slen);
  if (!s) return js_mkerr_typed(js, JS_ERR_TYPE, "argument must be a string or Buffer");
  
  *out_bytes = (const uint8_t *)s;
  *out_len = slen;
  
  return js_mkundef();
}

static ant_value_t zlib_sync_fn(ant_native_params_t, zlib_kind_t kind) {
  if (nargs < 1) return js_mkerr(js, "argument required");
  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t input = get_input_bytes(js, args[0], &bytes, &len);
  if (is_err(input)) return input;
  return zlib_sync_op(js, kind, bytes, len, nargs > 1 ? args[1] : js_mkundef());
}

static ant_value_t zlib_async_fn(ant_native_params_t, zlib_kind_t kind) {
  if (nargs < 1) return js_mkerr(js, "argument required");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t cb = pick_callback(args, nargs);

  ant_value_t input = get_input_bytes(js, args[0], &bytes, &len);
  if (is_err(input)) return input;

  ant_value_t opts = (nargs > 1 && is_object_type(args[1])) ? args[1] : js_mkundef();
  ant_value_t result = zlib_sync_op(js, kind, bytes, len, opts);

  if (is_callable(cb)) {
    if (is_err(result)) {
      ant_value_t argv[1] = { js_take_thrown(js, result) };
      sv_vm_call(js->vm, js, cb, js_mkundef(), argv, 1, NULL, js_mkundef());
    } else {
      ant_value_t null_val = js_mknull();
      ant_value_t argv[2] = { null_val, result };
      sv_vm_call(js->vm, js, cb, js_mkundef(), argv, 2, NULL, js_mkundef());
    }
    return js_mkundef();
  }

  return result;
}

#define ZLIB_SYNC_FN(name, kind) \
  static ant_value_t js_##name##Sync(ant_params_t) { return zlib_sync_fn(js, args, nargs, kind); }

#define ZLIB_ASYNC_FN(name, kind) \
  static ant_value_t js_##name(ant_params_t) { return zlib_async_fn(js, args, nargs, kind); }

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

static ant_value_t js_zlib_crc32(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "argument required");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  uLong init_val = 0;

  if (nargs >= 2 && vtype(args[1]) == kTypeNumber)
    init_val = (uLong)js_getnum(args[1]);

  ant_value_t input = get_input_bytes(js, args[0], &bytes, &len);
  if (is_err(input)) return input;

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
  zlib_set_num(js, c, "BROTLI_PARAM_MODE", BROTLI_PARAM_MODE);
  zlib_set_num(js, c, "BROTLI_MODE_GENERIC", 0);
  zlib_set_num(js, c, "BROTLI_MODE_TEXT", 1);
  zlib_set_num(js, c, "BROTLI_MODE_FONT", 2);
  zlib_set_num(js, c, "BROTLI_DEFAULT_MODE", 0);
  zlib_set_num(js, c, "BROTLI_PARAM_QUALITY", BROTLI_PARAM_QUALITY);
  zlib_set_num(js, c, "BROTLI_MIN_QUALITY", 0);
  zlib_set_num(js, c, "BROTLI_MAX_QUALITY", 11);
  zlib_set_num(js, c, "BROTLI_DEFAULT_QUALITY", 11);
  zlib_set_num(js, c, "BROTLI_PARAM_LGWIN", BROTLI_PARAM_LGWIN);
  zlib_set_num(js, c, "BROTLI_MIN_WINDOW_BITS", 10);
  zlib_set_num(js, c, "BROTLI_MAX_WINDOW_BITS", 24);
  zlib_set_num(js, c, "BROTLI_DEFAULT_WINDOW", 22);
  zlib_set_num(js, c, "BROTLI_LARGE_MAX_WINDOW_BITS", BROTLI_LARGE_MAX_WINDOW_BITS);
  zlib_set_num(js, c, "BROTLI_PARAM_LGBLOCK", BROTLI_PARAM_LGBLOCK);
  zlib_set_num(js, c, "BROTLI_MIN_INPUT_BLOCK_BITS", BROTLI_MIN_INPUT_BLOCK_BITS);
  zlib_set_num(js, c, "BROTLI_MAX_INPUT_BLOCK_BITS", BROTLI_MAX_INPUT_BLOCK_BITS);
  zlib_set_num(js, c, "BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING", BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING);
  zlib_set_num(js, c, "BROTLI_PARAM_SIZE_HINT", BROTLI_PARAM_SIZE_HINT);
  zlib_set_num(js, c, "BROTLI_PARAM_LARGE_WINDOW", BROTLI_PARAM_LARGE_WINDOW);
  zlib_set_num(js, c, "BROTLI_PARAM_NPOSTFIX", BROTLI_PARAM_NPOSTFIX);
  zlib_set_num(js, c, "BROTLI_PARAM_NDIRECT", BROTLI_PARAM_NDIRECT);
  zlib_set_num(js, c, "BROTLI_DECODER_RESULT_ERROR", BROTLI_DECODER_RESULT_ERROR);
  zlib_set_num(js, c, "BROTLI_DECODER_RESULT_SUCCESS", BROTLI_DECODER_RESULT_SUCCESS);
  zlib_set_num(js, c, "BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT", BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT);
  zlib_set_num(js, c, "BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT", BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
  zlib_set_num(js, c, "BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION", BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION);
  zlib_set_num(js, c, "BROTLI_DECODER_PARAM_LARGE_WINDOW", BROTLI_DECODER_PARAM_LARGE_WINDOW);

#define ZLIB_BROTLI_ERROR_CODE(PREFIX, NAME, CODE) zlib_set_num(js, c, "BROTLI_DECODER" #PREFIX #NAME, CODE);
#define ZLIB_BROTLI_ERROR_SEPARATOR
  BROTLI_DECODER_ERROR_CODES_LIST(ZLIB_BROTLI_ERROR_CODE, ZLIB_BROTLI_ERROR_SEPARATOR)
#undef ZLIB_BROTLI_ERROR_CODE
#undef ZLIB_BROTLI_ERROR_SEPARATOR

  zlib_set_num(js, c, "ZSTD_e_continue", 0);
  zlib_set_num(js, c, "ZSTD_e_flush", 1);
  zlib_set_num(js, c, "ZSTD_e_end", 2);
}

static ant_value_t make_constants(ant_t *js) {
  ant_value_t c = Ant_Object_CreateBare(js);
  zlib_install_constants(js, c);
  return c;
}

static ant_value_t make_codes(ant_t *js) {
  ant_value_t codes = Ant_Object_CreateBare(js);
  
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
  if (js->builtins.zlib_transform_proto) return;

  js->builtins.zlib_transform_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.zlib_transform_proto, stream_transform_prototype(js));

  js_set(js, js->builtins.zlib_transform_proto, "_transform", Ant_Function_CreateWithArity(js_zlib__transform, 3));
  js_set(js, js->builtins.zlib_transform_proto, "_flush", Ant_Function_CreateWithArity(js_zlib__flush, 1));
  js_set(js, js->builtins.zlib_transform_proto, "_destroy", Ant_Function_Create(js_zlib__destroy));
  js_set(js, js->builtins.zlib_transform_proto, "close", Ant_Function_Create(js_zlib_close));
  js_set(js, js->builtins.zlib_transform_proto, "flush", Ant_Function_Create(js_zlib_flush));
  js_set(js, js->builtins.zlib_transform_proto, "params", Ant_Function_Create(js_zlib_params));
  js_set(js, js->builtins.zlib_transform_proto, "reset", Ant_Function_Create(js_zlib_reset));

  js_set_getter_desc(js, js->builtins.zlib_transform_proto, "bytesWritten", 12,
  Ant_Function_Create(js_zlib_get_bytes_written), JS_DESC_C);
}

typedef struct {
  ant_cfunc_meta_t async;
  ant_cfunc_meta_t sync;
  ant_cfunc_meta_t create;
  ant_cfunc_meta_t ctor;
} zlib_kind_entry_t;

static const zlib_kind_entry_t zlib_kinds[ZLIB_KIND_COUNT] = {
  [ZLIB_KIND_GZIP] = {
    { js_gzip, "gzip", 3, 0 }, { js_gzipSync, "gzipSync", 2, 0 },
    { js_create_gzip, "createGzip", 1, 0 }, { js_create_gzip, "Gzip", 1, 0 },
  },
  [ZLIB_KIND_GUNZIP] = {
    { js_gunzip, "gunzip", 3, 0 }, { js_gunzipSync, "gunzipSync", 2, 0 },
    { js_create_gunzip, "createGunzip", 1, 0 }, { js_create_gunzip, "Gunzip", 1, 0 },
  },
  [ZLIB_KIND_DEFLATE] = {
    { js_deflate, "deflate", 3, 0 }, { js_deflateSync, "deflateSync", 2, 0 },
    { js_create_deflate, "createDeflate", 1, 0 }, { js_create_deflate, "Deflate", 1, 0 },
  },
  [ZLIB_KIND_INFLATE] = {
    { js_inflate, "inflate", 3, 0 }, { js_inflateSync, "inflateSync", 2, 0 },
    { js_create_inflate, "createInflate", 1, 0 }, { js_create_inflate, "Inflate", 1, 0 },
  },
  [ZLIB_KIND_DEFLATE_RAW] = {
    { js_deflateRaw, "deflateRaw", 3, 0 }, { js_deflateRawSync, "deflateRawSync", 2, 0 },
    { js_create_deflate_raw, "createDeflateRaw", 1, 0 }, { js_create_deflate_raw, "DeflateRaw", 1, 0 },
  },
  [ZLIB_KIND_INFLATE_RAW] = {
    { js_inflateRaw, "inflateRaw", 3, 0 }, { js_inflateRawSync, "inflateRawSync", 2, 0 },
    { js_create_inflate_raw, "createInflateRaw", 1, 0 }, { js_create_inflate_raw, "InflateRaw", 1, 0 },
  },
  [ZLIB_KIND_UNZIP] = {
    { js_unzip, "unzip", 3, 0 }, { js_unzipSync, "unzipSync", 2, 0 },
    { js_create_unzip, "createUnzip", 1, 0 }, { js_create_unzip, "Unzip", 1, 0 },
  },
  [ZLIB_KIND_BROTLI_COMPRESS] = {
    { js_brotliCompress, "brotliCompress", 3, 0 }, { js_brotliCompressSync, "brotliCompressSync", 2, 0 },
    { js_create_brotli_compress, "createBrotliCompress", 1, 0 }, { js_create_brotli_compress, "BrotliCompress", 1, 0 },
  },
  [ZLIB_KIND_BROTLI_DECOMPRESS] = {
    { js_brotliDecompress, "brotliDecompress", 3, 0 }, { js_brotliDecompressSync, "brotliDecompressSync", 2, 0 },
    { js_create_brotli_decompress, "createBrotliDecompress", 1, 0 }, { js_create_brotli_decompress, "BrotliDecompress", 1, 0 },
  },
};

static void zlib_define_kind(ant_t *js, ant_value_t lib, zlib_kind_t kind) {
  const zlib_kind_entry_t *entry = &zlib_kinds[kind];
  ant_value_t proto = Ant_Object_CreateBare(js);
  if (is_object_type(js->builtins.zlib_transform_proto)) js_set_proto_init(proto, js->builtins.zlib_transform_proto);
  js->builtins.zlib_protos[kind] = proto;

  js_set(js, lib, entry->async.name, js_mkfun_meta(&entry->async));
  js_set(js, lib, entry->sync.name, js_mkfun_meta(&entry->sync));
  js_set(js, lib, entry->ctor.name, js_make_ctor_cfunc(js, js_mkfun_meta(&entry->ctor), proto, ANT_PROP_ATTR_WRITABLE));
  js_set(js, lib, entry->create.name, js_mkfun_meta(&entry->create));
}

ant_value_t zlib_library(ant_t *js) {
  zlib_init_proto(js);

  ant_value_t lib = Ant_Object_CreateBare(js);
  ant_value_t consts = make_constants(js);
  ant_value_t codes = make_codes(js);

  for (int kind = 0; kind < ZLIB_KIND_COUNT; kind++) zlib_define_kind(js, lib, (zlib_kind_t)kind);
  
  js_set(js, lib, "default", lib);
  js_set(js, lib, "constants", consts);
  js_set(js, lib, "codes", codes);
  zlib_install_constants(js, lib);
  js_set(js, lib, "crc32", Ant_Function_CreateWithArity(js_zlib_crc32, 1));
  js_set_tostringtag(js, lib, js_mkstr(js, "zlib", 4));
  
  return lib;
}
