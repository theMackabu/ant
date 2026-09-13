#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "esm/builtin_bundle.h"
#include "builtin_bundle_data.h"

constexpr size_t BUNDLE_MODULE_SLOTS =
  sizeof(ant_builtin_bundle_modules) / 
  sizeof(ant_builtin_bundle_modules[0]);

static ant_builtin_bundle_module_t bundle_inflated[BUNDLE_MODULE_SLOTS];
static _Atomic bool bundle_inflated_ready[BUNDLE_MODULE_SLOTS];

#ifdef _WIN32
static SRWLOCK bundle_inflate_lock = SRWLOCK_INIT;
static void bundle_lock_acquire(void) { AcquireSRWLockExclusive(&bundle_inflate_lock); }
static void bundle_lock_release(void) { ReleaseSRWLockExclusive(&bundle_inflate_lock); }
#else
static pthread_mutex_t bundle_inflate_lock = PTHREAD_MUTEX_INITIALIZER;
static void bundle_lock_acquire(void) { pthread_mutex_lock(&bundle_inflate_lock); }
static void bundle_lock_release(void) { pthread_mutex_unlock(&bundle_inflate_lock); }
#endif

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

  if (atomic_load_explicit(&bundle_inflated_ready[module_id], memory_order_acquire))
    return &bundle_inflated[module_id];

  bundle_lock_acquire();

  if (atomic_load_explicit(&bundle_inflated_ready[module_id], memory_order_relaxed)) {
    bundle_lock_release();
    return &bundle_inflated[module_id];
  }

  uint8_t *code = malloc(stored->raw_len + 1);
  if (!code || !inflate_builtin_module(stored, code)) {
    free(code);
    bundle_lock_release();
    return NULL;
  }

  code[stored->raw_len] = '\0';
  bundle_inflated[module_id] = *stored;
  bundle_inflated[module_id].code = code;
  bundle_inflated[module_id].code_len = stored->raw_len;

  atomic_store_explicit(&bundle_inflated_ready[module_id], true, memory_order_release);
  bundle_lock_release();

  return &bundle_inflated[module_id];
}
