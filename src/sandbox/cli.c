#include <compat.h> // IWYU pragma: keep

#include "sandbox/cli.h"

#include "sandbox/transport.h"
#include "sandbox/vm.h"
#include "utils.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef _WIN32
#include <glob.h>
#endif

#define ANT_SANDBOX_GUEST_CWD "/workspace"

typedef struct {
  char image[4096];
  char kernel[4096];
  char cache_dir[4096];
} ant_sandbox_assets_t;

static const char *sandbox_cache_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x64";
#else
  return "unknown";
#endif
}

static const char *sandbox_local_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x64";
#else
  return "unknown";
#endif
}

static const char *sandbox_ops_image_name(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "ant-sandbox-aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "ant-sandbox-x64";
#else
  return "ant-sandbox";
#endif
}

static bool sandbox_file_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int sandbox_copy_file(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (!in) return -errno;

  FILE *out = fopen(dst, "wb");
  if (!out) {
    int rc = -errno;
    fclose(in);
    return rc;
  }

  char buf[64 * 1024];
  int rc = 0;

  while (!feof(in)) {
    size_t n = fread(buf, 1, sizeof(buf), in);
    if (n > 0 && fwrite(buf, 1, n, out) != n) {
      rc = -errno;
      break;
    }
    if (ferror(in)) {
      rc = -errno;
      break;
    }
  }

  if (fclose(out) != 0 && rc == 0) rc = -errno;
  fclose(in);
  return rc;
}

static int sandbox_cache_path(char *out, size_t out_size, const char *filename) {
  char suffix[512];
  int written = snprintf(suffix, sizeof(suffix), "sandbox/%s", filename);
  if (written < 0 || (size_t)written >= sizeof(suffix)) return -ENAMETOOLONG;
  return ant_xdg_cache_path(out, out_size, suffix) == 0 ? 0 : -EINVAL;
}

static int sandbox_cache_dir(char *out, size_t out_size) {
  return ant_xdg_cache_path(out, out_size, "sandbox") == 0 ? 0 : -EINVAL;
}

static int sandbox_seed_from_env(const char *env_name, const char *dst) {
  const char *src = getenv(env_name);
  if (!src || !src[0]) return -ENOENT;
  if (!sandbox_file_exists(src)) return -ENOENT;
  return sandbox_copy_file(src, dst);
}

static int sandbox_find_local_image(char *out, size_t out_size) {
  const char *env = getenv("ANT_SANDBOX_IMAGE");
  if (env && env[0] && sandbox_file_exists(env)) {
    int written = snprintf(out, out_size, "%s", env);
    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
  }

  char candidate[4096];
  int written = snprintf(candidate, sizeof(candidate), "nanos/out/%s/ant-sandbox.img", sandbox_local_arch());
  if (written >= 0 && (size_t)written < sizeof(candidate) && sandbox_file_exists(candidate)) {
    written = snprintf(out, out_size, "%s", candidate);
    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
  }

  const char *home = getenv("HOME");
  if (home && home[0]) {
    written = snprintf(candidate, sizeof(candidate), "%s/.ops/images/%s.img", home, sandbox_ops_image_name());
    if (written >= 0 && (size_t)written < sizeof(candidate) && sandbox_file_exists(candidate)) {
      written = snprintf(out, out_size, "%s", candidate);
      return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
    }

    written = snprintf(candidate, sizeof(candidate), "%s/.ops/images/%s", home, sandbox_ops_image_name());
    if (written >= 0 && (size_t)written < sizeof(candidate) && sandbox_file_exists(candidate)) {
      written = snprintf(out, out_size, "%s", candidate);
      return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
    }
  }

  return -ENOENT;
}

static int sandbox_find_local_kernel(char *out, size_t out_size) {
  const char *env = getenv("ANT_SANDBOX_KERNEL");
  if (env && env[0] && sandbox_file_exists(env)) {
    int written = snprintf(out, out_size, "%s", env);
    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
  }

#ifndef _WIN32
  const char *home = getenv("HOME");
  if (!home || !home[0]) return -ENOENT;

  char pattern[4096];
#if defined(__aarch64__) || defined(_M_ARM64)
  int written = snprintf(pattern, sizeof(pattern), "%s/.ops/*-arm/kernel.img", home);
#else
  int written = snprintf(pattern, sizeof(pattern), "%s/.ops/[0-9]*/kernel.img", home);
#endif
  if (written < 0 || (size_t)written >= sizeof(pattern)) return -ENAMETOOLONG;

  glob_t matches = { 0 };
  int glob_rc = glob(pattern, 0, NULL, &matches);
  if (glob_rc == 0 && matches.gl_pathc > 0) {
    written = snprintf(out, out_size, "%s", matches.gl_pathv[0]);
    globfree(&matches);
    return written < 0 || (size_t)written >= out_size ? -ENAMETOOLONG : 0;
  }

  globfree(&matches);
#endif
  return -ENOENT;
}

static int sandbox_ensure_cached_file(
  const char *kind,
  const char *dst,
  int (*find_source)(char *out, size_t out_size)
) {
  if (sandbox_file_exists(dst)) return 0;

  char source[4096];
  int rc = find_source(source, sizeof(source));
  if (rc != 0) {
    fprintf(stderr, "sandbox: missing cached %s at %s\n", kind, dst);
    fprintf(stderr, "sandbox: run nanos/build-sandbox.sh or set ANT_SANDBOX_%s\n",
            strcmp(kind, "image") == 0 ? "IMAGE" : "KERNEL");
    return rc;
  }

  rc = sandbox_copy_file(source, dst);
  if (rc != 0) {
    fprintf(stderr, "sandbox: failed to copy %s from %s to %s: %s\n",
            kind, source, dst, strerror(-rc));
    return rc;
  }

  return 0;
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

  written = snprintf(kernel_name, sizeof(kernel_name), "nanos-kernel-%s.img", sandbox_cache_arch());
  if (written < 0 || (size_t)written >= sizeof(kernel_name)) return -ENAMETOOLONG;

  rc = sandbox_cache_path(assets->image, sizeof(assets->image), image_name);
  if (rc != 0) return rc;

  rc = sandbox_cache_path(assets->kernel, sizeof(assets->kernel), kernel_name);
  if (rc != 0) return rc;

  if (!sandbox_file_exists(assets->image)) {
    rc = sandbox_seed_from_env("ANT_SANDBOX_IMAGE", assets->image);
    if (rc != 0 && rc != -ENOENT) return rc;
  }

  rc = sandbox_ensure_cached_file("image", assets->image, sandbox_find_local_image);
  if (rc != 0) return rc;

  if (!sandbox_file_exists(assets->kernel)) {
    rc = sandbox_seed_from_env("ANT_SANDBOX_KERNEL", assets->kernel);
    if (rc != 0 && rc != -ENOENT) return rc;
  }

  return sandbox_ensure_cached_file("kernel", assets->kernel, sandbox_find_local_kernel);
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

static char *sandbox_build_run_request(const char *entry, int argc, char **argv) {
  size_t len = strlen("{\"mode\":\"run\",\"cwd\":\"" ANT_SANDBOX_GUEST_CWD "\",\"entry\":\"\",\"argv\":[]}");
  len += sandbox_json_escaped_len(entry);
  for (int i = 0; i < argc; i++) len += sandbox_json_escaped_len(argv[i]) + 3;

  char *json = try_oom(len + 1);
  char *p = json;

  p += sprintf(p, "{\"mode\":\"run\",\"cwd\":\"" ANT_SANDBOX_GUEST_CWD "\",\"entry\":\"");
  p = sandbox_json_escape_into(p, entry);
  p += sprintf(p, "\",\"argv\":[");

  for (int i = 0; i < argc; i++) {
    if (i > 0) *p++ = ',';
    *p++ = '"';
    p = sandbox_json_escape_into(p, argv[i]);
    *p++ = '"';
  }

  p += sprintf(p, "]}");
  return json;
}

static void sandbox_print_usage(void) {
  fprintf(stderr, "Usage: ant sandbox <script.js> [args...]\n");
}

int ant_sandbox_cmd(int argc, char **argv) {
  if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
    sandbox_print_usage();
    return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  if (argv[1][0] == '-') {
    fprintf(stderr, "sandbox: explicit sandbox flags are not implemented yet\n");
    sandbox_print_usage();
    return EXIT_FAILURE;
  }

  ant_sandbox_assets_t assets;
  int rc = sandbox_assets_resolve(&assets);
  if (rc != 0) return EXIT_FAILURE;

  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) {
    fprintf(stderr, "sandbox: failed to read current directory: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  char *request = sandbox_build_run_request(argv[1], argc - 2, argv + 2);
  ant_sandbox_vm_config_t config = {
    .image_path = assets.image,
    .kernel_path = assets.kernel,
    .request_json = request,
    .shared_dir_path = cwd,
    .shared_dir_tag = "0",
    .shared_dir_readonly = true,
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
