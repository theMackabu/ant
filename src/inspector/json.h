#ifndef ANT_INSPECTOR_JSON_H
#define ANT_INSPECTOR_JSON_H

#include "bind.h"

typedef enum {
  INSPECTOR_JSON_OBJECT,
  INSPECTOR_JSON_ARRAY
} inspector_json_container_t;

typedef struct {
  inspector_json_container_t type;
  bool needs_comma;
  bool expecting_value;
} inspector_json_frame_t;

typedef struct {
  sbuf_t *out;
  inspector_json_frame_t stack[32];
  size_t depth;
  bool wrote_root;
} inspector_json_t;

void inspector_json_init(inspector_json_t *json, sbuf_t *out);
bool inspector_json_begin_object(inspector_json_t *json);
bool inspector_json_end_object(inspector_json_t *json);
bool inspector_json_begin_array(inspector_json_t *json);
bool inspector_json_end_array(inspector_json_t *json);

bool inspector_json_key(inspector_json_t *json, const char *key);
bool inspector_json_raw(inspector_json_t *json, const char *raw);
bool inspector_json_string(inspector_json_t *json, const char *value);
bool inspector_json_string_len(inspector_json_t *json, const char *value, size_t len);
bool inspector_json_bool(inspector_json_t *json, bool value);
bool inspector_json_int(inspector_json_t *json, int value);
bool inspector_json_uint64(inspector_json_t *json, uint64_t value);
bool inspector_json_size(inspector_json_t *json, size_t value);
bool inspector_json_double(inspector_json_t *json, double value);
bool inspector_json_null(inspector_json_t *json);

#endif
