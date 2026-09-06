#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <llhttp.h>

#include "http/http1_parser.h"

static bool http1_host_is_canonical(const char *host) {
  const char *port_sep;
  const char *host_end;
  size_t label_len = 0;
  bool numeric_host = true;

  if (!host || !host[0]) return false;
  port_sep = strchr(host, ':');
  if (port_sep && strchr(port_sep + 1, ':')) return false;
  host_end = port_sep ? port_sep : host + strlen(host);
  if (host_end == host || (size_t)(host_end - host) > 253) return false;

  for (const char *p = host; p < host_end; p++) {
    unsigned char ch = (unsigned char)*p;
    if (ch >= 'a' && ch <= 'z') {
      numeric_host = false;
      label_len++;
    } else if (ch >= '0' && ch <= '9') {
      label_len++;
    } else if (ch == '-') {
      numeric_host = false;
      if (label_len == 0 || p + 1 == host_end || p[1] == '.') return false;
      label_len++;
    } else if (ch == '.') {
      if (label_len == 0 || label_len > 63) return false;
      label_len = 0;
    } else {
      return false;
    }
  }
  
  if (label_len == 0 || label_len > 63) return false;
  const char *last_label = host_end - label_len;
  if (!numeric_host && *last_label >= '0' && *last_label <= '9') return false;

  if (numeric_host) {
    const char *p = host;
    for (int part = 0; part < 4; part++) {
      unsigned value = 0;
      const char *start = p;
      while (p < host_end && *p != '.') {
        value = value * 10u + (unsigned)(*p - '0');
        if (value > 255u) return false;
        p++;
      }
      if (p == start || (p - start > 1 && *start == '0')) return false;
      if (part < 3) {
        if (p == host_end) return false;
        p++;
      } else if (p != host_end) {
        return false;
      }
    }
  }

  if (port_sep) {
    const char *p = port_sep + 1;
    unsigned port = 0;
    if (!*p || (p[0] == '0' && p[1])) return false;
    for (; *p; p++) {
      if (*p < '0' || *p > '9') return false;
      port = port * 10u + (unsigned)(*p - '0');
      if (port > 65535u) return false;
    }
    if (port == 0 || port == 80) return false;
  }

  return true;
}
#include "http/http1_writer.h"

typedef ant_http1_parser_ctx_t parser_ctx_t;

static void parser_ctx_free(parser_ctx_t *ctx) {
  if (!ctx) return;
  if (!ctx->req.target_borrowed) free(ctx->req.target);
  free(ctx->req.body);
  ant_http_headers_free(ctx->req.headers);
  ant_http1_buffer_free(&ctx->target);
  ant_http1_buffer_free(&ctx->header_field);
  ant_http1_buffer_free(&ctx->header_value);
  ant_http1_buffer_free(&ctx->body);
  memset(ctx, 0, sizeof(*ctx));
}

static bool parser_copy_header(parser_ctx_t *ctx) {
  size_t name_len = ctx->header_field.len;
  size_t value_len = ctx->header_value.len;
  size_t required = 0;
  ant_http_header_t *hdr = NULL;

  if (name_len > SIZE_MAX - sizeof(*hdr) - 2 ||
    value_len > SIZE_MAX - sizeof(*hdr) - name_len - 2) return false;
  required = sizeof(*hdr) + name_len + value_len + 2;
  hdr = malloc(required);
  if (!hdr) return false;

  hdr->name = hdr->storage;
  hdr->value = hdr->storage + name_len + 1;
  
  hdr->next = NULL;
  if (name_len > 0) memcpy(hdr->name, ctx->header_field.data, name_len);
  
  hdr->name[name_len] = '\0';
  if (value_len > 0) memcpy(hdr->value, ctx->header_value.data, value_len);
  
  hdr->value[value_len] = '\0';
  ctx->header_field.len = 0;
  ctx->header_value.len = 0;

  *ctx->header_tail = hdr;
  ctx->header_tail = &hdr->next;

  if (strcasecmp(hdr->name, "host") == 0) {
    ctx->req.host = hdr->value;
    ctx->req.canonical_host = http1_host_is_canonical(ctx->req.host);
  } else if (strcasecmp(hdr->name, "content-type") == 0) {
    ctx->req.content_type = hdr->value;
  } else if (strcasecmp(hdr->name, "content-length") == 0) {
    ctx->req.content_length = (size_t)strtoull(hdr->value, NULL, 10);
  }

  return true;
}

static int parser_on_url(llhttp_t *parser, const char *at, size_t length) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;

  if (length == 0) return 0;
  if (ctx->target_root_candidate) {
    if (ctx->target.len == 0 && length == 1 && at[0] == '/') {
      ctx->target.len = 1;
      return 0;
    }
    if (ctx->target.len == 1) {
      ctx->target.len = 0;
      if (!ant_http1_buffer_append(&ctx->target, "/", 1)) return -1;
    }
    ctx->target_root_candidate = false;
  }
  return ant_http1_buffer_append(&ctx->target, at, length) ? 0 : -1;
}

static int parser_on_header_field(llhttp_t *parser, const char *at, size_t length) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  return ant_http1_buffer_append(&ctx->header_field, at, length) ? 0 : -1;
}

static int parser_on_header_value(llhttp_t *parser, const char *at, size_t length) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  return ant_http1_buffer_append(&ctx->header_value, at, length) ? 0 : -1;
}

static int parser_on_header_value_complete(llhttp_t *parser) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  return parser_copy_header(ctx) ? 0 : -1;
}

static int parser_on_headers_complete(llhttp_t *parser) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  if (ctx->header_field.len > 0 || ctx->header_value.len > 0) {
    if (!parser_copy_header(ctx)) return -1;
  }
  return 0;
}

static int parser_on_body(llhttp_t *parser, const char *at, size_t length) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  return ant_http1_buffer_append(&ctx->body, at, length) ? 0 : -1;
}

static int parser_on_message_complete(llhttp_t *parser) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  ctx->message_complete = true;
  return HPE_PAUSED;
}

static llhttp_settings_t g_request_settings = {
  .on_url              = parser_on_url,
  .on_header_field     = parser_on_header_field,
  .on_header_value     = parser_on_header_value,
  .on_header_value_complete = parser_on_header_value_complete,
  .on_headers_complete = parser_on_headers_complete,
  .on_body             = parser_on_body,
  .on_message_complete = parser_on_message_complete,
};

ant_http1_parse_result_t ant_http1_parse_request(
  const char *data,
  size_t len,
  ant_http1_parsed_request_t *out,
  const char **error_reason,
  const char **error_code
) {
  llhttp_t parser;
  llhttp_errno_t err = HPE_OK;
  parser_ctx_t ctx = {0};

  if (error_reason) *error_reason = NULL;
  if (error_code) *error_code = NULL;

  memset(out, 0, sizeof(*out));
  ctx.header_tail = &ctx.req.headers;
  ctx.target_root_candidate = true;

  ant_http1_buffer_init(&ctx.target);
  ant_http1_buffer_init(&ctx.header_field);
  ant_http1_buffer_init(&ctx.header_value);
  ant_http1_buffer_init(&ctx.body);

  llhttp_init(&parser, HTTP_REQUEST, &g_request_settings);
  parser.data = &ctx;
  
  err = llhttp_execute(&parser, data, len);
  if (llhttp_get_error_pos(&parser)) out->consumed_len = (size_t)(llhttp_get_error_pos(&parser) - data);

  if (err != HPE_OK && err != HPE_PAUSED) {
    if (error_reason) *error_reason = llhttp_get_error_reason(&parser);
    if (error_code) *error_code = llhttp_errno_name(err);
    parser_ctx_free(&ctx);
    return ANT_HTTP1_PARSE_ERROR;
  }

  if (!ctx.message_complete) {
    parser_ctx_free(&ctx);
    return ANT_HTTP1_PARSE_INCOMPLETE;
  }

  if (out->consumed_len == 0) ctx.req.consumed_len = len;
  else ctx.req.consumed_len = out->consumed_len;

  ctx.req.method = llhttp_method_name((llhttp_method_t)llhttp_get_method(&parser));
  if (ctx.target_root_candidate && ctx.target.len == 1) {
    ctx.req.target = (char *)"/";
    ctx.req.target_borrowed = true;
    ctx.target.len = 0;
  } else {
    ctx.req.target = ant_http1_buffer_take_cstr(&ctx.target);
  }
  ctx.req.body = (uint8_t *)ant_http1_buffer_take(&ctx.body, &ctx.req.body_len);
  if (!ctx.req.method || !ctx.req.target) {
    parser_ctx_free(&ctx);
    return ANT_HTTP1_PARSE_ERROR;
  }

  ctx.req.absolute_target =
    strncmp(ctx.req.target, "http://", 7)  == 0 ||
    strncmp(ctx.req.target, "https://", 8) == 0;
    
  ctx.req.keep_alive = llhttp_should_keep_alive(&parser) == 1;
  ctx.req.http_major = parser.http_major;
  ctx.req.http_minor = parser.http_minor;

  *out = ctx.req;
  memset(&ctx.req, 0, sizeof(ctx.req));
  parser_ctx_free(&ctx);
  
  return ANT_HTTP1_PARSE_OK;
}

void ant_http1_free_parsed_request(ant_http1_parsed_request_t *req) {
  if (!req) return;
  if (!req->target_borrowed) free(req->target);
  free(req->body);
  ant_http_headers_free(req->headers);
  memset(req, 0, sizeof(*req));
}

void ant_http1_conn_parser_init(ant_http1_conn_parser_t *cp) {
  if (!cp) return;

  memset(cp, 0, sizeof(*cp));
  cp->ctx.header_tail = &cp->ctx.req.headers;
  cp->ctx.target_root_candidate = true;
  llhttp_init(&cp->parser, HTTP_REQUEST, &g_request_settings);
  cp->parser.data = &cp->ctx;
}

void ant_http1_conn_parser_reset(ant_http1_conn_parser_t *cp) {
  if (!cp) return;

  ant_http1_buffer_free(&cp->ctx.target);
  ant_http1_buffer_free(&cp->ctx.body);

  /* Header tokens are copied into combined nodes, so these scratch buffers
   * can serve every request on the same keep-alive connection. */
  cp->ctx.header_field.len = 0;
  cp->ctx.header_field.failed = false;
  cp->ctx.header_value.len = 0;
  cp->ctx.header_value.failed = false;

  char *header_field_data = cp->ctx.header_field.data;
  size_t header_field_cap = cp->ctx.header_field.cap;
  char *header_value_data = cp->ctx.header_value.data;
  size_t header_value_cap = cp->ctx.header_value.cap;
  memset(&cp->ctx, 0, sizeof(cp->ctx));
  cp->ctx.header_field.data = header_field_data;
  cp->ctx.header_field.cap = header_field_cap;
  cp->ctx.header_value.data = header_value_data;
  cp->ctx.header_value.cap = header_value_cap;
  cp->ctx.header_tail = &cp->ctx.req.headers;
  cp->ctx.target_root_candidate = true;
  cp->fed_len = 0;
  llhttp_reset(&cp->parser);
  cp->parser.data = &cp->ctx;
}

void ant_http1_conn_parser_free(ant_http1_conn_parser_t *cp) {
  if (!cp) return;

  ant_http1_buffer_free(&cp->ctx.target);
  ant_http1_buffer_free(&cp->ctx.header_field);
  ant_http1_buffer_free(&cp->ctx.header_value);
  ant_http1_buffer_free(&cp->ctx.body);
  ant_http1_free_parsed_request(&cp->ctx.req);
}

ant_http1_parse_result_t ant_http1_conn_parser_execute(
  ant_http1_conn_parser_t *cp,
  const char *data,
  size_t len,
  ant_http1_parsed_request_t *out,
  size_t *consumed_out
) {
  const char *new_data = NULL;
  size_t new_len = 0;
  size_t old_fed = 0;
  
  const char *errpos = NULL;
  llhttp_errno_t err = HPE_OK;

  if (!cp || !data || !out) return ANT_HTTP1_PARSE_ERROR;

  memset(out, 0, sizeof(*out));
  if (consumed_out) *consumed_out = 0;
  if (cp->fed_len > len) return ANT_HTTP1_PARSE_ERROR;

  new_data = data + cp->fed_len;
  new_len = len - cp->fed_len;
  old_fed = cp->fed_len;
  if (new_len == 0) return ANT_HTTP1_PARSE_INCOMPLETE;

  err = llhttp_execute(&cp->parser, new_data, new_len);
  errpos = llhttp_get_error_pos(&cp->parser);
  if (errpos && consumed_out)
    *consumed_out = (size_t)(errpos - new_data) + old_fed;
  cp->fed_len = len;

  if (err != HPE_OK && err != HPE_PAUSED)
    return ANT_HTTP1_PARSE_ERROR;
    
  if (!cp->ctx.message_complete)
    return ANT_HTTP1_PARSE_INCOMPLETE;

  if (consumed_out && *consumed_out == 0) *consumed_out = len;
  cp->ctx.req.consumed_len = consumed_out ? *consumed_out : len;
    
  cp->ctx.req.method = llhttp_method_name(
    (llhttp_method_t)llhttp_get_method(&cp->parser));
  if (cp->ctx.target_root_candidate && cp->ctx.target.len == 1) {
    cp->ctx.req.target = (char *)"/";
    cp->ctx.req.target_borrowed = true;
    cp->ctx.target.len = 0;
  } else {
    cp->ctx.req.target = ant_http1_buffer_take_cstr(&cp->ctx.target);
  }
  cp->ctx.req.body = (uint8_t *)ant_http1_buffer_take(&cp->ctx.body, &cp->ctx.req.body_len);
  
  if (!cp->ctx.req.method || !cp->ctx.req.target)
    return ANT_HTTP1_PARSE_ERROR;
    
  cp->ctx.req.absolute_target =
    strncmp(cp->ctx.req.target, "http://", 7) == 0 ||
    strncmp(cp->ctx.req.target, "https://", 8) == 0;
    
  cp->ctx.req.keep_alive = llhttp_should_keep_alive(&cp->parser) == 1;
  cp->ctx.req.http_major = cp->parser.http_major;
  cp->ctx.req.http_minor = cp->parser.http_minor;

  *out = cp->ctx.req;
  memset(&cp->ctx.req, 0, sizeof(cp->ctx.req));
  return ANT_HTTP1_PARSE_OK;
}
