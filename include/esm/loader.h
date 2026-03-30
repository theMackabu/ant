#ifndef ESM_LOADER_H
#define ESM_LOADER_H

#include "types.h"

#include <stddef.h>
#include <stdbool.h>

typedef enum {
  MODULE_EVAL_FORMAT_UNKNOWN = 0,
  MODULE_EVAL_FORMAT_ESM,
  MODULE_EVAL_FORMAT_CJS,
} ant_module_format_t;

typedef struct ant_module_t {
  ant_value_t module_ns;
  ant_value_t import_meta;
  ant_value_t prev_import_meta_prop;
  const char *filename;
  const char *parent_path;
  ant_module_format_t format;
  struct ant_module_t *prev;
} ant_module_t;

void js_esm_cleanup_module_cache(void);

ant_value_t js_esm_import_sync(ant_t *js, ant_value_t specifier);
ant_value_t js_esm_make_file_url(ant_t *js, const char *path);
ant_value_t js_esm_import_sync_from(ant_t *js, ant_value_t specifier, const char *base_path);

ant_value_t js_esm_eval_module_source(
  ant_t *js, const char *resolved_path, 
  const char *js_code, size_t js_len, ant_value_t ns
);

ant_value_t js_esm_import_sync_cstr(ant_t *js, const char *specifier, size_t spec_len);
ant_value_t js_esm_resolve_specifier(ant_t *js, ant_value_t specifier, const char *base_path);
ant_value_t js_esm_import_sync_cstr_from(ant_t *js, const char *specifier, size_t spec_len, const char *base_path);
ant_value_t js_esm_import_dynamic(ant_t *js, ant_value_t specifier, const char *base_path, ant_value_t *out_tla_promise);

#endif
