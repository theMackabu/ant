#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "esm/builtin_bundle.h"
#include "builtin_bundle_data.h"

static ant_builtin_bundle_module_t bundle_inflated[
  sizeof(ant_builtin_bundle_modules) / sizeof(ant_builtin_bundle_modules[0])
];
static bool bundle_inflated_ready[
  sizeof(ant_builtin_bundle_modules) / sizeof(ant_builtin_bundle_modules[0])
];

static bool inflate_builtin_module(const ant_builtin_bundle_module_t *src, uint8_t *out) {
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, -15) != Z_OK) return false;

  zs.next_in = (Bytef *)src->code;
  zs.avail_in = (uInt)src->code_len;
  zs.next_out = out;
  zs.avail_out = (uInt)src->raw_len;

  int rc = inflate(&zs, Z_FINISH);
  bool ok = rc == Z_STREAM_END && zs.total_out == src->raw_len;
  
  inflateEnd(&zs);

  return ok;
}

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

  const ant_builtin_bundle_module_t *stored = &ant_builtin_bundle_modules[module_id];
  if (stored->raw_len == stored->code_len) return stored;
  if (bundle_inflated_ready[module_id]) return &bundle_inflated[module_id];

  uint8_t *code = malloc(stored->raw_len + 1);
  if (!code) return NULL;
  
  if (!inflate_builtin_module(stored, code)) {
    free(code);
    return NULL;
  }
  
  code[stored->raw_len] = '\0';
  bundle_inflated[module_id] = *stored;
  bundle_inflated[module_id].code = code;
  bundle_inflated[module_id].code_len = stored->raw_len;
  bundle_inflated_ready[module_id] = true;

  return &bundle_inflated[module_id];
}
