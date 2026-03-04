#ifndef ESM_LOADER_H
#define ESM_LOADER_H

#include "types.h"

#include <stddef.h>
#include <stdbool.h>

void js_esm_cleanup_module_cache(void);
void js_esm_gc_roots(void (*visit)(void *ctx, ant_value_t *val), void *ctx);

ant_value_t js_esm_import_sync(ant_t *js, ant_value_t specifier);
ant_value_t js_esm_make_file_url(ant_t *js, const char *path);

ant_value_t js_esm_eval_module_source(
  ant_t *js,
  const char *resolved_path, const char *js_code,
  size_t js_len, ant_value_t ns
);

ant_value_t js_esm_import_sync_cstr(ant_t *js, const char *specifier, size_t spec_len);
ant_value_t js_esm_resolve_specifier(ant_t *js, ant_value_t specifier, const char *base_path);

#endif
