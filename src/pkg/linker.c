#include "linker.h"
#include "cache.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <yyjson.h>

#ifdef __APPLE__
#include <sys/clonefile.h>
// Declare clonefileat just in case it is not present in some SDK versions
extern int clonefileat(int src_dirfd, const char *src, int dst_dirfd,
                       const char *dst, uint32_t flags);
#endif

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir(path, mode) _mkdir(path)
#define unlink _unlink
#define symlink(target, link) (!CreateSymbolicLinkA(link, target, 0))
#endif

struct linker {
  char *node_modules_path;
  int node_modules_fd;
  int bin_fd;

  _Atomic uint32_t files_linked;
  _Atomic uint32_t files_copied;
  _Atomic uint32_t files_cloned;
  _Atomic uint64_t bytes_linked;
  _Atomic uint64_t bytes_copied;
  _Atomic uint32_t dirs_created;
  _Atomic uint32_t bins_linked;
  _Atomic uint32_t packages_installed;
  _Atomic uint32_t packages_skipped;

  _Atomic bool cross_device;
};

static void make_path(const char *base, const char *rel_path) {
  char full_path[4096];
  snprintf(full_path, sizeof(full_path), "%s/%s", base, rel_path);

  char *p = strchr(full_path + strlen(base) + 1, '/');
  while (p) {
    *p = '\0';
    mkdir(full_path, 0755);
    *p = '/';
    p = strchr(p + 1, '/');
  }
  mkdir(full_path, 0755);
}

linker_t *linker_init(void) {
  linker_t *self = calloc(1, sizeof(linker_t));
  if (!self)
    return NULL;

  self->node_modules_fd = -1;
  self->bin_fd = -1;
  atomic_init(&self->cross_device, false);

  return self;
}

void linker_deinit(linker_t *self) {
  if (!self)
    return;
  if (self->node_modules_fd != -1)
    close(self->node_modules_fd);
  if (self->bin_fd != -1)
    close(self->bin_fd);
  free(self->node_modules_path);
  free(self);
}

bool linker_set_node_modules_path(linker_t *self, const char *path) {
  if (self->node_modules_fd != -1) {
    close(self->node_modules_fd);
    self->node_modules_fd = -1;
  }
  if (self->bin_fd != -1) {
    close(self->bin_fd);
    self->bin_fd = -1;
  }
  free(self->node_modules_path);
  self->node_modules_path = strdup(path);
  if (!self->node_modules_path)
    return false;

  // Make node_modules
  mkdir(self->node_modules_path, 0755);
#ifndef _WIN32
  self->node_modules_fd = open(self->node_modules_path, O_RDONLY | O_DIRECTORY);
#endif

  // Make node_modules/.bin
  char bin_path[4096];
  snprintf(bin_path, sizeof(bin_path), "%s/.bin", self->node_modules_path);
  mkdir(bin_path, 0755);
#ifndef _WIN32
  self->bin_fd = open(bin_path, O_RDONLY | O_DIRECTORY);
#endif

  return true;
}

static char *read_package_version(const char *dir_path) {
  char json_path[4096];
  snprintf(json_path, sizeof(json_path), "%s/package.json", dir_path);

  size_t size = 0;
  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_file(json_path, 0, NULL, &err);
  if (!doc)
    return NULL;

  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *ver_val = yyjson_obj_get(root, "version");
  char *ver = NULL;
  if (yyjson_is_str(ver_val)) {
    ver = strdup(yyjson_get_str(ver_val));
  }
  yyjson_doc_free(doc);
  return ver;
}

static uint32_t count_files_recursive(const char *path) {
  DIR *dir = opendir(path);
  if (!dir)
    return 0;
  uint32_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char sub_path[4096];
    snprintf(sub_path, sizeof(sub_path), "%s/%s", path, entry->d_name);

    struct stat st;
    if (stat(sub_path, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        count += count_files_recursive(sub_path);
      } else if (S_ISREG(st.st_mode)) {
        count++;
      }
    }
  }
  closedir(dir);
  return count;
}

static bool installed_file_count_matches(const char *path, uint32_t expected) {
  if (expected == 0)
    return true;
  return count_files_recursive(path) >= expected;
}

static bool copy_file(linker_t *self, const char *src, const char *dst,
                      mode_t mode) {
  FILE *s = fopen(src, "rb");
  if (!s)
    return false;

  FILE *d = fopen(dst, "wb");
  if (!d) {
    fclose(s);
    return false;
  }

  char buf[64 * 1024];
  size_t bytes;
  uint64_t total = 0;
  while ((bytes = fread(buf, 1, sizeof(buf), s)) > 0) {
    if (fwrite(buf, 1, bytes, d) < bytes) {
      fclose(s);
      fclose(d);
      return false;
    }
    total += bytes;
  }

  fclose(s);
  fclose(d);

#ifndef _WIN32
  chmod(dst, mode);
#endif

  atomic_fetch_add_explicit(&self->bytes_copied, total, memory_order_relaxed);
  return true;
}

static bool link_file(linker_t *self, int src_dir_fd, const char *src_rel,
                      const char *src_abs, int dst_dir_fd, const char *dst_rel,
                      const char *dst_abs) {
  struct stat st;
  if (stat(src_abs, &st) != 0)
    return false;

  unlink(dst_abs);

#ifndef _WIN32
  // 1. Try linkat (hardlink) if on POSIX and not cross device
  if (!atomic_load_explicit(&self->cross_device, memory_order_acquire)) {
    if (src_dir_fd != -1 && dst_dir_fd != -1) {
      if (linkat(src_dir_fd, src_rel, dst_dir_fd, dst_rel, 0) == 0) {
        atomic_fetch_add_explicit(&self->files_linked, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&self->bytes_linked, st.st_size,
                                  memory_order_relaxed);
        return true;
      }
      if (errno == EXDEV) {
        atomic_store_explicit(&self->cross_device, true, memory_order_release);
      }
    } else {
      if (link(src_abs, dst_abs) == 0) {
        atomic_fetch_add_explicit(&self->files_linked, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&self->bytes_linked, st.st_size,
                                  memory_order_relaxed);
        return true;
      }
      if (errno == EXDEV) {
        atomic_store_explicit(&self->cross_device, true, memory_order_release);
      }
    }
  }

  // 2. Try clonefileat (copy-on-write clone) if on macOS
#ifdef __APPLE__
  if (src_dir_fd != -1 && dst_dir_fd != -1) {
    if (clonefileat(src_dir_fd, src_rel, dst_dir_fd, dst_rel, 0) == 0) {
      atomic_fetch_add_explicit(&self->files_cloned, 1, memory_order_relaxed);
      return true;
    }
  }
#endif
#endif // _WIN32

  // 3. Fallback to full copy
  if (copy_file(self, src_abs, dst_abs, st.st_mode)) {
    atomic_fetch_add_explicit(&self->files_copied, 1, memory_order_relaxed);
    return true;
  }

  return false;
}

static bool link_directory_recursive(linker_t *self, const char *src_base,
                                     const char *src_rel, const char *dst_base,
                                     const char *dst_rel) {
  char src_path[4096];
  char dst_path[4096];

  if (src_rel && strlen(src_rel) > 0) {
    snprintf(src_path, sizeof(src_path), "%s/%s", src_base, src_rel);
    snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_base, dst_rel);
  } else {
    snprintf(src_path, sizeof(src_path), "%s", src_base);
    snprintf(dst_path, sizeof(dst_path), "%s", dst_base);
  }

  DIR *dir = opendir(src_path);
  if (!dir)
    return false;

#ifndef _WIN32
  int src_dir_fd = dirfd(dir);
  int dst_dir_fd = open(dst_path, O_RDONLY | O_DIRECTORY);
#else
  int src_dir_fd = -1;
  int dst_dir_fd = -1;
#endif

  struct dirent *entry;
  bool success = true;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char sub_src_rel[4096];
    char sub_dst_rel[4096];
    if (src_rel && strlen(src_rel) > 0) {
      snprintf(sub_src_rel, sizeof(sub_src_rel), "%s/%s", src_rel,
               entry->d_name);
      snprintf(sub_dst_rel, sizeof(sub_dst_rel), "%s/%s", dst_rel,
               entry->d_name);
    } else {
      snprintf(sub_src_rel, sizeof(sub_src_rel), "%s", entry->d_name);
      snprintf(sub_dst_rel, sizeof(sub_dst_rel), "%s", entry->d_name);
    }

    char sub_src_abs[4096];
    char sub_dst_abs[4096];
    snprintf(sub_src_abs, sizeof(sub_src_abs), "%s/%s", src_base, sub_src_rel);
    snprintf(sub_dst_abs, sizeof(sub_dst_abs), "%s/%s", dst_base, sub_dst_rel);

    struct stat st;
    if (stat(sub_src_abs, &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      mkdir(sub_dst_abs, 0755);
      atomic_fetch_add_explicit(&self->dirs_created, 1, memory_order_relaxed);
      if (!link_directory_recursive(self, src_base, sub_src_rel, dst_base,
                                    sub_dst_rel)) {
        success = false;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (!link_file(self, src_dir_fd, entry->d_name, sub_src_abs, dst_dir_fd,
                     entry->d_name, sub_dst_abs)) {
        success = false;
      }
    } else if (S_ISLNK(st.st_mode)) {
      char link_buf[4096];
      ssize_t len = readlink(sub_src_abs, link_buf, sizeof(link_buf) - 1);
      if (len != -1) {
        link_buf[len] = '\0';
        unlink(sub_dst_abs);
        symlink(link_buf, sub_dst_abs);
      }
    }
  }

  closedir(dir);
#ifndef _WIN32
  if (dst_dir_fd != -1)
    close(dst_dir_fd);
#endif
  return success;
}

static bool create_bin_symlink(linker_t *self, const char *pkg_name,
                               const char *cmd_name, const char *bin_path) {
  const char *normalized_path = bin_path;
  if (strncmp(normalized_path, "./", 2) == 0) {
    normalized_path += 2;
  }

  char target[4096];
  snprintf(target, sizeof(target), "../%s/%s", pkg_name, normalized_path);

  char link_path[4096];
  snprintf(link_path, sizeof(link_path), "%s/.bin/%s", self->node_modules_path,
           cmd_name);

  unlink(link_path);
  if (symlink(target, link_path) != 0) {
    return false;
  }

  atomic_fetch_add_explicit(&self->bins_linked, 1, memory_order_relaxed);
  return true;
}

static void link_binaries(linker_t *self, const char *pkg_name) {
  char pkg_path[4096];
  snprintf(pkg_path, sizeof(pkg_path), "%s/%s", self->node_modules_path,
           pkg_name);

  char pkg_json_path[4096];
  snprintf(pkg_json_path, sizeof(pkg_json_path), "%s/package.json", pkg_path);

  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_file(pkg_json_path, 0, NULL, &err);
  if (!doc)
    return;

  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *bin_val = yyjson_obj_get(root, "bin");
  if (yyjson_is_obj(bin_val)) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(bin_val, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
      yyjson_val *val = yyjson_obj_iter_get_val(key);
      const char *cmd_name = yyjson_get_str(key);
      const char *bin_path = yyjson_get_str(val);
      if (cmd_name && bin_path) {
        create_bin_symlink(self, pkg_name, cmd_name, bin_path);
      }
    }
  } else if (yyjson_is_str(bin_val)) {
    const char *bin_path = yyjson_get_str(bin_val);
    const char *simple_name = pkg_name;
    const char *slash = strchr(pkg_name, '/');
    if (slash) {
      simple_name = slash + 1;
    }
    if (bin_path) {
      create_bin_symlink(self, pkg_name, simple_name, bin_path);
    }
  }
  yyjson_doc_free(doc);
}

bool linker_link_package(linker_t *self, const package_link_t *pkg) {
  if (!self->node_modules_path)
    return false;

  char install_path[4096];
  if (pkg->parent_path && strlen(pkg->parent_path) > 0) {
    snprintf(install_path, sizeof(install_path), "%s/%s/node_modules/%s",
             self->node_modules_path, pkg->parent_path, pkg->name);
  } else {
    snprintf(install_path, sizeof(install_path), "%s/%s",
             self->node_modules_path, pkg->name);
  }

  char *source_version = read_package_version(pkg->cache_path);
  char *installed_version = read_package_version(install_path);

  bool should_skip = false;
  if (source_version && installed_version) {
    should_skip = (strcmp(source_version, installed_version) == 0) &&
                  installed_file_count_matches(install_path, pkg->file_count);
  }

  free(source_version);
  free(installed_version);

  if (should_skip) {
    atomic_fetch_add_explicit(&self->packages_skipped, 1, memory_order_relaxed);
    return true;
  }

  // Clean old path if it exists
  // For POSIX we can just recursively delete or deleteTree
  // But wait, standard deleteTree in C can be implemented by traversing and
  // unlinking/rmdir Let's implement a quick recursive delete helper in C Wait,
  // let's see if we already have it in cache.c or cache.h: cache_delete_tree!
  // Yes! cache_delete_tree is declared in cache.h! We can use that!
  cache_delete_tree(install_path);

  // Make destination path
  make_path(self->node_modules_path,
            pkg->parent_path && strlen(pkg->parent_path) > 0
                ? (char *)install_path + strlen(self->node_modules_path) + 1
                : pkg->name);
  atomic_fetch_add_explicit(&self->dirs_created, 1, memory_order_relaxed);

  if (!link_directory_recursive(self, pkg->cache_path, "", install_path, "")) {
    return false;
  }

  if (!pkg->parent_path && pkg->has_bin) {
    link_binaries(self, pkg->name);
  }

  atomic_fetch_add_explicit(&self->packages_installed, 1, memory_order_relaxed);
  return true;
}

linker_stats_t linker_get_stats(const linker_t *self) {
  linker_stats_t s = {
      .files_linked =
          atomic_load_explicit(&self->files_linked, memory_order_acquire),
      .files_copied =
          atomic_load_explicit(&self->files_copied, memory_order_acquire),
      .files_cloned =
          atomic_load_explicit(&self->files_cloned, memory_order_acquire),
      .bytes_linked =
          atomic_load_explicit(&self->bytes_linked, memory_order_acquire),
      .bytes_copied =
          atomic_load_explicit(&self->bytes_copied, memory_order_acquire),
      .dirs_created =
          atomic_load_explicit(&self->dirs_created, memory_order_acquire),
      .bins_linked =
          atomic_load_explicit(&self->bins_linked, memory_order_acquire),
      .packages_installed =
          atomic_load_explicit(&self->packages_installed, memory_order_acquire),
      .packages_skipped =
          atomic_load_explicit(&self->packages_skipped, memory_order_acquire),
  };
  return s;
}
