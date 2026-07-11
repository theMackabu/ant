#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <argtable3.h>
#include <crprintf.h>

#include "ant.h"
#include "gc.h"
#include "crash.h"
#include "repl.h"
#include "debug.h"
#include "utils.h"
#include "watch.h"
#include "reactor.h"
#include "runtime.h"
#include "snapshot.h"
#include "inspector.h"
#include "esm/commonjs.h"
#include "esm/loader.h"
#include "esm/library.h"
#include "esm/remote.h"
#include "internal.h"
#include "silver/vm.h"
#include "messages.h"

#ifdef _WIN32
#include "output.h"
#endif

#include "cli/pkg.h"
#include "cli/registry.h"
#include "cli/misc.h"
#include "cli/version.h"
#include "sandbox/assets.h"
#include "sandbox/cli.h"
#include "sandbox/host.h"
#include "sandbox/policy.h"
#include "sandbox/sandbox.h"
#include "sandbox/transport.h"
#include "sandbox/vm.h"

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
#include "modules/request.h"
#include "modules/response.h"
#include "modules/shell.h"
#include "modules/process.h"
#include "modules/tty.h"
#include "modules/path.h"
#include "modules/ffi.h"
#include "modules/events.h"
#include "modules/lmdb.h"
#include "modules/performance.h"
#include "modules/uri.h"
#include "modules/url.h"
#include "modules/reflect.h"
#include "modules/symbol.h"
#include "modules/date.h"
#include "modules/math.h"
#include "modules/bigint.h"
#include "modules/regex.h"
#include "modules/textcodec.h"
#include "modules/sessionstorage.h"
#include "modules/localstorage.h"
#include "modules/navigator.h"
#include "modules/child_process.h"
#include "modules/readline.h"
#include "modules/observable.h"
#include "modules/collections.h"
#include "modules/iterator.h"
#include "modules/generator.h"
#include "modules/module.h"
#include "modules/util.h"
#include "modules/async_hooks.h"
#include "modules/net.h"
#include "modules/tls.h"
#include "modules/http_metadata.h"
#include "modules/http_parser.h"
#include "modules/http_writer.h"
#include "modules/websocket.h"
#include "modules/eventsource.h"
#include "modules/dns.h"
#include "modules/assert.h"
#include "modules/domexception.h"
#include "modules/abort.h"
#include "modules/globals.h"
#include "modules/intl.h"
#include "modules/wasm.h"
#include "modules/string_decoder.h"
#include "modules/stream.h"
#include "modules/structured-clone.h"
#include "modules/v8.h"
#include "modules/worker_threads.h"
#include "modules/headers.h"
#include "modules/blob.h"
#include "modules/formdata.h"
#include "modules/zlib.h"
#include "modules/rpc.h"
#include "modules/sandbox.h"
#include "streams/queuing.h"
#include "streams/readable.h"
#include "streams/writable.h"
#include "streams/transform.h"
#include "streams/codec.h"
#include "streams/compression.h"

int js_result = EXIT_SUCCESS;
typedef int (*cmd_fn)(int argc, char **argv);

typedef struct {
  const char *name;
  const char *alias;
  const char *desc;
  cmd_fn fn;
} subcommand_t;

static const subcommand_t subcommands[] = {
  {"init",    NULL,      "Create a new package.json",                    pkg_cmd_init},
  {"install", "i",       "Install dependencies from lockfile",           pkg_cmd_install},
  {"update",  "up",      "Re-resolve dependencies and refresh lockfile", pkg_cmd_update},
  {"add",     "a",       "Add a package to dependencies",                pkg_cmd_add},
  {"remove",  "rm",      "Remove a package from dependencies",           pkg_cmd_remove},
  {"trust",   NULL,      "Run lifecycle scripts for packages",           pkg_cmd_trust},
  {"run",     NULL,      "Run a script from package.json",               pkg_cmd_run},
  {"exec",    "x",       "Run a command from node_modules/.bin",         pkg_cmd_exec},
  {"login",   NULL,      "Authenticate with ants.land",                  pkg_cmd_login},
  {"publish", NULL,      "Publish the current package to ants.land",     pkg_cmd_publish},
  {"config",  NULL,      "Manage package manager settings",              pkg_cmd_config},
  {"why",     "explain", "Show why a package is installed",              pkg_cmd_why},
  {"info",    NULL,      "Show package information from registry",       pkg_cmd_info},
  {"ls",      "list",    "List installed packages",                      pkg_cmd_ls},
  {"cache",   NULL,      "Manage the package cache",                     pkg_cmd_cache},
  {"create",  NULL,      "Scaffold a project from a template",           pkg_cmd_create},
  {"sandbox", NULL,      "Run a script in the Ant sandbox",              ant_sandbox_cmd},
  {"upgrade", NULL,      "Upgrade Ant to the latest version",            ant_upgrade},
  {NULL, NULL, NULL, NULL}
};

static void ant_debug_apply(const char *key, const char *val) {
  if (strcmp(key, "gc") == 0) {
    if (strcmp(val, "disable") == 0) gc_disabled = true;
  }

  else if (strcmp(key, "dump/parse") == 0) {
    if (strcmp(val, "trace") == 0) sv_debug_enable(SV_DEBUG_PARSE);
  }

  else if (strcmp(key, "dump/compile") == 0) {
    if (strcmp(val, "trace") == 0) sv_debug_enable(SV_DEBUG_COMPILE);
  }

  else if (strcmp(key, "dump/crprintf") == 0) {
    if (strcmp(val, "bytecode") == 0 || strcmp(val, "all") == 0) crprintf_set_debug(true);
    if (strcmp(val, "hex") == 0      || strcmp(val, "all") == 0) crprintf_set_debug_hex(true);
  }

  else if (strcmp(key, "dump/vm") == 0) {
    if (strcmp(val, "bytecode") == 0 || strcmp(val, "all") == 0) sv_debug_enable(SV_DEBUG_DUMP_BYTECODE);
    if (strcmp(val, "jit") == 0      || strcmp(val, "all") == 0) sv_debug_enable(SV_DEBUG_DUMP_JIT);
    if (strcmp(val, "op-warn") == 0  || strcmp(val, "all") == 0) sv_debug_enable(SV_DEBUG_JIT_WARN);
  }

  else if (strcmp(key, "sandbox") == 0) {
    if (strcmp(val, "bypass-manifest") == 0) ant_sandbox_assets_bypass_manifest = true;
  }
}

static inline void setup_console_colors(void) {
  crprintf_var("version", ANT_VERSION);
  crprintf_var("fatal", "<bold+red>FATAL</bold>");
  crprintf_var("error", "<red>Error</red>");
  crprintf_var("warn", "<yellow>Warning</yellow>");
}

static void parse_ant_debug_flags(void) {  
  const char *env = getenv("ANT_DEBUG");
  if (!env || !*env) return;

  char *buf = strdup(env);
  if (!buf) return;

  char *sp = NULL, *vp = NULL;
  for (char *e = strtok_r(buf, " ", &sp); e; e = strtok_r(NULL, " ", &sp)) {
    char *sep = strchr(e, ':');
    if (!sep) { crfprintf(stderr, msg.unknown_flag_warn, e); continue; }
    *sep++ = '\0';
    for (char *v = strtok_r(sep, ",", &vp); v; v = strtok_r(NULL, ",", &vp))
      ant_debug_apply(e, v);
  }

  free(buf);
}

static const subcommand_t *find_subcommand(const char *name) {
  for (const subcommand_t *cmd = subcommands; cmd->name; cmd++) {
    if (strcmp(name, cmd->name) == 0) return cmd;
    if (cmd->alias && strcmp(name, cmd->alias) == 0) return cmd;
  }
  return NULL;
}

static void print_subcommands(void) {
  crprintf("<bold>Commands:</>\n");
  for (const subcommand_t *cmd = subcommands; cmd->name; cmd++) {
    crprintf("  <pad=18>%s</pad> %s\n", cmd->name, cmd->desc);
  }
  crprintf(msg.ant_command_extra);
  printf("\n");
}

static void print_commands(void **argtable) {
  if (ant_version_print_update_hint(stdout)) printf("\n");
  crprintf(msg.ant_help_header);

  print_subcommands();
  crprintf(msg.ant_help_flags);
  
  print_flags_help(stdout, argtable);
  print_flag(stdout, (flag_help_t){ .s = "V", .l = "verbose",  .g = "enable verbose output" });
  print_flag(stdout, (flag_help_t){ .l = "no-color", .g = "disable colored output" });
}

typedef struct { 
  int argc; 
  char **argv; 
} argv_split_t;

static bool is_valued_flag(const char *arg) {
  return 
    strcmp(arg, "-e") == 0 || 
    strcmp(arg, "--eval") == 0 || 
    strcmp(arg, "--repl") == 0 ||
    strcmp(arg, "--type") == 0 ||
    strcmp(arg, "--localstorage-file") == 0;
}

static int find_argv_token_index(int argc, char **argv, const char *token) {
  if (!token) return -1;
  for (int i = 1; i < argc; i++) if (argv[i] == token) return i;
  return -1;
}

static argv_split_t split_script_args(int *argc, char **argv) {
  for (int i = 1; i < *argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      argv_split_t tail = { *argc - i - 1, argv + i + 1 };
      *argc = i;
      return tail;
    }
    
    if (argv[i][0] == '-') {
      if (is_valued_flag(argv[i]) && i + 1 < *argc) i++;
      continue;
    }
    
    argv_split_t tail = { *argc - i - 1, argv + i + 1 };
    *argc = i + 1;
    return tail;
  }
  
  return (argv_split_t){ 0, NULL };
}

static argv_split_t build_process_argv(int argc, char **argv, const char *module, argv_split_t script) {
  if (!module) return (argv_split_t){ argc, argv };

  int total = 2 + script.argc;
  char **out = try_oom(sizeof(char*) * (total + 1));
  char *resolved = esm_is_url(module) ? NULL : realpath(module, NULL);

  out[0] = argv[0]; out[1] = resolved ? resolved : (char *)module;
  for (int i = 0; i < script.argc; i++) out[2 + i] = script.argv[i];
  out[total] = NULL;

  return (argv_split_t){ total, out };
}

static void parse_inspector_spec(const char *spec, char *host, size_t host_len, int *port) {
  if (!spec || !*spec) return;
  bool all_digits = true;
  
  for (const char *p = spec; *p; p++) if (!isdigit((unsigned char)*p)) {
    all_digits = false;
    break;
  }

  if (all_digits) {
    int parsed = atoi(spec);
    if (parsed > 0) *port = parsed;
    return;
  }

  const char *colon = strrchr(spec, ':');
  if (colon && colon != spec && colon[1]) {
    size_t len = (size_t)(colon - spec);
    if (len >= host_len) len = host_len - 1;
    
    memcpy(host, spec, len);
    host[len] = '\0';
    
    int parsed = atoi(colon + 1);
    if (parsed > 0) *port = parsed;
    
    return;
  }

  snprintf(host, host_len, "%s", spec);
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

static void eval_code(
  ant_t *js, const char *script, size_t len,
  const char *tag, bool should_print, bool module_type
) {
  js_set_filename(js, tag);
  js_setup_import_meta(js, tag);
  
  js_set(js, js_glob(js), "__dirname", js_mkstr(js, ".", 1));
  js_set(js, js_glob(js), "__filename", js_mkstr(js, tag, strlen(tag)));
  
  ant_value_t result;
  if (module_type) {
    ant_value_t ns = js_mkobj(js);
    result = is_err(ns) ? ns : js_esm_eval_module_source(js, tag, script, len, ns);
  } else {
    ant_value_t ns = js_mkobj(js);
    result = is_err(ns) ? ns : esm_load_commonjs_module(js, tag, script, len, ns);
  }
  
  js_run_event_loop(js);
  
  if (print_uncaught_throw(js)) {
    js_result = EXIT_FAILURE;
    return;
  }
  
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

static int execute_module(ant_t *js, const char *filename) {
  char *use_path_owned = NULL;
  
  const char *use_path = filename;
  const char *stable_use_path = filename;

  ant_value_t ns = 0;
  ant_value_t specifier = 0;
  ant_value_t default_export = 0;
  
  if (esm_is_url(filename)) {
    js_set(js, js_glob(js), "__dirname", js_mkundef());
    specifier = js_mkstr(js, filename, strlen(filename));
  } else {
    char *file_path = strdup(filename);
    char *dir = dirname(file_path);
    js_set(js, js_glob(js), "__dirname", js_mkstr(js, dir, strlen(dir)));
    free(file_path);
    
    use_path_owned = realpath(filename, NULL);
    if (use_path_owned) use_path = use_path_owned;
    specifier = js_esm_make_file_url(js, use_path);
  }

  const char *interned = intern_string(use_path, strlen(use_path));
  if (interned) stable_use_path = interned;
  else stable_use_path = use_path;
  
  js_set(js, js_glob(js), 
    "__filename", 
    js_mkstr(js, filename, strlen(filename))
  );
  
  js_set_filename(js, stable_use_path);
  js_setup_import_meta(js, stable_use_path);
  ns = js_esm_import_sync(js, specifier);
  
  free(use_path_owned);
  if (print_uncaught_throw(js)) return EXIT_FAILURE;
  
  if (vtype(ns) == T_ERR) {
    fprintf(stderr, "%s\n", js_str(js, ns));
    return EXIT_FAILURE;
  }

  default_export = js_get(js, ns, "default");
  return server_maybe_start_from_export(js, default_export);
}

static char **build_sandbox_process_argv(const char *argv0, ant_sandbox_request_t *sandbox, int *argc_out) {
  int argc = 1;
  if (sandbox->mode == ANT_SANDBOX_REQUEST_RUN) argc = 2 + sandbox->argc;

  char **argv = try_oom(sizeof(*argv) * (size_t)(argc + 1));
  argv[0] = (char *)argv0;
  
  if (sandbox->mode == ANT_SANDBOX_REQUEST_RUN) {
    argv[1] = sandbox->entry;
    for (int i = 0; i < sandbox->argc; i++) argv[2 + i] = sandbox->argv[i];
  }
  
  argv[argc] = NULL;
  *argc_out = argc;
  
  return argv;
}

static int execute_sandbox_request(ant_t *js, ant_sandbox_request_t *sandbox, const char *argv0, bool *close_out) {
  *close_out = false;
  if (sandbox->mode == ANT_SANDBOX_REQUEST_CLOSE) {
    *close_out = true;
    return EXIT_SUCCESS;
  }

  int request_argc = 0;
  char **request_argv = build_sandbox_process_argv(argv0, sandbox, &request_argc);
  
  ant_runtime_set_argv(request_argc, request_argv);
  process_refresh_sandbox_argv();

  if (sandbox->cwd && chdir(sandbox->cwd) != 0) {
    fprintf(stderr, "sandbox daemon: failed to chdir to %s: %s\n", sandbox->cwd, strerror(errno));
    free(request_argv);
    return EXIT_FAILURE;
  }

  io_set_sandbox_terminal(sandbox->capabilities);
  process_set_sandbox_terminal(sandbox->capabilities, sandbox->tty_rows, sandbox->tty_cols);
  tty_set_sandbox_terminal(sandbox->capabilities, sandbox->tty_rows, sandbox->tty_cols);
  ant_sandbox_policy_set_forwards(sandbox->forward_ports, sandbox->forward_count);

  if (sandbox->mode == ANT_SANDBOX_REQUEST_EVAL) {
    int rc = ant_sandbox_eval_module(js, sandbox->source, strlen(sandbox->source));
    free(request_argv);
    return rc;
  }

  if (sandbox->mode == ANT_SANDBOX_REQUEST_RUN) {
    char *resolved_file = resolve_js_file(sandbox->entry);
    int rc = EXIT_SUCCESS;

    if (!resolved_file) {
      crfprintf(stderr, msg.module_not_found, sandbox->entry);
      rc = EXIT_FAILURE;
    } else {
      rc = execute_module(js, resolved_file);
      js_run_event_loop(js);
      free(resolved_file);
    }
    free(request_argv);
    return rc;
  }

  fprintf(stderr, "sandbox daemon: unsupported request mode\n");
  free(request_argv);
  
  return EXIT_FAILURE;
}

static int run_sandbox_daemon_loop(ant_t *js, ant_sandbox_request_t *sandbox, const char *argv0) {
for (;;) {
  bool close_requested = false;
  int code = execute_sandbox_request(js, sandbox, argv0, &close_requested);
  
  ant_sandbox_transport_send_exit(code);
  ant_sandbox_request_free(sandbox);
  memset(sandbox, 0, sizeof(*sandbox));

  if (close_requested) return code;
  if (!ant_sandbox_read_request_transport(sandbox)) return EXIT_FAILURE;
}}

int main(int argc, char *argv[]) {
  if (ant_sandbox_vm_helper_is_process(argv[0])) return ant_sandbox_vm_helper_process_main();
  bool internal_crash_report_mode = ant_crash_is_internal_report(argc, argv);
  
  if (internal_crash_report_mode) argc = 1;
  if (!internal_crash_report_mode && !getenv("ANT_NO_CRASH_HANDLER")) ant_crash_init(argc, argv);
  
  #ifdef _WIN32
  ant_output_init_console();
  #endif
  
  setup_console_colors();
  parse_ant_debug_flags();

  ant_inspector_options_t inspector = {
    .enabled = false,
    .wait_for_session = false,
    .host = "127.0.0.1",
    .port = 9229,
  };
  
  int filtered_argc = 0; int original_argc = argc;
  char **original_argv = argv;
  
  bool sandbox_daemon = false;
  ant_sandbox_request_t sandbox = { 0 };

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
  bool parse_global_args = true;
  
  for (int i = 0; i < argc; i++) {
    const char *arg = argv[i];
    
    if (i == 0 || !parse_global_args) {
      filtered_argv[filtered_argc++] = argv[i];
      continue;
    }
    
    if (strcmp(arg, "--") == 0) {
      parse_global_args = false;
      filtered_argv[filtered_argc++] = argv[i];
      continue;
    }
    
    if (is_valued_flag(arg)) {
      filtered_argv[filtered_argc++] = argv[i];
      if (i + 1 < argc) filtered_argv[filtered_argc++] = argv[++i];
      continue;
    }
    
    if (arg[0] != '-') {
      parse_global_args = false;
      filtered_argv[filtered_argc++] = argv[i];
      continue;
    }

    if (strcmp(arg, "-V") == 0 || strcmp(arg, "--verbose") == 0) pkg_verbose = true;
    else if (strcmp(arg, "--force") == 0) pkg_force = true;
    else if (strcmp(arg, "--no-color") == 0) { crprintf_set_color(false); io_no_color = true; }
    else if (strncmp(arg, "--stack-size=", 13) == 0) sv_user_stack_size_kb = atoi(arg + 13);
    else if (strcmp(arg, "--sandbox-daemon") == 0) sandbox_daemon = true;
    else if (strcmp(arg, "--inspect") == 0) inspector.enabled = true;
    
    else if (strncmp(arg, "--inspect=", 10) == 0) {
      inspector.enabled = true;
      parse_inspector_spec(arg + 10, inspector.host, sizeof(inspector.host), &inspector.port);
    }
    
    else if (strcmp(arg, "--inspect-wait") == 0) {
      inspector.enabled = true;
      inspector.wait_for_session = true;
    }
    
    else if (strncmp(arg, "--inspect-wait=", 15) == 0) {
      inspector.enabled = true;
      inspector.wait_for_session = true;
      parse_inspector_spec(arg + 15, inspector.host, sizeof(inspector.host), &inspector.port);
    }
    
    else filtered_argv[filtered_argc++] = argv[i];
  }
  
  argc = filtered_argc; 
  argv = filtered_argv;
  
  argv_split_t script_tail = split_script_args(&argc, argv);
  argv_split_t proc_argv = { 0, NULL };

  #define ARG_ITEMS(X) \
    X(struct arg_str *, eval, arg_str0("e", "eval", "<script>", "evaluate script")) \
    X(struct arg_str *, repl, arg_str0(NULL, "repl", "<script>", "start REPL after evaluating script")) \
    X(struct arg_str *, input_type, arg_str0(NULL, "type", "<type>", "set string input type: commonjs or module")) \
    X(struct arg_lit *, print, arg_lit0("p", "print", "evaluate script and print result")) \
    X(struct arg_lit *, watch, arg_lit0("w", "watch", "restart process when entry file changes")) \
    X(struct arg_lit *, web, arg_lit0(NULL, "web", "enable web-compatible globals")) \
    X(struct arg_lit *, no_clear_screen, arg_lit0(NULL, "no-clear-screen", "keep output when restarting in watch mode")) \
    X(struct arg_file *, localstorage_file, arg_file0(NULL, "localstorage-file", "<path>", "file path for localStorage persistence")) \
    X(struct arg_file *, file, arg_filen(NULL, NULL, NULL, 0, argc, NULL)) \
    X(struct arg_lit *, version, arg_lit0("v", "version", "display version information and exit")) \
    X(struct arg_lit *, version_raw, arg_lit0(NULL, "version-raw", "raw version number for scripts")) \
    X(struct arg_lit *, help, arg_lit0("h", "help", "display this help and exit")) \
    X(struct arg_end *, end, arg_end(20))
  
  #define DECL(t, n, init) t n = init;
  ARG_ITEMS(DECL)
  #undef DECL
  
  #define REF(t, n, init) n,
  void *argtable[] = { ARG_ITEMS(REF) };
  int nerrors = arg_parse(argc, argv, argtable);
  #undef REF
  
  #define CLEANUP_ARGS_AND_ARGV() ({ \
    if (proc_argv.argv != argv) free(proc_argv.argv); \
    ant_sandbox_request_free(&sandbox); \
    arg_freetable(argtable, ARGTABLE_COUNT); \
    free(filtered_argv); \
  })

  if (help->count > 0) {
    print_commands(argtable);
    CLEANUP_ARGS_AND_ARGV();
    return EXIT_SUCCESS;
  }
  
  if (version_raw->count > 0) {
    fputs(ANT_VERSION "\n", stdout);
    CLEANUP_ARGS_AND_ARGV();
    return EXIT_SUCCESS;
  }
  
  if (version->count > 0) {
    int res = ant_version(argtable);
    free(filtered_argv); return res;
  }
  

  if (nerrors > 0) {
    print_errors(stdout, end);
    print_commands(argtable);
    CLEANUP_ARGS_AND_ARGV();
    return EXIT_FAILURE;
  }

  if (no_clear_screen->count > 0 && watch->count == 0) {
    crfprintf(stderr, msg.misuse_clear_screen);
    CLEANUP_ARGS_AND_ARGV();
    return EXIT_FAILURE;
  }

  if (sandbox_daemon) if (!ant_sandbox_read_request_transport(&sandbox)) {
    CLEANUP_ARGS_AND_ARGV();
    return EXIT_FAILURE;
  }

  if (!sandbox_daemon && eval->count == 0 && repl->count == 0 && file->count > 0 && file->filename[0] != NULL) {
    const char *positional = file->filename[0];
    int first_pos_idx = find_argv_token_index(argc, argv, positional);
    
    if (first_pos_idx <= 0 || first_pos_idx >= argc) {
      crfprintf(stderr, msg.argument_fatal);
      CLEANUP_ARGS_AND_ARGV();
      return EXIT_FAILURE;
    }

    const subcommand_t *cmd = find_subcommand(positional);

    if (cmd) {
      if (watch->count > 0) {
        crfprintf(stderr, msg.watch_subcommand_error);
        CLEANUP_ARGS_AND_ARGV();
        return EXIT_FAILURE;
      }

      int cmd_argc = argc - first_pos_idx;
      char **cmd_argv = argv + first_pos_idx;
      char **cmd_argv_full = NULL;
      
      if (script_tail.argc > 0) {
        cmd_argv_full = try_oom(sizeof(*cmd_argv_full) * (size_t)(cmd_argc + script_tail.argc + 1));
        
        memcpy(cmd_argv_full, cmd_argv, sizeof(*cmd_argv_full) * (size_t)cmd_argc);
        memcpy(cmd_argv_full + cmd_argc, script_tail.argv, sizeof(*cmd_argv_full) * (size_t)script_tail.argc);
        
        cmd_argc += script_tail.argc;
        cmd_argv_full[cmd_argc] = NULL;
        cmd_argv = cmd_argv_full;
      }

      int exitcode = cmd->fn(cmd_argc, cmd_argv);
      free(cmd_argv_full);
      
      CLEANUP_ARGS_AND_ARGV();
      return exitcode;
    }

    if (pkg_script_exists("package.json", positional)) {
      if (watch->count > 0) {
        crfprintf(stderr, msg.watch_subcommand_error);
        CLEANUP_ARGS_AND_ARGV();
        return EXIT_FAILURE;
      }

      int run_argc = argc - first_pos_idx + 1 + script_tail.argc;
      char **run_argv = try_oom(sizeof(*run_argv) * (size_t)(run_argc + 1));
      
      run_argv[0] = argv[0];
      int copied = argc - first_pos_idx;
      
      memcpy(run_argv + 1, argv + first_pos_idx, sizeof(*run_argv) * (size_t)copied);
      if (script_tail.argc > 0) {
        memcpy(run_argv + 1 + copied, script_tail.argv, sizeof(*run_argv) * (size_t)script_tail.argc);
      }
      
      run_argv[run_argc] = NULL;
      int exitcode = pkg_cmd_run(run_argc, run_argv);
      
      free(run_argv);
      CLEANUP_ARGS_AND_ARGV();
      
      return exitcode;
    }
  }
  
  bool has_stdin = !isatty(STDIN_FILENO);
  bool repl_mode = repl->count > 0 || (file->count == 0 && eval->count == 0 && !has_stdin);
  bool stdin_mode = (has_stdin && file->count == 0);

  if (repl->count > 0 && (eval->count > 0 || file->count > 0)) {
    fprintf(stderr, "Error: --repl cannot be combined with --eval or a script file.\n");
    CLEANUP_ARGS_AND_ARGV();
    return EXIT_FAILURE;
  }
  
  if (watch->count > 0 && (eval->count > 0 || repl_mode || stdin_mode)) {
    crfprintf(stderr, msg.watch_module_error);
    CLEANUP_ARGS_AND_ARGV();
    return EXIT_FAILURE;
  }

  bool module_input_type = false;
  if (input_type->count > 0) {
    const char *type_value = input_type->sval[0];
    if (strcmp(type_value, "module") == 0) module_input_type = true;
    else if (strcmp(type_value, "commonjs") != 0) {
      fprintf(stderr, "Error: --type must be either \"commonjs\" or \"module\".\n");
      CLEANUP_ARGS_AND_ARGV();
      return EXIT_FAILURE;
    }

    if (eval->count == 0 && !stdin_mode) {
      fprintf(stderr, "Error: --type can only be used with --eval or stdin.\n");
      CLEANUP_ARGS_AND_ARGV();
      return EXIT_FAILURE;
    }
  }
  
  const char *module_file = (repl_mode || file->count == 0) 
    ? NULL 
    : file->filename[0];

  if (sandbox_daemon) {
    if (sandbox.cwd && chdir(sandbox.cwd) != 0) {
      fprintf(stderr, "sandbox daemon: failed to chdir to %s: %s\n", sandbox.cwd, strerror(errno));
      CLEANUP_ARGS_AND_ARGV();
      return EXIT_FAILURE;
    }
    if (!ant_sandbox_transport_install_output_frames()) {
      fprintf(stderr, "sandbox daemon: failed to install framed output transport: %s\n", strerror(errno));
      CLEANUP_ARGS_AND_ARGV();
      return EXIT_FAILURE;
    }

    repl_mode = false;
    stdin_mode = false;

    if (sandbox.mode == ANT_SANDBOX_REQUEST_RUN) {
      module_file = sandbox.entry;
      script_tail = (argv_split_t){ sandbox.argc, sandbox.argv };
    } else {
      module_file = NULL;
    }
  }

  if (watch->count > 0) {
    char *resolved_file = NULL;

    resolved_file = resolve_js_file(module_file);
    if (resolved_file) module_file = resolved_file;

    if (!module_file || esm_is_url(module_file)) {
      crfprintf(stderr, msg.watch_entrypoint_error);
      if (resolved_file) free(resolved_file);
      CLEANUP_ARGS_AND_ARGV();
      return EXIT_FAILURE;
    }

    int res = ant_watch_run(original_argc, original_argv, module_file, no_clear_screen->count > 0);
    if (resolved_file) free(resolved_file);
    CLEANUP_ARGS_AND_ARGV();
    return res;
  }
  
  ant_t *js;
  volatile char stack_base;
  
  if (!(js = js_create_dynamic())) {
    crfprintf(stderr, msg.ant_allocation_fatal);
    CLEANUP_ARGS_AND_ARGV();
    return EXIT_FAILURE;
  }
  
  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, os_thread_stack_size() * 3 / 4);
  
  proc_argv = build_process_argv(argc, argv, module_file, script_tail);
  if (sandbox_daemon) ant_sandbox_set_guest_process(true);
  
  ant_runtime_init(js, proc_argv.argc, proc_argv.argv, localstorage_file);
  if (web->count > 0) rt->flags |= ANT_RUNTIME_WEB;
  
  if (sandbox_daemon) {
    io_set_sandbox_terminal(sandbox.capabilities);
    process_set_sandbox_terminal(sandbox.capabilities, sandbox.tty_rows, sandbox.tty_cols);
    tty_set_sandbox_terminal(sandbox.capabilities, sandbox.tty_rows, sandbox.tty_cols);
    ant_sandbox_policy_set_forwards(sandbox.forward_ports, sandbox.forward_count);
  }

  init_symbol_module();
  init_iterator_module();
  init_generator_module();
  init_timer_module();
  init_domexception_module();
  init_globals_module();
  init_intl_module();
  init_wasm_module();
  init_builtin_module();
  init_buffer_module();
  init_structured_clone_module();
  init_abort_module();
  init_headers_module();
  init_blob_module();
  init_formdata_module();
  init_math_module();
  init_bigint_module();
  init_date_module();
  init_regex_module();
  init_collections_module();
  init_queuing_strategies_module();
  init_readable_stream_module();
  init_writable_stream_module();
  init_transform_stream_module();
  init_codec_stream_module();
  init_compression_stream_module();
  init_fs_module();
  init_atomics_module();
  init_crypto_module();
  init_request_module();
  init_response_module();
  init_fetch_module();
  init_console_module();
  init_json_module();
  init_process_module();
  init_tty_module();
  init_events_module();
  init_websocket_module();
  init_performance_module();
  init_uri_module();
  init_url_module();
  init_reflect_module();
  init_textcodec_module();
  init_eventsource_module();
  init_sessionstorage_module();
  init_localstorage_module();
  init_navigator_module();
  init_observable_module();
  
  ant_register_library(shell_library, "ant:shell", NULL);
  ant_register_library(ffi_library, "ant:ffi", NULL);
  ant_register_library(lmdb_library, "ant:lmdb", NULL);
  ant_register_library(rpc_library, "ant:rpc", NULL);
  ant_register_library(sandbox_library, "ant:sandbox", NULL);
  
  ant_register_library(internal_http_parser_library, "ant:internal/http_parser", NULL);
  ant_register_library(internal_http_writer_library, "ant:internal/http_writer", NULL);
  ant_register_library(internal_http_metadata_library, "ant:internal/http_metadata", NULL);

  ant_standard_library("util", util_library);
  ant_standard_library("util/types", util_types_library);
  ant_standard_library("console", console_library);
  ant_standard_library("net", net_library);
  ant_standard_library("tls", tls_library);
  ant_standard_library("dns", dns_library);
  ant_standard_library("assert", assert_library);
  ant_standard_library("module", module_library);
  ant_standard_library("buffer", buffer_library);
  ant_standard_library("path", path_library);
  ant_standard_library("fs", fs_library);
  ant_standard_library("constants", fs_constants_library);
  ant_standard_library("os", os_library);
  ant_standard_library("url", url_library);
  ant_standard_library("perf_hooks", perf_hooks_library);
  ant_standard_library("process", process_library);
  ant_standard_library("crypto", crypto_library);
  ant_standard_library("events", events_library);
  ant_standard_library("tty", tty_library);
  ant_standard_library("readline", readline_library);
  ant_standard_library("child_process", child_process_library);
  ant_standard_library("worker_threads", worker_threads_library);
  ant_standard_library("async_hooks", async_hooks_library);
  ant_standard_library("v8", v8_library);
  ant_standard_library("zlib", zlib_library);
  ant_standard_library("string_decoder", string_decoder_library);
  ant_standard_library("stream", stream_library);
  ant_standard_library("timers", timers_library);
  
  ant_register_library(path_posix_library, "path/posix", "ant:path/posix", "node:path/posix", NULL);
  ant_register_library(path_win32_library, "path/win32", "ant:path/win32", "node:path/win32", NULL);
  
  ant_standard_library("fs/promises", fs_promises_library);
  ant_standard_library("timers/promises", timers_promises_library);
  ant_standard_library("readline/promises", readline_promises_library);
  ant_standard_library("stream/promises", stream_promises_library);
  ant_standard_library("stream/web", stream_web_library);

  ant_value_t snapshot_result = ant_load_snapshot(js);
  if (vtype(snapshot_result) == T_ERR) {
    crfprintf(stderr, msg.snapshot_warn, js_str(js, snapshot_result));
  }

  if (sandbox_daemon) {
    js_result = run_sandbox_daemon_loop(js, &sandbox, original_argv[0]);
    goto cleanup;
  }

  if (inspector.enabled && !ant_inspector_start(js, &inspector)) {
    fprintf(stderr, "Unable to start inspector on %s:%d\n", inspector.host, inspector.port);
    js_result = EXIT_FAILURE;
    goto cleanup;
  }

  if (inspector.enabled && eval->count > 0) {
    const char *script = eval->sval[0];
    ant_inspector_register_script_source("[eval]", script, strlen(script), false);
  }

  if (inspector.enabled && eval->count == 0 && !repl_mode && !stdin_mode) for (
  int fi = 0; fi < file->count; fi++) {
    char *resolved_file = resolve_js_file(file->filename[fi]);
    if (!resolved_file) continue;
    ant_inspector_register_script_file(resolved_file, true);
    free(resolved_file);
  }
  
  if (inspector.wait_for_session) ant_inspector_wait_for_session();
  if (internal_crash_report_mode) js_result = ant_crash_run_internal_report(js);

  else if (eval->count > 0) {
    const char *script = eval->sval[0];
    eval_code(js, script, strlen(script), "[eval]", print->count > 0, module_input_type);
  }
  
  else if (repl_mode) {
    ant_repl_run(repl->count > 0 ? repl->sval[0] : NULL);
  }
  
  else if (stdin_mode) {
    size_t len = 0; char *buf = read_stdin(&len);
    if (!buf) { 
      crfprintf(stderr, msg.ant_allocation_fatal);
      js_result = EXIT_FAILURE; goto cleanup; 
    }
    if (inspector.enabled) ant_inspector_register_script_source("[stdin]", buf, len, false);
    eval_code(js, buf, len, "[stdin]", print->count > 0, module_input_type); free(buf);
  } 
  
  else {
  for (int fi = 0; fi < file->count; fi++) {
    const char *fl = file->filename[fi];
    char *resolved_file = resolve_js_file(fl);
    if (!resolved_file) {
      crfprintf(stderr, msg.module_not_found, fl);
      js_result = EXIT_FAILURE; break;
    }
    
    fl = resolved_file;
    js_result = execute_module(js, fl);
    js_run_event_loop(js);
    
    free(resolved_file);
    if (js_result != EXIT_SUCCESS) break;
  }}
    
  cleanup: {
    js_destroy(js);
    CLEANUP_ARGS_AND_ARGV();
  }
  #undef CLEANUP_ARGS_AND_ARGV
  
  return js_result;
}
