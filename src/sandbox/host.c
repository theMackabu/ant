#include <compat.h> // IWYU pragma: keep

#include "sandbox/host.h"
#include "cli/version.h"
#include "modules/io.h"
#include "sandbox/sandbox.h"
#include "utils.h"

#include <errno.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#define lstat stat
#endif

static void sandbox_host_error(char *err, size_t err_len, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));

static void sandbox_host_error(char *err, size_t err_len, const char *fmt, ...) {
  if (!err || err_len == 0) return;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_len, fmt, ap);
  va_end(ap);
}

const char *ant_sandbox_cache_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x64";
#else
  return "unknown";
#endif
}

static bool sandbox_file_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int sandbox_cache_path(char *out, size_t out_size, const char *filename) {
  char suffix[512];
  int written = snprintf(suffix, sizeof(suffix), "sandbox/%s/%s", ant_semver(), filename);
  if (written < 0 || (size_t)written >= sizeof(suffix)) return -ENAMETOOLONG;
  return ant_xdg_cache_path(out, out_size, suffix) == 0 ? 0 : -EINVAL;
}

static int sandbox_cache_dir(char *out, size_t out_size) {
  char suffix[512];
  int written = snprintf(suffix, sizeof(suffix), "sandbox/%s", ant_semver());
  if (written < 0 || (size_t)written >= sizeof(suffix)) return -ENAMETOOLONG;
  return ant_xdg_cache_path(out, out_size, suffix) == 0 ? 0 : -EINVAL;
}

static int sandbox_resolve_asset(
  char *out,
  size_t out_size,
  const char *kind,
  const char *env_name,
  const char *cache_path,
  char *err,
  size_t err_len
) {
  const char *src = getenv(env_name);
  if (src && src[0]) {
    if (!sandbox_file_exists(src)) {
      sandbox_host_error(err, err_len, "%s points at missing %s: %s", env_name, kind, src);
      return -ENOENT;
    }
    int written = snprintf(out, out_size, "%s", src);
    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
  }

  if (sandbox_file_exists(cache_path)) {
    int written = snprintf(out, out_size, "%s", cache_path);
    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
  }

  sandbox_host_error(err, err_len, "missing sandbox image %s at %s", kind, cache_path);
  return -ENOENT;
}

int ant_sandbox_assets_resolve(ant_sandbox_assets_t *assets, char *err, size_t err_len) {
  if (!assets) return -EINVAL;
  memset(assets, 0, sizeof(*assets));

  int rc = sandbox_cache_dir(assets->cache_dir, sizeof(assets->cache_dir));
  if (rc != 0) {
    sandbox_host_error(err, err_len, "failed to resolve sandbox cache directory");
    return rc;
  }

  rc = ant_mkdir_p(assets->cache_dir);
  if (rc != 0) {
    sandbox_host_error(err, err_len, "failed to create cache dir %s: %s", assets->cache_dir, strerror(errno));
    return -errno;
  }

  char image_name[128];
  char kernel_name[128];
  int written = snprintf(image_name, sizeof(image_name), "ant-sandbox-%s.img", ant_sandbox_cache_arch());
  if (written < 0 || (size_t)written >= sizeof(image_name)) return -ENAMETOOLONG;

  written = snprintf(kernel_name, sizeof(kernel_name), "ant-kernel-%s.img", ant_sandbox_cache_arch());
  if (written < 0 || (size_t)written >= sizeof(kernel_name)) return -ENAMETOOLONG;

  rc = sandbox_cache_path(assets->image, sizeof(assets->image), image_name);
  if (rc != 0) return rc;

  rc = sandbox_cache_path(assets->kernel, sizeof(assets->kernel), kernel_name);
  if (rc != 0) return rc;

  char image_path[4096];
  char kernel_path[4096];

  rc = sandbox_resolve_asset(image_path, sizeof(image_path), "image", "ANT_SANDBOX_IMAGE", assets->image, err, err_len);
  if (rc != 0) return rc;

  rc = sandbox_resolve_asset(kernel_path, sizeof(kernel_path), "kernel", "ANT_SANDBOX_KERNEL", assets->kernel, err, err_len);
  if (rc != 0) return rc;

  snprintf(assets->image, sizeof(assets->image), "%s", image_path);
  snprintf(assets->kernel, sizeof(assets->kernel), "%s", kernel_path);

  return 0;
}

void ant_sandbox_launch_options_init(ant_sandbox_launch_options_t *opts) {
  if (!opts) return;
  memset(opts, 0, sizeof(*opts));
  snprintf(opts->guest_cwd, sizeof(opts->guest_cwd), "%s", ANT_SANDBOX_DEFAULT_GUEST_CWD);
}

static bool sandbox_is_managed_temp_dir(const char *path) {
  if (!path || !path[0]) return false;
  const char *base = strrchr(path, '/');
  const char *win_base = strrchr(path, '\\');
  if (!base || (win_base && win_base > base)) base = win_base;
  base = base ? base + 1 : path;
  return strncmp(base, "ant-sandbox-write.", strlen("ant-sandbox-write.")) == 0;
}

static int sandbox_remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -errno;
  if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path) == 0 ? 0 : -errno;

  DIR *dir = opendir(path);
  if (!dir) return -errno;
  int rc = 0;
  struct dirent *ent = NULL;
  while ((ent = readdir(dir))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char child[4096];
    int written = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    if (written < 0 || (size_t)written >= sizeof(child)) {
      rc = -ENAMETOOLONG;
      break;
    }
    int child_rc = sandbox_remove_tree(child);
    if (child_rc != 0 && rc == 0) rc = child_rc;
  }
  closedir(dir);
  if (rc != 0) return rc;
  return rmdir(path) == 0 ? 0 : -errno;
}

void ant_sandbox_launch_options_cleanup(ant_sandbox_launch_options_t *opts) {
  if (!opts) return;
  for (size_t i = 0; i < opts->temp_dir_count; i++) {
    if (sandbox_is_managed_temp_dir(opts->temp_dirs[i])) {
      (void)sandbox_remove_tree(opts->temp_dirs[i]);
    }
    opts->temp_dirs[i][0] = '\0';
  }
  opts->temp_dir_count = 0;
}

static bool sandbox_guest_path_valid(const char *path) {
  if (!path || path[0] != '/' || path[1] == '\0') return false;
  const char *p = path;
  while ((p = strchr(p, '/')) != NULL) {
    p++;
    if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) return false;
    if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) return false;
  }
  return true;
}

static int sandbox_create_temp_dir(char *out, size_t out_len) {
#ifdef _WIN32
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir || !tmpdir[0]) tmpdir = getenv("TEMP");
  if (!tmpdir || !tmpdir[0]) tmpdir = ".";
  for (unsigned attempt = 0; attempt < 100; attempt++) {
    int written = snprintf(
      out, out_len,
      "%s/ant-sandbox-write.%lu.%u",
      tmpdir,
      (unsigned long)GetCurrentProcessId(),
      attempt
    );
    if (written < 0 || (size_t)written >= out_len) return -ENAMETOOLONG;
    if (_mkdir(out) == 0) return 0;
    if (errno != EEXIST) return -errno;
  }
  return -EEXIST;
#else
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
  int written = snprintf(out, out_len, "%s/ant-sandbox-write.XXXXXX", tmpdir);
  if (written < 0 || (size_t)written >= out_len) return -ENAMETOOLONG;
  return mkdtemp(out) ? 0 : -errno;
#endif
}

int ant_sandbox_launch_add_mount(
  ant_sandbox_launch_options_t *opts,
  const char *value,
  bool readonly,
  char *err,
  size_t err_len
) {
  if (!opts || !value) return -EINVAL;
  if (opts->mount_count >= ANT_SANDBOX_MAX_MOUNTS) {
    sandbox_host_error(err, err_len, "too many mounts");
    return -E2BIG;
  }

  const char *sep = strchr(value, ':');
  if (!sep || sep == value || sep[1] == '\0') {
    sandbox_host_error(err, err_len, "mount needs host:guest, got '%s'", value);
    return -EINVAL;
  }

  size_t host_len = (size_t)(sep - value);
  char host[4096];
  if (host_len >= sizeof(host)) return -ENAMETOOLONG;
  memcpy(host, value, host_len);
  host[host_len] = '\0';

  const char *guest = sep + 1;
  if (!sandbox_guest_path_valid(guest)) {
    sandbox_host_error(err, err_len, "invalid guest mount path '%s'", guest);
    return -EINVAL;
  }

  size_t idx = opts->mount_count;
  if (strcmp(host, "tmp") == 0) {
    if (opts->temp_dir_count >= ANT_SANDBOX_MAX_MOUNTS) return -E2BIG;
    char temp_dir[4096];
    int rc = sandbox_create_temp_dir(temp_dir, sizeof(temp_dir));
    if (rc != 0) {
      sandbox_host_error(err, err_len, "failed to create temporary mount: %s", strerror(-rc));
      return rc;
    }
    char resolved[4096];
    if (!realpath(temp_dir, resolved)) {
      rc = -errno;
      if (sandbox_is_managed_temp_dir(temp_dir)) (void)sandbox_remove_tree(temp_dir);
      return rc;
    }
    snprintf(opts->temp_dirs[opts->temp_dir_count],
      sizeof(opts->temp_dirs[opts->temp_dir_count]),
      "%s",
      resolved);
    snprintf(opts->mount_hosts[idx], sizeof(opts->mount_hosts[idx]), "%s", resolved);
    opts->temp_dir_count++;
    readonly = false;
  } else {
    char resolved[4096];
    if (!realpath(host, resolved)) {
      if (readonly) {
        sandbox_host_error(err, err_len, "missing mount path '%s': %s", host, strerror(errno));
        return -errno;
      }
      int rc = ant_mkdir_p(host);
      if (rc != 0) {
        sandbox_host_error(err, err_len, "failed to create mount path '%s': %s", host, strerror(errno));
        return -errno;
      }
      if (!realpath(host, resolved)) return -errno;
    }
    snprintf(opts->mount_hosts[idx], sizeof(opts->mount_hosts[idx]), "%s", resolved);
  }

  int written = snprintf(opts->mount_guests[idx], sizeof(opts->mount_guests[idx]), "%s", guest);
  if (written < 0 || (size_t)written >= sizeof(opts->mount_guests[idx])) return -ENAMETOOLONG;
  written = snprintf(opts->mount_tags[idx], sizeof(opts->mount_tags[idx]), "%zu:%s%s",
                     idx,
                     guest,
                     readonly ? ":ro" : "");
  if (written < 0 || (size_t)written >= sizeof(opts->mount_tags[idx])) return -ENAMETOOLONG;

  opts->mounts[idx] = (ant_sandbox_mount_t){
    .host_path = opts->mount_hosts[idx],
    .guest_path = opts->mount_guests[idx],
    .tag = opts->mount_tags[idx],
    .readonly = readonly,
  };
  opts->mount_count++;
  opts->explicit_mounts = true;
  if (opts->mount_count == 1) snprintf(opts->guest_cwd, sizeof(opts->guest_cwd), "%s", guest);
  return 0;
}

static int sandbox_parse_port(const char *value, const char *kind, uint16_t *out, char *err, size_t err_len) {
  if (!value || !value[0]) {
    sandbox_host_error(err, err_len, "missing %s port", kind);
    return -EINVAL;
  }

  char *end = NULL;
  errno = 0;
  unsigned long port = strtoul(value, &end, 10);
  if (errno != 0 || !end || *end != '\0' || port == 0 || port > 65535) {
    sandbox_host_error(err, err_len, "invalid %s port '%s'", kind, value);
    return -EINVAL;
  }

  *out = (uint16_t)port;
  return 0;
}

int ant_sandbox_launch_add_forward(
  ant_sandbox_launch_options_t *opts,
  const char *value,
  char *err,
  size_t err_len
) {
  if (!opts || !value) return -EINVAL;
  if (opts->forward_count >= ANT_SANDBOX_MAX_FORWARDS) {
    sandbox_host_error(err, err_len, "too many forwarded ports");
    return -E2BIG;
  }

  ant_sandbox_port_forward_t forward = { 0 };
  const char *sep = strchr(value, ':');
  if (!sep) {
    uint16_t port = 0;
    int rc = sandbox_parse_port(value, "forward", &port, err, err_len);
    if (rc != 0) return rc;
    forward.host_port = port;
    forward.guest_port = port;
  } else {
    char host[32];
    size_t host_len = (size_t)(sep - value);
    if (host_len == 0 || host_len >= sizeof(host)) {
      sandbox_host_error(err, err_len, "invalid forward '%s'", value);
      return -EINVAL;
    }
    memcpy(host, value, host_len);
    host[host_len] = '\0';

    int rc = sandbox_parse_port(host, "host", &forward.host_port, err, err_len);
    if (rc != 0) return rc;
    rc = sandbox_parse_port(sep + 1, "guest", &forward.guest_port, err, err_len);
    if (rc != 0) return rc;
  }

  opts->forwards[opts->forward_count++] = forward;
  return 0;
}

int ant_sandbox_launch_add_default_mount(
  ant_sandbox_launch_options_t *opts,
  const char *host_path,
  char *err,
  size_t err_len
) {
  if (!opts || !host_path) return -EINVAL;

  char default_mount[4096 + sizeof(":" ANT_SANDBOX_DEFAULT_GUEST_CWD)];
  int written = snprintf(default_mount, sizeof(default_mount), "%s:%s", host_path, ANT_SANDBOX_DEFAULT_GUEST_CWD);
  if (written < 0 || (size_t)written >= sizeof(default_mount)) return -ENAMETOOLONG;
  return ant_sandbox_launch_add_mount(opts, default_mount, true, err, err_len);
}

static void sandbox_terminal_size(uint16_t *rows_out, uint16_t *cols_out) {
  int rows = 24;
  int cols = 80;
#ifndef _WIN32
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_row > 0) rows = ws.ws_row;
    if (ws.ws_col > 0) cols = ws.ws_col;
  }
#endif
  if (rows > UINT16_MAX) rows = UINT16_MAX;
  if (cols > UINT16_MAX) cols = UINT16_MAX;
  *rows_out = (uint16_t)rows;
  *cols_out = (uint16_t)cols;
}

uint32_t ant_sandbox_terminal_capabilities(uint16_t *rows_out, uint16_t *cols_out) {
  uint32_t caps = 0;
  bool stdout_tty = isatty(STDOUT_FILENO) != 0;
  bool stderr_tty = isatty(STDERR_FILENO) != 0;

  if (stdout_tty) caps |= ANT_SANDBOX_CAP_STDOUT_TTY;
  if (stderr_tty) caps |= ANT_SANDBOX_CAP_STDERR_TTY;

  const char *force_color = getenv("FORCE_COLOR");
  const char *no_color = getenv("NO_COLOR");
  if (io_no_color || (no_color && *no_color)) {
    caps |= ANT_SANDBOX_CAP_COLOR_STRIP;
  } else if (force_color) {
    if (ant_env_bool(force_color, true)) caps |= ANT_SANDBOX_CAP_COLOR_FORCE;
    else caps |= ANT_SANDBOX_CAP_COLOR_STRIP;
  } else if (stdout_tty || stderr_tty) {
    caps |= ANT_SANDBOX_CAP_COLOR_FORCE;
  }

  sandbox_terminal_size(rows_out, cols_out);
  return caps;
}
