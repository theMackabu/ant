#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/buffer.h"
#include "modules/textcodec.h"
#include "streams/codec.h"
#include "streams/transform.h"

static constexpr uint32_t TES_NATIVE_TAG = Ant_NativeTag("TEST");
static constexpr uint32_t TDS_NATIVE_TAG = Ant_NativeTag("TDST");

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

static void tes_state_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, TES_NATIVE_TAG));
  js_clear_native(value, TES_NATIVE_TAG);
}

static void tds_state_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  td_state_free(js_get_native(value, TDS_NATIVE_TAG));
  js_clear_native(value, TDS_NATIVE_TAG);
}

static ant_value_t codec_transform_controller(ant_args_t) {
  return (nargs > 1) ? args[1] : Ant_Value_Undefined;
}

static ant_value_t codec_flush_controller(ant_args_t) {
  return (nargs > 0) ? args[0] : Ant_Value_Undefined;
}

static ant_value_t tes_transform(ant_params_t) {
  tes_state_t *st = (tes_state_t *)js_get_native(call_func, TES_NATIVE_TAG);
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextEncoderStream");
  ant_value_t ctrl_obj = codec_transform_controller(args, nargs);
  ant_value_t chunk = (nargs > 0) ? args[0] : Ant_Value_Undefined;
  
  size_t str_len = 0;
  const char *str = NULL;

  if (vtype(chunk) == kTypeString) {
    str = js_getstr(js, chunk, &str_len);
  } else {
    ant_value_t sv = js_tostring_val(js, chunk);
    if (is_err(sv)) return sv;
    str = js_getstr(js, sv, &str_len);
  }
  if (!str) { str = ""; str_len = 0; }

  const uint8_t *s = (const uint8_t *)str;
  size_t total = st->pending_len + str_len;

  if (total == 0) return Ant_Value_Undefined;

  uint8_t *buf = malloc(total);
  if (!buf) return Ant_Error_CreateOutOfMemory(js);

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
    return Ant_Value_Undefined;
  }

  uint8_t *out = malloc(out_len * 4 / 3 + 4);
  if (!out) { free(buf); return Ant_Error_CreateOutOfMemory(js); }

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

  ArrayBufferData *ab = create_array_buffer_data(js, o);
  if (!ab) { free(out); return Ant_Error_CreateOutOfMemory(js); }
  memcpy(ab->data, out, o);
  free(out);

  ant_value_t result = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, o, js->builtins.typedarray_protos[TYPED_ARRAY_UINT8]);
  return ts_is_controller(ctrl_obj)
    ? ts_ctrl_enqueue(js, ctrl_obj, result)
    : js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStreamDefaultController");
}

static ant_value_t tes_flush(ant_params_t) {
  tes_state_t *st = (tes_state_t *)js_get_native(call_func, TES_NATIVE_TAG);
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextEncoderStream");
  ant_value_t ctrl_obj = codec_flush_controller(args, nargs);

  if (st->pending_len > 0) {
    uint8_t fffd[3] = { 0xEF, 0xBF, 0xBD };
    ArrayBufferData *ab = create_array_buffer_data(js, 3);
    if (!ab) return Ant_Error_CreateOutOfMemory(js);
    memcpy(ab->data, fffd, 3);
    st->pending_len = 0;
    
    ant_value_t result = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, 3, js->builtins.typedarray_protos[TYPED_ARRAY_UINT8]);
    if (!ts_is_controller(ctrl_obj))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TransformStreamDefaultController");
    return ts_ctrl_enqueue(js, ctrl_obj, result);
  }

  return Ant_Value_Undefined;
}

static ant_value_t js_tes_get_encoding(ant_params_t) {
  return js_mkstr(js, "utf-8", 5);
}

static ant_value_t js_tes_get_readable(ant_params_t) {
  ant_value_t ts_obj = tes_get_ts(call_this);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextEncoderStream");
  return ts_stream_readable(ts_obj);
}

static ant_value_t js_tes_get_writable(ant_params_t) {
  ant_value_t ts_obj = tes_get_ts(call_this);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextEncoderStream");
  return ts_stream_writable(ts_obj);
}

static ant_value_t js_tes_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TextEncoderStream constructor requires 'new'");

  tes_state_t *st = calloc(1, sizeof(tes_state_t));
  if (!st) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.tes_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, st, TES_NATIVE_TAG);
  js_set_finalizer(obj, tes_state_finalize);

  ant_value_t transformer = Ant_Object_CreateBare(js);
  ant_value_t transform_fn = js_heavy_mkfun_native(js, tes_transform, st, TES_NATIVE_TAG);
  ant_value_t flush_fn = js_heavy_mkfun_native(js, tes_flush, st, TES_NATIVE_TAG);
  js_set(js, transformer, "transform", transform_fn);
  js_set(js, transformer, "flush", flush_fn);

  ant_value_t ts_obj = ts_create(js, transformer, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  if (is_err(ts_obj)) { 
    free(st); 
    return ts_obj; 
  }
  
  js_set_slot(obj, SLOT_ENTRIES, ts_obj);
  js_set_slot_wb(js, transform_fn, SLOT_ENTRIES, obj);
  js_set_slot_wb(js, flush_fn, SLOT_ENTRIES, obj);

  return obj;
}

static ant_value_t tds_transform(ant_params_t) {
  td_state_t *st = (td_state_t *)js_get_native(call_func, TDS_NATIVE_TAG);
  
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoderStream");
  ant_value_t ctrl_obj = codec_transform_controller(args, nargs);

  ant_value_t chunk = (nargs > 0) ? args[0] : Ant_Value_Undefined;
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

  return Ant_Value_Undefined;
}

static ant_value_t tds_flush(ant_params_t) {
  td_state_t *st = (td_state_t *)js_get_native(call_func, TDS_NATIVE_TAG);
  
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

  return Ant_Value_Undefined;
}

static ant_value_t js_tds_get_encoding(ant_params_t) {
  td_state_t *st = (td_state_t *)js_get_native(call_this, TDS_NATIVE_TAG);
  if (!st) return js_mkstr(js, "utf-8", 5);
  const char *name = td_encoding_name(st->encoding);
  return js_mkstr(js, name, strlen(name));
}

static ant_value_t js_tds_get_fatal(ant_params_t) {
  td_state_t *st = (td_state_t *)js_get_native(call_this, TDS_NATIVE_TAG);
  return (st && st->fatal) ? Ant_Value_True : Ant_Value_False;
}

static ant_value_t js_tds_get_ignore_bom(ant_params_t) {
  td_state_t *st = (td_state_t *)js_get_native(call_this, TDS_NATIVE_TAG);
  return (st && st->ignore_bom) ? Ant_Value_True : Ant_Value_False;
}

static ant_value_t js_tds_get_readable(ant_params_t) {
  ant_value_t ts_obj = tds_get_ts(call_this);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoderStream");
  return ts_stream_readable(ts_obj);
}

static ant_value_t js_tds_get_writable(ant_params_t) {
  ant_value_t ts_obj = tds_get_ts(call_this);
  if (!ts_is_stream(ts_obj)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoderStream");
  return ts_stream_writable(ts_obj);
}

static ant_value_t js_tds_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TextDecoderStream constructor requires 'new'");

  td_encoding_t enc = TD_ENC_UTF8;
  if (nargs > 0 && !is_undefined(args[0])) {
    ant_value_t err = td_encoding_from_label(js, args[0], "TextDecoderStream", &enc);
    if (is_err(err)) return err;
  }

  bool fatal = false;
  bool ignore_bom = false;
  if (nargs > 1 && is_object_type(args[1])) {
    ant_value_t fv = js_getprop_fallback(js, args[1], "fatal");
    if (is_err(fv)) return fv;
    if (vtype(fv) != kTypeUndefined) fatal = js_truthy(js, fv);
    ant_value_t bv = js_getprop_fallback(js, args[1], "ignoreBOM");
    if (is_err(bv)) return bv;
    if (vtype(bv) != kTypeUndefined) ignore_bom = js_truthy(js, bv);
  }

  td_state_t *st = td_state_new(enc, fatal, ignore_bom);
  if (!st) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.tds_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, st, TDS_NATIVE_TAG);
  js_set_finalizer(obj, tds_state_finalize);

  ant_value_t transformer = Ant_Object_CreateBare(js);
  ant_value_t transform_fn = js_heavy_mkfun_native(js, tds_transform, st, TDS_NATIVE_TAG);
  ant_value_t flush_fn = js_heavy_mkfun_native(js, tds_flush, st, TDS_NATIVE_TAG);
  
  js_set(js, transformer, "transform", transform_fn);
  js_set(js, transformer, "flush", flush_fn);

  ant_value_t ts_obj = ts_create(js, transformer, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
  if (is_err(ts_obj)) {
    js_clear_native(obj, TDS_NATIVE_TAG);
    td_state_free(st);
    return ts_obj;
  }
  
  js_set_slot(obj, SLOT_ENTRIES, ts_obj);
  js_set_slot_wb(js, transform_fn, SLOT_ENTRIES, obj);
  js_set_slot_wb(js, flush_fn, SLOT_ENTRIES, obj);

  return obj;
}

void init_codec_stream_module(ant_t *js) {
  ant_value_t g = js_glob(js);

  js->builtins.tes_proto = Ant_Object_CreateBare(js);
  js_set_getter_desc(js, js->builtins.tes_proto, "encoding", 8, Ant_Function_Create(js_tes_get_encoding), JS_DESC_C);
  js_set_getter_desc(js, js->builtins.tes_proto, "readable", 8, Ant_Function_Create(js_tes_get_readable), JS_DESC_C);
  js_set_getter_desc(js, js->builtins.tes_proto, "writable", 8, Ant_Function_Create(js_tes_get_writable), JS_DESC_C);
  js_set_tostringtag(js, js->builtins.tes_proto, js_mkstr(js, "TextEncoderStream", 17));

  ant_value_t tes_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_tes_ctor, js->builtins.tes_proto, "TextEncoderStream", 0);
  js_set(js, g, "TextEncoderStream", tes_ctor);
  js_set_descriptor(js, g, "TextEncoderStream", 17, JS_DESC_W | JS_DESC_C);

  js->builtins.tds_proto = Ant_Object_CreateBare(js);
  js_set_getter_desc(js, js->builtins.tds_proto, "encoding",  8, Ant_Function_Create(js_tds_get_encoding),  JS_DESC_C);
  js_set_getter_desc(js, js->builtins.tds_proto, "fatal",     5, Ant_Function_Create(js_tds_get_fatal),     JS_DESC_C);
  js_set_getter_desc(js, js->builtins.tds_proto, "ignoreBOM", 9, Ant_Function_Create(js_tds_get_ignore_bom), JS_DESC_C);
  js_set_getter_desc(js, js->builtins.tds_proto, "readable",  8, Ant_Function_Create(js_tds_get_readable),  JS_DESC_C);
  js_set_getter_desc(js, js->builtins.tds_proto, "writable",  8, Ant_Function_Create(js_tds_get_writable),  JS_DESC_C);
  js_set_tostringtag(js, js->builtins.tds_proto, js_mkstr(js, "TextDecoderStream", 17));

  ant_value_t tds_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_tds_ctor, js->builtins.tds_proto, "TextDecoderStream", 0);
  js_set(js, g, "TextDecoderStream", tds_ctor);
  js_set_descriptor(js, g, "TextDecoderStream", 17, JS_DESC_W | JS_DESC_C);
}
