#ifndef ESM_COMMONJS_H
#define ESM_COMMONJS_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

ant_value_t esm_require_cache(ant_t *js);
ant_value_t esm_require_cache_lookup(ant_t *js, const char *key);
ant_value_t esm_require_cached_exports(ant_t *js, ant_value_t cached);
ant_value_t esm_require_namespace(ant_t *js, ant_value_t exports);
ant_value_t esm_require_unwrap(ant_t *js, ant_value_t ns);

ant_value_t esm_create_cjs_module(ant_t *js, const char *filename, ant_value_t parent);
ant_value_t esm_cjs_require_paths(ant_t *js, ant_value_t *args, int nargs);
ant_value_t esm_cjs_require_module(ant_t *js, ant_value_t *args, int nargs);

ant_value_t esm_load_commonjs_module(
  ant_t *js, const char *module_path,
  const char *code, size_t code_len, ant_value_t ns
);

void esm_cjs_update_children(
  ant_t *js, ant_value_t parent,
  ant_value_t child, bool remove
);

#endif
