#include "modules/shell_internal.h"

#include <uv.h>
#include <limits.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ant.h"
#include "internal.h"
#include "modules/child_process.h"

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
} sh_bytes_t;

static bool sh_bytes_reserve(sh_bytes_t *bytes, size_t additional) {
  if (additional > SIZE_MAX - bytes->len - 1) return false;
  size_t need = bytes->len + additional + 1;
  if (need <= bytes->capacity) return true;
  size_t next = bytes->capacity ? bytes->capacity * 2 : 64;
  while (next < need) {
    if (next > SIZE_MAX / 2) return false;
    next *= 2;
  }
  char *grown = realloc(bytes->data, next);
  if (!grown) return false;
  bytes->data = grown;
  bytes->capacity = next;
  return true;
}

static bool sh_bytes_append(sh_bytes_t *bytes, const char *data, size_t len) {
  if (!sh_bytes_reserve(bytes, len)) return false;
  if (len) memcpy(bytes->data + bytes->len, data, len);
  bytes->len += len;
  bytes->data[bytes->len] = '\0';
  return true;
}

static ant_value_t sh_result(
  ant_t *js,
  const char *stdout_text,
  size_t stdout_len,
  const char *stderr_text,
  size_t stderr_len,
  int exit_code
) {
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "stdout", js_mkstr(js, stdout_text ? stdout_text : "", stdout_len));
  js_set(js, result, "stderr", js_mkstr(js, stderr_text ? stderr_text : "", stderr_len));
  js_set(js, result, "exitCode", js_mknum((double)exit_code));
  js_set(js, result, "signalCode", js_mknull());
  return result;
}

static ant_value_t sh_resolved_result(
  ant_t *js,
  const char *stdout_text,
  size_t stdout_len,
  const char *stderr_text,
  size_t stderr_len,
  int exit_code
) {
  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, sh_result(
    js, stdout_text, stdout_len, stderr_text, stderr_len, exit_code
  ));
  return promise;
}

static bool sh_value_append(ant_t *js, sh_bytes_t *bytes, ant_value_t value) {
  ant_value_t string = vtype(value) == T_STR ? value : js_tostring_val(js, value);
  if (is_err(string)) return false;
  size_t len = 0;
  char *text = js_getstr(js, string, &len);
  return text && sh_bytes_append(bytes, text, len);
}

static bool sh_build_word(
  ant_t *js,
  ant_value_t parts,
  ant_value_t values,
  ant_value_t *out
) {
  if (vtype(parts) != T_ARR) return false;
  sh_bytes_t word = {0};
  ant_offset_t part_count = js_arr_len(js, parts);

  for (ant_offset_t i = 0; i < part_count; i++) {
    ant_value_t part = js_arr_get(js, parts, i);
    if (vtype(part) != T_ARR || js_arr_len(js, part) < 3) goto fail;
    ant_value_t kind_value = js_arr_get(js, part, 0);
    if (vtype(kind_value) != T_NUM) goto fail;
    int kind = (int)js_getnum(kind_value);

    if (kind == SH_PART_LITERAL) {
      ant_value_t literal = js_arr_get(js, part, 2);
      if (vtype(literal) != T_STR || !sh_value_append(js, &word, literal)) goto fail;
    } else if (kind == SH_PART_INTERPOLATION) {
      ant_value_t index_value = js_arr_get(js, part, 2);
      if (vtype(index_value) != T_NUM) goto fail;
      int index = (int)js_getnum(index_value);
      if (index < 0 || vtype(values) != T_ARR || (ant_offset_t)index >= js_arr_len(js, values))
        goto fail;
      if (!sh_value_append(js, &word, js_arr_get(js, values, (ant_offset_t)index))) goto fail;
    } else goto fail;
  }

  *out = js_mkstr(js, word.data ? word.data : "", word.len);
  free(word.data);
  return !is_err(*out);

fail:
  free(word.data);
  return false;
}

static bool sh_push_command_word(
  ant_t *js,
  ant_value_t argv,
  ant_value_t word,
  ant_value_t values
) {
  if (vtype(word) != T_ARR) return false;

  // TODO: reduce nesting
  if (js_arr_len(js, word) == 1) {
    ant_value_t part = js_arr_get(js, word, 0);
    if (vtype(part) == T_ARR && js_arr_len(js, part) >= 3) {
      ant_value_t kind_value = js_arr_get(js, part, 0);
      ant_value_t quote_value = js_arr_get(js, part, 1);
      ant_value_t index_value = js_arr_get(js, part, 2);
      if (vtype(kind_value) == T_NUM && (int)js_getnum(kind_value) == SH_PART_INTERPOLATION &&
          vtype(quote_value) == T_NUM && (int)js_getnum(quote_value) == SH_QUOTE_NONE &&
          vtype(index_value) == T_NUM && vtype(values) == T_ARR) {
        int index = (int)js_getnum(index_value);
        if (index >= 0 && (ant_offset_t)index < js_arr_len(js, values)) {
          ant_value_t value = js_arr_get(js, values, (ant_offset_t)index);
          if (vtype(value) == T_ARR) {
            ant_offset_t count = js_arr_len(js, value);
            for (ant_offset_t i = 0; i < count; i++) {
              ant_value_t item = js_tostring_val(js, js_arr_get(js, value, i));
              if (is_err(item)) return false;
              js_arr_push(js, argv, item);
            }
            return true;
          }
        }
      }
    }
  }

  ant_value_t string = js_mkundef();
  if (!sh_build_word(js, word, values, &string)) return false;
  js_arr_push(js, argv, string);
  return true;
}

static ant_value_t sh_run_builtin(
  ant_t *js,
  ant_value_t context,
  ant_value_t argv
) {
  ant_offset_t argc = js_arr_len(js, argv);
  if (argc == 0) return sh_resolved_result(js, "", 0, "", 0, 0);

  ant_value_t name_value = js_arr_get(js, argv, 0);
  size_t name_len = 0;
  char *name = js_getstr(js, name_value, &name_len);
  if (!name) return js_mkundef();

  if (name_len == 4 && memcmp(name, "true", 4) == 0)
    return sh_resolved_result(js, "", 0, "", 0, 0);
  if (name_len == 5 && memcmp(name, "false", 5) == 0)
    return sh_resolved_result(js, "", 0, "", 0, 1);
  if (name_len == 1 && name[0] == ':')
    return sh_resolved_result(js, "", 0, "", 0, 0);

  if (name_len == 4 && memcmp(name, "exit", 4) == 0) {
    int status = 0;
    if (argc > 1) {
      ant_value_t status_value = js_arr_get(js, argv, 1);
      ant_value_t status_string = js_tostring_val(js, status_value);
      size_t status_len = 0;
      char *status_text = js_getstr(js, status_string, &status_len);
      if (!status_text || status_len == 0 || status_len > 10) status = 2;
      else {
        char local[12];
        memcpy(local, status_text, status_len);
        local[status_len] = '\0';
        char *end = NULL;
        long parsed = strtol(local, &end, 10);
        status = end && *end == '\0' ? (int)((unsigned long)parsed & 255u) : 2;
      }
    }
    return sh_resolved_result(js, "", 0, "", 0, status);
  }

  if (name_len == 4 && memcmp(name, "echo", 4) == 0) {
    sh_bytes_t output = {0};
    for (ant_offset_t i = 1; i < argc; i++) {
      if (i > 1 && !sh_bytes_append(&output, " ", 1)) goto echo_oom;
      if (!sh_value_append(js, &output, js_arr_get(js, argv, i))) goto echo_oom;
    }
    if (!sh_bytes_append(&output, "\n", 1)) goto echo_oom;
    ant_value_t promise = sh_resolved_result(js, output.data, output.len, "", 0, 0);
    free(output.data);
    return promise;
echo_oom:
    free(output.data);
    return sh_resolved_result(js, "", 0, "echo: out of memory\n", sizeof("echo: out of memory\n") - 1, 1);
  }

  if (name_len == 3 && memcmp(name, "pwd", 3) == 0) {
    ant_value_t cwd = js_get(js, context, "cwd");
    size_t cwd_len = 0;
    char *cwd_text = js_getstr(js, cwd, &cwd_len);
    if (!cwd_text) return sh_resolved_result(js, "", 0, "pwd: invalid cwd\n", sizeof("pwd: invalid cwd\n") - 1, 1);
    sh_bytes_t output = {0};
    if (!sh_bytes_append(&output, cwd_text, cwd_len) || !sh_bytes_append(&output, "\n", 1)) {
      free(output.data);
      return sh_resolved_result(js, "", 0, "pwd: out of memory\n", sizeof("pwd: out of memory\n") - 1, 1);
    }
    ant_value_t promise = sh_resolved_result(js, output.data, output.len, "", 0, 0);
    free(output.data);
    return promise;
  }

  if (name_len == 2 && memcmp(name, "cd", 2) == 0) {
    ant_value_t target_value;
    if (argc > 1) target_value = js_arr_get(js, argv, 1);
    else {
      const char *home = getenv("HOME");
      if (!home) return sh_resolved_result(js, "", 0, "cd: HOME not set\n", sizeof("cd: HOME not set\n") - 1, 1);
      target_value = js_mkstr(js, home, strlen(home));
    }
    size_t target_len = 0;
    char *target = js_getstr(js, target_value, &target_len);
    ant_value_t cwd_value = js_get(js, context, "cwd");
    size_t cwd_len = 0;
    char *cwd = js_getstr(js, cwd_value, &cwd_len);
    if (!target || !cwd) return sh_resolved_result(js, "", 0, "cd: invalid path\n", sizeof("cd: invalid path\n") - 1, 1);

    sh_bytes_t path = {0};
    bool absolute = target_len > 0 && (target[0] == '/' || target[0] == '\\');
    if ((!absolute && (!sh_bytes_append(&path, cwd, cwd_len) ||
        !sh_bytes_append(&path, "/", 1))) || !sh_bytes_append(&path, target, target_len)) {
      free(path.data);
      return sh_resolved_result(js, "", 0, "cd: out of memory\n", sizeof("cd: out of memory\n") - 1, 1);
    }

    uv_fs_t request;
    int rc = uv_fs_realpath(uv_default_loop(), &request, path.data, NULL);
    free(path.data);
    if (rc < 0) {
      const char *reason = uv_strerror(rc);
      sh_bytes_t error = {0};
      sh_bytes_append(&error, "cd: ", 4);
      sh_bytes_append(&error, target, target_len);
      sh_bytes_append(&error, ": ", 2);
      sh_bytes_append(&error, reason, strlen(reason));
      sh_bytes_append(&error, "\n", 1);
      ant_value_t promise = sh_resolved_result(js, "", 0, error.data, error.len, 1);
      free(error.data);
      uv_fs_req_cleanup(&request);
      return promise;
    }
    const char *resolved = request.ptr;
    uv_fs_t stat_request;
    int stat_rc = uv_fs_stat(uv_default_loop(), &stat_request, resolved, NULL);
    bool is_directory = stat_rc >= 0 && S_ISDIR(stat_request.statbuf.st_mode);
    uv_fs_req_cleanup(&stat_request);
    if (!is_directory) {
      const char *reason = uv_strerror(stat_rc < 0 ? stat_rc : UV_ENOTDIR);
      sh_bytes_t error = {0};
      sh_bytes_append(&error, "cd: ", 4);
      sh_bytes_append(&error, target, target_len);
      sh_bytes_append(&error, ": ", 2);
      sh_bytes_append(&error, reason, strlen(reason));
      sh_bytes_append(&error, "\n", 1);
      ant_value_t promise = sh_resolved_result(js, "", 0, error.data, error.len, 1);
      free(error.data);
      uv_fs_req_cleanup(&request);
      return promise;
    }
    js_set(js, context, "cwd", js_mkstr(js, resolved, strlen(resolved)));
    uv_fs_req_cleanup(&request);
    return sh_resolved_result(js, "", 0, "", 0, 0);
  }

  return js_mkundef();
}

static bool sh_build_command_argv(
  ant_t *js,
  ant_value_t command,
  ant_value_t values,
  ant_value_t *argv_out,
  ant_value_t *redirs_out
) {
  if (vtype(command) != T_ARR || js_arr_len(js, command) < 2) return false;
  ant_value_t words = js_arr_get(js, command, 0);
  ant_value_t redirs = js_arr_get(js, command, 1);
  if (vtype(words) != T_ARR || vtype(redirs) != T_ARR) return false;

  ant_value_t argv = js_mkarr(js);
  ant_offset_t word_count = js_arr_len(js, words);
  for (ant_offset_t i = 0; i < word_count; i++) {
    if (!sh_push_command_word(js, argv, js_arr_get(js, words, i), values)) return false;
  }
  *argv_out = argv;
  *redirs_out = redirs;
  return true;
}

static char *sh_resolve_path(
  ant_t *js,
  ant_value_t context,
  ant_value_t path_value
) {
  size_t path_len = 0;
  char *path = js_getstr(js, path_value, &path_len);
  if (!path) return NULL;
#ifdef _WIN32
  bool absolute = path_len > 0 && (
    path[0] == '/' || path[0] == '\\' ||
    (path_len > 2 && path[1] == ':' && (path[2] == '/' || path[2] == '\\'))
  );
#else
  bool absolute = path_len > 0 && path[0] == '/';
#endif
  if (absolute) {
    char *copy = malloc(path_len + 1);
    if (!copy) return NULL;
    memcpy(copy, path, path_len);
    copy[path_len] = '\0';
    return copy;
  }

  ant_value_t cwd_value = js_get(js, context, "cwd");
  size_t cwd_len = 0;
  char *cwd = js_getstr(js, cwd_value, &cwd_len);
  if (!cwd || cwd_len > SIZE_MAX - path_len - 2) return NULL;
  char *joined = malloc(cwd_len + 1 + path_len + 1);
  if (!joined) return NULL;
  memcpy(joined, cwd, cwd_len);
  joined[cwd_len] = '/';
  memcpy(joined + cwd_len + 1, path, path_len);
  joined[cwd_len + 1 + path_len] = '\0';
  return joined;
}

static bool sh_read_redirect_file(
  ant_t *js,
  const char *path,
  ant_value_t *contents,
  int *error_code
) {
  uv_fs_t request;
  int fd = uv_fs_open(uv_default_loop(), &request, path, O_RDONLY, 0, NULL);
  uv_fs_req_cleanup(&request);
  if (fd < 0) {
    *error_code = fd;
    return false;
  }

  sh_bytes_t bytes = {0};
  int64_t offset = 0;
  for (;;) {
    char chunk[16384];
    uv_buf_t buffer = uv_buf_init(chunk, sizeof(chunk));
    int read = uv_fs_read(uv_default_loop(), &request, fd, &buffer, 1, offset, NULL);
    uv_fs_req_cleanup(&request);
    if (read < 0) {
      *error_code = read;
      free(bytes.data);
      uv_fs_close(uv_default_loop(), &request, fd, NULL);
      uv_fs_req_cleanup(&request);
      return false;
    }
    if (read == 0) break;
    if (!sh_bytes_append(&bytes, chunk, (size_t)read)) {
      *error_code = UV_ENOMEM;
      free(bytes.data);
      uv_fs_close(uv_default_loop(), &request, fd, NULL);
      uv_fs_req_cleanup(&request);
      return false;
    }
    offset += read;
  }
  uv_fs_close(uv_default_loop(), &request, fd, NULL);
  uv_fs_req_cleanup(&request);
  *contents = js_mkstr(js, bytes.data ? bytes.data : "", bytes.len);
  free(bytes.data);
  return !is_err(*contents);
}

static ant_value_t sh_redirection_error(
  ant_t *js,
  const char *path,
  int error_code
) {
  sh_bytes_t message = {0};
  sh_bytes_append(&message, "ant:shell: ", sizeof("ant:shell: ") - 1);
  sh_bytes_append(&message, path, strlen(path));
  sh_bytes_append(&message, ": ", 2);
  const char *reason = uv_strerror(error_code);
  sh_bytes_append(&message, reason, strlen(reason));
  sh_bytes_append(&message, "\n", 1);
  ant_value_t promise = sh_resolved_result(js, "", 0,
    message.data, message.len, 1);
  free(message.data);
  return promise;
}

static bool sh_prepare_redirections(
  ant_t *js,
  ant_value_t context,
  ant_value_t redirs,
  ant_value_t values,
  ant_value_t options,
  ant_value_t *state_out,
  ant_value_t *error_promise
) {
  ant_value_t state = js_mkobj(js);
  js_set(js, state, "outputPath", js_mkundef());
  js_set(js, state, "outputAppend", js_false);
  js_set(js, state, "stderrMode", js_mknum(0));
  js_set(js, state, "stderrPath", js_mkundef());
  js_set(js, state, "stderrAppend", js_false);

  ant_offset_t count = js_arr_len(js, redirs);
  for (ant_offset_t i = 0; i < count; i++) {
    ant_value_t redir = js_arr_get(js, redirs, i);
    if (vtype(redir) != T_ARR || js_arr_len(js, redir) < 1) return false;
    ant_value_t kind_value = js_arr_get(js, redir, 0);
    if (vtype(kind_value) != T_NUM) return false;
    int kind = (int)js_getnum(kind_value);
    if (kind == SH_REDIR_STDERR_TO_STDOUT) {
      ant_value_t output_path = js_get(js, state, "outputPath");
      if (vtype(output_path) == T_STR) {
        js_set(js, state, "stderrMode", js_mknum(2));
        js_set(js, state, "stderrPath", output_path);
        js_set(js, state, "stderrAppend", js_get(js, state, "outputAppend"));
      } else {
        js_set(js, state, "stderrMode", js_mknum(1));
        js_set(js, state, "stderrPath", js_mkundef());
      }
      continue;
    }
    if (js_arr_len(js, redir) < 2) return false;
    ant_value_t target = js_mkundef();
    if (!sh_build_word(js, js_arr_get(js, redir, 1), values, &target)) return false;
    char *path = sh_resolve_path(js, context, target);
    if (!path) return false;

    if (kind == SH_REDIR_STDIN) {
      ant_value_t contents = js_mkundef();
      int error_code = 0;
      if (!sh_read_redirect_file(js, path, &contents, &error_code)) {
        *error_promise = sh_redirection_error(js, path, error_code);
        free(path);
        return false;
      }
      js_set(js, options, "input", contents);
      free(path);
      continue;
    }

    bool append = kind == SH_REDIR_STDOUT_APPEND;
    int flags = O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC);
    uv_fs_t request;
    int fd = uv_fs_open(uv_default_loop(), &request, path, flags, 0666, NULL);
    uv_fs_req_cleanup(&request);
    if (fd < 0) {
      *error_promise = sh_redirection_error(js, path, fd);
      free(path);
      return false;
    }
    uv_fs_close(uv_default_loop(), &request, fd, NULL);
    uv_fs_req_cleanup(&request);
    js_set(js, state, "outputPath", js_mkstr(js, path, strlen(path)));
    js_set(js, state, "outputAppend", js_bool(append));
    free(path);
  }

  *state_out = state;
  return true;
}

static void sh_append_result_stderr(
  ant_t *js,
  ant_value_t result,
  const char *text,
  size_t text_len
) {
  ant_value_t old = js_get(js, result, "stderr");
  size_t old_len = 0;
  char *old_text = vtype(old) == T_STR ? js_getstr(js, old, &old_len) : NULL;
  sh_bytes_t combined = {0};
  if (old_text) sh_bytes_append(&combined, old_text, old_len);
  sh_bytes_append(&combined, text, text_len);
  js_set(js, result, "stderr", js_mkstr(js,
    combined.data ? combined.data : "", combined.len));
  free(combined.data);
}

static bool sh_same_path(ant_t *js, ant_value_t left, ant_value_t right) {
  if (vtype(left) != T_STR || vtype(right) != T_STR) return false;
  size_t left_len = 0;
  size_t right_len = 0;
  char *left_text = js_getstr(js, left, &left_len);
  char *right_text = js_getstr(js, right, &right_len);
  return left_text && right_text && left_len == right_len &&
    memcmp(left_text, right_text, left_len) == 0;
}

static bool sh_write_redirect_file(
  ant_t *js,
  ant_value_t result,
  ant_value_t path_value,
  const char *text,
  size_t text_len,
  bool append
) {
  size_t path_len = 0;
  char *path = js_getstr(js, path_value, &path_len);
  if (!path || (!text && text_len)) return false;

  int flags = O_CREAT | O_WRONLY | (append ? O_APPEND : 0);
  uv_fs_t request;
  int fd = uv_fs_open(uv_default_loop(), &request, path, flags, 0666, NULL);
  uv_fs_req_cleanup(&request);
  int failure = fd < 0 ? fd : 0;

  size_t written_total = 0;
  while (!failure && written_total < text_len) {
    size_t remaining = text_len - written_total;
    unsigned int chunk_len = remaining > UINT_MAX ? UINT_MAX : (unsigned int)remaining;
    uv_buf_t buffer = uv_buf_init((char *)text + written_total, chunk_len);
    int written = uv_fs_write(
      uv_default_loop(), &request, fd, &buffer, 1,
      append ? -1 : (int64_t)written_total, NULL
    );
    uv_fs_req_cleanup(&request);
    if (written <= 0) failure = written < 0 ? written : UV_EIO;
    else written_total += (size_t)written;
  }

  if (fd >= 0) {
    int close_result = uv_fs_close(uv_default_loop(), &request, fd, NULL);
    uv_fs_req_cleanup(&request);
    if (!failure && close_result < 0) failure = close_result;
  }
  if (!failure) return true;

  sh_bytes_t message = {0};
  sh_bytes_append(&message, "ant:shell: ", sizeof("ant:shell: ") - 1);
  sh_bytes_append(&message, path, path_len);
  sh_bytes_append(&message, ": ", 2);
  const char *reason = uv_strerror(failure);
  sh_bytes_append(&message, reason, strlen(reason));
  sh_bytes_append(&message, "\n", 1);
  sh_append_result_stderr(js, result, message.data, message.len);
  free(message.data);
  js_set(js, result, "exitCode", js_mknum(1));
  return false;
}

static ant_value_t sh_apply_redirections_fulfilled(
  ant_t *js,
  ant_value_t *args,
  int nargs
) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  if (!is_special_object(result)) return result;

  ant_value_t stdout_value = js_get(js, result, "stdout");
  ant_value_t stderr_value = js_get(js, result, "stderr");
  size_t stdout_len = 0;
  size_t stderr_len = 0;
  char *stdout_text = vtype(stdout_value) == T_STR
    ? js_getstr(js, stdout_value, &stdout_len) : NULL;
  char *stderr_text = vtype(stderr_value) == T_STR
    ? js_getstr(js, stderr_value, &stderr_len) : NULL;

  ant_value_t output_path = js_get(js, state, "outputPath");
  ant_value_t stderr_path = js_get(js, state, "stderrPath");
  ant_value_t stderr_mode_value = js_get(js, state, "stderrMode");
  int stderr_mode = vtype(stderr_mode_value) == T_NUM
    ? (int)js_getnum(stderr_mode_value) : 0;

  sh_bytes_t captured_stdout = {0};
  if (vtype(output_path) != T_STR && stdout_text)
    sh_bytes_append(&captured_stdout, stdout_text, stdout_len);
  if (stderr_mode == 1 && stderr_text)
    sh_bytes_append(&captured_stdout, stderr_text, stderr_len);

  js_set(js, result, "stdout", js_mkstr(js,
    captured_stdout.data ? captured_stdout.data : "", captured_stdout.len));
  free(captured_stdout.data);
  js_set(js, result, "stderr", stderr_mode == 0 && stderr_text
    ? js_mkstr(js, stderr_text, stderr_len) : js_mkstr(js, "", 0));

  if (vtype(output_path) == T_STR) {
    bool append = js_truthy(js, js_get(js, state, "outputAppend"));
    (void)sh_write_redirect_file(
      js, result, output_path, stdout_text, stdout_len, append
    );
  }
  if (stderr_mode == 2 && vtype(stderr_path) == T_STR) {
    bool append = js_truthy(js, js_get(js, state, "stderrAppend"));
    if (sh_same_path(js, output_path, stderr_path)) append = true;
    (void)sh_write_redirect_file(
      js, result, stderr_path, stderr_text, stderr_len, append
    );
  }
  return result;
}

static ant_value_t sh_apply_redirections(
  ant_t *js,
  ant_value_t promise,
  ant_value_t state
) {
  return js_promise_then(js, promise,
    js_heavy_mkfun(js, sh_apply_redirections_fulfilled, state), js_mkundef());
}

ant_value_t sh_runtime_run(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3 || !is_special_object(args[0]) || vtype(args[1]) != T_ARR ||
      vtype(args[2]) != T_ARR) {
    return sh_resolved_result(js, "", 0, "ant:shell: invalid compiled pipeline\n", sizeof("ant:shell: invalid compiled pipeline\n") - 1, 2);
  }

  ant_value_t context = args[0];
  ant_value_t pipeline = args[1];
  ant_value_t values = args[2];
  ant_offset_t command_count = js_arr_len(js, pipeline);
  if (command_count == 0) return sh_resolved_result(js, "", 0, "", 0, 0);

  ant_value_t options = js_mkobj(js);
  ant_value_t cwd = js_get(js, context, "cwd");
  if (vtype(cwd) == T_STR) js_set(js, options, "cwd", cwd);

  // TODO: reduce nesting
  if (command_count > 1) {
    ant_value_t commands = js_mkarr(js);
    ant_value_t pipeline_redirs = js_mkarr(js);
    for (ant_offset_t i = 0; i < command_count; i++) {
      ant_value_t argv = js_mkundef();
      ant_value_t redirs = js_mkundef();
      if (!sh_build_command_argv(
        js, js_arr_get(js, pipeline, i), values, &argv, &redirs
      )) return sh_resolved_result(js, "", 0,
        "ant:shell: invalid pipeline command\n", sizeof("ant:shell: invalid pipeline command\n") - 1, 2);
      ant_offset_t redir_count = js_arr_len(js, redirs);
      for (ant_offset_t j = 0; j < redir_count; j++) {
        ant_value_t redir = js_arr_get(js, redirs, j);
        int kind = vtype(redir) == T_ARR && js_arr_len(js, redir) > 0 &&
          vtype(js_arr_get(js, redir, 0)) == T_NUM
          ? (int)js_getnum(js_arr_get(js, redir, 0)) : -1;
        bool allowed = (i == 0 && kind == SH_REDIR_STDIN) ||
          (i + 1 == command_count && kind != SH_REDIR_STDIN);
        if (!allowed) return sh_resolved_result(js, "", 0,
          "ant:shell: redirection on an intermediate pipeline stage is not implemented yet\n",
          sizeof("ant:shell: redirection on an intermediate pipeline stage is not implemented yet\n") - 1, 2);
        js_arr_push(js, pipeline_redirs, redir);
      }
      js_arr_push(js, commands, argv);
    }
    ant_value_t redir_state = js_mkundef();
    ant_value_t redir_error = js_mkundef();
    if (!sh_prepare_redirections(
      js, context, pipeline_redirs, values, options, &redir_state, &redir_error
    )) return is_undefined(redir_error)
      ? sh_resolved_result(js, "", 0, "ant:shell: invalid redirection\n",
          sizeof("ant:shell: invalid redirection\n") - 1, 2)
      : redir_error;
    return sh_apply_redirections(js,
      child_process_pipeline_result(js, commands, options), redir_state);
  }

  ant_value_t command = js_arr_get(js, pipeline, 0);
  ant_value_t argv = js_mkundef();
  ant_value_t redirs = js_mkundef();
  if (!sh_build_command_argv(js, command, values, &argv, &redirs))
    return sh_resolved_result(js, "", 0, "ant:shell: invalid command\n", sizeof("ant:shell: invalid command\n") - 1, 2);
  ant_value_t redir_state = js_mkundef();
  ant_value_t redir_error = js_mkundef();
  if (!sh_prepare_redirections(
    js, context, redirs, values, options, &redir_state, &redir_error
  )) return is_undefined(redir_error)
    ? sh_resolved_result(js, "", 0, "ant:shell: invalid redirection\n",
        sizeof("ant:shell: invalid redirection\n") - 1, 2)
    : redir_error;

  if (js_arr_len(js, argv) == 0) return sh_resolved_result(js, "", 0, "", 0, 0);

  ant_value_t builtin = sh_run_builtin(js, context, argv);
  if (!is_undefined(builtin)) return sh_apply_redirections(js, builtin, redir_state);

  ant_value_t file = js_arr_get(js, argv, 0);
  ant_value_t child_argv = js_mkarr(js);
  for (ant_offset_t i = 1; i < js_arr_len(js, argv); i++)
    js_arr_push(js, child_argv, js_arr_get(js, argv, i));

  return sh_apply_redirections(js,
    child_process_exec_file_result(js, file, child_argv, options), redir_state);
}

ant_value_t sh_runtime_context(ant_t *js) {
  size_t capacity = 256;
  char *cwd = NULL;
  int rc;
  do {
    char *grown = realloc(cwd, capacity);
    if (!grown) {
      free(cwd);
      return js_mkerr(js, "Out of memory");
    }
    cwd = grown;
    size_t size = capacity;
    rc = uv_cwd(cwd, &size);
    if (rc == UV_ENOBUFS) capacity = size + 1;
  } while (rc == UV_ENOBUFS);

  if (rc < 0) {
    free(cwd);
    return js_mkerr(js, "Failed to get current directory: %s", uv_strerror(rc));
  }

  ant_value_t context = js_mkobj(js);
  js_set(js, context, "cwd", js_mkstr(js, cwd, strlen(cwd)));
  free(cwd);
  return context;
}
