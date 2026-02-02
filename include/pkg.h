#ifndef PKG_H
#define PKG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
  PKG_OK = 0,
  PKG_OUT_OF_MEMORY = -1,
  PKG_INVALID_LOCKFILE = -2,
  PKG_IO_ERROR = -3,
  PKG_NETWORK_ERROR = -4,
  PKG_CACHE_ERROR = -5,
  PKG_EXTRACT_ERROR = -6,
  PKG_RESOLVE_ERROR = -7,
  PKG_INVALID_ARGUMENT = -8,
  PKG_NOT_FOUND = -9,
  PKG_INTEGRITY_MISMATCH = -10,
} pkg_error_t;

typedef enum {
  PKG_PHASE_RESOLVING = 0,
  PKG_PHASE_FETCHING = 1,
  PKG_PHASE_EXTRACTING = 2,
  PKG_PHASE_LINKING = 3,
  PKG_PHASE_CACHING = 4,
  PKG_PHASE_POSTINSTALL = 5,
} pkg_phase_t;

typedef void (*pkg_progress_cb)(
  void *user_data,
  pkg_phase_t phase,
  uint32_t current,
  uint32_t total,
  const char *message
);

typedef struct {
  const char *cache_dir;
  const char *registry_url;
  uint32_t max_connections;
  pkg_progress_cb progress_callback;
  void *user_data;
  bool verbose;
} pkg_options_t;

typedef struct pkg_context pkg_context_t;

const char *pkg_error_string(const pkg_context_t *ctx);

pkg_context_t *pkg_init(const pkg_options_t *options);

pkg_error_t pkg_install(
  pkg_context_t *ctx,
  const char *package_json_path,
  const char *lockfile_path,
  const char *node_modules_path
);

pkg_error_t pkg_resolve_and_install(
  pkg_context_t *ctx,
  const char *package_json_path,
  const char *lockfile_path,
  const char *node_modules_path
);

pkg_error_t pkg_add(
  pkg_context_t *ctx,
  const char *package_json_path,
  const char *package_spec,
  bool dev
);

pkg_error_t pkg_remove(
  pkg_context_t *ctx,
  const char *package_json_path,
  const char *package_name
);

void pkg_free(pkg_context_t *ctx);
void pkg_cache_sync(pkg_context_t *ctx);

typedef struct {
  uint64_t total_size;
  uint64_t db_size;
  uint32_t package_count;
} pkg_cache_stats_t;

pkg_error_t pkg_cache_stats(pkg_context_t *ctx, pkg_cache_stats_t *out);

int32_t pkg_cache_prune(pkg_context_t *ctx, uint32_t max_age_days);

typedef struct {
  uint32_t package_count;
  uint32_t cache_hits;
  uint32_t cache_misses;
  uint32_t files_linked;
  uint32_t files_copied;
  uint32_t packages_installed;
  uint32_t packages_skipped;
  uint64_t elapsed_ms;
} pkg_install_result_t;

typedef struct {
  const char *name;
  const char *version;
  bool direct;
} pkg_added_package_t;

typedef struct {
  const char *name;
  const char *script;
} pkg_lifecycle_script_t;

uint32_t pkg_get_added_count(const pkg_context_t *ctx);

pkg_error_t pkg_discover_lifecycle_scripts(
  pkg_context_t *ctx,
  const char *node_modules_path
);

uint32_t pkg_get_lifecycle_script_count(const pkg_context_t *ctx);

pkg_error_t pkg_get_lifecycle_script(
  const pkg_context_t *ctx,
  uint32_t index,
  pkg_lifecycle_script_t *out
);

pkg_error_t pkg_run_postinstall(
  pkg_context_t *ctx,
  const char *node_modules_path,
  const char **package_names,
  uint32_t count
);

pkg_error_t pkg_add_trusted_dependencies(
  const char *package_json_path,
  const char **package_names,
  uint32_t count
);

pkg_error_t pkg_get_install_result(
  pkg_context_t *ctx,
  pkg_install_result_t *out
);

pkg_error_t pkg_get_added_package(
  const pkg_context_t *ctx,
  uint32_t index,
  pkg_added_package_t *out
);

int pkg_get_bin_path(
  const char *node_modules_path,
  const char *bin_name,
  char *out_path,
  size_t out_path_len
);

typedef void (*pkg_bin_callback)(
  const char *name, 
  void *user_data
);

int pkg_list_bins(
  const char *node_modules_path,
  pkg_bin_callback callback,
  void *user_data
);

int pkg_list_package_bins(
  const char *node_modules_path,
  const char *package_name,
  pkg_bin_callback callback,
  void *user_data
);

int pkg_get_script(
  const char *package_json_path,
  const char *script_name,
  char *out_script,
  size_t out_script_len
);

typedef struct {
  int exit_code;
  int signal;
} pkg_script_result_t;

pkg_error_t pkg_run_script(
  const char *package_json_path,
  const char *script_name,
  const char *node_modules_path,
  const char *extra_args,
  pkg_script_result_t *result
);

typedef void (*pkg_script_callback)(
  const char *name,
  const char *command,
  void *user_data
);

int pkg_list_scripts(
  const char *package_json_path,
  pkg_script_callback callback,
  void *user_data
);

typedef struct {
  uint8_t peer: 1;
  uint8_t dev: 1;
  uint8_t optional: 1;
  uint8_t direct: 1;
  uint8_t _reserved: 4;
} pkg_dep_type_t;

typedef void (*pkg_why_callback)(
  const char *name,
  const char *version,
  const char *constraint,
  pkg_dep_type_t dep_type,
  void *user_data
);

typedef struct {
  char target_version[64];
  bool found;
  bool is_peer;
  bool is_dev;
  bool is_direct;
} pkg_why_info_t;

int pkg_why_info(
  const char *lockfile_path,
  const char *package_name,
  pkg_why_info_t *out
);

int pkg_why(
  const char *lockfile_path,
  const char *package_name,
  pkg_why_callback callback,
  void *user_data
);

typedef struct {
  const char *name;
  const char *version;
  const char *description;
  const char *license;
  const char *homepage;
  const char *tarball;
  const char *shasum;
  const char *integrity;
  const char *keywords;
  const char *published;
  uint32_t dep_count;
  uint32_t version_count;
  uint64_t unpacked_size;
} pkg_info_t;

typedef struct {
  const char *tag;
  const char *version;
} pkg_dist_tag_t;

typedef struct {
  const char *name;
  const char *email;
} pkg_maintainer_t;

pkg_error_t pkg_info(
  pkg_context_t *ctx,
  const char *package_spec,
  pkg_info_t *out
);

uint32_t pkg_info_dist_tag_count(const pkg_context_t *ctx);
pkg_error_t pkg_info_get_dist_tag(const pkg_context_t *ctx, uint32_t index, pkg_dist_tag_t *out);

uint32_t pkg_info_maintainer_count(const pkg_context_t *ctx);
pkg_error_t pkg_info_get_maintainer(const pkg_context_t *ctx, uint32_t index, pkg_maintainer_t *out);

typedef struct {
  const char *name;
  const char *version;
} pkg_dependency_t;

uint32_t pkg_info_dependency_count(const pkg_context_t *ctx);
pkg_error_t pkg_info_get_dependency(const pkg_context_t *ctx, uint32_t index, pkg_dependency_t *out);

pkg_error_t pkg_exec_temp(
  pkg_context_t *ctx,
  const char *package_spec,
  char *out_bin_path,
  size_t out_bin_path_len
);

pkg_error_t pkg_add_global(
  pkg_context_t *ctx,
  const char *package_spec
);

pkg_error_t pkg_remove_global(
  pkg_context_t *ctx,
  const char *package_name
);

typedef void (*pkg_global_list_callback)(
  const char *name,
  const char *version,
  void *user_data
);

pkg_error_t pkg_list_global(
  pkg_context_t *ctx,
  pkg_global_list_callback callback,
  void *user_data
);

#endif
