#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crprintf.h>

#include "ant.h"
#include "crash.h"
#include "bootstrap.h"
#include "repl.h"
#include "debug.h"
#include "utils.h"
#include "watch.h"
#include "reactor.h"
#include "runtime.h"
#include "isolate.h"
#include "inspector.h"
#include "esm/library.h"
#include "esm/remote.h"
#include "silver/vm.h"
#include "messages.h"

#include "cli/commands.h"
#include "cli/daemon.h"
#include "cli/entry.h"
#include "cli/options.h"
#include "cli/pkg.h"
#include "cli/preload.h"
#include "cli/startup.h"
#include "sandbox/host.h"
#include "sandbox/transport.h"
#include "sandbox/vm.h"
#include "modules/process.h"
#include "modules/sandbox.h"
#include "modules/url.h"

typedef enum {
  RUN_FILES,
  RUN_PKG,
  RUN_EVAL,
  RUN_REPL,
  RUN_STDIN,
  RUN_SANDBOX,
} run_mode_t;

typedef struct {
  run_mode_t mode;
  ant_input_mode_t input;
  const char *module_file;
  ant_argv_t script_tail;
  pkg_argv_t pkg;
  ant_sandbox_request_t sandbox;
} run_plan_t;

static bool check_location(const char *location) {
  if (!location) return true;
  url_state_t url;
  if (Ant_URL_ParseLocation(location, &url) != 0) {
    fprintf(stderr, "Error: --location must be an http: or https: URL, got \"%s\".\n", location);
    return false;
  }
  url_state_clear(&url);
  return true;
}

static bool parse_input_type(const ant_cli_t *cli, bool stdin_mode, ant_input_mode_t *out) {
  *out = ANT_INPUT_COMMONJS;
  if (!cli->input_type) return true;

  if (strcmp(cli->input_type, "module") == 0) *out = ANT_INPUT_MODULE;
  else if (strcmp(cli->input_type, "commonjs") != 0) {
    fprintf(stderr, "Error: --input-type must be either \"commonjs\" or \"module\".\n");
    return false;
  }

  if (!cli->eval && !stdin_mode) {
    fprintf(stderr, "Error: --input-type can only be used with --eval or stdin.\n");
    return false;
  }

  return true;
}

static run_mode_t pick_mode(const ant_cli_t *cli, const run_plan_t *plan, bool repl_mode, bool stdin_mode) {
  if (cli->sandbox_daemon) return RUN_SANDBOX;
  if (plan->pkg.argv) return RUN_PKG;
  if (cli->eval) return RUN_EVAL;
  if (repl_mode) return RUN_REPL;
  if (stdin_mode) return RUN_STDIN;
  return RUN_FILES;
}

static bool plan_run(const ant_cli_t *cli, run_plan_t *plan, int *exit_code) {
  *exit_code = EXIT_FAILURE;
  plan->script_tail = cli->script_tail;

  if (cli->sandbox_daemon && !ant_sandbox_read_request_transport(&plan->sandbox)) return false;
  if (ant_cli_route(cli, &plan->pkg, exit_code) == ANT_ROUTE_EXIT) return false;

  bool has_stdin = !isatty(STDIN_FILENO);
  bool explicit_stdin = cli->file_count == 1 && strcmp(cli->files[0], "-") == 0;
  bool repl_mode = cli->repl || (cli->file_count == 0 && !cli->eval && !has_stdin);
  bool stdin_mode = explicit_stdin || (has_stdin && cli->file_count == 0);

  if (cli->repl && (cli->eval || cli->file_count > 0)) {
    fprintf(stderr, "Error: --repl cannot be combined with --eval or a script file.\n");
    return false;
  }

  if ((cli->watch || cli->hot) && (cli->eval || repl_mode || stdin_mode)) {
    crfprintf(stderr, msg.watch_module_error);
    return false;
  }

  ant_input_mode_t input_mode;
  if (!check_location(cli->location) || !parse_input_type(cli, stdin_mode, &input_mode)) return false;

  plan->mode = pick_mode(cli, plan, repl_mode, stdin_mode);
  plan->input = input_mode;
  bool no_module = plan->pkg.argv || repl_mode || stdin_mode || cli->file_count == 0;
  plan->module_file = no_module ? NULL : cli->files[0];

  if (plan->mode == RUN_SANDBOX) {
    if (!ant_cli_daemon_prepare(&plan->sandbox)) return false;
    bool run = plan->sandbox.mode == ANT_SANDBOX_REQUEST_RUN;
    plan->module_file = run ? plan->sandbox.entry : NULL;
    if (run) plan->script_tail = (ant_argv_t){ plan->sandbox.argc, plan->sandbox.argv };
  }

  return true;
}

static ant_argv_t process_argv(const ant_cli_t *cli, const run_plan_t *plan) {
  const char *argv0 = cli->argv[0];
  switch (plan->mode) {
  case RUN_PKG:
    return (ant_argv_t){ plan->pkg.argc, plan->pkg.argv };
  case RUN_EVAL:
  case RUN_STDIN:
    return ant_entry_process_argv(argv0, cli->file_count > 0 ? cli->files[0] : NULL, false, plan->script_tail);
  case RUN_REPL:
    return ant_entry_process_argv(argv0, NULL, false, (ant_argv_t){ 0, NULL });
  case RUN_FILES:
  case RUN_SANDBOX:
    break;
  }
  return ant_entry_process_argv(argv0, plan->module_file, true, plan->script_tail);
}

static bool start_inspector(ant_t *js, const ant_cli_t *cli, const run_plan_t *plan) {
  if (!ant_inspector_start(js, &cli->inspector)) {
    fprintf(stderr, "Unable to start inspector on %s:%d\n", cli->inspector.host, cli->inspector.port);
    return false;
  }

  if (plan->mode == RUN_EVAL) ant_inspector_register_script_source("[eval]", cli->eval, strlen(cli->eval), false);
  if (plan->mode == RUN_FILES) for (int i = 0; i < cli->file_count; i++) {
    char *resolved = resolve_js_file(cli->files[i]);
    if (!resolved) continue;
    ant_inspector_register_script_file(resolved, true);
    free(resolved);
  }

  if (cli->inspector.wait_for_session) ant_inspector_wait_for_session();
  return true;
}

static int run_mode(ant_t *js, const ant_cli_t *cli, const run_plan_t *plan, ant_watch_t *watch) {
  switch (plan->mode) {
  case RUN_PKG: {
    int code = pkg_run(js);
    if (code == EXIT_SUCCESS) js_run_event_loop(js);
    return code;
  }
  case RUN_EVAL:
    return ant_entry_eval(js, cli->eval, strlen(cli->eval), "[eval]", cli->print, plan->input);
  case RUN_REPL:
    ant_repl_run(js, cli->repl_script);
    return EXIT_SUCCESS;
  case RUN_STDIN:
    return ant_entry_stdin(js, cli->print, plan->input, cli->inspector.enabled);
  case RUN_FILES:
    if (ant_watch_hot(watch)) return ant_entry_files_hot(js, watch, cli->files, cli->file_count);
    return ant_entry_files(js, cli->files, cli->file_count, cli->cron_period);
  case RUN_SANDBOX:
    break;
  }
  return EXIT_FAILURE;
}

static int run_in_runtime(ant_t *js, const ant_cli_t *cli, run_plan_t *plan, bool crash_report, ant_watch_t *watch) {
  if (plan->mode == RUN_SANDBOX) return ant_cli_daemon_run(js, &plan->sandbox, cli->original_argv[0]);
  if (cli->inspector.enabled && !start_inspector(js, cli, plan)) return EXIT_FAILURE;
  if (crash_report) return ant_crash_run_internal_report(js);

  int code = plan->mode == RUN_PKG ? EXIT_SUCCESS : ant_preload_run(js, cli->preload, cli->preload_count);
  if (code == EXIT_SUCCESS) code = run_mode(js, cli, plan, watch);
  
  return process_run_exit_handlers(js, code);
}

static int run_runtime(const ant_cli_t *cli, run_plan_t *plan, bool crash_report, ant_watch_t *watch) {
  volatile char stack_base;
  ant_t *js = ant_create(nullptr);
  
  if (!js) {
    crfprintf(stderr, msg.ant_allocation_fatal);
    return EXIT_FAILURE;
  }

  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, os_thread_stack_size() * 3 / 4);
  if (cli->jitless) sv_debug_enable(SV_DEBUG_JITLESS);

  ant_argv_t proc = process_argv(cli, plan);
  if (plan->mode == RUN_PKG) plan->pkg = (pkg_argv_t){ 0, NULL };

  if (plan->mode == RUN_SANDBOX) ant_sandbox_set_guest_process(true);
  ant_runtime_init(js, proc.argc, proc.argv, cli->localstorage_file);

  if (cli->web_compat) js->runtime.flags |= ANT_RUNTIME_WEB_COMPAT;
  if (cli->location) js->runtime.location = cli->location;
  if (plan->mode == RUN_SANDBOX) ant_cli_daemon_terminal(js, &plan->sandbox);

  ant_bootstrap_modules(js);
  ant_register_library(sandbox_library, "ant:sandbox");

  if (watch) ant_watch_attach(watch, js);
  int code = run_in_runtime(js, cli, plan, crash_report, watch);

  if (watch) {
    ant_watch_detach(watch, js);
    ant_inspector_stop();
  }

  js->process_exiting = true;
  js_destroy(js);
  free(proc.argv);
  
  return code;
}

static int run_watch(const ant_cli_t *cli, run_plan_t *plan) {
  char *resolved = resolve_js_file(plan->module_file);
  const char *entry = resolved ? resolved : plan->module_file;

  if (!entry || esm_is_url(entry)) {
    crfprintf(stderr, msg.watch_entrypoint_error);
    free(resolved);
    return EXIT_FAILURE;
  }

  ant_watch_t *watch = ant_watch_open(entry, cli->hot, !cli->no_clear_screen);
  free(resolved);
  if (!watch) return EXIT_FAILURE;

  do run_runtime(cli, plan, false, watch);
  while (!ant_watch_hot(watch) && ant_watch_wait(watch));

  int code = ant_watch_shutdown_code(watch);
  ant_watch_close(watch);
  return code;
}

static int run_cli(const ant_cli_t *cli, bool crash_report) {
  run_plan_t plan = { 0 };
  int code = EXIT_FAILURE;

  if (plan_run(cli, &plan, &code))
    code = cli->watch || cli->hot ? run_watch(cli, &plan) : run_runtime(cli, &plan, crash_report, NULL);

  pkg_argv_free(&plan.pkg);
  ant_sandbox_request_free(&plan.sandbox);
  
  return code;
}

static int ant_main(int argc, char *argv[]) {
  bool crash_report = ant_crash_is_internal_report(argc, argv);
  if (crash_report) argc = 1;
  ant_cli_process_init(argc, argv, crash_report);

  ant_cli_t cli;
  int code = EXIT_SUCCESS;
  
  const char *env_options = crash_report ? NULL : getenv(ANT_OPTIONS_ENV);
  if (ant_cli_parse(&cli, argc, argv, env_options, &code)) code = run_cli(&cli, crash_report);

  ant_cli_free(&cli);
  return code;
}

int main(int argc, char *argv[]) {
  if (ant_sandbox_vm_helper_is_process(argv[0])) return ant_sandbox_vm_helper_process_main();
  return os_run_main(ant_main, argc, argv);
}
