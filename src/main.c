#include <compat.h> // IWYU pragma: keep

#include <oxc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <argtable3.h>

#include "ant.h"
#include "repl.h"
#include "utils.h"
#include "runtime.h"
#include "snapshot.h"
#include "esm/remote.h"

#include "modules/builtin.h"
#include "modules/buffer.h"
#include "modules/atomics.h"
#include "modules/os.h"
#include "modules/io.h"
#include "modules/fs.h"
#include "modules/crypto.h"
#include "modules/server.h"
#include "modules/timer.h"
#include "modules/json.h"
#include "modules/fetch.h"
#include "modules/shell.h"
#include "modules/process.h"
#include "modules/path.h"
#include "modules/ffi.h"
#include "modules/events.h"
#include "modules/performance.h"
#include "modules/uri.h"
#include "modules/url.h"
#include "modules/reflect.h"
#include "modules/symbol.h"
#include "modules/textcodec.h"
#include "modules/sessionstorage.h"
#include "modules/localstorage.h"
#include "modules/navigator.h"
#include "modules/child_process.h"
#include "modules/readline.h"
#include "modules/observable.h"

int js_result = EXIT_SUCCESS;

static void eval_code(struct js *js, struct arg_str *eval, struct arg_lit *print) {
  const char *script = eval->sval[0];
  size_t len = strlen(script);
  
  js_set_filename(js, "[eval]");
  js_setup_import_meta(js, "[eval]");
  js_mkscope(js);
  
  js_set(js, js_glob(js), "__dirname", js_mkstr(js, ".", 1));
  js_set(js, js_glob(js), "__filename", js_mkstr(js, "[eval]", 6));
  
  jsval_t result = js_eval(js, script, len);
  js_run_event_loop(js);
  
  if (js_type(result) == JS_ERR) {
    fprintf(stderr, "%s\n", js_str(js, result));
    js_result = EXIT_FAILURE;
  } else if (print->count > 0) {
    if (js_type(result) == JS_STR) {
      char *str = js_getstr(js, result, NULL);
      if (str) printf("%s\n", str);
    } else {
      const char *str = js_str(js, result);
      if (str && strcmp(str, "undefined") != 0) { print_value_colored(str, stdout); printf("\n"); }
    }
  }
}

static char *read_file(const char *filename, size_t *len) {
  FILE *fp = fopen(filename, "rb");
  if (!fp) return NULL;
  
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  
  char *buffer = malloc(size + 1);
  if (!buffer) {
    fclose(fp);
    return NULL;
  }
  
  *len = fread(buffer, 1, size, fp);
  fclose(fp);
  buffer[*len] = '\0';
  
  return buffer;
}

static int execute_module(struct js *js, const char *filename) {
  char *buffer = NULL;
  size_t len = 0;
  
  char abs_path[PATH_MAX];
  const char *use_path = filename;
  
  if (esm_is_url(filename)) {
    char *error = NULL;
    buffer = esm_fetch_url(filename, &len, &error);
    
    if (!buffer) {
      fprintf(stderr, "Error: Could not fetch '%s': %s\n", filename, error ? error : "unknown error");
      free(error);
      return EXIT_FAILURE;
    }
    
    js_set(js, js_glob(js), "__dirname", js_mkundef());
  } else {
    buffer = read_file(filename, &len);
    if (!buffer) {
      fprintf(stderr, "Error: Could not open file '%s'\n", filename);
      return EXIT_FAILURE;
    }
    
    char *file_path = strdup(filename);
    char *dir = dirname(file_path);
    js_set(js, js_glob(js), "__dirname", js_mkstr(js, dir, strlen(dir)));
    free(file_path);
    
    if (realpath(filename, abs_path)) use_path = abs_path;
  }
  
  char *js_code = buffer;
  size_t js_len = len;
  
  if (is_typescript_file(filename)) {
    int result = OXC_strip_types(buffer, filename, buffer, len + 1);
    if (result < 0) {
      fprintf(stderr, "TypeScript error: strip failed (%d)\n", result);
      free(buffer);
      return EXIT_FAILURE;
    }
    js_len = (size_t)result;
  }

  js_set_filename(js, use_path);
  js_set(js, js_glob(js), "__filename", js_mkstr(js, filename, strlen(filename)));
  
  js_setup_import_meta(js, use_path);
  js_mkscope(js);
  
  jsval_t result = js_eval(js, js_code, js_len);
  free(js_code);
  
  if (js_type(result) == JS_ERR) {
    fprintf(stderr, "%s\n", js_str(js, result));
    return EXIT_FAILURE;
  }
  
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {  
  struct arg_lit *help = arg_lit0("h", "help", "display this help and exit");
  struct arg_lit *version = arg_lit0("v", "version", "display version information and exit");
  struct arg_lit *no_color = arg_lit0(NULL, "no-color", "disable colored output");
  struct arg_str *eval = arg_str0("e", "eval", "<script>", "evaluate script");
  struct arg_lit *print = arg_lit0("p", "print", "evaluate script and print result");
  struct arg_int *initial_mem = arg_int0(NULL, "initial-mem", "<size>", "initial memory size in KB (default: 16kb)");
  struct arg_int *max_mem = arg_int0(NULL, "max-mem", "<size>", "maximum memory size in MB (default: 1024mb)");
  struct arg_file *localstorage_file = arg_file0(NULL, "localstorage-file", "<path>", "file path for localStorage persistence");
  struct arg_file *file = arg_file0(NULL, NULL, "<module.js>", "JavaScript module file to execute");
  struct arg_end *end = arg_end(20);
  
  void *argtable[] = {
    help, version, no_color, eval,
    print, initial_mem, max_mem,
    localstorage_file, file, end
  };
  
  int nerrors = arg_parse(argc, argv, argtable);
  
  if (help->count > 0) {
    printf("Ant sized JavaScript\n\n");
    printf("Usage: ant [options] [module.js]\n\n");
    printf("If no module file is specified, ant starts in REPL mode.\n\n");
    printf("Options:\n");
    arg_print_glossary(stdout, argtable, "  %-25s %s\n");
    arg_freetable(argtable, ARGTABLE_COUNT);
    return EXIT_SUCCESS;
  }
  
  if (version->count > 0) return ant_version(argtable);
  
  if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant");
    printf("Try 'ant --help' for more information.\n");
    arg_freetable(argtable, ARGTABLE_COUNT);
    return EXIT_FAILURE;
  }
  
  bool repl_mode = (file->count == 0 && eval->count == 0);
  const char *module_file = repl_mode ? NULL : (file->count > 0 ? file->filename[0] : NULL);
  
  if (no_color->count > 0) io_no_color = true;
  
  struct js *js = js_create_dynamic(
    initial_mem->count > 0 ? (size_t)initial_mem->ival[0] * 1024 : 0,
    max_mem->count > 0 ? (size_t)max_mem->ival[0] * 1024 * 1024 : 0
  );
  
  if (js == NULL) {
    fprintf(stderr, "Error: Failed to allocate JavaScript runtime\n");
    arg_freetable(argtable, ARGTABLE_COUNT);
    return EXIT_FAILURE;
  }
  
  ant_runtime_init(js, argc, argv, localstorage_file);

  init_symbol_module();
  init_builtin_module();
  init_buffer_module();
  init_fs_module();
  init_atomics_module();
  init_crypto_module();
  init_fetch_module();
  init_console_module();
  init_json_module();
  init_server_module();
  init_timer_module();
  init_process_module();
  init_events_module();
  init_performance_module();
  init_uri_module();
  init_url_module();
  init_reflect_module();
  init_textcodec_module();
  init_sessionstorage_module();
  init_localstorage_module();
  init_navigator_module();
  init_observable_module();
  
  ant_register_library(shell_library, "ant:shell", NULL);
  ant_register_library(ffi_library, "ant:ffi", NULL);

  ant_standard_library("path", path_library);
  ant_standard_library("fs", fs_library);
  ant_standard_library("os", os_library);
  ant_standard_library("crypto", crypto_library);
  ant_standard_library("events", events_library);
  ant_standard_library("readline", readline_library);
  ant_standard_library("readline/promises", readline_promises_library);
  ant_standard_library("child_process", child_process_library);

  jsval_t snapshot_result = ant_load_snapshot(js);
  if (js_type(snapshot_result) == JS_ERR) {
    fprintf(stderr, "Warning: Failed to load snapshot: %s\n", js_str(js, snapshot_result));
  }

  if (eval->count > 0) eval_code(js, eval, print);
  else if (repl_mode) ant_repl_run(); else {
    struct stat path_stat;
    char *resolved_file = NULL;
    
    if (stat(module_file, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
      size_t len = strlen(module_file);
      int has_slash = (len > 0 && module_file[len - 1] == '/');
      resolved_file = malloc(len + 10 + (has_slash ? 0 : 1));
      sprintf(resolved_file, "%s%sindex.js", module_file, has_slash ? "" : "/");
      module_file = resolved_file;
    }
    
    js_result = execute_module(js, module_file);
    js_run_event_loop(js);
    
    if (resolved_file) free(resolved_file);
  }
    
  js_destroy(js);
  arg_freetable(argtable, ARGTABLE_COUNT);
  
  return js_result;
}