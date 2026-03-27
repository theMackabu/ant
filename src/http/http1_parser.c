#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <llhttp.h>

#include "http/http1_parser.h"
#include "http/http1_writer.h"

typedef struct {
  ant_http1_parsed_request_t req;
  ant_http1_buffer_t method;
  ant_http1_buffer_t target;
  ant_http1_buffer_t header_field;
  ant_http1_buffer_t header_value;
  ant_http1_buffer_t body;
  bool message_complete;
} parser_ctx_t;

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

  hdr->next = ctx->req.headers;
  ctx->req.headers = hdr;

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

ant_http1_parse_result_t ant_http1_parse_request(
  const char *data,
  size_t len,
  ant_http1_parsed_request_t *out,
  const char **error_reason
) {
  llhttp_t parser;
  llhttp_settings_t settings;
  llhttp_errno_t err = HPE_OK;
  parser_ctx_t ctx = {0};

  if (error_reason) *error_reason = NULL;
  memset(out, 0, sizeof(*out));

  ant_http1_buffer_init(&ctx.method);
  ant_http1_buffer_init(&ctx.target);
  ant_http1_buffer_init(&ctx.header_field);
  ant_http1_buffer_init(&ctx.header_value);
  ant_http1_buffer_init(&ctx.body);

  llhttp_settings_init(&settings);
  settings.on_method = parser_on_method;
  settings.on_url = parser_on_url;
  settings.on_header_field = parser_on_header_field;
  settings.on_header_value = parser_on_header_value;
  settings.on_headers_complete = parser_on_headers_complete;
  settings.on_body = parser_on_body;
  settings.on_message_complete = parser_on_message_complete;

  llhttp_init(&parser, HTTP_REQUEST, &settings);
  parser.data = &ctx;
  err = llhttp_execute(&parser, data, len);

  if (err != HPE_OK && err != HPE_PAUSED) {
    if (error_reason) *error_reason = llhttp_get_error_reason(&parser);
    parser_ctx_free(&ctx);
    return ANT_HTTP1_PARSE_ERROR;
  }

  if (!ctx.message_complete) {
    parser_ctx_free(&ctx);
    return ANT_HTTP1_PARSE_INCOMPLETE;
  }

  ctx.req.method = ant_http1_buffer_take(&ctx.method, NULL);
  ctx.req.target = ant_http1_buffer_take(&ctx.target, NULL);
  ctx.req.body = (uint8_t *)ant_http1_buffer_take(&ctx.body, &ctx.req.body_len);
  if (!ctx.req.method || !ctx.req.target) {
    parser_ctx_free(&ctx);
    return ANT_HTTP1_PARSE_ERROR;
  }

  ctx.req.absolute_target =
    strncmp(ctx.req.target, "http://", 7) == 0 ||
    strncmp(ctx.req.target, "https://", 8) == 0;
  ctx.req.keep_alive = llhttp_should_keep_alive(&parser) == 1;

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
