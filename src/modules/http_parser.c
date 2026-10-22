#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "ptr.h"

#include "http/http1_parser.h"
#include "modules/buffer.h"
#include "modules/http_parser.h"

static constexpr uint32_t HTTP_REQUEST_PARSER_NATIVE_TAG = Ant_NativeTag("HRQP");

static ant_value_t http_parser_make_buffer(ant_t *js, const uint8_t *data, size_t len) {
  ArrayBufferData *ab = create_array_buffer_data(js, len);
  if (!ab) return js_mkerr_typed(js, JS_ERR_TYPE, "Out of memory");
  if (len > 0 && data) memcpy(ab->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, len, js->builtins.buffer_proto);
}

static ant_value_t http_parser_make_error(
  ant_t *js, const char *message, const char *code, size_t bytes_parsed
) {
  ant_value_t props = Ant_Object_CreateBare(js);
  if (code) js_set(js, props, "code", js_mkstr(js, code, strlen(code)));
  js_set(js, props, "bytesParsed", js_mknum((double)bytes_parsed));
  return js_mkerr_props(js, JS_ERR_SYNTAX, props, "%s", message ? message : "Invalid HTTP request");
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

static ant_value_t http_parser_head_object(ant_t *js, const ant_http1_parsed_request_t *parsed) {
  char version_buf[8] = {0};
  
  ant_value_t out = Ant_Object_CreateBare(js);
  snprintf(version_buf, sizeof(version_buf), "%u.%u", parsed->http_major, parsed->http_minor);

  js_set(js, out, "method", js_mkstr(js, parsed->method, strlen(parsed->method)));
  js_set(js, out, "target", js_mkstr(js, parsed->target, strlen(parsed->target)));
  js_set(js, out, "host", parsed->host ? js_mkstr(js, parsed->host, strlen(parsed->host)) : js_mknull());
  js_set(js, out, "contentType", parsed->content_type ? js_mkstr(js, parsed->content_type, strlen(parsed->content_type)) : js_mknull());
  js_set(js, out, "rawHeaders", http_parser_raw_headers_array(js, parsed));
  js_set(js, out, "contentLength", js_mknum((double)parsed->content_length));
  js_set(js, out, "absoluteTarget", js_bool(parsed->absolute_target));
  js_set(js, out, "keepAlive", js_bool(parsed->keep_alive));
  js_set(js, out, "httpVersionMajor", js_mknum((double)parsed->http_major));
  js_set(js, out, "httpVersionMinor", js_mknum((double)parsed->http_minor));
  js_set(js, out, "httpVersion", js_mkstr(js, version_buf, strlen(version_buf)));

  return out;
}

static bool http_parser_input_bytes(ant_t *js, ant_value_t input, const uint8_t **bytes, size_t *len, ant_value_t *err) {
  if (buffer_source_get_bytes(js, input, bytes, len)) return true;

  ant_value_t str_value = js_tostring_val(js, input);
  if (is_err(str_value)) {
    *err = str_value;
    return false;
  }

  const char *str = js_getstr(js, str_value, len);
  if (!str) {
    *err = js_mkerr_typed(js, JS_ERR_TYPE, "HTTP parser input must be string or buffer-like");
    return false;
  }

  *bytes = (const uint8_t *)str;
  return true;
}

static ant_value_t js_http_parser_parse_request(ant_params_t) {
  ant_http1_parsed_request_t parsed = {0};
  
  const char *error_reason = NULL;
  const char *error_code = NULL;
  const uint8_t *bytes = NULL;
  
  size_t len = 0;
  ant_value_t err = js_mkundef();

  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "parseRequest requires 1 argument");
  if (!http_parser_input_bytes(js, args[0], &bytes, &len, &err)) return err;

  size_t max_header_size = 0;
  if (nargs > 1 && vtype(args[1]) == kTypeNumber && js_getnum(args[1]) >= 1)
    max_header_size = (size_t)js_getnum(args[1]);

  ant_http1_parse_result_t result = ant_http1_parse_request(
    (const char *)bytes, len, max_header_size, 
    &parsed, &error_reason, &error_code
  );
  
  if (result == ANT_HTTP1_PARSE_INCOMPLETE) return js_mknull();
  if (result == ANT_HTTP1_PARSE_ERROR) return http_parser_make_error(js, error_reason, error_code, parsed.consumed_len);

  ant_value_t body = http_parser_make_buffer(js, parsed.body, parsed.body_len);
  if (is_err(body)) {
    ant_http1_free_parsed_request(&parsed);
    return body;
  }

  ant_value_t out = http_parser_head_object(js, &parsed);
  js_set(js, out, "body", body);
  js_set(js, out, "bodyLength", js_mknum((double)parsed.body_len));
  js_set(js, out, "consumed", js_mknum((double)parsed.consumed_len));

  ant_http1_free_parsed_request(&parsed);
  return out;
}

static void http_request_parser_free(ant_value_t value) {
  ant_http1_conn_parser_t *cp = js_get_native(value, HTTP_REQUEST_PARSER_NATIVE_TAG);
  if (!cp) return;
  ant_http1_conn_parser_free(cp);
  free(cp);
  js_clear_native(value, HTTP_REQUEST_PARSER_NATIVE_TAG);
}

static void http_request_parser_finalize(ant_t *js, ant_object_t *obj) {
  http_request_parser_free(js_obj_from_ptr(obj));
}

static ant_value_t js_http_parser_create_request_parser(ant_params_t) {
  ant_http1_conn_parser_t *cp = malloc(sizeof(*cp));
  if (!cp) return js_mkerr(js, "out of memory");

  ant_http1_conn_parser_init(cp);
  if (nargs > 0 && vtype(args[0]) == kTypeNumber && js_getnum(args[0]) >= 1)
    cp->ctx.header_limit = (size_t)js_getnum(args[0]);

  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_native(obj, cp, HTTP_REQUEST_PARSER_NATIVE_TAG);
  js_set_finalizer(obj, http_request_parser_finalize);
  
  return obj;
}

static ant_value_t js_http_parser_feed_request_parser(ant_params_t) {
  ant_http1_conn_parser_t *cp = nargs > 0 ? js_get_native(args[0], HTTP_REQUEST_PARSER_NATIVE_TAG) : NULL;
  if (!cp) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid HTTP request parser");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t err = js_mkundef();
  if (!http_parser_input_bytes(js, nargs > 1 ? args[1] : js_mkundef(), &bytes, &len, &err)) return err;

  ant_http1_parsed_request_t head;
  size_t consumed = 0;
  ant_http1_parse_result_t result = ant_http1_conn_parser_feed(cp, (const char *)bytes, len, &head, &consumed);

  if (result == ANT_HTTP1_PARSE_ERROR) {
    ant_http1_free_parsed_request(&head);
    bool overflow = cp->ctx.header_overflow;
    return http_parser_make_error(
      js, overflow ? "Header overflow" : llhttp_get_error_reason(&cp->parser),
      overflow ? "HPE_HEADER_OVERFLOW" : llhttp_errno_name(llhttp_get_errno(&cp->parser)), consumed
    );
  }

  ant_value_t out = Ant_Object_CreateBare(js);
  js_set(js, out, "consumed", js_mknum((double)consumed));
  js_set(js, out, "complete", js_bool(result == ANT_HTTP1_PARSE_OK));

  if (head.method) {
    js_set(js, out, "head", http_parser_head_object(js, &head));
    ant_http1_free_parsed_request(&head);
  }

  if (cp->ctx.body_span) {
    size_t start = (size_t)((const uint8_t *)cp->ctx.body_span - bytes);
    js_set(js, out, "bodyStart", js_mknum((double)start));
    js_set(js, out, "bodyEnd", js_mknum((double)(start + cp->ctx.body_span_len)));
  } else if (cp->ctx.body.len > 0) {
    ant_value_t body = http_parser_make_buffer(js, (const uint8_t *)cp->ctx.body.data, cp->ctx.body.len);
    if (is_err(body)) return body;
    js_set(js, out, "body", body);
  }

  return out;
}

static ant_value_t js_http_parser_free_request_parser(ant_params_t) {
  if (nargs > 0) http_request_parser_free(args[0]);
  return js_mkundef();
}

ant_value_t internal_http_parser_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  js_set(js, lib, "parseRequest", Ant_Function_Create(js_http_parser_parse_request));
  js_set(js, lib, "createRequestParser", Ant_Function_Create(js_http_parser_create_request_parser));
  js_set(js, lib, "feedRequestParser", Ant_Function_Create(js_http_parser_feed_request_parser));
  js_set(js, lib, "freeRequestParser", Ant_Function_Create(js_http_parser_free_request_parser));
  return lib;
}
