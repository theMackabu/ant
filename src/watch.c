#include "watch.h"
#include "messages.h"
#include "utils.h"
#include "esm/loader.h"
#include "esm/remote.h"

#include <uv.h>
#include <ctype.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <crprintf.h>

#ifdef _WIN32
#include <direct.h>
#endif

#ifndef SIGKILL
#define SIGKILL SIGTERM
#endif

#define WATCH_RESTART_DEBOUNCE_MS 75
#define WATCH_CHILD_KILL_GRACE_MS 1000

typedef struct watch_state watch_state_t;

typedef struct watch_item {
  uv_fs_event_t event;
  watch_state_t *state;
  char *path;
  uint64_t size;
  time_t mtime_sec;
  long mtime_nsec;
  bool stat_valid;
  bool closing;
  struct watch_item *next;
} watch_item_t;

typedef struct watch_path_list {
  char **items;
  size_t len;
  size_t cap;
} watch_path_list_t;

struct watch_state {
  uv_loop_t loop;
  watch_item_t *watchers;
  uv_signal_t sigint;
  uv_signal_t sigterm;
  uv_timer_t debounce_timer;
  uv_timer_t kill_timer;
  uv_process_t *child;
  char **child_argv;
  char *watch_path;
  bool no_clear_screen;
  bool stop_requested;
  bool restart_pending;
  bool child_running;
  bool sigint_inited;
  bool sigterm_inited;
  bool debounce_timer_inited;
  bool debounce_timer_running;
  bool kill_timer_inited;
  bool kill_timer_running;
  int final_exit_code;
};

static void watch_clear_screen(void) {
  fputs("\033[3J\033[2J\033[H", stdout);
  fflush(stdout);
}

char *ant_watch_resolve_path(const char *path) {
#ifdef _WIN32
  char *resolved = _fullpath(NULL, path, 0);
  if (resolved) return resolved;
  return _strdup(path);
#else
  char abs_path[PATH_MAX];
  const char *resolved = realpath(path, abs_path);
  return strdup(resolved ? resolved : path);
#endif
}

int ant_watch_start(
  uv_loop_t *loop,
  uv_fs_event_t *event,
  const char *path,
  uv_fs_event_cb callback,
  void *data,
  unsigned int flags,
  char **resolved_path_out
) {
  char *watch_path = NULL;
  int rc = 0;

  if (!loop || !event || !path || !callback) return UV_EINVAL;

  watch_path = ant_watch_resolve_path(path);
  if (!watch_path) return UV_ENOMEM;

  rc = uv_fs_event_init(loop, event);
  if (rc != 0) goto cleanup;

  event->data = data;
  rc = uv_fs_event_start(event, callback, watch_path, flags);
  if (rc != 0) goto cleanup;

  if (resolved_path_out) *resolved_path_out = watch_path;
  else free(watch_path);
  return 0;

cleanup:
  free(watch_path);
  return rc;
}

void ant_watch_stop(uv_fs_event_t *event) {
  if (!event) return;
  uv_fs_event_stop(event);
}

static bool watch_arg_takes_value(const char *arg) {
  return
    strcmp(arg, "-e") == 0 ||
    strcmp(arg, "--eval") == 0 ||
    strcmp(arg, "--localstorage-file") == 0;
}

static bool watch_is_control_arg(const char *arg) {
  return
    strcmp(arg, "-w") == 0 ||
    strcmp(arg, "--watch") == 0 ||
    strcmp(arg, "--no-clear-screen") == 0;
}

static char **watch_build_child_argv(int argc, char **argv) {
  char **child_argv = calloc((size_t)argc + 1, sizeof(char *));
  if (!child_argv) return NULL;

  int out = 0;
  bool after_script_start = false;

  for (int i = 0; i < argc; i++) {
    const char *arg = argv[i];

    if (i == 0) {
      child_argv[out++] = argv[i];
      continue;
    }

    if (!after_script_start && strcmp(arg, "--") == 0) {
      after_script_start = true;
      child_argv[out++] = argv[i];
      continue;
    }

    if (!after_script_start && watch_is_control_arg(arg)) continue;

    child_argv[out++] = argv[i];

    if (after_script_start) continue;

    if (arg[0] != '-') {
      after_script_start = true;
      continue;
    }

    if (watch_arg_takes_value(arg) && i + 1 < argc) {
      child_argv[out++] = argv[++i];
    }
  }

  child_argv[out] = NULL;
  if (out == 0) {
    free(child_argv);
    return NULL;
  }

  return child_argv;
}

static void watch_path_list_free(watch_path_list_t *list) {
  if (!list) return;
  for (size_t i = 0; i < list->len; i++) free(list->items[i]);
  free(list->items);
  *list = (watch_path_list_t){0};
}

static bool watch_path_list_contains(watch_path_list_t *list, const char *path) {
  if (!list || !path) return false;
  for (size_t i = 0; i < list->len; i++) {
    if (strcmp(list->items[i], path) == 0) return true;
  }
  return false;
}

static int watch_path_list_push_owned(watch_path_list_t *list, char *path) {
  if (!list || !path) return UV_EINVAL;
  if (watch_path_list_contains(list, path)) {
    free(path);
    return 0;
  }

  if (list->len == list->cap) {
    size_t next_cap = list->cap ? list->cap * 2 : 8;
    char **next = realloc(list->items, next_cap * sizeof(*next));
    if (!next) {
      free(path);
      return UV_ENOMEM;
    }
    list->items = next;
    list->cap = next_cap;
  }

  list->items[list->len++] = path;
  return 0;
}

static bool watch_has_suffix(const char *path, const char *suffix) {
  size_t path_len = strlen(path);
  size_t suffix_len = strlen(suffix);
  return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool watch_is_graph_source_file(const char *path) {
  if (!path) return false;
  if (watch_has_suffix(path, ".json") || watch_has_suffix(path, ".node")) return false;

  for (const char *const *ext = module_resolve_extensions; *ext; ext++) {
    if (strcmp(*ext, ".json") == 0 || strcmp(*ext, ".node") == 0) continue;
    if (watch_has_suffix(path, *ext)) return true;
  }
  return false;
}

static bool watch_is_local_specifier(const char *specifier) {
  if (!specifier || !specifier[0]) return false;
  return
    specifier[0] == '/' ||
    strcmp(specifier, ".") == 0 ||
    strcmp(specifier, "..") == 0 ||
    strncmp(specifier, "./", 2) == 0 ||
    strncmp(specifier, "../", 3) == 0 ||
    strncmp(specifier, ".\\", 2) == 0 ||
    strncmp(specifier, "..\\", 3) == 0;
}

static char *watch_read_file(const char *path, size_t *out_len) {
  if (out_len) *out_len = 0;
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }

  long size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return NULL;
  }

  rewind(fp);
  char *buffer = malloc((size_t)size + 1);
  if (!buffer) {
    fclose(fp);
    return NULL;
  }

  size_t read_len = fread(buffer, 1, (size_t)size, fp);
  fclose(fp);
  buffer[read_len] = '\0';
  if (out_len) *out_len = read_len;
  return buffer;
}

static bool watch_is_ident_char(unsigned char ch) {
  return isalnum(ch) || ch == '_' || ch == '$';
}

static bool watch_word_equals(const char *src, size_t start, size_t end, const char *word) {
  size_t len = strlen(word);
  return end - start == len && memcmp(src + start, word, len) == 0;
}

static void watch_skip_line_comment(const char *src, size_t len, size_t *i) {
  *i += 2;
  while (*i < len && src[*i] != '\n' && src[*i] != '\r') (*i)++;
}

static void watch_skip_block_comment(const char *src, size_t len, size_t *i) {
  *i += 2;
  while (*i + 1 < len) {
    if (src[*i] == '*' && src[*i + 1] == '/') {
      *i += 2;
      return;
    }
    (*i)++;
  }
  *i = len;
}

static void watch_skip_ws_comments(const char *src, size_t len, size_t *i) {
  while (*i < len) {
    unsigned char ch = (unsigned char)src[*i];
    if (isspace(ch)) {
      (*i)++;
      continue;
    }
    if (*i + 1 < len && src[*i] == '/' && src[*i + 1] == '/') {
      watch_skip_line_comment(src, len, i);
      continue;
    }
    if (*i + 1 < len && src[*i] == '/' && src[*i + 1] == '*') {
      watch_skip_block_comment(src, len, i);
      continue;
    }
    break;
  }
}

static char *watch_parse_string_literal(const char *src, size_t len, size_t *i) {
  if (*i >= len) return NULL;

  char quote = src[*i];
  if (quote != '\'' && quote != '"' && quote != '`') return NULL;
  (*i)++;

  char *out = malloc(len - *i + 1);
  if (!out) return NULL;

  size_t out_len = 0;
  while (*i < len) {
    char ch = src[*i];
    if (ch == quote) {
      (*i)++;
      out[out_len] = '\0';
      return out;
    }

    if (ch == '\\' && *i + 1 < len) {
      (*i)++;
      out[out_len++] = src[*i];
      (*i)++;
      continue;
    }

    out[out_len++] = ch;
    (*i)++;
  }

  free(out);
  return NULL;
}

static void watch_skip_string_literal(const char *src, size_t len, size_t *i) {
  char *ignored = watch_parse_string_literal(src, len, i);
  free(ignored);
}

typedef void (*watch_specifier_cb)(const char *specifier, bool prefer_require, void *data);

static void watch_emit_string_specifier(
  const char *src,
  size_t len,
  size_t *i,
  bool prefer_require,
  watch_specifier_cb cb,
  void *data
) {
  char *specifier = watch_parse_string_literal(src, len, i);
  if (!specifier) return;
  cb(specifier, prefer_require, data);
  free(specifier);
}

static void watch_scan_import_clause(
  const char *src,
  size_t len,
  size_t *i,
  watch_specifier_cb cb,
  void *data
) {
  watch_skip_ws_comments(src, len, i);
  if (*i >= len) return;

  if (src[*i] == '(') {
    (*i)++;
    watch_skip_ws_comments(src, len, i);
    watch_emit_string_specifier(src, len, i, false, cb, data);
    return;
  }

  if (src[*i] == '\'' || src[*i] == '"' || src[*i] == '`') {
    watch_emit_string_specifier(src, len, i, false, cb, data);
    return;
  }

  while (*i < len) {
    watch_skip_ws_comments(src, len, i);
    if (*i >= len || src[*i] == ';') return;

    if (src[*i] == '\'' || src[*i] == '"' || src[*i] == '`') {
      watch_skip_string_literal(src, len, i);
      continue;
    }

    if (watch_is_ident_char((unsigned char)src[*i])) {
      size_t start = *i;
      while (*i < len && watch_is_ident_char((unsigned char)src[*i])) (*i)++;
      if (watch_word_equals(src, start, *i, "from")) {
        size_t after_from = *i;
        watch_skip_ws_comments(src, len, i);
        if (*i < len && (src[*i] == '\'' || src[*i] == '"' || src[*i] == '`')) {
          watch_emit_string_specifier(src, len, i, false, cb, data);
          return;
        }
        *i = after_from;
      }
      continue;
    }

    (*i)++;
  }
}

static void watch_scan_export_clause(
  const char *src,
  size_t len,
  size_t *i,
  watch_specifier_cb cb,
  void *data
) {
  while (*i < len) {
    watch_skip_ws_comments(src, len, i);
    if (*i >= len || src[*i] == ';') return;

    if (src[*i] == '\'' || src[*i] == '"' || src[*i] == '`') {
      watch_skip_string_literal(src, len, i);
      continue;
    }

    if (watch_is_ident_char((unsigned char)src[*i])) {
      size_t start = *i;
      while (*i < len && watch_is_ident_char((unsigned char)src[*i])) (*i)++;
      if (watch_word_equals(src, start, *i, "from")) {
        size_t after_from = *i;
        watch_skip_ws_comments(src, len, i);
        if (*i < len && (src[*i] == '\'' || src[*i] == '"' || src[*i] == '`')) {
          watch_emit_string_specifier(src, len, i, false, cb, data);
          return;
        }
        *i = after_from;
      }
      continue;
    }

    (*i)++;
  }
}

static void watch_scan_require_call(
  const char *src,
  size_t len,
  size_t *i,
  watch_specifier_cb cb,
  void *data
) {
  watch_skip_ws_comments(src, len, i);
  if (*i >= len || src[*i] != '(') return;

  (*i)++;
  watch_skip_ws_comments(src, len, i);
  watch_emit_string_specifier(src, len, i, true, cb, data);
}

typedef struct watch_graph_context {
  watch_path_list_t *paths;
  const char *base_path;
} watch_graph_context_t;

static void watch_graph_add_specifier(const char *specifier, bool prefer_require, void *data) {
  watch_graph_context_t *ctx = (watch_graph_context_t *)data;
  if (!watch_is_local_specifier(specifier)) return;

  char *resolved = js_esm_resolve_path_for_watch(specifier, ctx->base_path, prefer_require);
  if (!resolved) return;

  if (esm_is_url(resolved)) {
    free(resolved);
    return;
  }

  struct stat st;
  if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
    free(resolved);
    return;
  }

  watch_path_list_push_owned(ctx->paths, resolved);
}

static void watch_scan_module_file(
  const char *path,
  watch_path_list_t *paths
) {
  if (!watch_is_graph_source_file(path)) return;

  size_t len = 0;
  char *src = watch_read_file(path, &len);
  if (!src) return;

  watch_graph_context_t ctx = { paths, path };

  for (size_t i = 0; i < len;) {
    if (src[i] == '\'' || src[i] == '"' || src[i] == '`') {
      watch_skip_string_literal(src, len, &i);
      continue;
    }

    if (i + 1 < len && src[i] == '/' && src[i + 1] == '/') {
      watch_skip_line_comment(src, len, &i);
      continue;
    }

    if (i + 1 < len && src[i] == '/' && src[i + 1] == '*') {
      watch_skip_block_comment(src, len, &i);
      continue;
    }

    if (!watch_is_ident_char((unsigned char)src[i])) {
      i++;
      continue;
    }

    size_t start = i;
    while (i < len && watch_is_ident_char((unsigned char)src[i])) i++;

    if (watch_word_equals(src, start, i, "import")) {
      watch_scan_import_clause(src, len, &i, watch_graph_add_specifier, &ctx);
    } else if (watch_word_equals(src, start, i, "export")) {
      watch_scan_export_clause(src, len, &i, watch_graph_add_specifier, &ctx);
    } else if (watch_word_equals(src, start, i, "require")) {
      watch_scan_require_call(src, len, &i, watch_graph_add_specifier, &ctx);
    }
  }

  free(src);
}

static int watch_build_graph(const char *entry_file, watch_path_list_t *paths) {
  char *entry_copy = strdup(entry_file);
  if (!entry_copy) return UV_ENOMEM;

  int rc = watch_path_list_push_owned(paths, entry_copy);
  if (rc != 0) return rc;

  for (size_t i = 0; i < paths->len; i++) {
    watch_scan_module_file(paths->items[i], paths);
  }

  return 0;
}

static void watch_on_watcher_closed(uv_handle_t *handle) {
  watch_item_t *item = (watch_item_t *)handle->data;
  if (!item) return;
  free(item->path);
  free(item);
}

static long watch_stat_mtime_nsec(const struct stat *st) {
#ifdef _WIN32
  (void)st;
  return 0;
#elif defined(__APPLE__)
  return st->st_mtimespec.tv_nsec;
#else
  return st->st_mtim.tv_nsec;
#endif
}

static bool watch_item_refresh_stat(watch_item_t *item) {
  if (!item || !item->path) return true;

  struct stat st;
  if (stat(item->path, &st) != 0) {
    bool changed = item->stat_valid;
    item->stat_valid = false;
    item->size = 0;
    item->mtime_sec = 0;
    item->mtime_nsec = 0;
    return changed;
  }

  uint64_t size = (uint64_t)st.st_size;
  time_t mtime_sec = st.st_mtime;
  long mtime_nsec = watch_stat_mtime_nsec(&st);
  bool changed =
    !item->stat_valid ||
    item->size != size ||
    item->mtime_sec != mtime_sec ||
    item->mtime_nsec != mtime_nsec;

  item->stat_valid = true;
  item->size = size;
  item->mtime_sec = mtime_sec;
  item->mtime_nsec = mtime_nsec;
  return changed;
}

static void watch_request_shutdown(watch_state_t *state, int exit_code);
static void watch_try_spawn_child(watch_state_t *state);
static void watch_request_restart_now(watch_state_t *state);
static void watch_on_debounce_timer(uv_timer_t *timer);

static void watch_on_fs_event(uv_fs_event_t *handle, const char *filename, int events, int status) {
  (void)filename;
  watch_item_t *item = (watch_item_t *)handle->data;
  watch_state_t *state = item ? item->state : NULL;
  if (!state) return;
  if (!item || item->closing) return;

  if (status < 0) {
    crfprintf(stderr, msg.watch_warn_normal, item && item->path ? item->path : state->watch_path, uv_strerror(status));
    return;
  }

  if ((events & (UV_CHANGE | UV_RENAME)) == 0) return;
  if (!watch_item_refresh_stat(item)) return;
  if (state->restart_pending) return;

  if (!state->debounce_timer_inited) {
    watch_request_restart_now(state);
    return;
  }

  if (state->debounce_timer_running) uv_timer_stop(&state->debounce_timer);

  int rc = uv_timer_start(
    &state->debounce_timer,
    watch_on_debounce_timer,
    WATCH_RESTART_DEBOUNCE_MS,
    0
  );

  if (rc == 0) {
    state->debounce_timer_running = true;
  } else {
    watch_request_restart_now(state);
  }
}

static int watch_add_watcher(watch_state_t *state, const char *path) {
  watch_item_t *item = calloc(1, sizeof(*item));
  if (!item) return UV_ENOMEM;

  item->state = state;
  int rc = ant_watch_start(
    &state->loop,
    &item->event,
    path,
    watch_on_fs_event,
    item,
    0,
    &item->path
  );

  if (rc != 0) {
    free(item);
    return rc;
  }

  watch_item_refresh_stat(item);
  item->next = state->watchers;
  state->watchers = item;
  return 0;
}

static void watch_close_watchers(watch_state_t *state) {
  watch_item_t *item = state->watchers;
  state->watchers = NULL;

  while (item) {
    watch_item_t *next = item->next;
    item->closing = true;
    uv_fs_event_stop(&item->event);
    if (!uv_is_closing((uv_handle_t *)&item->event)) {
      uv_close((uv_handle_t *)&item->event, watch_on_watcher_closed);
    }
    item = next;
  }
}

static int watch_refresh_watchers(watch_state_t *state) {
  watch_path_list_t paths = {0};
  int rc = watch_build_graph(state->watch_path, &paths);
  if (rc != 0) {
    watch_path_list_free(&paths);
    return rc;
  }

  watch_close_watchers(state);

  for (size_t i = 0; i < paths.len; i++) {
    rc = watch_add_watcher(state, paths.items[i]);
    if (rc != 0) {
      if (i == 0) {
        crfprintf(stderr, msg.watch_file_failed, paths.items[i], uv_strerror(rc));
        watch_path_list_free(&paths);
        return rc;
      }
      crfprintf(stderr, msg.watch_file_warn, paths.items[i], uv_strerror(rc));
    }
  }

  watch_path_list_free(&paths);
  return 0;
}

static void watch_stop_kill_timer(watch_state_t *state) {
  if (!state->kill_timer_inited || !state->kill_timer_running) return;
  uv_timer_stop(&state->kill_timer);
  state->kill_timer_running = false;
}

static void watch_stop_debounce_timer(watch_state_t *state) {
  if (!state->debounce_timer_inited || !state->debounce_timer_running) return;
  uv_timer_stop(&state->debounce_timer);
  state->debounce_timer_running = false;
}

static void watch_on_child_closed(uv_handle_t *handle) { free(handle); }

static void watch_on_kill_timer(uv_timer_t *timer) {
  watch_state_t *state = (watch_state_t *)timer->data;
  state->kill_timer_running = false;

  if (state->child_running && state->child)
    uv_process_kill(state->child, SIGKILL);
}

static void watch_on_child_exit(uv_process_t *proc, int64_t exit_status, int term_signal) {
  watch_state_t *state = (watch_state_t *)proc->data;
  state->child_running = false;
  state->child = NULL;
  watch_stop_kill_timer(state);

  if (state->stop_requested) {
    if (state->kill_timer_inited && !uv_is_closing((uv_handle_t *)&state->kill_timer))
      uv_close((uv_handle_t *)&state->kill_timer, NULL);
  } else if (state->restart_pending) {
    watch_try_spawn_child(state);
  } else if (term_signal > 0) {
    state->final_exit_code = 128 + term_signal;
  } else {
    state->final_exit_code = (int)exit_status;
  }

  uv_close((uv_handle_t *)proc, watch_on_child_closed);
}

static int watch_spawn_child(watch_state_t *state) {
  uv_process_t *proc = calloc(1, sizeof(*proc));
  if (!proc) return UV_ENOMEM;

  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_INHERIT_FD; stdio[0].data.fd = 0;
  stdio[1].flags = UV_INHERIT_FD; stdio[1].data.fd = 1;
  stdio[2].flags = UV_INHERIT_FD; stdio[2].data.fd = 2;

  uv_process_options_t options = {0};
  options.file = state->child_argv[0];
  options.args = state->child_argv;
  options.exit_cb = watch_on_child_exit;
  options.stdio_count = 3;
  options.stdio = stdio;

  int rc = uv_spawn(&state->loop, proc, &options);
  if (rc != 0) {
    free(proc);
    return rc;
  }

  proc->data = state;
  state->child = proc;
  state->child_running = true;
  state->restart_pending = false;
  return 0;
}

static void watch_try_spawn_child(watch_state_t *state) {
  if (state->stop_requested) return;

  bool restarting = state->restart_pending;
  if (!state->no_clear_screen && restarting) watch_clear_screen();

  int rc = watch_refresh_watchers(state);
  if (rc != 0) {
    watch_request_shutdown(state, EXIT_FAILURE);
    return;
  }

  rc = watch_spawn_child(state);
  if (rc != 0) {
    crfprintf(stderr, msg.watch_spawn_failed, uv_strerror(rc));
    watch_request_shutdown(state, EXIT_FAILURE);
  }
}

static void watch_start_kill_timer(watch_state_t *state, uint64_t timeout_ms) {
  if (!state->kill_timer_inited || state->kill_timer_running) return;
  int rc = uv_timer_start(&state->kill_timer, watch_on_kill_timer, timeout_ms, 0);
  if (rc == 0) state->kill_timer_running = true;
}

static void watch_close_signals_timers_and_watchers(watch_state_t *state) {
  watch_close_watchers(state);
  watch_stop_debounce_timer(state);

  if (state->sigint_inited && !uv_is_closing((uv_handle_t *)&state->sigint)) {
    uv_signal_stop(&state->sigint);
    uv_close((uv_handle_t *)&state->sigint, NULL);
  }

  if (state->sigterm_inited && !uv_is_closing((uv_handle_t *)&state->sigterm)) {
    uv_signal_stop(&state->sigterm);
    uv_close((uv_handle_t *)&state->sigterm, NULL);
  }

  if (state->debounce_timer_inited && !uv_is_closing((uv_handle_t *)&state->debounce_timer)) {
    uv_close((uv_handle_t *)&state->debounce_timer, NULL);
  }
}

static void watch_request_shutdown(watch_state_t *state, int exit_code) {
  if (state->stop_requested) return;

  state->stop_requested = true;
  state->restart_pending = false;
  state->final_exit_code = exit_code;

  watch_close_signals_timers_and_watchers(state);

  if (state->child_running && state->child) {
    int rc = uv_process_kill(state->child, SIGTERM);
    if (rc != 0 && rc != UV_ESRCH) {
      crfprintf(stderr, msg.watch_graceful_term, uv_strerror(rc));
    }
    watch_start_kill_timer(state, WATCH_CHILD_KILL_GRACE_MS);
  } else if (state->kill_timer_inited && !uv_is_closing((uv_handle_t *)&state->kill_timer)) {
    uv_close((uv_handle_t *)&state->kill_timer, NULL);
  }
}

static void watch_request_restart_now(watch_state_t *state) {
  if (state->debounce_timer_inited) state->debounce_timer_running = false;
  if (state->stop_requested) return;

  state->restart_pending = true;

  if (state->child_running && state->child) {
    if (!state->kill_timer_running) {
      int rc = uv_process_kill(state->child, SIGTERM);
      if (rc != 0 && rc != UV_ESRCH) {
        crfprintf(stderr, msg.watch_child_error, uv_strerror(rc));
        watch_request_shutdown(state, EXIT_FAILURE);
        return;
      }
      watch_start_kill_timer(state, WATCH_CHILD_KILL_GRACE_MS);
    }
  } else watch_try_spawn_child(state);
}

static void watch_on_debounce_timer(uv_timer_t *timer) {
  watch_state_t *state = (watch_state_t *)timer->data;
  state->debounce_timer_running = false;
  watch_request_restart_now(state);
}

static void watch_on_signal(uv_signal_t *handle, int signum) {
  watch_state_t *state = (watch_state_t *)handle->data;
  watch_request_shutdown(state, 128 + signum);
}

int ant_watch_run(int argc, char **argv, const char *entry_file, bool no_clear_screen) {
  if (!entry_file || !*entry_file) {
    crfprintf(stderr, msg.watch_entrypoint_missing);
    return EXIT_FAILURE;
  }

  watch_state_t state = {0};
  state.child_argv = watch_build_child_argv(argc, argv);
  state.watch_path = ant_watch_resolve_path(entry_file);
  state.no_clear_screen = no_clear_screen;
  state.final_exit_code = EXIT_SUCCESS;

  if (!state.child_argv || !state.watch_path) {
    free(state.child_argv);
    free(state.watch_path);
    crfprintf(stderr, msg.watch_start_fatal);
    return EXIT_FAILURE;
  }

  int rc = uv_loop_init(&state.loop);
  if (rc != 0) {
    crfprintf(stderr, msg.watch_loop_fatal, uv_strerror(rc));
    free(state.child_argv); free(state.watch_path);
    return EXIT_FAILURE;
  }

  rc = uv_timer_init(&state.loop, &state.debounce_timer);
  if (rc == 0) {
    state.debounce_timer_inited = true;
    state.debounce_timer.data = &state;
  }

  if (rc == 0) {
    rc = uv_timer_init(&state.loop, &state.kill_timer);
    if (rc == 0) {
      state.kill_timer_inited = true;
      state.kill_timer.data = &state;
    }
  }

  if (rc == 0) {
    rc = watch_refresh_watchers(&state);
  }

  if (rc == 0) {
    rc = uv_signal_init(&state.loop, &state.sigint);
    if (rc == 0) state.sigint_inited = true;
    if (rc == 0) {
      state.sigint.data = &state;
      rc = uv_signal_start(&state.sigint, watch_on_signal, SIGINT);
    }
  }

  if (rc == 0) {
    rc = uv_signal_init(&state.loop, &state.sigterm);
    if (rc == 0) state.sigterm_inited = true;
    if (rc == 0) {
      state.sigterm.data = &state;
      int sigterm_rc = uv_signal_start(&state.sigterm, watch_on_signal, SIGTERM);
      if (sigterm_rc != 0 && sigterm_rc != UV_ENOSYS && sigterm_rc != UV_EINVAL) {
        rc = sigterm_rc;
      } else if (sigterm_rc != 0) {
        uv_signal_stop(&state.sigterm);
        uv_close((uv_handle_t *)&state.sigterm, NULL);
        state.sigterm_inited = false;
      }
    }
  }

  if (rc != 0) {
    crfprintf(stderr, msg.watch_loop_handles_fatal, uv_strerror(rc));
    watch_request_shutdown(&state, EXIT_FAILURE);
  }

  if (!state.stop_requested) watch_spawn_child(&state);
  while (uv_run(&state.loop, UV_RUN_DEFAULT) != 0) {}

  watch_close_signals_timers_and_watchers(&state);
  if (state.kill_timer_inited && !uv_is_closing((uv_handle_t *)&state.kill_timer)) {
    uv_close((uv_handle_t *)&state.kill_timer, NULL);
  }
  while (uv_run(&state.loop, UV_RUN_DEFAULT) != 0) {}

  rc = uv_loop_close(&state.loop);
  if (rc != 0) crfprintf(stderr, msg.watch_loop_cleanup, uv_strerror(rc));

  free(state.child_argv);
  free(state.watch_path);
  return state.final_exit_code;
}
