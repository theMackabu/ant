#ifndef PKG_CACHE_H
#define PKG_CACHE_H

#include "lmdb.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t integrity[64];
  char *path;
  uint64_t unpacked_size;
  uint32_t file_count;
  int64_t cached_at;
} cache_entry_t;

typedef struct __attribute__((packed)) {
  uint64_t unpacked_size;
  uint32_t file_count;
  int64_t cached_at;
  uint32_t path_len;
} cache_serialized_entry_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(cache_serialized_entry_t) == 24,
               "cache_serialized_entry_t must be 24 bytes");
#endif

void cache_delete_tree(const char *path);

typedef struct {
  MDB_env *env;
  MDB_dbi dbi_primary;
  MDB_dbi dbi_secondary;
  MDB_dbi dbi_metadata;
  char *cache_dir;
} cache_db_t;

typedef struct {
  uint32_t index;
  uint32_t file_count;
} cache_batch_hit_t;

typedef struct {
  cache_entry_t entry;
  const char *name;
  const char *version;
} cache_named_entry_t;

cache_db_t *cache_db_open(const char *cache_dir);
void cache_db_close(cache_db_t *db);

bool cache_db_lookup(cache_db_t *db, const uint8_t integrity[64],
                     cache_entry_t *out_entry);
bool cache_db_has_integrity(cache_db_t *db, const uint8_t integrity[64]);

bool cache_db_lookup_by_name(cache_db_t *db, const char *name,
                             const char *version, cache_entry_t *out_entry);

size_t cache_db_batch_lookup(cache_db_t *db, const uint8_t (*integrities)[64],
                             size_t count, cache_batch_hit_t *out_hits);

bool cache_db_insert(cache_db_t *db, const cache_entry_t *entry,
                     const char *name, const char *version);
bool cache_db_batch_insert(cache_db_t *db, const cache_entry_t *entries,
                           size_t count);
bool cache_db_batch_insert_named(cache_db_t *db,
                                 const cache_named_entry_t *entries,
                                 size_t count);

bool cache_db_delete(cache_db_t *db, const uint8_t integrity[64]);

// Allocates dynamic package folder path using cache_dir and hex-encoded
// integrity. Caller must free.
char *cache_db_get_package_path(cache_db_t *db, const uint8_t integrity[64]);

void cache_db_sync(cache_db_t *db);

// Stats retrieval
bool cache_db_stats(cache_db_t *db, size_t *out_entries, size_t *out_db_size,
                    size_t *out_cache_size);

char *cache_db_lookup_metadata(cache_db_t *db, const char *name);
bool cache_db_insert_metadata(cache_db_t *db, const char *name,
                              const char *json_data, size_t json_len);

int32_t cache_db_prune(cache_db_t *db, uint32_t max_age_days);

#endif // PKG_CACHE_H
