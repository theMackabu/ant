#ifndef ESM_BUILTIN_BUNDLE_H
#define ESM_BUILTIN_BUNDLE_H

#include "esm/loader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const uint8_t *code;
  size_t code_len;
  ant_module_format_t format;
} ant_builtin_bundle_module_t;

typedef struct {
  const char *specifier;
  size_t specifier_len;
  const char *source_name;
  size_t module_id;
} ant_builtin_bundle_alias_t;

bool esm_has_builtin_scheme(const char *specifier);

const ant_builtin_bundle_alias_t *esm_lookup_builtin_alias(const char *specifier, size_t spec_len);
const ant_builtin_bundle_module_t *esm_lookup_builtin_module(size_t module_id);

#endif
