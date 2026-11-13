#include <compat.h> // IWYU pragma: keep

#include "registry.h"
#include "loader_internal.h"
#include "esm/loader.h"
#include "internal.h"
#include "hash.h"
#include "utils.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uv.h>
#include <yyjson.h>

#define REGISTRY_NPMRC_NPM                \
  "registry=https://registry.npmjs.org\n" \
  "@jsr:registry=https://npm.jsr.io\n"

#define REGISTRY_NPMRC_LAND                        \
  "registry=https://npm.ants.land\n"               \
  "fallback-registry=https://registry.npmjs.org\n" \
  "@jsr:registry=https://npm.jsr.io\n"

static constexpr int REGISTRY_SLOT_LIMIT = 8;
static constexpr size_t REGISTRY_NAME_MAX = 256;
static constexpr size_t REGISTRY_REQ_MAX = 128;

typedef struct {
  const char *scheme;
  size_t scheme_len;
  const char *slot;
  const char *npmrc;
  const char *mirror_scope;
} registry_source_t;

typedef struct {
  const registry_source_t *source;
  const char *subpath;
  bool versioned;
  char name[REGISTRY_NAME_MAX];
  char req[REGISTRY_REQ_MAX];
} registry_spec_t;

static const registry_source_t registry_sources[] = {
  { "npm:", 4, "npm", REGISTRY_NPMRC_NPM, NULL },
  { "jsr:", 4, "npm", REGISTRY_NPMRC_NPM, "jsr" },
  { "ants.land:", 10, "land", REGISTRY_NPMRC_LAND, NULL },
  { "land:", 5, "land", REGISTRY_NPMRC_LAND, NULL },
};

static const registry_source_t *registry_source_of(const char *specifier) {
  if (!specifier) return NULL;
  for (size_t i = 0; i < sizeof(registry_sources) / sizeof(registry_sources[0]); i++) {
    const registry_source_t *source = &registry_sources[i];
    if (strncmp(specifier, source->scheme, source->scheme_len) == 0) return source;
  }
  return NULL;
}

bool esm_registry_specifier(const char *specifier) {
  return registry_source_of(specifier) != NULL;
}

static bool registry_name_part_valid(const char *part, size_t len) {
  if (len == 0 || part[0] == '.' || part[0] == '_') return false;
  for (size_t i = 0; i < len; i++) {
    char ch = part[i];
    bool ok =
      (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
      ch == '-' || ch == '_' || ch == '.' || ch == '~';
    if (!ok) return false;
  }
  return true;
}

static bool registry_req_valid(const char *req, size_t len) {
  if (len == 0 || len >= REGISTRY_REQ_MAX) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)req[i];
    if (ch < 0x20 || ch == ':' || ch == '\\' || ch == '"') return false;
  }
  return true;
}

static bool registry_parse(const char *specifier, registry_spec_t *out) {
  const registry_source_t *source = registry_source_of(specifier);
  if (!source) return false;

  const char *rest = specifier + source->scheme_len;
  if (rest[0] == '/') rest++;

  const char *scope = NULL;
  size_t scope_len = 0;
  const char *name = rest;

  if (rest[0] == '@') {
    const char *slash = strchr(rest, '/');
    if (!slash) return false;
    scope = rest + 1;
    scope_len = (size_t)(slash - scope);
    name = slash + 1;
  } else if (source->mirror_scope) return false;

  size_t name_len = strcspn(name, "@/");
  if (scope && !registry_name_part_valid(scope, scope_len)) return false;
  if (!registry_name_part_valid(name, name_len)) return false;

  int written = 0;
  if (source->mirror_scope) written = snprintf(
    out->name, sizeof(out->name), "@%s/%.*s__%.*s",
    source->mirror_scope, (int)scope_len, scope, (int)name_len, name
  );
  else if (scope) written = snprintf(
    out->name, sizeof(out->name), "@%.*s/%.*s",
    (int)scope_len, scope, (int)name_len, name
  );
  else written = snprintf(out->name, sizeof(out->name), "%.*s", (int)name_len, name);
  if (written < 0 || (size_t)written >= sizeof(out->name)) return false;

  const char *tail = name + name_len;
  out->versioned = tail[0] == '@';

  if (out->versioned) {
    const char *req = tail + 1;
    size_t req_len = strcspn(req, "/");
    if (!registry_req_valid(req, req_len)) return false;
    memcpy(out->req, req, req_len);
    out->req[req_len] = '\0';
    tail = req + req_len;
  } else strcpy(out->req, "latest");

  out->source = source;
  out->subpath = tail[0] == '/' && tail[1] != '\0' ? tail + 1 : NULL;
  return true;
}

static const char *registry_manifest_requirement(yyjson_doc *doc, const char *field, const char *name) {
  if (!doc) return NULL;
  yyjson_val *deps = yyjson_obj_get(yyjson_doc_get_root(doc), field);
  return yyjson_get_str(yyjson_obj_get(deps, name));
}

static bool registry_project_declares(yyjson_doc *doc, const char *name) {
  static const char *const fields[] = {
    "dependencies", "devDependencies", "optionalDependencies", "peerDependencies",
  };
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    if (registry_manifest_requirement(doc, fields[i], name)) return true;
  return false;
}

static bool registry_manifest_write(const char *path, yyjson_doc *current, const char *name, const char *req) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return false;

  yyjson_mut_val *root = current ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(current)) : NULL;
  if (!yyjson_mut_is_obj(root)) root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  if (name) {
    yyjson_mut_val *deps = yyjson_mut_obj_get(root, "dependencies");
    if (!yyjson_mut_is_obj(deps)) {
      yyjson_mut_obj_remove_key(root, "dependencies");
      deps = yyjson_mut_obj_add_obj(doc, root, "dependencies");
    }
    yyjson_mut_obj_add_str(doc, deps, name, req);
  }

  char tmp[PATH_MAX];
  int written = snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)uv_os_getpid());
  bool ok = written > 0 && (size_t)written < sizeof(tmp) && yyjson_mut_write_file(tmp, doc, YYJSON_WRITE_PRETTY, NULL, NULL);
  yyjson_mut_doc_free(doc);

  if (!ok) return false;
  if (rename(tmp, path) == 0) return true;
  remove(tmp);
  return false;
}

static bool registry_file_write(const char *path, const char *text) {
  FILE *fp = fopen(path, "wb");
  if (!fp) return false;
  size_t len = strlen(text);
  bool ok = fwrite(text, 1, len, fp) == len;
  return fclose(fp) == 0 && ok;
}

static void registry_file_dump(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) fwrite(buf, 1, n, stderr);
  fclose(fp);
}

static bool registry_installed(const char *dir, const char *name) {
  char path[PATH_MAX];
  int written = snprintf(path, sizeof(path), "%s/node_modules/%s/package.json", dir, name);
  if (written < 0 || (size_t)written >= sizeof(path)) return false;
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void registry_process_exited(uv_process_t *process, int64_t exit_status, int term_signal) {
  bool *ok = process->data;
  *ok = exit_status == 0 && term_signal == 0;
  uv_close((uv_handle_t *)process, NULL);
}

static bool registry_install(ant_t *js, const char *dir, const char *specifier) {
  if (js_esm_bundle_active(js)) return false;

  char exe[PATH_MAX];
  if (ant_get_exe_path(exe, sizeof(exe), js->runtime.argc, js->runtime.argv) != 0 || !exe[0]) return false;

  char log_path[PATH_MAX];
  int written = snprintf(log_path, sizeof(log_path), "%s/install.log", dir);
  if (written < 0 || (size_t)written >= sizeof(log_path)) return false;

  uv_fs_t req;
  int log_fd = uv_fs_open(NULL, &req, log_path, UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644, NULL);
  uv_fs_req_cleanup(&req);
  if (log_fd < 0) return false;

  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) {
    uv_fs_close(NULL, &req, log_fd, NULL);
    uv_fs_req_cleanup(&req);
    return false;
  }

  bool notify = uv_guess_handle(2) == UV_TTY;
  if (notify) {
    fprintf(stderr, "ant: installing %s", specifier);
    fflush(stderr);
  }

  bool ok = false;
  uv_process_t process = { .data = &ok };
  char *args[] = { exe, "install", "--dir", (char *)dir, NULL };

  uv_stdio_container_t stdio[3] = {
    { .flags = UV_IGNORE },
    { .flags = UV_INHERIT_FD, .data.fd = log_fd },
    { .flags = UV_INHERIT_FD, .data.fd = log_fd },
  };

  uv_process_options_t options = {
    .exit_cb = registry_process_exited,
    .file = exe,
    .args = args,
    .stdio = stdio,
    .stdio_count = 3,
    .flags = UV_PROCESS_WINDOWS_HIDE,
  };

  if (uv_spawn(&loop, &process, &options) != 0) uv_close((uv_handle_t *)&process, NULL);
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);

  uv_fs_close(NULL, &req, log_fd, NULL);
  uv_fs_req_cleanup(&req);

  if (notify) {
    fputs("\r\x1b[2K", stderr);
    fflush(stderr);
  }

  if (!ok) registry_file_dump(log_path);
  return ok;
}

static bool registry_slot_add(ant_t *js, const char *dir, yyjson_doc *manifest, const registry_spec_t *spec, const char *specifier) {
  if (js_esm_bundle_active(js) || ant_mkdir_p(dir) != 0) return false;

  char path[PATH_MAX];
  int written = snprintf(path, sizeof(path), "%s/.npmrc", dir);
  if (written < 0 || (size_t)written >= sizeof(path)) return false;
  if (!registry_file_write(path, spec->source->npmrc)) return false;

  snprintf(path, sizeof(path), "%s/package.json", dir);
  if (!registry_manifest_write(path, manifest, spec->name, spec->req)) return false;

  if (registry_install(js, dir, specifier) && registry_installed(dir, spec->name)) return true;
  registry_manifest_write(path, manifest, NULL, NULL);
  return false;
}

static bool registry_slot_prepare(
  ant_t *js, const char *root, const registry_spec_t *spec,
  const char *specifier, char *dir, size_t dir_size
) {
  uint64_t scope = hash_key(root, strlen(root));

  for (int slot = 0; slot < REGISTRY_SLOT_LIMIT; slot++) {
    char suffix[96];
    snprintf(suffix, sizeof(suffix), "imports/%016llx/%s%d", (unsigned long long)scope, spec->source->slot, slot);
    if (ant_xdg_cache_path(dir, dir_size, suffix) != 0) return false;

    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/package.json", dir);
    if (written < 0 || (size_t)written >= sizeof(path)) return false;

    yyjson_doc *manifest = yyjson_read_file(path, 0, NULL, NULL);
    const char *have = registry_manifest_requirement(manifest, "dependencies", spec->name);

    if (have && strcmp(have, spec->req) != 0) {
      yyjson_doc_free(manifest);
      continue;
    }

    bool ok = have
      ? registry_installed(dir, spec->name) ||
        (registry_install(js, dir, specifier) && registry_installed(dir, spec->name))
      : registry_slot_add(js, dir, manifest, spec, specifier);

    yyjson_doc_free(manifest);
    return ok;
  }

  return false;
}

char *esm_registry_resolve(ant_t *js, const char *specifier, const char *base_path, bool prefer_require) {
  registry_spec_t spec;
  if (!registry_parse(specifier, &spec)) return NULL;

  char request[PATH_MAX];
  int written = spec.subpath
    ? snprintf(request, sizeof(request), "%s/%s", spec.name, spec.subpath)
    : snprintf(request, sizeof(request), "%s", spec.name);
  if (written < 0 || (size_t)written >= sizeof(request)) return NULL;

  char *start_dir = esm_get_base_dir(js, base_path);
  if (!start_dir) return NULL;

  char root[PATH_MAX];
  bool owned = false;
  yyjson_doc *project = esm_find_nearest_package_json(js, start_dir, root, sizeof(root), false, &owned);
  bool declared = project && registry_project_declares(project, spec.name);

  if (!project) snprintf(root, sizeof(root), "%s", start_dir);
  if (owned) yyjson_doc_free(project);
  free(start_dir);

  if (declared && !spec.versioned) {
    char *local = esm_resolve_node_module_cond(js, request, base_path, prefer_require);
    if (local) return local;
  }

  char slot[PATH_MAX];
  if (!registry_slot_prepare(js, root, &spec, specifier, slot, sizeof(slot) - sizeof("/package.json"))) return NULL;

  strcat(slot, "/package.json");
  return esm_resolve_node_module_cond(js, request, slot, prefer_require);
}
