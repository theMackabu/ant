#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <crprintf.h>

#include "ant.h"
#include "crash.h"
#include "errors.h"
#include "bootstrap.h"
#include "internal.h"
#include "reactor.h"
#include "runtime.h"
#include "utils.h"
#include "pack.h"
#include "vfs_bundle.h"
#include "silver/vm.h"
#include "esm/loader.h"
#include "modules/server.h"

static int run_bundle_entry(ant_t *js, const ant_bundle_module_t *entry) {
  const char *key = entry->key;
  const char *slash = strrchr(key, '/');
  
  if (slash && slash != key) {
    js_set_global_builtin(js, "__dirname", js_mkstr(js, key, (size_t)(slash - key)));
  } else js_set_global_builtin(js, "__dirname", js_mkundef());
  js_set_global_builtin(js, "__filename", js_mkstr(js, key, strlen(key)));

  const char *stable_key = intern_string(key, strlen(key));
  if (!stable_key) stable_key = key;

  js_set_filename(js, stable_key);
  js_setup_import_meta(js, stable_key);

  ant_value_t ns = js_esm_import_sync_cstr(js, key, strlen(key));
  if (print_uncaught_throw(js)) return EXIT_FAILURE;

  if (vtype(ns) == T_ERR) {
    fprintf(stderr, "%s\n", js_str(js, ns));
    return EXIT_FAILURE;
  }

  ant_value_t default_export = js_get(js, ns, "default");
  return server_maybe_start_from_export(js, default_export);
}

int main(int argc, char *argv[]) {
  bool internal_crash_report_mode = ant_crash_is_internal_report(argc, argv);

  if (internal_crash_report_mode) argc = 1;
  if (!internal_crash_report_mode && !getenv("ANT_NO_CRASH_HANDLER")) ant_crash_init(argc, argv);

  char run_key[4096] = {0};
  const char *run_env = getenv(ANT_INTERNAL_RUN_ENV);
  if (run_env && run_env[0]) {
    snprintf(run_key, sizeof(run_key), "%s", run_env);
    #ifdef _WIN32
    _putenv(ANT_INTERNAL_RUN_ENV "=");
    #else
    unsetenv(ANT_INTERNAL_RUN_ENV);
    #endif
  }

  #ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
  #endif

  crprintf_var("version", ANT_VERSION);
  crprintf_var("fatal", "<bold+red>FATAL</bold>");
  crprintf_var("error", "<red>Error</red>");
  crprintf_var("warn", "<yellow>Warning</yellow>");

  static char exe_path[4096];
  if (ant_get_exe_path(exe_path, sizeof(exe_path), argc, argv) != 0) {
    fprintf(stderr, "ant-runtime: unable to determine executable path\n");
    return EXIT_FAILURE;
  }

  const char *debug_env = getenv("ANT_DEBUG");
  bool bypass_abi = debug_env && strstr(debug_env, "compile:bypass-abi");
  if (bypass_abi) fprintf(stderr, "ant-runtime: compile:bypass-abi active; skipping revision check\n");

  const char *packed_exe = getenv(ANT_PACK_ENV_EXE);
  bool packed = packed_exe && packed_exe[0];
  const char *bundle_path = exe_path;

  #ifdef __linux__
  if (packed) bundle_path = "/proc/self/exe";
  #endif

  ant_bundle_t bundle;
  ant_bundle_status_t status = ant_bundle_open(bundle_path, bypass_abi ? NULL : ANT_GIT_LONGHASH, &bundle);

  if (status == ANT_BUNDLE_ERR_NO_TRAILER) {
    fprintf(stderr,
      "ant-runtime: no embedded program found.\n"
      "This binary is a runtime stub; standalone executables are produced with `ant compile <entry>`.\n");
    return EXIT_FAILURE;
  }
  
  if (status == ANT_BUNDLE_ERR_ABI) {
    fprintf(stderr,
      "ant-runtime: %s\n  runtime revision: %s\n  program revision: %s\nRecompile with a matching ant (`ant compile <entry>`).\n",
      ant_bundle_status_str(status), ANT_GIT_LONGHASH, bundle.abi_hash);
    return EXIT_FAILURE;
  }
  
  if (status != ANT_BUNDLE_OK) {
    fprintf(stderr, "ant-runtime: %s (%s)\n", ant_bundle_status_str(status), bundle_path);
    return EXIT_FAILURE;
  }

  const ant_bundle_module_t *entry_mod;
  if (run_key[0]) {
    entry_mod = ant_bundle_get(&bundle, run_key);
    if (!entry_mod) {
      fprintf(stderr, "ant-runtime: module %s is not embedded in this executable\n", run_key);
      ant_bundle_close(&bundle);
      return EXIT_FAILURE;
    }
  } else entry_mod = ant_bundle_entry(&bundle);

  int user_argc = argc > 1 ? argc - 1 : 0;
  int proc_argc = 2 + user_argc;

  if (packed) {
    #ifdef _WIN32
    _putenv(ANT_PACK_ENV_EXE "=");
    #else
    unsetenv(ANT_PACK_ENV_EXE);
    #endif
  }

  char **proc_argv = try_oom(sizeof(char *) * (size_t)(proc_argc + 1));
  proc_argv[0] = packed ? (char *)packed_exe : exe_path;
  proc_argv[1] = (char *)entry_mod->key;
  
  for (int i = 0; i < user_argc; i++) proc_argv[2 + i] = argv[1 + i];
  proc_argv[proc_argc] = NULL;

  ant_t *js;
  volatile char stack_base;

  if (!(js = ant_create())) {
    fprintf(stderr, "ant-runtime: failed to allocate runtime\n");
    free(proc_argv);
    ant_bundle_close(&bundle);
    return EXIT_FAILURE;
  }

  js_setstackbase(js, (void *)&stack_base);
  js_setstacklimit(js, os_thread_stack_size() * 3 / 4);

  ant_runtime_init(js, proc_argc, proc_argv, NULL);
  ant_bootstrap_modules(js);

  if (!js_esm_bundle_activate(js, &bundle)) {
    fprintf(stderr, "ant-runtime: failed to load embedded program\n");
    js_destroy(js);
    ant_bundle_close(&bundle);
    free(proc_argv);
    return EXIT_FAILURE;
  }

  int js_result;
  if (internal_crash_report_mode) {
    js_result = ant_crash_run_internal_report(js);
  } else {
    js_result = run_bundle_entry(js, entry_mod);
    js_run_event_loop(js);
  }

  js_destroy(js);
  ant_bundle_close(&bundle);
  free(proc_argv);

  return js_result;
}
