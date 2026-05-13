#include "bind.h"
#include "base64.h"
#include "inspector.h"
#include "json.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void inspector_network_entry_free(inspector_network_entry_t *entry) {
  if (!entry) return;
  free(entry->request_body);
  free(entry->response_body);
  free(entry);
}

static void inspector_network_prune_entries(void) {
  if (g_inspector.network_entry_count <= k_inspector_network_entry_limit) return;

  size_t target = (k_inspector_network_entry_limit * 3) / 4;
  if (target == 0) target = k_inspector_network_entry_limit;

  inspector_network_entry_t *keep_tail = g_inspector.network_entries;
  for (size_t i = 1; keep_tail && i < target; i++) keep_tail = keep_tail->next;

  if (!keep_tail) {
    g_inspector.network_entry_count = 0;
    return;
  }

  inspector_network_entry_t *evict = keep_tail->next;
  keep_tail->next = NULL;
  while (evict) {
    inspector_network_entry_t *next = evict->next;
    inspector_network_entry_free(evict);
    evict = next;
  }
  g_inspector.network_entry_count = target;
}

static bool inspector_network_client_ready(const inspector_client_t *client) {
  return client &&
         client->network_enabled &&
         uv_stream_get_write_queue_size((const uv_stream_t *)&client->handle) <= k_inspector_network_write_queue_limit;
}

static bool inspector_network_has_ready_client(void) {
  for (const inspector_client_t *c = g_inspector.clients; c; c = c->next) {
    if (inspector_network_client_ready(c)) return true;
  }
  return false;
}

static inspector_network_entry_t *inspector_network_entry_for_id(uint64_t id, bool create) {
  if (id == 0) return NULL;
  for (inspector_network_entry_t *entry = g_inspector.network_entries; entry; entry = entry->next) {
    if (entry->id == id) return entry;
  }
  if (!create) return NULL;

  inspector_network_entry_t *entry = calloc(1, sizeof(*entry));
  if (!entry) return NULL;
  entry->id = id;
  entry->next = g_inspector.network_entries;
  g_inspector.network_entries = entry;
  g_inspector.network_entry_count++;
  inspector_network_prune_entries();
  return entry;
}

static void inspector_network_store_body(
  uint8_t **body,
  size_t *body_len,
  bool *truncated,
  const uint8_t *data,
  size_t len,
  bool append
) {
  if (!body || !body_len || !truncated || !data || len == 0 || *truncated) return;

  size_t keep = len;
  if (append) {
    if (*body_len >= k_inspector_network_body_limit) {
      *truncated = true;
      return;
    }
    if (keep > k_inspector_network_body_limit - *body_len) {
      keep = k_inspector_network_body_limit - *body_len;
      *truncated = true;
    }
  } else if (keep > k_inspector_network_body_limit) {
    keep = k_inspector_network_body_limit;
    *truncated = true;
  }

  if (append) {
    uint8_t *next = realloc(*body, *body_len + keep);
    if (!next) {
      *truncated = true;
      return;
    }
    memcpy(next + *body_len, data, keep);
    *body = next;
    *body_len += keep;
  } else {
    uint8_t *next = malloc(keep);
    if (!next) {
      *truncated = true;
      return;
    }
    memcpy(next, data, keep);
    free(*body);
    *body = next;
    *body_len = keep;
  }
}

static void inspector_notify_network(const char *json) {
  for (inspector_client_t *c = g_inspector.clients; c; c = c->next) {
    if (!inspector_network_client_ready(c)) continue;
    inspector_send_ws(c, json);
  }
}

static bool inspector_append_headers_object(sbuf_t *b, const ant_http_header_t *headers) {
  if (!sbuf_append(b, "{")) return false;
  bool first = true;
  for (const ant_http_header_t *header = headers; header; header = header->next) {
    if (!header->name || !header->value) continue;
    if (!first && !sbuf_append(b, ",")) return false;
    first = false;
    if (!sbuf_json_string(b, header->name)) return false;
    if (!sbuf_append(b, ":")) return false;
    if (!sbuf_json_string(b, header->value)) return false;
  }
  return sbuf_append(b, "}");
}

static void inspector_notify_request_extra(uint64_t request_id, const ant_http_header_t *headers) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.requestWillBeSentExtraInfo\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"associatedCookies\":[],\"headers\":")) goto done;
  if (!inspector_append_headers_object(&b, headers)) goto done;
  if (!sbuf_append(&b, ",\"connectTiming\":{\"requestTime\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, "},\"siteHasCookieInOtherPartition\":false}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
}

static void inspector_notify_response_extra(uint64_t request_id, int status, const ant_http_header_t *headers) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.responseReceivedExtraInfo\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"blockedCookies\":[],\"headers\":")) goto done;
  if (!inspector_append_headers_object(&b, headers)) goto done;
  if (!sbuf_append(&b, ",\"resourceIPAddressSpace\":\"Unknown\",\"statusCode\":")) goto done;
  if (!sbuf_appendf(&b, "%d", status)) goto done;
  if (!sbuf_append(&b, "}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
}

static bool inspector_append_network_initiator(sbuf_t *b, const char *label) {
  const char *name = label && *label ? label : "Fetch";
  const char *url = "Fetch";
  if (strcmp(name, "Server") == 0) url = "Server";
  else if (strcmp(name, "EventSource") == 0) url = "EventSource";

  if (!sbuf_append(b, "{\"type\":\"parser\",\"url\":")) return false;
  if (!sbuf_json_string(b, url)) return false;
  return sbuf_append(b, "}");
}

static uint64_t inspector_request_id_param(yyjson_val *params) {
  yyjson_val *request_id = params ? yyjson_obj_get(params, "requestId") : NULL;
  if (!request_id || !yyjson_is_str(request_id)) return 0;
  const char *s = yyjson_get_str(request_id);
  if (!s || !*s) return 0;
  return strtoull(s, NULL, 10);
}

void inspector_get_response_body(inspector_client_t *client, int id, yyjson_val *params) {
  uint64_t request_id = inspector_request_id_param(params);
  inspector_network_entry_t *entry = inspector_network_entry_for_id(request_id, false);
  sbuf_t b = {0};
  inspector_json_t json;
  size_t out_len = 0;
  char *encoded = NULL;
  bool ok = false;

  if (!entry || !entry->response_body || entry->response_body_len == 0) out_len = 0; else {
    encoded = ant_base64_encode(entry->response_body, entry->response_body_len, &out_len);
    if (!encoded) { inspector_send_error(client, id, -32000, "Out of memory"); return; }
  }

  inspector_json_init(&json, &b);
  if (!inspector_json_begin_object(&json)) goto done;
  if (!inspector_json_key(&json, "body")) goto done;
  if (!inspector_json_string_len(&json, encoded ? encoded : "", out_len)) goto done;
  if (!inspector_json_key(&json, "base64Encoded")) goto done;
  if (!inspector_json_bool(&json, encoded != NULL)) goto done;
  if (!inspector_json_end_object(&json)) goto done;
  ok = true;

done:
  if (ok) inspector_send_response_obj(client, id, b.data);
  else inspector_send_error(client, id, -32000, "Out of memory");
  free(encoded);
  free(b.data);
}

void inspector_get_request_post_data(inspector_client_t *client, int id, yyjson_val *params) {
  uint64_t request_id = inspector_request_id_param(params);
  inspector_network_entry_t *entry = inspector_network_entry_for_id(request_id, false);
  sbuf_t b = {0};
  inspector_json_t json;
  inspector_json_init(&json, &b);
  const char *body = entry && entry->request_body ? (const char *)entry->request_body : "";
  size_t body_len = entry && entry->request_body ? entry->request_body_len : 0;
  bool ok =
    inspector_json_begin_object(&json) &&
    inspector_json_key(&json, "postData") &&
    inspector_json_string_len(&json, body, body_len) &&
    inspector_json_end_object(&json);

  if (ok) inspector_send_response_obj(client, id, b.data);
  else inspector_send_error(client, id, -32000, "Out of memory");
  free(b.data);
}
uint64_t ant_inspector_network_request(
  const char *method,
  const char *url,
  const char *type,
  const char *initiator,
  bool has_post_data,
  const ant_http_header_t *headers
) {
  if (!g_inspector.started || !g_inspector.attached || !inspector_network_has_ready_client()) return 0;
  uint64_t request_id = ++g_inspector.next_network_request_id;
  if (request_id == 0) request_id = ++g_inspector.next_network_request_id;
  inspector_network_entry_for_id(request_id, true);

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.requestWillBeSent\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"loaderId\":\"ant\",\"documentURL\":")) goto done;
  if (!sbuf_json_string(&b, url ? url : "")) goto done;
  if (!sbuf_append(&b, ",\"request\":{\"url\":")) goto done;
  if (!sbuf_json_string(&b, url ? url : "")) goto done;
  if (!sbuf_append(&b, ",\"method\":")) goto done;
  if (!sbuf_json_string(&b, method ? method : "GET")) goto done;
  if (!sbuf_append(&b, ",\"headers\":")) goto done;
  if (!inspector_append_headers_object(&b, headers)) goto done;
  if (has_post_data) {
    if (!sbuf_append(&b, ",\"hasPostData\":true")) goto done;
  }
  if (!sbuf_append(&b, ",\"initialPriority\":\"High\",\"referrerPolicy\":\"no-referrer\"},\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, ",\"wallTime\":")) goto done;
  if (!sbuf_appendf(&b, "%.3f", inspector_wall_time_seconds())) goto done;
  if (!sbuf_append(&b, ",\"initiator\":")) goto done;
  if (!inspector_append_network_initiator(&b, initiator)) goto done;
  if (!sbuf_append(&b, ",\"type\":")) goto done;
  if (!sbuf_json_string(&b, type ? type : "Fetch")) goto done;
  if (!sbuf_append(&b, "}}")) goto done;
  inspector_notify_network(b.data);
  inspector_notify_request_extra(request_id, headers);

done:
  free(b.data);
  return request_id;
}

void ant_inspector_network_response(
  uint64_t request_id,
  const char *url,
  int status,
  const char *status_text,
  const char *mime_type,
  const char *type,
  const ant_http_header_t *headers
) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.responseReceived\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"loaderId\":\"ant\",\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, ",\"type\":")) goto done;
  if (!sbuf_json_string(&b, type ? type : "Fetch")) goto done;
  if (!sbuf_append(&b, ",\"response\":{\"url\":")) goto done;
  if (!sbuf_json_string(&b, url ? url : "")) goto done;
  if (!sbuf_append(&b, ",\"status\":")) goto done;
  if (!sbuf_appendf(&b, "%d", status)) goto done;
  if (!sbuf_append(&b, ",\"statusText\":")) goto done;
  if (!sbuf_json_string(&b, status_text ? status_text : "")) goto done;
  if (!sbuf_append(&b, ",\"headers\":")) goto done;
  if (!inspector_append_headers_object(&b, headers)) goto done;
  if (!sbuf_append(&b, ",\"mimeType\":")) goto done;
  if (!sbuf_json_string(&b, mime_type ? mime_type : "")) goto done;
  if (!sbuf_append(&b, ",\"connectionReused\":false,\"connectionId\":0,\"encodedDataLength\":0,\"protocol\":\"http/1.1\",\"securityState\":\"unknown\"}}}")) goto done;
  inspector_notify_network(b.data);
  inspector_notify_response_extra(request_id, status, headers);

done:
  free(b.data);
}

void ant_inspector_network_finish(uint64_t request_id, size_t encoded_data_length) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.loadingFinished\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, ",\"encodedDataLength\":")) goto done;
  if (!sbuf_appendf(&b, "%zu", encoded_data_length)) goto done;
  if (!sbuf_append(&b, "}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
}

void ant_inspector_network_fail(
  uint64_t request_id,
  const char *error_text,
  bool canceled,
  const char *type
) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.loadingFailed\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, ",\"type\":")) goto done;
  if (!sbuf_json_string(&b, type ? type : "Fetch")) goto done;
  if (!sbuf_append(&b, ",\"errorText\":")) goto done;
  if (!sbuf_json_string(&b, error_text ? error_text : "Failed")) goto done;
  if (!sbuf_append(&b, ",\"canceled\":")) goto done;
  if (!sbuf_append(&b, canceled ? "true" : "false")) goto done;
  if (!sbuf_append(&b, "}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
}

void ant_inspector_network_set_request_body(uint64_t request_id, const uint8_t *data, size_t len) {
  inspector_network_entry_t *entry = inspector_network_entry_for_id(request_id, true);
  if (!entry) return;
  inspector_network_store_body(
    &entry->request_body,
    &entry->request_body_len,
    &entry->request_body_truncated,
    data,
    len,
    false
  );
}

void ant_inspector_network_append_response_body(uint64_t request_id, const uint8_t *data, size_t len) {
  inspector_network_entry_t *entry = inspector_network_entry_for_id(request_id, true);
  if (!entry) return;
  inspector_network_store_body(
    &entry->response_body,
    &entry->response_body_len,
    &entry->response_body_truncated,
    data,
    len,
    true
  );
  if (request_id && len > 0 && g_inspector.started && g_inspector.attached) {
    sbuf_t b = {0};
    if (sbuf_append(&b, "{\"method\":\"Network.dataReceived\",\"params\":{\"requestId\":\"") &&
        sbuf_appendf(&b, "%" PRIu64, request_id) &&
        sbuf_append(&b, "\",\"timestamp\":") &&
        sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds()) &&
        sbuf_append(&b, ",\"dataLength\":") &&
        sbuf_appendf(&b, "%zu", len) &&
        sbuf_append(&b, ",\"encodedDataLength\":") &&
        sbuf_appendf(&b, "%zu", len) &&
        sbuf_append(&b, "}}")) {
      inspector_notify_network(b.data);
    }
    free(b.data);
  }
}

uint64_t ant_inspector_websocket_created(const char *url) {
  if (!g_inspector.started || !g_inspector.attached || !inspector_network_has_ready_client()) return 0;
  uint64_t request_id = ++g_inspector.next_network_request_id;
  if (request_id == 0) request_id = ++g_inspector.next_network_request_id;
  inspector_network_entry_for_id(request_id, true);

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.webSocketCreated\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"url\":")) goto done;
  if (!sbuf_json_string(&b, url ? url : "")) goto done;
  if (!sbuf_append(&b, ",\"initiator\":{\"type\":\"WebSocket\"}}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
  return request_id;
}

void ant_inspector_websocket_request(uint64_t request_id, const ant_http_header_t *headers) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.webSocketWillSendHandshakeRequest\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, ",\"wallTime\":")) goto done;
  if (!sbuf_appendf(&b, "%.3f", inspector_wall_time_seconds())) goto done;
  if (!sbuf_append(&b, ",\"request\":{\"headers\":")) goto done;
  if (!inspector_append_headers_object(&b, headers)) goto done;
  if (!sbuf_append(&b, "}}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
}

void ant_inspector_websocket_response(uint64_t request_id, int status, const char *status_text, const ant_http_header_t *headers) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.webSocketHandshakeResponseReceived\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, ",\"response\":{\"status\":")) goto done;
  if (!sbuf_appendf(&b, "%d", status)) goto done;
  if (!sbuf_append(&b, ",\"statusText\":")) goto done;
  if (!sbuf_json_string(&b, status_text ? status_text : "")) goto done;
  if (!sbuf_append(&b, ",\"headers\":")) goto done;
  if (!inspector_append_headers_object(&b, headers)) goto done;
  if (!sbuf_append(&b, "}}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
}

static void inspector_websocket_frame(uint64_t request_id, const char *method, const uint8_t *data, size_t len, bool binary) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  char *encoded = NULL;
  size_t encoded_len = 0;
  const char *payload = (const char *)data;
  size_t payload_len = len;

  if (binary) {
    encoded = ant_base64_encode(data, len, &encoded_len);
    if (!encoded) return;
    payload = encoded;
    payload_len = encoded_len;
  }

  if (!sbuf_append(&b, "{\"method\":\"")) goto done;
  if (!sbuf_append(&b, method)) goto done;
  if (!sbuf_append(&b, "\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, ",\"response\":{\"opcode\":")) goto done;
  if (!sbuf_append(&b, binary ? "2" : "1")) goto done;
  if (!sbuf_append(&b, ",\"mask\":false,\"payloadData\":")) goto done;
  if (!sbuf_json_string_len(&b, payload ? payload : "", payload ? payload_len : 0)) goto done;
  if (!sbuf_append(&b, "}}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(encoded);
  free(b.data);
}

void ant_inspector_websocket_frame_sent(uint64_t request_id, const uint8_t *data, size_t len, bool binary) {
  inspector_websocket_frame(request_id, "Network.webSocketFrameSent", data, len, binary);
}

void ant_inspector_websocket_frame_received(uint64_t request_id, const uint8_t *data, size_t len, bool binary) {
  inspector_websocket_frame(request_id, "Network.webSocketFrameReceived", data, len, binary);
}

void ant_inspector_websocket_error(uint64_t request_id, const char *message) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.webSocketFrameError\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, ",\"errorMessage\":")) goto done;
  if (!sbuf_json_string(&b, message ? message : "WebSocket error")) goto done;
  if (!sbuf_append(&b, "}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
}

void ant_inspector_websocket_closed(uint64_t request_id) {
  if (!request_id || !g_inspector.started || !g_inspector.attached) return;

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Network.webSocketClosed\",\"params\":{\"requestId\":\"")) goto done;
  if (!sbuf_appendf(&b, "%" PRIu64, request_id)) goto done;
  if (!sbuf_append(&b, "\",\"timestamp\":")) goto done;
  if (!sbuf_appendf(&b, "%.6f", inspector_timestamp_seconds())) goto done;
  if (!sbuf_append(&b, "}}")) goto done;
  inspector_notify_network(b.data);

done:
  free(b.data);
}
