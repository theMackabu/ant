#include "utils.h"

#include <stdio.h>

const char *const module_resolve_extensions[] = {
  ".js", ".mjs", ".cjs", ".ts", ".mts", ".cts", ".json", NULL
};

int is_typescript_file(const char *filename) {
  (void)filename;
  return 0;
}

char *resolve_js_file(const char *filename) {
  (void)filename;
  return NULL;
}

char *resolve_typescript_source_fallback(const char *filename) {
  (void)filename;
  return NULL;
}

int ant_mkdir_p(const char *path) { (void)path; return -1; }
int ant_user_bin_path(char *out, size_t size) { (void)out; (void)size; return -1; }
int ant_get_exe_path(char *out, size_t size, int argc, char **argv) {
  (void)out; (void)size; (void)argc; (void)argv; return -1;
}
int ant_xdg_cache_path(char *out, size_t size, const char *suffix) {
  (void)out; (void)size; (void)suffix; return -1;
}
int ant_xdg_data_path(char *out, size_t size, const char *suffix) {
  (void)out; (void)size; (void)suffix; return -1;
}
int ant_xdg_state_path(char *out, size_t size, const char *suffix) {
  (void)out; (void)size; (void)suffix; return -1;
}

bool ant_env_bool(const char *value, bool default_value) {
  if (!value || !*value) return default_value;
  return strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

int strip_typescript_inplace(
  char **buffer, size_t len, const char *filename,
  size_t *out_len, const char **error_detail
) {
  (void)buffer; (void)filename;
  if (out_len) *out_len = len;
  if (error_detail) *error_detail = "TypeScript is not available in @antjs.org/wasm";
  return -1;
}

int transform_typescript(
  char **buffer, size_t len, const char *filename,
  ant_ts_source_mode_t source_mode, size_t *out_len,
  const char **error_detail
) {
  (void)source_mode;
  return strip_typescript_inplace(buffer, len, filename, out_len, error_detail);
}

void *try_oom(size_t size) {
  void *value = malloc(size);
  if (!value) abort();
  return value;
}
