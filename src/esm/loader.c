#include <compat.h> // IWYU pragma: keep

#include "esm/loader.h"
#include "esm/commonjs.h"
#include "esm/library.h"
#include "esm/remote.h"
#include "esm/builtin_bundle.h"

#include "modules/json.h"
#include "modules/napi.h"
#include "modules/uri.h"

#include "errors.h"
#include "gc/modules.h"
#include "internal.h"
#include "reactor.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <libgen.h>
#include <unistd.h>
#endif
#include <uthash.h>
#include <yyjson.h>

typedef enum {
  ESM_MODULE_KIND_CODE = 0,
  ESM_MODULE_KIND_JSON,
  ESM_MODULE_KIND_TEXT,
  ESM_MODULE_KIND_IMAGE,
  ESM_MODULE_KIND_NATIVE,
  ESM_MODULE_KIND_URL,
} esm_module_kind_t;

typedef struct esm_module {
  char *path;
  char *cache_key;
  char *resolved_path;
  char *url_content;
  
  size_t url_content_len;
  const uint8_t *embedded_code;
  size_t embedded_code_len;
  
  ant_value_t namespace_obj;
  ant_value_t default_export;
  ant_value_t tla_promise;
  UT_hash_handle hh;
  esm_module_kind_t kind;
  ant_module_format_t format;
  
  bool is_loaded;
  bool is_loading;
  bool has_tla;
} esm_module_t;

typedef struct {
  esm_module_t *modules;
  int count;
} esm_module_cache_t;

typedef struct {
  char *data;
  size_t size;
} esm_file_data_t;

static esm_module_cache_t global_module_cache = {NULL, 0};
static int esm_dynamic_import_depth = 0;

static char *esm_resolve_node_module(const char *specifier, const char *base_path);
static char *esm_canonicalize_path(const char *path);

static char *esm_file_url_to_path(ant_t *js, const char *specifier) {
  if (!specifier || strncmp(specifier, "file:", 5) != 0) return NULL;

  const char *p = specifier + 5;
  if (strncmp(p, "///", 3) == 0) p += 2;
  else if (strncmp(p, "//localhost/", 12) == 0) p += 11;

  if (*p == '\0') return NULL;

  ant_value_t encoded = js_mkstr(js, p, strlen(p));
  ant_value_t decoded = js_decodeURI(js, &encoded, 1);

  size_t len = 0;
  char *str = js_getstr(js, decoded, &len);
  
  return str ? strndup(str, len) : NULL;
}

static char *esm_make_cache_key(const char *module_key) {
  if (!module_key) return NULL;
  if (esm_has_builtin_scheme(module_key)) return strdup(module_key);
  if (esm_is_data_url(module_key)) return strdup(module_key);
  if (esm_is_url(module_key)) return strdup(module_key);
  return esm_canonicalize_path(module_key);
}

static char *esm_get_extension(const char *path) {
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');

  if (dot && (!slash || dot > slash)) {
    return strdup(dot);
  }
  return strdup(".js");
}

static bool esm_is_relative_specifier(const char *specifier) {
  return 
    strcmp(specifier, ".") == 0 ||
    strcmp(specifier, "..") == 0 ||
    strncmp(specifier, "./", 2) == 0 ||
    strncmp(specifier, "../", 3) == 0;
}

static char *esm_try_resolve(const char *dir, const char *spec, const char *suffix) {
  char path[PATH_MAX];
  snprintf(path, PATH_MAX, "%s/%s%s", dir, spec, suffix);
  char *resolved = realpath(path, NULL);
  if (resolved) {
    struct stat st;
    if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) return resolved;
    free(resolved);
  }
  return NULL;
}

static bool esm_has_extension(const char *spec) {
  const char *slash = strrchr(spec, '/');
  const char *dot = strrchr(slash ? slash : spec, '.');
  
  if (!dot) return false;
  for (
    const char *const *ext = module_resolve_extensions; 
    *ext; ext++
  ) if (strcmp(dot, *ext) == 0) return true;
  
  return false;
}

static char *esm_try_resolve_with_exts(const char *dir, const char *spec, bool has_ext) {
  const char *const *exts = module_resolve_extensions;
  char *result = NULL;

  if ((result = esm_try_resolve(dir, spec, ""))) return result;
  if (has_ext) return NULL;

  for (int i = 0; exts[i]; i++) {
    if ((result = esm_try_resolve(dir, spec, exts[i]))) return result;
  }
  return NULL;
}

static char *esm_try_resolve_index_with_exts(const char *dir, const char *spec) {
  char idx[PATH_MAX];
  snprintf(idx, sizeof(idx), "%s/index", spec);
  return esm_try_resolve_with_exts(dir, idx, false);
}

static char *esm_try_resolve_relative_typescript_source_fallback(
  const char *dir,
  const char *spec,
  const char *base_path
) {
  if (!is_typescript_file(base_path)) return NULL;

  char *ts_spec = resolve_typescript_source_fallback(spec);
  if (!ts_spec) return NULL;

  char *resolved = esm_try_resolve(dir, ts_spec, "");
  free(ts_spec);
  return resolved;
}

static ant_value_t esm_default_export_or_namespace(ant_t *js, ant_value_t ns) {
  ant_value_t default_val = js_get_slot(ns, SLOT_DEFAULT);
  return vtype(default_val) != T_UNDEF ? default_val : ns;
}

static ant_value_t esm_make_namespace_object(ant_t *js) {
  ant_value_t ns = js_mkobj(js);
  js_set_slot(ns, SLOT_BRAND, js_mknum(BRAND_MODULE_NAMESPACE));
  js_set_slot(ns, SLOT_MODULE_LOADING, js_true);
  return ns;
}

static ant_value_t esm_complete_value_module(esm_module_t *mod, ant_value_t value) {
  if (is_err(value)) {
    mod->is_loading = false;
    return value;
  }
  mod->namespace_obj = value;
  mod->default_export = value;
  mod->is_loaded = true;
  mod->is_loading = false;
  return value;
}

static ant_value_t esm_complete_namespace_module(ant_t *js, esm_module_t *mod, ant_value_t ns) {
  mod->namespace_obj = ns;
  mod->default_export = esm_default_export_or_namespace(js, ns);
  mod->is_loaded = true;
  mod->is_loading = false;
  js_set_slot(ns, SLOT_MODULE_LOADING, js_mkundef());
  return ns;
}

static ant_value_t esm_prepare_eval_ctx(
  ant_t *js,
  const char *resolved_path,
  ant_value_t ns,
  ant_module_format_t format,
  bool is_main,
  const char *fallback_parent_path,
  ant_module_t *out_ctx
) {
  ant_value_t module_ctx = js_create_module_context(js, resolved_path, is_main);
  
  if (is_err(module_ctx)) return module_ctx;
  if (is_object_type(ns)) js_set_slot_wb(js, ns, SLOT_MODULE_CTX, module_ctx);

  *out_ctx = (ant_module_t){
    .module_ns = ns,
    .module_ctx = module_ctx,
    .prev_import_meta_prop = js_mkundef(),
    .format = format,
    .prev = NULL,
  };
  
  return js_mkundef();
}

static char *esm_try_resolve_from_extension_list(
  const char *dir,
  const char *spec,
  const char *first_ext,
  const char *skip_ext
) {
  char *result = NULL;
  if (first_ext && first_ext[0]) {
    if ((result = esm_try_resolve(dir, spec, first_ext))) return result;
  }

  const char *const *exts = module_resolve_extensions;
  for (int i = 0; exts[i]; i++) {
    if (skip_ext && strcmp(skip_ext, exts[i]) == 0) continue;
    if ((result = esm_try_resolve(dir, spec, exts[i]))) return result;
  }
  return NULL;
}

static char *esm_resolve_absolute(const char *specifier) {
  char *result = esm_try_resolve_with_exts("", specifier, esm_has_extension(specifier));
  if (result) return result;
  return esm_try_resolve_index_with_exts("", specifier);
}

static char *esm_get_base_dir(const char *base_path) {
  if (!base_path || !base_path[0]) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    return strdup(cwd);
  }

  char candidate[PATH_MAX];
  if (base_path[0] == '/') {
    snprintf(candidate, sizeof(candidate), "%s", base_path);
  } else {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    snprintf(candidate, sizeof(candidate), "%s/%s", cwd, base_path);
  }

  char *resolved = realpath(candidate, NULL);
  const char *resolved_or_candidate = resolved ? resolved : candidate;

  struct stat st;
  if (stat(resolved_or_candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
    char *out = realpath(resolved_or_candidate, NULL);
    if (!out) out = strdup(resolved_or_candidate);
    if (resolved) free(resolved);
    return out;
  }

  char *tmp = strdup(resolved_or_candidate);
  if (resolved) free(resolved);
  if (!tmp) return NULL;

  char *dir = dirname(tmp);
  char *out = realpath(dir, NULL);
  if (!out) out = strdup(dir);
  
  free(tmp);
  return out;
}

static bool esm_split_package_specifier(
  const char *specifier,
  char *package_name,
  size_t package_name_size,
  const char **subpath_out
) {
  if (!specifier || !specifier[0]) return false;
  if (specifier[0] == '.' || specifier[0] == '/' || specifier[0] == '#') return false;

  const char *slash = NULL;
  if (specifier[0] == '@') {
    const char *first = strchr(specifier, '/');
    if (!first || first == specifier + 1) return false;
    slash = strchr(first + 1, '/');
    if (first[1] == '\0') return false;
    if (!slash) {
      if (strlen(specifier) >= package_name_size) return false;
      strcpy(package_name, specifier);
      *subpath_out = NULL;
      return true;
    }
  } else slash = strchr(specifier, '/');

  size_t name_len = slash ? (size_t)(slash - specifier) : strlen(specifier);
  if (name_len == 0 || name_len >= package_name_size) return false;
  memcpy(package_name, specifier, name_len);
  package_name[name_len] = '\0';

  if (slash && slash[1] != '\0') *subpath_out = slash + 1;
  else *subpath_out = NULL;
  
  return true;
}

static char *esm_find_node_module_dir(const char *start_dir, const char *package_name) {
  if (!start_dir || !package_name) return NULL;

  char current[PATH_MAX];
  snprintf(current, sizeof(current), "%s", start_dir);

  while (true) {
    char candidate[PATH_MAX];
    snprintf(candidate, sizeof(candidate), "%s/node_modules/%s", current, package_name);

    struct stat st;
    if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
      char *resolved = realpath(candidate, NULL);
      return resolved ? resolved : strdup(candidate);
    }

    if (strcmp(current, "/") == 0) break;
    char *slash = strrchr(current, '/');
    
    if (!slash) break;
    if (slash == current) current[1] = '\0';
    else *slash = '\0';
  }
  
  return NULL;
}

static bool esm_matches_pattern_key(const char *key, const char *request, const char **capture, size_t *capture_len) {
  const char *star = strchr(key, '*');
  if (!star) return false;

  size_t key_len = strlen(key);
  size_t req_len = strlen(request);
  size_t prefix_len = (size_t)(star - key);
  size_t suffix_len = key_len - prefix_len - 1;
  
  if (req_len < prefix_len + suffix_len) return false;
  if (strncmp(request, key, prefix_len) != 0) return false;
  if (suffix_len > 0 && strcmp(request + req_len - suffix_len, star + 1) != 0) return false;

  *capture = request + prefix_len;
  *capture_len = req_len - prefix_len - suffix_len;
  
  return true;
}

static char *esm_replace_star(const char *pattern, const char *capture, size_t capture_len) {
  const char *star = strchr(pattern, '*');
  if (!star) return strdup(pattern);

  size_t prefix_len = (size_t)(star - pattern);
  size_t suffix_len = strlen(star + 1);
  size_t out_len = prefix_len + capture_len + suffix_len;
  char *out = (char *)malloc(out_len + 1);
  if (!out) return NULL;

  memcpy(out, pattern, prefix_len);
  memcpy(out + prefix_len, capture, capture_len);
  memcpy(out + prefix_len + capture_len, star + 1, suffix_len);
  out[out_len] = '\0';
  
  return out;
}

static char *esm_resolve_exports_target(
  yyjson_val *target,
  const char *package_dir,
  const char *capture,
  size_t capture_len,
  const char *base_path,
  bool allow_bare_specifiers,
  bool prefer_require
) {
  if (!target) return NULL;

  if (yyjson_is_str(target)) {
    const char *target_str = yyjson_get_str(target);
    if (!target_str || !target_str[0]) return NULL;

    if (target_str[0] == '.' && target_str[1] == '/') {
      char *mapped = esm_replace_star(target_str + 2, capture, capture_len);
      if (!mapped) return NULL;

      char *resolved = esm_try_resolve_with_exts(package_dir, mapped, esm_has_extension(mapped));
      if (!resolved) resolved = esm_try_resolve_index_with_exts(package_dir, mapped);
      free(mapped);
      return resolved;
    }

    if (!allow_bare_specifiers) return NULL;
    if (target_str[0] == '#') return NULL;
    return esm_resolve_node_module(target_str, base_path);
  }

  if (yyjson_is_arr(target)) {
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(target, idx, max, item) {
      char *resolved = esm_resolve_exports_target(
        item, package_dir, capture,
        capture_len, base_path, allow_bare_specifiers, prefer_require
      );
      if (resolved) return resolved;
    }
    return NULL;
  }

  if (yyjson_is_obj(target)) {
    static const char *const import_conditions[] = {"import", "node", "default"};
    static const char *const require_conditions[] = {"require", "node", "default"};
    
    const char *const *conditions = prefer_require 
      ? require_conditions 
      : import_conditions;
      
    size_t condition_count = prefer_require
      ? sizeof(require_conditions) / sizeof(require_conditions[0])
      : sizeof(import_conditions) / sizeof(import_conditions[0]);
      
    for (size_t i = 0; i < condition_count; i++) {
      yyjson_val *cond_target = yyjson_obj_get(target, conditions[i]);
      if (!cond_target) continue;
      char *resolved = esm_resolve_exports_target(
        cond_target, package_dir, capture,
        capture_len, base_path, allow_bare_specifiers, prefer_require
      );
      if (resolved) return resolved;
    }
  }

  return NULL;
}

static char *esm_resolve_package_map(
  yyjson_val *map_obj,
  const char *request_key,
  const char *package_dir,
  const char *base_path,
  bool allow_bare_specifiers,
  bool prefer_require
) {
  if (!map_obj || !yyjson_is_obj(map_obj)) return NULL;

  yyjson_val *exact = yyjson_obj_get(map_obj, request_key);
  if (exact) return esm_resolve_exports_target(
    exact, package_dir, "", 0, 
    base_path, allow_bare_specifiers, prefer_require
  );

  const char *best_capture = NULL;
  size_t best_capture_len = 0;
  
  yyjson_val *best_target = NULL;
  size_t best_prefix_len = 0;

  size_t idx, max;
  yyjson_val *k, *v;
  
  yyjson_obj_foreach(map_obj, idx, max, k, v) {
    if (!yyjson_is_str(k)) continue;
    const char *key = yyjson_get_str(k);
    if (!key) continue;

    const char *capture = NULL;
    size_t capture_len = 0;
    if (!esm_matches_pattern_key(key, request_key, &capture, &capture_len)) continue;

    const char *star = strchr(key, '*');
    size_t prefix_len = (size_t)(star - key);
    if (best_target && prefix_len < best_prefix_len) continue;

    best_target = v;
    best_capture = capture;
    best_capture_len = capture_len;
    best_prefix_len = prefix_len;
  }

  if (!best_target) return NULL;
  return esm_resolve_exports_target(
    best_target, package_dir, best_capture,
    best_capture_len, base_path, allow_bare_specifiers, prefer_require
  );
}

static char *esm_resolve_package_main_entry(yyjson_val *root, const char *package_dir) {
  if (!root || !yyjson_is_obj(root)) return NULL;

  yyjson_val *main = yyjson_obj_get(root, "main");
  if (!main || !yyjson_is_str(main)) return NULL;

  const char *main_str = yyjson_get_str(main);
  if (!main_str || !main_str[0]) return NULL;

  char *resolved = esm_try_resolve_with_exts(package_dir, main_str, esm_has_extension(main_str));
  if (!resolved) resolved = esm_try_resolve_index_with_exts(package_dir, main_str);
  return resolved;
}

static char *esm_resolve_package_entrypoint(const char *package_dir, const char *subpath, const char *base_path, bool prefer_require) {
  char pkg_json_path[PATH_MAX];
  snprintf(pkg_json_path, sizeof(pkg_json_path), "%s/package.json", package_dir);

  yyjson_doc *doc = yyjson_read_file(pkg_json_path, 0, NULL, NULL);
  yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
  yyjson_val *exports = (root && yyjson_is_obj(root)) ? yyjson_obj_get(root, "exports") : NULL;

  if (exports) {
    char subpath_key[PATH_MAX];
    if (subpath && subpath[0]) snprintf(subpath_key, sizeof(subpath_key), "./%s", subpath);
    else snprintf(subpath_key, sizeof(subpath_key), ".");

    char *resolved = NULL;
    if (yyjson_is_obj(exports)) {
      bool has_subpath_keys = false;
      size_t idx, max;
      yyjson_val *k, *v;
      yyjson_obj_foreach(exports, idx, max, k, v) {
        if (!yyjson_is_str(k)) continue;
        const char *key = yyjson_get_str(k);
        if (key && key[0] == '.') { has_subpath_keys = true; break; }
      }
      if (has_subpath_keys) resolved = esm_resolve_package_map(exports, subpath_key, package_dir, base_path, false, prefer_require);
      else if (!subpath || !subpath[0]) resolved = esm_resolve_exports_target(exports, package_dir, "", 0, base_path, false, prefer_require);
    } else if (!subpath || !subpath[0]) resolved = esm_resolve_exports_target(exports, package_dir, "", 0, base_path, false, prefer_require);

    if (doc) yyjson_doc_free(doc);
    return resolved;
  }

  if (!subpath || !subpath[0]) {
    char *resolved = esm_resolve_package_main_entry(root, package_dir);
    if (resolved) {
      if (doc) yyjson_doc_free(doc);
      return resolved;
    }
    if (doc) yyjson_doc_free(doc);
    return esm_try_resolve_index_with_exts(package_dir, ".");
  }

  char *resolved = esm_try_resolve_with_exts(package_dir, subpath, esm_has_extension(subpath));
  if (!resolved) resolved = esm_try_resolve_index_with_exts(package_dir, subpath);
  if (doc) yyjson_doc_free(doc);
  return resolved;
}

static char *esm_resolve_package_imports(const char *specifier, const char *base_path, bool prefer_require) {
  char *start_dir = esm_get_base_dir(base_path);
  if (!start_dir) return NULL;

  char current[PATH_MAX];
  snprintf(current, sizeof(current), "%s", start_dir);
  free(start_dir);

  while (true) {
    char pkg_json_path[PATH_MAX];
    snprintf(pkg_json_path, sizeof(pkg_json_path), "%s/package.json", current);

    yyjson_doc *doc = yyjson_read_file(pkg_json_path, 0, NULL, NULL);
    if (doc) {
      yyjson_val *root = yyjson_doc_get_root(doc);
      yyjson_val *imports = (root && yyjson_is_obj(root)) ? yyjson_obj_get(root, "imports") : NULL;
      char *resolved = NULL;
      if (imports && yyjson_is_obj(imports)) {
        resolved = esm_resolve_package_map(
          imports, specifier, current,
          base_path, true, prefer_require
        );
      }
      yyjson_doc_free(doc);
      return resolved;
    }

    if (strcmp(current, "/") == 0) break;
    char *slash = strrchr(current, '/');
    if (!slash) break;
    if (slash == current) current[1] = '\0';
    else *slash = '\0';
  }

  return NULL;
}

static char *esm_resolve_node_module_cond(const char *specifier, const char *base_path, bool prefer_require) {
  char package_name[PATH_MAX];
  const char *subpath = NULL;
  if (!esm_split_package_specifier(specifier, package_name, sizeof(package_name), &subpath)) {
    return NULL;
  }

  char *start_dir = esm_get_base_dir(base_path);
  if (!start_dir) return NULL;

  char *package_dir = esm_find_node_module_dir(start_dir, package_name);
  free(start_dir);
  if (!package_dir) return NULL;

  char *resolved = esm_resolve_package_entrypoint(package_dir, subpath, base_path, prefer_require);
  free(package_dir);
  return resolved;
}

static char *esm_resolve_node_module(const char *specifier, const char *base_path) {
  return esm_resolve_node_module_cond(specifier, base_path, false);
}

static char *esm_resolve_relative_path(const char *specifier, const char *base_path) {
  char *base_copy = strdup(base_path);
  if (!base_copy) return NULL;
  char *dir = dirname(base_copy);
  char *result = NULL;

  const char *spec = specifier;
  if (strncmp(specifier, "./", 2) == 0) spec = specifier + 2;
  
  bool has_ext = esm_has_extension(spec);
  if ((result = esm_try_resolve(dir, spec, ""))) goto cleanup;
  
  if (has_ext) {
    result = esm_try_resolve_relative_typescript_source_fallback(dir, spec, base_path);
    goto cleanup;
  }

  char *base_ext = esm_get_extension(base_path);
  if (!base_ext) goto cleanup;

  if ((result = esm_try_resolve_from_extension_list(dir, spec, base_ext, base_ext))) goto cleanup_ext;

  char idx[PATH_MAX];
  snprintf(idx, sizeof(idx), "%s/index%s", spec, base_ext);
  if ((result = esm_try_resolve(dir, idx, ""))) goto cleanup_ext;

  snprintf(idx, sizeof(idx), "%s/index", spec);
  if ((result = esm_try_resolve_from_extension_list(dir, idx, base_ext, base_ext))) goto cleanup_ext;

  cleanup_ext: {
    free(base_ext);
  }
  
  cleanup: {
    free(base_copy);
    return result;
  }
}

static char *esm_resolve_path_cond(const char *specifier, const char *base_path, bool prefer_require) {
  if (!specifier || !specifier[0]) return NULL;

  if (specifier[0] == '/') {
    return esm_resolve_absolute(specifier);
  }

  if (esm_is_relative_specifier(specifier)) {
    return esm_resolve_relative_path(specifier, base_path);
  }

  if (specifier[0] == '#') {
    return esm_resolve_package_imports(specifier, base_path, prefer_require);
  }

  return esm_resolve_node_module_cond(specifier, base_path, prefer_require);
}

static char *esm_resolve_path(const char *specifier, const char *base_path) {
  return esm_resolve_path_cond(specifier, base_path, false);
}

static char *esm_resolve_path_require(const char *specifier, const char *base_path) {
  return esm_resolve_path_cond(specifier, base_path, true);
}

static bool esm_has_suffix(const char *path, const char *ext) {
  size_t len = strlen(path);
  size_t elen = strlen(ext);
  return len > elen && strcmp(path + len - elen, ext) == 0;
}

static inline bool esm_is_json(const char *path) {
  return esm_has_suffix(path, ".json");
}

static inline bool esm_is_text(const char *path) {
  return 
    esm_has_suffix(path, ".txt") ||
    esm_has_suffix(path, ".md") ||
    esm_has_suffix(path, ".html") ||
    esm_has_suffix(path, ".css");
}

static inline bool esm_is_image(const char *path) {
  return
    esm_has_suffix(path, ".png") ||
    esm_has_suffix(path, ".jpg") ||
    esm_has_suffix(path, ".jpeg") ||
    esm_has_suffix(path, ".gif") ||
    esm_has_suffix(path, ".svg") ||
    esm_has_suffix(path, ".webp");
}

static inline bool esm_is_native(const char *path) {
  return esm_has_suffix(path, ".node");
}

static inline bool esm_is_cjs_extension(const char *path) {
  return
    esm_has_suffix(path, ".cjs") ||
    esm_has_suffix(path, ".cts");
}

static inline bool esm_is_esm_extension(const char *path) {
  return
    esm_has_suffix(path, ".mjs") ||
    esm_has_suffix(path, ".mts");
}

static bool esm_path_contains_node_modules(const char *path) {
  if (!path) return false;
  if (strstr(path, "/node_modules/")) return true;
  return strstr(path, "\\node_modules\\") != NULL;
}

static bool esm_read_package_json_type_module(const char *pkg_json_path, bool *has_type) {
  if (has_type) *has_type = false;

  yyjson_doc *doc = yyjson_read_file(pkg_json_path, 0, NULL, NULL);
  if (!doc) return false;

  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *type = (root && yyjson_is_obj(root)) ? yyjson_obj_get(root, "type") : NULL;

  if (!type || !yyjson_is_str(type)) {
    yyjson_doc_free(doc);
    return false;
  }

  const char *type_str = yyjson_get_str(type);
  if (has_type) *has_type = true;

  bool is_module = type_str && strcmp(type_str, "module") == 0;
  yyjson_doc_free(doc);
  
  return is_module;
}

static bool esm_lookup_package_type_module(const char *resolved_path, bool *is_module) {
  if (is_module) *is_module = false;
  if (!resolved_path || !resolved_path[0]) return false;

  char path_copy[PATH_MAX];
  snprintf(path_copy, sizeof(path_copy), "%s", resolved_path);
  char *dir = dirname(path_copy);
  if (!dir || !dir[0]) return false;

  char current[PATH_MAX];
  snprintf(current, sizeof(current), "%s", dir);

  while (true) {
    char pkg_json_path[PATH_MAX];
    snprintf(pkg_json_path, sizeof(pkg_json_path), "%s/package.json", current);

    struct stat st;
    if (stat(pkg_json_path, &st) == 0 && S_ISREG(st.st_mode)) {
      bool has_type = false;
      bool pkg_is_module = esm_read_package_json_type_module(pkg_json_path, &has_type);
      if (is_module) *is_module = has_type && pkg_is_module;
      return true;
    }

    if (strcmp(current, "/") == 0) break;
    char *slash = strrchr(current, '/');
    if (!slash) break;
    if (slash == current) current[1] = '\0';
    else *slash = '\0';
  }

  return false;
}

static ant_module_format_t esm_decide_module_format(const char *resolved_path) {
  if (!resolved_path || !resolved_path[0]) return MODULE_EVAL_FORMAT_ESM;
  if (esm_is_cjs_extension(resolved_path)) return MODULE_EVAL_FORMAT_CJS;
  if (esm_is_esm_extension(resolved_path)) return MODULE_EVAL_FORMAT_ESM;

  if (esm_has_suffix(resolved_path, ".js")) {
    bool pkg_is_module = false;
    bool has_package_json = esm_lookup_package_type_module(resolved_path, &pkg_is_module);
    if (!has_package_json) return MODULE_EVAL_FORMAT_CJS;
    return pkg_is_module ? MODULE_EVAL_FORMAT_ESM : MODULE_EVAL_FORMAT_CJS;
  }

  return MODULE_EVAL_FORMAT_ESM;
}

static ant_value_t esm_eval_module_with_format(
  ant_t *js,
  const char *resolved_path,
  const char *js_code,
  size_t js_len,
  ant_value_t ns,
  ant_module_format_t format
) {
  if (format == MODULE_EVAL_FORMAT_CJS) {
    return esm_load_commonjs_module(js, resolved_path, js_code, js_len, ns);
  }
  return js_eval_bytecode_module(js, js_code, js_len);
}

ant_value_t js_esm_eval_module_source(
  ant_t *js,
  const char *resolved_path, const char *js_code,
  size_t js_len, ant_value_t ns
) {
  ant_module_format_t format = esm_decide_module_format(resolved_path);
  ant_module_t eval_ctx;
  
  ant_value_t prep_res = esm_prepare_eval_ctx(
    js, resolved_path, ns, format, 
    js->module == NULL, NULL, &eval_ctx
  );
  
  if (is_err(prep_res)) return prep_res;
  js_module_eval_ctx_push(js, &eval_ctx);
  
  ant_value_t result = esm_eval_module_with_format(
    js, resolved_path, js_code, 
    js_len, ns, format
  );

  js_module_eval_ctx_pop(js, &eval_ctx);
  return result;
}

static esm_module_kind_t esm_classify_module_kind(const char *resolved_path) {
  if (esm_is_data_url(resolved_path)) return ESM_MODULE_KIND_URL;
  if (esm_is_url(resolved_path)) return ESM_MODULE_KIND_URL;
  if (esm_is_json(resolved_path)) return ESM_MODULE_KIND_JSON;
  if (esm_is_text(resolved_path)) return ESM_MODULE_KIND_TEXT;
  if (esm_is_image(resolved_path)) return ESM_MODULE_KIND_IMAGE;
  if (esm_is_native(resolved_path)) return ESM_MODULE_KIND_NATIVE;
  return ESM_MODULE_KIND_CODE;
}

static char *esm_canonicalize_path(const char *path) {
  if (!path) return NULL;

  char *canonical = strdup(path);
  if (!canonical) return NULL;

  char *src = canonical, *dst = canonical;

  while (*src) {
    if (*src == '/') {
      *dst++ = '/';
      while (*src == '/') src++;

      if (strncmp(src, "./", 2) == 0) {
        src += 2;
      } else if (strncmp(src, "../", 3) == 0) {
        src += 3;
        if (dst > canonical + 1) {
          dst--;
          while (dst > canonical && *(dst - 1) != '/') dst--;
        }
      }
    } else {
      *dst++ = *src++;
    }
  }

  *dst = '\0';

  if (strlen(canonical) > 1 && canonical[strlen(canonical) - 1] == '/') {
    canonical[strlen(canonical) - 1] = '\0';
  }

  return canonical;
}

static esm_module_t *esm_find_module(const char *module_key) {
  char *cache_key = esm_make_cache_key(module_key);
  if (!cache_key) return NULL;

  esm_module_t *mod = NULL;
  HASH_FIND_STR(global_module_cache.modules, cache_key, mod);

  free(cache_key);
  return mod;
}

static esm_module_t *esm_create_module(
  const char *path,
  const char *resolved_path,
  const char *module_key,
  ant_module_format_t format,
  const uint8_t *embedded_code,
  size_t embedded_code_len
) {
  char *cache_key = esm_make_cache_key(module_key);
  if (!cache_key) return NULL;

  esm_module_t *existing_mod = NULL;
  HASH_FIND_STR(global_module_cache.modules, cache_key, existing_mod);
  if (existing_mod) {
    free(cache_key);
    return existing_mod;
  }

  esm_module_t *mod = (esm_module_t *)malloc(sizeof(esm_module_t));
  if (!mod) {
    free(cache_key);
    return NULL;
  }

  *mod = (esm_module_t){
    .path = strdup(path),
    .cache_key = cache_key,
    .resolved_path = strdup(resolved_path),
    .namespace_obj = js_mkundef(),
    .default_export = js_mkundef(),
    .is_loaded = false,
    .is_loading = false,
    .kind = esm_classify_module_kind(resolved_path),
    .format = format,
    .url_content = NULL,
    .url_content_len = 0,
    .embedded_code = embedded_code,
    .embedded_code_len = embedded_code_len,
    .tla_promise = js_mkundef(),
    .has_tla = false,
  };
  
  if (!mod->path || !mod->resolved_path) {
    free(mod->path);
    free(mod->cache_key);
    free(mod->resolved_path);
    free(mod);
    return NULL;
  }
  
  HASH_ADD_STR(global_module_cache.modules, cache_key, mod);
  global_module_cache.count++;

  return mod;
}

void js_esm_cleanup_module_cache(void) {
  esm_module_t *current, *tmp;
  HASH_ITER(hh, global_module_cache.modules, current, tmp) {
    HASH_DEL(global_module_cache.modules, current);
    if (current->path) free(current->path);
    if (current->cache_key) free(current->cache_key);
    if (current->resolved_path) free(current->resolved_path);
    if (current->url_content) free(current->url_content);
    free(current);
  }
  global_module_cache.count = 0;
}

static ant_value_t esm_read_file(ant_t *js, const char *path, const char *kind, esm_file_data_t *out) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return js_mkerr(js, "Cannot open %s: %s", kind, path);

  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  char *buf = (char *)malloc((size_t)fsize + 1);
  if (!buf) {
    fclose(fp);
    return js_mkerr(js, "OOM loading %s", kind);
  }

  fread(buf, 1, (size_t)fsize, fp);
  fclose(fp);
  buf[fsize] = '\0';

  out->data = buf;
  out->size = (size_t)fsize;
  return js_mkundef();
}

static ant_value_t esm_load_json(ant_t *js, const char *path) {
  esm_file_data_t file;
  ant_value_t err = esm_read_file(js, path, "JSON file", &file);
  if (is_err(err)) return err;

  ant_value_t json_str = js_mkstr(js, file.data, file.size);
  free(file.data);
  return json_parse_value(js, json_str);
}

static ant_value_t esm_load_text(ant_t *js, const char *path) {
  esm_file_data_t file;
  ant_value_t err = esm_read_file(js, path, "text file", &file);
  if (is_err(err)) return err;

  ant_value_t result = js_mkstr(js, file.data, file.size);
  free(file.data);
  return result;
}

static ant_value_t esm_load_image(ant_t *js, const char *path) {
  esm_file_data_t file;
  ant_value_t err = esm_read_file(js, path, "image file", &file);
  if (is_err(err)) return err;

  unsigned char *content = (unsigned char *)file.data;
  size_t size = file.size;

  ant_value_t obj = js_mkobj(js);
  ant_value_t data_arr = js_mkarr(js);

  for (size_t i = 0; i < size; i++) {
    js_arr_push(js, data_arr, tov((double)content[i]));
  }

  js_setprop(js, obj, js_mkstr(js, "data", 4), data_arr);
  js_setprop(js, obj, js_mkstr(js, "path", 4), js_mkstr(js, path, strlen(path)));
  js_setprop(js, obj, js_mkstr(js, "size", 4), tov((double)size));

  free(file.data);
  return obj;
}

static ant_value_t esm_load_module(ant_t *js, esm_module_t *mod) {
  if (mod->is_loaded) return mod->namespace_obj;
  if (mod->is_loading) return mod->namespace_obj;

  mod->is_loading = true;

  switch (mod->kind) {
    case ESM_MODULE_KIND_JSON: {
      ant_value_t json_val = esm_load_json(js, mod->resolved_path);
      return esm_complete_value_module(mod, json_val);
    }
    case ESM_MODULE_KIND_TEXT: {
      ant_value_t text_val = esm_load_text(js, mod->resolved_path);
      return esm_complete_value_module(mod, text_val);
    }
    case ESM_MODULE_KIND_IMAGE: {
      ant_value_t img_val = esm_load_image(js, mod->resolved_path);
      return esm_complete_value_module(mod, img_val);
    }
    case ESM_MODULE_KIND_NATIVE: {
      ant_value_t ns = esm_make_namespace_object(js);
      mod->namespace_obj = ns;
      
      ant_value_t module_ctx = js_create_module_context(js, mod->resolved_path, false);
      if (is_err(module_ctx)) {
        mod->is_loading = false;
        return module_ctx;
      }
      js_set_slot_wb(js, ns, SLOT_MODULE_CTX, module_ctx);
      
      ant_value_t native_exports = napi_load_native_module(js, mod->resolved_path, ns);
      if (is_err(native_exports)) {
        mod->is_loading = false;
        return native_exports;
      }
      return esm_complete_namespace_module(js, mod, ns);
    }
    case ESM_MODULE_KIND_CODE:
    case ESM_MODULE_KIND_URL: break;
  }

  char *content = NULL;
  size_t size = 0;

  if (mod->embedded_code) {
    content = (char *)malloc(mod->embedded_code_len + 1);
    if (!content) {
      mod->is_loading = false;
      return js_mkerr(js, "OOM loading bundled module");
    }
    memcpy(content, mod->embedded_code, mod->embedded_code_len);
    size = mod->embedded_code_len;
  } else if (mod->kind == ESM_MODULE_KIND_URL && esm_is_data_url(mod->resolved_path)) {
    content = esm_parse_data_url(mod->resolved_path, &size);
    if (!content) {
      mod->is_loading = false;
      return js_mkerr(js, "Cannot parse data URL module");
    }
  } else if (mod->kind == ESM_MODULE_KIND_URL) {
    if (mod->url_content) {
      content = strdup(mod->url_content);
      size = mod->url_content_len;
    } else {
      char *error = NULL;
      content = esm_fetch_url(mod->resolved_path, &size, &error);
      if (!content) {
        mod->is_loading = false;
        ant_value_t err = js_mkerr(js, "Cannot fetch module %s: %s", mod->resolved_path, error ? error : "unknown error");
        if (error) free(error);
        return err;
      }
      mod->url_content = strdup(content);
      mod->url_content_len = size;
    }
  } else {
    esm_file_data_t file;
    ant_value_t err = esm_read_file(js, mod->resolved_path, "module", &file);
    if (is_err(err)) {
      mod->is_loading = false;
      return err;
    }
    content = file.data;
    size = file.size;
  }
  content[size] = '\0';

  size_t js_len = size;
  const char *strip_detail = NULL;

  if (!mod->embedded_code) {
  int strip_result = strip_typescript_inplace(
    &content, size, mod->resolved_path, &js_len, &strip_detail
  );

  if (strip_result < 0) {
    ant_value_t err = js_mkerr(
      js, "TypeScript error: strip failed (%d): %s",
      strip_result, strip_detail
    );
    free(content);
    mod->is_loading = false;
    return err;
  }}
  
  char *js_code = content;
  ant_value_t ns = esm_make_namespace_object(js);
  mod->namespace_obj = ns;

  const char *prev_filename = js->filename;
  ant_module_t eval_ctx;
  
  ant_value_t prep_res = esm_prepare_eval_ctx(
    js, mod->resolved_path,
    ns, mod->format,
    false, prev_filename,
    &eval_ctx
  );
  
  if (is_err(prep_res)) {
    free(content);
    mod->is_loading = false;
    return prep_res;
  }

  js_set_filename(js, mod->resolved_path);
  js_module_eval_ctx_push(js, &eval_ctx);

  if (mod->format == MODULE_EVAL_FORMAT_UNKNOWN) {
    mod->format = esm_decide_module_format(mod->resolved_path);
    eval_ctx.format = mod->format;
  }

  ant_value_t result = esm_eval_module_with_format(
    js, mod->resolved_path, js_code, js_len, ns, mod->format
  );
  
  free(content);
  if (vtype(result) == T_PROMISE) {
  if (esm_dynamic_import_depth > 0) {
    mod->has_tla = true;
    mod->tla_promise = result;
  } else js_run_event_loop(js); }

  js_module_eval_ctx_pop(js, &eval_ctx);
  js_set_filename(js, prev_filename);

  if (is_err(result)) {
    mod->is_loading = false;
    return result;
  }
  return esm_complete_namespace_module(js, mod, ns);
}

static ant_value_t esm_get_or_load(
  ant_t *js,
  const char *specifier,
  const char *resolved_path,
  const char *module_key,
  ant_module_format_t format,
  const uint8_t *embedded_code,
  size_t embedded_code_len
) {
  esm_module_t *mod = esm_find_module(module_key);
  if (!mod) {
    mod = esm_create_module(
      specifier,
      resolved_path,
      module_key,
      format,
      embedded_code,
      embedded_code_len
    );
    if (!mod) return js_mkerr(js, "Cannot create module");
  }
  return esm_load_module(js, mod);
}

static const char *esm_default_base_path(ant_t *js) {
  const char *active = js_module_eval_active_filename(js);
  return (active && active[0]) ? active : ".";
}

ant_value_t js_esm_import_sync_cstr_from(
  ant_t *js,
  const char *specifier,
  size_t spec_len,
  const char *base_path
) {
  const ant_builtin_bundle_alias_t *bundle = NULL;
  const ant_builtin_bundle_module_t *module = NULL;
  
  char *spec_copy = strndup(specifier, spec_len);
  if (!spec_copy) return js_mkerr(js, "oom");

  char *file_url_path = esm_file_url_to_path(js, spec_copy);
  if (file_url_path) {
    free(spec_copy);
    spec_copy = file_url_path;
    spec_len = strlen(spec_copy);
  }

  bundle = esm_lookup_builtin_alias(spec_copy, spec_len);
  if (bundle) {
    module = esm_lookup_builtin_module(bundle->module_id);
    if (!module) {
      free(spec_copy);
      return js_mkerr(js, "Invalid builtin module id");
    }

    ant_value_t ns = esm_get_or_load(
      js, spec_copy,
      bundle->source_name,
      bundle->source_name,
      module->format,
      module->code,
      module->code_len
    );
    
    free(spec_copy);
    return ns;
  }

  bool loaded = false;
  ant_value_t lib = js_esm_load_registered_library(js, spec_copy, spec_len, &loaded);
  if (loaded) {
    free(spec_copy);
    return lib;
  }

  if (!base_path || !base_path[0]) base_path = esm_default_base_path(js);
  char *resolved_path = esm_resolve(spec_copy, base_path, esm_resolve_path);
  if (!resolved_path) {
    ant_value_t err = js_mkerr(js, "Cannot resolve module: %s", spec_copy);
    free(spec_copy);
    return err;
  }

  ant_value_t ns = esm_get_or_load(
    js,
    spec_copy,
    resolved_path,
    resolved_path,
    MODULE_EVAL_FORMAT_UNKNOWN,
    NULL,
    0
  );
  free(resolved_path);
  free(spec_copy);
  return ns;
}

ant_value_t js_esm_import_sync_cstr_from_require(
  ant_t *js,
  const char *specifier,
  size_t spec_len,
  const char *base_path
) {
  const ant_builtin_bundle_alias_t *bundle = NULL;
  const ant_builtin_bundle_module_t *module = NULL;

  char *spec_copy = strndup(specifier, spec_len);
  if (!spec_copy) return js_mkerr(js, "oom");

  char *file_url_path = esm_file_url_to_path(js, spec_copy);
  if (file_url_path) {
    free(spec_copy);
    spec_copy = file_url_path;
    spec_len = strlen(spec_copy);
  }

  bundle = esm_lookup_builtin_alias(spec_copy, spec_len);
  if (bundle) {
    module = esm_lookup_builtin_module(bundle->module_id);
    if (!module) {
      free(spec_copy);
      return js_mkerr(js, "Invalid builtin module id");
    }

    ant_value_t ns = esm_get_or_load(
      js, spec_copy,
      bundle->source_name,
      bundle->source_name,
      module->format,
      module->code,
      module->code_len
    );

    free(spec_copy);
    return ns;
  }

  bool loaded = false;
  ant_value_t lib = js_esm_load_registered_library(js, spec_copy, spec_len, &loaded);
  if (loaded) {
    free(spec_copy);
    return lib;
  }

  if (!base_path || !base_path[0]) base_path = esm_default_base_path(js);
  char *resolved_path = esm_resolve(spec_copy, base_path, esm_resolve_path_require);
  if (!resolved_path) {
    ant_value_t err = js_mkerr(js, "Cannot resolve module: %s", spec_copy);
    free(spec_copy);
    return err;
  }

  ant_value_t ns = esm_get_or_load(
    js, spec_copy,
    resolved_path,
    resolved_path,
    MODULE_EVAL_FORMAT_UNKNOWN,
    NULL, 0
  );
  
  free(resolved_path);
  free(spec_copy);
  
  return ns;
}

ant_value_t js_esm_import_sync_cstr(ant_t *js, const char *specifier, size_t spec_len) {
  return js_esm_import_sync_cstr_from(js, specifier, spec_len, NULL);
}

ant_value_t js_esm_import_sync_from(ant_t *js, ant_value_t specifier, const char *base_path) {
  if (vtype(specifier) != T_STR)
    return js_mkerr(js, "import() requires a string specifier");

  ant_offset_t spec_len = 0;
  ant_offset_t spec_off = vstr(js, specifier, &spec_len);
  const char *spec_str = (const char *)(uintptr_t)(spec_off);

  return js_esm_import_sync_cstr_from(js, spec_str, (size_t)spec_len, base_path);
}

ant_value_t js_esm_import_sync_from_require(ant_t *js, ant_value_t specifier, const char *base_path) {
  if (vtype(specifier) != T_STR)
    return js_mkerr(js, "require() expects a string specifier");
    
  ant_offset_t spec_len = 0;
  ant_offset_t spec_off = vstr(js, specifier, &spec_len);
  const char *spec_str = (const char *)(uintptr_t)(spec_off);

  return js_esm_import_sync_cstr_from_require(js, spec_str, (size_t)spec_len, base_path);
}

ant_value_t js_esm_import_sync(ant_t *js, ant_value_t specifier) {
  return js_esm_import_sync_from(js, specifier, NULL);
}

ant_value_t js_esm_import_dynamic(ant_t *js, ant_value_t specifier, const char *base_path, ant_value_t *out_tla_promise) {
  *out_tla_promise = js_mkundef();
  esm_dynamic_import_depth++;
  
  ant_value_t ns = js_esm_import_sync_from(js, specifier, base_path);
  esm_dynamic_import_depth--;
  if (is_err(ns)) return ns;

  esm_module_t *mod = NULL, *tmp = NULL;
  HASH_ITER(hh, global_module_cache.modules, mod, tmp) {
  if (mod->has_tla && mod->namespace_obj == ns) {
    *out_tla_promise = mod->tla_promise;
    mod->has_tla = false;
    mod->tla_promise = js_mkundef();
    break;
  }}
  
  return ns;
}

ant_value_t js_esm_make_file_url(ant_t *js, const char *path) {
  size_t path_len = strlen(path);
  size_t raw_len = 7 + path_len;
  
  char *raw = malloc(raw_len + 1);
  if (!raw) return js_mkerr(js, "oom");

  snprintf(raw, raw_len + 1, "file://%s", path);
  ant_value_t raw_val = js_mkstr(js, raw, raw_len);
  free(raw);

  return js_encodeURI(js, &raw_val, 1);
}

void gc_mark_esm(ant_t *js, gc_mark_fn mark) {
esm_module_t *mod = NULL, *tmp = NULL;
HASH_ITER(hh, global_module_cache.modules, mod, tmp) {
  mark(js, mod->namespace_obj);
  mark(js, mod->default_export);
  if (mod->has_tla) mark(js, mod->tla_promise);
}}

ant_value_t js_esm_resolve_specifier(ant_t *js, ant_value_t specifier, const char *base_path) {
  const ant_builtin_bundle_alias_t *bundle = NULL;
  if (vtype(specifier) != T_STR) {
    return js_mkerr(js, "import.meta.resolve() requires a string specifier");
  }

  ant_offset_t spec_len = 0;
  ant_offset_t spec_off = vstr(js, specifier, &spec_len);
  const char *spec_str = (const char *)(uintptr_t)(spec_off);
  char *spec_copy = strndup(spec_str, (size_t)spec_len);
  if (!spec_copy) return js_mkerr(js, "oom");

  bundle = esm_lookup_builtin_alias(spec_copy, (size_t)spec_len);
  if (bundle) {
    ant_value_t result = js_mkstr(js, bundle->source_name, strlen(bundle->source_name));
    free(spec_copy);
    return result;
  }

  if (!base_path || !base_path[0]) base_path = esm_default_base_path(js);
  char *resolved_path = esm_resolve(spec_copy, base_path, esm_resolve_path);
  free(spec_copy);

  if (!resolved_path) {
    return js_mkerr(js, "Cannot resolve module");
  }

  if (esm_is_url(resolved_path)) {
    ant_value_t result = js_mkstr(js, resolved_path, strlen(resolved_path));
    free(resolved_path);
    return result;
  }

  ant_value_t result = js_esm_make_file_url(js, resolved_path);
  free(resolved_path);
  return result;
}

ant_value_t js_esm_resolve_specifier_require(ant_t *js, ant_value_t specifier, const char *base_path) {
  const ant_builtin_bundle_alias_t *bundle = NULL;
  if (vtype(specifier) != T_STR) {
    return js_mkerr(js, "require.resolve() expects a string specifier");
  }

  ant_offset_t spec_len = 0;
  ant_offset_t spec_off = vstr(js, specifier, &spec_len);
  const char *spec_str = (const char *)(uintptr_t)(spec_off);
  char *spec_copy = strndup(spec_str, (size_t)spec_len);
  if (!spec_copy) return js_mkerr(js, "oom");

  bundle = esm_lookup_builtin_alias(spec_copy, (size_t)spec_len);
  if (bundle) {
    ant_value_t result = js_mkstr(js, bundle->source_name, strlen(bundle->source_name));
    free(spec_copy);
    return result;
  }

  if (!base_path || !base_path[0]) base_path = esm_default_base_path(js);
  char *resolved_path = esm_resolve(spec_copy, base_path, esm_resolve_path_require);
  free(spec_copy);

  if (!resolved_path) {
    return js_mkerr(js, "Cannot resolve module");
  }

  if (esm_is_url(resolved_path)) {
    ant_value_t result = js_mkstr(js, resolved_path, strlen(resolved_path));
    free(resolved_path);
    return result;
  }

  ant_value_t result = js_esm_make_file_url(js, resolved_path);
  free(resolved_path);
  
  return result;
}
