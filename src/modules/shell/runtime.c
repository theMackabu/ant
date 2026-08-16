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
#include "modules/buffer.h"
#include "../process_plan.h"
#include "ptr.h"

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
} sh_bytes_t;

typedef struct {
  sh_bytes_t stdout_bytes;
  sh_bytes_t stderr_bytes;
} sh_output_accumulator_t;

enum { SH_OUTPUT_ACCUMULATOR_TAG = 0x53484143u }; // SHAC
enum { SH_PROCESS_BUILDER_TAG = 0x53485042u }; // SHPB

typedef struct {
  ant_process_plan_t plan;
  char **argv;
  size_t argc;
  size_t argv_capacity;
} sh_process_builder_t;

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

static ant_value_t sh_make_byte_array(
  ant_t *js, const char *data, size_t len
) {
  ArrayBufferData *buffer = create_array_buffer_data(len);
  if (!buffer) return js_mkerr(js, "Out of memory");
  if (len) memcpy(buffer->data, data, len);
  return create_typed_array(
    js, TYPED_ARRAY_UINT8, buffer, 0, len, "Uint8Array"
  );
}

static void sh_output_accumulator_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t context = js_obj_from_ptr(obj);
  sh_output_accumulator_t *accumulator = js_get_native(
    context, SH_OUTPUT_ACCUMULATOR_TAG
  );
  if (accumulator) {
    free(accumulator->stdout_bytes.data);
    free(accumulator->stderr_bytes.data);
    free(accumulator);
  }
  js_clear_native(context, SH_OUTPUT_ACCUMULATOR_TAG);
}

static ant_value_t sh_result(
  ant_t *js,
  const char *stdout_text,
  size_t stdout_len,
  const char *stderr_text,
  size_t stderr_len,
  int exit_code
) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t result = js_mkobj(js);
  
  GC_ROOT_PIN(js, result);
  ant_value_t stdout_value = sh_make_byte_array(
    js, stdout_text ? stdout_text : "", stdout_len
  );
  
  if (is_err(stdout_value)) {
    GC_ROOT_RESTORE(js, root_mark);
    return stdout_value;
  }
  
  GC_ROOT_PIN(js, stdout_value);
  ant_value_t stderr_value = sh_make_byte_array(
    js, stderr_text ? stderr_text : "", stderr_len
  );
  
  if (is_err(stderr_value)) {
    GC_ROOT_RESTORE(js, root_mark);
    return stderr_value;
  }
  
  js_set(js, result, "stdout", stdout_value);
  js_set(js, result, "stderr", stderr_value);
  js_set(js, result, "exitCode", js_mknum((double)exit_code));
  js_set(js, result, "signalCode", js_mknull());
  js_set(js, result, "exited", js_false);
  GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

static bool sh_value_append(ant_t *js, sh_bytes_t *bytes, ant_value_t value) {
  ant_value_t string = vtype(value) == T_STR ? value : js_tostring_val(js, value);
  if (is_err(string)) return false;
  size_t len = 0;
  char *text = js_getstr(js, string, &len);
  return text && sh_bytes_append(bytes, text, len);
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

static void sh_process_builder_clear_argv(sh_process_builder_t *builder) {
  if (!builder) return;
  for (size_t i = 0; i < builder->argc; i++) free(builder->argv[i]);
  free(builder->argv);
  builder->argv = NULL;
  builder->argc = 0;
  builder->argv_capacity = 0;
}

static void sh_process_builder_dispose(sh_process_builder_t *builder) {
  if (!builder) return;
  sh_process_builder_clear_argv(builder);
  ant_process_plan_dispose(&builder->plan);
  free(builder);
}

static void sh_process_builder_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t holder = js_obj_from_ptr(obj);
  sh_process_builder_t *builder = js_get_native(
    holder, SH_PROCESS_BUILDER_TAG
  );
  sh_process_builder_dispose(builder);
  js_clear_native(holder, SH_PROCESS_BUILDER_TAG);
}

static sh_process_builder_t *sh_process_builder_get(ant_value_t value) {
  return is_special_object(value)
    ? js_get_native(value, SH_PROCESS_BUILDER_TAG) : NULL;
}

static bool sh_process_builder_append(
  sh_process_builder_t *builder, const char *text, size_t len
) {
  if (!builder || (!text && len)) return false;
  if (text && memchr(text, '\0', len)) return false;
  if (builder->argc > SIZE_MAX - 2) return false;
  size_t needed = builder->argc + 2;
  if (needed > builder->argv_capacity) {
    size_t next = builder->argv_capacity ? builder->argv_capacity * 2 : 8;
    while (next < needed) {
      if (next > SIZE_MAX / 2) return false;
      next *= 2;
    }
    if (next > SIZE_MAX / sizeof(*builder->argv)) return false;
    char **grown = realloc(builder->argv, next * sizeof(*builder->argv));
    if (!grown) return false;
    builder->argv = grown;
    builder->argv_capacity = next;
  }
  if (len == SIZE_MAX) return false;
  char *copy = malloc(len + 1);
  if (!copy) return false;
  if (len) memcpy(copy, text, len);
  copy[len] = '\0';
  builder->argv[builder->argc++] = copy;
  builder->argv[builder->argc] = NULL;
  return true;
}

static bool sh_number_index(ant_value_t value, size_t *index) {
  if (vtype(value) != T_NUM) return false;
  double number = js_getnum(value);
  if (number != number || number < 0 || number > (double)SIZE_MAX) return false;
  size_t converted = (size_t)number;
  if ((double)converted != number) return false;
  *index = converted;
  return true;
}

static const sh_word_t *sh_compiled_word(
  ant_value_t holder, ant_value_t clause_value,
  ant_value_t command_value, ant_value_t word_value
) {
  sh_compiled_program_t *compiled = js_get_native(
    holder, SH_COMPILED_PROGRAM_TAG
  );
  size_t clause_index, command_index, word_index;
  if (!compiled || !sh_number_index(clause_value, &clause_index) ||
      !sh_number_index(command_value, &command_index) ||
      !sh_number_index(word_value, &word_index) ||
      clause_index >= compiled->program.clause_count) return NULL;
  const sh_pipeline_t *pipeline =
    &compiled->program.clauses[clause_index].pipeline;
  if (command_index >= pipeline->command_count) return NULL;
  const sh_command_t *command = &pipeline->commands[command_index];
  return word_index < command->word_count ? &command->words[word_index] : NULL;
}

static const sh_redir_t *sh_compiled_redirect(
  ant_value_t holder, ant_value_t clause_value,
  ant_value_t command_value, ant_value_t redirect_value
) {
  sh_compiled_program_t *compiled = js_get_native(
    holder, SH_COMPILED_PROGRAM_TAG
  );
  size_t clause_index, command_index, redirect_index;
  if (!compiled || !sh_number_index(clause_value, &clause_index) ||
      !sh_number_index(command_value, &command_index) ||
      !sh_number_index(redirect_value, &redirect_index) ||
      clause_index >= compiled->program.clause_count) return NULL;
  const sh_pipeline_t *pipeline =
    &compiled->program.clauses[clause_index].pipeline;
  if (command_index >= pipeline->command_count) return NULL;
  const sh_command_t *command = &pipeline->commands[command_index];
  return redirect_index < command->redir_count
    ? &command->redirs[redirect_index] : NULL;
}

static bool sh_process_builder_append_value(
  ant_t *js, sh_process_builder_t *builder, ant_value_t value
) {
  ant_value_t string = vtype(value) == T_STR
    ? value : js_tostring_val(js, value);
  if (is_err(string)) return false;
  size_t len = 0;
  const char *text = js_getstr(js, string, &len);
  if (text && memchr(text, '\0', len)) {
    js_mkerr_typed(
      js, JS_ERR_TYPE,
      "ant:shell: command arguments cannot contain NUL bytes"
    );
    return false;
  }
  return text && sh_process_builder_append(builder, text, len);
}

static bool sh_process_builder_expand_word(
  ant_t *js, sh_process_builder_t *builder,
  const sh_word_t *word, ant_value_t values
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
        for (ant_offset_t i = 0; i < count; i++)
          if (!sh_process_builder_append_value(
            js, builder, js_arr_get(js, value, i)
          )) return false;
        return true;
      }
    }
  }

  sh_bytes_t bytes = {0};
  for (size_t i = 0; i < word->part_count; i++) {
    const sh_word_part_t *part = &word->parts[i];
    if (part->kind == SH_PART_LITERAL) {
      if (!sh_bytes_append(
        &bytes, part->text ? part->text : "", part->text_len
      )) goto fail;
    } else if (part->kind == SH_PART_INTERPOLATION &&
               part->interpolation < (size_t)js_arr_len(js, values)) {
      if (!sh_value_append(js, &bytes, js_arr_get(
        js, values, (ant_offset_t)part->interpolation
      ))) goto fail;
    } else goto fail;
  }
  if (bytes.data && memchr(bytes.data, '\0', bytes.len)) {
    js_mkerr_typed(
      js, JS_ERR_TYPE,
      "ant:shell: command arguments cannot contain NUL bytes"
    );
    free(bytes.data);
    return false;
  }
  bool appended = sh_process_builder_append(
    builder, bytes.data ? bytes.data : "", bytes.len
  );
  free(bytes.data);
  return appended;

fail:
  free(bytes.data);
  return false;
}

static bool sh_expand_redirect_word(
  ant_t *js, const sh_word_t *word, ant_value_t values, sh_bytes_t *bytes
) {
  if (!word || vtype(values) != T_ARR) return false;
  for (size_t i = 0; i < word->part_count; i++) {
    const sh_word_part_t *part = &word->parts[i];
    if (part->kind == SH_PART_LITERAL) {
      if (!sh_bytes_append(
        bytes, part->text ? part->text : "", part->text_len
      )) return false;
    } else if (part->kind == SH_PART_INTERPOLATION &&
               part->interpolation < (size_t)js_arr_len(js, values)) {
      if (!sh_value_append(js, bytes, js_arr_get(
        js, values, (ant_offset_t)part->interpolation
      ))) return false;
    } else return false;
  }
  return true;
}

static char *sh_resolve_path_text(
  ant_t *js, ant_value_t context, const char *path, size_t path_len
) {
  if (!path || memchr(path, '\0', path_len)) {
    js_mkerr_typed(
      js, JS_ERR_TYPE,
      "ant:shell: redirection paths cannot contain NUL bytes"
    );
    return NULL;
  }
  if (sh_path_is_absolute(path, path_len)) {
    char *copy = malloc(path_len + 1);
    if (!copy) return NULL;
    memcpy(copy, path, path_len);
    copy[path_len] = '\0';
    return copy;
  }
  ant_value_t cwd_value = js_get(js, context, "cwd");
  size_t cwd_len = 0;
  const char *cwd = js_getstr(js, cwd_value, &cwd_len);
  if (!cwd || cwd_len > SIZE_MAX - path_len - 2) return NULL;
  char *joined = malloc(cwd_len + path_len + 2);
  if (!joined) return NULL;
  memcpy(joined, cwd, cwd_len);
  joined[cwd_len] = '/';
  memcpy(joined + cwd_len + 1, path, path_len);
  joined[cwd_len + path_len + 1] = '\0';
  return joined;
}

ant_value_t sh_runtime_begin(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_special_object(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell context");
  ant_value_t cwd_value = js_get(js, args[0], "cwd");
  size_t cwd_len = 0;
  const char *cwd = js_getstr(js, cwd_value, &cwd_len);
  if (!cwd || memchr(cwd, '\0', cwd_len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell cwd");

  sh_process_builder_t *builder = calloc(1, sizeof(*builder));
  if (!builder) return js_mkerr(js, "Out of memory");
  ant_process_plan_init(&builder->plan);
  builder->plan.result_mode = ANT_PROCESS_RESULT_BYTES;
  builder->plan.cwd = malloc(cwd_len + 1);
  if (!builder->plan.cwd) {
    sh_process_builder_dispose(builder);
    return js_mkerr(js, "Out of memory");
  }
  memcpy(builder->plan.cwd, cwd, cwd_len);
  builder->plan.cwd[cwd_len] = '\0';

  ant_value_t holder = js_mkobj(js);
  if (is_err(holder)) {
    sh_process_builder_dispose(builder);
    return holder;
  }
  js_set_native(holder, builder, SH_PROCESS_BUILDER_TAG);
  if (js_get_native(holder, SH_PROCESS_BUILDER_TAG) != builder) {
    sh_process_builder_dispose(builder);
    return js_mkerr(js, "Out of memory");
  }
  js_set_finalizer(holder, sh_process_builder_finalize);
  return holder;
}

ant_value_t sh_runtime_arg(ant_t *js, ant_value_t *args, int nargs) {
  sh_process_builder_t *builder = nargs > 0
    ? sh_process_builder_get(args[0]) : NULL;
  if (!builder || nargs < 2 || vtype(args[1]) != T_STR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid compiled shell argument");
  size_t len = 0;
  const char *text = js_getstr(js, args[1], &len);
  if (text && memchr(text, '\0', len)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "ant:shell: command arguments cannot contain NUL bytes"
  );
  if (!text || !sh_process_builder_append(builder, text, len))
    return js_mkerr(js, "Out of memory");
  return js_mkundef();
}

ant_value_t sh_runtime_word(ant_t *js, ant_value_t *args, int nargs) {
  sh_process_builder_t *builder = nargs > 0
    ? sh_process_builder_get(args[0]) : NULL;
  const sh_word_t *word = nargs >= 5
    ? sh_compiled_word(args[1], args[2], args[3], args[4]) : NULL;
  if (!builder || !word || nargs < 6 || vtype(args[5]) != T_ARR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid compiled shell word");
  if (!sh_process_builder_expand_word(js, builder, word, args[5])) {
    if (js->thrown_exists) return mkval(T_ERR, 0);
    return js_mkerr(js, "Out of memory");
  }
  return js_mkundef();
}

static bool sh_is_builtin_name(const char *name) {
  return name && (
    strcmp(name, ":") == 0 || strcmp(name, "true") == 0 ||
    strcmp(name, "false") == 0 || strcmp(name, "exit") == 0 ||
    strcmp(name, "echo") == 0 || strcmp(name, "pwd") == 0 ||
    strcmp(name, "cd") == 0
  );
}

static ant_value_t sh_process_builder_add_builtin(
  ant_t *js, sh_process_builder_t *builder,
  ant_value_t context, size_t command_count
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, context);
  ant_value_t stage_context = context;
  if (command_count > 1) {
    stage_context = js_mkobj(js);
    if (is_err(stage_context)) {
      GC_ROOT_RESTORE(js, root_mark);
      return stage_context;
    }
    GC_ROOT_PIN(js, stage_context);
    js_set(js, stage_context, "cwd", js_get(js, context, "cwd"));
  }

  ant_value_t argv = js_mkarr(js);
  if (is_err(argv)) {
    GC_ROOT_RESTORE(js, root_mark);
    return argv;
  }
  GC_ROOT_PIN(js, argv);
  for (size_t i = 0; i < builder->argc; i++) {
    ant_value_t value = js_mkstr(
      js, builder->argv[i], strlen(builder->argv[i])
    );
    if (is_err(value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return value;
    }
    js_arr_push(js, argv, value);
  }

  ant_value_t result = sh_run_builtin_result(js, stage_context, argv);
  if (is_err(result) || is_undefined(result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }
  GC_ROOT_PIN(js, result);
  ant_value_t stdout_value = js_get(js, result, "stdout");
  ant_value_t stderr_value = js_get(js, result, "stderr");
  GC_ROOT_PIN(js, stdout_value);
  GC_ROOT_PIN(js, stderr_value);
  const uint8_t *stdout_bytes = NULL;
  const uint8_t *stderr_bytes = NULL;
  size_t stdout_len = 0;
  size_t stderr_len = 0;
  ant_value_t exit_value = js_get(js, result, "exitCode");
  bool valid = buffer_source_get_bytes(
    js, stdout_value, &stdout_bytes, &stdout_len
  ) && buffer_source_get_bytes(
    js, stderr_value, &stderr_bytes, &stderr_len
  ) && vtype(exit_value) == T_NUM;
  if (!valid || !ant_process_plan_add_native_stage(
    &builder->plan, (const char *)stdout_bytes, stdout_len,
    (const char *)stderr_bytes, stderr_len, (int)js_getnum(exit_value)
  )) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "Out of memory");
  }
  if (command_count == 1 && js_truthy(js, js_get(js, result, "exited")))
    builder->plan.exited = true;
  sh_process_builder_clear_argv(builder);
  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

ant_value_t sh_runtime_command(ant_t *js, ant_value_t *args, int nargs) {
  sh_process_builder_t *builder = nargs > 0
    ? sh_process_builder_get(args[0]) : NULL;
  size_t command_count = 0;
  if (!builder || nargs < 3 || !is_special_object(args[1]) ||
      !sh_number_index(args[2], &command_count) || command_count == 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid compiled shell command");

  for (int i = 3; i < nargs; i++) {
    if (vtype(args[i]) != T_STR)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid compiled shell argument");
    size_t len = 0;
    const char *text = js_getstr(js, args[i], &len);
    if (text && memchr(text, '\0', len)) return js_mkerr_typed(
      js, JS_ERR_TYPE, "ant:shell: command arguments cannot contain NUL bytes"
    );
    if (!text || !sh_process_builder_append(builder, text, len))
      return js_mkerr(js, "Out of memory");
  }

  if (builder->argc == 0) {
    if (!ant_process_plan_add_native_stage(
      &builder->plan, "", 0, "", 0, 0
    )) return js_mkerr(js, "Out of memory");
    return js_mkundef();
  }
  if (builder->argv[0][0] == '\0') return js_mkerr_typed(
    js, JS_ERR_TYPE, "ant:shell: executable cannot be empty"
  );
  if (sh_is_builtin_name(builder->argv[0]))
    return sh_process_builder_add_builtin(
      js, builder, args[1], command_count
    );
  if (!ant_process_plan_take_command(
    &builder->plan, builder->argv, builder->argc
  )) return js_mkerr(js, "Out of memory");
  builder->argv = NULL;
  builder->argc = 0;
  builder->argv_capacity = 0;
  return js_mkundef();
}

static ant_value_t sh_process_builder_add_redirect(
  ant_t *js, sh_process_builder_t *builder, ant_value_t context,
  size_t kind, const char *target, size_t target_len,
  size_t command_index, size_t command_count
) {
  if (!builder || kind > SH_REDIR_STDERR_TO_STDOUT ||
      command_count == 0 || command_index >= command_count)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid compiled shell redirection");
  bool allowed = (command_index == 0 && kind == SH_REDIR_STDIN) ||
    (command_index + 1 == command_count && kind != SH_REDIR_STDIN);
  if (!allowed) return js_mkerr(
    js, "ant:shell: redirection on an intermediate pipeline stage is not implemented yet"
  );

  ant_process_redirect_kind_t process_kind =
    (ant_process_redirect_kind_t)kind;
  if (kind == SH_REDIR_STDERR_TO_STDOUT) {
    if (!ant_process_plan_add_redirect(&builder->plan, process_kind, NULL))
      return js_mkerr(js, "Out of memory");
    return js_mkundef();
  }
  char *path = sh_resolve_path_text(js, context, target, target_len);
  if (!path) return js->thrown_exists
    ? mkval(T_ERR, 0) : js_mkerr(js, "Out of memory");
  bool added = ant_process_plan_add_redirect(
    &builder->plan, process_kind, path
  );
  free(path);
  return added ? js_mkundef() : js_mkerr(js, "Out of memory");
}

ant_value_t sh_runtime_redirect(ant_t *js, ant_value_t *args, int nargs) {
  sh_process_builder_t *builder = nargs > 0
    ? sh_process_builder_get(args[0]) : NULL;
  size_t kind, command_index, command_count;
  if (!builder || nargs < 6 || !is_special_object(args[1]) ||
      !sh_number_index(args[2], &kind) ||
      !sh_number_index(args[4], &command_index) ||
      !sh_number_index(args[5], &command_count))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid compiled shell redirection");
  if (kind == SH_REDIR_STDERR_TO_STDOUT)
    return sh_process_builder_add_redirect(
      js, builder, args[1], kind, NULL, 0, command_index, command_count
    );
  if (vtype(args[3]) != T_STR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid compiled shell redirection");
  size_t target_len = 0;
  const char *target = js_getstr(js, args[3], &target_len);
  return sh_process_builder_add_redirect(
    js, builder, args[1], kind, target, target_len,
    command_index, command_count
  );
}

ant_value_t sh_runtime_redirect_word(
  ant_t *js, ant_value_t *args, int nargs
) {
  sh_process_builder_t *builder = nargs > 0
    ? sh_process_builder_get(args[0]) : NULL;
  const sh_redir_t *redirect = nargs >= 6
    ? sh_compiled_redirect(args[2], args[3], args[4], args[5]) : NULL;
  size_t command_index, command_count;
  if (!builder || !redirect || nargs < 8 || !is_special_object(args[1]) ||
      vtype(args[6]) != T_ARR ||
      !sh_number_index(args[4], &command_index) ||
      !sh_number_index(args[7], &command_count))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid compiled shell redirection");
  sh_bytes_t target = {0};
  if (!sh_expand_redirect_word(js, &redirect->target, args[6], &target)) {
    free(target.data);
    return js->thrown_exists ? mkval(T_ERR, 0) : js_mkerr(js, "Out of memory");
  }
  ant_value_t result = sh_process_builder_add_redirect(
    js, builder, args[1], (size_t)redirect->kind,
    target.data ? target.data : "", target.len,
    command_index, command_count
  );
  free(target.data);
  return result;
}

static ant_value_t sh_accumulate_fulfilled(
  ant_t *js, ant_value_t *args, int nargs
);

ant_value_t sh_runtime_submit(ant_t *js, ant_value_t *args, int nargs) {
  sh_process_builder_t *builder = nargs > 1
    ? sh_process_builder_get(args[1]) : NULL;
  if (!builder || nargs < 2 || !is_special_object(args[0]) || builder->argc)
    return ant_process_plan_rejected_result(
      js, js_mkerr(js, "Invalid compiled process plan")
    );
  ant_value_t result = ant_process_plan_submit(js, &builder->plan);
  if (vtype(result) != T_PROMISE) return result;
  sh_output_accumulator_t *accumulator = js_get_native(
    args[0], SH_OUTPUT_ACCUMULATOR_TAG
  );
  if (!accumulator) return result;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  GC_ROOT_PIN(js, args[0]);
  ant_value_t accumulated = js_promise_then(
    js, result, js_heavy_mkfun(js, sh_accumulate_fulfilled, args[0]),
    js_mkundef()
  );
  GC_ROOT_RESTORE(js, root_mark);
  return accumulated;
}

static ant_value_t sh_accumulate_fulfilled(
  ant_t *js, ant_value_t *args, int nargs
) {
  ant_value_t context = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  
  sh_output_accumulator_t *accumulator = js_get_native(context, SH_OUTPUT_ACCUMULATOR_TAG);
  if (!accumulator || !is_special_object(result))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell accumulator");

  const uint8_t *stdout_bytes = NULL;
  const uint8_t *stderr_bytes = NULL;
  
  size_t stdout_len = 0;
  size_t stderr_len = 0;
  
  if (!buffer_source_get_bytes(
      js, js_get(js, result, "stdout"), &stdout_bytes, &stdout_len
    ) || !buffer_source_get_bytes(
      js, js_get(js, result, "stderr"), &stderr_bytes, &stderr_len
    )) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell output");

  if (!sh_bytes_append(
      &accumulator->stdout_bytes, (const char *)stdout_bytes, stdout_len
    ) || !sh_bytes_append(
      &accumulator->stderr_bytes, (const char *)stderr_bytes, stderr_len
    )) return js_mkerr(js, "Out of memory");
  return result;
}

ant_value_t sh_runtime_finish(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_special_object(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell accumulator");
  sh_output_accumulator_t *accumulator = js_get_native(
    args[0], SH_OUTPUT_ACCUMULATOR_TAG
  );
  if (!accumulator)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid shell accumulator");

  ant_value_t last = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t exit_code_value = is_special_object(last)
    ? js_get(js, last, "exitCode") : js_mknum(0);
  int exit_code = vtype(exit_code_value) == T_NUM
    ? (int)js_getnum(exit_code_value) : 0;
  ant_value_t result = sh_result(
    js,
    accumulator->stdout_bytes.data ? accumulator->stdout_bytes.data : "",
    accumulator->stdout_bytes.len,
    accumulator->stderr_bytes.data ? accumulator->stderr_bytes.data : "",
    accumulator->stderr_bytes.len,
    exit_code
  );
  if (is_err(result)) return result;
  if (is_special_object(last))
    js_set(js, result, "signalCode", js_get(js, last, "signalCode"));
  return result;
}

ant_value_t sh_runtime_context(ant_t *js, bool needs_accumulator) {
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

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t context = js_mkobj(js);
  if (is_err(context)) {
    free(cwd);
    GC_ROOT_RESTORE(js, root_mark);
    return context;
  }
  GC_ROOT_PIN(js, context);
  if (needs_accumulator) {
    sh_output_accumulator_t *accumulator = calloc(1, sizeof(*accumulator));
    if (!accumulator) {
      free(cwd);
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "Out of memory");
    }
    js_set_native(context, accumulator, SH_OUTPUT_ACCUMULATOR_TAG);
    if (js_get_native(context, SH_OUTPUT_ACCUMULATOR_TAG) != accumulator) {
      free(accumulator);
      free(cwd);
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "Out of memory");
    }
    js_set_finalizer(context, sh_output_accumulator_finalize);
  }
  js_set(js, context, "cwd", js_mkstr(js, cwd, strlen(cwd)));
  free(cwd);
  GC_ROOT_RESTORE(js, root_mark);
  return context;
}
