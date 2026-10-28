#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "ascii.h"
#include "errors.h"
#include "utf8.h"
#include "internal.h"
#include "modules/uri.h"

static const unsigned char uri_unreserved[256] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,1,0,0,0,0,0,1,1,1,1,0,0,1,1,0,
  1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,
  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const unsigned char uri_reserved[256] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,1,1,0,1,0,0,0,0,1,1,0,0,1,
  0,0,0,0,0,0,0,0,0,0,1,1,0,1,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

#define is_uri_unreserved(c) (uri_unreserved[(unsigned char)(c)])
#define is_uri_reserved(c) (uri_reserved[(unsigned char)(c)])

static int is_valid_continuation(unsigned char c) {
  return (c & 0xC0) == 0x80;
}

static int is_lone_surrogate(const unsigned char *str, int seq_len) {
  if (seq_len != 3) return 0;
  if (str[0] != 0xED) return 0;
  return (str[1] >= 0xA0 && str[1] <= 0xBF);
}

static int decode_escape_sequence(const char *str, size_t len, size_t *pos, unsigned char *out_byte) {
  if (*pos + 2 >= len) return -1;
  if (str[*pos] != '%') return -1;
  
  int high = ant_ascii_hex_value((unsigned char)str[*pos + 1]);
  int low = ant_ascii_hex_value((unsigned char)str[*pos + 2]);
  if (high < 0 || low < 0) return -1;
  
  *out_byte = (unsigned char)((high << 4) | low);
  *pos += 3;
  return 0;
}

static ant_value_t escape_arg_string(ant_native_params_t) {
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(value) == kTypeString) return value;
  if (vtype(value) == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a string");
  return coerce_to_str(js, value);
}

static ant_value_t uri_encode(ant_t *js, ant_value_t value, bool keep_reserved) {
  size_t len = 0;
  
  const char *str = js_getstr(js, value, &len);
  if (!str) return js_mkstr(js, "", 0);

  size_t out_len = 0;
  for (size_t i = 0; i < len;) {
    unsigned char c = (unsigned char)str[i];
    if (is_uri_unreserved(c) || (keep_reserved && is_uri_reserved(c))) {
      out_len++;
      i++;
      continue;
    }

    int seq_len = utf8_sequence_length(c);
    if (seq_len < 0 || i + (size_t)seq_len > len) goto malformed;
    for (int j = 1; j < seq_len; j++)
      if (!is_valid_continuation((unsigned char)str[i + j])) goto malformed;
    if (is_lone_surrogate((const unsigned char *)&str[i], seq_len)) goto malformed;

    out_len += 3 * (size_t)seq_len;
    i += (size_t)seq_len;
  }

  if (out_len == len) return value;
  static const char hex[] = "0123456789ABCDEF";
  
  char stack[512];
  char *out = out_len <= sizeof(stack) ? stack : malloc(out_len);
  if (!out) return Ant_Error_CreateOutOfMemory(js);

  size_t pos = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (is_uri_unreserved(c) || (keep_reserved && is_uri_reserved(c))) {
      out[pos++] = (char)c;
      continue;
    }
    out[pos++] = '%';
    out[pos++] = hex[c >> 4];
    out[pos++] = hex[c & 15];
  }

  ant_value_t result = js_mkstr_ascii(js, out, out_len);
  if (out != stack) free(out);
  return result;

malformed:
  return js_mkerr_typed(js, JS_ERR_URI, "URI malformed");
}

// encodeURIComponent()
ant_value_t js_encodeURIComponent(ant_params_t) {
  ant_value_t value = escape_arg_string(js, args, nargs);
  return is_err(value) ? value : uri_encode(js, value, false);
}

// encodeURI()
ant_value_t js_encodeURI(ant_params_t) {
  ant_value_t value = escape_arg_string(js, args, nargs);
  return is_err(value) ? value : uri_encode(js, value, true);
}

// decodeURIComponent()
ant_value_t js_decodeURIComponent(ant_params_t) {
  ant_value_t result;
  char *out = NULL;
  
  ant_value_t value = escape_arg_string(js, args, nargs);
  if (is_err(value)) return value;
  
  size_t len = 0;
  const char *str = js_getstr(js, value, &len);
  if (!str) return js_mkstr(js, "", 0);
  
  out = malloc(len + 1);
  if (!out) return Ant_Error_CreateOutOfMemory(js);
  
  size_t out_len = 0;
  size_t i = 0;
  
  while (i < len) {
    if (str[i] != '%') {
      out[out_len++] = str[i++];
      continue;
    }
    
    unsigned char first_byte;
    if (decode_escape_sequence(str, len, &i, &first_byte) < 0) goto malformed;
    
    int seq_len = utf8_sequence_length(first_byte);
    if (seq_len < 0) goto malformed;
    
    out[out_len++] = (char)first_byte;
    
    for (int j = 1; j < seq_len; j++) {
      unsigned char cont_byte;
      if (decode_escape_sequence(str, len, &i, &cont_byte) < 0) goto malformed;
      if (!is_valid_continuation(cont_byte)) goto malformed;
      out[out_len++] = (char)cont_byte;
    }
    
    if (utf8_valid_prefix(out + out_len - seq_len, (size_t)seq_len, NULL) != (size_t)seq_len) goto malformed;
  }
  
  out[out_len] = '\0';
  result = js_mkstr(js, out, out_len);
  free(out);
  return result;

malformed:
  free(out);
  return js_mkerr_typed(js, JS_ERR_URI, "URI malformed");
}

// decodeURI()
ant_value_t js_decodeURI(ant_params_t) {
  ant_value_t result;
  char *out = NULL;
  
  ant_value_t value = escape_arg_string(js, args, nargs);
  if (is_err(value)) return value;
  
  size_t len = 0;
  const char *str = js_getstr(js, value, &len);
  if (!str) return js_mkstr(js, "", 0);
  
  out = malloc(len + 1);
  if (!out) return Ant_Error_CreateOutOfMemory(js);
  
  size_t out_len = 0;
  size_t i = 0;
  
  while (i < len) {
    if (str[i] != '%') {
      out[out_len++] = str[i++];
      continue;
    }
    
    if (i + 2 >= len) goto malformed;
    
    int high = ant_ascii_hex_value((unsigned char)str[i + 1]);
    int low = ant_ascii_hex_value((unsigned char)str[i + 2]);
    if (high < 0 || low < 0) goto malformed;
    
    unsigned char first_byte = (unsigned char)((high << 4) | low);
    
    if (first_byte < 128 && is_uri_reserved((char)first_byte)) {
      out[out_len++] = str[i++];
      out[out_len++] = str[i++];
      out[out_len++] = str[i++];
      continue;
    }
    
    i += 3;
    
    int seq_len = utf8_sequence_length(first_byte);
    if (seq_len < 0) goto malformed;
    
    out[out_len++] = (char)first_byte;
    
    for (int j = 1; j < seq_len; j++) {
      unsigned char cont_byte;
      if (decode_escape_sequence(str, len, &i, &cont_byte) < 0) goto malformed;
      if (!is_valid_continuation(cont_byte)) goto malformed;
      out[out_len++] = (char)cont_byte;
    }
    
    if (utf8_valid_prefix(out + out_len - seq_len, (size_t)seq_len, NULL) != (size_t)seq_len) goto malformed;
  }
  
  out[out_len] = '\0';
  result = js_mkstr(js, out, out_len);
  free(out);
  return result;

malformed:
  free(out);
  return js_mkerr_typed(js, JS_ERR_URI, "URI malformed");
}

static int is_escape_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') ||
         c == '@' || c == '*' || c == '_' || c == '+' ||
         c == '-' || c == '.' || c == '/';
}

static ant_value_t js_escape(ant_params_t) {
  static const char hex[] = "0123456789ABCDEF";
  ant_value_t value = escape_arg_string(js, args, nargs);
  if (is_err(value)) return value;

  size_t len = 0;
  const char *str = js_getstr(js, value, &len);
  if (!str || len == 0) return js_mkstr(js, "", 0);

  char *out = malloc(len * 6 + 1);
  if (!out) return Ant_Error_CreateOutOfMemory(js);

  size_t out_len = 0;
  utf16_iterator_t it = utf16_iterator(str, len);
  uint16_t unit;

  while (utf16_next_code_unit(&it, &unit)) {
    if (unit < 0x80 && is_escape_unreserved((unsigned char)unit)) {
      out[out_len++] = (char)unit;
    } else if (unit < 0x100) {
      out[out_len++] = '%';
      out[out_len++] = hex[unit >> 4];
      out[out_len++] = hex[unit & 0xf];
    } else {
      out[out_len++] = '%';
      out[out_len++] = 'u';
      out[out_len++] = hex[(unit >> 12) & 0xf];
      out[out_len++] = hex[(unit >> 8) & 0xf];
      out[out_len++] = hex[(unit >> 4) & 0xf];
      out[out_len++] = hex[unit & 0xf];
    }
  }

  ant_value_t result = js_mkstr_ascii(js, out, out_len);
  free(out);
  return result;
}

static inline int32_t unescape_hex(const char *str, int digits) {
  int32_t value = 0;
  
  for (int k = 0; k < digits; k++) {
    int d = ant_ascii_hex_value((unsigned char)str[k]);
    if (d < 0) return -1;
    value = (value << 4) | d;
  }
  
  return value;
}

static inline size_t unescape_sequence(const char *str, size_t avail, uint32_t *unit) {
  int32_t value;
  
  if (avail >= 6 && str[1] == 'u' && (value = unescape_hex(str + 2, 4)) >= 0) {
    *unit = (uint32_t)value;
    return 6;
  }
  
  if (avail >= 3 && (value = unescape_hex(str + 1, 2)) >= 0) {
    *unit = (uint32_t)value;
    return 3;
  }
  
  return 0;
}

static ant_value_t js_unescape(ant_params_t) {
  ant_value_t value = escape_arg_string(js, args, nargs);
  if (is_err(value)) return value;

  size_t len = 0;
  const char *str = js_getstr(js, value, &len);
  if (!str || len == 0) return js_mkstr(js, "", 0);

  char *out = malloc(len + 1);
  if (!out) return Ant_Error_CreateOutOfMemory(js);

  size_t out_len = 0;
  size_t i = 0;

  while (i < len) {
    uint32_t unit;
    size_t step = str[i] == '%' ? unescape_sequence(str + i, len - i, &unit) : 0;
    
    if (!step) {
      out[out_len++] = str[i++];
      continue;
    }
    
    out_len += (size_t)utf8_encode(unit, out + out_len);
    i += step;
  }

  ant_value_t result = js_mkstr(js, out, out_len);
  free(out);
  
  return result;
}

void init_uri_module(ant_t *js) {
  js_set_global_builtin(js, "encodeURI", Ant_Function_CreateWithArity(js_encodeURI, 1));
  js_set_global_builtin(js, "encodeURIComponent", Ant_Function_CreateWithArity(js_encodeURIComponent, 1));
  
  js_set_global_builtin(js, "decodeURI", Ant_Function_CreateWithArity(js_decodeURI, 1));
  js_set_global_builtin(js, "decodeURIComponent", Ant_Function_CreateWithArity(js_decodeURIComponent, 1));
  
  js_set_global_builtin(js, "escape", Ant_Function_CreateWithArity(js_escape, 1));
  js_set_global_builtin(js, "unescape", Ant_Function_CreateWithArity(js_unescape, 1));
}
