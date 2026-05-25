#pragma once

#include <stdbool.h>
#include <yyjson.h>

char *esm_resolve_cache_get(const char *key);
void esm_resolve_cache_put(const char *key, const char *resolved_path);

bool esm_path_resolve_cache_get(const char *path, char **resolved_path_out);
void esm_path_resolve_cache_put(const char *path, const char *resolved_path);

bool esm_base_dir_cache_get(const char *base_path, char **base_dir_out);
void esm_base_dir_cache_put(const char *base_path, const char *base_dir);

bool esm_package_dir_cache_get(const char *start_dir, const char *package_name, char **package_dir_out);
void esm_package_dir_cache_put(const char *start_dir, const char *package_name, const char *package_dir);

yyjson_doc *esm_package_json_cache_read(const char *pkg_json_path);
void esm_loader_cache_cleanup(void);
