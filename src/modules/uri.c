#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ant.h"
#include "runtime.h"
#include "modules/uri.h"

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int is_uri_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') ||
         c == '-' || c == '_' || c == '.' || c == '!' ||
         c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}

static int is_uri_reserved(unsigned char c) {
  return c == ';' || c == '/' || c == '?' || c == ':' ||
         c == '@' || c == '&' || c == '=' || c == '+' ||
         c == '$' || c == ',' || c == '#';
}

static int utf8_sequence_length(unsigned char first_byte) {
  if ((first_byte & 0x80) == 0) return 1;
  if ((first_byte & 0xE0) == 0xC0) return 2;
  if ((first_byte & 0xF0) == 0xE0) return 3;
  if ((first_byte & 0xF8) == 0xF0) return 4;
  return -1;
}

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
  
  int high = hex_digit(str[*pos + 1]);
  int low = hex_digit(str[*pos + 2]);
  if (high < 0 || low < 0) return -1;
  
  *out_byte = (unsigned char)((high << 4) | low);
  *pos += 3;
  return 0;
}

// encodeURIComponent()
static jsval_t js_encodeURIComponent(struct js *js, jsval_t *args, int nargs) {
  jsval_t result;
  char *out = NULL;
  
  if (nargs < 1) return js_mkstr(js, "undefined", 9);
  
  char *str = js_getstr(js, args[0], NULL);
  if (!str) return js_mkstr(js, "", 0);
  
  size_t len = strlen(str);
  size_t out_cap = len * 12 + 1;
  out = malloc(out_cap);
  if (!out) return js_mkerr(js, "out of memory");
  
  size_t out_len = 0;
  size_t i = 0;
  
  while (i < len) {
    unsigned char c = (unsigned char)str[i];
    
    if (is_uri_unreserved(c)) {
      out[out_len++] = (char)c;
      i++;
      continue;
    }
    
    int seq_len = utf8_sequence_length(c);
    if (seq_len < 0) goto malformed;
    if (i + seq_len > len) goto malformed;
    
    for (int j = 1; j < seq_len; j++) {
      if (!is_valid_continuation((unsigned char)str[i + j])) goto malformed;
    }
    
    if (is_lone_surrogate((unsigned char *)&str[i], seq_len)) goto malformed;
    
    for (int j = 0; j < seq_len; j++) {
      out_len += sprintf(out + out_len, "%%%02X", (unsigned char)str[i + j]);
    }
    i += seq_len;
  }
  
  out[out_len] = '\0';
  result = js_mkstr(js, out, out_len);
  free(out);
  return result;

malformed:
  free(out);
  return js_mkerr_typed(js, JS_ERR_URI, "URI malformed");
}

// encodeURI()
static jsval_t js_encodeURI(struct js *js, jsval_t *args, int nargs) {
  jsval_t result;
  char *out = NULL;
  
  if (nargs < 1) return js_mkstr(js, "undefined", 9);
  
  char *str = js_getstr(js, args[0], NULL);
  if (!str) return js_mkstr(js, "", 0);
  
  size_t len = strlen(str);
  size_t out_cap = len * 12 + 1;
  out = malloc(out_cap);
  if (!out) return js_mkerr(js, "out of memory");
  
  size_t out_len = 0;
  size_t i = 0;
  
  while (i < len) {
    unsigned char c = (unsigned char)str[i];
    
    if (is_uri_unreserved(c) || is_uri_reserved(c)) {
      out[out_len++] = (char)c;
      i++;
      continue;
    }
    
    int seq_len = utf8_sequence_length(c);
    if (seq_len < 0) goto malformed;
    if (i + seq_len > len) goto malformed;
    
    for (int j = 1; j < seq_len; j++) {
      if (!is_valid_continuation((unsigned char)str[i + j])) goto malformed;
    }
    
    if (is_lone_surrogate((unsigned char *)&str[i], seq_len)) goto malformed;
    
    for (int j = 0; j < seq_len; j++) {
      out_len += sprintf(out + out_len, "%%%02X", (unsigned char)str[i + j]);
    }
    i += seq_len;
  }
  
  out[out_len] = '\0';
  result = js_mkstr(js, out, out_len);
  free(out);
  return result;

malformed:
  free(out);
  return js_mkerr_typed(js, JS_ERR_URI, "URI malformed");
}

// decodeURIComponent()
static jsval_t js_decodeURIComponent(struct js *js, jsval_t *args, int nargs) {
  jsval_t result;
  char *out = NULL;
  
  if (nargs < 1) return js_mkstr(js, "undefined", 9);
  
  char *str = js_getstr(js, args[0], NULL);
  if (!str) return js_mkstr(js, "", 0);
  
  size_t len = strlen(str);
  out = malloc(len + 1);
  if (!out) return js_mkerr(js, "out of memory");
  
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
static jsval_t js_decodeURI(struct js *js, jsval_t *args, int nargs) {
  jsval_t result;
  char *out = NULL;
  
  if (nargs < 1) return js_mkstr(js, "undefined", 9);
  
  char *str = js_getstr(js, args[0], NULL);
  if (!str) return js_mkstr(js, "", 0);
  
  size_t len = strlen(str);
  out = malloc(len + 1);
  if (!out) return js_mkerr(js, "out of memory");
  
  size_t out_len = 0;
  size_t i = 0;
  
  while (i < len) {
    if (str[i] != '%') {
      out[out_len++] = str[i++];
      continue;
    }
    
    if (i + 2 >= len) goto malformed;
    
    int high = hex_digit(str[i + 1]);
    int low = hex_digit(str[i + 2]);
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

static jsval_t js_escape(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkstr(js, "undefined", 9);
  
  char *str = js_getstr(js, args[0], NULL);
  if (!str) return js_mkstr(js, "undefined", 9);
  
  size_t len = strlen(str);
  size_t out_cap = len * 6 + 1;
  char *out = malloc(out_cap);
  if (!out) return js_mkerr(js, "out of memory");
  
  size_t out_len = 0;
  
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    
    if (is_escape_unreserved(c)) {
      out[out_len++] = (char)c;
    } else out_len += sprintf(out + out_len, "%%%02X", c);
  }
  
  out[out_len] = '\0';
  jsval_t result = js_mkstr(js, out, out_len);
  free(out);
  return result;
}

static jsval_t js_unescape(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkstr(js, "undefined", 9);
  
  char *str = js_getstr(js, args[0], NULL);
  if (!str) return js_mkstr(js, "undefined", 9);
  
  size_t len = strlen(str);
  char *out = malloc(len + 1);
  if (!out) return js_mkerr(js, "out of memory");
  
  size_t out_len = 0;
  size_t i = 0;
  
  while (i < len) {
    if (str[i] == '%' && i + 2 < len) {
      int high = hex_digit(str[i + 1]);
      int low = hex_digit(str[i + 2]);
      if (high >= 0 && low >= 0) {
        out[out_len++] = (char)((high << 4) | low);
        i += 3;
        continue;
      }
    }
    out[out_len++] = str[i++];
  }
  
  out[out_len] = '\0';
  jsval_t result = js_mkstr(js, out, out_len);
  free(out);
  return result;
}

void init_uri_module(void) {
  struct js *js = rt->js;
  jsval_t glob = js_glob(js);

  js_set(js, glob, "encodeURI", js_mkfun(js_encodeURI));
  js_set(js, glob, "encodeURIComponent", js_mkfun(js_encodeURIComponent));
  js_set(js, glob, "decodeURI", js_mkfun(js_decodeURI));
  js_set(js, glob, "decodeURIComponent", js_mkfun(js_decodeURIComponent));
  js_set(js, glob, "escape", js_mkfun(js_escape));
  js_set(js, glob, "unescape", js_mkfun(js_unescape));
}
