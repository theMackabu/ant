#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>

#include "ant.h"
#include "config.h"
#include "runtime.h"
#include "argtable3.h"

#include "modules/io.h"
#include "modules/crypto.h"
#include "modules/server.h"
#include "modules/timer.h"
#include "modules/json.h"
#include "modules/fetch.h"

static struct {
  char *path;
  jsval_t exports;
} *module_cache = NULL;

static int module_count = 0;
static int module_capacity = 0;

static struct {
  struct js *js;
  jsval_t handler;
} signal_handlers[32] = {0};

static jsval_t js_gc_trigger(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  size_t before_brk = js_getbrk(js);
  
  js_gc(js);
  size_t after_brk = js_getbrk(js);
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "before", js_mknum((double)before_brk));
  js_set(js, result, "after", js_mknum((double)after_brk));
  
  return result;
}

static jsval_t js_alloc(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  
  size_t total = 0, min_free = 0, cstack = 0;
  js_stats(js, &total, &min_free, &cstack);
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "used", js_mknum((double)total));
  js_set(js, result, "minFree", js_mknum((double)min_free));
  
  return result;
}

static jsval_t js_stats_fn(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  
  size_t total = 0, min_free = 0, cstack = 0;
  js_stats(js, &total, &min_free, &cstack);
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "used", js_mknum((double)total));
  js_set(js, result, "minFree", js_mknum((double)min_free));
  js_set(js, result, "cstack", js_mknum((double)cstack));
  
  return result;
}

static void general_signal_handler(int signum) {
  if (signum >= 0 && signum < 32 && signal_handlers[signum].js != NULL) {
    struct js *js = signal_handlers[signum].js;
    jsval_t handler = signal_handlers[signum].handler;
    
    if (js_type(handler) != JS_UNDEF) {
      jsval_t sig_num = js_mknum(signum);
      jsval_t args[1] = {sig_num};
      js_call(js, handler, args, 1);
    }
  }
  
  exit(0);
}

static jsval_t js_signal(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    fprintf(stderr, "Error: Ant.signal() requires 2 arguments (signal, handler)\n");
    return js_mkundef();
  }
  
  char *signal_name = js_getstr(js, args[0], NULL);
  if (signal_name == NULL) {
    return js_mkerr(js, "signal name must be a string");
  }
  
  int signum = -1;
  if (strcmp(signal_name, "SIGINT") == 0 || strcmp(signal_name, "sigint") == 0) {
    signum = SIGINT;
  } else if (strcmp(signal_name, "SIGTERM") == 0 || strcmp(signal_name, "sigterm") == 0) {
    signum = SIGTERM;
  } else if (strcmp(signal_name, "SIGHUP") == 0 || strcmp(signal_name, "sighup") == 0) {
    signum = SIGHUP;
  } else if (strcmp(signal_name, "SIGUSR1") == 0 || strcmp(signal_name, "sigusr1") == 0) {
    signum = SIGUSR1;
  } else if (strcmp(signal_name, "SIGUSR2") == 0 || strcmp(signal_name, "sigusr2") == 0) {
    signum = SIGUSR2;
  } else {
    return js_mkerr(js, "unsupported signal: %s", signal_name);
  }
  
  signal_handlers[signum].js = js;
  signal_handlers[signum].handler = args[1];
  signal(signum, general_signal_handler);
  
  return js_mkundef();
}

static jsval_t js_require(struct js *js, jsval_t *args, int nargs) {
  if (nargs != 1) return js_mkundef();
  
  char data[8192];
  char *req_path = js_getstr(js, args[0], NULL);
  char full_path[PATH_MAX];
  
  jsval_t ant_obj = js_get(js, js_glob(js), "Ant");
  jsval_t dirname_val = js_get(js, ant_obj, "__dirname");
  char *base_path = js_getstr(js, dirname_val, NULL);
  
  if (base_path == NULL) base_path = ".";
  
  if (req_path[0] == '.') {
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, req_path);
  } else {
    snprintf(full_path, sizeof(full_path), "%s", req_path);
  }
  
  FILE *fp = fopen(full_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Error: Could not open required file '%s'\n", full_path);
    return js_mkundef();
  }
  
  size_t len = fread(data, 1, sizeof(data), fp);
  fclose(fp);
  
  for (int i = 0; i < module_count; i++) {
    if (strcmp(module_cache[i].path, full_path) == 0) return module_cache[i].exports;
  }
  
  jsval_t module_exports = js_mkobj(js);
  jsval_t prev_exports = js_get(js, ant_obj, "exports");
  
  js_set(js, ant_obj, "exports", module_exports);
  js_mkscope(js);
  js_set_filename(js, full_path);
  
  jsval_t result = js_eval(js, data, len);
  js_delscope(js);
  
  if (js_type(result) == JS_ERR) {
    fprintf(stderr, "%s\n", js_str(js, result));
    return js_mkundef();
  }
  
  jsval_t final_exports = js_get(js, ant_obj, "exports");
  js_set(js, ant_obj, "exports", prev_exports);
  
  if (module_count >= module_capacity) {
    module_capacity = module_capacity == 0 ? 8 : module_capacity * 2;
    module_cache = realloc(module_cache, module_capacity * sizeof(*module_cache));
    if (module_cache == NULL) {
      fprintf(stderr, "Error: Failed to allocate module cache\n");
      return final_exports;
    }
  }
  
  module_cache[module_count].path = strdup(full_path);
  module_cache[module_count].exports = final_exports;
  module_count++;
  
  return final_exports;
}

static int execute_module(struct js *js, const char *filename) {
  char *filename_copy = strdup(filename);
  char *dir = dirname(filename_copy);
  
  jsval_t ant_obj = js_get(js, js_glob(js), "Ant");
  js_set(js, ant_obj, "__dirname", js_mkstr(js, dir, strlen(dir)));
  
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
  js_set_filename(js, filename);
  
  jsval_t result = js_eval(js, buffer, len);
  free(buffer);
  
  if (js_type(result) == JS_ERR) {
    fprintf(stderr, "%s\n", js_str(js, result));
    return EXIT_FAILURE;
  }
  
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  char dump = 0;
  
  struct arg_lit *help = arg_lit0("h", "help", "display this help and exit");
  struct arg_lit *version = arg_lit0("v", "version", "display version information and exit");
  struct arg_lit *debug = arg_litn("d", "debug", 0, 10, "dump VM state (can be repeated for more detail)");
  struct arg_int *gct = arg_int0(NULL, "gct", "<threshold>", "set garbage collection threshold");
  struct arg_int *initial_mem = arg_int0(NULL, "initial-mem", "<size>", "initial memory size in MB (default: 4)");
  struct arg_int *max_mem = arg_int0(NULL, "max-mem", "<size>", "maximum memory size in MB (default: 512)");
  struct arg_file *file = arg_file0(NULL, NULL, "<module.js>", "JavaScript module file to execute");
  struct arg_end *end = arg_end(20);
  
  void *argtable[] = {help, version, debug, gct, initial_mem, max_mem, file, end};
  int nerrors = arg_parse(argc, argv, argtable);
  
  if (help->count > 0) {
    printf("Ant sized JavaScript\n\n");
    printf("Usage: ant");
    arg_print_syntax(stdout, argtable, "\n\n");
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
  
  if (file->count == 0) {
    fprintf(stderr, "Error: No input file specified\n");
    printf("Try 'ant --help' for more information.\n");
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return EXIT_FAILURE;
  }
  
  const char *module_file = file->filename[0];
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
  
  if (gct->count > 0) {
    js_setgct(js, gct->ival[0]);
  }
  
  struct ant_runtime *rt = ant_runtime_init(js);
  
  init_console_module();
  init_json_module();
  init_fetch_module();
  
  init_timer_module(js);
  init_crypto_module(js, rt->ant_obj);
  
  js_set(js, rt->ant_obj, "serve", js_mkfun(js_serve));
  js_set(js, rt->ant_obj, "require", js_mkfun(js_require));
  js_set(js, rt->ant_obj, "signal", js_mkfun(js_signal));
  js_set(js, rt->ant_obj, "gc", js_mkfun(js_gc_trigger));
  js_set(js, rt->ant_obj, "alloc", js_mkfun(js_alloc));
  js_set(js, rt->ant_obj, "stats", js_mkfun(js_stats_fn));

  jsval_t exports_obj = js_mkobj(js);
  js_set(js, rt->ant_obj, "exports", exports_obj);

  int result = execute_module(js, module_file);
  
  while (has_pending_microtasks() || has_pending_timers()) {
    process_microtasks(js);
    
    if (has_pending_timers()) {
      int64_t next_timeout_ms = get_next_timer_timeout();
      
      if (next_timeout_ms <= 0) {
        process_timers(js);
        continue;
      } else {
        usleep(next_timeout_ms > 1000000 ? 1000000 : next_timeout_ms * 1000);
      }
    }
  }
  
  if (dump) js_dump(js);
  
  js_destroy(js);
  arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
  
  return result;
}