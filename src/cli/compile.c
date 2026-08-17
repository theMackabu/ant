#include <compat.h> // IWYU pragma: keep

#include "cli/compile.h"
#include "cli/version.h"
#include "download.h"
#include "cli/misc.h"

#include "ant.h"
#include "bootstrap.h"
#include "runtime.h"
#include "utils.h"
#include "vfs_bundle.h"
#include "esm/trace.h"
#include "silver/vm.h"

#include <argtable3.h>
#include <crprintf.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

bool ant_compile_bypass_revision = false;

typedef struct {
  char download_url[2048];
  uint64_t size;
} compile_runtime_info_t;

static int64_t compile_tell(FILE *f) {
#ifdef _WIN32
  return _ftelli64(f);
#else
  return (int64_t)ftello(f);
#endif
}

static const char *compile_json_string(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_str(val) ? yyjson_get_str(val) : NULL;
}

static int compile_manifest_select_runtime(const char *json, size_t json_len, compile_runtime_info_t *out, char *err, size_t err_len) {
  memset(out, 0, sizeof(*out));
  const char *target = ant_release_platform_target();

  yyjson_doc *doc = yyjson_read(json, json_len, 0);
  if (!doc) {
    snprintf(err, err_len, "manifest response was not valid JSON");
    return -EINVAL;
  }

  int rc = -ENOENT;
  bool wrong_revision = false;
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *items = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "runtime") : NULL;

  if (items && yyjson_is_arr(items)) {
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(items, idx, max, item) {
      const char *item_target = compile_json_string(item, "target");
      yyjson_val *available = yyjson_obj_get(item, "available");
      if (!item_target || strcmp(item_target, target) != 0) continue;
      if (available && yyjson_is_bool(available) && !yyjson_get_bool(available)) continue;

      const char *revision = compile_json_string(item, "revision");
      if (!ant_compile_bypass_revision && (!revision || strcmp(revision, ANT_GIT_LONGHASH) != 0)) {
        wrong_revision = true;
        continue;
      }

      const char *download_url = compile_json_string(item, "download_url");
      if (!download_url) break;
      snprintf(out->download_url, sizeof(out->download_url), "%s", download_url);

      yyjson_val *artifact = yyjson_obj_get(item, "artifact");
      yyjson_val *size = artifact && yyjson_is_obj(artifact) ? yyjson_obj_get(artifact, "size_in_bytes") : NULL;
      out->size = size && yyjson_is_uint(size) ? yyjson_get_uint(size) : 0;

      rc = 0;
      break;
    }
  }

  if (rc != 0) {
    if (wrong_revision) snprintf(err, err_len,
      "manifest has no ant-runtime for revision %s (target %s); upgrade ant or pass --runtime <path>",
      ANT_GIT_LONGHASH, target);
    else  snprintf(err, err_len, "manifest has no ant-runtime for target %s; pass --runtime <path>", target);
  }

  yyjson_doc_free(doc);
  return rc;
}

static int compile_copy_file(const char *src_path, FILE *dst, char *err, size_t err_len) {
  FILE *src = fopen(src_path, "rb");
  if (!src) {
    snprintf(err, err_len, "cannot open runtime binary %s: %s", src_path, strerror(errno));
    return -1;
  }

  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
    if (fwrite(buf, 1, n, dst) != n) {
      snprintf(err, err_len, "write failed: %s", strerror(errno));
      fclose(src);
      return -1;
    }
  }

  bool read_err = ferror(src) != 0;
  fclose(src);
  if (read_err) {
    snprintf(err, err_len, "read failed for %s", src_path);
    return -1;
  }
  return 0;
}

static bool compile_runtime_cache_path(char *out, size_t out_len) {
  char suffix[128];
  char dir[4096];
  if ((size_t)snprintf(suffix, sizeof(suffix), "compile/%s", ANT_GIT_LONGHASH) >= sizeof(suffix)) return false;
  if (ant_xdg_cache_path(dir, sizeof(dir), suffix) != 0) return false;
  if (ant_mkdir_p(dir) != 0) return false;
  return (size_t)snprintf(out, out_len, "%s/ant-runtime", dir) < out_len;
}

static int compile_download_runtime(FILE *dst, char *err, size_t err_len) {
  char *manifest = NULL;
  size_t manifest_len = 0;
  if (ant_manifest_fetch(&manifest, &manifest_len, err, err_len) != 0) return -1;

  compile_runtime_info_t info;
  int rc = compile_manifest_select_runtime(manifest, manifest_len, &info, err, err_len);
  free(manifest);
  if (rc != 0) return -1;

  if (ant_http_download_file(info.download_url, dst, "Downloading ant-runtime", err, err_len) != 0) return -1;

  if (info.size > 0) {
    int64_t written = compile_tell(dst);
    if (written < 0 || (uint64_t)written != info.size) {
      snprintf(err, err_len, "downloaded runtime is %lld bytes, manifest expects %llu",
        (long long)written, (unsigned long long)info.size);
      return -1;
    }
  }
  return 0;
}

static int compile_write_runtime(FILE *out, const char *runtime_path, bool no_cache, char *err, size_t err_len) {
  if (runtime_path) return compile_copy_file(runtime_path, out, err, err_len);

  char cache_path[4096];
  bool cached = !no_cache && compile_runtime_cache_path(cache_path, sizeof(cache_path));

  if (cached) {
    FILE *probe = fopen(cache_path, "rb");
    if (probe) {
      fclose(probe);
      return compile_copy_file(cache_path, out, err, err_len);
    }
  }

  if (!cached) return compile_download_runtime(out, err, err_len);

  char cache_tmp[4096];
  if ((size_t)snprintf(cache_tmp, sizeof(cache_tmp), "%s.tmp.%ld", cache_path, (long)getpid()) >= sizeof(cache_tmp)) {
    return compile_download_runtime(out, err, err_len);
  }

  FILE *cf = fopen(cache_tmp, "wb");
  if (!cf) return compile_download_runtime(out, err, err_len);

  int rc = compile_download_runtime(cf, err, err_len);
  if (fclose(cf) != 0 || rc != 0 || rename(cache_tmp, cache_path) != 0) {
    remove(cache_tmp);
    if (rc != 0) return -1;
    return compile_download_runtime(out, err, err_len);
  }

  ant_cache_prune_revisions("compile", ANT_GIT_LONGHASH);
  return compile_copy_file(cache_path, out, err, err_len);
}

#ifdef __APPLE__
#include <mach-o/loader.h>

static int compile_run_codesign(const char *path, bool remove) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
    if (remove) execl("/usr/bin/codesign", "codesign", "--remove-signature", path, (char *)NULL);
    else execl("/usr/bin/codesign", "codesign", "--force", "--sign", "-", path, (char *)NULL);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int compile_macho_cover_trailer(const char *path, char *err, size_t err_len) {
  FILE *f = fopen(path, "r+b");
  if (!f) {
    snprintf(err, err_len, "cannot reopen %s: %s", path, strerror(errno));
    return -1;
  }

  int rc = -1;
  struct mach_header_64 hdr;
  if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) goto done;
  if (hdr.magic != MH_MAGIC_64) {
    snprintf(err, err_len, "unsupported runtime binary format (not a 64-bit Mach-O)");
    goto done;
  }

  if (fseek(f, 0, SEEK_END) != 0) goto done;
  long file_size = ftell(f);
  if (file_size <= 0) goto done;

  long off = (long)sizeof(hdr);
  for (uint32_t i = 0; i < hdr.ncmds; i++) {
    struct load_command lc;
    if (fseek(f, off, SEEK_SET) != 0 || fread(&lc, 1, sizeof(lc), f) != sizeof(lc)) goto done;

    if (lc.cmd == LC_SEGMENT_64) {
      struct segment_command_64 seg;
      if (fseek(f, off, SEEK_SET) != 0 || fread(&seg, 1, sizeof(seg), f) != sizeof(seg)) goto done;

      if (strncmp(seg.segname, SEG_LINKEDIT, sizeof(seg.segname)) == 0) {
        uint64_t covered = (uint64_t)file_size - seg.fileoff;
        seg.filesize = covered;
        const uint64_t page = 0x4000;
        seg.vmsize = (covered + page - 1) & ~(page - 1);

        if (fseek(f, off, SEEK_SET) != 0 || fwrite(&seg, 1, sizeof(seg), f) != sizeof(seg)) goto done;
        rc = 0;
        goto done;
      }
    }
    off += lc.cmdsize;
  }
  snprintf(err, err_len, "runtime binary has no __LINKEDIT segment");

done:
  if (rc != 0 && !err[0]) snprintf(err, err_len, "failed to update runtime binary layout");
  fclose(f);
  return rc;
}
#endif

static bool compile_is_path_sep(char c) {
#ifdef _WIN32
  if (c == '\\') return true;
#endif
  return c == '/';
}

static const char *compile_last_sep(const char *path) {
  const char *last = NULL;
  for (const char *p = path; *p; p++) {
    if (compile_is_path_sep(*p)) last = p;
  }
  return last;
}

static const char *entry_dir_name(const char *entry_path, size_t *len) {
  const char *dir_end = compile_last_sep(entry_path);
  if (!dir_end || dir_end == entry_path) return NULL;

  const char *dir_start = dir_end - 1;
  while (dir_start > entry_path && !compile_is_path_sep(dir_start[-1])) dir_start--;

  *len = (size_t)(dir_end - dir_start);
  return *len ? dir_start : NULL;
}

static void compile_default_output(const char *entry_path, char *out, size_t out_len) {
  const char *base = compile_last_sep(entry_path);
  base = base ? base + 1 : entry_path;

  char name[512];
  snprintf(name, sizeof(name), "%s", base);
  char *dot = strrchr(name, '.');
  if (dot && dot != name) *dot = '\0';

  size_t dir_len = 0;
  const char *dir = NULL;
  if (strcmp(name, "index") == 0 || strcmp(name, "main") == 0) dir = entry_dir_name(entry_path, &dir_len);
  if (dir) snprintf(name, sizeof(name), "%.*s", (int)dir_len, dir);

  if (!name[0]) snprintf(name, sizeof(name), "app");
  snprintf(out, out_len, "./%s", name);
}

int ant_cmd_compile(int argc, char **argv) {
  struct arg_file *entry = arg_file1(NULL, NULL, "<entry>", "entry script to compile");
  struct arg_file *outfile = arg_file0("o", "outfile", "<path>", "output executable path");
  struct arg_file *runtime = arg_file0(NULL, "runtime", "<path>", "use a local ant-runtime binary instead of downloading");
  struct arg_file *root = arg_file0(NULL, "root", "<dir>", "pack root for embedded module paths");
  struct arg_lit *no_cache = arg_lit0(NULL, "no-cache", "always download the runtime instead of using the cache");
  struct arg_lit *help = arg_lit0("h", "help", "display this help and exit");
  struct arg_end *end = arg_end(20);

  void *argtable[] = { entry, outfile, runtime, root, no_cache, help, end };
  int nerrors = arg_parse(argc, argv, argtable);

  int rc = EXIT_FAILURE;
  char *resolved_entry = NULL;
  char *entry_abs = NULL;
  ant_t *js = NULL;
  bool traced = false;
  ant_trace_result_t trace = {0};
  char tmp_path[4096] = {0};

  if (help->count > 0 || nerrors > 0 || entry->count == 0) {
    if (help->count == 0 && entry->count == 0) crfprintf(stderr, "{error}: missing entry script\n\n");
    else if (help->count == 0 && nerrors > 0) print_errors(stderr, end);

    crprintf("<bold>Usage:</> ant compile [flags] <entry>\n\n");
    crprintf("Compile a script and its imports into a standalone executable.\n\n");
    crprintf("<bold>Flags:</>\n");
    print_flags_help(stdout, argtable);

    rc = help->count > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    goto cleanup;
  }

  resolved_entry = resolve_js_file(entry->filename[0]);
  if (!resolved_entry) {
    crfprintf(stderr, "{error}: cannot find entry module: %s\n", entry->filename[0]);
    goto cleanup;
  }

  entry_abs = realpath(resolved_entry, NULL);
  if (!entry_abs) {
    crfprintf(stderr, "{error}: cannot resolve %s: %s\n", resolved_entry, strerror(errno));
    goto cleanup;
  }

  char root_dir[4096];
  if (root->count > 0) {
    char *root_abs = realpath(root->filename[0], NULL);
    if (!root_abs) {
      crfprintf(stderr, "{error}: invalid --root %s: %s\n", root->filename[0], strerror(errno));
      goto cleanup;
    }
    snprintf(root_dir, sizeof(root_dir), "%s", root_abs);
    free(root_abs);
  } else if (ant_trace_default_root(entry_abs, root_dir, sizeof(root_dir)) != 0) {
    crfprintf(stderr, "{error}: cannot determine pack root for %s\n", entry_abs);
    goto cleanup;
  }

  volatile char stack_base;
  if (!(js = ant_create())) {
    crfprintf(stderr, "{error}: failed to allocate runtime\n");
    goto cleanup;
  }

  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, os_thread_stack_size() * 3 / 4);

  char *init_argv[] = { argv[0], NULL };
  ant_runtime_init(js, 1, init_argv, NULL);
  ant_bootstrap_modules(js);

  if (ant_esm_trace_graph(js, entry_abs, root_dir, &trace) != 0) {
    crfprintf(stderr, "{error}: %s\n", trace.error);
    goto cleanup;
  }
  traced = true;

  for (uint32_t i = 0; i < trace.warning_count; i++) {
    crfprintf(stderr, "{warn}: %s\n", trace.warnings[i]);
  }

  char out_path[4096];
  if (outfile->count > 0) snprintf(out_path, sizeof(out_path), "%s", outfile->filename[0]);
  else compile_default_output(entry_abs, out_path, sizeof(out_path));

  int written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", out_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
    crfprintf(stderr, "{error}: output path is too long\n");
    tmp_path[0] = '\0';
    goto cleanup;
  }

  FILE *out = fopen(tmp_path, "wb");
  if (!out) {
    crfprintf(stderr, "{error}: cannot open %s: %s\n", tmp_path, strerror(errno));
    tmp_path[0] = '\0';
    goto cleanup;
  }

  char err[512] = {0};
  if (compile_write_runtime(out, runtime->count > 0 ? runtime->filename[0] : NULL, no_cache->count > 0, err, sizeof(err)) != 0) {
    crfprintf(stderr, "{error}: %s\n", err);
    fclose(out);
    goto cleanup;
  }

  if (fclose(out) != 0) {
    crfprintf(stderr, "{error}: failed to write %s: %s\n", tmp_path, strerror(errno));
    goto cleanup;
  }

#ifdef __APPLE__
  compile_run_codesign(tmp_path, true);
#endif

  out = fopen(tmp_path, "r+b");
  if (!out) {
    crfprintf(stderr, "{error}: cannot reopen %s: %s\n", tmp_path, strerror(errno));
    goto cleanup;
  }

  ant_bundle_build_module_t *mods = try_oom(sizeof(*mods) * trace.module_count);
  ant_bundle_build_edge_t *edges = trace.edge_count ? try_oom(sizeof(*edges) * trace.edge_count) : NULL;

  for (uint32_t i = 0; i < trace.module_count; i++) {
    mods[i] = (ant_bundle_build_module_t){
      .key = trace.modules[i].key,
      .format = trace.modules[i].format,
      .kind = trace.modules[i].kind,
      .data = trace.modules[i].data,
      .data_len = trace.modules[i].data_len,
    };
  }
  for (uint32_t i = 0; i < trace.edge_count; i++) {
    edges[i] = (ant_bundle_build_edge_t){
      .parent_idx = trace.edges[i].parent_idx,
      .spec = trace.edges[i].spec,
      .child_idx = trace.edges[i].child_idx,
      .is_require = trace.edges[i].is_require,
    };
  }

  ant_bundle_build_t build = {
    .abi_hash = ANT_GIT_LONGHASH,
    .entry_idx = trace.entry_idx,
    .modules = mods,
    .module_count = trace.module_count,
    .edges = edges,
    .edge_count = trace.edge_count,
  };

  int write_rc = ant_bundle_write(out, &build);
  free(mods);
  free(edges);

  long total_size = write_rc == 0 ? ftell(out) : -1;
  if (fclose(out) != 0) write_rc = -1;

  if (write_rc != 0) {
    crfprintf(stderr, "{error}: failed to write %s\n", tmp_path);
    goto cleanup;
  }

#ifdef __APPLE__
  err[0] = '\0';
  if (compile_macho_cover_trailer(tmp_path, err, sizeof(err)) != 0) {
    crfprintf(stderr, "{error}: %s\n", err);
    goto cleanup;
  }

  if (compile_run_codesign(tmp_path, false) != 0) {
#ifdef __aarch64__
    crfprintf(stderr, "{error}: codesign failed for %s; macOS will refuse to run unsigned arm64 binaries\n", tmp_path);
    goto cleanup;
#else
    crfprintf(stderr, "{warn}: codesign failed for %s\n", tmp_path);
#endif
  }
#endif

#ifndef _WIN32
  if (chmod(tmp_path, 0755) != 0) {
    crfprintf(stderr, "{error}: failed to mark %s executable: %s\n", tmp_path, strerror(errno));
    goto cleanup;
  }
#endif

  if (rename(tmp_path, out_path) != 0) {
    crfprintf(stderr, "{error}: failed to write %s: %s\n", out_path, strerror(errno));
    goto cleanup;
  }
  tmp_path[0] = '\0';

  uint64_t embedded_bytes = 0;
  for (uint32_t i = 0; i < trace.module_count; i++) embedded_bytes += trace.modules[i].data_len;

  crprintf("<bold>Compiled</> <bright_green>%s</>\n", out_path);
  if (embedded_bytes < 1024) {
    crprintf("<dim>%u module%s, %llu bytes embedded, %ld KB total</>\n",
      trace.module_count, trace.module_count == 1 ? "" : "s",
      (unsigned long long)embedded_bytes, total_size > 0 ? total_size / 1024 : 0);
  } else {
    crprintf("<dim>%u module%s, %llu KB embedded, %ld KB total</>\n",
      trace.module_count, trace.module_count == 1 ? "" : "s",
      (unsigned long long)(embedded_bytes / 1024), total_size > 0 ? total_size / 1024 : 0);
  }

  rc = EXIT_SUCCESS;

cleanup:
  if (tmp_path[0]) remove(tmp_path);
  if (traced) ant_esm_trace_free(&trace);
  if (js) js_destroy(js);
  
  free(entry_abs);
  free(resolved_entry);
  arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
  
  return rc;
}
