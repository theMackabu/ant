#include "utils.h"
#include "messages.h"

#include <crprintf.h>
#include <errno.h>
#include <skim.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define ANT_MKDIR(path) _mkdir(path)
#else
#include <limits.h>
#include <unistd.h>
#define ANT_MKDIR(path) mkdir(path, 0755)
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

const char *const module_resolve_extensions[] = {
  ".js", ".mjs", ".cjs",
  ".ts", ".mts", ".cts",
  ".json", ".node", NULL
};

static _Thread_local skim_context_t ts_strip_context;
static _Thread_local bool ts_strip_context_ready;

static int ensure_ts_strip_context(const char **error_detail) {
  if (ts_strip_context_ready) return 0;

  if (skim_context_init(&ts_strip_context) != 0) {
    if (error_detail) *error_detail = "out of memory while initializing TypeScript stripper";
    return SKIM_ERR_TRANSFORM_FAILED;
  }

  ts_strip_context_ready = true;
  return 0;
}

static const char *ant_home_dir(void) {
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
  if (home && home[0]) return home;
#else
  const char *home = getenv("HOME");
  if (home && home[0]) return home;
#endif
  return NULL;
}

static const char *ant_absolute_env(const char *name) {
#ifdef _WIN32
  return NULL;
#else
  const char *value = getenv(name);
  if (!value || value[0] != '/') return NULL;
  return value;
#endif
}

bool ant_env_bool(const char *value, bool default_value) {
  if (!value || !*value) return default_value;
  if (
    strcmp(value, "0") == 0     ||
    strcmp(value, "false") == 0 ||
    strcmp(value, "FALSE") == 0 ||
    strcmp(value, "off") == 0   ||
    strcmp(value, "OFF") == 0   ||
    strcmp(value, "no") == 0    ||
    strcmp(value, "NO") == 0
  ) return false;
  return true;
}

static int ant_join_app_path(
  char *out, size_t out_size,
  const char *base,
  const char *app,
  const char *suffix
) {
  if (!out || out_size == 0 || !base || !base[0] || !app || !app[0]) return -1;
  while (suffix && (*suffix == '/' || *suffix == '\\')) suffix++;

  int written = 0;
  if (suffix && suffix[0]) written = snprintf(out, out_size, "%s/%s/%s", base, app, suffix);
  else written = snprintf(out, out_size, "%s/%s", base, app);
  
  return (written < 0 || (size_t)written >= out_size) ? -1 : 0;
}

static int ant_legacy_home_path(char *out, size_t out_size, const char *suffix) {
  const char *home = ant_home_dir();
  if (!home) return -1;
  return ant_join_app_path(out, out_size, home, ".ant", suffix);
}

static bool ant_legacy_home_exists(void) {
  char path[4096];
  if (ant_legacy_home_path(path, sizeof(path), NULL) != 0) return false;
  struct stat st;
  return stat(path, &st) == 0;
}

static int ant_xdg_path(
  char *out, size_t out_size,
  const char *env_name,
  const char *home_suffix,
  const char *suffix
) {
#ifdef _WIN32
  const char *home = ant_home_dir();
  if (!home) return -1;
  return ant_join_app_path(out, out_size, home, ".ant", suffix);
#else
  if (ant_legacy_home_exists()) {
    return ant_legacy_home_path(out, out_size, suffix);
  }

  const char *base = ant_absolute_env(env_name);
  if (base) return ant_join_app_path(out, out_size, base, "ant", suffix);

  const char *home = ant_home_dir();
  if (!home) return -1;
  char fallback[4096];
  
  int written = snprintf(fallback, sizeof(fallback), "%s/%s", home, home_suffix);
  if (written < 0 || (size_t)written >= sizeof(fallback)) return -1;
  
  return ant_join_app_path(out, out_size, fallback, "ant", suffix);
#endif
}

int ant_mkdir_p(const char *path) {
  if (!path || !path[0]) return -1;

  char tmp[4096];
  size_t len = strlen(path);
  
  if (len >= sizeof(tmp)) return -1;
  memcpy(tmp, path, len + 1);

  while (
    len > 1 && (tmp[len - 1] == '/' || 
    tmp[len - 1] == '\\')
  ) tmp[--len] = '\0';

  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/' && *p != '\\') continue;
    char sep = *p; *p = '\0';
    if (ANT_MKDIR(tmp) != 0 && errno != EEXIST) return -1;
    *p = sep;
  }

  if (ANT_MKDIR(tmp) != 0 && errno != EEXIST) return -1;
  return 0;
}

int ant_xdg_cache_path(char *out, size_t out_size, const char *suffix) {
  return ant_xdg_path(out, out_size, "XDG_CACHE_HOME", ".cache", suffix);
}

int ant_xdg_data_path(char *out, size_t out_size, const char *suffix) {
  return ant_xdg_path(out, out_size, "XDG_DATA_HOME", ".local/share", suffix);
}

int ant_xdg_state_path(char *out, size_t out_size, const char *suffix) {
  return ant_xdg_path(out, out_size, "XDG_STATE_HOME", ".local/state", suffix);
}

int ant_user_bin_path(char *out, size_t out_size) {
  const char *home = ant_home_dir();
  if (!home || !out || out_size == 0) return -1;
#ifdef _WIN32
  int written = snprintf(out, out_size, "%s/.ant/bin", home);
#else
  if (ant_legacy_home_exists()) {
    return ant_legacy_home_path(out, out_size, "bin");
  }
  int written = snprintf(out, out_size, "%s/.local/bin", home);
#endif
  return (written < 0 || (size_t)written >= out_size) ? -1 : 0;
}

int ant_get_exe_path(char *out, size_t out_len, int argc, char **argv) {
  if (!out || out_len == 0) return -1;
  out[0] = '\0';

#ifdef _WIN32
  DWORD len = GetModuleFileNameA(NULL, out, (DWORD)out_len);
  if (len > 0 && len < out_len) return 0;
#else
#ifdef __APPLE__
  char tmp[PATH_MAX];
  uint32_t size = (uint32_t)sizeof(tmp);
  if (_NSGetExecutablePath(tmp, &size) == 0) {
    char resolved[PATH_MAX];
    const char *src = realpath(tmp, resolved) ? resolved : tmp;
    if ((size_t)snprintf(out, out_len, "%s", src) < out_len) return 0;
  }
#elif defined(__linux__)
  ssize_t len = readlink("/proc/self/exe", out, out_len);
  if (len > 0 && (size_t)len < out_len) {
    out[len] = '\0';
    return 0;
  }
#endif
#endif

  if (argc > 0 && argv && argv[0] && (size_t)snprintf(out, out_len, "%s", argv[0]) < out_len) return 0;
  return -1;
}

int is_typescript_file(const char *filename) {
  if (filename == NULL) return 0;
  size_t len = strlen(filename);
  if (len < 3) return 0;
  
  const char *ext = filename + len;
  while (ext > filename && *(ext - 1) != '.' && *(ext - 1) != '/') ext--;
  if (ext == filename || *(ext - 1) != '.') return 0;
  ext--;
  
  return (strcmp(ext, ".ts") == 0 || strcmp(ext, ".mts") == 0 || strcmp(ext, ".cts") == 0);
}

int transform_typescript(
  char **buffer, size_t len,
  const char *filename, ant_ts_source_mode_t source_mode,
  size_t *out_len, const char **error_detail
) {
  if (out_len) *out_len = len;
  if (error_detail) *error_detail = NULL;

  if (!buffer || !*buffer) {
    if (error_detail) *error_detail = "null input/output passed";
    return SKIM_ERR_NULL_INPUT;
  }
  
  char *input = *buffer;
  char error_buf[256] = {0};
  size_t stripped_len = 0;

  int init_result = ensure_ts_strip_context(error_detail);
  if (init_result < 0) return init_result;

  skim_context_reset(&ts_strip_context);
  skim_error_t strip_error = SKIM_ERR_TRANSFORM_FAILED;
  skim_source_mode_t skim_mode = SKIM_SOURCE_AUTO;
  
  switch (source_mode) {
    case ANT_TS_SOURCE_MODULE:   skim_mode = SKIM_SOURCE_MODULE; break;
    case ANT_TS_SOURCE_SCRIPT:   skim_mode = SKIM_SOURCE_SCRIPT; break;
    case ANT_TS_SOURCE_COMMONJS: skim_mode = SKIM_SOURCE_COMMONJS; break;
    case ANT_TS_SOURCE_AUTO:     break;
  }

  const char *stripped = skim_strip_typescript_borrowed(
    &ts_strip_context, input, len, filename, 
    skim_mode, NULL,
    &stripped_len, &strip_error, error_buf, sizeof(error_buf)
  );

  if (!stripped) {
    if (error_buf[0] != '\0') {
      size_t msg_len = strlen(error_buf);
      size_t copy_len = msg_len > len ? len : msg_len;
      memcpy(input, error_buf, copy_len);
      input[copy_len] = '\0';
    } else input[0] = '\0';
    
    if (error_detail) 
      *error_detail = input[0] != '\0' 
      ? input : "unknown strip error";
    
    return (int)strip_error;
  }

  char *next = realloc(input, stripped_len + 1);
  if (!next) {
    if (error_detail) *error_detail = "out of memory while resizing strip output buffer";
    return SKIM_ERR_OUTPUT_TOO_LARGE;
  }

  memcpy(next, stripped, stripped_len + 1);
  *buffer = next;
  if (out_len) *out_len = stripped_len;

  return 0;
}

int strip_typescript_inplace(
  char **buffer, size_t len,
  const char *filename, size_t *out_len,
  const char **error_detail
) {
  if (out_len) *out_len = len;
  if (error_detail) *error_detail = NULL;
  if (!is_typescript_file(filename)) return 0;
  return transform_typescript(
    buffer, len, filename, ANT_TS_SOURCE_AUTO,
    out_len, error_detail
  );
}

static bool is_entrypoint_script_extension(const char *ext) {
  return 
    ext && 
    strcmp(ext, ".json") != 0 &&
    strcmp(ext, ".node") != 0;
}

static bool has_js_extension(const char *filename) {
  const char *slash = strrchr(filename, '/');
  const char *base = slash ? slash + 1 : filename;
  const char *dot = strrchr(base, '.');
  if (!dot) return false;
  for (const char *const *ext = module_resolve_extensions; *ext; ext++) {
    if (!is_entrypoint_script_extension(*ext)) continue;
    if (strcmp(dot, *ext) == 0) return true;
  }
  return false;
}

char *resolve_js_file(const char *filename) {
  extern bool esm_is_url(const char *path);
  if (!filename) return NULL;
  if (esm_is_url(filename)) return strdup(filename);
  
  struct stat st;
  if (stat(filename, &st) == 0) {
    if (S_ISREG(st.st_mode)) {
      const char *slash = strrchr(filename, '/');
      const char *base = slash ? slash + 1 : filename;
      const char *dot = strrchr(base, '.');
      if (dot && !has_js_extension(filename)) return NULL;
      return strdup(filename);
    }
    if (!S_ISDIR(st.st_mode)) return NULL;

    size_t len = strlen(filename);
    int has_slash = (len > 0 && filename[len - 1] == '/');
    
    for (const char *const *ext = module_resolve_extensions; *ext; ext++) {
      if (!is_entrypoint_script_extension(*ext)) continue;
      size_t ext_len = strlen(*ext);
      char *index_path = try_oom(len + 7 + ext_len + 1);
      sprintf(index_path, "%s%sindex%s", filename, has_slash ? "" : "/", *ext);
      if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) return index_path;
      free(index_path);
    }
    
    return NULL;
  }
  
  if (has_js_extension(filename)) return NULL;
  size_t base_len = strlen(filename);
  
  for (const char *const *ext = module_resolve_extensions; *ext; ext++) {
    if (!is_entrypoint_script_extension(*ext)) continue;
    size_t ext_len = strlen(*ext);
    char *test_path = try_oom(base_len + ext_len + 1);
    
    memcpy(test_path, filename, base_len);
    memcpy(test_path + base_len, *ext, ext_len + 1);
    
    if (stat(test_path, &st) == 0 && S_ISREG(st.st_mode)) {
      return test_path;
    } free(test_path);
  }
  
  return NULL;
}

char *resolve_typescript_source_fallback(const char *filename) {
  if (!filename) return NULL;

  const char *mapped_ext = NULL;
  size_t trim_len = 0;

  size_t len = strlen(filename);
  if (len > 3 && strcmp(filename + len - 3, ".js") == 0) {
    mapped_ext = ".ts";
    trim_len = 3;
  } else if (len > 4 && strcmp(filename + len - 4, ".mjs") == 0) {
    mapped_ext = ".mts";
    trim_len = 4;
  } else if (len > 4 && strcmp(filename + len - 4, ".cjs") == 0) {
    mapped_ext = ".cts";
    trim_len = 4;
  } else return NULL;

  size_t mapped_len = strlen(mapped_ext);
  char *mapped = try_oom(len - trim_len + mapped_len + 1);
  memcpy(mapped, filename, len - trim_len);
  memcpy(mapped + len - trim_len, mapped_ext, mapped_len + 1);
  
  return mapped;
}

void *try_oom(size_t size) {
  void *p = malloc(size);
  if (!p) {
    crfprintf(stderr, msg.oom_fatal);
    exit(EXIT_FAILURE);
  } return p;
}
