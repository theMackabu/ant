#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <tlsuv/http.h>

#include "modules/http.h"
#include "streams/brotli.h"

struct ant_http_request_s {
  tlsuv_http_t client;
  ant_http_response_t response;
  tlsuv_http_req_t *req;
  
  ant_http_response_cb on_response;
  ant_http_body_cb on_body;
  ant_http_complete_cb on_complete;
  
  void *user_data;
  char *error_message;
  brotli_stream_state_t *brotli_decoder;
  
  int error_code;
  bool completed;
  bool decode_brotli;
};

void ant_http_headers_free(ant_http_header_t *headers) {
while (headers) {
  ant_http_header_t *next = headers->next;
  free(headers->name);
  free(headers->value);
  free(headers);
  headers = next;
}}

static void ant_http_request_free(ant_http_request_t *req) {
  if (!req) return;
  free((char *)req->response.status_text);
  ant_http_headers_free((ant_http_header_t *)req->response.headers);
  free(req->error_message);
  if (req->brotli_decoder) brotli_stream_state_destroy(req->brotli_decoder);
  free(req);
}

static char *ant_http_copy_slice(const char *src, size_t len) {
  char *out = malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, src, len);
  out[len] = '\0';
  return out;
}

static char *ant_http_build_host_url(const struct tlsuv_url_s *url) {
  size_t size = 0;
  char port_buf[16] = {0};
  int port_len = 0;

  if (url->port != 0) port_len = snprintf(port_buf, sizeof(port_buf), ":%u", url->port);
  size = url->scheme_len + 3 + url->hostname_len + (size_t)port_len + 1;

  char *host_url = malloc(size);
  if (!host_url) return NULL;
  snprintf(
    host_url, size, "%.*s://%.*s%s",
    (int)url->scheme_len, url->scheme,
    (int)url->hostname_len, url->hostname,
    port_buf
  );
  
  return host_url;
}

static char *ant_http_build_path(const struct tlsuv_url_s *url) {
  const char *path = (url->path && url->path_len > 0) ? url->path : "/";
  size_t path_len = (url->path && url->path_len > 0) ? url->path_len : 1;
  size_t query_extra = url->query && url->query_len > 0 ? (size_t)url->query_len + 1 : 0;
  char *request_path = malloc(path_len + query_extra + 1);
  if (!request_path) return NULL;

  memcpy(request_path, path, path_len);
  if (query_extra > 0) {
    request_path[path_len] = '?';
    memcpy(request_path + path_len + 1, url->query, url->query_len);
    request_path[path_len + query_extra] = '\0';
  } else request_path[path_len] = '\0';

  return request_path;
}

static ant_http_header_t *ant_http_header_dup(const char *name, const char *value) {
  ant_http_header_t *hdr = calloc(1, sizeof(*hdr));
  if (!hdr) return NULL;

  hdr->name = strdup(name ? name : "");
  hdr->value = strdup(value ? value : "");
  
  if (!hdr->name || !hdr->value) {
    free(hdr->name);
    free(hdr->value);
    free(hdr);
    return NULL;
  }

  return hdr;
}

static ant_http_header_t *ant_http_copy_headers(tlsuv_http_resp_t *resp) {
  ant_http_header_t *head = NULL;
  ant_http_header_t **tail = &head;
  tlsuv_http_hdr *hdr = NULL;

  LIST_FOREACH(hdr, &resp->headers, _next) {
    ant_http_header_t *copy = ant_http_header_dup(hdr->name, hdr->value);
    if (!copy) {
      ant_http_headers_free(head);
      return NULL;
    }
    
    *tail = copy;
    tail = &copy->next;
  }

  return head;
}


static const char *ant_http_find_header(const ant_http_header_t *headers, const char *name) {
  for (const ant_http_header_t *entry = headers; entry; entry = entry->next) {
    if (entry->name && name && strcasecmp(entry->name, name) == 0) return entry->value;
  }
  return NULL;
}

static void ant_http_on_close(tlsuv_http_t *client) {
  ant_http_request_t *req = (ant_http_request_t *)client->data;
  if (!req) return;

  if (req->on_complete) req->on_complete(
    req, req->error_code,
    req->error_message,
    req->user_data
  );
  
  ant_http_request_free(req);
}

static void ant_http_complete(ant_http_request_t *req, int error_code, const char *error_message) {
  if (!req || req->completed) return;
  req->completed = 1;
  req->error_code = error_code;

  free(req->error_message);
  req->error_message = error_message ? strdup(error_message) : NULL;

  tlsuv_http_close(&req->client, ant_http_on_close);
}

static int ant_http_brotli_body_cb(void *ctx, const uint8_t *chunk, size_t len) {
  ant_http_request_t *req = (ant_http_request_t *)ctx;
  if (req->on_body && len > 0) req->on_body(req, chunk, len, req->user_data);
  return 0;
}

static void ant_http_tlsuv_body_cb(tlsuv_http_req_t *http_req, char *body, ssize_t len) {
  ant_http_request_t *req = (ant_http_request_t *)http_req->data;
  if (!req) return;

  if (len == UV_EOF) {
    if (req->decode_brotli && req->brotli_decoder &&
      brotli_stream_finish(req->brotli_decoder, ant_http_brotli_body_cb, req) != 0) {
      ant_http_complete(req, UV_EINVAL, "brotli decompression failed");
      return;
    }
    
    ant_http_complete(req, 0, NULL);
    return;
  }

  if (len < 0) {
    ant_http_complete(req, (int)len, uv_strerror((int)len));
    return;
  }

  if (req->decode_brotli && req->brotli_decoder) {
    if (brotli_stream_process(
      req->brotli_decoder, (const uint8_t *)body, (size_t)len,
      ant_http_brotli_body_cb, req) != 0
    ) ant_http_complete(req, UV_EINVAL, "brotli decompression failed");
    return;
  }

  if (req->on_body && len > 0) req->on_body(req, (const uint8_t *)body, (size_t)len, req->user_data);
}

static void ant_http_resp_cb(tlsuv_http_resp_t *resp, void *data) {
  ant_http_request_t *req = (ant_http_request_t *)resp->req->data;
  
  const char *content_encoding = NULL;
  if (!req) return;

  if (resp->code < 0) {
    ant_http_complete(req, resp->code, uv_strerror(resp->code));
    return;
  }

  req->response.status = resp->code;
  req->response.status_text = resp->status ? strdup(resp->status) : strdup("");
  req->response.headers = ant_http_copy_headers(resp);
  
  if (!req->response.status_text || (resp->headers.lh_first && !req->response.headers)) {
    ant_http_complete(req, UV_ENOMEM, "out of memory");
    return;
  }

  content_encoding = ant_http_find_header(req->response.headers, "content-encoding");
  if (content_encoding && strcasecmp(content_encoding, "br") == 0) {
    req->brotli_decoder = brotli_stream_state_new(true);
    if (!req->brotli_decoder) {
      ant_http_complete(req, UV_ENOMEM, "out of memory");
      return;
    }
    req->decode_brotli = 1;
  }

  resp->body_cb = ant_http_tlsuv_body_cb;
  if (req->on_response) req->on_response(req, &req->response, req->user_data);
}

const ant_http_response_t *ant_http_request_response(ant_http_request_t *req) {
  return req ? &req->response : NULL;
}

int ant_http_request_cancel(ant_http_request_t *req) {
  if (!req || !req->req || req->completed) return 0;
  return tlsuv_http_req_cancel(&req->client, req->req);
}

int ant_http_request_start(
  uv_loop_t *loop,
  const ant_http_request_options_t *options,
  ant_http_response_cb on_response,
  ant_http_body_cb on_body,
  ant_http_complete_cb on_complete,
  void *user_data,
  ant_http_request_t **out_req
) {
  struct tlsuv_url_s parsed = {0};
  ant_http_request_t *req = NULL;
  char *host_url = NULL;
  char *request_path = NULL;
  int rc = 0;

  if (out_req) *out_req = NULL;
  if (!loop || !options || !options->method || !options->url) return UV_EINVAL;
  if (tlsuv_parse_url(&parsed, options->url) != 0) return UV_EINVAL;
  if (!parsed.scheme || !parsed.hostname) return UV_EINVAL;

  req = calloc(1, sizeof(ant_http_request_t));
  if (!req) return UV_ENOMEM;

  req->on_response = on_response;
  req->on_body = on_body;
  req->on_complete = on_complete;
  req->user_data = user_data;

  host_url = ant_http_build_host_url(&parsed);
  request_path = ant_http_build_path(&parsed);
  if (!host_url || !request_path) {
    free(host_url);
    free(request_path);
    ant_http_request_free(req);
    return UV_ENOMEM;
  }

  rc = tlsuv_http_init(loop, &req->client, host_url);
  free(host_url);
  if (rc != 0) {
    free(request_path);
    ant_http_request_free(req);
    return rc;
  }

  req->client.data = req;
  tlsuv_http_header(&req->client, "Accept-Encoding", NULL);
  
  req->req = tlsuv_http_req(
    &req->client,
    options->method,
    request_path, ant_http_resp_cb, req
  );
  free(request_path);

  if (!req->req) {
    tlsuv_http_close(&req->client, NULL);
    ant_http_request_free(req);
    return UV_ENOMEM;
  }

  req->req->data = req;
  for (const ant_http_header_t *hdr = options->headers; hdr; hdr = hdr->next) {
    tlsuv_http_req_header(req->req, hdr->name, hdr->value);
  }

  if (options->body && options->body_len > 0) {
  rc = tlsuv_http_req_data(req->req, (const char *)options->body, options->body_len, NULL);
  if (rc != 0) {
    ant_http_complete(req, rc, uv_strerror(rc));
    if (out_req) *out_req = req;
    return 0;
  }}

  if (out_req) *out_req = req;
  return 0;
}
