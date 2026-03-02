#ifndef ESM_COMMONJS_H
#define ESM_COMMONJS_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

jsval_t esm_load_commonjs_module(
  ant_t *js,
  const char *module_path, const char *code,
  size_t code_len, jsval_t ns
);

#endif
