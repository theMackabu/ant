#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "ascii.h"
#include "utils.h"
#include "errors.h"
#include "base64.h"
#include "internal.h"
#include "modules/buffer.h"
#include "buffer_internal.h"

static bool hex_decode_into(
  ant_t *js, ant_value_t source, const char *data, size_t len,
  uint8_t *out, size_t max, size_t *read, size_t *written
) {
  *read = 0; *written = 0;
  if (str_utf16_len(js, source) % 2 != 0) return false;

  size_t count = len / 2 < max ? len / 2 : max;
  size_t i = 0;
  bool ok = true;

  for (; i < count; i++) {
    int hi = ant_ascii_hex_value((unsigned char)data[i * 2]);
    int lo = ant_ascii_hex_value((unsigned char)data[i * 2 + 1]);
    if (hi < 0 || lo < 0) { ok = false; break; }
    out[i] = (uint8_t)((hi << 4) | lo);
  }

  if (ok && count < max && len % 2 != 0) ok = false;
  *read = i * 2; *written = i;
  return ok;
}

static ant_value_t uint8array_from_bytes(ant_t *js, const uint8_t *bytes, size_t len) {
  ArrayBufferData *buffer = create_array_buffer_data(js, len);
  if (!buffer) return js_mkerr(js, "Failed to allocate buffer");
  if (len > 0) memcpy(buffer->data, bytes, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, js->builtins.typedarray_protos[TYPED_ARRAY_UINT8]);
}

static ant_value_t codec_source_arg(ant_native_params_t, const char **str, size_t *len) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return js_mkerr_typed(js, JS_ERR_TYPE, "input argument must be a string");
  *str = js_getstr(js, args[0], len);
  return Ant_Value_Undefined;
}

static ant_value_t js_uint8array_fromHex(ant_params_t) {
  const char *str; size_t len;
  ant_value_t checked = codec_source_arg(js, args, nargs, &str, &len);
  if (is_err(checked)) return checked;

  uint8_t *decoded = malloc(len / 2 + 1);
  if (!decoded) return js_mkerr(js, "Failed to allocate buffer");

  size_t read, written;
  if (!hex_decode_into(js, args[0], str, len, decoded, SIZE_MAX, &read, &written)) {
    free(decoded);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid hex string");
  }

  ant_value_t result = uint8array_from_bytes(js, decoded, written);
  free(decoded);

  return result;
}

static bool base64_option_is(ant_t *js, ant_value_t value, const char *name) {
  size_t len = 0;
  const char *str = vtype(value) == kTypeString ? js_getstr(js, value, &len) : NULL;
  return str && len == strlen(name) && memcmp(str, name, len) == 0;
}

static ant_value_t base64_options(
  ant_t *js, ant_value_t options, bool *url, 
  ant_base64_last_chunk_t *last_chunk, bool *omit_padding
) {
  *url = false;
  
  if (last_chunk) *last_chunk = ANT_BASE64_LOOSE;
  if (omit_padding) *omit_padding = false;
  
  if (vtype(options) == kTypeUndefined) return Ant_Value_Undefined;
  if (!is_object_type(options)) return js_mkerr_typed(js, JS_ERR_TYPE, "options must be an object");

  ant_value_t alphabet = js_get(js, options, "alphabet");
  if (base64_option_is(js, alphabet, "base64url")) *url = true;
  else if (vtype(alphabet) != kTypeUndefined && !base64_option_is(js, alphabet, "base64"))
    return js_mkerr_typed(js, JS_ERR_TYPE, "alphabet must be \"base64\" or \"base64url\"");

  if (last_chunk) {
    ant_value_t handling = js_get(js, options, "lastChunkHandling");
    if (base64_option_is(js, handling, "strict")) *last_chunk = ANT_BASE64_STRICT;
    else if (base64_option_is(js, handling, "stop-before-partial")) *last_chunk = ANT_BASE64_STOP_BEFORE_PARTIAL;
    else if (vtype(handling) != kTypeUndefined && !base64_option_is(js, handling, "loose"))
      return js_mkerr_typed(js, JS_ERR_TYPE, "lastChunkHandling must be \"loose\", \"strict\", or \"stop-before-partial\"");
  }

  if (omit_padding) *omit_padding = js_truthy(js, js_get(js, options, "omitPadding"));
  return Ant_Value_Undefined;
}

static ant_value_t js_uint8array_fromBase64(ant_params_t) {
  const char *str; size_t len;
  ant_value_t checked = codec_source_arg(js, args, nargs, &str, &len);
  if (is_err(checked)) return checked;

  bool url; ant_base64_last_chunk_t last_chunk;
  checked = base64_options(js, nargs > 1 ? args[1] : Ant_Value_Undefined, &url, &last_chunk, NULL);
  if (is_err(checked)) return checked;

  size_t cap = (len / 4) * 3 + 3, read = 0, written = 0;
  uint8_t *decoded = malloc(cap);
  if (!decoded) return js_mkerr(js, "Failed to allocate buffer");

  const char *error = NULL;
  if (!ant_base64_decode_into(str, len, url, last_chunk, decoded, cap, &read, &written, &error)) {
    free(decoded);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "%s", error);
  }

  ant_value_t result = uint8array_from_bytes(js, decoded, written);
  free(decoded);
  
  return result;
}

static ant_value_t js_uint8array_toHex(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array.prototype.toHex called on incompatible receiver");
  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &len))
    return js_mkerr(js, "Cannot read from detached Uint8Array");

  char *hex = malloc(len * 2 + 1);
  
  if (!hex) return js_mkerr(js, "Failed to allocate hex string");
  hex_encode(data, len, hex);

  ant_value_t result = js_mkstr(js, hex, len * 2);
  free(hex);
  
  return result;
}

static ant_value_t js_uint8array_toBase64(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array.prototype.toBase64 called on incompatible receiver");
  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &len))
    return js_mkerr(js, "Cannot read from detached Uint8Array");

  bool url, omit_padding;
  ant_value_t checked = base64_options(js, nargs > 0 ? args[0] : Ant_Value_Undefined, &url, NULL, &omit_padding);
  if (is_err(checked)) return checked;

  size_t out_len = 0;
  char *encoded = ant_base64_encode(data, len, &out_len);
  if (!encoded) return js_mkerr(js, "Failed to encode base64");

  if (url) for (size_t i = 0; i < out_len; i++) {
    if (encoded[i] == '+') encoded[i] = '-';
    else if (encoded[i] == '/') encoded[i] = '_';
  }
  if (omit_padding) while (out_len > 0 && encoded[out_len - 1] == '=') out_len--;

  ant_value_t result = js_mkstr(js, encoded, out_len);
  free(encoded);
  return result;
}

static ant_value_t uint8array_set_result(ant_t *js, size_t read, size_t written) {
  ant_value_t result = Ant_Object_Create(js);
  js_set(js, result, "read", Ant_Value_FromNumber((double)read));
  js_set(js, result, "written", Ant_Value_FromNumber((double)written));
  return result;
}

static ant_value_t js_uint8array_setFromHex(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array setFrom called on incompatible receiver");

  const char *str; size_t len;
  ant_value_t checked = codec_source_arg(js, args, nargs, &str, &len);
  if (is_err(checked)) return checked;

  uint8_t *data; size_t cap;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &cap)) return js_mkerr(js, "Cannot write to detached Uint8Array");

  size_t read, written;
  if (!hex_decode_into(js, args[0], str, len, data, cap, &read, &written))
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "Invalid hex string");
  return uint8array_set_result(js, read, written);
}

static ant_value_t js_uint8array_setFromBase64(ant_params_t) {
  TypedArrayData *ta_data = buffer_get_typedarray_data(call_this);
  if (!ta_data || ta_data->type != TYPED_ARRAY_UINT8)
    return js_mkerr(js, "Uint8Array setFrom called on incompatible receiver");

  const char *str; size_t len;
  ant_value_t checked = codec_source_arg(js, args, nargs, &str, &len);
  if (is_err(checked)) return checked;

  bool url; ant_base64_last_chunk_t last_chunk;
  checked = base64_options(js, nargs > 1 ? args[1] : Ant_Value_Undefined, &url, &last_chunk, NULL);
  if (is_err(checked)) return checked;

  uint8_t *data; size_t cap;
  if (!Ant_Buffer_GetViewBytes(ta_data, &data, &cap)) return js_mkerr(js, "Cannot write to detached Uint8Array");

  size_t read = 0, written = 0;
  const char *error = NULL;
  if (!ant_base64_decode_into(str, len, url, last_chunk, data, cap, &read, &written, &error))
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "%s", error);
  return uint8array_set_result(js, read, written);
}

void uint8array_codec_initialize(ant_t *js) {
  ant_value_t glob = js->global;
  ant_value_t uint8array_codec_ctor = js_get(js, glob, "Uint8Array");
  ant_value_t uint8array_codec_proto = js_get(js, uint8array_codec_ctor, "prototype");
  
  defmethod(js, uint8array_codec_ctor, "fromHex", 7, Ant_Function_CreateWithArity(js_uint8array_fromHex, 1));
  defmethod(js, uint8array_codec_ctor, "fromBase64", 10, Ant_Function_CreateWithArity(js_uint8array_fromBase64, 1));
  defmethod(js, uint8array_codec_proto, "toHex", 5, Ant_Function_Create(js_uint8array_toHex));
  defmethod(js, uint8array_codec_proto, "toBase64", 8, Ant_Function_Create(js_uint8array_toBase64));
  defmethod(js, uint8array_codec_proto, "setFromHex", 10, Ant_Function_CreateWithArity(js_uint8array_setFromHex, 1));
  defmethod(js, uint8array_codec_proto, "setFromBase64", 13, Ant_Function_CreateWithArity(js_uint8array_setFromBase64, 1));
}
