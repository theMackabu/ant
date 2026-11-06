#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ant.h"
#include "ascii.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"

#include "http/http1_writer.h"
#include "modules/buffer.h"
#include "modules/http_writer.h"

static ant_value_t http_writer_make_buffer_value(ant_t *js, ant_http1_buffer_t *buf) {
  ant_value_t out = 0;
  char *data = NULL;
  size_t len = 0;
  ArrayBufferData *ab = NULL;

  data = ant_http1_buffer_take(buf, &len);
  ab = create_array_buffer_data(js, len);
  if (!ab) {
    free(data);
    return Ant_Error_CreateOutOfMemory(js);
  }

  if (len > 0 && data) memcpy(ab->data, data, len);
  free(data);
  out = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, js->builtins.buffer_proto);
  return out;
}

static bool http_writer_is_token(const char *s, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++) if (!ant_ascii_is_tchar((unsigned char)s[i])) return false;
  return true;
}

static bool http_writer_is_field_value(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) if (!ant_ascii_is_field_char((unsigned char)s[i])) return false;
  return true;
}

static bool http_writer_is_latin1_field_value(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    
    if (c < 0x80) {
      if (!ant_ascii_is_field_char(c)) return false;
      continue;
    }
    
    if ((c != 0xc2 && c != 0xc3) || i + 1 >= len || ((unsigned char)s[i + 1] & 0xc0) != 0x80) return false;
    i++;
  }
  
  return true;
}

static bool http_writer_append_raw_headers(
  ant_t *js, ant_http1_buffer_t *buf, ant_value_t raw_headers, ant_value_t *error_out
) {
  ant_offset_t len = 0;

  if (error_out) *error_out = Ant_Value_Undefined;
  if (vtype(raw_headers) == kTypeUndefined || vtype(raw_headers) == kTypeNull) return true;
  
  if (vtype(raw_headers) != kTypeArray) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "rawHeaders must be an array");
    return false;
  }

  len = js_arr_len(js, raw_headers);
  if ((len & 1) != 0) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "rawHeaders must contain name/value pairs");
    return false;
  }

  for (ant_offset_t i = 0; i < len; i += 2) {
    ant_value_t name_value = js_tostring_val(js, js_arr_get(js, raw_headers, i));
    ant_value_t header_value = js_tostring_val(js, js_arr_get(js, raw_headers, i + 1));
    
    const char *name = NULL;
    const char *value = NULL;
    
    size_t name_len = 0;
    size_t value_len = 0;

    if (is_err(name_value)) {
      if (error_out) *error_out = name_value;
      return false;
    }
    
    if (is_err(header_value)) {
      if (error_out) *error_out = header_value;
      return false;
    }

    name = js_getstr(js, name_value, &name_len);
    value = js_getstr(js, header_value, &value_len);
    
    if (!name || !value) {
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid raw header entry");
      return false;
    }
    
    if (!http_writer_is_token(name, name_len)) {
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Header name must be a valid HTTP token");
      return false;
    }
    
    if (!http_writer_is_field_value(value, value_len)) {
      if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid character in header content");
      return false;
    }

    if (strcasecmp(name, "connection") == 0) continue;
    if (strcasecmp(name, "content-length") == 0) continue;
    if (strcasecmp(name, "transfer-encoding") == 0) continue;
    if (!ant_http1_buffer_append_header(buf, name, value)) break;
  }

  if (buf->failed && error_out && vtype(*error_out) == kTypeUndefined)
    *error_out = Ant_Error_CreateOutOfMemory(js);
  return !buf->failed;
}

static bool http_writer_parse_bytes(
  ant_t *js, ant_value_t value, const uint8_t **bytes_out, size_t *len_out, ant_value_t *error_out
) {
  ant_value_t str_value = 0;
  const char *str = NULL;
  size_t len = 0;

  if (error_out) *error_out = Ant_Value_Undefined;
  if (bytes_out) *bytes_out = NULL;
  if (len_out) *len_out = 0;
  
  if (vtype(value) == kTypeUndefined || vtype(value) == kTypeNull) return true;
  if (buffer_source_get_bytes(js, value, bytes_out, len_out)) return true;

  str_value = js_tostring_val(js, value);
  if (is_err(str_value)) {
    if (error_out) *error_out = str_value;
    return false;
  }

  str = js_getstr(js, str_value, &len);
  if (!str) {
    if (error_out) *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Invalid HTTP body chunk");
    return false;
  }

  if (bytes_out) *bytes_out = (const uint8_t *)str;
  if (len_out) *len_out = len;
  
  return true;
}

static ant_value_t js_http_writer_default_status_text(ant_params_t) {
  int status = nargs > 0 ? (int)Ant_Value_AsNumberUnchecked(args[0]) : 200;
  const char *text = ant_http1_default_status_text(status);
  return js_mkstr(js, text, strlen(text));
}

static ant_value_t js_http_writer_write_chunk(ant_params_t) {
  ant_http1_buffer_t buf;
  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t error = Ant_Value_Undefined;

  ant_http1_buffer_init(&buf);
  if (!http_writer_parse_bytes(js, nargs > 0 ? args[0] : Ant_Value_Undefined, &bytes, &len, &error)) return error;
  if (!ant_http1_write_chunk(&buf, bytes, len)) {
    ant_http1_buffer_free(&buf);
    return Ant_Error_CreateOutOfMemory(js);
  }

  return http_writer_make_buffer_value(js, &buf);
}

static ant_value_t js_http_writer_write_final_chunk(ant_params_t) {
  ant_http1_buffer_t buf;

  ant_http1_buffer_init(&buf);
  if (!ant_http1_write_final_chunk(&buf)) {
    ant_http1_buffer_free(&buf);
    return Ant_Error_CreateOutOfMemory(js);
  }

  return http_writer_make_buffer_value(js, &buf);
}

static ant_value_t js_http_writer_write_basic_response(ant_params_t) {
  ant_http1_buffer_t buf;
  ant_value_t error = Ant_Value_Undefined;
  const uint8_t *body = NULL;
  
  size_t body_len = 0;
  int status = nargs > 0 ? (int)Ant_Value_AsNumberUnchecked(args[0]) : 200;
  
  const char *status_text = NULL;
  const char *content_type = NULL;
  bool keep_alive = false;

  if (nargs > 1 && vtype(args[1]) != kTypeUndefined && vtype(args[1]) != kTypeNull) {
    ant_value_t status_text_value = js_tostring_val(js, args[1]);
    if (is_err(status_text_value)) return status_text_value;
    status_text = js_getstr(js, status_text_value, NULL);
  }

  if (nargs > 2 && vtype(args[2]) != kTypeUndefined && vtype(args[2]) != kTypeNull) {
    ant_value_t content_type_value = js_tostring_val(js, args[2]);
    if (is_err(content_type_value)) return content_type_value;
    content_type = js_getstr(js, content_type_value, NULL);
  }

  if (!http_writer_parse_bytes(js, nargs > 3 ? args[3] : Ant_Value_Undefined, &body, &body_len, &error)) return error;
  keep_alive = nargs > 4 && js_truthy(js, args[4]);

  ant_http1_buffer_init(&buf);
  if (!ant_http1_write_basic_response(&buf, status, status_text, content_type, body, body_len, keep_alive)) {
    ant_http1_buffer_free(&buf);
    return Ant_Error_CreateOutOfMemory(js);
  }

  return http_writer_make_buffer_value(js, &buf);
}

static ant_value_t js_http_writer_write_head(ant_params_t) {
  ant_http1_buffer_t buf;
  ant_value_t error = Ant_Value_Undefined;
  
  int status = nargs > 0 ? (int)Ant_Value_AsNumberUnchecked(args[0]) : 200;
  const char *status_text = NULL;
  ant_value_t raw_headers = Ant_Value_Undefined;
  
  bool body_is_stream = false;
  size_t body_size = 0;
  bool keep_alive = false;
  int index = 1;

  if (nargs > index && vtype(args[index]) != kTypeUndefined && vtype(args[index]) != kTypeNull && vtype(args[index]) != kTypeArray) {
    ant_value_t status_text_value = js_tostring_val(js, args[index]);
    if (is_err(status_text_value)) return status_text_value;
    
    size_t status_text_len = 0;
    status_text = js_getstr(js, status_text_value, &status_text_len);
    
    if (status_text && !http_writer_is_field_value(status_text, status_text_len))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid character in statusMessage");
    index++;
  }

  if (nargs > index) raw_headers = args[index++];
  if (nargs > index) body_is_stream = js_truthy(js, args[index++]);
  if (nargs > index) body_size = (size_t)Ant_Value_AsNumberUnchecked(args[index++]);
  if (nargs > index) keep_alive = js_truthy(js, args[index++]);

  ant_http1_buffer_init(&buf);
  if (!ant_http1_buffer_append_status_line(&buf, status, status_text)) {
    ant_http1_buffer_free(&buf);
    return Ant_Error_CreateOutOfMemory(js);
  }

  if (!http_writer_append_raw_headers(js, &buf, raw_headers, &error)) {
    ant_http1_buffer_free(&buf);
    return error;
  }

  if (body_is_stream) ant_http1_buffer_append_cstr(&buf, "Transfer-Encoding: chunked\r\n");
  else ant_http1_buffer_append_content_length(&buf, body_size);
  ant_http1_buffer_append_cstr(&buf, keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");

  if (buf.failed) {
    ant_http1_buffer_free(&buf);
    return Ant_Error_CreateOutOfMemory(js);
  }

  return http_writer_make_buffer_value(js, &buf);
}

static ant_value_t js_http_writer_is_token(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return Ant_Value_False;
  size_t len = 0;
  const char *s = js_getstr(js, args[0], &len);
  return Ant_Value_FromBool(s && http_writer_is_token(s, len));
}

static ant_value_t js_http_writer_is_field_value(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return Ant_Value_False;
  size_t len = 0;
  const char *s = js_getstr(js, args[0], &len);
  return Ant_Value_FromBool(s && http_writer_is_latin1_field_value(s, len));
}

static ant_value_t http_writer_value_error(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeNumber) return Ant_Value_FromNumber(0);
  ant_value_t str = vtype(value) == kTypeString ? value : js_tostring_val(js, value);
  if (is_err(str)) return str;
  size_t len = 0;
  const char *s = js_getstr(js, str, &len);
  return Ant_Value_FromNumber(s && http_writer_is_latin1_field_value(s, len) ? 0 : 3);
}

static ant_value_t http_writer_header_value_error(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeUndefined) return Ant_Value_FromNumber(2);
  if (vtype(value) != kTypeArray) return http_writer_value_error(js, value);

  ant_offset_t len = js_arr_len(js, value);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t result = http_writer_value_error(js, js_arr_get(js, value, i));
    if (is_err(result) || Ant_Value_AsNumberUnchecked(result) != 0) return result;
  }
  
  return Ant_Value_FromNumber(0);
}

static ant_value_t js_http_writer_header_key(ant_params_t) {
  ant_value_t name = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(name) != kTypeString) return Ant_Value_FromNumber(1);

  size_t len = 0;
  const char *str = js_getstr(js, name, &len);
  if (!str || len == 0) return Ant_Value_FromNumber(1);

  bool upper = false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (!ant_ascii_is_tchar(c)) return Ant_Value_FromNumber(1);
    upper |= ant_ascii_is_upper(c);
  }

  ant_value_t error = http_writer_header_value_error(js, nargs > 1 ? args[1] : Ant_Value_Undefined);
  if (is_err(error) || Ant_Value_AsNumberUnchecked(error) != 0) return error;
  if (!upper) return name;

  str = js_getstr(js, name, &len);
  char stack[128];
  char *lower = len <= sizeof(stack) ? stack : malloc(len);
  if (!lower) return Ant_Error_CreateOutOfMemory(js);
  
  for (size_t i = 0; i < len; i++) lower[i] = (char)ant_ascii_to_lower((unsigned char)str[i]);
  ant_value_t key = js_mkstr(js, lower, len);
  if (lower != stack) free(lower);
  
  return key;
}

ant_value_t internal_http_writer_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);

  js_set(js, lib, "defaultStatusText", Ant_Function_Create(js_http_writer_default_status_text));
  js_set(js, lib, "writeHead", Ant_Function_Create(js_http_writer_write_head));
  js_set(js, lib, "writeBasicResponse", Ant_Function_Create(js_http_writer_write_basic_response));
  js_set(js, lib, "writeChunk", Ant_Function_Create(js_http_writer_write_chunk));
  js_set(js, lib, "writeFinalChunk", Ant_Function_Create(js_http_writer_write_final_chunk));
  js_set(js, lib, "isToken", Ant_Function_Create(js_http_writer_is_token));
  js_set(js, lib, "isFieldValue", Ant_Function_Create(js_http_writer_is_field_value));
  js_set(js, lib, "headerKey", Ant_Function_Create(js_http_writer_header_key));
  
  return lib;
}
