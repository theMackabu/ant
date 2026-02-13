#include <compat.h> // IWYU pragma: keep
#include <arena.h>

#include <oxc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <argtable3.h>

#include "ant.h"
#include "config.h"
#include "repl.h"
#include "utils.h"
#include "reactor.h"
#include "runtime.h"
#include "snapshot.h"
#include "esm/remote.h"
#include "internal.h"

#include "cli/pkg.h"
#include "cli/misc.h"
#include "cli/version.h"
#include "cli/cprintf.h"

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
#include "modules/collections.h"

int js_result = EXIT_SUCCESS;
typedef int (*cmd_fn)(int argc, char **argv);

typedef struct {
  const char *name;
  const char *alias;
  const char *desc;
  cmd_fn fn;
} subcommand_t;

static const subcommand_t subcommands[] = {
  {"init",    NULL,      "Create a new package.json",              pkg_cmd_init},
  {"install", "i",       "Install dependencies from lockfile",     pkg_cmd_install},
  {"add",     "a",       "Add a package to dependencies",          pkg_cmd_add},
  {"remove",  "rm",      "Remove a package from dependencies",     pkg_cmd_remove},
  {"trust",   NULL,      "Run lifecycle scripts for packages",     pkg_cmd_trust},
  {"run",     NULL,      "Run a script from package.json",         pkg_cmd_run},
  {"exec",    "x",       "Run a command from node_modules/.bin",   pkg_cmd_exec},
  {"why",     "explain", "Show why a package is installed",        pkg_cmd_why},
  {"info",    NULL,      "Show package information from registry", pkg_cmd_info},
  {"ls",      "list",    "List installed packages",                pkg_cmd_ls},
  {"cache",   NULL,      "Manage the package cache",               pkg_cmd_cache},
  {"create",  NULL,      "Scaffold a project from a template",     pkg_cmd_create},
  {NULL, NULL, NULL, NULL}
};

static void parse_ant_debug(const char *flag) {
  if (strncmp(flag, "dump-cprintf=", 13) == 0) {
    const char *mode = flag + 13;
    if (strcmp(mode, "bytecode") == 0 || strcmp(mode, "all") == 0) cprintf_debug = true;
    if (strcmp(mode, "hex") == 0      || strcmp(mode, "all") == 0) cprintf_debug_hex = true;
  }
  
  else fprintf(stderr, "warning: unknown ANT_DEBUG flag: %s\n", flag);
}

static const subcommand_t *find_subcommand(const char *name) {
  for (const subcommand_t *cmd = subcommands; cmd->name; cmd++) {
    if (strcmp(name, cmd->name) == 0) return cmd;
    if (cmd->alias && strcmp(name, cmd->alias) == 0) return cmd;
  }
  return NULL;
}

static void print_subcommands(void) {
  cprintf("<bold>Commands:</>\n");
  for (const subcommand_t *cmd = subcommands; cmd->name; cmd++) {
    cprintf("  <pad=18>%s</pad> %s\n", cmd->name, cmd->desc);
  }
  cprintf("\n  <pad=18><command> <bold_cyan>--help</></pad> Print help text for command.\n");
  printf("\n");
}

static char *read_stdin(size_t *len) {
  size_t cap = 4096;
  *len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  
  size_t n;
  while ((n = fread(buf + *len, 1, cap - *len, stdin)) > 0) {
    *len += n;
    if (*len == cap) {
      cap *= 2; char *next = realloc(buf, cap);
      if (!next) { free(buf); return NULL; }
      buf = next;
    }
  }
  buf[*len] = '\0';
  return buf;
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

static void eval_code(struct js *js, const char *script, size_t len, const char *tag, bool should_print) {
  js_set_filename(js, tag);
  js_setup_import_meta(js, tag);
  js_mkscope(js);
  
  js_set(js, js_glob(js), "__dirname", js_mkstr(js, ".", 1));
  js_set(js, js_glob(js), "__filename", js_mkstr(js, tag, strlen(tag)));
  
  jsval_t result = js_eval(js, script, len);
  js_run_event_loop(js);
  
  char cbuf_stack[512]; js_cstr_t cstr = js_to_cstr(
    js, result, cbuf_stack, sizeof(cbuf_stack)
  );
  
  if (vtype(result) == T_ERR) {
    fprintf(stderr, "%s\n", cstr.ptr);
    js_result = EXIT_FAILURE;
  } else if (should_print) {
    if (vtype(result) == T_STR) printf("%s\n", cstr.ptr ? cstr.ptr : "");
    else if (cstr.ptr && strcmp(cstr.ptr, "undefined") != 0) {
      print_value_colored(cstr.ptr, stdout); printf("\n");
    }
  } 
  
  if (cstr.needs_free) free((void *)cstr.ptr);
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
  
  jsval_t result = js_eval_cached(js, js_code, js_len);
  free(js_code);
  
  if (vtype(result) == T_ERR) {
    fprintf(stderr, "%s\n", js_str(js, result));
    return EXIT_FAILURE;
  }
  
  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  int filtered_argc = 0;
  
  const char *binary_name = strrchr(argv[0], '/');
  binary_name = binary_name ? binary_name + 1 : argv[0];
  
  if (strcmp(binary_name, "antx") == 0) {
    char **exec_argv = try_oom(sizeof(char*) * (argc + 2));
    exec_argv[0] = argv[0]; exec_argv[1] = "x";
    for (int i = 1; i < argc; i++) exec_argv[i + 1] = argv[i];
    exec_argv[argc + 1] = NULL;
    
    int exitcode = pkg_cmd_exec(argc, exec_argv + 1);
    free(exec_argv); return exitcode;
  }
  
  char **filtered_argv = try_oom(sizeof(char*) * argc);
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--verbose") == 0) pkg_verbose = true;
    else if (strcmp(argv[i], "--no-color") == 0) io_no_color = true;
    else if (strncmp(argv[i], "--ANT_DEBUG:", 12) == 0) parse_ant_debug(argv[i] + 12);
    else filtered_argv[filtered_argc++] = argv[i];
  }
  
  if (filtered_argc >= 2 && filtered_argv[1][0] != '-') {
    const subcommand_t *cmd = find_subcommand(filtered_argv[1]);
    if (cmd) {
      int exitcode = cmd->fn(filtered_argc - 1, filtered_argv + 1);
      free(filtered_argv);
      return exitcode;
    }
    
    if (pkg_script_exists("package.json", filtered_argv[1])) {
      int exitcode = pkg_cmd_run(filtered_argc, filtered_argv);
      free(filtered_argv);
      return exitcode;
    }
  }
  
  argc = filtered_argc;
  argv = filtered_argv;

  struct arg_str *eval = arg_str0("e", "eval", "<script>", "evaluate script");
  struct arg_lit *print = arg_lit0("p", "print", "evaluate script and print result");
    
  struct arg_file *file = arg_file0(NULL, NULL, NULL, NULL);
  struct arg_file *localstorage_file = arg_file0(NULL, "localstorage-file", "<path>", "file path for localStorage persistence");
  
  struct arg_lit *version = arg_lit0("v", "version", "display version information and exit");
  struct arg_lit *version_raw = arg_lit0(NULL, "version-raw", "raw version number for scripts");
  
  struct arg_lit *help = arg_lit0("h", "help", "display this help and exit");
  struct arg_end *end = arg_end(20);
  
  void *argtable[] = {
    eval, print, 
    localstorage_file, file,
    version, version_raw,
    help, end
  };

  int nerrors = arg_parse(argc, argv, argtable);
  
  if (help->count > 0) {
    cprintf("<bold_red>Ant</> is a tiny JavaScript runtime and package manager (%s)\n\n", ANT_VERSION);
    cprintf("<bold>Usage: ant <yellow>[module.js]</yellow> <cyan>[...flags]</><reset/>\n");
    cprintf("<bold><space=7/>ant <<command>><space=3/><cyan>[...args]</><reset/>\n\n");
    printf("If no module file is specified, Ant starts in REPL mode.\n\n");
    print_subcommands();
    cprintf("<bold>Flags:</>\n");
    print_flags_help(stdout, argtable);
    print_flag(stdout, (flag_help_t){ .l = "verbose",  .g = "enable verbose output" });
    print_flag(stdout, (flag_help_t){ .l = "no-color", .g = "disable colored output" });
    arg_freetable(argtable, ARGTABLE_COUNT);
    free(filtered_argv);
    return EXIT_SUCCESS;
  }
  
  if (version_raw->count > 0) {
    fputs(ANT_VERSION "\n", stdout);
    arg_freetable(argtable, ARGTABLE_COUNT);
    free(filtered_argv); return EXIT_SUCCESS;
  }
  
  if (version->count > 0) {
    int res = ant_version(argtable);
    free(filtered_argv); return res;
  }
  
  if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant");
    printf("Try 'ant --help' for more information.\n");
    arg_freetable(argtable, ARGTABLE_COUNT);
    free(filtered_argv); return EXIT_FAILURE;
  }
  
  bool has_stdin = !isatty(STDIN_FILENO);
  bool repl_mode = (file->count == 0 && eval->count == 0 && !has_stdin);
  
  const char *module_file = repl_mode 
    ? NULL 
    : (file->count > 0 ? file->filename[0] : NULL);
  
  ant_t *js;
  volatile char stack_base;
  
  if (!(js = js_create_dynamic())) {
    cfprintf(stderr, "<bold_red>FATAL</>: Failed to allocate for Ant.\n");
    arg_freetable(argtable, ARGTABLE_COUNT); free(filtered_argv);
    return EXIT_FAILURE;
  }
  
  js_setstackbase(js, (void *)&stack_base);
  ant_runtime_init(js, argc, argv, localstorage_file);

  init_symbol_module();
  init_collections_module();
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
  if (vtype(snapshot_result) == T_ERR) {
    fprintf(stderr, "Warning: Failed to load snapshot: %s\n", js_str(js, snapshot_result));
  }

  if (eval->count > 0) {
    const char *script = eval->sval[0];
    eval_code(js, script, strlen(script), "[eval]", print->count > 0);
  }
  
  else if (repl_mode) {
    ant_repl_run();
  }
  
  else if (has_stdin && file->count == 0) {
    size_t len = 0; char *buf = read_stdin(&len);
    if (!buf) { 
      fprintf(stderr, "Error: Out of memory\n"); 
      js_result = EXIT_FAILURE; goto cleanup; 
    }
    eval_code(js, buf, len, "[stdin]", print->count > 0); free(buf);
  } 
  
  else {
    struct stat path_stat;
    char *resolved_file = NULL;
    
    resolved_file = resolve_js_file(module_file);
    if (resolved_file) module_file = resolved_file;
    
    if (stat(module_file, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
      size_t len = strlen(module_file);
      int has_slash = (len > 0 && module_file[len - 1] == '/');
      if (resolved_file) free(resolved_file);
      resolved_file = try_oom(len + 10 + (has_slash ? 0 : 1));
      sprintf(resolved_file, "%s%sindex.js", module_file, has_slash ? "" : "/");
      module_file = resolved_file;
    }
    
    js_result = execute_module(js, module_file);
    js_run_event_loop(js);
    
    if (resolved_file) free(resolved_file);
  }
    
  cleanup: {
    js_destroy(js);
    arg_freetable(argtable, ARGTABLE_COUNT);
    free(filtered_argv);
  }
  
  return js_result;
}
