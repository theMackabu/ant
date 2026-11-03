#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ant.h"
#include "cfunc.h"
#include "utf8.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/textcodec.h"
#include "modules/buffer.h"

static constexpr uint32_t TEXT_DECODER_NATIVE_TAG = Ant_NativeTag("TDEC");

td_state_t *td_state_new(td_encoding_t enc, bool fatal, bool ignore_bom) {
  td_state_t *st = calloc(1, sizeof(td_state_t));
  if (!st) return NULL;
  st->encoding = enc;
  st->fatal = fatal;
  st->ignore_bom = ignore_bom;
  return st;
}

#ifdef ANT_HAVE_ENCODING_RS
typedef struct td_rs_encoding td_rs_encoding_t;
static constexpr uint32_t TD_RS_INPUT_EMPTY = 0;

extern const td_rs_encoding_t *encoding_for_label(const uint8_t *label, size_t label_len);
extern td_rs_decoder_t *encoding_new_decoder_with_bom_removal(const td_rs_encoding_t *encoding);
extern td_rs_decoder_t *encoding_new_decoder_without_bom_handling(const td_rs_encoding_t *encoding);

extern void decoder_free(td_rs_decoder_t *decoder);
extern size_t decoder_max_utf8_buffer_length(const td_rs_decoder_t *decoder, size_t byte_length);
extern size_t decoder_max_utf8_buffer_length_without_replacement(const td_rs_decoder_t *decoder, size_t byte_length);

extern uint32_t decoder_decode_to_utf8(
  td_rs_decoder_t *decoder, const uint8_t *src, size_t *src_len,
  uint8_t *dst, size_t *dst_len, bool last, bool *had_replacements
);

extern uint32_t decoder_decode_to_utf8_without_replacement(
  td_rs_decoder_t *decoder, const uint8_t *src, size_t *src_len,
  uint8_t *dst, size_t *dst_len, bool last
);
#endif

void td_state_free(td_state_t *st) {
  if (!st) return;
#ifdef ANT_HAVE_ENCODING_RS
  if (st->rs) decoder_free(st->rs);
#endif
  free(st);
}

static td_state_t *td_get_state(ant_value_t obj) {
  return (td_state_t *)js_get_native(obj, TEXT_DECODER_NATIVE_TAG);
}

static void td_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  td_state_free(js_get_native(value, TEXT_DECODER_NATIVE_TAG));
  js_clear_native(value, TEXT_DECODER_NATIVE_TAG);
}

typedef struct {
  const char *label;
  uint8_t len;
  td_encoding_t enc;
} td_label_t;

static const td_label_t td_labels[] = {
#define TD_LABEL(label, id) { label, sizeof(label) - 1, TD_ENC_##id },
#include "textcodec/labels.h"
};

static const char *const td_encoding_names[TD_ENC_COUNT] = {
#define TD_ENCODING(id, name) [TD_ENC_##id] = name,
#include "textcodec/encodings.h"
};

static bool td_encoding_decodable(td_encoding_t enc) {
#ifdef ANT_HAVE_ENCODING_RS
  return enc != TD_ENC_REPLACEMENT;
#else
  return enc == TD_ENC_UTF8;
#endif
}

const char *td_encoding_name(td_encoding_t enc) {
  return enc < TD_ENC_COUNT ? td_encoding_names[enc] : "utf-8";
}

static bool td_label_equals(const char *s, size_t len, const td_label_t *entry) {
  if (len != entry->len) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
    if (c != (unsigned char)entry->label[i]) return false;
  }
  return true;
}

ant_value_t td_encoding_from_label(ant_t *js, ant_value_t label, const char *ctor, td_encoding_t *out) {
  ant_value_t str = vtype(label) == kTypeString ? label : coerce_to_str(js, label);
  if (is_err(str)) return str;

  size_t len = 0;
  const char *s = js_getstr(js, str, &len);
  if (!s) { s = ""; len = 0; }
  while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\f' || *s == '\r')) { s++; len--; }
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\f' || s[len - 1] == '\r')) len--;

  for (size_t i = 0; i < sizeof(td_labels) / sizeof(td_labels[0]); i++) {
    if (!td_label_equals(s, len, &td_labels[i])) continue;
    if (!td_encoding_decodable(td_labels[i].enc)) break;
    *out = td_labels[i].enc;
    return Ant_Value_Undefined;
  }

  return js_mkerr_typed(
    js, JS_ERR_RANGE, "Failed to construct '%s': The encoding label provided ('%.*s') is invalid.",
    ctor, (int)len, s
  );
}

static ant_value_t js_textencoder_get_encoding(ant_params_t) {
  return js_mkstr(js, "utf-8", 5);
}

ant_value_t te_encode(ant_t *js, const char *str, size_t str_len) {
  ArrayBufferData *ab = create_array_buffer_data(js, str_len);
  if (!ab) return Ant_Error_CreateOutOfMemory(js);
  size_t written = utf8_export_into(str, str_len, ab->data, str_len, NULL);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, written, js->builtins.typedarray_protos[TYPED_ARRAY_UINT8]);
}

static ant_value_t js_textencoder_encode(ant_params_t) {
  size_t str_len = 0;
  const char *str = "";
  
  if (nargs > 0 && vtype(args[0]) == kTypeString) {
    str = js_getstr(js, args[0], &str_len);
    if (!str) { str = ""; str_len = 0; }
  } else if (nargs > 0 && vtype(args[0]) != kTypeUndefined) {
    ant_value_t sv = js_tostring_val(js, args[0]);
    if (is_err(sv)) return sv;
    str = js_getstr(js, sv, &str_len);
    if (!str) { str = ""; str_len = 0; }
  }
  
  return te_encode(js, str, str_len);
}

static ant_value_t js_textencoder_encode_into(ant_params_t) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "encodeInto requires 2 arguments");

  size_t str_len = 0;
  const char *str = "";
  if (vtype(args[0]) == kTypeString) {
    str = js_getstr(js, args[0], &str_len);
    if (!str) { str = ""; str_len = 0; }
  } else if (vtype(args[0]) != kTypeUndefined) {
    ant_value_t sv = js_tostring_val(js, args[0]);
    if (is_err(sv)) return sv;
    str = js_getstr(js, sv, &str_len);
    if (!str) { str = ""; str_len = 0; }
  }

  TypedArrayData *ta = buffer_get_typedarray_data(args[1]);
  if (!ta) return js_mkerr_typed(js, JS_ERR_TYPE, "Second argument must be a Uint8Array");

  uint8_t *dest; size_t available;
  Ant_Buffer_GetViewBytes(ta, &dest, &available);
  size_t read_units = 0;
  
  size_t written = utf8_export_into(
    str, str_len, dest, 
    available, &read_units
  );

  ant_value_t result = Ant_Object_Create(js);
  js_set(js, result, "read", Ant_Value_FromNumber((double)read_units));
  js_set(js, result, "written", Ant_Value_FromNumber((double)written));
  
  return result;
}

static ant_value_t js_textencoder_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TextEncoder constructor requires 'new'");
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.textencoder_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  return obj;
}

static ant_value_t js_textdecoder_get_encoding(ant_params_t) {
  td_state_t *st = td_get_state(call_this);
  const char *name = td_encoding_name(st ? st->encoding : TD_ENC_UTF8);
  return js_mkstr(js, name, strlen(name));
}

static ant_value_t js_textdecoder_get_fatal(ant_params_t) {
  td_state_t *st = td_get_state(call_this);
  return (st && st->fatal) ? Ant_Value_True : Ant_Value_False;
}

static ant_value_t js_textdecoder_get_ignore_bom(ant_params_t) {
  td_state_t *st = td_get_state(call_this);
  return (st && st->ignore_bom) ? Ant_Value_True : Ant_Value_False;
}

static ant_value_t td_invalid_data_error(ant_t *js, td_state_t *st) {
  char message[64];
  snprintf(message, sizeof(message), "The encoded data was not valid for encoding %s", td_encoding_name(st->encoding));
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_ENCODING_INVALID_ENCODED_DATA", message);
}

static ant_value_t td_decode_utf8(ant_t *js, td_state_t *st, const uint8_t *input, size_t input_len, bool stream_mode) {
  utf8_dec_t dec = { .ignore_bom = st->ignore_bom, .bom_seen = st->bom_seen };
  const uint8_t *p = input;
  
  size_t n = input_len;
  char head[24];
  size_t head_len = 0;

  if (st->pending_len > 0) {
    uint8_t joined[8];
    size_t pend = (size_t)st->pending_len;
    size_t take = n < 3 ? n : 3;
    bool more = take < n;
    
    memcpy(joined, st->pending, pend);
    if (take) memcpy(joined + pend, p, take);
    st->pending_len = 0;
    
    utf8proc_ssize_t r = utf8_whatwg_decode(&dec, joined, pend + take, head, st->fatal, more || stream_mode);
    if (r < 0) return td_invalid_data_error(js, st);
    head_len = (size_t)r;
    
    if (more) {
      take -= (size_t)dec.pend_pos;
      dec.pend_pos = 0;
    }
    
    p += take;
    n -= take;
  }

  if (
    !dec.bom_seen && !dec.ignore_bom && n >= 3 &&
    p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf
  ) {
    p += 3;
    n -= 3;
    dec.bom_seen = true;
  }

  ant_value_t result;
  size_t valid = 0;

  if (head_len == 0) {
    size_t tail = stream_mode ? utf8_truncated_tail((const char *)p, n) : 0;
    result = js_mkstr_utf8_try(js, p, n - tail, &valid);
    if (is_err(result)) return result;
    
    if (vtype(result) != kTypeUndefined) {
      if (tail > 0) {
        st->pending_len = (int)tail;
        memcpy(st->pending, p + n - tail, tail);
      } else {
        st->pending_len = dec.pend_pos;
        memcpy(st->pending, dec.pend_buf, (size_t)dec.pend_pos);
      }
      
      st->bom_seen = stream_mode && (dec.bom_seen || n > tail);
      return result;
    }
  }

  if (valid > 0) dec.bom_seen = true;
  char *out = malloc(head_len + valid + (n - valid) * 3 + 1);
  if (!out) return js_mkerr(js, "out of memory");

  memcpy(out, head, head_len);
  memcpy(out + head_len, p, valid);

  utf8proc_ssize_t r = utf8_whatwg_decode(
    &dec, p + valid, n - valid,
    out + head_len + valid, st->fatal, stream_mode
  );

  if (r < 0) {
    free(out);
    return td_invalid_data_error(js, st);
  }

  size_t out_len = head_len + valid + (size_t)r;
  result = out_len == valid ? js_mkstr_utf8(js, out, out_len) : js_mkstr_utf8_valid(js, out, out_len, false);
  free(out);

  st->pending_len = dec.pend_pos;
  memcpy(st->pending, dec.pend_buf, (size_t)dec.pend_pos);
  st->bom_seen = stream_mode ? dec.bom_seen : false;
  
  return result;
}

#ifdef ANT_HAVE_ENCODING_RS
static ant_value_t td_decode_rs(ant_t *js, td_state_t *st, const uint8_t *input, size_t input_len, bool stream_mode) {
  static const uint8_t empty = 0;
  if (!input) input = &empty;

  if (!st->rs) {
    const char *name = td_encoding_names[st->encoding];
    const td_rs_encoding_t *encoding = encoding_for_label((const uint8_t *)name, strlen(name));
    if (!encoding) return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported encoding %s", name);
    st->rs = st->ignore_bom
      ? encoding_new_decoder_without_bom_handling(encoding)
      : encoding_new_decoder_with_bom_removal(encoding);
  }

  size_t cap = st->fatal
    ? decoder_max_utf8_buffer_length_without_replacement(st->rs, input_len)
    : decoder_max_utf8_buffer_length(st->rs, input_len);
  if (cap == SIZE_MAX) return Ant_Error_CreateOutOfMemory(js);

  uint8_t *out = malloc(cap ? cap : 1);
  if (!out) return Ant_Error_CreateOutOfMemory(js);

  bool last = !stream_mode;
  size_t src_len = input_len, dst_len = cap;
  uint32_t status;

  if (st->fatal) status = decoder_decode_to_utf8_without_replacement(st->rs, input, &src_len, out, &dst_len, last);
  else {
    bool had_replacements = false;
    status = decoder_decode_to_utf8(st->rs, input, &src_len, out, &dst_len, last, &had_replacements);
  }

  if (status != TD_RS_INPUT_EMPTY || last) {
    decoder_free(st->rs);
    st->rs = NULL;
  }

  if (status != TD_RS_INPUT_EMPTY) {
    free(out);
    return td_invalid_data_error(js, st);
  }

  ant_value_t result = js_mkstr(js, (const char *)out, dst_len);
  free(out);
  return result;
}
#endif

ant_value_t td_decode(ant_t *js, td_state_t *st, const uint8_t *input, size_t input_len, bool stream_mode) {
  if (st->encoding != TD_ENC_UTF8) {
#ifdef ANT_HAVE_ENCODING_RS
    return td_decode_rs(js, st, input, input_len, stream_mode);
#else
    return js_mkerr_typed(js, JS_ERR_TYPE, "The encoding %s is not available in this build", td_encoding_name(st->encoding));
#endif
  }

  if ((size_t)st->pending_len + input_len == 0) {
    if (!stream_mode) st->bom_seen = false;
    return js_mkstr(js, "", 0);
  }

  return td_decode_utf8(js, st, input, input_len, stream_mode);
}

static ant_value_t js_textdecoder_decode(ant_params_t) {
  td_state_t *st = td_get_state(call_this);
  if (!st) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid TextDecoder");

  bool stream_mode = false;
  if (nargs > 1 && is_object_type(args[1])) {
    ant_value_t sv = js_get(js, args[1], "stream");
    stream_mode = js_truthy(js, sv);
  }

  const uint8_t *input = NULL;
  size_t input_len = 0;
  if (nargs > 0 && is_object_type(args[0]))
    buffer_source_get_bytes(js, args[0], &input, &input_len);

  return td_decode(js, st, input, input_len, stream_mode);
}

static ant_value_t js_textdecoder_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "TextDecoder constructor requires 'new'");

  td_encoding_t enc = TD_ENC_UTF8;
  if (nargs > 0 && !is_undefined(args[0])) {
    ant_value_t err = td_encoding_from_label(js, args[0], "TextDecoder", &enc);
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
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.textdecoder_proto, call_new_target);
  
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, st, TEXT_DECODER_NATIVE_TAG);
  js_set_finalizer(obj, td_finalize);
  
  return obj;
}

void init_textcodec_module(ant_t *js) {
  ant_value_t g = js_glob(js);

  js->builtins.textencoder_proto = Ant_Object_CreateBare(js);
  js_set_getter_desc(js, js->builtins.textencoder_proto, "encoding", 8, Ant_Function_Create(js_textencoder_get_encoding), JS_DESC_C);
  js_set(js, js->builtins.textencoder_proto, "encode",     Ant_Function_Create(js_textencoder_encode));
  js_set(js, js->builtins.textencoder_proto, "encodeInto", Ant_Function_CreateWithArity(js_textencoder_encode_into, 2));
  js_set_tostringtag(js, js->builtins.textencoder_proto, js_mkstr(js, "TextEncoder", 11));
  
  ant_value_t te_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_textencoder_ctor, js->builtins.textencoder_proto, "TextEncoder", 0);
  js_set(js, g, "TextEncoder", te_ctor);
  js_set_descriptor(js, g, "TextEncoder", 11, JS_DESC_W | JS_DESC_C);

  js->builtins.textdecoder_proto = Ant_Object_CreateBare(js);
  js_set_getter_desc(js, js->builtins.textdecoder_proto, "encoding",  8, Ant_Function_Create(js_textdecoder_get_encoding),  JS_DESC_C);
  js_set_getter_desc(js, js->builtins.textdecoder_proto, "fatal",     5, Ant_Function_Create(js_textdecoder_get_fatal),     JS_DESC_C);
  js_set_getter_desc(js, js->builtins.textdecoder_proto, "ignoreBOM", 9, Ant_Function_Create(js_textdecoder_get_ignore_bom), JS_DESC_C);
  js_set(js, js->builtins.textdecoder_proto, "decode", Ant_Function_Create(js_textdecoder_decode));
  js_set_tostringtag(js, js->builtins.textdecoder_proto, js_mkstr(js, "TextDecoder", 11));
  
  ant_value_t td_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_textdecoder_ctor, js->builtins.textdecoder_proto, "TextDecoder", 0);
  js_set(js, g, "TextDecoder", td_ctor);
  js_set_descriptor(js, g, "TextDecoder", 11, JS_DESC_W | JS_DESC_C);
}
