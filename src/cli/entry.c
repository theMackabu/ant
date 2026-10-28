#include <compat.h> // IWYU pragma: keep

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crprintf.h>

#include "ant.h"
#include "utils.h"
#include "utf8.h"
#include "reactor.h"
#include "internal.h"
#include "messages.h"
#include "inspector.h"
#include "watch.h"
#include "esm/commonjs.h"
#include "esm/loader.h"
#include "esm/remote.h"
#include "modules/io.h"
#include "modules/cron.h"
#include "modules/server.h"
#include "modules/process.h"
#include "cli/entry.h"

ant_argv_t ant_entry_process_argv(const char *argv0, const char *first, bool resolve, ant_argv_t tail) {
  int lead = first ? 2 : 1;
  int total = lead + tail.argc;
  
  char **out = try_oom(sizeof(char *) * (size_t)(total + 1));
  char *resolved = first && resolve && !esm_is_url(first) ? realpath(first, NULL) : NULL;

  out[0] = (char *)argv0;
  if (first) out[1] = resolved ? resolved : (char *)first;
  for (int i = 0; i < tail.argc; i++) out[lead + i] = tail.argv[i];
  out[total] = NULL;

  return (ant_argv_t){ total, out };
}

static ant_value_t set_commonjs_globals(ant_t *js, const char *tag) {
  char cwd[PATH_MAX];
  char path[PATH_MAX + 16];
  
  if (getcwd(cwd, sizeof(cwd))) snprintf(path, sizeof(path), "%s/%s", cwd, tag);
  else snprintf(path, sizeof(path), "%s", tag);

  GC_ROOT_SAVE(mark, js);
  ant_value_t module = esm_create_cjs_module(js, path, js_mkundef());
  GC_ROOT_PIN(js, module);

  ant_value_t result = module;
  if (!is_err(module)) {
    js_set(js, module, "id", js_mkstr(js, tag, strlen(tag)));
    ant_value_t require = esm_create_require(js, module);
    
    GC_ROOT_PIN(js, require);
    js_set_global_builtin(js, "module", module);
    js_set_global_builtin(js, "exports", js_get(js, module, "exports"));
    js_set_global_builtin(js, "require", require);
  }

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t eval_source(ant_t *js, const char *source, size_t len, const char *tag, ant_input_mode_t mode) {
  if (mode == ANT_INPUT_MODULE) {
    ant_value_t ns = Ant_Object_CreateBare(js);
    return is_err(ns) ? ns : js_esm_eval_module_source(js, tag, source, len, ns);
  }

  ant_value_t globals = set_commonjs_globals(js, tag);
  if (is_err(globals)) return globals;

  ant_value_t script = js_vm_script_compile(js, source, len);
  if (is_err(script)) return script;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, script);
  
  ant_value_t result = js_vm_script_run(js, script);
  GC_ROOT_RESTORE(js, mark);
  
  return result;
}

static void print_result(const char *text, ant_value_t result) {
  const char *shown = text ? text : "undefined";
  if (vtype(result) == kTypeString || !io_should_color_fd(STDOUT_FILENO)) printf("%s\n", shown);
  else {
    print_value_colored(shown, stdout);
    printf("\n");
  }
}

int ant_entry_eval(
  ant_t *js, const char *source, size_t len,
  const char *tag, bool print, ant_input_mode_t mode
) {
  js_set_filename(js, tag);
  js_setup_import_meta(js, tag);

  js_set_global_builtin(js, "__dirname", js_mkstr(js, ".", 1));
  js_set_global_builtin(js, "__filename", js_mkstr(js, tag, strlen(tag)));

  ant_value_t result = eval_source(js, source, len, tag, mode);

  if (Ant_Exception_Pending(js)) {
    if (!process_has_event_listeners(js, "uncaughtException")) {
      print_uncaught_throw(js);
      return EXIT_FAILURE;
    }
    process_report_uncaught_exception(js);
    result = js_mkundef();
  }

  js_run_event_loop(js);
  if (print_uncaught_throw(js)) return EXIT_FAILURE;

  char stack_buf[512];
  js_cstr_t text = js_to_cstr(js, result, stack_buf, sizeof(stack_buf));
  int code = EXIT_SUCCESS;

  if (vtype(result) == kTypeError) {
    fprintf(stderr, "%s\n", text.ptr);
    code = EXIT_FAILURE;
  } else if (print) print_result(text.ptr, result);

  if (text.needs_free) free((void *)text.ptr);
  return code;
}

static char *read_stdin(size_t *len) {
  size_t cap = 4096;
  *len = 0;
  
  char *buf = malloc(cap);
  if (!buf) return NULL;

  size_t n;
  while ((n = fread(buf + *len, 1, cap - *len, stdin)) > 0) {
    *len += n;
    if (*len < cap) continue;
    cap *= 2;
    char *next = realloc(buf, cap);
    if (!next) {
      free(buf);
      return NULL;
    }
    buf = next;
  }

  buf[*len] = '\0';
  
  size_t valid = utf8_valid_prefix(buf, *len, NULL);
  if (valid == *len) return buf;
  
  char *clean = utf8_replace_invalid(buf, *len, valid, len);
  free(buf);
  
  return clean;
}

int ant_entry_stdin(ant_t *js, bool print, ant_input_mode_t mode, bool inspect) {
  size_t len = 0;
  char *source = read_stdin(&len);
  
  if (!source) {
    crfprintf(stderr, msg.ant_allocation_fatal);
    return EXIT_FAILURE;
  }

  if (inspect) ant_inspector_register_script_source("[stdin]", source, len, false);
  int code = ant_entry_eval(js, source, len, "[stdin]", print, mode);
  free(source);
  
  return code;
}

static void set_module_globals(ant_t *js, const char *filename) {
  if (esm_is_url(filename)) js_set_global_builtin(js, "__dirname", js_mkundef());
  else {
    char *path = strdup(filename);
    char *dir = dirname(path);
    js_set_global_builtin(js, "__dirname", js_mkstr(js, dir, strlen(dir)));
    free(path);
  }
  js_set_global_builtin(js, "__filename", js_mkstr(js, filename, strlen(filename)));
}

int ant_entry_module(ant_t *js, const char *filename, const char *cron_period) {
  set_module_globals(js, filename);

  char *resolved = esm_is_url(filename) ? NULL : realpath(filename, NULL);
  const char *path = resolved ? resolved : filename;
  
  ant_value_t specifier = esm_is_url(filename)
    ? js_mkstr(js, filename, strlen(filename))
    : js_esm_make_file_url(js, path);

  const char *interned = intern_string(js, path, strlen(path));
  const char *stable = interned ? interned : path;

  js_set_filename(js, stable);
  js_setup_import_meta(js, stable);
  
  ant_value_t ns = js_esm_import_sync(js, specifier);
  free(resolved);

  if (Ant_Exception_Pending(js) && process_has_event_listeners(js, "uncaughtException")) {
    process_report_uncaught_exception(js);
    return EXIT_SUCCESS;
  }

  if (print_uncaught_throw(js)) return EXIT_FAILURE;

  if (vtype(ns) == kTypeError) {
    fprintf(stderr, "%s\n", js_str(js, ns));
    return EXIT_FAILURE;
  }

  ant_value_t default_export = js_get(js, ns, "default");
  if (cron_period) return cron_run_scheduled_export(js, default_export, cron_period);
  
  return server_maybe_start_from_export(js, default_export);
}

int ant_entry_files(ant_t *js, const char **files, int count, const char *cron_period) {
  int code = EXIT_SUCCESS;

  for (int i = 0; i < count && code == EXIT_SUCCESS; i++) {
    char *resolved = resolve_js_file(files[i]);
    if (!resolved) {
      crfprintf(stderr, msg.module_not_found, files[i]);
      return EXIT_FAILURE;
    }

    code = ant_entry_module(js, resolved, cron_period);
    if (code == EXIT_SUCCESS) js_run_event_loop(js);
    free(resolved);
  }

  return code;
}

int ant_entry_files_hot(ant_t *js, ant_watch_t *watch, const char **files, int count) {
  do {
    for (int i = 0; i < count && !js->loop_yield && !js->terminating; i++) {
      char *resolved = resolve_js_file(files[i]);
      if (!resolved) crfprintf(stderr, msg.module_not_found, files[i]);
      else ant_entry_module(js, resolved, NULL);
      free(resolved);
    }

    if (!js->loop_yield && !js->terminating) js_run_event_loop(js);
    js->loop_yield = false;
  } while (!js->terminating && ant_watch_begin_reload(watch, js));

  return ant_watch_shutdown_code(watch);
}
