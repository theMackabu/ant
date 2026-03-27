#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ant.h"
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
  ab = create_array_buffer_data(len);
  if (!ab) {
    free(data);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }

  if (len > 0 && data) memcpy(ab->data, data, len);
  free(data);
  out = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Buffer");
  return out;
}

static bool http_writer_append_raw_headers(
  ant_t *js,
  ant_http1_buffer_t *buf,
  ant_value_t raw_headers,
  ant_value_t *error_out
) {
  ant_offset_t len = 0;

  if (error_out) *error_out = js_mkundef();
  if (vtype(raw_headers) == T_UNDEF || vtype(raw_headers) == T_NULL) return true;
  if (vtype(raw_headers) != T_ARR) {
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

    if (strcasecmp(name, "connection") == 0) continue;
    if (strcasecmp(name, "content-length") == 0) continue;
    if (strcasecmp(name, "transfer-encoding") == 0) continue;
    if (!ant_http1_buffer_appendf(buf, "%s: %s\r\n", name, value)) break;
  }

  if (buf->failed && error_out && vtype(*error_out) == T_UNDEF)
    *error_out = js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  return !buf->failed;
}

static bool http_writer_parse_bytes(
  ant_t *js,
  ant_value_t value,
  const uint8_t **bytes_out,
  size_t *len_out,
  ant_value_t *error_out
) {
  ant_value_t str_value = 0;
  const char *str = NULL;
  size_t len = 0;

  if (error_out) *error_out = js_mkundef();
  if (bytes_out) *bytes_out = NULL;
  if (len_out) *len_out = 0;
  
  if (vtype(value) == T_UNDEF || vtype(value) == T_NULL) return true;
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

static ant_value_t js_http_writer_default_status_text(ant_t *js, ant_value_t *args, int nargs) {
  int status = nargs > 0 ? (int)js_getnum(args[0]) : 200;
  const char *text = ant_http1_default_status_text(status);
  return js_mkstr(js, text, strlen(text));
}

static ant_value_t js_http_writer_write_chunk(ant_t *js, ant_value_t *args, int nargs) {
  ant_http1_buffer_t buf;
  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t error = js_mkundef();

  ant_http1_buffer_init(&buf);
  if (!http_writer_parse_bytes(js, nargs > 0 ? args[0] : js_mkundef(), &bytes, &len, &error)) return error;
  if (!ant_http1_write_chunk(&buf, bytes, len)) {
    ant_http1_buffer_free(&buf);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }

  return http_writer_make_buffer_value(js, &buf);
}

static ant_value_t js_http_writer_write_final_chunk(ant_t *js, ant_value_t *args, int nargs) {
  ant_http1_buffer_t buf;

  ant_http1_buffer_init(&buf);
  if (!ant_http1_write_final_chunk(&buf)) {
    ant_http1_buffer_free(&buf);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }

  return http_writer_make_buffer_value(js, &buf);
}

static ant_value_t js_http_writer_write_basic_response(ant_t *js, ant_value_t *args, int nargs) {
  ant_http1_buffer_t buf;
  ant_value_t error = js_mkundef();
  const uint8_t *body = NULL;
  
  size_t body_len = 0;
  int status = nargs > 0 ? (int)js_getnum(args[0]) : 200;
  
  const char *status_text = NULL;
  const char *content_type = NULL;
  bool keep_alive = false;

  if (nargs > 1 && vtype(args[1]) != T_UNDEF && vtype(args[1]) != T_NULL) {
    ant_value_t status_text_value = js_tostring_val(js, args[1]);
    if (is_err(status_text_value)) return status_text_value;
    status_text = js_getstr(js, status_text_value, NULL);
  }

  if (nargs > 2 && vtype(args[2]) != T_UNDEF && vtype(args[2]) != T_NULL) {
    ant_value_t content_type_value = js_tostring_val(js, args[2]);
    if (is_err(content_type_value)) return content_type_value;
    content_type = js_getstr(js, content_type_value, NULL);
  }

  if (!http_writer_parse_bytes(js, nargs > 3 ? args[3] : js_mkundef(), &body, &body_len, &error)) return error;
  keep_alive = nargs > 4 && js_truthy(js, args[4]);

  ant_http1_buffer_init(&buf);
  if (!ant_http1_write_basic_response(&buf, status, status_text, content_type, body, body_len, keep_alive)) {
    ant_http1_buffer_free(&buf);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }

  return http_writer_make_buffer_value(js, &buf);
}

static ant_value_t js_http_writer_write_head(ant_t *js, ant_value_t *args, int nargs) {
  ant_http1_buffer_t buf;
  ant_value_t error = js_mkundef();
  
  int status = nargs > 0 ? (int)js_getnum(args[0]) : 200;
  const char *status_text = NULL;
  ant_value_t raw_headers = js_mkundef();
  
  bool body_is_stream = false;
  size_t body_size = 0;
  bool keep_alive = false;
  int index = 1;

  if (nargs > index && vtype(args[index]) != T_UNDEF && vtype(args[index]) != T_NULL && vtype(args[index]) != T_ARR) {
    ant_value_t status_text_value = js_tostring_val(js, args[index]);
    if (is_err(status_text_value)) return status_text_value;
    status_text = js_getstr(js, status_text_value, NULL);
    index++;
  }

  if (nargs > index) raw_headers = args[index++];
  if (nargs > index) body_is_stream = js_truthy(js, args[index++]);
  if (nargs > index) body_size = (size_t)js_getnum(args[index++]);
  if (nargs > index) keep_alive = js_truthy(js, args[index++]);

  ant_http1_buffer_init(&buf);
  if (!ant_http1_buffer_appendf(&buf, "HTTP/1.1 %d %s\r\n", status, status_text ? status_text : ant_http1_default_status_text(status))) {
    ant_http1_buffer_free(&buf);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }

  if (!http_writer_append_raw_headers(js, &buf, raw_headers, &error)) {
    ant_http1_buffer_free(&buf);
    return error;
  }

  if (body_is_stream) ant_http1_buffer_append_cstr(&buf, "Transfer-Encoding: chunked\r\n");
  else ant_http1_buffer_appendf(&buf, "Content-Length: %zu\r\n", body_size);
  ant_http1_buffer_append_cstr(&buf, keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");

  if (buf.failed) {
    ant_http1_buffer_free(&buf);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  }

  return http_writer_make_buffer_value(js, &buf);
}

ant_value_t internal_http_writer_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  js_set(js, lib, "defaultStatusText", js_mkfun(js_http_writer_default_status_text));
  js_set(js, lib, "writeHead", js_mkfun(js_http_writer_write_head));
  js_set(js, lib, "writeBasicResponse", js_mkfun(js_http_writer_write_basic_response));
  js_set(js, lib, "writeChunk", js_mkfun(js_http_writer_write_chunk));
  js_set(js, lib, "writeFinalChunk", js_mkfun(js_http_writer_write_final_chunk));
  
  return lib;
}
