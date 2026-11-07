#include <compat.h> // IWYU pragma: keep
#include "tsconfig.h"

#include "esm/loader.h"
#include "loader_cache.h"
#include "loader_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uthash.h>
#include <yyjson.h>

#define TSCONFIG_MAX_EXTENDS_DEPTH 32

typedef struct esm_tsconfig_entry {
  char *path;
  yyjson_doc *doc;
  yyjson_val *paths;
  char *paths_dir;
  char *base_url;
  bool found;
  bool loading;
  UT_hash_handle hh;
} esm_tsconfig_entry_t;

typedef struct esm_tsconfig_dir_entry {
  char *dir;
  esm_tsconfig_entry_t *config;
  UT_hash_handle hh;
} esm_tsconfig_dir_entry_t;

static bool TSConfig_IsAbsolute(const char *path) {
  if (!path || !path[0]) return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  return isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static bool TSConfig_IsRelative(const char *path) {
  return
    strcmp(path, ".") == 0 || strcmp(path, "..") == 0 ||
    strncmp(path, "./", 2) == 0 || strncmp(path, "../", 3) == 0 ||
    strncmp(path, ".\\", 2) == 0 || strncmp(path, "..\\", 3) == 0;
}

static bool TSConfig_IsFile(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool TSConfig_InNodeModules(const char *dir) {
  for (const char *p = dir; (p = strstr(p, "node_modules")); p += 12) {
    bool starts = p == dir || p[-1] == '/' || p[-1] == '\\';
    bool ends = p[12] == '\0' || p[12] == '/' || p[12] == '\\';
    if (starts && ends) return true;
  }
  return false;
}

static char *TSConfig_Join(const char *dir, const char *path, const char *suffix) {
  if (!suffix) suffix = "";
  size_t len = strlen(dir) + strlen(path) + strlen(suffix) + 2;
  char *joined = malloc(len);
  if (!joined) return NULL;

  if (TSConfig_IsAbsolute(path)) snprintf(joined, len, "%s%s", path, suffix);
  else snprintf(joined, len, "%s/%s%s", dir, path, suffix);

  char *canonical = esm_make_absolute_path(joined);
  free(joined);
  return canonical;
}

static char *TSConfig_ParentDir(const char *dir) {
  char *parent = esm_path_dirname(dir);
  if (!parent) return NULL;
  if (strcmp(parent, dir) == 0 || strcmp(parent, ".") == 0) {
    free(parent);
    return NULL;
  }
  return parent;
}

static size_t TSConfig_SkipString(const char *s, size_t i, size_t n) {
  for (i++; i < n && s[i] != '"'; i++) {
    if (s[i] == '\\' && i + 1 < n) i++;
  }
  return i < n ? i + 1 : n;
}

static void TSConfig_StripComments(char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (s[i] == '"') {
      i = TSConfig_SkipString(s, i, n);
      continue;
    }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n') s[i++] = ' ';
      continue;
    }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
      s[i++] = ' ';
      s[i++] = ' ';
      while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) {
        if (s[i] != '\n') s[i] = ' ';
        i++;
      }
      if (i < n) s[i++] = ' ';
      if (i < n) s[i++] = ' ';
      continue;
    }
    i++;
  }
}

static void TSConfig_StripTrailingCommas(char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (s[i] == '"') {
      i = TSConfig_SkipString(s, i, n);
      continue;
    }
    if (s[i] == ',') {
      size_t j = i + 1;
      while (j < n && isspace((unsigned char)s[j])) j++;
      if (j < n && (s[j] == '}' || s[j] == ']')) s[i] = ' ';
    }
    i++;
  }
}

static yyjson_doc *TSConfig_Parse(const char *path, bool *found) {
  *found = false;
  if (!TSConfig_IsFile(path)) return NULL;

  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  *found = true;

  char *buf = NULL;
  size_t len = 0, cap = 0;
  for (;;) {
    if (len == cap) {
      size_t next = cap ? cap * 2 : 4096;
      char *grown = realloc(buf, next);
      if (!grown) {
        free(buf);
        fclose(f);
        return NULL;
      }
      buf = grown;
      cap = next;
    }
    size_t got = fread(buf + len, 1, cap - len, f);
    if (got == 0) break;
    len += got;
  }
  fclose(f);
  if (!buf) return NULL;

  if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
    buf[0] = buf[1] = buf[2] = ' ';
  }

  TSConfig_StripComments(buf, len);
  TSConfig_StripTrailingCommas(buf, len);

  yyjson_doc *doc = yyjson_read(buf, len, 0);
  free(buf);
  return doc;
}

static char *TSConfig_ResolveExtends(const char *dir, const char *ext) {
  if (!ext[0]) return NULL;

  if (TSConfig_IsRelative(ext) || TSConfig_IsAbsolute(ext)) {
    char *path = TSConfig_Join(dir, ext, NULL);
    if (!path) return NULL;
    if (TSConfig_IsFile(path)) return path;

    size_t len = strlen(path);
    bool has_json = len >= 5 && strcmp(path + len - 5, ".json") == 0;
    free(path);
    if (has_json) return NULL;

    path = TSConfig_Join(dir, ext, ".json");
    if (path && TSConfig_IsFile(path)) return path;
    free(path);
    return NULL;
  }

  static const char *const suffixes[] = { "", ".json", "/tsconfig.json" };
  char *current = strdup(dir);

  while (current) {
    char *base = TSConfig_Join(current, "node_modules", NULL);
    for (size_t i = 0; base && i < sizeof(suffixes) / sizeof(*suffixes); i++) {
      char *candidate = TSConfig_Join(base, ext, suffixes[i]);
      if (candidate && TSConfig_IsFile(candidate)) {
        free(base);
        free(current);
        return candidate;
      }
      free(candidate);
    }
    free(base);

    char *parent = TSConfig_ParentDir(current);
    free(current);
    current = parent;
  }

  return NULL;
}

static esm_tsconfig_entry_t *TSConfig_Load(ant_esm_state_t *st, const char *path, int depth);

static void TSConfig_Inherit(ant_esm_state_t *st, esm_tsconfig_entry_t *entry, const char *dir, yyjson_val *ext, int depth) {
  if (!yyjson_is_str(ext)) return;

  char *parent_path = TSConfig_ResolveExtends(dir, yyjson_get_str(ext));
  if (!parent_path) return;

  esm_tsconfig_entry_t *parent = TSConfig_Load(st, parent_path, depth + 1);
  free(parent_path);
  if (!parent || !parent->found || parent->loading) return;

  if (parent->paths && parent->paths_dir) {
    char *paths_dir = strdup(parent->paths_dir);
    if (paths_dir) {
      free(entry->paths_dir);
      entry->paths = parent->paths;
      entry->paths_dir = paths_dir;
    }
  }

  if (parent->base_url) {
    char *base_url = strdup(parent->base_url);
    if (base_url) {
      free(entry->base_url);
      entry->base_url = base_url;
    }
  }
}

static void TSConfig_Apply(ant_esm_state_t *st, esm_tsconfig_entry_t *entry, int depth) {
  yyjson_val *root = yyjson_doc_get_root(entry->doc);
  if (!yyjson_is_obj(root)) return;

  char *dir = esm_path_dirname(entry->path);
  if (!dir) return;

  if (depth < TSCONFIG_MAX_EXTENDS_DEPTH) {
    yyjson_val *ext = yyjson_obj_get(root, "extends");
    if (yyjson_is_arr(ext)) {
      size_t idx, max;
      yyjson_val *item;
      yyjson_arr_foreach(ext, idx, max, item) TSConfig_Inherit(st, entry, dir, item, depth);
    } else TSConfig_Inherit(st, entry, dir, ext, depth);
  }

  yyjson_val *opts = yyjson_obj_get(root, "compilerOptions");
  if (yyjson_is_obj(opts)) {
    yyjson_val *base_url = yyjson_obj_get(opts, "baseUrl");
    if (yyjson_is_str(base_url)) {
      char *joined = TSConfig_Join(dir, yyjson_get_str(base_url), NULL);
      if (joined) {
        free(entry->base_url);
        entry->base_url = joined;
      }
    }

    yyjson_val *paths = yyjson_obj_get(opts, "paths");
    if (yyjson_is_obj(paths)) {
      char *paths_dir = strdup(dir);
      if (paths_dir) {
        free(entry->paths_dir);
        entry->paths = paths;
        entry->paths_dir = paths_dir;
      }
    }
  }

  free(dir);
}

static esm_tsconfig_entry_t *TSConfig_Load(ant_esm_state_t *st, const char *path, int depth) {
  esm_tsconfig_entry_t *entry = NULL;
  HASH_FIND_STR(st->tsconfig_cache, path, entry);
  if (entry) return entry;

  entry = calloc(1, sizeof(*entry));
  if (!entry) return NULL;

  entry->path = strdup(path);
  if (!entry->path) {
    free(entry);
    return NULL;
  }

  HASH_ADD_STR(st->tsconfig_cache, path, entry);

  entry->loading = true;
  entry->doc = TSConfig_Parse(path, &entry->found);
  if (entry->doc) TSConfig_Apply(st, entry, depth);
  entry->loading = false;

  return entry;
}

static esm_tsconfig_entry_t *TSConfig_FindNearest(ant_esm_state_t *st, const char *dir) {
  esm_tsconfig_dir_entry_t *cached = NULL;
  HASH_FIND_STR(st->tsconfig_dir_cache, dir, cached);
  if (cached) return cached->config;

  static const char *const names[] = { "tsconfig.json", "jsconfig.json" };
  esm_tsconfig_entry_t *config = NULL;

  for (size_t i = 0; !config && i < sizeof(names) / sizeof(*names); i++) {
    char *path = TSConfig_Join(dir, names[i], NULL);
    if (!path) continue;
    esm_tsconfig_entry_t *entry = TSConfig_Load(st, path, 0);
    free(path);
    if (entry && entry->found) config = entry;
  }

  if (!config) {
    char *parent = TSConfig_ParentDir(dir);
    if (parent) {
      config = TSConfig_FindNearest(st, parent);
      free(parent);
    }
  }

  esm_tsconfig_dir_entry_t *dir_entry = calloc(1, sizeof(*dir_entry));
  if (!dir_entry) return config;

  dir_entry->dir = strdup(dir);
  if (!dir_entry->dir) {
    free(dir_entry);
    return config;
  }

  dir_entry->config = config;
  HASH_ADD_STR(st->tsconfig_dir_cache, dir, dir_entry);
  return config;
}

static char *TSConfig_Substitute(const char *target, const char *capture, size_t capture_len) {
  const char *star = strchr(target, '*');
  if (!star) return strdup(target);

  size_t prefix_len = (size_t)(star - target);
  size_t suffix_len = strlen(star + 1);
  char *out = malloc(prefix_len + capture_len + suffix_len + 1);
  if (!out) return NULL;

  memcpy(out, target, prefix_len);
  memcpy(out + prefix_len, capture, capture_len);
  memcpy(out + prefix_len + capture_len, star + 1, suffix_len + 1);
  return out;
}

char *esm_tsconfig_resolve(
  ant_t *js,
  const char *specifier,
  const char *start_dir,
  esm_tsconfig_probe_fn probe,
  void *ctx
) {
  if (!specifier || !specifier[0] || !start_dir || !start_dir[0]) return NULL;
  if (TSConfig_InNodeModules(start_dir)) return NULL;

  ant_esm_state_t *st = esm_state(js);
  if (!st) return NULL;

  esm_tsconfig_entry_t *config = TSConfig_FindNearest(st, start_dir);
  if (!config || !config->paths) return NULL;

  size_t spec_len = strlen(specifier);
  yyjson_val *targets = NULL;
  const char *capture = "";
  size_t capture_len = 0;
  size_t best_prefix = 0;

  size_t idx, max;
  yyjson_val *key, *val;
  yyjson_obj_foreach(config->paths, idx, max, key, val) {
    const char *pattern = yyjson_get_str(key);
    if (!pattern) continue;

    const char *star = strchr(pattern, '*');
    if (!star) {
      if (strcmp(pattern, specifier) != 0) continue;
      targets = val;
      capture = "";
      capture_len = 0;
      break;
    }
    if (strchr(star + 1, '*')) continue;

    size_t prefix_len = (size_t)(star - pattern);
    size_t suffix_len = strlen(star + 1);
    if (spec_len < prefix_len + suffix_len) continue;
    if (strncmp(specifier, pattern, prefix_len) != 0) continue;
    if (strcmp(specifier + spec_len - suffix_len, star + 1) != 0) continue;
    if (targets && prefix_len <= best_prefix) continue;

    targets = val;
    best_prefix = prefix_len;
    capture = specifier + prefix_len;
    capture_len = spec_len - prefix_len - suffix_len;
  }

  if (!yyjson_is_arr(targets)) return NULL;

  const char *base = config->base_url ? config->base_url : config->paths_dir;
  if (!base) return NULL;

  yyjson_val *target;
  yyjson_arr_foreach(targets, idx, max, target) {
    if (!yyjson_is_str(target)) continue;

    char *substituted = TSConfig_Substitute(yyjson_get_str(target), capture, capture_len);
    if (!substituted) continue;

    char *candidate = TSConfig_Join(base, substituted, NULL);
    free(substituted);
    if (!candidate) continue;

    char *resolved = probe(js, candidate, ctx);
    free(candidate);
    if (resolved) return resolved;
  }

  return NULL;
}

void esm_tsconfig_cache_cleanup(ant_esm_state_t *st) {
  if (!st) return;

  esm_tsconfig_dir_entry_t *dir_current, *dir_tmp;
  HASH_ITER(hh, st->tsconfig_dir_cache, dir_current, dir_tmp) {
    HASH_DEL(st->tsconfig_dir_cache, dir_current);
    free(dir_current->dir);
    free(dir_current);
  }

  esm_tsconfig_entry_t *current, *tmp;
  HASH_ITER(hh, st->tsconfig_cache, current, tmp) {
    HASH_DEL(st->tsconfig_cache, current);
    free(current->path);
    free(current->paths_dir);
    free(current->base_url);
    if (current->doc) yyjson_doc_free(current->doc);
    free(current);
  }
}
