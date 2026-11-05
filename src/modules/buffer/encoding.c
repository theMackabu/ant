#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "ascii.h"
#include "utf8.h"
#include "utils.h"
#include "errors.h"
#include "base64.h"
#include "internal.h"
#include "gc/roots.h"
#include "modules/buffer.h"
#include "buffer_internal.h"

static uint8_t *hex_decode_node_prefix(const char *data, size_t len, size_t *out_len) {
  size_t max_len = len / 2;
  uint8_t *decoded = malloc(max_len == 0 ? 1 : max_len);
  if (!decoded) return NULL;

  size_t count = 0;
  for (size_t i = 0; i + 1 < len; i += 2) {
    int hi = ant_ascii_hex_value((unsigned char)data[i]);
    int lo = ant_ascii_hex_value((unsigned char)data[i + 1]);
    if (hi < 0 || lo < 0) break;
    decoded[count++] = (uint8_t)((hi << 4) | lo);
  }

  *out_len = count;
  return decoded;
}

BufferEncoding parse_encoding(const char *enc, size_t len) {
  if (len == 3 && strncasecmp(enc, "hex", 3) == 0) return ENC_HEX;
  if (len == 5 && strncasecmp(enc, "ascii", 5) == 0) return ENC_ASCII;
  if (len == 6 && strncasecmp(enc, "base64", 6) == 0) return ENC_BASE64;
  if (len == 9 && strncasecmp(enc, "base64url", 9) == 0) return ENC_BASE64URL;
  if ((len == 4 && strncasecmp(enc, "utf8", 4) == 0) || (len == 5 && strncasecmp(enc, "utf-8", 5) == 0)) return ENC_UTF8;
  if ((len == 6 && strncasecmp(enc, "latin1", 6) == 0) || (len == 6 && strncasecmp(enc, "binary", 6) == 0)) return ENC_LATIN1;
  
  if (
    (len == 4 && strncasecmp(enc, "ucs2", 4) == 0) ||
    (len == 5 && strncasecmp(enc, "ucs-2", 5) == 0) ||
    (len == 7 && strncasecmp(enc, "utf16le", 7) == 0) ||
    (len == 8 && strncasecmp(enc, "utf-16le", 8) == 0)
  ) return ENC_UCS2;
  
  return ENC_UNKNOWN;
}

ant_value_t buffer_unknown_encoding_error(ant_t *js, ant_value_t encoding) {
  ant_value_t str = js_tostring_val(js, encoding);
  if (is_err(str)) return str;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  
  size_t len = 0;
  const char *enc = js_getstr(js, str, &len);
  ant_value_t props = Ant_Object_CreateBare(js);
  
  js_set(js, props, "code", js_mkstr(js, "ERR_UNKNOWN_ENCODING", 20));
  ant_value_t err = js_mkerr_props(js, JS_ERR_TYPE, props, "Unknown encoding: %.*s", (int)len, enc ? enc : "");
  
  GC_ROOT_RESTORE(js, root_mark);
  return err;
}

bool Ant_Buffer_IsEncoding(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeString) return false;
  size_t len = 0;
  const char *enc = js_getstr(js, value, &len);
  return enc && parse_encoding(enc, len) != ENC_UNKNOWN;
}

ant_value_t Ant_Buffer_UnknownEncodingError(ant_t *js, ant_value_t encoding) {
  return buffer_unknown_encoding_error(js, encoding);
}

ant_value_t buffer_require_encoding(ant_t *js, ant_value_t value, BufferEncoding *out) {
  ant_value_t str = vtype(value) == kTypeString ? value : js_tostring_val(js, value);
  if (is_err(str)) return str;
  
  size_t len = 0;
  const char *enc = js_getstr(js, str, &len);
  *out = enc ? parse_encoding(enc, len) : ENC_UNKNOWN;
  
  if (*out == ENC_UNKNOWN) return buffer_unknown_encoding_error(js, str);
  return Ant_Value_Undefined;
}

static size_t buffer_encode_units_into(const char *str, size_t len, uint8_t *dst, size_t cap, bool wide) {
  if (!wide && str_is_ascii(str)) {
    size_t written = len < cap ? len : cap;
    memcpy(dst, str, written);
    return written;
  }

  utf16_iterator_t iterator = utf16_iterator(str, len);
  size_t written = 0, step = wide ? 2 : 1;
  uint16_t unit;
  while (cap - written >= step && utf16_next_code_unit(&iterator, &unit)) {
    dst[written++] = (uint8_t)unit;
    if (wide) dst[written++] = (uint8_t)(unit >> 8);
  }
  return written;
}

ant_value_t buffer_decode_binary(
  ant_t *js, const char *str, size_t len, BufferEncoding encoding, uint8_t **out, size_t *out_len
) {
  if (encoding == ENC_HEX) {
    *out = hex_decode_node_prefix(str, len, out_len);
    return *out ? Ant_Value_Undefined : js_mkerr(js, "Failed to allocate buffer");
  }
  *out = ant_base64_decode_loose(str, len, out_len);
  return *out ? Ant_Value_Undefined : js_mkerr_typed(js, JS_ERR_TYPE, "Invalid base64 string");
}

ant_value_t buffer_encode_into(
  ant_t *js, const char *str, size_t len, BufferEncoding encoding,
  uint8_t *dst, size_t cap, size_t *written
) {
  if (buffer_encoding_is_binary(encoding)) {
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    ant_value_t result = buffer_decode_binary(js, str, len, encoding, &decoded, &decoded_len);
    if (is_err(result)) return result;
    *written = decoded_len < cap ? decoded_len : cap;
    memcpy(dst, decoded, *written);
    free(decoded);
    return Ant_Value_Undefined;
  }

  if (encoding == ENC_UTF8) *written = utf8_export_into(str, len, dst, cap, NULL);
  else *written = buffer_encode_units_into(str, len, dst, cap, encoding == ENC_UCS2);
  return Ant_Value_Undefined;
}

size_t buffer_encode_capacity(ant_t *js, ant_value_t str_value, size_t len, BufferEncoding encoding) {
  if (encoding == ENC_UTF8) return len;
  size_t units = (size_t)str_utf16_len(js, str_value);
  return encoding == ENC_UCS2 ? units * 2 : units;
}

ant_value_t buffer_encode_string(ant_t *js, ant_value_t value, BufferEncoding encoding, const uint8_t **out, size_t *out_len, uint8_t **owned) {
  ant_value_t str_value = js_tostring_val(js, value);
  if (is_err(str_value)) return str_value;

  size_t len = 0;
  const char *str = js_getstr(js, str_value, &len);
  *owned = NULL;

  if (encoding == ENC_UTF8 && str_is_valid_utf8(str)) {
    *out = (const uint8_t *)str;
    *out_len = len;
    return Ant_Value_Undefined;
  }

  if (buffer_encoding_is_binary(encoding)) {
    ant_value_t result = buffer_decode_binary(js, str, len, encoding, owned, out_len);
    *out = *owned;
    return result;
  }

  size_t cap = buffer_encode_capacity(js, str_value, len, encoding);
  *owned = malloc(cap ? cap : 1);
  if (!*owned) return js_mkerr(js, "Failed to allocate buffer");
  *out = *owned;
  return buffer_encode_into(js, str, len, encoding, *owned, cap, out_len);
}

size_t buffer_encoded_length(ant_t *js, ant_value_t str_value, const char *str, size_t len, BufferEncoding encoding) {
  if (encoding == ENC_UTF8) return utf8_export_length(str, len);
  size_t units = (size_t)str_utf16_len(js, str_value);
  
  switch (encoding) {
    case ENC_HEX: return units >> 1;
    case ENC_UCS2: return units * 2;
    case ENC_BASE64:
    case ENC_BASE64URL:
      if (units > 0 && str[len - 1] == '=') {
        units--;
        if (units > 0 && str[len - 2] == '=') units--;
      }
      return (units * 3) >> 2;
    default: return units;
  }
}

ant_value_t buffer_bytes_to_string_enc(ant_t *js, const uint8_t *data, size_t len, BufferEncoding encoding) {
  if (encoding == ENC_BASE64 || encoding == ENC_BASE64URL) {
    size_t out_len;
    
    char *encoded = encoding == ENC_BASE64URL
      ? ant_base64url_encode(data, len, &out_len)
      : ant_base64_encode(data, len, &out_len);
    
    if (!encoded) return js_mkerr(js, "Failed to encode base64");
    
    ant_value_t result = js_mkstr(js, encoded, out_len);
    free(encoded);
    return result;
  } else if (encoding == ENC_HEX) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return js_mkerr(js, "Failed to allocate hex string");
    
    hex_encode(data, len, hex);
    
    ant_value_t result = js_mkstr(js, hex, len * 2);
    free(hex);
    return result;
  } else if (encoding == ENC_LATIN1) {
    size_t out_len;
    char *out = latin1_to_utf8(data, len, &out_len);
    if (!out) return js_mkerr(js, "Failed to decode buffer as Latin-1");

    ant_value_t result = js_mkstr(js, out, out_len);
    free(out);
    return result;
  } else if (encoding == ENC_ASCII) {
    char *out = malloc(len == 0 ? 1 : len);
    if (!out) return js_mkerr(js, "Failed to allocate string");
    for (size_t i = 0; i < len; i++) out[i] = (char)(data[i] & 0x7f);
    ant_value_t result = js_mkstr(js, out, len);
    free(out);
    return result;
  } else if (encoding == ENC_UCS2) {
    size_t char_count = len / 2;
    char *str = malloc(char_count * 3 + 1);
    if (!str) return js_mkerr(js, "Failed to allocate string");
    
    size_t out_len = 0;
    for (size_t i = 0; i < char_count; i++) {
      uint32_t unit = (uint32_t)data[i * 2] | ((uint32_t)data[i * 2 + 1] << 8);
      out_len += (size_t)utf8_encode(unit, str + out_len);
    }
    
    str[out_len] = '\0';
    ant_value_t result = js_mkstr(js, str, out_len);
    free(str);
    
    return result;
  } else return js_mkstr_utf8(js, data, len);
}

bool buffer_bytes_to_string(
  ant_t *js, const uint8_t *data, size_t len,
  const char *encoding, size_t encoding_len, ant_value_t *out
) {
  BufferEncoding parsed = parse_encoding(encoding, encoding_len);
  if (parsed == ENC_UNKNOWN) return false;
  *out = buffer_bytes_to_string_enc(js, data, len, parsed);
  return true;
}
