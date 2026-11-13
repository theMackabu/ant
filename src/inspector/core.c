#include "bind.h"
#include "http/websocket.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

inspector_state_t g_inspector = {0};

const size_t k_inspector_network_body_limit = 10 * 1024 * 1024;
const size_t k_inspector_network_entry_limit = 1024;

const size_t k_inspector_network_write_queue_limit = 4 * 1024 * 1024;
const size_t k_inspector_console_event_limit = 200;

static void *inspector_doc_malloc(void *ctx, size_t size) {
  void *ptr = malloc(size);
  if (!ptr) *(bool *)ctx = true;
  return ptr;
}

static void *inspector_doc_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
  void *next = realloc(ptr, size);
  if (!next) *(bool *)ctx = true;
  return next;
}

static void inspector_doc_free(void *ctx, void *ptr) {
  free(ptr);
}

yyjson_mut_doc *inspector_doc_begin(inspector_doc_t *d) {
  yyjson_alc alc = {inspector_doc_malloc, inspector_doc_realloc, inspector_doc_free, &d->failed};
  d->failed = false;
  d->doc = yyjson_mut_doc_new(&alc);
  return d->doc;
}

char *inspector_doc_finish(inspector_doc_t *d, yyjson_mut_val *root) {
  char *json = NULL;
  if (d->doc && root && !d->failed) {
    yyjson_mut_doc_set_root(d->doc, root);
    json = yyjson_mut_write_opts(d->doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL, NULL, NULL);
  }
  yyjson_mut_doc_free(d->doc);
  d->doc = NULL;
  return json;
}

char *inspector_event_finish(inspector_doc_t *d, const char *method, yyjson_mut_val *params) {
  yyjson_mut_val *root = yyjson_mut_obj(d->doc);
  yyjson_mut_obj_add_str(d->doc, root, "method", method);
  yyjson_mut_obj_add_val(d->doc, root, "params", params);
  return inspector_doc_finish(d, root);
}

void inspector_send_ws(inspector_client_t *client, const char *json) {
  if (!client || !client->websocket || !json) return;
  size_t frame_len = 0;
  uint8_t *frame = ant_ws_encode_frame(ANT_WS_OPCODE_TEXT, (const uint8_t *)json, strlen(json), false, false, &frame_len);
  if (frame) ant_conn_write(client->conn, (char *)frame, frame_len, NULL, NULL);
}

void inspector_send_result(inspector_client_t *client, int id, inspector_doc_t *d, yyjson_mut_val *result) {
  yyjson_mut_val *root = yyjson_mut_obj(d->doc);
  yyjson_mut_obj_add_int(d->doc, root, "id", id);
  yyjson_mut_obj_add_val(d->doc, root, "result", result);

  char *json = inspector_doc_finish(d, root);
  if (json) inspector_send_ws(client, json);
  else inspector_send_error(client, id, -32000, "Out of memory");
  free(json);
}

void inspector_send_response_obj(inspector_client_t *client, int id, const char *result_obj) {
  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  inspector_send_result(client, id, &d, yyjson_mut_raw(doc, result_obj ? result_obj : "{}"));
}

void inspector_send_empty_result(inspector_client_t *client, int id) {
  inspector_send_response_obj(client, id, "{}");
}

void inspector_send_error(inspector_client_t *client, int id, int code, const char *message) {
  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_int(doc, root, "id", id);

  yyjson_mut_val *error = yyjson_mut_obj_add_obj(doc, root, "error");
  yyjson_mut_obj_add_int(doc, error, "code", code);
  yyjson_mut_obj_add_str(doc, error, "message", message ? message : "Inspector error");

  char *json = inspector_doc_finish(&d, root);
  inspector_send_ws(client, json);
  free(json);
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

char *inspector_concat(const char *prefix, const char *suffix) {
  size_t prefix_len = strlen(prefix);
  size_t suffix_len = strlen(suffix);
  char *out = malloc(prefix_len + suffix_len + 1);
  if (!out) return NULL;
  memcpy(out, prefix, prefix_len);
  memcpy(out + prefix_len, suffix, suffix_len + 1);
  return out;
}
