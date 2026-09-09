#ifndef JSON_H
#define JSON_H

#include "types.h"

static constexpr unsigned JSON_LAYOUT_CACHE_ENTRIES = 16;
static constexpr size_t JSON_LAYOUT_CACHE_BYTES = 256 * 1024;
static constexpr size_t JSON_BULK_LAYOUT_MIN_PROPS = 128;

typedef struct {
  ant_shape_t *shape;
  uint64_t hash;
  size_t bytes;
} json_layout_cache_entry_t;

typedef struct json_layout_cache {
  json_layout_cache_entry_t entries[JSON_LAYOUT_CACHE_ENTRIES];
  size_t bytes;
  unsigned count;
} json_layout_cache_t;

void init_json_module(ant_t *js);
void json_layout_cache_clear(ant_t *js);

ant_value_t js_json_parse(ant_params_t);
ant_value_t js_json_stringify(ant_params_t);

ant_value_t json_parse_value(ant_t *js, ant_value_t value);
ant_value_t json_stringify_value(ant_t *js, ant_value_t value);

#endif
