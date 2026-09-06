#ifndef ESM_COMMONJS_H
#define ESM_COMMONJS_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

ant_value_t esm_require_cache(ant_t *js);

ant_value_t esm_load_commonjs_module(
  ant_t *js,
  const char *module_path, const char *code,
  size_t code_len, ant_value_t ns
);

#endif
