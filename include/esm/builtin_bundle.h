#ifndef ESM_BUILTIN_BUNDLE_H
#define ESM_BUILTIN_BUNDLE_H

#include "esm/loader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *specifier;
  const char *source_name;
  const uint8_t *code;
  size_t code_len;
  ant_module_format_t format;
} ant_builtin_bundle_entry_t;

bool esm_is_builtin_specifier(const char *specifier);
const ant_builtin_bundle_entry_t *esm_lookup_builtin_bundle(const char *specifier, size_t spec_len);

#endif
