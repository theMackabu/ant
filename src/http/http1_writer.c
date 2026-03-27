#include <compat.h> // IWYU pragma: keep

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http/http1_writer.h"
#include "modules/headers.h"

void ant_http1_buffer_init(ant_http1_buffer_t *buf) {
  memset(buf, 0, sizeof(*buf));
}

bool ant_http1_buffer_reserve(ant_http1_buffer_t *buf, size_t extra) {
  size_t need = buf->len + extra;
  size_t next_cap = 0;
  char *next = NULL;

  if (buf->failed) return false;
  if (need <= buf->cap) return true;

  next_cap = buf->cap ? buf->cap * 2 : 256;
  while (next_cap < need) next_cap *= 2;
  next = realloc(buf->data, next_cap);
  if (!next) {
    buf->failed = true;
    return false;
  }

  buf->data = next;
  buf->cap = next_cap;
  return true;
}

bool ant_http1_buffer_append(ant_http1_buffer_t *buf, const void *data, size_t len) {
  if (!ant_http1_buffer_reserve(buf, len)) return false;
  if (len > 0) memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  return true;
}

bool ant_http1_buffer_append_cstr(ant_http1_buffer_t *buf, const char *str) {
  return ant_http1_buffer_append(buf, str, strlen(str));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

bool ant_http1_buffer_appendfv(ant_http1_buffer_t *buf, const char *fmt, va_list ap) {
  char stack[256];
  va_list ap_copy;
  int written;

  if (!buf || buf->failed) return false;

  va_copy(ap_copy, ap);
  written = vsnprintf(stack, sizeof(stack), fmt, ap_copy);
  va_end(ap_copy);

  if (written < 0) {
    buf->failed = true;
    return false;
  }

  if ((size_t)written < sizeof(stack))
    return ant_http1_buffer_append(buf, stack, (size_t)written);

  if (!ant_http1_buffer_reserve(buf, (size_t)written + 1)) return false;
  vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap);
  buf->len += (size_t)written;
  
  return true;
}

bool ant_http1_buffer_appendf(ant_http1_buffer_t *buf, const char *fmt, ...) {
  va_list ap;
  bool ok;

  va_start(ap, fmt);
  ok = ant_http1_buffer_appendfv(buf, fmt, ap);
  va_end(ap);
  return ok;
}

#pragma GCC diagnostic pop

char *ant_http1_buffer_take(ant_http1_buffer_t *buf, size_t *len_out) {
  char *data = buf->data;
  if (len_out) *len_out = buf->len;
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
  buf->failed = false;
  return data;
}

void ant_http1_buffer_free(ant_http1_buffer_t *buf) {
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
  buf->failed = false;
}

const char *ant_http1_default_status_text(int status) {
switch (status) {
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 204: return "No Content";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 303: return "See Other";
  case 304: return "Not Modified";
  case 307: return "Temporary Redirect";
  case 308: return "Permanent Redirect";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 408: return "Request Timeout";
  case 409: return "Conflict";
  case 410: return "Gone";
  case 413: return "Payload Too Large";
  case 415: return "Unsupported Media Type";
  case 421: return "Misdirected Request";
  case 429: return "Too Many Requests";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  default: return "OK";
}}

typedef struct {
  ant_http1_buffer_t *buf;
} response_header_ctx_t;

static void ant_http1_append_response_header(const char *name, const char *value, void *ctx) {
  response_header_ctx_t *state = (response_header_ctx_t *)ctx;
  if (!state || state->buf->failed) return;

  if (strcasecmp(name, "connection") == 0) return;
  if (strcasecmp(name, "content-length") == 0) return;
  if (strcasecmp(name, "transfer-encoding") == 0) return;
  ant_http1_buffer_appendf(state->buf, "%s: %s\r\n", name, value);
}

bool ant_http1_write_basic_response(
  ant_http1_buffer_t *buf,
  int status,
  const char *status_text,
  const char *content_type,
  const uint8_t *body,
  size_t body_len,
  bool keep_alive
) {
  ant_http1_buffer_appendf(buf, "HTTP/1.1 %d %s\r\n", status, status_text 
    ? status_text 
    : ant_http1_default_status_text(status)
  );
  
  ant_http1_buffer_appendf(buf, "Content-Type: %s\r\n", content_type 
    ? content_type 
    : "text/plain;charset=UTF-8"
  );
  
  ant_http1_buffer_appendf(buf, "Content-Length: %zu\r\n", body_len);
  ant_http1_buffer_append_cstr(buf, keep_alive 
    ? "Connection: keep-alive\r\n\r\n" 
    : "Connection: close\r\n\r\n"
  );
  
  if (body_len > 0) ant_http1_buffer_append(buf, body, body_len);
  return !buf->failed;
}

bool ant_http1_write_response_head(
  ant_http1_buffer_t *buf,
  int status,
  const char *status_text,
  ant_value_t headers,
  bool body_is_stream,
  size_t body_size,
  bool keep_alive
) {
  response_header_ctx_t ctx = { .buf = buf };

  ant_http1_buffer_appendf(buf, "HTTP/1.1 %d %s\r\n", status, status_text 
    ? status_text 
    : ant_http1_default_status_text(status)
  );
  
  headers_for_each(headers, ant_http1_append_response_header, &ctx);
  if (body_is_stream) ant_http1_buffer_append_cstr(buf, "Transfer-Encoding: chunked\r\n");
  else ant_http1_buffer_appendf(buf, "Content-Length: %zu\r\n", body_size);
  ant_http1_buffer_append_cstr(buf, keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
  
  return !buf->failed;
}

bool ant_http1_write_chunk(ant_http1_buffer_t *buf, const uint8_t *chunk, size_t len) {
  ant_http1_buffer_appendf(buf, "%zx\r\n", len);
  if (len > 0) ant_http1_buffer_append(buf, chunk, len);
  ant_http1_buffer_append_cstr(buf, "\r\n");
  return !buf->failed;
}

bool ant_http1_write_final_chunk(ant_http1_buffer_t *buf) {
  return ant_http1_buffer_append_cstr(buf, "0\r\n\r\n");
}
