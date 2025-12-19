#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>
#include <argtable3.h>

#include "ant.h"
#include "config.h"
#include "runtime.h"
#include "repl.h"

#ifndef ANT_SNAPSHOT_GENERATOR
#include "snapshot.h"
#endif

#include "modules/builtin.h"
#include "modules/buffer.h"
#include "modules/atomics.h"
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

int js_result = EXIT_SUCCESS;

static void eval_code(struct js *js, struct arg_str *eval, struct arg_lit *print) {
  const char *script = eval->sval[0];
  size_t len = strlen(script);
  
  js_set_filename(js, "[eval]");
  js_setup_import_meta(js, "[eval]");

  js_mkscope(js);
  js_protect_init_memory(js);
  
  js_set(js, js_glob(js), "__dirname", js_mkstr(js, ".", 1));
  js_set(js, js_glob(js), "__filename", js_mkstr(js, "[eval]", 6));
  
  jsval_t result = js_eval(js, script, len);
  
  if (js_type(result) == JS_ERR) {
    fprintf(stderr, "%s\n", js_str(js, result));
    js_result = EXIT_FAILURE;
  } else if (print->count > 0) {
    if (js_type(result) == JS_STR) {
      char *str = js_getstr(js, result, NULL);
      if (str) printf("%s\n", str);
    } else {
      const char *str = js_str(js, result);
      if (str && strcmp(str, "undefined") != 0) {
        print_value_colored(str, stdout);
        printf("\n");
      }
    }
  }
  
  js_run_event_loop(js);
}

static int execute_module(struct js *js, const char *filename) {
  char *filename_copy = strdup(filename);
  char *dir = dirname(filename_copy);
  
  js_set(js, js_glob(js), "__dirname", js_mkstr(js, dir, strlen(dir)));
  js_set(js, js_glob(js), "__filename", js_mkstr(js, filename, strlen(filename)));
  
  free(filename_copy);

  FILE *fp = fopen(filename, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Error: Could not open file '%s'\n", filename);
    return EXIT_FAILURE;
  }

  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  char *buffer = malloc(file_size + 1);
  if (buffer == NULL) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    fclose(fp);
    return EXIT_FAILURE;
  }

  size_t len = fread(buffer, 1, file_size, fp);
  fclose(fp);
  buffer[len] = '\0';
  
  char abs_path[4096];
  char *use_path = NULL;
  if (realpath(filename, abs_path) != NULL) {
    use_path = strdup(abs_path);
  } else use_path = strdup(filename);

  js_set_filename(js, use_path);
  js_setup_import_meta(js, use_path);
  
  js_mkscope(js);
  js_protect_init_memory(js);
  
  jsval_t result = js_eval(js, buffer, len);
  free(buffer);
  
  if (js_type(result) == JS_ERR) {
    fprintf(stderr, "%s\n", js_str(js, result));
    free(use_path);
    return EXIT_FAILURE;
  }
  
  free(use_path);
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  char dump = 0;
  
  struct arg_lit *help = arg_lit0("h", "help", "display this help and exit");
  struct arg_lit *version = arg_lit0("v", "version", "display version information and exit");
  struct arg_lit *debug = arg_litn("d", "debug", 0, 10, "dump VM state (can be repeated for more detail)");
  struct arg_str *eval = arg_str0("e", "eval", "<script>", "evaluate script");
  struct arg_lit *print = arg_lit0("p", "print", "evaluate script and print result");
  struct arg_int *gct = arg_int0(NULL, "gct", "<threshold>", "set garbage collection threshold");
  struct arg_int *initial_mem = arg_int0(NULL, "initial-mem", "<size>", "initial memory size in MB (default: 4)");
  struct arg_int *max_mem = arg_int0(NULL, "max-mem", "<size>", "maximum memory size in MB (default: 512)");
  struct arg_file *file = arg_file0(NULL, NULL, "<module.js>", "JavaScript module file to execute");
  struct arg_end *end = arg_end(20);
  
  void *argtable[] = {help, version, debug, eval, print, gct, initial_mem, max_mem, file, end};
  int nerrors = arg_parse(argc, argv, argtable);
  
  if (help->count > 0) {
    printf("Ant sized JavaScript\n\n");
    printf("Usage: ant [options] [module.js]\n\n");
    printf("If no module file is specified, ant starts in REPL mode.\n\n");
    printf("Options:\n");
    arg_print_glossary(stdout, argtable, "  %-25s %s\n");
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return EXIT_SUCCESS;
  }
  
  if (version->count > 0) {
    printf("ant %s (%s %s) [release]\n", ANT_VERSION, ANT_BUILD_DATE, ANT_GIT_HASH);
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return EXIT_SUCCESS;
  }
  
  if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant");
    printf("Try 'ant --help' for more information.\n");
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return EXIT_FAILURE;
  }
  
  bool repl_mode = (file->count == 0 && eval->count == 0);
  const char *module_file = repl_mode ? NULL : (file->count > 0 ? file->filename[0] : NULL);
  dump = debug->count;
  
  size_t initial_size = 4 * 1024 * 1024;
  size_t max_size = 512 * 1024 * 1024;
  
  if (initial_mem->count > 0) initial_size = (size_t)initial_mem->ival[0] * 1024 * 1024;
  if (max_mem->count > 0) max_size = (size_t)max_mem->ival[0] * 1024 * 1024;
  
  struct js *js = js_create_dynamic(initial_size, max_size);
  
  if (js == NULL) {
    fprintf(stderr, "Error: Failed to allocate JavaScript runtime\n");
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return EXIT_FAILURE;
  }
  
  if (gct->count > 0) js_setgct(js, gct->ival[0]);  
  ant_runtime_init(js, argc, argv);

  init_builtin_module();
  init_buffer_module();
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
  
  ant_register_library(shell_library, "ant:shell", NULL);
  ant_register_library(ffi_library, "ant:ffi", NULL);

  ant_register_library(path_library, "ant:path", "node:path", NULL);
  ant_register_library(fs_library, "ant:fs", "node:fs", NULL);
  ant_register_library(crypto_library, "ant:crypto", "node:crypto", NULL);
  ant_register_library(events_library, "ant:events", "node:events", NULL);
  
  #ifndef ANT_SNAPSHOT_GENERATOR
    jsval_t snapshot_result = ant_load_snapshot(js);
    if (js_type(snapshot_result) == JS_ERR) {
      fprintf(stderr, "Warning: Failed to load snapshot: %s\n", js_str(js, snapshot_result));
    }
  #endif

  if (eval->count > 0) eval_code(js, eval, print);
  else if (repl_mode) ant_repl_run(); else {
    js_result = execute_module(js, module_file);
    js_run_event_loop(js);
  }
  
  if (dump) js_dump(js);
  
  js_destroy(js);
  arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
  
  return js_result;
}