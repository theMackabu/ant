#include <compat.h> // IWYU pragma: keep

#include "loader_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uthash.h>

typedef struct esm_package_json_cache_entry {
  char *path;
  yyjson_doc *doc;
  UT_hash_handle hh;
} esm_package_json_cache_entry_t;

typedef struct esm_resolve_cache_entry {
  char *key;
  char *resolved_path;
  UT_hash_handle hh;
} esm_resolve_cache_entry_t;

typedef struct esm_path_resolve_cache_entry {
  char *key;
  char *resolved_path;
  bool found;
  UT_hash_handle hh;
} esm_path_resolve_cache_entry_t;

typedef struct esm_base_dir_cache_entry {
  char *key;
  char *base_dir;
  UT_hash_handle hh;
} esm_base_dir_cache_entry_t;

typedef struct esm_package_dir_cache_entry {
  char *key;
  char *package_dir;
  bool found;
  UT_hash_handle hh;
} esm_package_dir_cache_entry_t;

static esm_package_json_cache_entry_t *global_package_json_cache = NULL;
static esm_resolve_cache_entry_t *global_resolve_cache = NULL;
static esm_path_resolve_cache_entry_t *global_path_resolve_cache = NULL;
static esm_base_dir_cache_entry_t *global_base_dir_cache = NULL;
static esm_package_dir_cache_entry_t *global_package_dir_cache = NULL;

static char *esm_make_pair_cache_key(const char *left, const char *right) {
  size_t left_len = left ? strlen(left) : 0;
  size_t right_len = right ? strlen(right) : 0;
  size_t len = snprintf(NULL, 0, "%zu:%s:%zu:%s",
                        left_len,
                        left ? left : "",
                        right_len,
                        right ? right : "");
  char *key = malloc(len + 1u);
  if (!key) return NULL;
  snprintf(key, len + 1u, "%zu:%s:%zu:%s",
           left_len,
           left ? left : "",
           right_len,
           right ? right : "");
  return key;
}

char *esm_resolve_cache_get(const char *key) {
  if (!key) return NULL;

  esm_resolve_cache_entry_t *entry = NULL;
  HASH_FIND_STR(global_resolve_cache, key, entry);
  return entry ? strdup(entry->resolved_path) : NULL;
}

void esm_resolve_cache_put(const char *key, const char *resolved_path) {
  if (!key || !resolved_path) return;

  esm_resolve_cache_entry_t *existing = NULL;
  HASH_FIND_STR(global_resolve_cache, key, existing);
  if (existing) return;

  esm_resolve_cache_entry_t *entry = (esm_resolve_cache_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) return;

  entry->key = strdup(key);
  entry->resolved_path = strdup(resolved_path);
  if (!entry->key || !entry->resolved_path) {
    free(entry->key);
    free(entry->resolved_path);
    free(entry);
    return;
  }

  HASH_ADD_STR(global_resolve_cache, key, entry);
}

bool esm_path_resolve_cache_get(const char *path, char **resolved_path_out) {
  if (resolved_path_out) *resolved_path_out = NULL;
  if (!path) return false;

  esm_path_resolve_cache_entry_t *entry = NULL;
  HASH_FIND_STR(global_path_resolve_cache, path, entry);
  if (!entry) return false;

  if (resolved_path_out && entry->found) *resolved_path_out = strdup(entry->resolved_path);
  return true;
}

void esm_path_resolve_cache_put(const char *path, const char *resolved_path) {
  if (!path) return;

  esm_path_resolve_cache_entry_t *existing = NULL;
  HASH_FIND_STR(global_path_resolve_cache, path, existing);
  if (existing) return;

  esm_path_resolve_cache_entry_t *entry = (esm_path_resolve_cache_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) return;

  entry->key = strdup(path);
  entry->resolved_path = resolved_path ? strdup(resolved_path) : NULL;
  entry->found = resolved_path != NULL;
  if (!entry->key || (resolved_path && !entry->resolved_path)) {
    free(entry->key);
    free(entry->resolved_path);
    free(entry);
    return;
  }

  HASH_ADD_STR(global_path_resolve_cache, key, entry);
}

bool esm_base_dir_cache_get(const char *base_path, char **base_dir_out) {
  if (base_dir_out) *base_dir_out = NULL;
  if (!base_path) return false;

  esm_base_dir_cache_entry_t *entry = NULL;
  HASH_FIND_STR(global_base_dir_cache, base_path, entry);
  if (!entry) return false;

  if (base_dir_out) *base_dir_out = strdup(entry->base_dir);
  return true;
}

void esm_base_dir_cache_put(const char *base_path, const char *base_dir) {
  if (!base_path || !base_dir) return;

  esm_base_dir_cache_entry_t *existing = NULL;
  HASH_FIND_STR(global_base_dir_cache, base_path, existing);
  if (existing) return;

  esm_base_dir_cache_entry_t *entry = (esm_base_dir_cache_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) return;

  entry->key = strdup(base_path);
  entry->base_dir = strdup(base_dir);
  if (!entry->key || !entry->base_dir) {
    free(entry->key);
    free(entry->base_dir);
    free(entry);
    return;
  }

  HASH_ADD_STR(global_base_dir_cache, key, entry);
}

bool esm_package_dir_cache_get(const char *start_dir, const char *package_name, char **package_dir_out) {
  if (package_dir_out) *package_dir_out = NULL;
  char *key = esm_make_pair_cache_key(start_dir, package_name);
  if (!key) return false;

  esm_package_dir_cache_entry_t *entry = NULL;
  HASH_FIND_STR(global_package_dir_cache, key, entry);
  free(key);
  if (!entry) return false;

  if (package_dir_out && entry->found) *package_dir_out = strdup(entry->package_dir);
  return true;
}

void esm_package_dir_cache_put(const char *start_dir, const char *package_name, const char *package_dir) {
  char *key = esm_make_pair_cache_key(start_dir, package_name);
  if (!key) return;

  esm_package_dir_cache_entry_t *existing = NULL;
  HASH_FIND_STR(global_package_dir_cache, key, existing);
  if (existing) {
    free(key);
    return;
  }

  esm_package_dir_cache_entry_t *entry = (esm_package_dir_cache_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) {
    free(key);
    return;
  }

  entry->key = key;
  entry->package_dir = package_dir ? strdup(package_dir) : NULL;
  entry->found = package_dir != NULL;
  if (package_dir && !entry->package_dir) {
    free(entry->key);
    free(entry);
    return;
  }

  HASH_ADD_STR(global_package_dir_cache, key, entry);
}

yyjson_doc *esm_package_json_cache_read(const char *pkg_json_path) {
  if (!pkg_json_path || !pkg_json_path[0]) return NULL;

  esm_package_json_cache_entry_t *entry = NULL;
  HASH_FIND_STR(global_package_json_cache, pkg_json_path, entry);
  if (entry) return entry->doc;

  entry = (esm_package_json_cache_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) return yyjson_read_file(pkg_json_path, 0, NULL, NULL);

  entry->path = strdup(pkg_json_path);
  if (!entry->path) {
    free(entry);
    return yyjson_read_file(pkg_json_path, 0, NULL, NULL);
  }

  entry->doc = yyjson_read_file(pkg_json_path, 0, NULL, NULL);
  if (!entry->doc) {
    free(entry->path);
    free(entry);
    return NULL;
  }
  HASH_ADD_STR(global_package_json_cache, path, entry);
  return entry->doc;
}

void esm_loader_cache_cleanup(void) {
  esm_package_json_cache_entry_t *pkg_current, *pkg_tmp;
  HASH_ITER(hh, global_package_json_cache, pkg_current, pkg_tmp) {
    HASH_DEL(global_package_json_cache, pkg_current);
    free(pkg_current->path);
    if (pkg_current->doc) yyjson_doc_free(pkg_current->doc);
    free(pkg_current);
  }

  esm_resolve_cache_entry_t *resolve_current, *resolve_tmp;
  HASH_ITER(hh, global_resolve_cache, resolve_current, resolve_tmp) {
    HASH_DEL(global_resolve_cache, resolve_current);
    free(resolve_current->key);
    free(resolve_current->resolved_path);
    free(resolve_current);
  }

  esm_path_resolve_cache_entry_t *path_current, *path_tmp;
  HASH_ITER(hh, global_path_resolve_cache, path_current, path_tmp) {
    HASH_DEL(global_path_resolve_cache, path_current);
    free(path_current->key);
    free(path_current->resolved_path);
    free(path_current);
  }

  esm_base_dir_cache_entry_t *base_current, *base_tmp;
  HASH_ITER(hh, global_base_dir_cache, base_current, base_tmp) {
    HASH_DEL(global_base_dir_cache, base_current);
    free(base_current->key);
    free(base_current->base_dir);
    free(base_current);
  }

  esm_package_dir_cache_entry_t *dir_current, *dir_tmp;
  HASH_ITER(hh, global_package_dir_cache, dir_current, dir_tmp) {
    HASH_DEL(global_package_dir_cache, dir_current);
    free(dir_current->key);
    free(dir_current->package_dir);
    free(dir_current);
  }
}
