#ifndef PKG_RESOLVER_H
#define PKG_RESOLVER_H

#include "cache.h"
#include "fetcher.h"
#include "intern.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint64_t major;
  uint64_t minor;
  uint64_t patch;
  char *prerelease;
  char *build;
} semver_version_t;

typedef enum {
  CONSTRAINT_EXACT,
  CONSTRAINT_CARET,
  CONSTRAINT_TILDE,
  CONSTRAINT_GTE,
  CONSTRAINT_GT,
  CONSTRAINT_LTE,
  CONSTRAINT_LT,
  CONSTRAINT_ANY
} constraint_kind_t;

typedef struct {
  constraint_kind_t kind;
  semver_version_t version;
} semver_constraint_t;

typedef struct {
  uint8_t peer : 1;
  uint8_t optional : 1;
} resolved_dep_flags_t;

typedef struct {
  interned_string_t name;
  char *constraint;
  resolved_dep_flags_t flags;
} resolved_dep_t;

typedef struct resolved_package {
  interned_string_t name;
  semver_version_t version;
  uint8_t integrity[64];
  char *tarball_url;
  resolved_dep_t *dependencies;
  uint32_t dep_count;
  uint32_t dep_capacity;
  uint32_t depth;
  bool direct;
  char *parent_path;
  bool has_bin;
} resolved_package_t;

#include <uthash.h>

typedef struct {
  char *name;
  char *constraint;
} name_value_t;

typedef struct {
  semver_version_t version;
  char *version_str;
  uint8_t integrity[64];
  char *tarball_url;

  name_value_t *dependencies;
  uint32_t dep_count;
  name_value_t *optional_dependencies;
  uint32_t opt_dep_count;
  name_value_t *peer_dependencies;
  uint32_t peer_dep_count;
  char **peer_dependencies_meta;
  uint32_t peer_meta_count;

  char *os;
  char *cpu;
  char *libc;
  name_value_t *bin;
  uint32_t bin_count;
} version_info_t;

typedef struct {
  char *name;
  version_info_t *versions;
  uint32_t version_count;
  bool has_latest;
  semver_version_t dist_tag_latest;
} package_metadata_t;

typedef struct {
  char *key;
  package_metadata_t value;
  UT_hash_handle hh;
} metadata_entry_t;

typedef struct resolver resolver_t;

resolver_t *resolver_init(string_pool_t *pool, cache_db_t *cache_db,
                          fetcher_t *http);
void resolver_deinit(resolver_t *self);

void resolver_set_on_package_resolved(
    resolver_t *self,
    void (*callback)(const resolved_package_t *pkg, void *user_data),
    void *user_data);

bool resolver_resolve_from_package_json(resolver_t *self, const char *path);
bool resolver_write_lockfile(resolver_t *self, const char *path);

// Helper functions for Semver
bool semver_parse(const char *str, semver_version_t *out);
int semver_compare(const semver_version_t *a, const semver_version_t *b);
void semver_free(semver_version_t *v);
char *semver_format(const semver_version_t *v);

bool semver_constraint_parse(const char *str, semver_constraint_t *out);
bool semver_constraint_satisfies(const semver_constraint_t *constraint,
                                 const semver_version_t *version);
void semver_constraint_free(semver_constraint_t *c);

void resolver_add_metadata_to_cache(resolver_t *self, const char *name,
                                    package_metadata_t meta);
package_metadata_t parse_metadata_json(const char *name, const char *json_data,
                                       size_t json_len);
const version_info_t *
select_best_version(const package_metadata_t *metadata,
                    const semver_constraint_t *constraint);

#endif // PKG_RESOLVER_H
