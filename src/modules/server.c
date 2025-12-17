#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <uv.h>
#include <zlib.h>

#include "ant.h"
#include "config.h"
#include "runtime.h"
#include "modules/server.h"
#include "modules/timer.h"
#include "modules/json.h"

#define MAX_WRITE_HANDLES 1000
#define READ_BUFFER_SIZE 8192
#define GZIP_MIN_SIZE 1024

typedef struct response_ctx_s {
  int status;
  char *body;
  size_t body_len;
  char *content_type;
  int sent;
  int supports_gzip;
  uv_tcp_t *client_handle;
  struct response_ctx_s *next;
} response_ctx_t;

typedef struct http_server_s {
  struct js *js;
  jsval_t handler;
  int port;
  uv_tcp_t server;
  uv_loop_t *loop;
  response_ctx_t *pending_responses;
} http_server_t;

typedef struct {
  uv_tcp_t handle;
  http_server_t *server;
  char *buffer;
  size_t buffer_len;
  size_t buffer_capacity;
  response_ctx_t *response_ctx;
} client_t;

typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} write_req_t;

static uv_loop_t *g_loop = NULL;
static int g_loop_initialized = 0;
static uv_timer_t g_js_timer;

static void server_signal_handler(int signum) {
  (void)signum;
  if (g_loop_initialized && g_loop) uv_stop(g_loop);
  exit(0);
}

static void on_js_timer(uv_timer_t *handle) {
  struct js *js = (struct js*)handle->data;
  if (js && has_pending_timers()) {
    int64_t next_timeout = get_next_timer_timeout();
    if (next_timeout <= 0) process_timers(js);
  }
}

static int parse_accept_encoding(const char *buffer, size_t len) {
  const char *accept_encoding = strstr(buffer, "Accept-Encoding:");
  if (!accept_encoding) accept_encoding = strstr(buffer, "accept-encoding:");
  if (!accept_encoding) return 0;
  
  const char *line_end = strstr(accept_encoding, "\r\n");
  if (!line_end) return 0;
  
  size_t header_len = line_end - accept_encoding;
  if (header_len > 1024) return 0;
  
  char header[1024];
  memcpy(header, accept_encoding, header_len);
  header[header_len] = '\0';
  
  return (strstr(header, "gzip") != NULL);
}

static int should_compress_content_type(const char *content_type) {
  if (!content_type) return 0;
  
  const char *compressible_types[] = {
    "text/html",
    "text/plain",
    "text/css",
    "text/javascript",
    "application/javascript",
    "application/json",
    "application/xml",
    "text/xml",
    NULL
  };
  
  for (int i = 0; compressible_types[i] != NULL; i++) {
    if (strstr(content_type, compressible_types[i]) != NULL) return 1;
  }
  
  return 0;
}

static const char* get_status_text(int status) {
  switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 306: return "Switch Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a Teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    
    default: return "Unknown";
  }
}

typedef struct {
  char method[16];
  char uri[2048];
  char query[2048];
  char *body;
  size_t body_len;
  int accepts_gzip;
} http_request_t;

static int parse_http_request(const char *buffer, size_t len, http_request_t *req) {
  const char *method_end = strchr(buffer, ' ');
  if (!method_end || method_end - buffer >= sizeof(req->method)) return -1;
  
  memcpy(req->method, buffer, method_end - buffer);
  req->method[method_end - buffer] = '\0';
  
  const char *uri_start = method_end + 1;
  const char *uri_end = strchr(uri_start, ' ');
  if (!uri_end) return -1;
  
  const char *query_start = strchr(uri_start, '?');
  if (query_start && query_start < uri_end) {
    size_t uri_len = query_start - uri_start;
    if (uri_len >= sizeof(req->uri)) return -1;
    memcpy(req->uri, uri_start, uri_len);
    req->uri[uri_len] = '\0';
    
    size_t query_len = uri_end - query_start - 1;
    if (query_len >= sizeof(req->query)) return -1;
    memcpy(req->query, query_start + 1, query_len);
    req->query[query_len] = '\0';
  } else {
    size_t uri_len = uri_end - uri_start;
    if (uri_len >= sizeof(req->uri)) return -1;
    memcpy(req->uri, uri_start, uri_len);
    req->uri[uri_len] = '\0';
    req->query[0] = '\0';
  }
  
  req->accepts_gzip = parse_accept_encoding(buffer, len);
  
  const char *body_start = strstr(buffer, "\r\n\r\n");
  if (body_start) {
    body_start += 4;
    req->body_len = len - (body_start - buffer);
    if (req->body_len > 0) {
      req->body = malloc(req->body_len + 1);
      if (req->body) {
        memcpy(req->body, body_start, req->body_len);
        req->body[req->body_len] = '\0';
      }
    } else {
      req->body = NULL;
    }
  } else {
    req->body = NULL;
    req->body_len = 0;
  }
  
  return 0;
}

static void free_http_request(http_request_t *req) {
  if (req->body) {
    free(req->body);
    req->body = NULL;
  }
}

static void on_close(uv_handle_t *handle);
static void send_response(uv_stream_t *client, response_ctx_t *res_ctx);

static jsval_t res_status(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t this_val = js_getthis(js);
  jsval_t ctx_val = js_get(js, this_val, "__response_ctx");
  if (js_type(ctx_val) != JS_NUM) return js_mkundef();
  
  response_ctx_t *ctx = (response_ctx_t *)(unsigned long)js_getnum(ctx_val);
  if (!ctx) return js_mkundef();
  
  if (js_type(args[0]) == JS_NUM) {
    ctx->status = (int)js_getnum(args[0]);
  }
  
  return js_mkundef();
}

static jsval_t res_body(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t this_val = js_getthis(js);
  jsval_t ctx_val = js_get(js, this_val, "__response_ctx");
  if (js_type(ctx_val) != JS_NUM) return js_mkundef();
  
  response_ctx_t *ctx = (response_ctx_t *)(unsigned long)js_getnum(ctx_val);
  if (!ctx) return js_mkundef();
  
  if (js_type(args[0]) == JS_STR) {
    ctx->body = js_getstr(js, args[0], &ctx->body_len);
  }
  
  if (nargs >= 2 && js_type(args[1]) == JS_NUM) {
    ctx->status = (int)js_getnum(args[1]);
  }
  
  if (nargs >= 3 && js_type(args[2]) == JS_STR) {
    ctx->content_type = js_getstr(js, args[2], NULL);
  } else {
    ctx->content_type = "text/plain";
  }
  
  ctx->sent = 1;
  
  return js_mkundef();
}

static jsval_t res_html(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t this_val = js_getthis(js);
  jsval_t ctx_val = js_get(js, this_val, "__response_ctx");
  if (js_type(ctx_val) != JS_NUM) return js_mkundef();
  
  response_ctx_t *ctx = (response_ctx_t *)(unsigned long)js_getnum(ctx_val);
  if (!ctx) return js_mkundef();
  
  if (js_type(args[0]) == JS_STR) {
    ctx->body = js_getstr(js, args[0], &ctx->body_len);
  }
  
  if (nargs >= 2 && js_type(args[1]) == JS_NUM) {
    ctx->status = (int)js_getnum(args[1]);
  }
  
  ctx->content_type = "text/html";
  ctx->sent = 1;
  
  return js_mkundef();
}

static jsval_t res_json(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t this_val = js_getthis(js);
  jsval_t ctx_val = js_get(js, this_val, "__response_ctx");
  if (js_type(ctx_val) != JS_NUM) return js_mkundef();
  
  response_ctx_t *ctx = (response_ctx_t *)(unsigned long)js_getnum(ctx_val);
  if (!ctx) return js_mkundef();
  
  jsval_t stringify_args[1] = { args[0] };
  jsval_t result = js_json_stringify(js, stringify_args, 1);
  
  if (js_type(result) == JS_STR) {
    ctx->body = js_getstr(js, result, &ctx->body_len);
  } else if (js_type(result) == JS_ERR) {
    const char *json_str = js_str(js, args[0]);
    if (json_str) {
      ctx->body = (char *)json_str;
      ctx->body_len = strlen(json_str);
    }
  }
  
  if (nargs >= 2 && js_type(args[1]) == JS_NUM) {
    ctx->status = (int)js_getnum(args[1]);
  }
  
  ctx->content_type = "application/json";
  ctx->sent = 1;
  
  return js_mkundef();
}

static char* gzip_compress(const char *data, size_t data_len, size_t *compressed_len) {
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return NULL;
  }
  
  size_t bound = deflateBound(&stream, data_len);
  char *compressed = malloc(bound);
  if (!compressed) {
    deflateEnd(&stream);
    return NULL;
  }
  
  stream.next_in = (Bytef *)data;
  stream.avail_in = data_len;
  stream.next_out = (Bytef *)compressed;
  stream.avail_out = bound;
  
  if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
    free(compressed);
    deflateEnd(&stream);
    return NULL;
  }
  
  *compressed_len = stream.total_out;
  deflateEnd(&stream);
  
  return compressed;
}

static void on_write(uv_write_t *req, int status) {
  write_req_t *wr = (write_req_t *)req;
  if (status) fprintf(stderr, "Write error: %s\n", uv_strerror(status));
  if (wr->buf.base) free(wr->buf.base);
  free(wr);
}

static void send_response(uv_stream_t *client, response_ctx_t *res_ctx) {
  char *body_to_send = res_ctx->body;
  size_t body_len_to_send = res_ctx->body_len;
  char *compressed = NULL;
  int use_gzip = 0;
  
  if (res_ctx->supports_gzip && 
      res_ctx->body_len >= GZIP_MIN_SIZE &&
      should_compress_content_type(res_ctx->content_type)) {
    
    size_t compressed_len;
    compressed = gzip_compress(res_ctx->body, res_ctx->body_len, &compressed_len);
    
    if (compressed && compressed_len < res_ctx->body_len) {
      body_to_send = compressed;
      body_len_to_send = compressed_len;
      use_gzip = 1;
    } else if (compressed) {
      free(compressed);
      compressed = NULL;
    }
  }
  
  char header[4096];
  int header_len = snprintf(header, sizeof(header),
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "%s"
    "Connection: close\r\n"
    "\r\n",
    res_ctx->status,
    get_status_text(res_ctx->status),
    res_ctx->content_type ? res_ctx->content_type : "text/plain",
    body_len_to_send,
    use_gzip ? "Content-Encoding: gzip\r\n" : ""
  );
  
  size_t total_len = header_len + body_len_to_send;
  
  write_req_t *write_req = malloc(sizeof(write_req_t));
  if (!write_req) {
    if (compressed) free(compressed);
    return;
  }
  
  char *response = malloc(total_len);
  if (!response) {
    free(write_req);
    if (compressed) free(compressed);
    return;
  }
  
  memcpy(response, header, header_len);
  if (body_len_to_send > 0) memcpy(response + header_len, body_to_send, body_len_to_send);
  
  if (compressed) free(compressed);
  
  write_req->buf = uv_buf_init(response, total_len);
  uv_write((uv_write_t *)write_req, client, &write_req->buf, 1, on_write);
}

static void handle_http_request(client_t *client, http_request_t *http_req) {
  http_server_t *server = client->server;
  jsval_t result = js_mkundef();
  
  response_ctx_t *res_ctx = malloc(sizeof(response_ctx_t));
  if (!res_ctx) {
    fprintf(stderr, "Failed to allocate response context\n");
    uv_close((uv_handle_t *)&client->handle, on_close);
    return;
  }
  
  res_ctx->status = 200;
  res_ctx->body = "";
  res_ctx->body_len = 0;
  res_ctx->content_type = "text/plain";
  res_ctx->sent = 0;
  res_ctx->supports_gzip = http_req->accepts_gzip;
  res_ctx->client_handle = &client->handle;
  res_ctx->next = NULL;
  
  client->response_ctx = res_ctx;
  res_ctx->next = server->pending_responses;
  server->pending_responses = res_ctx;
  
  if (server->handler != 0 && js_type(server->handler) != JS_UNDEF) {
    jsval_t req = js_mkobj(server->js);
    js_set(server->js, req, "method", js_mkstr(server->js, http_req->method, strlen(http_req->method)));
    js_set(server->js, req, "uri", js_mkstr(server->js, http_req->uri, strlen(http_req->uri)));
    js_set(server->js, req, "query", js_mkstr(server->js, http_req->query, strlen(http_req->query)));
    js_set(server->js, req, "body", js_mkstr(server->js, http_req->body ? http_req->body : "", http_req->body ? http_req->body_len : 0));
    
    jsval_t res_obj = js_mkobj(server->js);
    js_set(server->js, res_obj, "__response_ctx", js_mknum((unsigned long)res_ctx));
    js_set(server->js, res_obj, "status", js_mkfun(res_status));
    js_set(server->js, res_obj, "body", js_mkfun(res_body));
    js_set(server->js, res_obj, "html", js_mkfun(res_html));
    js_set(server->js, res_obj, "json", js_mkfun(res_json));
    
    jsval_t args[2] = {req, res_obj};
    result = js_call(server->js, server->handler, args, 2);
    if (js_type(result) == JS_PROMISE) return;
    
    if (js_type(result) == JS_ERR) {
      fprintf(stderr, "Handler error: %s\n", js_str(server->js, result));
      res_ctx->status = 500;
      // pass error of throw in server into "internal server error" message
      res_ctx->body = "internal server error\nant http v" ANT_VERSION " (" ANT_GIT_HASH ")";
      res_ctx->body_len = strlen(res_ctx->body);
      res_ctx->content_type = "text/plain";
      res_ctx->sent = 1;
    } else if (!res_ctx->sent) {
      res_ctx->status = 404;
      res_ctx->body = "not found\nant http v" ANT_VERSION " (" ANT_GIT_HASH ")";
      res_ctx->body_len = strlen(res_ctx->body);
      res_ctx->content_type = "text/plain";
      res_ctx->sent = 1;
    }
    
    return;
  }
  
  res_ctx->status = 404;
  res_ctx->body = "not found\nant http v" ANT_VERSION " (" ANT_GIT_HASH ")";
  res_ctx->body_len = strlen(res_ctx->body);
  res_ctx->content_type = "text/plain";
  res_ctx->sent = 1;
}

static void on_close(uv_handle_t *handle) {
  client_t *client = (client_t *)handle->data;
  if (client) {
    if (client->buffer) free(client->buffer);
    free(client);
  }
}

static void check_pending_responses(http_server_t *server) {
  response_ctx_t **current = &server->pending_responses;
  
  while (*current) {
    response_ctx_t *ctx = *current;
    
    if (ctx->sent) {
      *current = ctx->next;
      
      if (!uv_is_closing((uv_handle_t *)ctx->client_handle)) {
        send_response((uv_stream_t *)ctx->client_handle, ctx);
        uv_close((uv_handle_t *)ctx->client_handle, on_close);
      }
      
      free(ctx);
    } else {
      current = &ctx->next;
    }
  }
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  client_t *client = (client_t *)stream->data;
  
  if (nread < 0) {
    if (nread != UV_EOF) fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
    uv_close((uv_handle_t *)stream, on_close);
    free(buf->base);
    return;
  }
  
  if (nread == 0) {
    free(buf->base);
    return;
  }
  
  size_t new_len = client->buffer_len + nread;
  if (new_len > client->buffer_capacity) {
    size_t new_capacity = client->buffer_capacity * 2;
    if (new_capacity < new_len) new_capacity = new_len;
    char *new_buffer = realloc(client->buffer, new_capacity);
    if (!new_buffer) {
      free(buf->base);
      uv_close((uv_handle_t *)stream, on_close);
      return;
    }
    client->buffer = new_buffer;
    client->buffer_capacity = new_capacity;
  }
  
  memcpy(client->buffer + client->buffer_len, buf->base, nread);
  client->buffer_len = new_len;
  free(buf->base);
  
  if (strstr(client->buffer, "\r\n\r\n")) {
    uv_read_stop(stream);
    
    http_request_t http_req = {0};
    if (parse_http_request(client->buffer, client->buffer_len, &http_req) == 0) {
      handle_http_request(client, &http_req);
      check_pending_responses(client->server);
      free_http_request(&http_req);
    } else {
      response_ctx_t *res_ctx = malloc(sizeof(response_ctx_t));
      if (res_ctx) {
        res_ctx->status = 400;
        res_ctx->body = "bad request\nant http v" ANT_VERSION " (" ANT_GIT_HASH ")";
        res_ctx->body_len = strlen(res_ctx->body);
        res_ctx->content_type = "text/plain";
        res_ctx->sent = 1;
        res_ctx->supports_gzip = 0;
        res_ctx->client_handle = &client->handle;
        res_ctx->next = client->server->pending_responses;
        client->server->pending_responses = res_ctx;
        client->response_ctx = res_ctx;
        check_pending_responses(client->server);
      }
    }
  }
}

static void on_connection(uv_stream_t *server, int status) {
  if (status < 0) {
    fprintf(stderr, "Connection error: %s\n", uv_strerror(status));
    return;
  }
  
  http_server_t *http_server = (http_server_t *)server->data;
  
  client_t *client = malloc(sizeof(client_t));
  if (!client) {
    fprintf(stderr, "Failed to allocate client\n");
    return;
  }
  
  client->server = http_server;
  client->buffer_capacity = READ_BUFFER_SIZE;
  client->buffer = malloc(client->buffer_capacity);
  client->buffer_len = 0;
  client->response_ctx = NULL;
  
  if (!client->buffer) {
    free(client);
    return;
  }
  
  uv_tcp_init(http_server->loop, &client->handle);
  client->handle.data = client;
  
  if (uv_accept(server, (uv_stream_t *)&client->handle) == 0) {
    uv_read_start((uv_stream_t *)&client->handle, alloc_buffer, on_read);
  } else {
    uv_close((uv_handle_t *)&client->handle, on_close);
  }
}

// Ant.serve(port, handler)
jsval_t js_serve(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    fprintf(stderr, "Error: Ant.serve() requires at least 1 argument (port)\n");
    return js_mkundef();
  }
  
  int port = 8000;
  if (js_type(args[0]) == JS_NUM) {
    port = (int)js_getnum(args[0]);
  }
  
  http_server_t *server = malloc(sizeof(http_server_t));
  if (server == NULL) {
    fprintf(stderr, "Error: Failed to allocate server data\n");
    return js_mkundef();
  }
  
  server->js = js;
  server->port = port;
  server->handler = (nargs >= 2) ? args[1] : js_mkundef();
  
  if (!g_loop_initialized) {
    g_loop = uv_default_loop();
    g_loop_initialized = 1;
    
    signal(SIGINT, server_signal_handler);
    signal(SIGTERM, server_signal_handler);
  }
  
  server->loop = g_loop;
  
  uv_tcp_init(g_loop, &server->server);
  server->server.data = server;
  
  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", port, &addr);
  
  int r = uv_tcp_bind(&server->server, (const struct sockaddr *)&addr, 0);
  if (r) {
    fprintf(stderr, "Error: Failed to bind to port %d: %s\n", port, uv_strerror(r));
    free(server);
    return js_mknum(0);
  }
  
  r = uv_listen((uv_stream_t *)&server->server, 128, on_connection);
  if (r) {
    fprintf(stderr, "Error: Failed to listen on port %d: %s\n", port, uv_strerror(r));
    free(server);
    return js_mknum(0);
  }
  
  server->pending_responses = NULL;
  uv_timer_init(g_loop, &g_js_timer);
  
  g_js_timer.data = js;
  rt->external_event_loop_active = 1;
  
  while (uv_loop_alive(g_loop)) {
    if (has_pending_timers()) {
      int64_t next_timeout_ms = get_next_timer_timeout();
      if (next_timeout_ms >= 0) {
        uint64_t timeout = next_timeout_ms > 0 ? (uint64_t)next_timeout_ms : 0;
        uv_timer_start(&g_js_timer, on_js_timer, timeout, 0);
      }
    } else uv_timer_stop(&g_js_timer);
    
    uv_run(g_loop, UV_RUN_ONCE);
    js_poll_events(js);
    check_pending_responses(server);
  }
  
  return js_mknum(1);
}

void init_server_module() {
  js_set(rt->js, rt->ant_obj, "serve", js_mkfun(js_serve));
}