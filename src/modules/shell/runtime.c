#include "shell_internal.h"

#include <uv.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ant.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "internal.h"
#include "modules/child_process.h"
#include "ptr.h"

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
  js_set(js, result, "exited", js_false);
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

static ant_value_t sh_reject_current_exception(ant_t *js) {
  ant_value_t error = js->thrown_exists ? js->thrown_value : js_mkerr(js, "ant:shell failed");
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
  js->thrown_stack = js_mkundef();
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, error);
  return promise;
}

static ant_value_t sh_abrupt_or_error(ant_t *js, const char *message) {
  if (!js->thrown_exists) js_mkerr(js, "%s", message);
  return sh_reject_current_exception(js);
}

static ant_value_t sh_rejected_type_error(ant_t *js, const char *message) {
  js_mkerr_typed(js, JS_ERR_TYPE, "%s", message);
  return sh_reject_current_exception(js);
}

static bool sh_argv_has_nul(ant_t *js, ant_value_t argv) {
  ant_offset_t count = js_arr_len(js, argv);
  for (ant_offset_t i = 0; i < count; i++) {
    size_t len = 0;
    char *text = js_getstr(js, js_arr_get(js, argv, i), &len);
    if (text && memchr(text, '\0', len)) return true;
  }
  return false;
}

static bool sh_build_word(
  ant_t *js,
  const sh_word_t *word_plan,
  ant_value_t values,
  ant_value_t *out
) {
  if (!word_plan || vtype(values) != T_ARR) return false;
  sh_bytes_t word = {0};

  for (size_t i = 0; i < word_plan->part_count; i++) {
    const sh_word_part_t *part = &word_plan->parts[i];
    if (part->kind == SH_PART_LITERAL) {
      if (!sh_bytes_append(
        &word, part->text ? part->text : "", part->text_len
      )) goto fail;
    } else if (part->kind == SH_PART_INTERPOLATION) {
      if (part->interpolation >= (size_t)js_arr_len(js, values)) goto fail;
      if (!sh_value_append(js, &word, js_arr_get(
        js, values, (ant_offset_t)part->interpolation
      ))) goto fail;
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
  const sh_word_t *word,
  ant_value_t values
) {
  if (!word || vtype(values) != T_ARR) return false;

  if (word->part_count == 1) {
    const sh_word_part_t *part = &word->parts[0];
    if (part->kind == SH_PART_INTERPOLATION &&
        part->quote == SH_QUOTE_NONE &&
        part->interpolation < (size_t)js_arr_len(js, values)) {
      ant_value_t value = js_arr_get(
        js, values, (ant_offset_t)part->interpolation
      );
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

  ant_value_t string = js_mkundef();
  if (!sh_build_word(js, word, values, &string)) return false;
  js_arr_push(js, argv, string);
  return true;
}

static bool sh_path_is_absolute(const char *path, size_t len) {
#ifdef _WIN32
  return len > 0 && (
    path[0] == '/' || path[0] == '\\' ||
    (len > 2 && path[1] == ':' &&
      (path[2] == '/' || path[2] == '\\'))
  );
#else
  return len > 0 && path[0] == '/';
#endif
}

static ant_value_t sh_run_builtin_result(
  ant_t *js,
  ant_value_t context,
  ant_value_t argv
) {
  ant_offset_t argc = js_arr_len(js, argv);
  if (argc == 0) return sh_result(js, "", 0, "", 0, 0);

  ant_value_t name_value = js_arr_get(js, argv, 0);
  size_t name_len = 0;
  char *name = js_getstr(js, name_value, &name_len);
  if (!name) return js_mkundef();

  if (name_len == 4 && memcmp(name, "true", 4) == 0)
    return sh_result(js, "", 0, "", 0, 0);
  if (name_len == 5 && memcmp(name, "false", 5) == 0)
    return sh_result(js, "", 0, "", 0, 1);
  if (name_len == 1 && name[0] == ':')
    return sh_result(js, "", 0, "", 0, 0);

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
    ant_value_t result = sh_result(js, "", 0, "", 0, status);
    js_set(js, result, "exited", js_true);
    return result;
  }

  if (name_len == 4 && memcmp(name, "echo", 4) == 0) {
    sh_bytes_t output = {0};
    for (ant_offset_t i = 1; i < argc; i++) {
      if (i > 1 && !sh_bytes_append(&output, " ", 1)) goto echo_oom;
      if (!sh_value_append(js, &output, js_arr_get(js, argv, i))) goto echo_oom;
    }
    if (!sh_bytes_append(&output, "\n", 1)) goto echo_oom;
    ant_value_t result = sh_result(js, output.data, output.len, "", 0, 0);
    free(output.data);
    return result;
echo_oom:
    free(output.data);
    return sh_result(js, "", 0, "echo: out of memory\n", sizeof("echo: out of memory\n") - 1, 1);
  }

  if (name_len == 3 && memcmp(name, "pwd", 3) == 0) {
    ant_value_t cwd = js_get(js, context, "cwd");
    size_t cwd_len = 0;
    char *cwd_text = js_getstr(js, cwd, &cwd_len);
    if (!cwd_text) return sh_result(js, "", 0, "pwd: invalid cwd\n", sizeof("pwd: invalid cwd\n") - 1, 1);
    sh_bytes_t output = {0};
    if (!sh_bytes_append(&output, cwd_text, cwd_len) || !sh_bytes_append(&output, "\n", 1)) {
      free(output.data);
      return sh_result(js, "", 0, "pwd: out of memory\n", sizeof("pwd: out of memory\n") - 1, 1);
    }
    ant_value_t result = sh_result(js, output.data, output.len, "", 0, 0);
    free(output.data);
    return result;
  }

  if (name_len == 2 && memcmp(name, "cd", 2) == 0) {
    ant_value_t target_value;
    if (argc > 1) target_value = js_arr_get(js, argv, 1);
    else {
      const char *home = getenv("HOME");
      if (!home) return sh_result(js, "", 0, "cd: HOME not set\n", sizeof("cd: HOME not set\n") - 1, 1);
      target_value = js_mkstr(js, home, strlen(home));
    }
    size_t target_len = 0;
    char *target = js_getstr(js, target_value, &target_len);
    ant_value_t cwd_value = js_get(js, context, "cwd");
    size_t cwd_len = 0;
    char *cwd = js_getstr(js, cwd_value, &cwd_len);
    if (!target || !cwd) return sh_result(js, "", 0, "cd: invalid path\n", sizeof("cd: invalid path\n") - 1, 1);

    sh_bytes_t path = {0};
    bool absolute = sh_path_is_absolute(target, target_len);
    if ((!absolute && (!sh_bytes_append(&path, cwd, cwd_len) ||
        !sh_bytes_append(&path, "/", 1))) || !sh_bytes_append(&path, target, target_len)) {
      free(path.data);
      return sh_result(js, "", 0, "cd: out of memory\n", sizeof("cd: out of memory\n") - 1, 1);
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
      ant_value_t result = sh_result(js, "", 0, error.data, error.len, 1);
      free(error.data);
      uv_fs_req_cleanup(&request);
      return result;
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
      ant_value_t result = sh_result(js, "", 0, error.data, error.len, 1);
      free(error.data);
      uv_fs_req_cleanup(&request);
      return result;
    }
    js_set(js, context, "cwd", js_mkstr(js, resolved, strlen(resolved)));
    uv_fs_req_cleanup(&request);
    return sh_result(js, "", 0, "", 0, 0);
  }

  return js_mkundef();
}

static ant_value_t sh_run_builtin(
  ant_t *js,
  ant_value_t context,
  ant_value_t argv
) {
  ant_value_t result = sh_run_builtin_result(js, context, argv);
  if (is_undefined(result) || is_err(result)) return result;
  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, result);
  return promise;
}

static bool sh_build_command_argv(
  ant_t *js,
  const sh_command_t *command,
  ant_value_t values,
  ant_value_t *argv_out
) {
  if (!command) return false;

  ant_value_t argv = js_mkarr(js);
  for (size_t i = 0; i < command->word_count; i++) {
    if (!sh_push_command_word(js, argv, &command->words[i], values))
      return false;
  }
  *argv_out = argv;
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
  if (memchr(path, '\0', path_len)) {
    js_mkerr_typed(js, JS_ERR_TYPE, "ant:shell: redirection paths cannot contain NUL bytes");
    return NULL;
  }
  bool absolute = sh_path_is_absolute(path, path_len);
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

static bool sh_prepare_redirections(
  ant_t *js,
  ant_value_t context,
  const sh_pipeline_t *pipeline,
  ant_value_t values,
  ant_value_t options,
  ant_value_t *state_out
) {
  if (!pipeline) return false;
  ant_value_t state = js_mkobj(js);
  ant_value_t redirect_plan = js_mkarr(js);
  js_set(js, options, "redirections", redirect_plan);
  js_set(js, state, "redirections", redirect_plan);
  js_set(js, state, "outputPath", js_mkundef());
  js_set(js, state, "outputAppend", js_false);
  js_set(js, state, "stderrMode", js_mknum(0));
  js_set(js, state, "stderrPath", js_mkundef());
  js_set(js, state, "stderrAppend", js_false);

  for (size_t command_index = 0;
       command_index < pipeline->command_count;
       command_index++) {
    const sh_command_t *command = &pipeline->commands[command_index];
    for (size_t i = 0; i < command->redir_count; i++) {
      const sh_redir_t *redir = &command->redirs[i];
      sh_redir_kind_t kind = redir->kind;
      if (kind == SH_REDIR_STDERR_TO_STDOUT) {
        ant_value_t action = js_mkobj(js);
        js_set(js, action, "kind", js_mknum(SH_REDIR_STDERR_TO_STDOUT));
        js_arr_push(js, redirect_plan, action);
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

      ant_value_t target = js_mkundef();
      if (!sh_build_word(js, &redir->target, values, &target)) return false;
      char *path = sh_resolve_path(js, context, target);
      if (!path) return false;

      ant_value_t action = js_mkobj(js);
      js_set(js, action, "kind", js_mknum((double)kind));
      js_set(js, action, "path", js_mkstr(js, path, strlen(path)));
      js_arr_push(js, redirect_plan, action);

      if (kind == SH_REDIR_STDIN) {
        free(path);
        continue;
      }

      bool append = kind == SH_REDIR_STDOUT_APPEND;
      js_set(js, state, "outputPath", js_mkstr(js, path, strlen(path)));
      js_set(js, state, "outputAppend", js_bool(append));
      free(path);
    }
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

#define SH_REDIRECT_WRITE_CHUNK_SIZE (64u * 1024u)

typedef struct {
  ant_value_t path;
  ant_value_t text;
  size_t text_len;
  bool append;
} sh_redirect_spec_t;

typedef struct {
  char *path;
  size_t path_len;
  ant_value_t text;
  size_t text_len;
  bool append;
} sh_redirect_target_t;

typedef struct {
  ant_t *js;
  ant_value_t promise;
  ant_value_t result;
  sh_redirect_target_t *targets;
  size_t target_count;
  size_t target_index;
  size_t written_total;
  size_t chunk_offset;
  size_t chunk_len;
  char *chunk;
  uv_file fd;
  int failure;
  uv_fs_t request;
} sh_redirect_write_t;

static void sh_redirect_write_next(sh_redirect_write_t *write);
static void sh_redirect_write_open_done(uv_fs_t *request);

static void sh_redirect_write_dispose(sh_redirect_write_t *write) {
  if (!write) return;
  for (size_t i = 0; i < write->target_count; i++)
    free(write->targets[i].path);
  free(write->targets);
  free(write->chunk);
  free(write);
}

static void sh_redirect_write_finish(sh_redirect_write_t *write) {
  ant_t *js = write->js;
  ant_value_t promise = write->promise;
  ant_value_t result = write->result;
  sh_redirect_write_dispose(write);
  js_resolve_promise(js, promise, result);
  gc_unroot_pending_promise(js, js_obj_ptr(promise));
}

static void sh_redirect_write_report_error(sh_redirect_write_t *write) {
  sh_redirect_target_t *target = &write->targets[write->target_index];
  sh_bytes_t message = {0};
  sh_bytes_append(&message, "ant:shell: ", sizeof("ant:shell: ") - 1);
  sh_bytes_append(&message, target->path, target->path_len);
  sh_bytes_append(&message, ": ", 2);
  const char *reason = uv_strerror(write->failure);
  sh_bytes_append(&message, reason, strlen(reason));
  sh_bytes_append(&message, "\n", 1);
  sh_append_result_stderr(
    write->js, write->result, message.data ? message.data : "", message.len
  );
  free(message.data);
  js_set(write->js, write->result, "exitCode", js_mknum(1));
}

static void sh_redirect_write_advance(sh_redirect_write_t *write) {
  if (write->failure) sh_redirect_write_report_error(write);
  write->target_index++;
  sh_redirect_write_next(write);
}

static void sh_redirect_write_close_done(uv_fs_t *request) {
  sh_redirect_write_t *write = request->data;
  int close_result = (int)request->result;
  uv_fs_req_cleanup(request);
  write->fd = -1;
  if (!write->failure && close_result < 0) write->failure = close_result;
  sh_redirect_write_advance(write);
}

static void sh_redirect_write_close(sh_redirect_write_t *write) {
  int rc = uv_fs_close(
    uv_default_loop(), &write->request, write->fd,
    sh_redirect_write_close_done
  );
  if (rc >= 0) return;

  uv_fs_req_cleanup(&write->request);
  uv_fs_t close_request;
  (void)uv_fs_close(NULL, &close_request, write->fd, NULL);
  uv_fs_req_cleanup(&close_request);
  write->fd = -1;
  if (!write->failure) write->failure = rc;
  sh_redirect_write_advance(write);
}

static void sh_redirect_write_done(uv_fs_t *request) {
  sh_redirect_write_t *write = request->data;
  int64_t written = request->result;
  uv_fs_req_cleanup(request);

  if (written <= 0) {
    write->failure = written < 0 ? (int)written : UV_EIO;
    sh_redirect_write_close(write);
    return;
  }

  write->written_total += (size_t)written;
  write->chunk_offset += (size_t)written;
  sh_redirect_write_next(write);
}

static void sh_redirect_write_next(sh_redirect_write_t *write) {
  if (write->target_index >= write->target_count) {
    sh_redirect_write_finish(write);
    return;
  }

  sh_redirect_target_t *target = &write->targets[write->target_index];
  if (write->fd < 0) {
    write->failure = 0;
    write->written_total = 0;
    write->chunk_offset = 0;
    write->chunk_len = 0;
    int flags = O_CREAT | O_WRONLY | (target->append ? O_APPEND : O_TRUNC);
    int rc = uv_fs_open(
      uv_default_loop(), &write->request, target->path, flags, 0666,
      sh_redirect_write_open_done
    );
    if (rc >= 0) return;
    uv_fs_req_cleanup(&write->request);
    write->failure = rc;
    sh_redirect_write_advance(write);
    return;
  }

  if (write->written_total >= target->text_len) {
    sh_redirect_write_close(write);
    return;
  }

  if (write->chunk_offset >= write->chunk_len) {
    size_t source_len = 0;
    char *source = js_getstr(write->js, target->text, &source_len);
    if (!source || source_len < target->text_len) {
      write->failure = UV_EIO;
      sh_redirect_write_close(write);
      return;
    }
    size_t remaining = target->text_len - write->written_total;
    write->chunk_len = remaining < SH_REDIRECT_WRITE_CHUNK_SIZE
      ? remaining : SH_REDIRECT_WRITE_CHUNK_SIZE;
    write->chunk_offset = 0;
    memcpy(write->chunk, source + write->written_total, write->chunk_len);
  }

  size_t remaining = write->chunk_len - write->chunk_offset;
  uv_buf_t buffer = uv_buf_init(
    write->chunk + write->chunk_offset, (unsigned int)remaining
  );
  int64_t offset = target->append ? -1 : (int64_t)write->written_total;
  int rc = uv_fs_write(
    uv_default_loop(), &write->request, write->fd, &buffer, 1, offset,
    sh_redirect_write_done
  );
  if (rc >= 0) return;
  uv_fs_req_cleanup(&write->request);
  write->failure = rc;
  sh_redirect_write_close(write);
}

static void sh_redirect_write_open_done(uv_fs_t *request) {
  sh_redirect_write_t *write = request->data;
  int open_result = (int)request->result;
  uv_fs_req_cleanup(request);
  if (open_result < 0) {
    write->failure = open_result;
    sh_redirect_write_advance(write);
    return;
  }
  write->fd = open_result;
  sh_redirect_write_next(write);
}

static ant_value_t sh_write_redirect_files(
  ant_t *js,
  ant_value_t result,
  sh_redirect_spec_t *specs,
  size_t spec_count
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  for (size_t i = 0; i < spec_count; i++) {
    GC_ROOT_PIN(js, specs[i].path);
    GC_ROOT_PIN(js, specs[i].text);
  }

  sh_redirect_write_t *write = calloc(1, sizeof(*write));
  if (!write) {
    ant_value_t error = js_mkerr(js, "Out of memory");
    GC_ROOT_RESTORE(js, root_mark);
    return error;
  }
  write->js = js;
  write->result = result;
  write->fd = -1;
  write->target_count = spec_count;
  write->targets = calloc(spec_count, sizeof(*write->targets));
  if (!write->targets) goto oom;

  size_t max_text_len = 0;
  for (size_t i = 0; i < spec_count; i++) {
    size_t path_len = 0;
    char *path = js_getstr(js, specs[i].path, &path_len);
    if (!path) goto oom;
    write->targets[i].path = malloc(path_len + 1);
    if (!write->targets[i].path) goto oom;
    memcpy(write->targets[i].path, path, path_len);
    write->targets[i].path[path_len] = '\0';
    write->targets[i].path_len = path_len;
    write->targets[i].text = specs[i].text;
    write->targets[i].text_len = specs[i].text_len;
    write->targets[i].append = specs[i].append;
    if (specs[i].text_len > max_text_len) max_text_len = specs[i].text_len;
  }

  if (max_text_len) {
    size_t chunk_size = max_text_len < SH_REDIRECT_WRITE_CHUNK_SIZE
      ? max_text_len : SH_REDIRECT_WRITE_CHUNK_SIZE;
    write->chunk = malloc(chunk_size);
    if (!write->chunk) goto oom;
  }

  write->promise = js_mkpromise(js);
  if (is_err(write->promise)) {
    ant_value_t error = write->promise;
    sh_redirect_write_dispose(write);
    GC_ROOT_RESTORE(js, root_mark);
    return error;
  }
  ant_value_t promise = write->promise;
  GC_ROOT_PIN(js, promise);

  ant_value_t roots = js_mkarr(js);
  if (is_err(roots)) {
    ant_value_t error = roots;
    sh_redirect_write_dispose(write);
    GC_ROOT_RESTORE(js, root_mark);
    return error;
  }
  GC_ROOT_PIN(js, roots);
  js_arr_push(js, roots, result);
  for (size_t i = 0; i < spec_count; i++) js_arr_push(js, roots, specs[i].text);
  js_set_slot_wb(js, promise, SLOT_DATA, roots);
  gc_root_pending_promise(js, js_obj_ptr(promise));
  write->request.data = write;
  sh_redirect_write_next(write);
  GC_ROOT_RESTORE(js, root_mark);
  return promise;

oom:
  sh_redirect_write_dispose(write);
  {
    ant_value_t error = js_mkerr(js, "Out of memory");
    GC_ROOT_RESTORE(js, root_mark);
    return error;
  }
}

static ant_value_t sh_apply_redirections_fulfilled(
  ant_t *js,
  ant_value_t *args,
  int nargs
) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  if (!is_special_object(result)) return result;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, state);
  GC_ROOT_PIN(js, result);

  ant_value_t stdout_value = js_get(js, result, "stdout");
  ant_value_t stderr_value = js_get(js, result, "stderr");
  GC_ROOT_PIN(js, stdout_value);
  GC_ROOT_PIN(js, stderr_value);
  size_t stdout_len = 0;
  size_t stderr_len = 0;
  char *stdout_text = vtype(stdout_value) == T_STR
    ? js_getstr(js, stdout_value, &stdout_len) : NULL;
  char *stderr_text = vtype(stderr_value) == T_STR
    ? js_getstr(js, stderr_value, &stderr_len) : NULL;

  ant_value_t output_path = js_get(js, state, "outputPath");
  ant_value_t redirections = js_get(js, state, "redirections");
  ant_value_t stderr_path = js_get(js, state, "stderrPath");
  GC_ROOT_PIN(js, output_path);
  GC_ROOT_PIN(js, redirections);
  GC_ROOT_PIN(js, stderr_path);
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

  size_t redirection_count = vtype(redirections) == T_ARR
    ? (size_t)js_arr_len(js, redirections) : 0;
  size_t output_count = 0;
  for (size_t i = 0; i < redirection_count; i++) {
    ant_value_t action = js_arr_get(js, redirections, (ant_offset_t)i);
    ant_value_t kind_value = js_get(js, action, "kind");
    if (vtype(kind_value) != T_NUM) continue;
    int kind = (int)js_getnum(kind_value);
    if (kind == SH_REDIR_STDOUT || kind == SH_REDIR_STDOUT_APPEND)
      output_count++;
  }
  
  size_t spec_capacity = output_count + (stderr_mode == 2 ? 1 : 0);
  sh_redirect_spec_t *specs = spec_capacity
    ? calloc(spec_capacity, sizeof(*specs)) : NULL;
  if (spec_capacity && !specs) {
    ant_value_t error = js_mkerr(js, "Out of memory");
    GC_ROOT_RESTORE(js, root_mark);
    return error;
  }
  
  size_t spec_count = 0;
  ant_value_t empty_text = js_mkstr(js, "", 0);
  GC_ROOT_PIN(js, empty_text);
  size_t output_index = 0;
  
  for (size_t i = 0; i < redirection_count; i++) {
    ant_value_t action = js_arr_get(js, redirections, (ant_offset_t)i);
    ant_value_t path = js_get(js, action, "path");
    ant_value_t kind_value = js_get(js, action, "kind");
    if (vtype(kind_value) != T_NUM) continue;
    int kind = (int)js_getnum(kind_value);
    if (kind != SH_REDIR_STDOUT && kind != SH_REDIR_STDOUT_APPEND) continue;
    bool final_output = ++output_index == output_count;
    specs[spec_count++] = (sh_redirect_spec_t){
      .path = path,
      .text = final_output ? stdout_value : empty_text,
      .text_len = final_output ? stdout_len : 0,
      .append = kind == SH_REDIR_STDOUT_APPEND,
    };
  }
  
  if (stderr_mode == 2 && vtype(stderr_path) == T_STR) {
    bool append = js_truthy(js, js_get(js, state, "stderrAppend"));
    if (sh_same_path(js, output_path, stderr_path)) append = true;
    specs[spec_count++] = (sh_redirect_spec_t){
      .path = stderr_path,
      .text = stderr_value,
      .text_len = stderr_len,
      .append = append,
    };
  }

  ant_value_t redirected = spec_count
    ? sh_write_redirect_files(js, result, specs, spec_count) : result;
  free(specs);
  GC_ROOT_RESTORE(js, root_mark);
  return redirected;
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
  if (nargs < 4 || !is_special_object(args[0]) || vtype(args[2]) != T_NUM ||
      vtype(args[3]) != T_ARR) {
    return sh_resolved_result(js, "", 0, "ant:shell: invalid compiled pipeline\n", sizeof("ant:shell: invalid compiled pipeline\n") - 1, 2);
  }

  ant_value_t context = args[0];
  sh_compiled_program_t *compiled = js_get_native(
    args[1], SH_COMPILED_PROGRAM_TAG
  );
  double clause_number = js_getnum(args[2]);
  if (!compiled || clause_number != clause_number || clause_number < 0 ||
      clause_number >= (double)compiled->program.clause_count) {
    return sh_resolved_result(js, "", 0, "ant:shell: invalid compiled pipeline\n", sizeof("ant:shell: invalid compiled pipeline\n") - 1, 2);
  }
  size_t clause_index = (size_t)clause_number;
  if (clause_number != (double)clause_index)
    return sh_resolved_result(js, "", 0, "ant:shell: invalid compiled pipeline\n", sizeof("ant:shell: invalid compiled pipeline\n") - 1, 2);
  const sh_pipeline_t *pipeline =
    &compiled->program.clauses[clause_index].pipeline;
  ant_value_t values = args[3];
  size_t command_count = pipeline->command_count;
  if (command_count == 0) return sh_resolved_result(js, "", 0, "", 0, 0);

  ant_value_t options = js_mkobj(js);
  ant_value_t cwd = js_get(js, context, "cwd");
  if (vtype(cwd) == T_STR) js_set(js, options, "cwd", cwd);

  // TODO: reduce nesting
  if (command_count > 1) {
    ant_value_t commands = js_mkarr(js);
    for (size_t i = 0; i < command_count; i++) {
      ant_value_t argv = js_mkundef();
      if (!sh_build_command_argv(
        js, &pipeline->commands[i], values, &argv
      )) return sh_abrupt_or_error(js, "ant:shell: invalid pipeline command");
      if (sh_argv_has_nul(js, argv))
        return sh_rejected_type_error(js, "ant:shell: command arguments cannot contain NUL bytes");
      const sh_command_t *command = &pipeline->commands[i];
      for (size_t j = 0; j < command->redir_count; j++) {
        sh_redir_kind_t kind = command->redirs[j].kind;
        bool allowed = (i == 0 && kind == SH_REDIR_STDIN) ||
          (i + 1 == command_count && kind != SH_REDIR_STDIN);
        if (!allowed) return sh_resolved_result(js, "", 0,
          "ant:shell: redirection on an intermediate pipeline stage is not implemented yet\n",
          sizeof("ant:shell: redirection on an intermediate pipeline stage is not implemented yet\n") - 1, 2);
      }
      ant_value_t stage_context = js_mkobj(js);
      js_set(js, stage_context, "cwd", js_get(js, context, "cwd"));
      ant_value_t builtin = sh_run_builtin_result(js, stage_context, argv);
      if (is_err(builtin))
        return sh_abrupt_or_error(js, "ant:shell: pipeline builtin failed");
      js_arr_push(js, commands, is_undefined(builtin) ? argv : builtin);
    }
    
    ant_value_t redir_state = js_mkundef();
    if (!sh_prepare_redirections(
      js, context, pipeline, values, options, &redir_state
    )) return js->thrown_exists ? sh_reject_current_exception(js)
      : js_mkerr(js, "ant:shell: invalid redirection");
    return child_process_pipeline_result(js, commands, options);
  }

  const sh_command_t *command = &pipeline->commands[0];
  ant_value_t argv = js_mkundef();
  
  if (!sh_build_command_argv(js, command, values, &argv))
    return sh_abrupt_or_error(js, "ant:shell: invalid command");
    
  if (sh_argv_has_nul(js, argv))
    return sh_rejected_type_error(js, "ant:shell: command arguments cannot contain NUL bytes");
    
  ant_value_t redir_state = js_mkundef();
  if (!sh_prepare_redirections(
    js, context, pipeline, values, options, &redir_state
  )) return js->thrown_exists ? sh_reject_current_exception(js)
    : js_mkerr(js, "ant:shell: invalid redirection");

  if (js_arr_len(js, argv) == 0) return sh_apply_redirections(
    js, sh_resolved_result(js, "", 0, "", 0, 0), redir_state
  );

  ant_value_t builtin = sh_run_builtin(js, context, argv);
  if (!is_undefined(builtin)) return sh_apply_redirections(js, builtin, redir_state);

  ant_value_t file = js_arr_get(js, argv, 0);
  ant_value_t child_argv = js_mkarr(js);
  for (ant_offset_t i = 1; i < js_arr_len(js, argv); i++)
    js_arr_push(js, child_argv, js_arr_get(js, argv, i));

  return child_process_exec_file_result(js, file, child_argv, options);
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
