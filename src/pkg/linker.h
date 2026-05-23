#ifndef PKG_LINKER_H
#define PKG_LINKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t files_linked;
  uint32_t files_copied;
  uint32_t files_cloned;
  uint64_t bytes_linked;
  uint64_t bytes_copied;
  uint32_t dirs_created;
  uint32_t bins_linked;
  uint32_t packages_installed;
  uint32_t packages_skipped;
} linker_stats_t;

typedef struct {
  const char *cache_path;
  const char *node_modules_path;
  const char *name;
  const char *parent_path;
  uint32_t file_count;
  bool has_bin;
} package_link_t;

typedef struct linker linker_t;

linker_t *linker_init(void);
void linker_deinit(linker_t *self);
bool linker_set_node_modules_path(linker_t *self, const char *path);
bool linker_link_package(linker_t *self, const package_link_t *pkg);
linker_stats_t linker_get_stats(const linker_t *self);

#endif // PKG_LINKER_H
