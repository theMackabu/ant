#include "sandbox/host.h"
#include "../src/sandbox/backends/darwin/virtio_9p.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__aarch64__) && defined(__APPLE__)

static void init_dev(ant_hvf_9p_device_t *dev, const char *root) {
  memset(dev, 0, sizeof(*dev));
  dev->root = root;
}

static void test_relative_path_policy(void) {
  char root_template[] = "/tmp/ant-9p-policy.XXXXXX";
  char *root_tmp = mkdtemp(root_template);
  assert(root_tmp);

  char root[4096];
  assert(realpath(root_tmp, root));

  ant_hvf_9p_device_t dev;
  init_dev(&dev, root);

  char host[4096];
  assert(ant_hvf_9p_existing_path(&dev, "", host, sizeof(host)) == 0);
  assert(strcmp(host, root) == 0);
  assert(ant_hvf_9p_host_path(&dev, "../x", host, sizeof(host)) == -ENOENT);
  assert(ant_hvf_9p_host_path(&dev, "/x", host, sizeof(host)) == -ENOENT);
  assert(ant_hvf_9p_host_path(&dev, "a//b", host, sizeof(host)) == -ENOENT);
  assert(ant_hvf_9p_host_path(&dev, "a/./b", host, sizeof(host)) == -ENOENT);
  assert(ant_hvf_9p_host_path(&dev, "a/../b", host, sizeof(host)) == -ENOENT);

  rmdir(root);
}

static void test_symlink_escape_policy(void) {
  char root_template[] = "/tmp/ant-9p-root.XXXXXX";
  char external_template[] = "/tmp/ant-9p-external.XXXXXX";
  char *root_tmp = mkdtemp(root_template);
  char *external_tmp = mkdtemp(external_template);
  assert(root_tmp);
  assert(external_tmp);

  char root[4096];
  char external[4096];
  assert(realpath(root_tmp, root));
  assert(realpath(external_tmp, external));

  char link_path[4096];
  int written = snprintf(link_path, sizeof(link_path), "%s/escape", root);
  assert(written > 0 && (size_t)written < sizeof(link_path));
  assert(symlink(external, link_path) == 0);

  ant_hvf_9p_device_t dev;
  init_dev(&dev, root);

  char host[4096];
  assert(ant_hvf_9p_existing_path(&dev, "escape", host, sizeof(host)) == -EPERM);
  assert(ant_hvf_9p_child_path(&dev, "escape", "new-file", host, sizeof(host)) == -EPERM);

  assert(ant_hvf_9p_symlink_target_bad(""));
  assert(ant_hvf_9p_symlink_target_bad("/abs"));
  assert(ant_hvf_9p_symlink_target_bad("../x"));
  assert(ant_hvf_9p_symlink_target_bad("a/../b"));
  assert(ant_hvf_9p_symlink_target_bad("a//b"));
  assert(!ant_hvf_9p_symlink_target_bad("target"));
  assert(!ant_hvf_9p_symlink_target_bad("dir/file"));

  unlink(link_path);
  rmdir(root);
  rmdir(external);
}

#else

static void test_relative_path_policy(void) {}
static void test_symlink_escape_policy(void) {}

#endif

static void test_temp_write_cleanup(void) {
  ant_sandbox_launch_options_t opts;
  ant_sandbox_launch_options_init(&opts);

  char err[512] = {0};
  assert(ant_sandbox_launch_add_mount(&opts, "tmp:/tmp", false, err, sizeof(err)) == 0);
  assert(opts.temp_dir_count == 1);
  assert(opts.mount_count == 1);
  assert(!opts.mounts[0].readonly);

  char temp_dir[4096];
  snprintf(temp_dir, sizeof(temp_dir), "%s", opts.temp_dirs[0]);
  assert(access(temp_dir, F_OK) == 0);

  char nested[4096];
  int written = snprintf(nested, sizeof(nested), "%s/nested", temp_dir);
  assert(written > 0 && (size_t)written < sizeof(nested));
  assert(mkdir(nested, 0700) == 0);

  char file[4096];
  written = snprintf(file, sizeof(file), "%s/file", nested);
  assert(written > 0 && (size_t)written < sizeof(file));
  FILE *fp = fopen(file, "w");
  assert(fp);
  fputs("ok", fp);
  fclose(fp);

  ant_sandbox_launch_options_cleanup(&opts);
  assert(opts.temp_dir_count == 0);
  assert(access(temp_dir, F_OK) != 0);
  assert(errno == ENOENT);
}

int main(void) {
  test_relative_path_policy();
  test_symlink_escape_policy();
  test_temp_write_cleanup();
  return 0;
}
