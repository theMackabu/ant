#ifndef ESM_COMMONJS_H
#define ESM_COMMONJS_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  ant_value_t module_obj;
  ant_value_t exports_obj;
  ant_value_t require_fn;
  ant_value_t filename_val;
  ant_value_t dirname_val;
} esm_commonjs_module_record_t;

ant_value_t esm_load_commonjs_module(
  ant_t *js,
  const char *module_path, const char *code,
  size_t code_len, ant_value_t ns
);

ant_value_t esm_commonjs_node_module_paths(
  ant_t *js,
  const char *path,
  size_t path_len
);

ant_value_t esm_create_commonjs_module_record(
  ant_t *js,
  const char *filename,
  size_t filename_len,
  ant_value_t parent,
  ant_value_t ns,
  esm_commonjs_module_record_t *out
);

void esm_setup_commonjs_require(
  ant_t *js,
  ant_value_t require_fn,
  ant_value_t resolve_fn
);

#endif
