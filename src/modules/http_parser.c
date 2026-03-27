#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"

#include "http/http1_parser.h"
#include "modules/buffer.h"
#include "modules/http_parser.h"

static ant_value_t http_parser_make_buffer(ant_t *js, const uint8_t *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(len);
  if (!ab) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  if (len > 0 && data) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, "Buffer");
}

static ant_value_t http_parser_make_error(
  ant_t *js,
  const char *message,
  const char *code,
  size_t bytes_parsed
) {
  ant_value_t err = js_mkerr_typed(js, JS_ERR_SYNTAX, "%s", message ? message : "Invalid HTTP request");

  if (!is_err(err)) return err;
  if (code) js_set(js, err, "code", js_mkstr(js, code, strlen(code)));
  js_set(js, err, "bytesParsed", js_mknum((double)bytes_parsed));
  return err;
}

static ant_value_t http_parser_raw_headers_array(ant_t *js, const ant_http1_parsed_request_t *parsed) {
  ant_value_t raw_headers = js_mkarr(js);
  const ant_http_header_t *hdr = NULL;

  for (hdr = parsed->headers; hdr; hdr = hdr->next) {
    js_arr_push(js, raw_headers, js_mkstr(js, hdr->name, strlen(hdr->name)));
    js_arr_push(js, raw_headers, js_mkstr(js, hdr->value, strlen(hdr->value)));
  }

  return raw_headers;
}

static ant_value_t js_http_parser_parse_request(ant_t *js, ant_value_t *args, int nargs) {
  ant_http1_parsed_request_t parsed = {0};
  ant_http1_parse_result_t result = ANT_HTTP1_PARSE_INCOMPLETE;
  ant_value_t input = 0;
  ant_value_t out = 0;
  ant_value_t body = 0;
  ant_value_t version = 0;
  
  const uint8_t *bytes = NULL;
  size_t len = 0;
  
  const char *error_reason = NULL;
  const char *error_code = NULL;
  char version_buf[8] = {0};

  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "parseRequest requires 1 argument");
  input = args[0];

  if (!buffer_source_get_bytes(js, input, &bytes, &len)) {
    ant_value_t str_value = js_tostring_val(js, input);
    const char *str = NULL;
    
    if (is_err(str_value)) return str_value;
    str = js_getstr(js, str_value, &len);
    
    if (!str) return js_mkerr_typed(js, JS_ERR_TYPE, "parseRequest input must be string or buffer-like");
    bytes = (const uint8_t *)str;
  }

  result = ant_http1_parse_request((const char *)bytes, len, &parsed, &error_reason, &error_code);
  if (result == ANT_HTTP1_PARSE_INCOMPLETE) return js_mknull();
  if (result == ANT_HTTP1_PARSE_ERROR) return http_parser_make_error(js, error_reason, error_code, parsed.consumed_len);

  out = js_mkobj(js);
  body = http_parser_make_buffer(js, parsed.body, parsed.body_len);
  
  if (is_err(body)) {
    ant_http1_free_parsed_request(&parsed);
    return body;
  }

  snprintf(version_buf, sizeof(version_buf), "%u.%u", parsed.http_major, parsed.http_minor);
  version = js_mkstr(js, version_buf, strlen(version_buf));

  js_set(js, out, "method", js_mkstr(js, parsed.method, strlen(parsed.method)));
  js_set(js, out, "target", js_mkstr(js, parsed.target, strlen(parsed.target)));
  js_set(js, out, "host", parsed.host ? js_mkstr(js, parsed.host, strlen(parsed.host)) : js_mknull());
  js_set(js, out, "contentType", parsed.content_type ? js_mkstr(js, parsed.content_type, strlen(parsed.content_type)) : js_mknull());
  js_set(js, out, "rawHeaders", http_parser_raw_headers_array(js, &parsed));
  
  js_set(js, out, "body", body);
  js_set(js, out, "bodyLength", js_mknum((double)parsed.body_len));
  js_set(js, out, "contentLength", js_mknum((double)parsed.content_length));
  js_set(js, out, "absoluteTarget", js_bool(parsed.absolute_target));
  js_set(js, out, "keepAlive", js_bool(parsed.keep_alive));
  js_set(js, out, "httpVersionMajor", js_mknum((double)parsed.http_major));
  js_set(js, out, "httpVersionMinor", js_mknum((double)parsed.http_minor));
  js_set(js, out, "httpVersion", version);
  js_set(js, out, "consumed", js_mknum((double)parsed.consumed_len));

  ant_http1_free_parsed_request(&parsed);
  return out;
}

ant_value_t internal_http_parser_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  js_set(js, lib, "parseRequest", js_mkfun(js_http_parser_parse_request));
  return lib;
}
