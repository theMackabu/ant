#include "bind.h"
#include "base64.h"
#include "inspector.h"

#include <inttypes.h>
#include <stdio.h>
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
  return client && client->network_enabled && ant_conn_write_queue_size(client->conn) <= k_inspector_network_write_queue_limit;
}

static bool inspector_network_has_ready_client(void) {
  for (const inspector_client_t *c = g_inspector.clients; c; c = c->next)
    if (inspector_network_client_ready(c)) return true;
  return false;
}

static inline bool inspector_network_tracking(uint64_t request_id) {
  return __builtin_expect(request_id && g_inspector.started && g_inspector.attached, 0);
}

bool ant_inspector_network_active(void) {
  return g_inspector.started &&
    g_inspector.attached &&
    inspector_network_has_ready_client();
}

static inspector_network_entry_t *inspector_network_entry_for_id(uint64_t id, bool create) {
  if (id == 0) return NULL;
  for (inspector_network_entry_t *entry = g_inspector.network_entries; entry; entry = entry->next)
    if (entry->id == id) return entry;
  
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
  uint8_t **body, size_t *body_len,
  bool *truncated, const uint8_t *data,
  size_t len, bool append
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

static yyjson_mut_val *inspector_network_params(inspector_doc_t *d, uint64_t request_id) {
  char id[24];
  snprintf(id, sizeof(id), "%" PRIu64, request_id);
  yyjson_mut_doc *doc = inspector_doc_begin(d);
  yyjson_mut_val *params = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, params, "requestId", id);
  return params;
}

static void inspector_network_emit(inspector_doc_t *d, const char *method, yyjson_mut_val *params) {
  char *json = inspector_event_finish(d, method, params);
  inspector_notify_network(json);
  free(json);
}

static void inspector_add_timestamp(yyjson_mut_doc *doc, yyjson_mut_val *params) {
  yyjson_mut_obj_add_real(doc, params, "timestamp", inspector_timestamp_seconds());
}

static void inspector_add_headers(yyjson_mut_doc *doc, yyjson_mut_val *obj, const ant_http_header_t *headers) {
  yyjson_mut_val *out = yyjson_mut_obj_add_obj(doc, obj, "headers");
  for (const ant_http_header_t *header = headers; header; header = header->next) {
    if (!header->name || !header->value) continue;
    yyjson_mut_obj_add_str(doc, out, header->name, header->value);
  }
}

static void inspector_notify_request_extra(uint64_t request_id, const ant_http_header_t *headers) {
  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  yyjson_mut_obj_add_arr(d.doc, params, "associatedCookies");
  inspector_add_headers(d.doc, params, headers);
  yyjson_mut_val *timing = yyjson_mut_obj_add_obj(d.doc, params, "connectTiming");
  yyjson_mut_obj_add_real(d.doc, timing, "requestTime", inspector_timestamp_seconds());
  yyjson_mut_obj_add_bool(d.doc, params, "siteHasCookieInOtherPartition", false);
  inspector_network_emit(&d, "Network.requestWillBeSentExtraInfo", params);
}

static void inspector_notify_response_extra(uint64_t request_id, int status, const ant_http_header_t *headers) {
  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  yyjson_mut_obj_add_arr(d.doc, params, "blockedCookies");
  inspector_add_headers(d.doc, params, headers);
  yyjson_mut_obj_add_str(d.doc, params, "resourceIPAddressSpace", "Unknown");
  yyjson_mut_obj_add_int(d.doc, params, "statusCode", status);
  inspector_network_emit(&d, "Network.responseReceivedExtraInfo", params);
}

static const char *inspector_network_initiator_url(const char *label) {
  if (label && strcmp(label, "Server") == 0) return "Server";
  if (label && strcmp(label, "EventSource") == 0) return "EventSource";
  return "Fetch";
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
  size_t out_len = 0;
  char *encoded = NULL;

  if (entry && entry->response_body && entry->response_body_len > 0) {
    encoded = ant_base64_encode(entry->response_body, entry->response_body_len, &out_len);
    if (!encoded) { inspector_send_error(client, id, -32000, "Out of memory"); return; }
  }

  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strn(doc, result, "body", encoded ? encoded : "", out_len);
  yyjson_mut_obj_add_bool(doc, result, "base64Encoded", encoded != NULL);
  inspector_send_result(client, id, &d, result);
  free(encoded);
}

void inspector_get_request_post_data(inspector_client_t *client, int id, yyjson_val *params) {
  uint64_t request_id = inspector_request_id_param(params);
  inspector_network_entry_t *entry = inspector_network_entry_for_id(request_id, false);
  const char *body = entry && entry->request_body ? (const char *)entry->request_body : "";
  size_t body_len = entry && entry->request_body ? entry->request_body_len : 0;

  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strn(doc, result, "postData", body, body_len);
  inspector_send_result(client, id, &d, result);
}

uint64_t ant_inspector_network_request(
  const char *method,
  const char *url,
  const char *type,
  const char *initiator,
  bool has_post_data,
  const ant_http_header_t *headers
) {
  if (!ant_inspector_network_active()) return 0;
  uint64_t request_id = ++g_inspector.next_network_request_id;
  if (request_id == 0) request_id = ++g_inspector.next_network_request_id;
  inspector_network_entry_for_id(request_id, true);

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  yyjson_mut_obj_add_str(d.doc, params, "loaderId", "ant");
  yyjson_mut_obj_add_str(d.doc, params, "documentURL", url ? url : "");

  yyjson_mut_val *request = yyjson_mut_obj_add_obj(d.doc, params, "request");
  yyjson_mut_obj_add_str(d.doc, request, "url", url ? url : "");
  yyjson_mut_obj_add_str(d.doc, request, "method", method ? method : "GET");
  inspector_add_headers(d.doc, request, headers);
  if (has_post_data) yyjson_mut_obj_add_bool(d.doc, request, "hasPostData", true);
  yyjson_mut_obj_add_str(d.doc, request, "initialPriority", "High");
  yyjson_mut_obj_add_str(d.doc, request, "referrerPolicy", "no-referrer");

  inspector_add_timestamp(d.doc, params);
  yyjson_mut_obj_add_real(d.doc, params, "wallTime", inspector_wall_time_seconds());
  yyjson_mut_val *initiator_obj = yyjson_mut_obj_add_obj(d.doc, params, "initiator");
  yyjson_mut_obj_add_str(d.doc, initiator_obj, "type", "parser");
  yyjson_mut_obj_add_str(d.doc, initiator_obj, "url", inspector_network_initiator_url(initiator));
  yyjson_mut_obj_add_str(d.doc, params, "type", type ? type : "Fetch");

  inspector_network_emit(&d, "Network.requestWillBeSent", params);
  inspector_notify_request_extra(request_id, headers);
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
  if (!inspector_network_tracking(request_id)) return;

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  yyjson_mut_obj_add_str(d.doc, params, "loaderId", "ant");
  inspector_add_timestamp(d.doc, params);
  yyjson_mut_obj_add_str(d.doc, params, "type", type ? type : "Fetch");

  yyjson_mut_val *response = yyjson_mut_obj_add_obj(d.doc, params, "response");
  yyjson_mut_obj_add_str(d.doc, response, "url", url ? url : "");
  yyjson_mut_obj_add_int(d.doc, response, "status", status);
  yyjson_mut_obj_add_str(d.doc, response, "statusText", status_text ? status_text : "");
  inspector_add_headers(d.doc, response, headers);
  yyjson_mut_obj_add_str(d.doc, response, "mimeType", mime_type ? mime_type : "");
  yyjson_mut_obj_add_bool(d.doc, response, "connectionReused", false);
  yyjson_mut_obj_add_int(d.doc, response, "connectionId", 0);
  yyjson_mut_obj_add_int(d.doc, response, "encodedDataLength", 0);
  yyjson_mut_obj_add_str(d.doc, response, "protocol", "http/1.1");
  yyjson_mut_obj_add_str(d.doc, response, "securityState", "unknown");

  inspector_network_emit(&d, "Network.responseReceived", params);
  inspector_notify_response_extra(request_id, status, headers);
}

void ant_inspector_network_finish(uint64_t request_id, size_t encoded_data_length) {
  if (!inspector_network_tracking(request_id)) return;

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  inspector_add_timestamp(d.doc, params);
  yyjson_mut_obj_add_uint(d.doc, params, "encodedDataLength", encoded_data_length);
  inspector_network_emit(&d, "Network.loadingFinished", params);
}

void ant_inspector_network_fail(
  uint64_t request_id,
  const char *error_text,
  bool canceled,
  const char *type
) {
  if (!inspector_network_tracking(request_id)) return;

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  inspector_add_timestamp(d.doc, params);
  yyjson_mut_obj_add_str(d.doc, params, "type", type ? type : "Fetch");
  yyjson_mut_obj_add_str(d.doc, params, "errorText", error_text ? error_text : "Failed");
  yyjson_mut_obj_add_bool(d.doc, params, "canceled", canceled);
  inspector_network_emit(&d, "Network.loadingFailed", params);
}

void ant_inspector_network_set_request_body(uint64_t request_id, const uint8_t *data, size_t len) {
  inspector_network_entry_t *entry = inspector_network_entry_for_id(request_id, true);
  if (!entry) return;
  inspector_network_store_body(
    &entry->request_body,
    &entry->request_body_len,
    &entry->request_body_truncated,
    data, len, false
  );
}

void ant_inspector_network_append_response_body(uint64_t request_id, const uint8_t *data, size_t len) {
  inspector_network_entry_t *entry = inspector_network_entry_for_id(request_id, true);
  if (!entry) return;
  inspector_network_store_body(
    &entry->response_body,
    &entry->response_body_len,
    &entry->response_body_truncated,
    data, len, true
  );
  if (len == 0 || !inspector_network_tracking(request_id)) return;

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  inspector_add_timestamp(d.doc, params);
  yyjson_mut_obj_add_uint(d.doc, params, "dataLength", len);
  yyjson_mut_obj_add_uint(d.doc, params, "encodedDataLength", len);
  inspector_network_emit(&d, "Network.dataReceived", params);
}

uint64_t ant_inspector_websocket_created(const char *url) {
  if (!__builtin_expect(ant_inspector_network_active(), 0)) return 0;
  uint64_t request_id = ++g_inspector.next_network_request_id;
  if (request_id == 0) request_id = ++g_inspector.next_network_request_id;
  inspector_network_entry_for_id(request_id, true);

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  yyjson_mut_obj_add_str(d.doc, params, "url", url ? url : "");
  yyjson_mut_val *initiator = yyjson_mut_obj_add_obj(d.doc, params, "initiator");
  yyjson_mut_obj_add_str(d.doc, initiator, "type", "WebSocket");
  inspector_network_emit(&d, "Network.webSocketCreated", params);
  return request_id;
}

void ant_inspector_websocket_request(uint64_t request_id, const ant_http_header_t *headers) {
  if (!inspector_network_tracking(request_id)) return;

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  inspector_add_timestamp(d.doc, params);
  yyjson_mut_obj_add_real(d.doc, params, "wallTime", inspector_wall_time_seconds());
  inspector_add_headers(d.doc, yyjson_mut_obj_add_obj(d.doc, params, "request"), headers);
  inspector_network_emit(&d, "Network.webSocketWillSendHandshakeRequest", params);
}

void ant_inspector_websocket_response(uint64_t request_id, int status, const char *status_text, const ant_http_header_t *headers) {
  if (!inspector_network_tracking(request_id)) return;

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  inspector_add_timestamp(d.doc, params);
  yyjson_mut_val *response = yyjson_mut_obj_add_obj(d.doc, params, "response");
  yyjson_mut_obj_add_int(d.doc, response, "status", status);
  yyjson_mut_obj_add_str(d.doc, response, "statusText", status_text ? status_text : "");
  inspector_add_headers(d.doc, response, headers);
  inspector_network_emit(&d, "Network.webSocketHandshakeResponseReceived", params);
}

static void inspector_websocket_frame(uint64_t request_id, const char *method, const uint8_t *data, size_t len, bool binary) {
  if (!inspector_network_tracking(request_id)) return;

  char *encoded = NULL;
  const char *payload = (const char *)data;
  size_t payload_len = len;

  if (binary) {
    encoded = ant_base64_encode(data, len, &payload_len);
    if (!encoded) return;
    payload = encoded;
  }

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  inspector_add_timestamp(d.doc, params);
  yyjson_mut_val *response = yyjson_mut_obj_add_obj(d.doc, params, "response");
  yyjson_mut_obj_add_int(d.doc, response, "opcode", binary ? 2 : 1);
  yyjson_mut_obj_add_bool(d.doc, response, "mask", false);
  yyjson_mut_obj_add_strn(d.doc, response, "payloadData", payload ? payload : "", payload ? payload_len : 0);
  inspector_network_emit(&d, method, params);
  free(encoded);
}

void ant_inspector_websocket_frame_sent(uint64_t request_id, const uint8_t *data, size_t len, bool binary) {
  inspector_websocket_frame(request_id, "Network.webSocketFrameSent", data, len, binary);
}

void ant_inspector_websocket_frame_received(uint64_t request_id, const uint8_t *data, size_t len, bool binary) {
  inspector_websocket_frame(request_id, "Network.webSocketFrameReceived", data, len, binary);
}

void ant_inspector_websocket_error(uint64_t request_id, const char *message) {
  if (!inspector_network_tracking(request_id)) return;

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  inspector_add_timestamp(d.doc, params);
  yyjson_mut_obj_add_str(d.doc, params, "errorMessage", message ? message : "WebSocket error");
  inspector_network_emit(&d, "Network.webSocketFrameError", params);
}

void ant_inspector_websocket_closed(uint64_t request_id) {
  if (!inspector_network_tracking(request_id)) return;

  inspector_doc_t d;
  yyjson_mut_val *params = inspector_network_params(&d, request_id);
  inspector_add_timestamp(d.doc, params);
  inspector_network_emit(&d, "Network.webSocketClosed", params);
}
