#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ant.h"
#include "ptr.h"
#include "base64.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/buffer.h"
#include "modules/symbol.h"
#include "modules/textcodec.h"
#include "modules/string_decoder.h"

#define SD_ENC_UTF8     0
#define SD_ENC_UTF16LE  1
#define SD_ENC_UTF16BE  2
#define SD_ENC_LATIN1   3
#define SD_ENC_HEX      4
#define SD_ENC_BASE64   5

typedef struct {
  int encoding;
  td_state_t *td;
  uint8_t pending[2];
  int pending_len;
} sd_state_t;

static ant_value_t g_string_decoder_proto = 0;

enum { STRING_DECODER_NATIVE_TAG = 0x53444543u }; // SDEC

static sd_state_t *sd_get_state(ant_value_t obj) {
  if (!js_check_native_tag(obj, STRING_DECODER_NATIVE_TAG)) return NULL;
  return (sd_state_t *)js_get_native_ptr(obj);
}

static void sd_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj || obj->native.tag != STRING_DECODER_NATIVE_TAG) return;
  sd_state_t *st = (sd_state_t *)obj->native.ptr;
  if (st) { free(st->td); free(st); }
  obj->native.ptr = NULL;
  obj->native.tag = 0;
}

static int sd_parse_encoding(const char *s, size_t len) {
  static const struct { const char *label; uint8_t llen; int enc; } map[] = {
    {"utf8",      4, SD_ENC_UTF8},
    {"utf-8",     5, SD_ENC_UTF8},
    {"utf16le",   7, SD_ENC_UTF16LE},
    {"utf-16le",  8, SD_ENC_UTF16LE},
    {"ucs2",      4, SD_ENC_UTF16LE},
    {"ucs-2",     5, SD_ENC_UTF16LE},
    {"latin1",    6, SD_ENC_LATIN1},
    {"binary",    6, SD_ENC_LATIN1},
    {"ascii",     5, SD_ENC_LATIN1},
    {"hex",       3, SD_ENC_HEX},
    {"base64",    6, SD_ENC_BASE64},
    {"base64url", 9, SD_ENC_BASE64},
    {NULL, 0, 0}
  };
  for (int i = 0; map[i].label; i++) {
    if (len == map[i].llen && strncasecmp(s, map[i].label, len) == 0)
      return map[i].enc;
  }
  return SD_ENC_UTF8;
}

static const char *sd_encoding_name(int enc) {
switch (enc) {
  case SD_ENC_UTF16LE: return "utf-16le";
  case SD_ENC_LATIN1:  return "latin1";
  case SD_ENC_HEX:     return "hex";
  case SD_ENC_BASE64:  return "base64";
  default:             return "utf-8";
}}

static ant_value_t sd_latin1_to_str(ant_t *js, const uint8_t *src, size_t len) {
  char *out = malloc(len * 2 + 1);
  if (!out) return js_mkerr(js, "out of memory");
  size_t o = 0;
  
  for (size_t i = 0; i < len; i++) {
  uint8_t b = src[i];
  
  if (b < 0x80) out[o++] = (char)b;
  else {
    out[o++] = (char)(0xC0 | (b >> 6));
    out[o++] = (char)(0x80 | (b & 0x3F));
  }}
  
  ant_value_t result = js_mkstr(js, out, o);
  free(out);
  
  return result;
}

static ant_value_t sd_hex_decode(ant_t *js, const uint8_t *src, size_t len) {
  static const char hex[] = "0123456789abcdef";
  if (len == 0) return js_mkstr(js, "", 0);
  
  char *out = malloc(len * 2 + 1);
  if (!out) return js_mkerr(js, "out of memory");
  for (size_t i = 0; i < len; i++) {
    out[i*2]   = hex[(src[i] >> 4) & 0xF];
    out[i*2+1] = hex[src[i] & 0xF];
  }
  
  ant_value_t result = js_mkstr(js, out, len * 2);
  free(out);
  
  return result;
}

static ant_value_t sd_base64_write(ant_t *js, sd_state_t *st, const uint8_t *src, size_t len, bool flush) {
  size_t total = (size_t)st->pending_len + len;
  if (total == 0) return js_mkstr(js, "", 0);

  uint8_t *work = malloc(total);
  if (!work) return js_mkerr(js, "out of memory");

  if (st->pending_len > 0)
    memcpy(work, st->pending, (size_t)st->pending_len);
  if (src && len > 0)
    memcpy(work + st->pending_len, src, len);

  size_t encode_len = flush ? total : (total / 3) * 3;
  int new_pending = (int)(total - encode_len);

  ant_value_t result;
  if (encode_len == 0) result = js_mkstr(js, "", 0); else {
    size_t out_len;
    char *out = ant_base64_encode(work, encode_len, &out_len);
    if (!out) { free(work); return js_mkerr(js, "out of memory"); }
    result = js_mkstr(js, out, out_len);
    free(out);
  }

  st->pending_len = new_pending;
  if (new_pending > 0)
    memcpy(st->pending, work + encode_len, (size_t)new_pending);

  free(work);
  return result;
}

static ant_value_t sd_do_write(ant_t *js, sd_state_t *st, const uint8_t *src, size_t len, bool flush) {
switch (st->encoding) {
  case SD_ENC_UTF8:
  case SD_ENC_UTF16LE:
  case SD_ENC_UTF16BE:
    return td_decode(js, st->td, src, len, !flush);
  case SD_ENC_LATIN1:
    if (!src || len == 0) return js_mkstr(js, "", 0);
    return sd_latin1_to_str(js, src, len);
  case SD_ENC_HEX:
    if (!src || len == 0) return js_mkstr(js, "", 0);
    return sd_hex_decode(js, src, len);
  case SD_ENC_BASE64:
    return sd_base64_write(js, st, src, len, flush);
  default:
    return js_mkstr(js, "", 0);
}}

static ant_value_t js_sd_get_encoding(ant_t *js, ant_value_t *args, int nargs) {
  sd_state_t *st = sd_get_state(js->this_val);
  if (!st) return js_mkstr(js, "utf-8", 5);
  const char *name = sd_encoding_name(st->encoding);
  return js_mkstr(js, name, strlen(name));
}

static ant_value_t js_sd_write(ant_t *js, ant_value_t *args, int nargs) {
  sd_state_t *st = sd_get_state(js->this_val);
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid StringDecoder");
  if (nargs < 1) return js_mkstr(js, "", 0);

  if (vtype(args[0]) == T_STR) {
    size_t slen;
    const char *s = js_getstr(js, args[0], &slen);
    return s ? js_mkstr(js, s, slen) : js_mkstr(js, "", 0);
  }

  const uint8_t *src = NULL;
  size_t len = 0;
  if (is_object_type(args[0]))
    buffer_source_get_bytes(js, args[0], &src, &len);

  return sd_do_write(js, st, src, len, false);
}

static ant_value_t js_sd_end(ant_t *js, ant_value_t *args, int nargs) {
  sd_state_t *st = sd_get_state(js->this_val);
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid StringDecoder");

  const uint8_t *src = NULL;
  size_t len = 0;
  if (nargs > 0 && is_object_type(args[0]))
    buffer_source_get_bytes(js, args[0], &src, &len);

  return sd_do_write(js, st, src, len, true);
}

ant_value_t string_decoder_create(ant_t *js, ant_value_t encoding) {
  int enc = SD_ENC_UTF8;
  if (!is_undefined(encoding)) {
  ant_value_t label_val = (vtype(encoding) == T_STR) ? encoding : coerce_to_str(js, encoding);
  if (!is_err(label_val) && vtype(label_val) == T_STR) {
    size_t llen;
    const char *label = js_getstr(js, label_val, &llen);
    if (label) enc = sd_parse_encoding(label, llen);
  }}

  sd_state_t *st = calloc(1, sizeof(sd_state_t));
  if (!st) return js_mkerr(js, "out of memory");
  st->encoding = enc;

  if (enc == SD_ENC_UTF8 || enc == SD_ENC_UTF16LE || enc == SD_ENC_UTF16BE) {
    td_encoding_t td_enc = (enc == SD_ENC_UTF16LE) ? TD_ENC_UTF16LE : (enc == SD_ENC_UTF16BE) ? TD_ENC_UTF16BE : TD_ENC_UTF8;
    st->td = td_state_new(td_enc, false, false);
    if (!st->td) { free(st); return js_mkerr(js, "out of memory"); }
  }

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_string_decoder_proto);
  
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, st, STRING_DECODER_NATIVE_TAG);
  js_set_finalizer(obj, sd_finalize);

  return obj;
}

ant_value_t string_decoder_decode_bytes(
  ant_t *js, ant_value_t decoder,
  const uint8_t *src, size_t len, bool flush
) {
  sd_state_t *st = sd_get_state(decoder);
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid StringDecoder");
  return sd_do_write(js, st, src, len, flush);
}

ant_value_t string_decoder_decode_value(
  ant_t *js, ant_value_t decoder,
  ant_value_t chunk, bool flush
) {
  sd_state_t *st = sd_get_state(decoder);
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid StringDecoder");

  if (vtype(chunk) == T_STR) {
    size_t slen = 0;
    const char *s = js_getstr(js, chunk, &slen);
    return s ? js_mkstr(js, s, slen) : js_mkstr(js, "", 0);
  }

  const uint8_t *src = NULL;
  size_t len = 0;
  if (is_object_type(chunk))
    buffer_source_get_bytes(js, chunk, &src, &len);
    
  return sd_do_write(js, st, src, len, flush);
}

static ant_value_t js_sd_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "StringDecoder constructor requires 'new'");
    
  ant_value_t encoding = nargs > 0 ? args[0] : js_mkundef();
  return string_decoder_create(js, encoding);
}

ant_value_t string_decoder_library(ant_t *js) {
  g_string_decoder_proto = js_mkobj(js);
  
  js_set_getter_desc(js, g_string_decoder_proto, "encoding", 8, js_mkfun(js_sd_get_encoding), JS_DESC_C);
  js_set(js, g_string_decoder_proto, "write", js_mkfun(js_sd_write));
  js_set(js, g_string_decoder_proto, "end",   js_mkfun(js_sd_end));
  js_set_sym(js, g_string_decoder_proto, get_toStringTag_sym(), js_mkstr(js, "StringDecoder", 13));

  ant_value_t ctor = js_make_ctor(js, js_sd_ctor, g_string_decoder_proto, "StringDecoder", 13);
  ant_value_t lib = js_mkobj(js);
  js_set(js, lib, "StringDecoder", ctor);
  
  return lib;
}
