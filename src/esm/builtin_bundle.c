#include <string.h>

#include "esm/builtin_bundle.h"
#include "builtin_bundle_data.h"

bool esm_has_builtin_scheme(const char *specifier) {
  if (!specifier) return false;
  
  if (specifier[0] == 'n') return strncmp(specifier, "node:", 5) == 0;
  if (specifier[0] == 'a') return strncmp(specifier, "ant:" , 4) == 0;
  
  return false;
}


const ant_builtin_bundle_alias_t *esm_lookup_builtin_alias(const char *specifier, size_t spec_len) {
  if (!specifier) return NULL;

  for (size_t i = 0; i < ant_builtin_bundle_alias_count; i++) {
    const ant_builtin_bundle_alias_t *alias = &ant_builtin_bundle_aliases[i];
    if (alias->specifier_len != spec_len) continue;
    if (memcmp(alias->specifier, specifier, spec_len) != 0) continue;
    return alias;
  }

  return NULL;
}

const ant_builtin_bundle_module_t *esm_lookup_builtin_module(size_t module_id) {
  if (module_id >= ant_builtin_bundle_module_count) return NULL;
  return &ant_builtin_bundle_modules[module_id];
}
