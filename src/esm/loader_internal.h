#pragma once

#include "types.h"
#include "esm/loader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uthash.h>

typedef enum {
  ESM_MODULE_KIND_NONE = -1,
  ESM_MODULE_KIND_CODE = 0,
  ESM_MODULE_KIND_JSON,
  ESM_MODULE_KIND_TEXT,
  ESM_MODULE_KIND_IMAGE,
  ESM_MODULE_KIND_NATIVE,
  ESM_MODULE_KIND_URL,
} esm_module_kind_t;

typedef struct esm_module {
  char *path;
  char *cache_key;
  char *resolved_path;
  char *url_content;
  
  size_t url_content_len;
  const uint8_t *embedded_code;
  size_t embedded_code_len;
  
  ant_value_t namespace_obj;
  ant_value_t default_export;
  ant_value_t tla_promise;
  UT_hash_handle hh;
  esm_module_kind_t kind;
  ant_module_format_t format;
  
  bool is_loaded;
  bool is_loading;
  bool has_tla;
  bool owns_embedded;
} esm_module_t;

typedef struct {
  char *data;
  size_t size;
} esm_file_data_t;

const char *esm_default_base_path(ant_t *js);
char *esm_file_url_to_path(ant_t *js, const char *specifier);
char *esm_path_to_file_url(const char *path);
char *esm_make_absolute_path(const char *path);
bool esm_is_json(const char *path);
ant_module_format_t esm_decide_module_format(ant_t *js, const char *resolved_path);
char *esm_resolve_path(ant_t *js, const char *specifier, const char *base_path);
char *esm_resolve_path_require(ant_t *js, const char *specifier, const char *base_path);
ant_value_t esm_read_file(ant_t *js, const char *path, const char *kind, esm_file_data_t *out);
esm_module_t *esm_find_module(ant_t *js, const char *module_key);

ant_value_t esm_get_or_load_ex(
  ant_t *js,
  const char *specifier,
  const char *resolved_path,
  const char *module_key,
  ant_module_format_t format,
  const uint8_t *embedded_code,
  size_t embedded_code_len,
  bool copy_embedded,
  esm_module_kind_t kind_hint
);

bool esm_hooks_present(ant_t *js);
ant_value_t esm_import_via_hooks(ant_t *js, const char *specifier, size_t spec_len, const char *base_path, ant_value_t attrs, bool is_require, bool *handled);
