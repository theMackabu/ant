#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"
#include "utf8.h"

#include "modules/textcodec.h"
#include "modules/buffer.h"
#include "modules/symbol.h"

static ant_value_t g_textencoder_proto = 0;
static ant_value_t g_textdecoder_proto = 0;

td_state_t *td_state_new(td_encoding_t enc, bool fatal, bool ignore_bom) {
  td_state_t *st = calloc(1, sizeof(td_state_t));
  if (!st) return NULL;
  st->encoding = enc;
  st->fatal = fatal;
  st->ignore_bom = ignore_bom;
  return st;
}

static td_state_t *td_get_state(ant_value_t obj) {
  ant_value_t s = js_get_slot(obj, SLOT_DATA);
  if (vtype(s) != T_NUM) return NULL;
  return (td_state_t *)(uintptr_t)(size_t)js_getnum(s);
}

static void td_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
  
  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    free((td_state_t *)(uintptr_t)(size_t)js_getnum(entries[i].value));
    return;
  }}
}

static int resolve_encoding(const char *s, size_t len) {
  static const struct { const char *label; uint8_t len; td_encoding_t enc; } map[] = {
    {"unicode-1-1-utf-8", 18, TD_ENC_UTF8},    {"unicode11utf8", 13, TD_ENC_UTF8},
    {"unicode20utf8",     13, TD_ENC_UTF8},    {"utf-8",          5, TD_ENC_UTF8},
    {"utf8",               4, TD_ENC_UTF8},    {"x-unicode20utf8",17, TD_ENC_UTF8},
    {"unicodefffe",       11, TD_ENC_UTF16BE}, {"utf-16be",        8, TD_ENC_UTF16BE},
    {"csunicode",          9, TD_ENC_UTF16LE}, {"iso-10646-ucs-2",16, TD_ENC_UTF16LE},
    {"ucs-2",              5, TD_ENC_UTF16LE}, {"unicode",         7, TD_ENC_UTF16LE},
    {"unicodefeff",       11, TD_ENC_UTF16LE}, {"utf-16",          6, TD_ENC_UTF16LE},
    {"utf-16le",           8, TD_ENC_UTF16LE},
    {NULL, 0, 0}
  };
  for (int i = 0; map[i].label; i++) {
    if (len == map[i].len && strncasecmp(s, map[i].label, len) == 0) return (int)map[i].enc;
  }
  return -1;
}

static const char *encoding_name(td_encoding_t enc) {
switch (enc) {
  case TD_ENC_UTF16LE: return "utf-16le";
  case TD_ENC_UTF16BE: return "utf-16be";
  default:             return "utf-8";
}}

static const char *trim_label(const char *s, size_t len, size_t *out_len) {
  while (len > 0 && (unsigned char)*s <= 0x20) { s++; len--; }
  while (len > 0 && (unsigned char)s[len - 1] <= 0x20) { len--; }
  *out_len = len;
  return s;
}

static bool get_buffer_source(ant_t *js, ant_value_t arg, const uint8_t **out, size_t *len) {
  ant_value_t slot = js_get_slot(arg, SLOT_BUFFER);
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
  
  ant_value_t buf_prop = js_get(js, arg, "buffer");
  if (is_object_type(buf_prop)) {
  ant_value_t buf_slot = js_get_slot(buf_prop, SLOT_BUFFER);
  
  if (vtype(buf_slot) == T_NUM) {
    ArrayBufferData *ab = (ArrayBufferData *)(uintptr_t)(size_t)js_getnum(buf_slot);
    if (!ab || ab->is_detached) { *out = NULL; *len = 0; return true; }
    ant_value_t off_v = js_get(js, arg, "byteOffset");
    ant_value_t len_v = js_get(js, arg, "byteLength");
    size_t off  = (vtype(off_v) == T_NUM) ? (size_t)js_getnum(off_v) : 0;
    size_t blen = (vtype(len_v) == T_NUM) ? (size_t)js_getnum(len_v) : ab->length - off;
    *out = ab->data + off;
    *len = blen;
    return true;
  }}
  
  return false;
}

static ant_value_t make_ctor(ant_t *js, ant_cfunc_t fn, ant_value_t proto, const char *name, size_t nlen) {
  ant_value_t obj = js_mkobj(js);
  js_set_slot(obj, SLOT_CFUNC, js_mkfun(fn));
  js_mkprop_fast(js, obj, "prototype", 9, proto);
  js_mkprop_fast(js, obj, "name", 4, js_mkstr(js, name, nlen));
  js_set_descriptor(js, obj, "name", 4, 0);
  
  ant_value_t fn_val = js_obj_to_func(obj);
  js_set(js, proto, "constructor", fn_val);
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  return fn_val;
}

static ant_value_t js_textencoder_get_encoding(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkstr(js, "utf-8", 5);
}

ant_value_t te_encode(ant_t *js, const char *str, size_t str_len) {
  ArrayBufferData *ab = create_array_buffer_data(str_len);
  if (!ab) return js_mkerr(js, "out of memory");
  
  if (str_len > 0) {
    const uint8_t *s = (const uint8_t *)str;
    uint8_t *d = ab->data; size_t i = 0;
    
    while (i < str_len) {
    if (s[i] == 0xED && i + 2 < str_len && s[i+1] >= 0xA0 && s[i+1] <= 0xBF) {
      d[i] = 0xEF; d[i+1] = 0xBF; d[i+2] = 0xBD;
      i += 3;
    } else { d[i] = s[i]; i++; }}
  }
  
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, str_len, "Uint8Array");
}

static ant_value_t js_textencoder_encode(ant_t *js, ant_value_t *args, int nargs) {
  size_t str_len = 0;
  const char *str = "";
  
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    str = js_getstr(js, args[0], &str_len);
    if (!str) { str = ""; str_len = 0; }
  } else if (nargs > 0 && vtype(args[0]) != T_UNDEF) {
    ant_value_t sv = js_tostring_val(js, args[0]);
    if (is_err(sv)) return sv;
    str = js_getstr(js, sv, &str_len);
    if (!str) { str = ""; str_len = 0; }
  }
  
  return te_encode(js, str, str_len);
}

static ant_value_t js_textencoder_encode_into(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "encodeInto requires 2 arguments");

  size_t str_len = 0;
  const char *str = "";
  if (vtype(args[0]) == T_STR) {
    str = js_getstr(js, args[0], &str_len);
    if (!str) { str = ""; str_len = 0; }
  } else if (vtype(args[0]) != T_UNDEF) {
    ant_value_t sv = js_tostring_val(js, args[0]);
    if (is_err(sv)) return sv;
    str = js_getstr(js, sv, &str_len);
    if (!str) { str = ""; str_len = 0; }
  }

  TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(js_get_slot(args[1], SLOT_BUFFER));
  if (!ta) return js_mkerr_typed(js, JS_ERR_TYPE, "Second argument must be a Uint8Array");

  uint8_t *dest = (ta->buffer && !ta->buffer->is_detached)
    ? ta->buffer->data + ta->byte_offset : NULL;
  size_t available = ta->byte_length;

  const utf8proc_uint8_t *src = (const utf8proc_uint8_t *)str;
  utf8proc_ssize_t src_len = (utf8proc_ssize_t)str_len;
  utf8proc_ssize_t pos = 0;
  
  size_t written = 0;
  size_t read_units = 0;

  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    utf8proc_uint8_t tmp[4];
    utf8proc_ssize_t enc_len;
    
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      tmp[0] = 0xEF; tmp[1] = 0xBF; tmp[2] = 0xBD;
      enc_len = 3;
    } else {
      enc_len = (cp >= 0) ? utf8proc_encode_char(cp, tmp) : 0;
      if (enc_len <= 0) { tmp[0] = 0xEF; tmp[1] = 0xBF; tmp[2] = 0xBD; enc_len = 3; }
    }
    
    if (written + (size_t)enc_len > available) break;
    if (dest) memcpy(dest + written, tmp, (size_t)enc_len);
    
    written += (size_t)enc_len;
    pos += n;
    read_units += (cp >= 0x10000 && cp <= 0x10FFFF) ? 2 : 1;
  }

  ant_value_t result = js_mkobj(js);
  js_set(js, result, "read", js_mknum((double)read_units));
  js_set(js, result, "written", js_mknum((double)written));
  
  return result;
}

static ant_value_t js_textencoder_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TextEncoder constructor requires 'new'");
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_textencoder_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  return obj;
}

static ant_value_t js_textdecoder_get_encoding(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = td_get_state(js->this_val);
  const char *name = encoding_name(st ? st->encoding : TD_ENC_UTF8);
  return js_mkstr(js, name, strlen(name));
}

static ant_value_t js_textdecoder_get_fatal(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = td_get_state(js->this_val);
  return (st && st->fatal) ? js_true : js_false;
}

static ant_value_t js_textdecoder_get_ignore_bom(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = td_get_state(js->this_val);
  return (st && st->ignore_bom) ? js_true : js_false;
}

static inline uint16_t u16_read(const uint8_t *p, bool be) {
  return be 
    ? (uint16_t)((uint16_t)p[0] << 8 | p[1])
    : (uint16_t)((uint16_t)p[1] << 8 | p[0]);
}

static inline size_t u8_emit(char *out, size_t o, utf8proc_int32_t cp) {
  utf8proc_ssize_t n = utf8proc_encode_char(cp, (utf8proc_uint8_t *)(out + o));
  return n > 0 ? o + (size_t)n : o;
}

static inline size_t u8_fffd(char *out, size_t o) {
  out[o] = (char)0xEF; out[o+1] = (char)0xBF; out[o+2] = (char)0xBD;
  return o + 3;
}

#define U16_IS_HIGH(cu) ((cu) >= 0xD800 && (cu) <= 0xDBFF)
#define U16_IS_LOW(cu)  ((cu) >= 0xDC00 && (cu) <= 0xDFFF)
#define U16_PAIR(hi,lo) (0x10000 + ((uint32_t)((hi) - 0xD800) << 10) + ((lo) - 0xDC00))

static utf8proc_ssize_t utf16_decode(td_state_t *st, const uint8_t *src, size_t len, char *out, bool stream) {
  bool be = (st->encoding == TD_ENC_UTF16BE);
  size_t i = 0, o = 0;
  size_t avail;

  if (!st->bom_seen && len >= 2 && u16_read(src, be) == 0xFEFF && !st->ignore_bom) i = 2;
  st->bom_seen = true;

  while (i < len) {
    avail = len - i;
    
    if (avail < 2) goto pend_tail;
    uint16_t cu = u16_read(src + i, be);
    i += 2;
    
    if (!U16_IS_HIGH(cu) && !U16_IS_LOW(cu)) {
      o = u8_emit(out, o, (utf8proc_int32_t)cu);
      continue;
    }
    
    if (U16_IS_LOW(cu)) goto err;
    
    avail = len - i;
    if (avail < 2) goto pend_hi;
    
    uint16_t lo = u16_read(src + i, be);
    if (U16_IS_LOW(lo)) { i += 2; o = u8_emit(out, o, (utf8proc_int32_t)U16_PAIR(cu, lo)); continue; }
    
    goto err;

  pend_tail:
    if (stream) { st->pending[0] = src[i]; st->pending_len = 1; }
    else { if (st->fatal) return -1; o = u8_fffd(out, o); }
    break;
    
  pend_hi:
    if (stream) { st->pending_len = (int)(len - (i - 2)); memcpy(st->pending, src + i - 2, (size_t)st->pending_len); }
    else { if (st->fatal) return -1; o = u8_fffd(out, o); if (avail == 1) o = u8_fffd(out, o); }
    break;
    
  err:
    if (st->fatal) return -1;
    o = u8_fffd(out, o);
    continue;
  }
  
  return (utf8proc_ssize_t)o;
}

#undef U16_IS_HIGH
#undef U16_IS_LOW
#undef U16_PAIR

ant_value_t td_decode(ant_t *js, td_state_t *st, const uint8_t *input, size_t input_len, bool stream_mode) {
  size_t total = (size_t)st->pending_len + input_len;
  if (total == 0) {
    if (!stream_mode) st->bom_seen = false;
    return js_mkstr(js, "", 0);
  }

  uint8_t *work = NULL;
  const uint8_t *src;
  if (st->pending_len > 0) {
    work = malloc(total);
    if (!work) return js_mkerr(js, "out of memory");
    memcpy(work, st->pending, (size_t)st->pending_len);
    if (input && input_len > 0) memcpy(work + st->pending_len, input, input_len);
    src = work;
  } else src = input;
  st->pending_len = 0;

  char *out = malloc(total * 3 + 1);
  if (!out) { free(work); return js_mkerr(js, "out of memory"); }

  utf8proc_ssize_t n;
  if (st->encoding == TD_ENC_UTF16LE || st->encoding == TD_ENC_UTF16BE) {
    n = utf16_decode(st, src, total, out, stream_mode);
  } else {
    utf8_dec_t dec = { .ignore_bom = st->ignore_bom, .bom_seen = st->bom_seen };
    n = utf8_whatwg_decode(&dec, src, total, out, st->fatal, stream_mode);
    st->pending_len = dec.pend_pos;
    memcpy(st->pending, dec.pend_buf, (size_t)dec.pend_pos);
    st->bom_seen = stream_mode ? dec.bom_seen : false;
  }

  if (n < 0) {
    free(work); free(out);
    return js_mkerr_typed(js, JS_ERR_TYPE, "The encoded data was not valid.");
  }

  if (st->encoding != TD_ENC_UTF8) {
    if (!stream_mode) st->bom_seen = false;
  }

  ant_value_t result = js_mkstr(js, out, (size_t)n);
  free(work);
  free(out);
  
  return result;
}

static ant_value_t js_textdecoder_decode(ant_t *js, ant_value_t *args, int nargs) {
  td_state_t *st = td_get_state(js->this_val);
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoder");

  bool stream_mode = false;
  if (nargs > 1 && is_object_type(args[1])) {
    ant_value_t sv = js_get(js, args[1], "stream");
    stream_mode = js_truthy(js, sv);
  }

  const uint8_t *input = NULL;
  size_t input_len = 0;
  if (nargs > 0 && is_object_type(args[0]))
    get_buffer_source(js, args[0], &input, &input_len);

  return td_decode(js, st, input, input_len, stream_mode);
}

static ant_value_t js_textdecoder_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TextDecoder constructor requires 'new'");

  td_encoding_t enc = TD_ENC_UTF8;
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    size_t llen;
    const char *raw = js_getstr(js, args[0], &llen);
    
  if (raw) {
    size_t tlen;
    const char *trimmed = trim_label(raw, llen, &tlen);
    int resolved = resolve_encoding(trimmed, tlen);
    
    if (resolved < 0) return js_mkerr_typed(
      js, JS_ERR_RANGE, "Failed to construct 'TextDecoder': The encoding label provided ('%.*s') is invalid.", 
      (int)tlen, trimmed
    );
    
    enc = (td_encoding_t)resolved;
  }}

  bool fatal = false;
  bool ignore_bom = false;
  
  if (nargs > 1 && is_object_type(args[1])) {
    ant_value_t fv = js_get(js, args[1], "fatal");
    if (vtype(fv) != T_UNDEF) fatal = js_truthy(js, fv);
    ant_value_t bv = js_get(js, args[1], "ignoreBOM");
    if (vtype(bv) != T_UNDEF) ignore_bom = js_truthy(js, bv);
  }

  td_state_t *st = td_state_new(enc, fatal, ignore_bom);
  if (!st) return js_mkerr(js, "out of memory");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_textdecoder_proto);
  
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_slot(obj, SLOT_DATA, ANT_PTR(st));
  js_set_finalizer(obj, td_finalize);
  
  return obj;
}

void init_textcodec_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);

  g_textencoder_proto = js_mkobj(js);
  js_set_getter_desc(js, g_textencoder_proto, "encoding", 8, js_mkfun(js_textencoder_get_encoding), JS_DESC_C);
  js_set(js, g_textencoder_proto, "encode",     js_mkfun(js_textencoder_encode));
  js_set(js, g_textencoder_proto, "encodeInto", js_mkfun(js_textencoder_encode_into));
  js_set_sym(js, g_textencoder_proto, get_toStringTag_sym(), js_mkstr(js, "TextEncoder", 11));
  
  ant_value_t te_ctor = make_ctor(js, js_textencoder_ctor, g_textencoder_proto, "TextEncoder", 11);
  js_set(js, g, "TextEncoder", te_ctor);
  js_set_descriptor(js, g, "TextEncoder", 11, JS_DESC_W | JS_DESC_C);

  g_textdecoder_proto = js_mkobj(js);
  js_set_getter_desc(js, g_textdecoder_proto, "encoding",  8, js_mkfun(js_textdecoder_get_encoding),  JS_DESC_C);
  js_set_getter_desc(js, g_textdecoder_proto, "fatal",     5, js_mkfun(js_textdecoder_get_fatal),     JS_DESC_C);
  js_set_getter_desc(js, g_textdecoder_proto, "ignoreBOM", 9, js_mkfun(js_textdecoder_get_ignore_bom), JS_DESC_C);
  js_set(js, g_textdecoder_proto, "decode", js_mkfun(js_textdecoder_decode));
  js_set_sym(js, g_textdecoder_proto, get_toStringTag_sym(), js_mkstr(js, "TextDecoder", 11));
  
  ant_value_t td_ctor = make_ctor(js, js_textdecoder_ctor, g_textdecoder_proto, "TextDecoder", 11);
  js_set(js, g, "TextDecoder", td_ctor);
  js_set_descriptor(js, g, "TextDecoder", 11, JS_DESC_W | JS_DESC_C);
}
