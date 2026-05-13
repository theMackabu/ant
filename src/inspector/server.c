#include "bind.h"
#include "internal.h"
#include "inspector.h"
#include "http/websocket.h"
#include "modules/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void inspector_process_ws(inspector_client_t *client) {
  for (;;) {
    ant_ws_frame_t frame = {0};
    ant_ws_frame_result_t r = ant_ws_parse_frame(
      (const uint8_t *)client->read_buf,
      client->read_len,
      true,
      &frame
    );
    if (r == ANT_WS_FRAME_INCOMPLETE) return;
    if (r == ANT_WS_FRAME_PROTOCOL_ERROR) {
      uv_close((uv_handle_t *)&client->handle, NULL);
      return;
    }
    if (frame.opcode == ANT_WS_OPCODE_TEXT) {
      inspector_handle_message(client, (const char *)frame.payload, frame.payload_len);
    } else if (frame.opcode == ANT_WS_OPCODE_CLOSE) {
      ant_ws_frame_clear(&frame);
      uv_close((uv_handle_t *)&client->handle, NULL);
      return;
    } else if (frame.opcode == ANT_WS_OPCODE_PING) {
      size_t len = 0;
      uint8_t *pong = ant_ws_encode_frame(ANT_WS_OPCODE_PONG, frame.payload, frame.payload_len, false, &len);
      if (pong) {
        inspector_send_raw(client, (const char *)pong, len);
        free(pong);
      }
    }
    size_t consumed = frame.consumed_len;
    ant_ws_frame_clear(&frame);
    if (consumed >= client->read_len) {
      client->read_len = 0;
      return;
    }
    memmove(client->read_buf, client->read_buf + consumed, client->read_len - consumed);
    client->read_len -= consumed;
  }
}

static const char *header_value(const char *headers, const char *name, char *buf, size_t bufsz) {
  size_t name_len = strlen(name);
  const char *p = headers;
  while ((p = strstr(p, "\n")) != NULL) {
    p++;
    while (*p == '\r' || *p == '\n') p++;
    if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
      p += name_len + 1;
      while (*p == ' ' || *p == '\t') p++;
      const char *end = p;
      while (*end && *end != '\r' && *end != '\n') end++;
      size_t len = (size_t)(end - p);
      if (len >= bufsz) len = bufsz - 1;
      memcpy(buf, p, len);
      buf[len] = '\0';
      return buf;
    }
  }
  return NULL;
}

static bool host_allowed(const char *headers) {
  char host[256];
  const char *value = header_value(headers, "Host", host, sizeof(host));
  if (!value) return true;
  if (strncmp(value, "127.0.0.1", 9) == 0) return true;
  if (strncmp(value, "localhost", 9) == 0) return true;
  if (strncmp(value, "[::1]", 5) == 0) return true;
  return false;
}

static bool header_token_eq(const char *headers, const char *name, const char *expected) {
  char value[256];
  const char *p = header_value(headers, name, value, sizeof(value));
  if (!p) return false;
  size_t expected_len = strlen(expected);
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    const char *start = p;
    while (*p && *p != ',') p++;
    const char *end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if ((size_t)(end - start) == expected_len && strncasecmp(start, expected, expected_len) == 0)
      return true;
  }
  return false;
}

static bool request_path_matches_uuid(const char *req) {
  char expected[80];
  snprintf(expected, sizeof(expected), "GET /%s ", g_inspector.uuid);
  return strncmp(req, expected, strlen(expected)) == 0;
}

static bool inspector_append_target_url(sbuf_t *b, const char *file) {
  if (!file || !*file || strcmp(file, "[repl]") == 0 || strcmp(file, "ant") == 0)
    return sbuf_json_string(b, "file:///");
  if (inspector_is_url_like(file)) return sbuf_json_string(b, file);
  if (file[0] == '/') {
    if (!sbuf_append(b, "\"file://")) return false;
    if (!sbuf_append(b, file)) return false;
    return sbuf_append(b, "\"");
  }
  if (!sbuf_append(b, "\"file:///")) return false;
  if (!sbuf_append(b, file)) return false;
  return sbuf_append(b, "\"");
}

static bool inspector_append_devtools_url(sbuf_t *b) {
  return 
    sbuf_append(b, "devtools://devtools/bundled/js_app.html?v8only=true&ws=") &&
    sbuf_append(b, g_inspector.host) &&
    sbuf_appendf(b, ":%d/%s", g_inspector.port, g_inspector.uuid);
}

static bool inspector_append_browser_devtools_url(sbuf_t *b) {
  return 
    sbuf_append(b, "http://") &&
    sbuf_append(b, g_inspector.host) &&
    sbuf_appendf(b, ":%d/devtools", g_inspector.port);
}

static char *json_list_response(void) {
  sbuf_t b = {0};
  inspector_script_t *entry = inspector_entry_script();
  const char *file = entry && entry->url
    ? entry->url
    : (g_inspector.js && g_inspector.js->filename ? g_inspector.js->filename : "ant");
  uv_pid_t pid = uv_os_getpid();
  if (!sbuf_append(&b, "[")) goto fail;
  if (!sbuf_append(&b, "{\"description\":\"ant[")) goto fail;
  if (!sbuf_appendf(&b, "%d", (int)pid)) goto fail;
  if (!sbuf_append(&b, "]\",\"devtoolsFrontendUrl\":\"")) goto fail;
  if (!inspector_append_devtools_url(&b)) goto fail;
  if (!sbuf_append(&b, "\",\"id\":\"")) goto fail;
  if (!sbuf_append(&b, g_inspector.uuid)) goto fail;
  if (!sbuf_append(&b, "\",\"title\":\"ant[")) goto fail;
  if (!sbuf_appendf(&b, "%d", (int)pid)) goto fail;
  if (!sbuf_append(&b, "]\"")) goto fail;
  if (!sbuf_append(&b, ",\"type\":\"node\",\"url\":")) goto fail;
  if (!inspector_append_target_url(&b, file)) goto fail;
  if (!sbuf_append(&b, ",\"webSocketDebuggerUrl\":\"ws://")) goto fail;
  if (!sbuf_append(&b, g_inspector.host)) goto fail;
  if (!sbuf_appendf(&b, ":%d/%s\"}]", g_inspector.port, g_inspector.uuid)) goto fail;
  return b.data;
fail:
  free(b.data);
  return NULL;
}

static void inspector_http_response(inspector_client_t *client, const char *status, const char *type, const char *body) {
  sbuf_t b = {0};
  size_t body_len = body ? strlen(body) : 0;
  if (sbuf_appendf(
    &b,
    "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
    status, type ? type : "text/plain", body_len
  ) && sbuf_append_len(&b, body, body_len)) {
    inspector_send_raw(client, b.data, b.len);
  }
  free(b.data);
  uv_close((uv_handle_t *)&client->handle, NULL);
}

static void inspector_http_devtools_page(inspector_client_t *client) {
  sbuf_t devtools = {0};
  sbuf_t body = {0};
  if (
    inspector_append_devtools_url(&devtools) &&
    sbuf_append(&body, "<!doctype html><meta charset=\"utf-8\"><title>Ant DevTools</title>") &&
    sbuf_append(&body, "<style>body{font:84%/1.4 'Segoe UI',Tahoma,sans-serif;color:#000;background:#fff;margin:0;padding:24px;max-width:800px}h1{font-size:22px;font-weight:normal;margin:0 0 4px}.subtitle{color:#5f6368;font-size:13px;margin-bottom:24px}table{border-collapse:collapse;width:100%}td{padding:6px 14px 6px 0;vertical-align:top;font-size:13px}td.label{text-align:right;color:#5f6368;white-space:nowrap;width:1%}td.value{font-family:ui-monospace,Menlo,Consolas,monospace;word-break:break-all}@media (prefers-color-scheme:dark){body{background:#202124;color:#e8eaed}.subtitle,td.label{color:#9aa0a6}}</style>") &&
    sbuf_append(&body, "<h1>Ant DevTools</h1><div class=\"subtitle\">Paste into Chrome's address bar.</div><table><tr><td class=\"label\">DevTools frontend</td><td class=\"value\">") &&
    sbuf_append(&body, devtools.data) &&
    sbuf_append(&body, "</td></tr></table>")
  ) inspector_http_response(client, "200 OK", "text/html; charset=UTF-8", body.data);
  else inspector_http_response(client, "500 Internal Server Error", "text/plain", "Out of memory\n");
  free(devtools.data);
  free(body.data);
}

static void inspector_upgrade(inspector_client_t *client, const char *headers) {
  char key[256];
  if (!host_allowed(headers) || !request_path_matches_uuid(headers)) {
    inspector_http_response(client, "403 Forbidden", "text/plain", "Forbidden\n");
    return;
  }
  if (!header_value(headers, "Sec-WebSocket-Key", key, sizeof(key))) {
    inspector_http_response(client, "400 Bad Request", "text/plain", "Missing WebSocket key\n");
    return;
  }
  char *accept = ant_ws_accept_key(key);
  if (!accept) {
    inspector_http_response(client, "400 Bad Request", "text/plain", "Invalid WebSocket key\n");
    return;
  }
  sbuf_t b = {0};
  if (sbuf_appendf(
    &b,
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
    accept
  )) inspector_send_raw(client, b.data, b.len);
  free(accept);
  free(b.data);
  client->websocket = true;
  g_inspector.attached = true;
  client->read_len = 0;
}

static void inspector_process_http(inspector_client_t *client) {
  char *end = strstr(client->read_buf, "\r\n\r\n");
  if (!end) return;
  if (!host_allowed(client->read_buf)) {
    inspector_http_response(client, "403 Forbidden", "text/plain", "Forbidden\n");
    return;
  }
  if (strncmp(client->read_buf, "GET /json/list ", 15) == 0 ||
      strncmp(client->read_buf, "GET /json ", 10) == 0) {
    char *body = json_list_response();
    inspector_http_response(client, "200 OK", "application/json; charset=UTF-8", body ? body : "[]");
    free(body);
  } else if (strncmp(client->read_buf, "GET /devtools ", 14) == 0 ||
             strncmp(client->read_buf, "GET /devtools/ ", 15) == 0) {
    inspector_http_devtools_page(client);
  } else if (strncmp(client->read_buf, "GET /json/version ", 18) == 0) {
    inspector_http_response(
      client,
      "200 OK",
      "application/json; charset=UTF-8",
      "{\"Browser\":\"Ant/v" ANT_VERSION "\",\"Protocol-Version\":\"1.3\",\"V8-Version\":\"ant\",\"WebKit-Version\":\"ant\"}"
    );
  } else if (
    strncasecmp(client->read_buf, "GET /", 5) == 0 &&
    header_token_eq(client->read_buf, "Upgrade", "websocket") &&
    header_token_eq(client->read_buf, "Connection", "upgrade")
  ) inspector_upgrade(client, client->read_buf);
  else inspector_http_response(client, "404 Not Found", "text/plain", "Not found\n");
}

static void inspector_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void inspector_client_closed(uv_handle_t *handle) {
  inspector_client_t *client = (inspector_client_t *)handle;
  inspector_client_t **p = &g_inspector.clients;
  while (*p) {
    if (*p == client) {
      *p = client->next;
      break;
    }
    p = &(*p)->next;
  }
  free(client->read_buf);
  free(client);
}

static void inspector_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  inspector_client_t *client = (inspector_client_t *)stream;
  if (nread <= 0) {
    free(buf->base);
    if (nread < 0) uv_close((uv_handle_t *)&client->handle, inspector_client_closed);
    return;
  }
  if (client->read_len + (size_t)nread + 1 > client->read_cap) {
    size_t next_cap = client->read_cap ? client->read_cap * 2 : 8192;
    while (next_cap < client->read_len + (size_t)nread + 1) next_cap *= 2;
    char *next = realloc(client->read_buf, next_cap);
    if (!next) {
      free(buf->base);
      uv_close((uv_handle_t *)&client->handle, inspector_client_closed);
      return;
    }
    client->read_buf = next;
    client->read_cap = next_cap;
  }
  memcpy(client->read_buf + client->read_len, buf->base, (size_t)nread);
  client->read_len += (size_t)nread;
  client->read_buf[client->read_len] = '\0';
  free(buf->base);

  if (client->websocket) inspector_process_ws(client);
  else inspector_process_http(client);
}

static void inspector_connection_cb(uv_stream_t *server, int status) {
  if (status < 0) return;
  inspector_client_t *client = calloc(1, sizeof(*client));
  if (!client) return;
  client->js = g_inspector.js;
  uv_tcp_init(uv_default_loop(), &client->handle);
  client->handle.data = client;
  if (uv_accept(server, (uv_stream_t *)&client->handle) != 0) {
    uv_close((uv_handle_t *)&client->handle, inspector_client_closed);
    return;
  }
  client->next = g_inspector.clients;
  g_inspector.clients = client;
  uv_read_start((uv_stream_t *)&client->handle, inspector_alloc_cb, inspector_read_cb);
}

static bool inspector_parse_sockaddr(const char *host, int port, struct sockaddr_in *addr) {
  const char *bind_host = host && *host ? host : "127.0.0.1";
  if (uv_ip4_addr(bind_host, port, addr) == 0) return true;
  if (strcmp(bind_host, "localhost") == 0 && uv_ip4_addr("127.0.0.1", port, addr) == 0) return true;
  return false;
}

bool ant_inspector_start(ant_t *js, const ant_inspector_options_t *options) {
  if (!options || !options->enabled || g_inspector.started) return true;
  memset(&g_inspector, 0, sizeof(g_inspector));
  g_inspector.js = js;
  g_inspector.port = options->port > 0 ? options->port : 9229;
  snprintf(g_inspector.host, sizeof(g_inspector.host), "%s", options->host[0] ? options->host : "127.0.0.1");
  if (crypto_random_uuid(g_inspector.uuid) < 0) return false;

  struct sockaddr_in addr;
  if (!inspector_parse_sockaddr(g_inspector.host, g_inspector.port, &addr)) return false;
  if (uv_tcp_init(uv_default_loop(), &g_inspector.server) != 0) return false;
  if (uv_tcp_bind(&g_inspector.server, (const struct sockaddr *)&addr, 0) != 0) return false;
  if (uv_listen((uv_stream_t *)&g_inspector.server, 16, inspector_connection_cb) != 0) return false;
  if (!options->wait_for_session) uv_unref((uv_handle_t *)&g_inspector.server);
  g_inspector.waiting_for_debugger = options->wait_for_session;
  g_inspector.started = true;

  sbuf_t devtools = {0};
  if (inspector_append_browser_devtools_url(&devtools))
    fprintf(stderr, "Debugger listening on %s\n", devtools.data);
  free(devtools.data);
  return true;
}

void ant_inspector_stop(void) {
  if (!g_inspector.started) return;
  for (inspector_client_t *c = g_inspector.clients; c; c = c->next)
    uv_close((uv_handle_t *)&c->handle, inspector_client_closed);
  uv_close((uv_handle_t *)&g_inspector.server, NULL);
  inspector_clear_console_events();
  while (g_inspector.scripts) {
    inspector_script_t *script = g_inspector.scripts;
    g_inspector.scripts = script->next;
    free(script->url);
    free(script->source);
    free(script);
  }
  while (g_inspector.network_entries) {
    inspector_network_entry_t *entry = g_inspector.network_entries;
    g_inspector.network_entries = entry->next;
    free(entry->request_body);
    free(entry->response_body);
    free(entry);
  }
  g_inspector.started = false;
}

void ant_inspector_wait_for_session(void) {
  while (g_inspector.started && (!g_inspector.attached || g_inspector.waiting_for_debugger))
    uv_run(uv_default_loop(), UV_RUN_ONCE);
  if (g_inspector.started) uv_unref((uv_handle_t *)&g_inspector.server);
}
