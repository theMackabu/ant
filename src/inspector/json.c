#include "json.h"

#include <inttypes.h>
#include <string.h>

void inspector_json_init(inspector_json_t *json, sbuf_t *out) {
  json->out = out;
  json->depth = 0;
  json->wrote_root = false;
}

static bool inspector_json_before_value(inspector_json_t *json) {
  if (!json || !json->out) return false;

  if (json->depth == 0) {
    if (json->wrote_root) return false;
    json->wrote_root = true;
    return true;
  }

  inspector_json_frame_t *frame = &json->stack[json->depth - 1];
  if (frame->type == INSPECTOR_JSON_ARRAY) {
    if (frame->needs_comma && !sbuf_append(json->out, ",")) return false;
    frame->needs_comma = true;
    return true;
  }

  if (!frame->expecting_value) return false;
  frame->expecting_value = false;
  return true;
}

static bool inspector_json_push(inspector_json_t *json, inspector_json_container_t type) {
  if (json->depth >= sizeof(json->stack) / sizeof(json->stack[0])) return false;
  json->stack[json->depth++] = (inspector_json_frame_t){
    .type = type,
    .needs_comma = false,
    .expecting_value = false,
  };
  return true;
}

static bool inspector_json_pop(inspector_json_t *json, inspector_json_container_t type) {
  if (!json || json->depth == 0) return false;
  inspector_json_frame_t *frame = &json->stack[json->depth - 1];
  if (frame->type != type || frame->expecting_value) return false;
  json->depth--;
  return true;
}

bool inspector_json_begin_object(inspector_json_t *json) {
  return
    inspector_json_before_value(json) &&
    sbuf_append(json->out, "{") &&
    inspector_json_push(json, INSPECTOR_JSON_OBJECT);
}

bool inspector_json_end_object(inspector_json_t *json) {
  return
    inspector_json_pop(json, INSPECTOR_JSON_OBJECT) &&
    sbuf_append(json->out, "}");
}

bool inspector_json_begin_array(inspector_json_t *json) {
  return 
    inspector_json_before_value(json) &&
    sbuf_append(json->out, "[") &&
    inspector_json_push(json, INSPECTOR_JSON_ARRAY);
}

bool inspector_json_end_array(inspector_json_t *json) {
  return 
    inspector_json_pop(json, INSPECTOR_JSON_ARRAY) &&
    sbuf_append(json->out, "]");
}

bool inspector_json_key(inspector_json_t *json, const char *key) {
  if (!json || json->depth == 0) return false;
  inspector_json_frame_t *frame = &json->stack[json->depth - 1];
  if (frame->type != INSPECTOR_JSON_OBJECT || frame->expecting_value) return false;

  if (frame->needs_comma && !sbuf_append(json->out, ",")) return false;
  if (!sbuf_json_string(json->out, key ? key : "")) return false;
  if (!sbuf_append(json->out, ":")) return false;

  frame->needs_comma = true;
  frame->expecting_value = true;
  return true;
}

bool inspector_json_raw(inspector_json_t *json, const char *raw) {
  return inspector_json_before_value(json) && sbuf_append(json->out, raw ? raw : "null");
}

bool inspector_json_string(inspector_json_t *json, const char *value) {
  return inspector_json_string_len(json, value ? value : "", value ? strlen(value) : 0);
}

bool inspector_json_string_len(inspector_json_t *json, const char *value, size_t len) {
  return inspector_json_before_value(json) &&
         sbuf_json_string_len(json->out, value ? value : "", value ? len : 0);
}

bool inspector_json_bool(inspector_json_t *json, bool value) {
  return inspector_json_raw(json, value ? "true" : "false");
}

bool inspector_json_int(inspector_json_t *json, int value) {
  if (!inspector_json_before_value(json)) return false;
  return sbuf_appendf(json->out, "%d", value);
}

bool inspector_json_uint64(inspector_json_t *json, uint64_t value) {
  if (!inspector_json_before_value(json)) return false;
  return sbuf_appendf(json->out, "%" PRIu64, value);
}

bool inspector_json_size(inspector_json_t *json, size_t value) {
  if (!inspector_json_before_value(json)) return false;
  return sbuf_appendf(json->out, "%zu", value);
}

bool inspector_json_double(inspector_json_t *json, double value) {
  if (!inspector_json_before_value(json)) return false;
  return sbuf_appendf(json->out, "%.6f", value);
}

bool inspector_json_null(inspector_json_t *json) {
  return inspector_json_raw(json, "null");
}
