#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/symbol.h"
#include "modules/buffer.h"
#include "modules/textcodec.h"
#include "streams/codec.h"
#include "streams/transform.h"

ant_value_t g_tes_proto;
ant_value_t g_tds_proto;

enum {
  TES_NATIVE_TAG = 0x54455354u, // TEST
  TDS_NATIVE_TAG = 0x54445354u  // TDST
};

typedef struct {
  uint8_t pending[3];
  uint8_t pending_len;
} tes_state_t;

static ant_value_t tes_get_ts(ant_value_t obj) {
  return js_get_slot(obj, SLOT_ENTRIES);
}

static ant_value_t tds_get_ts(ant_value_t obj) {
  return js_get_slot(obj, SLOT_ENTRIES);
}

bool tes_is_stream(ant_value_t obj) {
  return is_object_type(obj)
    && js_check_native_tag(obj, TES_NATIVE_TAG)
    && ts_is_stream(tes_get_ts(obj));
}

bool tds_is_stream(ant_value_t obj) {
  return is_object_type(obj)
    && js_check_native_tag(obj, TDS_NATIVE_TAG)
    && ts_is_stream(tds_get_ts(obj));
}

ant_value_t tes_stream_readable(ant_value_t obj) {
  return ts_stream_readable(tes_get_ts(obj));
}

ant_value_t tes_stream_writable(ant_value_t obj) {
  return ts_stream_writable(tes_get_ts(obj));
}

ant_value_t tds_stream_readable(ant_value_t obj) {
  return ts_stream_readable(tds_get_ts(obj));
}

ant_value_t tds_stream_writable(ant_value_t obj) {
  return ts_stream_writable(tds_get_ts(obj));
}

static void tes_state_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, TES_NATIVE_TAG));
  js_clear_native(value, TES_NATIVE_TAG);
}

static void tds_state_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, TDS_NATIVE_TAG));
  js_clear_native(value, TDS_NATIVE_TAG);
}

static ant_value_t codec_transform_controller(ant_value_t *args, int nargs) {
  return (nargs > 1) ? args[1] : js_mkundef();
}

static ant_value_t codec_flush_controller(ant_value_t *args, int nargs) {
  return (nargs > 0) ? args[0] : js_mkundef();
}

static ant_value_t tes_transform(ant_t *js, ant_value_t *args, int nargs) {
  tes_state_t *st = (tes_state_t *)js_get_native(js->current_func, TES_NATIVE_TAG);
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextEncoderStream");
  ant_value_t ctrl_obj = codec_transform_controller(args, nargs);
  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  
  size_t str_len = 0;
  const char *str = NULL;

  if (vtype(chunk) == T_STR) {
    str = js_getstr(js, chunk, &str_len);
  } else {
    ant_value_t sv = js_tostring_val(js, chunk);
    if (is_err(sv)) return sv;
    str = js_getstr(js, sv, &str_len);
  }
  if (!str) { str = ""; str_len = 0; }

  const uint8_t *s = (const uint8_t *)str;
  size_t total = st->pending_len + str_len;

  if (total == 0) return js_mkundef();

  uint8_t *buf = malloc(total);
  if (!buf) return js_mkerr(js, "out of memory");

  size_t off = 0;
  if (st->pending_len > 0) {
    memcpy(buf, st->pending, st->pending_len);
    off = st->pending_len;
    st->pending_len = 0;
  }
  memcpy(buf + off, s, str_len);

  size_t out_len = total;
  if (out_len >= 3) {
  uint8_t b0 = buf[out_len - 3], b1 = buf[out_len - 2], b2 = buf[out_len - 1];
  if (b0 == 0xED && b1 >= 0xA0 && b1 <= 0xAF) {
    st->pending[0] = b0;
    st->pending[1] = b1;
    st->pending[2] = b2;
    st->pending_len = 3;
    out_len -= 3;
  }}

  if (out_len == 0) {
    free(buf);
    return js_mkundef();
  }

  uint8_t *out = malloc(out_len * 4 / 3 + 4);
  if (!out) { free(buf); return js_mkerr(js, "out of memory"); }

  size_t i = 0, o = 0;
  while (i < out_len) {
  if (i + 5 < out_len &&
      buf[i] == 0xED && buf[i+1] >= 0xA0 && buf[i+1] <= 0xAF &&
      buf[i+3] == 0xED && buf[i+4] >= 0xB0 && buf[i+4] <= 0xBF) {
    uint32_t hi_cp = ((uint32_t)0x0D << 12) | ((uint32_t)(buf[i+1] & 0x3F) << 6) | (buf[i+2] & 0x3F);
    uint32_t lo_cp = ((uint32_t)0x0D << 12) | ((uint32_t)(buf[i+4] & 0x3F) << 6) | (buf[i+5] & 0x3F);
    uint32_t cp = 0x10000 + ((hi_cp - 0xD800) << 10) + (lo_cp - 0xDC00);
    out[o++] = (uint8_t)(0xF0 | (cp >> 18));
    out[o++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[o++] = (uint8_t)(0x80 | (cp & 0x3F));
    i += 6;
  } else if (buf[i] == 0xED && i + 2 < out_len && buf[i+1] >= 0xA0 && buf[i+1] <= 0xBF) {
    out[o++] = 0xEF; out[o++] = 0xBF; out[o++] = 0xBD;
    i += 3;
  } else out[o++] = buf[i++]; }

  free(buf);

  ArrayBufferData *ab = create_array_buffer_data(o);
  if (!ab) { free(out); return js_mkerr(js, "out of memory"); }
  memcpy(ab->data, out, o);
  free(out);

  ant_value_t result = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, o, "Uint8Array");
  return ts_is_controller(ctrl_obj)
    ? ts_ctrl_enqueue(js, ctrl_obj, result)
    : js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStreamDefaultController");
}

static ant_value_t tes_flush(ant_t *js, ant_value_t *args, int nargs) {
  tes_state_t *st = (tes_state_t *)js_get_native(js->current_func, TES_NATIVE_TAG);
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextEncoderStream");
  ant_value_t ctrl_obj = codec_flush_controller(args, nargs);

  if (st->pending_len > 0) {
    uint8_t fffd[3] = { 0xEF, 0xBF, 0xBD };
    ArrayBufferData *ab = create_array_buffer_data(3);
    if (!ab) return js_mkerr(js, "out of memory");
    memcpy(ab->data, fffd, 3);
    st->pending_len = 0;
    
    ant_value_t result = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, 3, "Uint8Array");
    if (!ts_is_controller(ctrl_obj))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStreamDefaultController");
    return ts_ctrl_enqueue(js, ctrl_obj, result);
  }

  return js_mkundef();
}

static ant_value_t js_tes_get_encoding(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkstr(js, "utf-8", 5);
}

static ant_value_t js_tes_get_readable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = tes_get_ts(js->this_val);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextEncoderStream");
  return ts_stream_readable(ts_obj);
}

static ant_value_t js_tes_get_writable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = tes_get_ts(js->this_val);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextEncoderStream");
  return ts_stream_writable(ts_obj);
}

static ant_value_t js_tes_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TextEncoderStream constructor requires 'new'");

  tes_state_t *st = calloc(1, sizeof(tes_state_t));
  if (!st) return js_mkerr(js, "out of memory");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_tes_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, st, TES_NATIVE_TAG);
  js_set_finalizer(obj, tes_state_finalize);

  ant_value_t transformer = js_mkobj(js);
  ant_value_t transform_fn = js_heavy_mkfun_native(js, tes_transform, st, TES_NATIVE_TAG);
  ant_value_t flush_fn = js_heavy_mkfun_native(js, tes_flush, st, TES_NATIVE_TAG);
  js_set(js, transformer, "transform", transform_fn);
  js_set(js, transformer, "flush", flush_fn);

  ant_value_t ctor_args[1] = { transformer };
  ant_value_t saved_new_target = js->new_target;
  ant_value_t saved_this = js->this_val;
  js->new_target = js_mknum(1);

  ant_value_t ts_obj = js_ts_ctor(js, ctor_args, 1);
  js->new_target = saved_new_target;
  js->this_val = saved_this;

  if (is_err(ts_obj)) { free(st); return ts_obj; }
  js_set_slot(obj, SLOT_ENTRIES, ts_obj);
  js_set_slot_wb(js, transform_fn, SLOT_ENTRIES, obj);
  js_set_slot_wb(js, flush_fn, SLOT_ENTRIES, obj);

  return obj;
}

static ant_value_t tds_transform(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = (td_state_t *)js_get_native(js->current_func, TDS_NATIVE_TAG);
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoderStream");
  ant_value_t ctrl_obj = codec_transform_controller(args, nargs);

  ant_value_t chunk = (nargs > 0) ? args[0] : js_mkundef();
  const uint8_t *input = NULL;
  size_t input_len = 0;

  if (!is_object_type(chunk) || !buffer_source_get_bytes(js, chunk, &input, &input_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");

  ant_value_t result = td_decode(js, st, input, input_len, true);
  if (is_err(result)) return result;

  size_t slen = 0;
  const char *sval = js_getstr(js, result, &slen);
  if (sval && slen > 0) {
    if (!ts_is_controller(ctrl_obj))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStreamDefaultController");
    return ts_ctrl_enqueue(js, ctrl_obj, result);
  }

  return js_mkundef();
}

static ant_value_t tds_flush(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = (td_state_t *)js_get_native(js->current_func, TDS_NATIVE_TAG);
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoderStream");
  ant_value_t ctrl_obj = codec_flush_controller(args, nargs);

  ant_value_t result = td_decode(js, st, NULL, 0, false);
  if (is_err(result)) return result;

  size_t slen = 0;
  const char *sval = js_getstr(js, result, &slen);
  if (sval && slen > 0) {
    if (!ts_is_controller(ctrl_obj))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStreamDefaultController");
    return ts_ctrl_enqueue(js, ctrl_obj, result);
  }

  return js_mkundef();
}

static ant_value_t js_tds_get_encoding(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = (td_state_t *)js_get_native(js->this_val, TDS_NATIVE_TAG);
  if (!st) return js_mkstr(js, "utf-8", 5);
  switch (st->encoding) {
    case TD_ENC_UTF16LE: return js_mkstr(js, "utf-16le", 8);
    case TD_ENC_UTF16BE: return js_mkstr(js, "utf-16be", 8);
    case TD_ENC_WINDOWS_1252: return js_mkstr(js, "windows-1252", 12);
    case TD_ENC_ISO_8859_2: return js_mkstr(js, "iso-8859-2", 10);
    default: return js_mkstr(js, "utf-8", 5);
  }
}

static ant_value_t js_tds_get_fatal(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = (td_state_t *)js_get_native(js->this_val, TDS_NATIVE_TAG);
  return (st && st->fatal) ? js_true : js_false;
}

static ant_value_t js_tds_get_ignore_bom(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = (td_state_t *)js_get_native(js->this_val, TDS_NATIVE_TAG);
  return (st && st->ignore_bom) ? js_true : js_false;
}

static ant_value_t js_tds_get_readable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = tds_get_ts(js->this_val);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoderStream");
  return ts_stream_readable(ts_obj);
}

static ant_value_t js_tds_get_writable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ts_obj = tds_get_ts(js->this_val);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoderStream");
  return ts_stream_writable(ts_obj);
}

static const char *tds_trim_label(const char *s, size_t len, size_t *out_len) {
  while (len > 0 && (unsigned char)*s <= 0x20) { s++; len--; }
  while (len > 0 && (unsigned char)s[len - 1] <= 0x20) { len--; }
  *out_len = len;
  return s;
}

static int tds_resolve_encoding(const char *s, size_t len) {
  static const struct { const char *label; uint8_t label_len; td_encoding_t enc; } map[] = {
    {"unicode-1-1-utf-8", 17, TD_ENC_UTF8},    {"unicode11utf8", 13, TD_ENC_UTF8},
    {"unicode20utf8",     13, TD_ENC_UTF8},    {"utf-8",          5, TD_ENC_UTF8},
    {"utf8",               4, TD_ENC_UTF8},    {"x-unicode20utf8",17, TD_ENC_UTF8},
    {"windows-1252",      12, TD_ENC_WINDOWS_1252}, {"ascii",           5, TD_ENC_WINDOWS_1252},
    {"unicodefffe",       11, TD_ENC_UTF16BE}, {"utf-16be",        8, TD_ENC_UTF16BE},
    {"csunicode",          9, TD_ENC_UTF16LE}, {"iso-10646-ucs-2",16, TD_ENC_UTF16LE},
    {"ucs-2",              5, TD_ENC_UTF16LE}, {"unicode",         7, TD_ENC_UTF16LE},
    {"unicodefeff",       11, TD_ENC_UTF16LE}, {"utf-16",          6, TD_ENC_UTF16LE},
    {"utf-16le",           8, TD_ENC_UTF16LE},
    {"iso-8859-2",        10, TD_ENC_ISO_8859_2},
    {NULL, 0, 0}
  };
  for (int i = 0; map[i].label; i++) {
    if (len == map[i].label_len && strncasecmp(s, map[i].label, len) == 0) return (int)map[i].enc;
  }
  return -1;
}

static ant_value_t js_tds_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TextDecoderStream constructor requires 'new'");

  td_encoding_t enc = TD_ENC_UTF8;
  if (nargs > 0 && !is_undefined(args[0])) {
    ant_value_t label = (vtype(args[0]) == T_STR) ? args[0] : coerce_to_str(js, args[0]);
    if (is_err(label)) return label;
    
    size_t llen;
    const char *raw = js_getstr(js, label, &llen);
    if (raw) {
      size_t tlen;
      const char *trimmed = tds_trim_label(raw, llen, &tlen);
      int resolved = tds_resolve_encoding(trimmed, tlen);
      if (resolved < 0) return js_mkerr_typed(
        js, JS_ERR_RANGE, "Failed to construct 'TextDecoderStream': The encoding label provided ('%.*s') is invalid.",
        (int)tlen, trimmed
      );
      enc = (td_encoding_t)resolved;
    }
  }

  bool fatal = false;
  bool ignore_bom = false;
  if (nargs > 1 && is_object_type(args[1])) {
    ant_value_t fv = js_getprop_fallback(js, args[1], "fatal");
    if (is_err(fv)) return fv;
    if (vtype(fv) != T_UNDEF) fatal = js_truthy(js, fv);
    ant_value_t bv = js_getprop_fallback(js, args[1], "ignoreBOM");
    if (is_err(bv)) return bv;
    if (vtype(bv) != T_UNDEF) ignore_bom = js_truthy(js, bv);
  }

  td_state_t *st = td_state_new(enc, fatal, ignore_bom);
  if (!st) return js_mkerr(js, "out of memory");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_tds_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, st, TDS_NATIVE_TAG);
  js_set_finalizer(obj, tds_state_finalize);

  ant_value_t transformer = js_mkobj(js);
  ant_value_t transform_fn = js_heavy_mkfun_native(js, tds_transform, st, TDS_NATIVE_TAG);
  ant_value_t flush_fn = js_heavy_mkfun_native(js, tds_flush, st, TDS_NATIVE_TAG);
  
  js_set(js, transformer, "transform", transform_fn);
  js_set(js, transformer, "flush", flush_fn);

  ant_value_t ctor_args[1] = { transformer };

  ant_value_t saved_new_target = js->new_target;
  ant_value_t saved_this = js->this_val;
  js->new_target = js_mknum(1);

  ant_value_t ts_obj = js_ts_ctor(js, ctor_args, 1);

  js->new_target = saved_new_target;
  js->this_val = saved_this;

  if (is_err(ts_obj)) { free(st); return ts_obj; }
  js_set_slot(obj, SLOT_ENTRIES, ts_obj);
  js_set_slot_wb(js, transform_fn, SLOT_ENTRIES, obj);
  js_set_slot_wb(js, flush_fn, SLOT_ENTRIES, obj);

  return obj;
}

void init_codec_stream_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);

  g_tes_proto = js_mkobj(js);
  js_set_getter_desc(js, g_tes_proto, "encoding", 8, js_mkfun(js_tes_get_encoding), JS_DESC_C);
  js_set_getter_desc(js, g_tes_proto, "readable", 8, js_mkfun(js_tes_get_readable), JS_DESC_C);
  js_set_getter_desc(js, g_tes_proto, "writable", 8, js_mkfun(js_tes_get_writable), JS_DESC_C);
  js_set_sym(js, g_tes_proto, get_toStringTag_sym(), js_mkstr(js, "TextEncoderStream", 17));

  ant_value_t tes_ctor = js_make_ctor(js, js_tes_ctor, g_tes_proto, "TextEncoderStream", 17);
  js_set(js, g, "TextEncoderStream", tes_ctor);
  js_set_descriptor(js, g, "TextEncoderStream", 17, JS_DESC_W | JS_DESC_C);

  g_tds_proto = js_mkobj(js);
  js_set_getter_desc(js, g_tds_proto, "encoding",  8, js_mkfun(js_tds_get_encoding),  JS_DESC_C);
  js_set_getter_desc(js, g_tds_proto, "fatal",     5, js_mkfun(js_tds_get_fatal),     JS_DESC_C);
  js_set_getter_desc(js, g_tds_proto, "ignoreBOM", 9, js_mkfun(js_tds_get_ignore_bom), JS_DESC_C);
  js_set_getter_desc(js, g_tds_proto, "readable",  8, js_mkfun(js_tds_get_readable),  JS_DESC_C);
  js_set_getter_desc(js, g_tds_proto, "writable",  8, js_mkfun(js_tds_get_writable),  JS_DESC_C);
  js_set_sym(js, g_tds_proto, get_toStringTag_sym(), js_mkstr(js, "TextDecoderStream", 17));

  ant_value_t tds_ctor = js_make_ctor(js, js_tds_ctor, g_tds_proto, "TextDecoderStream", 17);
  js_set(js, g, "TextDecoderStream", tds_ctor);
  js_set_descriptor(js, g, "TextDecoderStream", 17, JS_DESC_W | JS_DESC_C);
}

void gc_mark_codec_streams(ant_t *js, void (*mark)(ant_t *, ant_value_t)) {
  mark(js, g_tes_proto);
  mark(js, g_tds_proto);
}
