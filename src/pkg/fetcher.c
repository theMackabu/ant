#include "fetcher.h"
#include "extractor.h"
#include <nghttp2/nghttp2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tlsuv/tlsuv.h>
#include <uv.h>

#define MAX_PENDING_REQUESTS 20
#define NUM_CONNECTIONS 6
#define NUM_META_CONNECTIONS 3
#define META_SLOW_LOG_MS 250

extern const char *ant_semver(void);

typedef enum {
  CONTENT_ENCODING_IDENTITY,
  CONTENT_ENCODING_GZIP
} content_encoding_t;

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} array_list_t;

static void array_list_init(array_list_t *list) {
  list->data = NULL;
  list->len = 0;
  list->cap = 0;
}

static void array_list_deinit(array_list_t *list) {
  free(list->data);
  list->data = NULL;
  list->len = 0;
  list->cap = 0;
}

static bool array_list_append(array_list_t *list, const uint8_t *bytes,
                              size_t count) {
  if (list->len + count > list->cap) {
    size_t new_cap = list->cap ? list->cap * 2 : 1024;
    while (new_cap < list->len + count)
      new_cap *= 2;
    uint8_t *new_data = realloc(list->data, new_cap);
    if (!new_data)
      return false;
    list->data = new_data;
    list->cap = new_cap;
  }
  memcpy(list->data + list->len, bytes, count);
  list->len += count;
  return true;
}

static void array_list_clear(array_list_t *list) { list->len = 0; }

typedef struct {
  int32_t stream_id;
  char *path;
  void (*on_data)(const uint8_t *data, size_t len, void *user_data);
  void (*on_complete)(uint16_t status_code, void *user_data);
  void (*on_error)(fetch_error_t err, void *user_data);
  void *userdata;
  array_list_t response_body;
  uint16_t status_code;
  bool done;
  bool has_error;
  uint64_t start_ns;
  uint64_t end_ns;
  size_t bytes;
  content_encoding_t content_encoding;
} request_state_t;

typedef struct {
  uv_loop_t *loop;
  tlsuv_stream_t tls;
  nghttp2_session *h2_session;
  char *host;
  bool use_tls;
  int connected;
  bool connect_pending;
  bool closing;
  array_list_t write_buf;
  request_state_t requests[MAX_PENDING_REQUESTS];
  size_t request_count;
  size_t requests_done;
  uint16_t last_response_status_code;
} http2_client_t;

static http2_client_t *http2_client_init(const char *host, bool use_tls);
static void http2_client_deinit(http2_client_t *client);
static fetch_error_t client_ensure_connected(http2_client_t *self);
static fetch_error_t client_initiate_connect_async(http2_client_t *self);
static fetch_error_t client_flush(http2_client_t *self);
static void client_reset_requests(http2_client_t *client);
static void client_recycle_completed_requests(http2_client_t *client);
static bool client_has_capacity(const http2_client_t *client);
static request_state_t *client_find_or_alloc_slot(http2_client_t *client);
static void on_stream_close_cb(uv_handle_t *handle);
static void deinit_request_state(request_state_t *req);
static request_state_t *find_request(http2_client_t *client, int32_t stream_id);

static http2_client_t *http2_client_init(const char *host, bool use_tls) {
  http2_client_t *client = calloc(1, sizeof(http2_client_t));
  if (!client)
    return NULL;

  client->host = strdup(host);
  if (!client->host) {
    free(client);
    return NULL;
  }

  client->loop = uv_default_loop();
  client->use_tls = use_tls;
  array_list_init(&client->write_buf);

  for (size_t i = 0; i < MAX_PENDING_REQUESTS; i++) {
    client->requests[i].stream_id = -1;
  }

  if (tlsuv_stream_init(client->loop, &client->tls, NULL) != 0) {
    free(client->host);
    free(client);
    return NULL;
  }

  tlsuv_stream_set_hostname(&client->tls, client->host);
  static const char *const alpn_protocols[] = {"h2", "http/1.1"};
  tlsuv_stream_set_protocols(&client->tls, 2, alpn_protocols);

  return client;
}

static void http2_client_deinit(http2_client_t *client) {
  if (!client)
    return;
  client->closing = true;
  client->connect_pending = false;

  for (size_t i = 0; i < client->request_count; i++) {
    client->requests[i].on_data = NULL;
    client->requests[i].on_complete = NULL;
    client->requests[i].on_error = NULL;
    client->requests[i].userdata = NULL;
  }

  if (client->connected > 0) {
    client->tls.data = client;
    tlsuv_stream_close(&client->tls, on_stream_close_cb);
    while (client->connected > 0) {
      uv_run(client->loop, UV_RUN_ONCE);
    }
  }

  if (client->h2_session) {
    nghttp2_session_del(client->h2_session);
  }

  for (size_t i = 0; i < client->request_count; i++) {
    if (client->requests[i].stream_id != -1) {
      deinit_request_state(&client->requests[i]);
    }
  }

  array_list_deinit(&client->write_buf);
  free(client->host);
  free(client);
}

typedef struct {
  fetch_stream_handler_t handler;
  bool done;
  bool has_error;
  char *url;
  uint64_t start_ns;
  size_t bytes;
} tarball_ctx_t;

typedef struct {
  char *url;
  size_t bytes;
  uint64_t elapsed_ms;
} tarball_stats_t;

typedef struct {
  char *url;
  fetch_stream_handler_t handler;
} pending_req_t;

struct fetcher {
  char *registry_host;
  http2_client_t *meta_clients[NUM_META_CONNECTIONS];
  bool meta_clients_initialized;

  pending_req_t *pending;
  size_t pending_count;
  size_t pending_cap;

  http2_client_t *tarball_clients[NUM_CONNECTIONS];
  bool tarball_clients_initialized;

  tarball_ctx_t **tarball_contexts;
  size_t tarball_contexts_count;
  size_t tarball_contexts_cap;

  size_t tarball_round_robin;

  tarball_stats_t *tarball_stats;
  size_t tarball_stats_count;
  size_t tarball_stats_cap;

  char *last_http_error_url;
  uint16_t last_http_error_status;
};

typedef struct {
  char scheme[16];
  char host[256];
  uint16_t port;
  char path[1024];
} parsed_url_t;

static uint64_t get_time_ns(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (uint64_t)(counter.QuadPart * 1000000000ULL / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

static bool parse_url(const char *url, parsed_url_t *out) {
  const char *p = strstr(url, "://");
  if (!p)
    return false;
  size_t scheme_len = p - url;
  if (scheme_len >= sizeof(out->scheme))
    return false;
  strncpy(out->scheme, url, scheme_len);
  out->scheme[scheme_len] = '\0';

  const char *host_port = p + 3;
  const char *path_start = strchr(host_port, '/');
  size_t host_port_len =
      path_start ? (size_t)(path_start - host_port) : strlen(host_port);

  if (host_port_len >= sizeof(out->host))
    return false;
  char host_port_buf[256];
  strncpy(host_port_buf, host_port, host_port_len);
  host_port_buf[host_port_len] = '\0';

  if (path_start) {
    if (strlen(path_start) >= sizeof(out->path))
      return false;
    strcpy(out->path, path_start);
  } else {
    strcpy(out->path, "/");
  }

  out->port = (strcmp(out->scheme, "https") == 0) ? 443 : 80;
  char *colon = strchr(host_port_buf, ':');
  if (colon) {
    *colon = '\0';
    out->port = (uint16_t)atoi(colon + 1);
  }
  strcpy(out->host, host_port_buf);
  return true;
}

static void init_request_state(request_state_t *req) {
  req->stream_id = 0;
  req->path = NULL;
  req->on_data = NULL;
  req->on_complete = NULL;
  req->on_error = NULL;
  req->userdata = NULL;
  array_list_init(&req->response_body);
  req->status_code = 0;
  req->done = false;
  req->has_error = false;
  req->start_ns = 0;
  req->end_ns = 0;
  req->bytes = 0;
  req->content_encoding = CONTENT_ENCODING_IDENTITY;
}

static void deinit_request_state(request_state_t *req) {
  free(req->path);
  array_list_deinit(&req->response_body);
}

static request_state_t *find_request(http2_client_t *client,
                                     int32_t stream_id) {
  for (size_t i = 0; i < client->request_count; i++) {
    if (client->requests[i].stream_id == stream_id) {
      return &client->requests[i];
    }
  }
  return NULL;
}

static void client_reset_requests(http2_client_t *client) {
  for (size_t i = 0; i < client->request_count; i++) {
    deinit_request_state(&client->requests[i]);
  }
  client->request_count = 0;
  client->requests_done = 0;
}

static bool client_has_capacity(const http2_client_t *client) {
  for (size_t i = 0; i < client->request_count; i++) {
    if (client->requests[i].stream_id == -1)
      return true;
  }
  return client->request_count < MAX_PENDING_REQUESTS - 1;
}

static void client_recycle_completed_requests(http2_client_t *client) {
  if (client->requests_done == 0)
    return;
  for (size_t i = 0; i < client->request_count; i++) {
    request_state_t *req = &client->requests[i];
    if (req->done && req->stream_id != -1) {
      deinit_request_state(req);
      req->stream_id = -1;
    }
  }
}

static request_state_t *client_find_or_alloc_slot(http2_client_t *client) {
  for (size_t i = 0; i < client->request_count; i++) {
    if (client->requests[i].stream_id == -1) {
      init_request_state(&client->requests[i]);
      return &client->requests[i];
    }
  }
  if (client->request_count < MAX_PENDING_REQUESTS) {
    request_state_t *req = &client->requests[client->request_count++];
    init_request_state(req);
    return req;
  }
  return NULL;
}

static ssize_t h2_send_cb(nghttp2_session *session, const uint8_t *data,
                          size_t length, int flags, void *user_data) {
  (void)session;
  (void)flags;
  http2_client_t *client = (http2_client_t *)user_data;
  if (!array_list_append(&client->write_buf, data, length)) {
    return NGHTTP2_ERR_NOMEM;
  }
  return (ssize_t)length;
}

static int h2_on_frame_recv_cb(nghttp2_session *session,
                               const nghttp2_frame *frame, void *user_data) {
  (void)session;
  http2_client_t *client = (http2_client_t *)user_data;
  if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
    request_state_t *req = find_request(client, frame->hd.stream_id);
    if (req && !req->done) {
      req->done = true;
      client->requests_done++;
      if (req->on_complete) {
        req->on_complete(req->status_code, req->userdata);
      }
    }
  }
  return 0;
}

static int h2_on_data_chunk_recv_cb(nghttp2_session *session, uint8_t flags,
                                    int32_t stream_id, const uint8_t *data,
                                    size_t len, void *user_data) {
  (void)flags;
  http2_client_t *client = (http2_client_t *)user_data;
  request_state_t *req = find_request(client, stream_id);
  if (!req)
    return 0;

  if (req->on_data) {
    req->on_data(data, len, req->userdata);
  } else {
    if (!array_list_append(&req->response_body, data, len)) {
      req->has_error = true;
    }
  }
  req->bytes += len;
  nghttp2_session_consume(session, stream_id, len);
  return 0;
}

static int h2_on_header_cb(nghttp2_session *session, const nghttp2_frame *frame,
                           const uint8_t *name, size_t namelen,
                           const uint8_t *value, size_t valuelen, uint8_t flags,
                           void *user_data) {
  (void)session;
  (void)flags;
  http2_client_t *client = (http2_client_t *)user_data;
  if (frame->hd.type != NGHTTP2_HEADERS)
    return 0;

  request_state_t *req = find_request(client, frame->hd.stream_id);
  if (!req)
    return 0;

  if (namelen == 7 && memcmp(name, ":status", 7) == 0) {
    char val_buf[16];
    size_t copy_len =
        (valuelen < sizeof(val_buf) - 1) ? valuelen : sizeof(val_buf) - 1;
    memcpy(val_buf, value, copy_len);
    val_buf[copy_len] = '\0';
    req->status_code = (uint16_t)atoi(val_buf);
  }
  if (namelen == 16 && memcmp(name, "content-encoding", 16) == 0) {
    if (valuelen >= 4 && memcmp(value, "gzip", 4) == 0) {
      req->content_encoding = CONTENT_ENCODING_GZIP;
    }
  }
  return 0;
}

static int h2_on_stream_close_cb(nghttp2_session *session, int32_t stream_id,
                                 uint32_t error_code, void *user_data) {
  (void)session;
  http2_client_t *client = (http2_client_t *)user_data;
  request_state_t *req = find_request(client, stream_id);
  if (!req)
    return 0;

  if (!req->done) {
    req->done = true;
    client->requests_done++;
    if (error_code != 0) {
      req->has_error = true;
      if (req->on_error) {
        req->on_error(FETCH_HTTP2_ERROR, req->userdata);
      }
    } else {
      if (req->on_complete) {
        req->on_complete(req->status_code, req->userdata);
      }
    }
  }
  return 0;
}

static void on_stream_close_cb(uv_handle_t *handle) {
  tlsuv_stream_t *tls = (tlsuv_stream_t *)handle;
  http2_client_t *client = (http2_client_t *)tls->data;
  client->connected = -2;
  client->connect_pending = false;
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size,
                         uv_buf_t *buf) {
  (void)handle;
  buf->base = malloc(suggested_size);
  buf->len = buf->base ? suggested_size : 0;
}

static void on_write_cb(uv_write_t *wr, int status) {
  (void)status;
  free(wr->data);
  free(wr);
}

static fetch_error_t client_flush(http2_client_t *self) {
  if (self->closing)
    return FETCH_CONNECTION_FAILED;
  if (self->h2_session) {
    while (nghttp2_session_want_write(self->h2_session) != 0) {
      if (nghttp2_session_send(self->h2_session) != 0) {
        break;
      }
    }
  }

  if (self->write_buf.len > 0) {
    uint8_t *data_copy = malloc(self->write_buf.len);
    if (!data_copy)
      return FETCH_OUT_OF_MEMORY;
    memcpy(data_copy, self->write_buf.data, self->write_buf.len);

    uv_write_t *wr = malloc(sizeof(uv_write_t));
    if (!wr) {
      free(data_copy);
      return FETCH_OUT_OF_MEMORY;
    }
    wr->data = data_copy;

    uv_buf_t buf = uv_buf_init((char *)data_copy, self->write_buf.len);
    array_list_clear(&self->write_buf);

    if (tlsuv_stream_write(wr, &self->tls, &buf, on_write_cb) != 0) {
      free(data_copy);
      free(wr);
      return FETCH_CONNECTION_FAILED;
    }
  }
  return FETCH_OK;
}

static void on_read_cb(uv_stream_t *stream, ssize_t nread,
                       const uv_buf_t *buf) {
  tlsuv_stream_t *tls = (tlsuv_stream_t *)stream;
  http2_client_t *client = (http2_client_t *)tls->data;
  if (client->closing) {
    if (buf->base)
      free(buf->base);
    return;
  }

  if (nread < 0) {
    for (size_t i = 0; i < client->request_count; i++) {
      request_state_t *req = &client->requests[i];
      if (req->stream_id != -1 && !req->done) {
        req->done = true;
        req->has_error = true;
        client->requests_done++;
        if (req->on_error) {
          req->on_error(FETCH_CONNECTION_FAILED, req->userdata);
        }
      }
    }
    if (buf->base)
      free(buf->base);
    return;
  }

  if (nread > 0 && client->h2_session) {
    nghttp2_session_mem_recv(client->h2_session, (const uint8_t *)buf->base,
                             nread);
    client_flush(client);
  }
  if (buf->base)
    free(buf->base);
}

static fetch_error_t client_init_h2(http2_client_t *self) {
  nghttp2_session_callbacks *callbacks;
  if (nghttp2_session_callbacks_new(&callbacks) != 0)
    return FETCH_HTTP2_ERROR;

  nghttp2_session_callbacks_set_send_callback2(callbacks, h2_send_cb);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       h2_on_frame_recv_cb);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, h2_on_data_chunk_recv_cb);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, h2_on_header_cb);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks,
                                                         h2_on_stream_close_cb);

  int ret = nghttp2_session_client_new(&self->h2_session, callbacks, self);
  nghttp2_session_callbacks_del(callbacks);
  if (ret != 0)
    return FETCH_HTTP2_ERROR;

  nghttp2_settings_entry settings[] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, MAX_PENDING_REQUESTS},
      {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 16 * 1024 * 1024}};
  if (nghttp2_submit_settings(self->h2_session, NGHTTP2_FLAG_NONE, settings,
                              sizeof(settings) / sizeof(settings[0])) != 0) {
    return FETCH_HTTP2_ERROR;
  }

  int32_t conn_window_increase = (16 * 1024 * 1024) - 65535;
  nghttp2_submit_window_update(self->h2_session, NGHTTP2_FLAG_NONE, 0,
                               conn_window_increase);

  return FETCH_OK;
}

typedef struct {
  http2_client_t *client;
  uv_connect_t req;
} connect_ctx_t;

static void on_connect_cb(uv_connect_t *req, int status) {
  connect_ctx_t *ctx = (connect_ctx_t *)req->data;
  http2_client_t *client = ctx->client;
  client->connect_pending = false;

  if (client->closing) {
    client->connected = -1;
    tlsuv_stream_close(&client->tls, on_stream_close_cb);
    free(ctx);
    return;
  }

  if (status < 0) {
    client->connected = -1;
    free(ctx);
    return;
  }

  client->connected = 1;
  client->tls.data = client;

  if (client_init_h2(client) != FETCH_OK) {
    client->connected = -1;
    free(ctx);
    return;
  }

  tlsuv_stream_read_start(&client->tls, alloc_buffer, (uv_read_cb)on_read_cb);
  client_flush(client);
  free(ctx);
}

static fetch_error_t client_ensure_connected(http2_client_t *self) {
  if (self->closing)
    return FETCH_CONNECTION_FAILED;
  if (self->connected > 0)
    return FETCH_OK;
  if (self->connected < 0)
    return FETCH_CONNECTION_FAILED;

  if (!self->connect_pending) {
    connect_ctx_t *ctx = malloc(sizeof(connect_ctx_t));
    if (!ctx)
      return FETCH_OUT_OF_MEMORY;
    ctx->client = self;
    ctx->req.data = ctx;

    int port = self->use_tls ? 443 : 80;
    if (tlsuv_stream_connect(&ctx->req, &self->tls, self->host, port,
                             on_connect_cb) != 0) {
      free(ctx);
      return FETCH_CONNECTION_FAILED;
    }
    self->connect_pending = true;
  }

  while (self->connected == 0) {
    uv_run(self->loop, UV_RUN_ONCE);
  }

  if (self->connected < 0)
    return FETCH_CONNECTION_FAILED;
  return FETCH_OK;
}

static fetch_error_t client_initiate_connect_async(http2_client_t *self) {
  if (self->closing)
    return FETCH_CONNECTION_FAILED;
  if (self->connected > 0)
    return FETCH_OK;
  if (self->connected < 0)
    return FETCH_CONNECTION_FAILED;
  if (self->connect_pending)
    return FETCH_OK;

  connect_ctx_t *ctx = malloc(sizeof(connect_ctx_t));
  if (!ctx)
    return FETCH_OUT_OF_MEMORY;
  ctx->client = self;
  ctx->req.data = ctx;

  int port = self->use_tls ? 443 : 80;
  if (tlsuv_stream_connect(&ctx->req, &self->tls, self->host, port,
                           on_connect_cb) != 0) {
    free(ctx);
    return FETCH_CONNECTION_FAILED;
  }
  self->connect_pending = true;
  return FETCH_OK;
}

static nghttp2_nv make_nv(const char *name, const char *value) {
  nghttp2_nv nv;
  nv.name = (uint8_t *)name;
  nv.value = (uint8_t *)value;
  nv.namelen = strlen(name);
  nv.valuelen = strlen(value);
  nv.flags = NGHTTP2_NV_FLAG_NONE;
  return nv;
}

static fetch_error_t http2_client_get_stream(
    http2_client_t *self, const char *path,
    void (*on_data)(const uint8_t *data, size_t len, void *user_data),
    void (*on_complete)(uint16_t status_code, void *user_data),
    void (*on_error)(fetch_error_t err, void *user_data), void *userdata) {
  fetch_error_t err = client_ensure_connected(self);
  if (err != FETCH_OK)
    return err;

  request_state_t *req = client_find_or_alloc_slot(self);
  if (!req)
    return FETCH_OUT_OF_MEMORY;

  req->path = strdup(path);
  if (!req->path)
    return FETCH_OUT_OF_MEMORY;
  req->on_data = on_data;
  req->on_complete = on_complete;
  req->on_error = on_error;
  req->userdata = userdata;
  req->start_ns = get_time_ns();

  char ua_buf[128];
  snprintf(ua_buf, sizeof(ua_buf), "ant/%s", ant_semver());

  nghttp2_nv hdrs[] = {
      make_nv(":method", "GET"),   make_nv(":path", req->path),
      make_nv(":scheme", "https"), make_nv(":authority", self->host),
      make_nv("accept", "*/*"),    make_nv("user-agent", ua_buf)};

  int32_t sid = nghttp2_submit_request(
      self->h2_session, NULL, hdrs, sizeof(hdrs) / sizeof(hdrs[0]), NULL, req);
  if (sid < 0) {
    free(req->path);
    req->path = NULL;
    req->stream_id = -1;
    return FETCH_HTTP2_ERROR;
  }
  req->stream_id = sid;
  return client_flush(self);
}

static void append_to_array_list(const uint8_t *data, size_t len,
                                 void *user_data) {
  array_list_t *list = (array_list_t *)user_data;
  array_list_append(list, data, len);
}

static uint8_t *decode_metadata(request_state_t *req, size_t *out_len,
                                bool *out_compressed) {
  if (req->has_error || req->status_code != 200)
    return NULL;

  if (req->content_encoding != CONTENT_ENCODING_GZIP) {
    uint8_t *data = malloc(req->response_body.len + 1);
    if (!data)
      return NULL;
    memcpy(data, req->response_body.data, req->response_body.len);
    data[req->response_body.len] = '\0';
    if (out_len)
      *out_len = req->response_body.len;
    if (out_compressed)
      *out_compressed = false;
    return data;
  }

  array_list_t decomp_buf;
  array_list_init(&decomp_buf);

  extract_error_t err =
      gzip_decompress(req->response_body.data, req->response_body.len,
                      append_to_array_list, &decomp_buf);
  if (err != EXTRACT_OK) {
    array_list_deinit(&decomp_buf);
    return NULL;
  }

  uint8_t *data = malloc(decomp_buf.len + 1);
  if (!data) {
    array_list_deinit(&decomp_buf);
    return NULL;
  }
  memcpy(data, decomp_buf.data, decomp_buf.len);
  data[decomp_buf.len] = '\0';
  if (out_len)
    *out_len = decomp_buf.len;
  if (out_compressed)
    *out_compressed = true;

  array_list_deinit(&decomp_buf);
  return data;
}

static uint8_t *http2_client_get_with_accept(http2_client_t *self,
                                             const char *path,
                                             const char *accept,
                                             size_t *out_len,
                                             uint16_t *out_status) {
  fetch_error_t err = client_ensure_connected(self);
  if (err != FETCH_OK)
    return NULL;

  if (self->request_count >= MAX_PENDING_REQUESTS) {
    client_reset_requests(self);
  }

  request_state_t *req = &self->requests[self->request_count++];
  init_request_state(req);

  req->path = strdup(path);
  if (!req->path)
    return NULL;
  req->start_ns = get_time_ns();

  char ua_buf[128];
  snprintf(ua_buf, sizeof(ua_buf), "ant/%s", ant_semver());

  nghttp2_nv hdrs[] = {
      make_nv(":method", "GET"),   make_nv(":path", req->path),
      make_nv(":scheme", "https"), make_nv(":authority", self->host),
      make_nv("accept", accept),   make_nv("user-agent", ua_buf)};

  int32_t sid = nghttp2_submit_request(
      self->h2_session, NULL, hdrs, sizeof(hdrs) / sizeof(hdrs[0]), NULL, req);
  if (sid < 0) {
    self->request_count--;
    deinit_request_state(req);
    return NULL;
  }
  req->stream_id = sid;

  if (client_flush(self) != FETCH_OK)
    return NULL;

  while (!req->done) {
    uv_run(self->loop, UV_RUN_ONCE);
    client_flush(self);
  }

  self->last_response_status_code = req->status_code;
  if (out_status)
    *out_status = req->status_code;

  if (req->has_error || req->status_code != 200) {
    return NULL;
  }

  bool compressed = false;
  return decode_metadata(req, out_len, &compressed);
}

fetcher_t *fetcher_init(const char *registry_host) {
  fetcher_t *self = calloc(1, sizeof(fetcher_t));
  if (!self)
    return NULL;

  self->registry_host = strdup(registry_host);
  if (!self->registry_host) {
    free(self);
    return NULL;
  }

  return self;
}

void fetcher_deinit(fetcher_t *self) {
  if (!self)
    return;

  free(self->last_http_error_url);

  for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
    if (self->meta_clients[i]) {
      http2_client_deinit(self->meta_clients[i]);
    }
  }

  for (size_t i = 0; i < self->pending_count; i++) {
    free(self->pending[i].url);
  }
  free(self->pending);

  for (size_t i = 0; i < NUM_CONNECTIONS; i++) {
    if (self->tarball_clients[i]) {
      http2_client_deinit(self->tarball_clients[i]);
    }
  }

  for (size_t i = 0; i < self->tarball_contexts_count; i++) {
    free(self->tarball_contexts[i]->url);
    free(self->tarball_contexts[i]);
  }
  free(self->tarball_contexts);

  for (size_t i = 0; i < self->tarball_stats_count; i++) {
    free(self->tarball_stats[i].url);
  }
  free(self->tarball_stats);

  free(self->registry_host);
  free(self);
}

static void fetcher_clear_last_http_error(fetcher_t *self) {
  free(self->last_http_error_url);
  self->last_http_error_url = NULL;
  self->last_http_error_status = 0;
}

static void fetcher_set_last_http_error(fetcher_t *self, const char *url,
                                        uint16_t status) {
  fetcher_clear_last_http_error(self);
  self->last_http_error_url = strdup(url);
  self->last_http_error_status = status;
}

static fetch_error_t fetcher_ensure_meta_clients(fetcher_t *self) {
  if (self->meta_clients_initialized)
    return FETCH_OK;

  bool any_connected = false;
  for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
    http2_client_t *client = http2_client_init(self->registry_host, true);
    if (!client)
      continue;

    if (client_ensure_connected(client) != FETCH_OK) {
      http2_client_deinit(client);
      continue;
    }
    self->meta_clients[i] = client;
    any_connected = true;
  }

  if (!any_connected)
    return FETCH_CONNECTION_FAILED;
  self->meta_clients_initialized = true;
  return FETCH_OK;
}

static fetch_error_t fetcher_ensure_tarball_clients(fetcher_t *self) {
  if (self->tarball_clients_initialized)
    return FETCH_OK;

  bool any_connected = false;
  for (size_t i = 0; i < NUM_CONNECTIONS; i++) {
    http2_client_t *client = http2_client_init(self->registry_host, true);
    if (!client)
      continue;

    if (client_ensure_connected(client) != FETCH_OK) {
      http2_client_deinit(client);
      continue;
    }
    self->tarball_clients[i] = client;
    any_connected = true;
  }

  if (!any_connected)
    return FETCH_CONNECTION_FAILED;
  self->tarball_clients_initialized = true;
  return FETCH_OK;
}

void fetcher_initiate_tarball_connections_async(fetcher_t *self) {
  if (self->tarball_clients_initialized)
    return;

  bool any_connected = false;
  for (size_t i = 0; i < NUM_CONNECTIONS; i++) {
    http2_client_t *client = http2_client_init(self->registry_host, true);
    if (!client)
      continue;

    if (client_initiate_connect_async(client) != FETCH_OK) {
      http2_client_deinit(client);
      continue;
    }
    self->tarball_clients[i] = client;
    any_connected = true;
  }

  if (any_connected) {
    self->tarball_clients_initialized = true;
  }
}

static http2_client_t *fetcher_find_available_client(fetcher_t *self,
                                                     size_t *out_idx) {
  for (size_t i = 0; i < NUM_CONNECTIONS; i++) {
    size_t idx = (self->tarball_round_robin + i) % NUM_CONNECTIONS;
    http2_client_t *client = self->tarball_clients[idx];
    if (client && client_has_capacity(client)) {
      if (out_idx)
        *out_idx = idx;
      return client;
    }
  }
  return NULL;
}

static void tarball_on_data(const uint8_t *data, size_t len, void *user_data) {
  tarball_ctx_t *ctx = (tarball_ctx_t *)user_data;
  ctx->bytes += len;
  ctx->handler.on_data(data, len, ctx->handler.user_data);
}

static void tarball_on_complete(uint16_t status_code, void *user_data) {
  tarball_ctx_t *ctx = (tarball_ctx_t *)user_data;
  ctx->handler.on_complete(status_code, ctx->handler.user_data);
  ctx->done = true;
}

static void tarball_on_error(fetch_error_t err, void *user_data) {
  tarball_ctx_t *ctx = (tarball_ctx_t *)user_data;
  ctx->handler.on_error(err, ctx->handler.user_data);
  ctx->done = true;
  ctx->has_error = true;
}

fetch_error_t fetcher_fetch_tarball(fetcher_t *self, const char *url,
                                    fetch_stream_handler_t handler) {
  if (self->pending_count >= self->pending_cap) {
    self->pending_cap = self->pending_cap ? self->pending_cap * 2 : 32;
    pending_req_t *new_pending =
        realloc(self->pending, self->pending_cap * sizeof(pending_req_t));
    if (!new_pending)
      return FETCH_OUT_OF_MEMORY;
    self->pending = new_pending;
  }
  self->pending[self->pending_count].url = strdup(url);
  self->pending[self->pending_count].handler = handler;
  self->pending_count++;
  return FETCH_OK;
}

static fetch_error_t fetcher_dispatch_request(fetcher_t *self,
                                              http2_client_t *client,
                                              const char *url,
                                              fetch_stream_handler_t handler) {
  parsed_url_t parsed;
  if (!parse_url(url, &parsed))
    return FETCH_INVALID_URL;

  tarball_ctx_t *ctx = calloc(1, sizeof(tarball_ctx_t));
  if (!ctx)
    return FETCH_OUT_OF_MEMORY;
  ctx->handler = handler;
  ctx->url = strdup(url);
  ctx->start_ns = get_time_ns();

  if (self->tarball_contexts_count >= self->tarball_contexts_cap) {
    self->tarball_contexts_cap =
        self->tarball_contexts_cap ? self->tarball_contexts_cap * 2 : 32;
    tarball_ctx_t **new_ctxs =
        realloc(self->tarball_contexts,
                self->tarball_contexts_cap * sizeof(tarball_ctx_t *));
    if (!new_ctxs) {
      free(ctx->url);
      free(ctx);
      return FETCH_OUT_OF_MEMORY;
    }
    self->tarball_contexts = new_ctxs;
  }
  self->tarball_contexts[self->tarball_contexts_count++] = ctx;

  fetch_error_t err =
      http2_client_get_stream(client, parsed.path, tarball_on_data,
                              tarball_on_complete, tarball_on_error, ctx);
  if (err != FETCH_OK) {
    self->tarball_contexts_count--;
    free(ctx->url);
    free(ctx);
    return err;
  }
  return FETCH_OK;
}

static void fetcher_dispatch_pending(fetcher_t *self) {
  while (self->pending_count > 0) {
    size_t client_idx = 0;
    http2_client_t *client = fetcher_find_available_client(self, &client_idx);
    if (!client)
      break;

    pending_req_t req = self->pending[0];
    memmove(self->pending, self->pending + 1,
            (self->pending_count - 1) * sizeof(pending_req_t));
    self->pending_count--;

    fetch_error_t err =
        fetcher_dispatch_request(self, client, req.url, req.handler);
    if (err != FETCH_OK) {
      if (req.handler.on_error) {
        req.handler.on_error(err, req.handler.user_data);
      }
    }
    free(req.url);
  }
}

static size_t fetcher_cleanup_completed_contexts(fetcher_t *self) {
  size_t completed = 0;
  size_t i = 0;
  while (i < self->tarball_contexts_count) {
    tarball_ctx_t *ctx = self->tarball_contexts[i];
    if (ctx->done) {
      completed++;
      if (!ctx->has_error) {
        uint64_t elapsed_ms = (get_time_ns() - ctx->start_ns) / 1000000;
        if (self->tarball_stats_count >= self->tarball_stats_cap) {
          self->tarball_stats_cap =
              self->tarball_stats_cap ? self->tarball_stats_cap * 2 : 32;
          tarball_stats_t *new_stats =
              realloc(self->tarball_stats,
                      self->tarball_stats_cap * sizeof(tarball_stats_t));
          if (new_stats)
            self->tarball_stats = new_stats;
        }
        if (self->tarball_stats_count < self->tarball_stats_cap) {
          self->tarball_stats[self->tarball_stats_count].url = strdup(ctx->url);
          self->tarball_stats[self->tarball_stats_count].bytes = ctx->bytes;
          self->tarball_stats[self->tarball_stats_count].elapsed_ms =
              elapsed_ms;
          self->tarball_stats_count++;
        }
      }
      free(ctx->url);
      free(ctx);
      self->tarball_contexts[i] =
          self->tarball_contexts[self->tarball_contexts_count - 1];
      self->tarball_contexts_count--;
    } else {
      i++;
    }
  }
  return completed;
}

size_t fetcher_tick(fetcher_t *self) {
  if (fetcher_ensure_tarball_clients(self) != FETCH_OK)
    return 0;

  for (size_t i = 0; i < NUM_CONNECTIONS; i++) {
    if (self->tarball_clients[i]) {
      client_flush(self->tarball_clients[i]);
    }
  }

  uv_run(uv_default_loop(), UV_RUN_NOWAIT);

  for (size_t i = 0; i < NUM_CONNECTIONS; i++) {
    if (self->tarball_clients[i]) {
      client_recycle_completed_requests(self->tarball_clients[i]);
    }
  }

  size_t completed = fetcher_cleanup_completed_contexts(self);
  fetcher_dispatch_pending(self);

  return completed;
}

void fetcher_finish_tarballs(fetcher_t *self) {
  if (fetcher_ensure_tarball_clients(self) != FETCH_OK) {
    while (self->pending_count > 0) {
      pending_req_t req = self->pending[0];
      memmove(self->pending, self->pending + 1,
              (self->pending_count - 1) * sizeof(pending_req_t));
      self->pending_count--;
      if (req.handler.on_error) {
        req.handler.on_error(FETCH_CONNECTION_FAILED, req.handler.user_data);
      }
      free(req.url);
    }
    return;
  }

  fetcher_dispatch_pending(self);

  while (self->tarball_contexts_count > 0 || self->pending_count > 0) {
    for (size_t i = 0; i < NUM_CONNECTIONS; i++) {
      if (self->tarball_clients[i]) {
        client_flush(self->tarball_clients[i]);
      }
    }

    if (uv_run(uv_default_loop(), UV_RUN_ONCE) == 0 &&
        self->pending_count == 0 && self->tarball_contexts_count == 0) {
      break;
    }

    for (size_t i = 0; i < NUM_CONNECTIONS; i++) {
      if (self->tarball_clients[i]) {
        client_recycle_completed_requests(self->tarball_clients[i]);
      }
    }

    fetcher_cleanup_completed_contexts(self);
    fetcher_dispatch_pending(self);
  }
}

size_t fetcher_pending_tarball_count(const fetcher_t *self) {
  return self->tarball_contexts_count;
}

fetch_error_t fetcher_run(fetcher_t *self) {
  if (self->pending_count == 0 && self->tarball_contexts_count == 0)
    return FETCH_OK;
  fetch_error_t err = fetcher_ensure_tarball_clients(self);
  if (err != FETCH_OK)
    return err;

  fetcher_finish_tarballs(self);
  return FETCH_OK;
}

char *fetcher_fetch_metadata_full(fetcher_t *self, const char *package_name,
                                  bool full, size_t *out_len) {
  if (fetcher_ensure_meta_clients(self) != FETCH_OK)
    return NULL;
  fetcher_clear_last_http_error(self);

  char path_buf[512];
  snprintf(path_buf, sizeof(path_buf), "/%s", package_name);
  const char *accept =
      full ? "application/json" : "application/vnd.npm.install-v1+json";

  for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
    http2_client_t *client = self->meta_clients[i];
    if (client) {
      uint16_t status = 0;
      uint8_t *data = http2_client_get_with_accept(client, path_buf, accept,
                                                   out_len, &status);
      if (data) {
        return (char *)data;
      }
      if (status != 0 && status != 200) {
        char url_buf[1024];
        snprintf(url_buf, sizeof(url_buf), "https://%s/%s", self->registry_host,
                 package_name);
        fetcher_set_last_http_error(self, url_buf, status);
      }
    }
  }
  return NULL;
}

char *fetcher_fetch_metadata(fetcher_t *self, const char *package_name,
                             size_t *out_len) {
  return fetcher_fetch_metadata_full(self, package_name, false, out_len);
}

static bool meta_client_can_queue(const http2_client_t *c) {
  return c->h2_session != NULL && c->connected == 1 &&
         c->request_count < MAX_PENDING_REQUESTS - 1;
}

static http2_client_t *fetcher_next_meta_client(fetcher_t *self,
                                                size_t *conn_idx) {
  for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
    size_t idx = (*conn_idx + i) % NUM_META_CONNECTIONS;
    http2_client_t *c = self->meta_clients[idx];
    if (c && meta_client_can_queue(c)) {
      *conn_idx = idx;
      return c;
    }
  }
  return NULL;
}

static bool fetcher_meta_requests_complete(fetcher_t *self) {
  for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
    http2_client_t *c = self->meta_clients[i];
    if (c) {
      for (size_t j = 0; j < c->request_count; j++) {
        request_state_t *req = &c->requests[j];
        if (req->stream_id != -1 && !req->done && !req->has_error) {
          return false;
        }
      }
    }
  }
  return true;
}

fetch_error_t
fetcher_fetch_metadata_batch(fetcher_t *self, const char *const *names,
                             size_t count,
                             fetch_metadata_result_t *out_results) {
  if (count == 0)
    return FETCH_OK;
  fetch_error_t err = fetcher_ensure_meta_clients(self);
  if (err != FETCH_OK)
    return err;

  size_t active_connections = 0;
  for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
    if (self->meta_clients[i])
      active_connections++;
  }
  if (active_connections == 0)
    return FETCH_CONNECTION_FAILED;

  for (size_t i = 0; i < count; i++) {
    out_results[i].name = names[i];
    out_results[i].data = NULL;
    out_results[i].data_len = 0;
    out_results[i].has_error = false;
    out_results[i].compressed = false;
  }

  size_t total_capacity = active_connections * (MAX_PENDING_REQUESTS - 1);
  size_t offset = 0;

  while (offset < count) {
    size_t end =
        (offset + total_capacity < count) ? (offset + total_capacity) : count;
    size_t conn_idx = 0;

    for (size_t i = offset; i < end; i++) {
      fetch_metadata_result_t *result = &out_results[i];
      http2_client_t *c = fetcher_next_meta_client(self, &conn_idx);
      if (!c || !c->h2_session) {
        result->has_error = true;
        continue;
      }

      char path_buf[512];
      snprintf(path_buf, sizeof(path_buf), "/%s", result->name);

      request_state_t *req = &c->requests[c->request_count++];
      init_request_state(req);
      req->path = strdup(path_buf);
      req->userdata = result;
      req->start_ns = get_time_ns();

      char ua_buf[128];
      snprintf(ua_buf, sizeof(ua_buf), "ant/%s", ant_semver());

      nghttp2_nv hdrs[] = {
          make_nv(":method", "GET"),
          make_nv(":path", req->path),
          make_nv(":scheme", "https"),
          make_nv(":authority", c->host),
          make_nv("accept", "application/vnd.npm.install-v1+json"),
          make_nv("accept-encoding", "gzip"),
          make_nv("user-agent", ua_buf)};

      int32_t sid = nghttp2_submit_request(
          c->h2_session, NULL, hdrs, sizeof(hdrs) / sizeof(hdrs[0]), NULL, req);
      if (sid < 0) {
        c->request_count--;
        deinit_request_state(req);
        result->has_error = true;
        continue;
      }
      req->stream_id = sid;
      conn_idx = (conn_idx + 1) % NUM_META_CONNECTIONS;
    }

    for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
      if (self->meta_clients[i])
        client_flush(self->meta_clients[i]);
    }

    while (!fetcher_meta_requests_complete(self)) {
      uv_run(uv_default_loop(), UV_RUN_ONCE);
    }

    for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
      http2_client_t *c = self->meta_clients[i];
      if (c) {
        for (size_t j = 0; j < c->request_count; j++) {
          request_state_t *req = &c->requests[j];
          fetch_metadata_result_t *res =
              (fetch_metadata_result_t *)req->userdata;
          if (req->has_error || req->status_code != 200) {
            res->has_error = true;
          } else {
            res->data = decode_metadata(req, &res->data_len, &res->compressed);
            if (!res->data)
              res->has_error = true;
          }
        }
        client_reset_requests(c);
      }
    }

    offset = end;
  }

  return FETCH_OK;
}

typedef struct {
  const char *name;
  size_t index;
} metadata_stream_tracker_t;

static void emit_completed_streaming_callbacks(fetcher_t *self, bool *processed,
                                               fetch_metadata_cb callback,
                                               void *user_data) {
  for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
    http2_client_t *c = self->meta_clients[i];
    if (c) {
      for (size_t j = 0; j < c->request_count; j++) {
        request_state_t *req = &c->requests[j];
        if (req->stream_id != -1 && (req->done || req->has_error)) {
          metadata_stream_tracker_t *tracker =
              (metadata_stream_tracker_t *)req->userdata;
          if (!processed[tracker->index]) {
            processed[tracker->index] = true;
            if (req->has_error || req->status_code != 200) {
              callback(tracker->name, NULL, 0, true, user_data);
            } else {
              size_t dec_len = 0;
              bool compressed = false;
              uint8_t *dec_data = decode_metadata(req, &dec_len, &compressed);
              if (dec_data) {
                callback(tracker->name, dec_data, dec_len, false, user_data);
                free(dec_data);
              } else {
                callback(tracker->name, NULL, 0, true, user_data);
              }
            }
          }
        }
      }
    }
  }
}

fetch_error_t fetcher_fetch_metadata_streaming(fetcher_t *self,
                                               const char *const *names,
                                               size_t count,
                                               fetch_metadata_cb callback,
                                               void *user_data) {
  if (count == 0)
    return FETCH_OK;
  fetch_error_t err = fetcher_ensure_meta_clients(self);
  if (err != FETCH_OK)
    return err;

  size_t active_connections = 0;
  for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
    if (self->meta_clients[i])
      active_connections++;
  }
  if (active_connections == 0)
    return FETCH_CONNECTION_FAILED;

  bool *processed = calloc(count, sizeof(bool));
  if (!processed)
    return FETCH_OUT_OF_MEMORY;

  metadata_stream_tracker_t *trackers =
      malloc(count * sizeof(metadata_stream_tracker_t));
  if (!trackers) {
    free(processed);
    return FETCH_OUT_OF_MEMORY;
  }

  for (size_t i = 0; i < count; i++) {
    trackers[i].name = names[i];
    trackers[i].index = i;
  }

  size_t total_capacity = active_connections * (MAX_PENDING_REQUESTS - 1);
  size_t offset = 0;

  while (offset < count) {
    size_t end =
        (offset + total_capacity < count) ? (offset + total_capacity) : count;
    size_t conn_idx = 0;

    for (size_t i = offset; i < end; i++) {
      metadata_stream_tracker_t *tracker = &trackers[i];
      http2_client_t *c = fetcher_next_meta_client(self, &conn_idx);
      if (!c || !c->h2_session) {
        processed[tracker->index] = true;
        callback(tracker->name, NULL, 0, true, user_data);
        continue;
      }

      char path_buf[512];
      snprintf(path_buf, sizeof(path_buf), "/%s", tracker->name);

      request_state_t *req = &c->requests[c->request_count++];
      init_request_state(req);
      req->path = strdup(path_buf);
      req->userdata = tracker;
      req->start_ns = get_time_ns();

      char ua_buf[128];
      snprintf(ua_buf, sizeof(ua_buf), "ant/%s", ant_semver());

      nghttp2_nv hdrs[] = {
          make_nv(":method", "GET"),
          make_nv(":path", req->path),
          make_nv(":scheme", "https"),
          make_nv(":authority", c->host),
          make_nv("accept", "application/vnd.npm.install-v1+json"),
          make_nv("accept-encoding", "gzip"),
          make_nv("user-agent", ua_buf)};

      int32_t sid = nghttp2_submit_request(
          c->h2_session, NULL, hdrs, sizeof(hdrs) / sizeof(hdrs[0]), NULL, req);
      if (sid < 0) {
        c->request_count--;
        deinit_request_state(req);
        processed[tracker->index] = true;
        callback(tracker->name, NULL, 0, true, user_data);
        continue;
      }
      req->stream_id = sid;
      conn_idx = (conn_idx + 1) % NUM_META_CONNECTIONS;
    }

    for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
      if (self->meta_clients[i])
        client_flush(self->meta_clients[i]);
    }

    while (!fetcher_meta_requests_complete(self)) {
      uv_run(uv_default_loop(), UV_RUN_ONCE);
      emit_completed_streaming_callbacks(self, processed, callback, user_data);
    }

    emit_completed_streaming_callbacks(self, processed, callback, user_data);

    for (size_t i = 0; i < NUM_META_CONNECTIONS; i++) {
      if (self->meta_clients[i]) {
        client_reset_requests(self->meta_clients[i]);
      }
    }

    offset = end;
  }

  free(trackers);
  free(processed);
  return FETCH_OK;
}
