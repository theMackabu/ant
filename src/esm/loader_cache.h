#pragma once

#include "types.h"

#include <stdbool.h>
#include <yyjson.h>

struct ant_esm_state {
  struct esm_resolve_cache_entry      *resolve_cache;
  struct esm_base_dir_cache_entry     *base_dir_cache;
  struct esm_package_dir_cache_entry  *package_dir_cache;
  struct esm_package_json_cache_entry *package_json_cache;
  struct esm_path_resolve_cache_entry *path_resolve_cache;

  struct esm_module *modules;
  struct esm_module *last_tla_module;
  
  int module_count;
  int dynamic_import_depth;

  char **active_conditions;
  int active_condition_count;
};

ant_esm_state_t *esm_state(ant_t *js);

char *esm_resolve_cache_get(ant_t *js, const char *key);
void esm_resolve_cache_put(ant_t *js, const char *key, const char *resolved_path);

bool esm_path_resolve_cache_get(ant_t *js, const char *path, char **resolved_path_out);
void esm_path_resolve_cache_put(ant_t *js, const char *path, const char *resolved_path);

bool esm_base_dir_cache_get(ant_t *js, const char *base_path, char **base_dir_out);
void esm_base_dir_cache_put(ant_t *js, const char *base_path, const char *base_dir);

bool esm_package_dir_cache_get(ant_t *js, const char *start_dir, const char *package_name, char **package_dir_out);
void esm_package_dir_cache_put(ant_t *js, const char *start_dir, const char *package_name, const char *package_dir);

yyjson_doc *esm_package_json_cache_read(ant_t *js, const char *pkg_json_path, bool *out_owned);
void esm_loader_cache_cleanup(ant_t *js);
