#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "runtime.h"
#include "errors.h"
#include "internal.h"

#include "modules/textcodec.h"
#include "modules/buffer.h"
#include "modules/symbol.h"

// TextEncoder.prototype.encode(string)
static jsval_t js_textencoder_encode(struct js *js, jsval_t *args, int nargs) {
  size_t str_len = 0;
  const char *str = "";
  
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    str = js_getstr(js, args[0], &str_len);
    if (!str) {
      str = "";
      str_len = 0;
    }
  }
  
  jsval_t glob = js_glob(js);
  jsval_t uint8array_ctor = js_get(js, glob, "Uint8Array");
  
  jsval_t len_arg = js_mknum((double)str_len);
  jsval_t arr = js_call(js, uint8array_ctor, &len_arg, 1);
  
  if (vtype(arr) == T_ERR) return arr;
  
  if (str_len > 0) {
    jsval_t ta_data_val = js_get_slot(js, arr, SLOT_BUFFER);
    TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
    if (ta_data && ta_data->buffer && ta_data->buffer->data) memcpy(ta_data->buffer->data, str, str_len);
  }
  
  return arr;
}

// TextEncoder.prototype.encodeInto(string, uint8array)
static jsval_t js_textencoder_encodeInto(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "encodeInto requires string and Uint8Array arguments");
  }
  
  size_t str_len = 0;
  const char *str = "";
  
  if (vtype(args[0]) == T_STR) {
    str = js_getstr(js, args[0], &str_len);
    if (!str) {
      str = "";
      str_len = 0;
    }
  }
  
  jsval_t ta_data_val = js_get_slot(js, args[1], SLOT_BUFFER);
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (!ta_data) return js_mkerr(js, "Second argument must be a Uint8Array");
  
  size_t available = ta_data->byte_length;
  size_t to_write = str_len < available ? str_len : available;
  
  if (to_write > 0) {
    memcpy(ta_data->buffer->data + ta_data->byte_offset, str, to_write);
  }
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "read", js_mknum((double)to_write));
  js_set(js, result, "written", js_mknum((double)to_write));
  
  return result;
}

static jsval_t js_textencoder_constructor(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  
  jsval_t obj = js_mkobj(js);
  js_set(js, obj, "encoding", js_mkstr(js, "utf-8", 5));
  js_set(js, obj, "encode", js_mkfun(js_textencoder_encode));
  js_set(js, obj, "encodeInto", js_mkfun(js_textencoder_encodeInto));
  js_set(js, obj, get_toStringTag_sym_key(), js_mkstr(js, "TextEncoder", 11));
  
  return obj;
}

// TextDecoder.prototype.decode(bufferSource)
static jsval_t js_textdecoder_decode(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkstr(js, "", 0);
  }
  
  jsval_t ta_data_val = js_get_slot(js, args[0], SLOT_BUFFER);
  TypedArrayData *ta_data = (TypedArrayData *)js_gettypedarray(ta_data_val);
  if (ta_data) {
    if (!ta_data->buffer) return js_mkstr(js, "", 0);
    uint8_t *data = ta_data->buffer->data + ta_data->byte_offset;
    size_t len = ta_data->byte_length;
    return js_mkstr(js, (const char *)data, len);
  }
  
  jsval_t ab_data_val = js_get_slot(js, args[0], SLOT_BUFFER);
  if (vtype(ab_data_val) == T_NUM) {
    ArrayBufferData *ab_data = (ArrayBufferData *)(uintptr_t)js_getnum(ab_data_val);
    if (!ab_data || !ab_data->data) return js_mkstr(js, "", 0);    
    return js_mkstr(js, (const char *)ab_data->data, ab_data->length);
  }
  
  return js_mkstr(js, "", 0);
}

static jsval_t js_textdecoder_constructor(struct js *js, jsval_t *args, int nargs) {
  const char *encoding = "utf-8";
  size_t encoding_len = 5;
  
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    encoding = js_getstr(js, args[0], &encoding_len);
    if (encoding && (
        strcasecmp(encoding, "utf-8") == 0 || 
        strcasecmp(encoding, "utf8") == 0)
      ) { encoding = "utf-8"; encoding_len = 5; }
  }
  
  jsval_t obj = js_mkobj(js);
  js_set(js, obj, "encoding", js_mkstr(js, encoding, encoding_len));
  js_set(js, obj, "fatal", js_mkfalse());
  js_set(js, obj, "ignoreBOM", js_mkfalse());
  js_set(js, obj, "decode", js_mkfun(js_textdecoder_decode));
  js_set(js, obj, get_toStringTag_sym_key(), js_mkstr(js, "TextDecoder", 11));
  
  return obj;
}

void init_textcodec_module(void) {
  struct js *js = rt->js;
  jsval_t glob = js_glob(js);
  
  jsval_t textencoder_constructor = js_mkfun(js_textencoder_constructor);
  jsval_t textencoder_proto = js_mkobj(js);
  
  js_set(js, textencoder_proto, "encode", js_mkfun(js_textencoder_encode));
  js_set(js, textencoder_proto, "encodeInto", js_mkfun(js_textencoder_encodeInto));
  js_set(js, textencoder_proto, "encoding", js_mkstr(js, "utf-8", 5));
  js_set(js, textencoder_constructor, "prototype", textencoder_proto);
  js_set(js, glob, "TextEncoder", textencoder_constructor);
  
  jsval_t textdecoder_constructor = js_mkfun(js_textdecoder_constructor);
  jsval_t textdecoder_proto = js_mkobj(js);
  
  js_set(js, textdecoder_proto, "decode", js_mkfun(js_textdecoder_decode));
  js_set(js, textdecoder_proto, "encoding", js_mkstr(js, "utf-8", 5));
  js_set(js, textdecoder_proto, "fatal", js_mkfalse());
  js_set(js, textdecoder_proto, "ignoreBOM", js_mkfalse());
  js_set(js, textdecoder_constructor, "prototype", textdecoder_proto);
  js_set(js, glob, "TextDecoder", textdecoder_constructor);
}
