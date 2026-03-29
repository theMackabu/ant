#include <string.h>

#include "esm/builtin_bundle.h"
#include "builtin_bundle_data.h"

bool esm_is_builtin_specifier(const char *specifier) {
  if (!specifier) return false;
  return
    strncmp(specifier, "node:", 5) == 0 ||
    strncmp(specifier, "ant:", 4) == 0;
}

const ant_builtin_bundle_entry_t *esm_lookup_builtin_bundle(const char *specifier, size_t spec_len) {
  size_t i = 0;
  if (!specifier) return NULL;
  
  for (i = 0; i < ant_builtin_bundle_data_count; i++) {
    const ant_builtin_bundle_entry_t *entry = &ant_builtin_bundle_data[i];
    size_t entry_len = strlen(entry->specifier);
    if (entry_len == spec_len && memcmp(entry->specifier, specifier, spec_len) == 0) return entry;
  }

  return NULL;
}
