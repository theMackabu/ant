#include <compat.h> // IWYU pragma: keep

#include "sandbox/cli.h"

#include "cli/version.h"
#include "sandbox/vm.h"
#include "utils.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANT_SANDBOX_GUEST_CWD "/workspace"

typedef struct {
  char image[4096];
  char kernel[4096];
  char cache_dir[4096];
} ant_sandbox_assets_t;

#define ANT_SANDBOX_MAX_FORWARDS 32
#define ANT_SANDBOX_MAX_MOUNTS 8

typedef struct {
  ant_sandbox_mount_t mounts[ANT_SANDBOX_MAX_MOUNTS];
  char mount_hosts[ANT_SANDBOX_MAX_MOUNTS][4096];
  char mount_guests[ANT_SANDBOX_MAX_MOUNTS][1024];
  char mount_tags[ANT_SANDBOX_MAX_MOUNTS][1200];
  ant_sandbox_port_forward_t forwards[ANT_SANDBOX_MAX_FORWARDS];
  char temp_dirs[ANT_SANDBOX_MAX_MOUNTS][4096];
  size_t mount_count;
  size_t temp_dir_count;
  char guest_cwd[1024];
  bool explicit_mounts;
  size_t forward_count;
  int script_index;
} ant_sandbox_cli_options_t;

static const char *sandbox_cache_arch(void) {
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
  const char *cache_path
) {
  const char *src = getenv(env_name);
  if (src && src[0]) {
    if (!sandbox_file_exists(src)) {
      fprintf(stderr, "sandbox: %s points at missing %s: %s\n", env_name, kind, src);
      return -ENOENT;
    }
    int written = snprintf(out, out_size, "%s", src);
    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
  }

  if (sandbox_file_exists(cache_path)) {
    int written = snprintf(out, out_size, "%s", cache_path);
    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
  }

  fprintf(stderr, "sandbox: missing sandbox image %s at %s\n", kind, cache_path);
  return -ENOENT;
}

static int sandbox_assets_resolve(ant_sandbox_assets_t *assets) {
  memset(assets, 0, sizeof(*assets));

  int rc = sandbox_cache_dir(assets->cache_dir, sizeof(assets->cache_dir));
  if (rc != 0) return rc;

  rc = ant_mkdir_p(assets->cache_dir);
  if (rc != 0) {
    fprintf(stderr, "sandbox: failed to create cache dir %s: %s\n", assets->cache_dir, strerror(errno));
    return -errno;
  }

  char image_name[128];
  char kernel_name[128];
  int written = snprintf(image_name, sizeof(image_name), "ant-sandbox-%s.img", sandbox_cache_arch());
  if (written < 0 || (size_t)written >= sizeof(image_name)) return -ENAMETOOLONG;

  written = snprintf(kernel_name, sizeof(kernel_name), "ant-kernel-%s.img", sandbox_cache_arch());
  if (written < 0 || (size_t)written >= sizeof(kernel_name)) return -ENAMETOOLONG;

  rc = sandbox_cache_path(assets->image, sizeof(assets->image), image_name);
  if (rc != 0) return rc;

  rc = sandbox_cache_path(assets->kernel, sizeof(assets->kernel), kernel_name);
  if (rc != 0) return rc;

  char image_path[4096];
  char kernel_path[4096];

  rc = sandbox_resolve_asset(image_path, sizeof(image_path), "image", "ANT_SANDBOX_IMAGE", assets->image);
  if (rc != 0) return rc;

  rc = sandbox_resolve_asset(kernel_path, sizeof(kernel_path), "kernel", "ANT_SANDBOX_KERNEL", assets->kernel);
  if (rc != 0) return rc;

  snprintf(assets->image, sizeof(assets->image), "%s", image_path);
  snprintf(assets->kernel, sizeof(assets->kernel), "%s", kernel_path);
  
  return 0;
}

static size_t sandbox_json_escaped_len(const char *str) {
  size_t len = 0;
  for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
    switch (*p) {
      case '"':
      case '\\':
      case '\b':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
        len += 2;
        break;
      default:
        len += *p < 0x20 ? 6 : 1;
        break;
    }
  }
  return len;
}

static char *sandbox_json_escape_into(char *out, const char *str) {
  static const char hex[] = "0123456789abcdef";
  for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
    switch (*p) {
      case '"': *out++ = '\\'; *out++ = '"'; break;
      case '\\': *out++ = '\\'; *out++ = '\\'; break;
      case '\b': *out++ = '\\'; *out++ = 'b'; break;
      case '\f': *out++ = '\\'; *out++ = 'f'; break;
      case '\n': *out++ = '\\'; *out++ = 'n'; break;
      case '\r': *out++ = '\\'; *out++ = 'r'; break;
      case '\t': *out++ = '\\'; *out++ = 't'; break;
      default:
        if (*p < 0x20) {
          *out++ = '\\'; *out++ = 'u'; *out++ = '0'; *out++ = '0';
          *out++ = hex[*p >> 4]; *out++ = hex[*p & 0xf];
        } else {
          *out++ = (char)*p;
        }
        break;
    }
  }
  return out;
}

static char *sandbox_build_run_request(
  const char *cwd,
  const ant_sandbox_mount_t *mounts,
  size_t mount_count,
  const char *entry,
  int argc,
  char **argv
) {
  size_t len = strlen("{\"mode\":\"run\",\"cwd\":\"\",\"entry\":\"\",\"argv\":[],\"mounts\":[]}");
  len += sandbox_json_escaped_len(cwd);
  len += sandbox_json_escaped_len(entry);
  for (int i = 0; i < argc; i++) len += sandbox_json_escaped_len(argv[i]) + 3;
  for (size_t i = 0; i < mount_count; i++) {
    len += strlen("{\"tag\":\"\",\"guest\":\"\",\"readonly\":false}") + 1;
    len += sandbox_json_escaped_len(mounts[i].tag);
    len += sandbox_json_escaped_len(mounts[i].guest_path);
  }

  char *json = try_oom(len + 1);
  char *p = json;

  p += sprintf(p, "{\"mode\":\"run\",\"cwd\":\"");
  p = sandbox_json_escape_into(p, cwd);
  p += sprintf(p, "\",\"entry\":\"");
  p = sandbox_json_escape_into(p, entry);
  p += sprintf(p, "\",\"argv\":[");

  for (int i = 0; i < argc; i++) {
    if (i > 0) *p++ = ',';
    *p++ = '"';
    p = sandbox_json_escape_into(p, argv[i]);
    *p++ = '"';
  }

  p += sprintf(p, "],\"mounts\":[");
  for (size_t i = 0; i < mount_count; i++) {
    if (i > 0) *p++ = ',';
    p += sprintf(p, "{\"tag\":\"");
    p = sandbox_json_escape_into(p, mounts[i].tag);
    p += sprintf(p, "\",\"guest\":\"");
    p = sandbox_json_escape_into(p, mounts[i].guest_path);
    p += sprintf(p, "\",\"readonly\":%s}", mounts[i].readonly ? "true" : "false");
  }
  p += sprintf(p, "]}");
  return json;
}

static bool sandbox_guest_path_valid(const char *path) {
  if (!path || path[0] != '/' || path[1] == '\0') return false;
  if (strstr(path, "/../") || strstr(path, "/..") || strstr(path, "/./")) return false;
  return strcmp(path, "/..") != 0 && strcmp(path, "/.") != 0;
}

static int sandbox_create_temp_dir(char *out, size_t out_len) {
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
  int written = snprintf(out, out_len, "%s/ant-sandbox-write.XXXXXX", tmpdir);
  if (written < 0 || (size_t)written >= out_len) return -ENAMETOOLONG;
  return mkdtemp(out) ? 0 : -errno;
}

static int sandbox_parse_mount(
  ant_sandbox_cli_options_t *opts,
  const char *value,
  bool readonly
) {
  if (opts->explicit_mounts && opts->mount_count >= ANT_SANDBOX_MAX_MOUNTS) {
    fprintf(stderr, "sandbox: too many mounts\n");
    return -E2BIG;
  }

  const char *sep = strchr(value, ':');
  if (!sep || sep == value || sep[1] == '\0') {
    fprintf(stderr, "sandbox: mount needs host:guest, got '%s'\n", value);
    return -EINVAL;
  }

  size_t host_len = (size_t)(sep - value);
  char host[4096];
  if (host_len >= sizeof(host)) return -ENAMETOOLONG;
  memcpy(host, value, host_len);
  host[host_len] = '\0';

  const char *guest = sep + 1;
  if (!sandbox_guest_path_valid(guest)) {
    fprintf(stderr, "sandbox: invalid guest mount path '%s'\n", guest);
    return -EINVAL;
  }

  size_t idx = opts->mount_count;
  if (strcmp(host, "tmp") == 0) {
    int rc = sandbox_create_temp_dir(opts->temp_dirs[opts->temp_dir_count],
                                     sizeof(opts->temp_dirs[opts->temp_dir_count]));
    if (rc != 0) {
      fprintf(stderr, "sandbox: failed to create temporary mount: %s\n", strerror(-rc));
      return rc;
    }
    snprintf(opts->mount_hosts[idx], sizeof(opts->mount_hosts[idx]), "%s",
             opts->temp_dirs[opts->temp_dir_count]);
    opts->temp_dir_count++;
    readonly = false;
  } else {
    char resolved[4096];
    if (!realpath(host, resolved)) {
      if (readonly) {
        fprintf(stderr, "sandbox: missing mount path '%s': %s\n", host, strerror(errno));
        return -errno;
      }
      int rc = ant_mkdir_p(host);
      if (rc != 0) {
        fprintf(stderr, "sandbox: failed to create mount path '%s': %s\n", host, strerror(errno));
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
  if (opts->mount_count == 1) {
    snprintf(opts->guest_cwd, sizeof(opts->guest_cwd), "%s", guest);
  }
  return 0;
}

static int sandbox_parse_port(const char *value, const char *kind, uint16_t *out) {
  if (!value || !value[0]) {
    fprintf(stderr, "sandbox: missing %s port\n", kind);
    return -EINVAL;
  }

  char *end = NULL;
  errno = 0;
  unsigned long port = strtoul(value, &end, 10);
  if (errno != 0 || !end || *end != '\0' || port == 0 || port > 65535) {
    fprintf(stderr, "sandbox: invalid %s port '%s'\n", kind, value);
    return -EINVAL;
  }

  *out = (uint16_t)port;
  return 0;
}

static int sandbox_parse_forward(const char *value, ant_sandbox_port_forward_t *out) {
  const char *sep = strchr(value, ':');
  if (!sep) {
    uint16_t port = 0;
    int rc = sandbox_parse_port(value, "forward", &port);
    if (rc != 0) return rc;
    out->host_port = port;
    out->guest_port = port;
    return 0;
  }

  char host[32];
  size_t host_len = (size_t)(sep - value);
  if (host_len == 0 || host_len >= sizeof(host)) {
    fprintf(stderr, "sandbox: invalid forward '%s'\n", value);
    return -EINVAL;
  }
  memcpy(host, value, host_len);
  host[host_len] = '\0';

  int rc = sandbox_parse_port(host, "host", &out->host_port);
  if (rc != 0) return rc;
  return sandbox_parse_port(sep + 1, "guest", &out->guest_port);
}

static int sandbox_parse_options(int argc, char **argv, ant_sandbox_cli_options_t *opts) {
  memset(opts, 0, sizeof(*opts));
  opts->script_index = -1;
  snprintf(opts->guest_cwd, sizeof(opts->guest_cwd), "%s", ANT_SANDBOX_GUEST_CWD);

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "sandbox: missing script after --\n");
        return -EINVAL;
      }
      opts->script_index = i + 1;
      return 0;
    }

    if (strcmp(arg, "--forward") == 0 || strncmp(arg, "--forward=", 10) == 0) {
      const char *value = NULL;
      if (strcmp(arg, "--forward") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: --forward needs a port or host:guest pair\n");
          return -EINVAL;
        }
        value = argv[++i];
      } else {
        value = arg + 10;
      }

      if (opts->forward_count >= ANT_SANDBOX_MAX_FORWARDS) {
        fprintf(stderr, "sandbox: too many forwarded ports\n");
        return -E2BIG;
      }

      int rc = sandbox_parse_forward(value, &opts->forwards[opts->forward_count]);
      if (rc != 0) return rc;
      opts->forward_count++;
      continue;
    }

    if (strcmp(arg, "--mount") == 0 || strncmp(arg, "--mount=", 8) == 0 ||
        strcmp(arg, "--write") == 0 || strncmp(arg, "--write=", 8) == 0) {
      bool readonly = arg[2] == 'm';
      const char *value = NULL;
      if (strcmp(arg, "--mount") == 0 || strcmp(arg, "--write") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: %s needs host:guest\n", arg);
          return -EINVAL;
        }
        value = argv[++i];
      } else {
        value = arg + 8;
      }
      opts->explicit_mounts = true;
      int rc = sandbox_parse_mount(opts, value, readonly);
      if (rc != 0) return rc;
      continue;
    }

    if (arg[0] == '-') {
      fprintf(stderr, "sandbox: unknown option '%s'\n", arg);
      return -EINVAL;
    }

    opts->script_index = i;
    return 0;
  }

  return -EINVAL;
}

static void sandbox_print_usage(void) {
  fprintf(stderr, "Usage: ant sandbox [--mount host:guest] [--write host:guest] [--forward <port|host:guest>] <script.js> [args...]\n");
}

int ant_sandbox_cmd(int argc, char **argv) {
  if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
    sandbox_print_usage();
    return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  ant_sandbox_cli_options_t opts;
  int rc = sandbox_parse_options(argc, argv, &opts);
  if (rc != 0 || opts.script_index < 0) {
    sandbox_print_usage();
    return EXIT_FAILURE;
  }

  ant_sandbox_assets_t assets;
  rc = sandbox_assets_resolve(&assets);
  if (rc != 0) return EXIT_FAILURE;

  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) {
    fprintf(stderr, "sandbox: failed to read current directory: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  if (!opts.explicit_mounts) {
    char default_mount[sizeof(cwd) + sizeof(":/workspace")];
    snprintf(default_mount, sizeof(default_mount), "%s:%s", cwd, ANT_SANDBOX_GUEST_CWD);
    rc = sandbox_parse_mount(&opts, default_mount, true);
    if (rc != 0) return EXIT_FAILURE;
  }

  int script_argc = argc - opts.script_index - 1;
  char **script_argv = argv + opts.script_index + 1;
  char *request = sandbox_build_run_request(opts.guest_cwd,
                                            opts.mounts,
                                            opts.mount_count,
                                            argv[opts.script_index],
                                            script_argc,
                                            script_argv);
  ant_sandbox_vm_config_t config = {
    .image_path = assets.image,
    .kernel_path = assets.kernel,
    .request_json = request,
    .mounts = opts.mounts,
    .mount_count = opts.mount_count,
    .network_enabled = true,
    .forwards = opts.forwards,
    .forward_count = opts.forward_count,
    .cpu_count = 1,
    .memory_size = 1024ull * 1024ull * 1024ull,
    .timeout_ms = 0,
  };

  rc = ant_sandbox_vm_start(&config);
  free(request);

  if (rc == 0) return EXIT_SUCCESS;
  if (rc == -ENOSYS) {
    fprintf(stderr, "sandbox: VM backend is not ready to run the cached image yet\n");
  }
  return EXIT_FAILURE;
}
