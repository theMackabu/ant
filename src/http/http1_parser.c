#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <llhttp.h>

#include "http/http1_parser.h"
#include "http/http1_writer.h"

typedef ant_http1_parser_ctx_t parser_ctx_t;

static void parser_ctx_free(parser_ctx_t *ctx) {
  if (!ctx) return;
  free(ctx->req.method);
  free(ctx->req.target);
  free(ctx->req.host);
  free(ctx->req.content_type);
  free(ctx->req.body);
  ant_http_headers_free(ctx->req.headers);
  ant_http1_buffer_free(&ctx->method);
  ant_http1_buffer_free(&ctx->target);
  ant_http1_buffer_free(&ctx->header_field);
  ant_http1_buffer_free(&ctx->header_value);
  ant_http1_buffer_free(&ctx->body);
  memset(ctx, 0, sizeof(*ctx));
}

static bool parser_copy_header(parser_ctx_t *ctx) {
  ant_http_header_t *hdr = calloc(1, sizeof(*hdr));
  if (!hdr) return false;

  hdr->name = ant_http1_buffer_take(&ctx->header_field, NULL);
  hdr->value = ant_http1_buffer_take(&ctx->header_value, NULL);
  if (!hdr->name || !hdr->value) {
    free(hdr->name);
    free(hdr->value);
    free(hdr);
    return false;
  }

  *ctx->header_tail = hdr;
  ctx->header_tail = &hdr->next;

  if (strcasecmp(hdr->name, "host") == 0) {
    free(ctx->req.host);
    ctx->req.host = strdup(hdr->value);
    if (!ctx->req.host) return false;
  } else if (strcasecmp(hdr->name, "content-type") == 0) {
    free(ctx->req.content_type);
    ctx->req.content_type = strdup(hdr->value);
    if (!ctx->req.content_type) return false;
  } else if (strcasecmp(hdr->name, "content-length") == 0) {
    ctx->req.content_length = (size_t)strtoull(hdr->value, NULL, 10);
  }

  return true;
}

static int parser_on_method(llhttp_t *parser, const char *at, size_t length) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  return ant_http1_buffer_append(&ctx->method, at, length) ? 0 : -1;
}

static int parser_on_url(llhttp_t *parser, const char *at, size_t length) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  return ant_http1_buffer_append(&ctx->target, at, length) ? 0 : -1;
}

static int parser_on_header_field(llhttp_t *parser, const char *at, size_t length) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  if (ctx->header_value.len > 0) {
    if (!parser_copy_header(ctx)) return -1;
  }
  return ant_http1_buffer_append(&ctx->header_field, at, length) ? 0 : -1;
}

static int parser_on_header_value(llhttp_t *parser, const char *at, size_t length) {
  parser_ctx_t *ctx = (parser_ctx_t *)parser->data;
  return ant_http1_buffer_append(&ctx->header_value, at, length) ? 0 : -1;
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
  .on_method           = parser_on_method,
  .on_url              = parser_on_url,
  .on_header_field     = parser_on_header_field,
  .on_header_value     = parser_on_header_value,
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

  ant_http1_buffer_init(&ctx.method);
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

  ctx.req.method = ant_http1_buffer_take(&ctx.method, NULL);
  ctx.req.target = ant_http1_buffer_take(&ctx.target, NULL);
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
  free(req->method);
  free(req->target);
  free(req->host);
  free(req->content_type);
  free(req->body);
  ant_http_headers_free(req->headers);
  memset(req, 0, sizeof(*req));
}

void ant_http1_conn_parser_init(ant_http1_conn_parser_t *cp) {
  if (!cp) return;

  memset(cp, 0, sizeof(*cp));
  cp->ctx.header_tail = &cp->ctx.req.headers;
  llhttp_init(&cp->parser, HTTP_REQUEST, &g_request_settings);
  cp->parser.data = &cp->ctx;
}

void ant_http1_conn_parser_reset(ant_http1_conn_parser_t *cp) {
  if (!cp) return;

  ant_http1_buffer_free(&cp->ctx.method);
  ant_http1_buffer_free(&cp->ctx.target);
  ant_http1_buffer_free(&cp->ctx.header_field);
  ant_http1_buffer_free(&cp->ctx.header_value);
  ant_http1_buffer_free(&cp->ctx.body);
  
  memset(&cp->ctx, 0, sizeof(cp->ctx));
  cp->ctx.header_tail = &cp->ctx.req.headers;
  cp->fed_len = 0;
  llhttp_reset(&cp->parser);
  cp->parser.data = &cp->ctx;
}

void ant_http1_conn_parser_free(ant_http1_conn_parser_t *cp) {
  if (!cp) return;

  ant_http1_buffer_free(&cp->ctx.method);
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
    
  cp->ctx.req.method = ant_http1_buffer_take(&cp->ctx.method, NULL);
  cp->ctx.req.target = ant_http1_buffer_take(&cp->ctx.target, NULL);
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
