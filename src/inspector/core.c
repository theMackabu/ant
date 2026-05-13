#include "bind.h"
#include "http/websocket.h"
#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

inspector_state_t g_inspector = {0};

const size_t k_inspector_network_body_limit = 10 * 1024 * 1024;
const size_t k_inspector_console_event_limit = 200;

static bool sbuf_reserve(sbuf_t *b, size_t extra) {
  if (extra > SIZE_MAX - b->len - 1) return false;
  size_t need = b->len + extra + 1;
  
  if (need <= b->cap) return true;
  size_t cap = b->cap ? b->cap * 2 : 256;
  
  while (cap < need) {
    if (cap > SIZE_MAX / 2) return false;
    cap *= 2;
  }
  
  char *next = realloc(b->data, cap);
  if (!next) return false;
  
  b->data = next;
  b->cap = cap;
  
  return true;
}

bool sbuf_append_len(sbuf_t *b, const char *s, size_t len) {
  if (!s) return true;
  if (!sbuf_reserve(b, len)) return false;
  
  memcpy(b->data + b->len, s, len);
  b->len += len;
  b->data[b->len] = '\0';
  
  return true;
}

bool sbuf_append(sbuf_t *b, const char *s) {
  return sbuf_append_len(b, s, s ? strlen(s) : 0);
}

static bool sbuf_vappendf(sbuf_t *b, const char *fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
  int n = vsnprintf(NULL, 0, fmt, cp);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  va_end(cp);
  if (n < 0) return false;
  if (!sbuf_reserve(b, (size_t)n)) {
    return false;
  }
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
  vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  b->len += (size_t)n;
  return true;
}

#if defined(__GNUC__) || defined(__clang__)
bool sbuf_appendf(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
bool sbuf_appendf(sbuf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool ok = sbuf_vappendf(b, fmt, ap);
  va_end(ap);
  return ok;
}

bool sbuf_json_string_len(sbuf_t *b, const char *s, size_t len) {
  if (!sbuf_append(b, "\"")) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  if (!sbuf_append(b, "\\\"")) return false; break;
      case '\\': if (!sbuf_append(b, "\\\\")) return false; break;
      case '\b': if (!sbuf_append(b, "\\b")) return false; break;
      case '\f': if (!sbuf_append(b, "\\f")) return false; break;
      case '\n': if (!sbuf_append(b, "\\n")) return false; break;
      case '\r': if (!sbuf_append(b, "\\r")) return false; break;
      case '\t': if (!sbuf_append(b, "\\t")) return false; break;
      default:
        if (c < 0x20) {
          if (!sbuf_appendf(b, "\\u%04x", (unsigned)c)) return false;
        } else if (!sbuf_append_len(b, (const char *)&c, 1)) return false;
        break;
    }
  }
  return sbuf_append(b, "\"");
}

bool sbuf_json_string(sbuf_t *b, const char *s) {
  return sbuf_json_string_len(b, s ? s : "", s ? strlen(s) : 0);
}

static void inspector_write_free(uv_write_t *req, int status) {
  inspector_write_t *wr = (inspector_write_t *)req;
  free(wr->buf.base);
  free(wr);
}

void inspector_send_raw(inspector_client_t *client, const char *data, size_t len) {
  if (!client || !data) return;
  inspector_write_t *wr = calloc(1, sizeof(*wr));
  
  if (!wr) return;
  wr->buf.base = malloc(len);
  
  if (!wr->buf.base) {
    free(wr);
    return;
  }
  
  memcpy(wr->buf.base, data, len);
  wr->buf.len = len;
  
  if (uv_write(&wr->req, (uv_stream_t *)&client->handle, &wr->buf, 1, inspector_write_free) != 0) {
    free(wr->buf.base);
    free(wr);
  }
}

void inspector_send_ws(inspector_client_t *client, const char *json) {
  if (!client || !client->websocket || !json) return;
  size_t frame_len = 0;
  uint8_t *frame = ant_ws_encode_frame(ANT_WS_OPCODE_TEXT, (const uint8_t *)json, strlen(json), false, &frame_len);
  if (!frame) return;
  inspector_send_raw(client, (const char *)frame, frame_len);
  free(frame);
}

void inspector_send_response_obj(inspector_client_t *client, int id, const char *result_obj) {
  sbuf_t b = {0};
  inspector_json_t json;
  inspector_json_init(&json, &b);
  if (
    inspector_json_begin_object(&json) &&
    inspector_json_key(&json, "id") &&
    inspector_json_int(&json, id) &&
    inspector_json_key(&json, "result") &&
    inspector_json_raw(&json, result_obj ? result_obj : "{}") &&
    inspector_json_end_object(&json)
  ) {
    inspector_send_ws(client, b.data);
  }
  free(b.data);
}

void inspector_send_empty_result(inspector_client_t *client, int id) {
  inspector_send_response_obj(client, id, "{}");
}

void inspector_send_error(inspector_client_t *client, int id, int code, const char *message) {
  sbuf_t b = {0};
  inspector_json_t json;
  inspector_json_init(&json, &b);
  if (
    inspector_json_begin_object(&json) &&
    inspector_json_key(&json, "id") &&
    inspector_json_int(&json, id) &&
    inspector_json_key(&json, "error") &&
    inspector_json_begin_object(&json) &&
    inspector_json_key(&json, "code") &&
    inspector_json_int(&json, code) &&
    inspector_json_key(&json, "message") &&
    inspector_json_string(&json, message ? message : "Inspector error") &&
    inspector_json_end_object(&json) &&
    inspector_json_end_object(&json)
  ) {
    inspector_send_ws(client, b.data);
  }
  free(b.data);
}

bool inspector_param_bool(yyjson_val *params, const char *name) {
  yyjson_val *value = params ? yyjson_obj_get(params, name) : NULL;
  return value && yyjson_is_bool(value) && yyjson_get_bool(value);
}

double inspector_timestamp_seconds(void) {
  return (double)uv_hrtime() / 1000000000.0;
}

double inspector_wall_time_seconds(void) {
  return (double)time(NULL);
}
