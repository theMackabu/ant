#include "ascii.h"
#include "bind.h"
#include "isolate.h"
#include "inspector.h"
#include "http/http1_parser.h"
#include "http/websocket.h"
#include "modules/crypto.h"
#include "reactor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void inspector_process_ws(inspector_client_t *client) {
  ant_conn_t *conn = client->conn;
  
  for (;;) {
    ant_ws_frame_t frame = {0};
    ant_ws_frame_result_t r = ant_ws_parse_frame(
      (uint8_t *)ant_conn_buffer(conn), ant_conn_buffer_len(conn),
      true, false, false, SIZE_MAX, &frame
    );
    
    if (r == ANT_WS_FRAME_INCOMPLETE) return;
    if (r != ANT_WS_FRAME_OK || frame.opcode == ANT_WS_OPCODE_CLOSE) {
      ant_ws_frame_clear(&frame);
      ant_conn_close(conn);
      return;
    }
    
    if (frame.opcode == ANT_WS_OPCODE_TEXT) inspector_handle_message(client, (const char *)frame.payload, frame.payload_len);
    else if (frame.opcode == ANT_WS_OPCODE_PING) {
      size_t len = 0;
      uint8_t *pong = ant_ws_encode_frame(ANT_WS_OPCODE_PONG, frame.payload, frame.payload_len, false, false, &len);
      if (pong) ant_conn_write(conn, (char *)pong, len, NULL, NULL);
    }
    
    size_t consumed = frame.consumed_len;
    ant_ws_frame_clear(&frame);
    ant_conn_consume(conn, consumed);
  }
}

static bool host_allowed(const char *value) {
  char host[256];
  unsigned char addr[16];
  if (!value) return true;

  size_t len = strlen(value);
  if (len >= sizeof(host)) return false;
  memcpy(host, value, len + 1);

  char *colon = strrchr(host, ':');
  char *bracket = strrchr(host, ']');
  
  if (colon && (!bracket || colon > bracket)) {
    if (!colon[1]) return false;
    for (const char *p = colon + 1; *p; p++) if (!ant_ascii_is_digit((unsigned char)*p)) return false;
    *colon = '\0';
  }

  len = strlen(host);
  if (strcasecmp(host, "localhost") == 0) return true;
  if (uv_inet_pton(AF_INET, host, addr) == 0) return true;
  if (len < 4 || host[0] != '[' || host[len - 1] != ']') return false;
  
  host[len - 1] = '\0';
  return uv_inet_pton(AF_INET6, host + 1, addr) == 0;
}

static char *inspector_target_url(const char *file) {
  if (!file || !*file || strcmp(file, "[repl]") == 0 || strcmp(file, "ant") == 0) return strdup("file:///");
  if (inspector_is_url_like(file)) return strdup(file);
  return inspector_concat(file[0] == '/' ? "file://" : "file:///", file);
}

static char *json_list_response(void) {
  inspector_script_t *entry = inspector_entry_script();
  const char *file = entry && entry->url
    ? entry->url
    : (g_inspector.js && g_inspector.js->filename ? g_inspector.js->filename : "ant");

  char *target = inspector_target_url(file);
  if (!target) return NULL;

  char title[64];
  char devtools[192];
  char websocket[128];
  snprintf(title, sizeof(title), "ant[%d]", (int)uv_os_getpid());
  snprintf(websocket, sizeof(websocket), "ws://%s:%d/%s", g_inspector.host, g_inspector.port, g_inspector.uuid);
  snprintf(devtools, sizeof(devtools), "devtools://devtools/bundled/js_app.html?v8only=true&ws=%s", websocket + strlen("ws://"));

  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  yyjson_mut_val *list = yyjson_mut_arr(doc);
  yyjson_mut_val *item = inspector_arr_add_obj(doc, list);
  inspector_add_str(doc, item, "description", title);
  inspector_add_str(doc, item, "devtoolsFrontendUrl", devtools);
  inspector_add_str(doc, item, "id", g_inspector.uuid);
  inspector_add_str(doc, item, "title", title);
  inspector_add_str(doc, item, "type", "node");
  inspector_add_str(doc, item, "url", target);
  inspector_add_str(doc, item, "webSocketDebuggerUrl", websocket);

  char *json = inspector_doc_finish(&d, list);
  free(target);
  return json;
}

static void inspector_http_response(inspector_client_t *client, const char *status, const char *type, const char *body) {
  char head[256];
  size_t body_len = body ? strlen(body) : 0;
  
  int head_len = snprintf(
    head, sizeof(head),
    "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
    status, type ? type : "text/plain", body_len
  );

  char *out = malloc((size_t)head_len + body_len);
  if (out) {
    memcpy(out, head, (size_t)head_len);
    if (body_len) memcpy(out + head_len, body, body_len);
    ant_conn_write(client->conn, out, (size_t)head_len + body_len, NULL, NULL);
  }
  
  ant_conn_close(client->conn);
}

static void inspector_upgrade(inspector_client_t *client, const ant_http1_parsed_request_t *req) {
  const char *key = NULL;
  if (req->target[0] != '/' || strcmp(req->target + 1, g_inspector.uuid) != 0) {
    inspector_http_response(client, "403 Forbidden", "text/plain", "Forbidden\n");
    return;
  }

  char *accept = ant_ws_validate_client_handshake(req->headers, &key) ? ant_ws_accept_key(key) : NULL;
  if (!accept) {
    inspector_http_response(client, "400 Bad Request", "text/plain", "Invalid WebSocket handshake\n");
    return;
  }

  char response[192];
  int len = snprintf(
    response, sizeof(response),
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
    accept
  );
  
  ant_conn_write_copy(client->conn, response, (size_t)len, NULL, NULL);
  free(accept);
  
  client->websocket = true;
  g_inspector.attached = true;
  
  ant_conn_consume(client->conn, ant_conn_buffer_len(client->conn));
}

static void inspector_process_http(inspector_client_t *client) {
  ant_http1_parsed_request_t req;
  
  ant_http1_parse_result_t parsed = ant_http1_parse_request(
    ant_conn_buffer(client->conn), 
    ant_conn_buffer_len(client->conn), 0, &req, NULL, NULL
  );
  
  if (parsed == ANT_HTTP1_PARSE_INCOMPLETE) return;
  
  if (parsed == ANT_HTTP1_PARSE_ERROR) {
    inspector_http_response(client, "400 Bad Request", "text/plain", "Bad request\n");
    return;
  }

  const char *path = strcmp(req.method, "GET") == 0 ? req.target : "";
  if (!host_allowed(ant_ws_find_header(req.headers, "host"))) {
    inspector_http_response(client, "403 Forbidden", "text/plain", "Forbidden\n");
  } else if (strcmp(path, "/json/list") == 0 || strcmp(path, "/json") == 0) {
    char *body = json_list_response();
    inspector_http_response(client, "200 OK", "application/json; charset=UTF-8", body ? body : "[]");
    free(body);
  } else if (strcmp(path, "/json/version") == 0) inspector_http_response(
    client, "200 OK",
    "application/json; charset=UTF-8",
    "{\"Browser\":\"Ant/v" ANT_VERSION "\",\"Protocol-Version\":\"1.3\",\"V8-Version\":\"ant\",\"WebKit-Version\":\"ant\"}"
  );
  
  else if (*path && ant_ws_find_header(req.headers, "upgrade")) inspector_upgrade(client, &req);
  else inspector_http_response(client, "404 Not Found", "text/plain", "Not found\n");

  ant_http1_free_parsed_request(&req);
}

static void inspector_on_accept(ant_listener_t *listener, ant_conn_t *conn, void *user_data) {
  inspector_client_t *client = calloc(1, sizeof(*client));
  if (!client) return;
  
  client->conn = conn;
  client->js = g_inspector.js;
  client->id = ++g_inspector.next_client_id;
  
  if (client->id == 0) client->id = ++g_inspector.next_client_id;
  client->next = g_inspector.clients;
  g_inspector.clients = client;
  
  ant_conn_set_user_data(conn, client);
}

static void inspector_on_read(ant_conn_t *conn, ssize_t nread, void *user_data) {
  inspector_client_t *client = ant_conn_get_user_data(conn);
  if (!client) ant_conn_close(conn);
  else if (client->websocket) inspector_process_ws(client);
  else inspector_process_http(client);
}

static void inspector_on_end(ant_conn_t *conn, void *user_data) {
  ant_conn_close(conn);
}

static void inspector_on_conn_close(ant_conn_t *conn, void *user_data) {
  inspector_client_t *client = ant_conn_get_user_data(conn);
  if (!client) return;
  inspector_cancel_client_awaits(client->id);

  inspector_client_t **p = &g_inspector.clients;
  while (*p && *p != client) p = &(*p)->next;
  
  if (*p) *p = client->next;
  free(client);
}

bool ant_inspector_start(ant_t *js, const ant_inspector_options_t *options) {
  if (!options || !options->enabled || g_inspector.started) return true;
  memset(&g_inspector, 0, sizeof(g_inspector));
  
  g_inspector.js = js;
  g_inspector.port = options->port > 0 ? options->port : 9229;
  
  snprintf(g_inspector.host, sizeof(g_inspector.host), "%s", options->host[0] ? options->host : "127.0.0.1");
  if (crypto_random_uuid(g_inspector.uuid) < 0) return false;

  static const ant_listener_callbacks_t callbacks = {
    .on_accept = inspector_on_accept,
    .on_read = inspector_on_read,
    .on_end = inspector_on_end,
    .on_conn_close = inspector_on_conn_close,
  };
  
  const char *bind_host = strcmp(g_inspector.host, "localhost") == 0 ? "127.0.0.1" : g_inspector.host;
  
  if (ant_listener_listen_tcp(
    &g_inspector.listener, js->loop, 
    bind_host, g_inspector.port, 16, 0, &callbacks, NULL
  ) != 0) return false;
  
  if (!options->wait_for_session) ant_listener_unref(&g_inspector.listener);
  
  g_inspector.waiting_for_debugger = options->wait_for_session;
  g_inspector.started = true;

  fprintf(stderr, "Debugger listening on ws://%s:%d/%s\n", g_inspector.host, g_inspector.port, g_inspector.uuid);
  return true;
}

void ant_inspector_stop(void) {
  if (!g_inspector.started) return;
  
  ant_listener_stop(&g_inspector.listener, true);
  inspector_clear_console_events();
  inspector_clear_pending_awaits();
  
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
  
  g_inspector.network_entry_count = 0;
  g_inspector.started = false;
}

void ant_inspector_wait_for_session(void) {
  while (g_inspector.started && (!g_inspector.attached || g_inspector.waiting_for_debugger)) {
    ant_uv_run(g_inspector.js->loop, UV_RUN_ONCE);
    js_poll_events(g_inspector.js);
  }
  
  if (g_inspector.started) ant_listener_unref(&g_inspector.listener);
}
