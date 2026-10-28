#include <compat.h> // IWYU pragma: keep

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crprintf.h>

#include "gc.h"
#include "gc/trace.h"
#include "crash.h"
#include "debug.h"
#include "messages.h"
#include "modules/io.h"
#include "cli/compile.h"
#include "cli/startup.h"
#include "../sandbox/assets.h"

#ifdef _WIN32
#include "output.h"
#endif

static void ant_debug_apply(const char *key, const char *val) {
  if (strcmp(key, "gc") == 0) {
    if (strcmp(val, "disable") == 0) gc_disabled = true;
    if (strcmp(val, "trace") == 0) gc_trace_enabled = true;
  }

  else if (strcmp(key, "dump/parse") == 0) {
    if (strcmp(val, "trace") == 0) sv_debug_enable(SV_DEBUG_PARSE);
  }

  else if (strcmp(key, "dump/compile") == 0) {
    if (strcmp(val, "trace") == 0) sv_debug_enable(SV_DEBUG_COMPILE);
    if (strcmp(val, "shell") == 0) sv_debug_enable(SV_DEBUG_DUMP_SHELL);
  }

  else if (strcmp(key, "dump/crprintf") == 0) {
    if (strcmp(val, "bytecode") == 0 || strcmp(val, "all") == 0) crprintf_set_debug(true);
    if (strcmp(val, "hex") == 0      || strcmp(val, "all") == 0) crprintf_set_debug_hex(true);
  }

  else if (strcmp(key, "dump/vm") == 0) {
    if (strcmp(val, "bytecode") == 0 || strcmp(val, "all") == 0) sv_debug_enable(SV_DEBUG_DUMP_BC);
    if (strcmp(val, "jit") == 0      || strcmp(val, "all") == 0) sv_debug_enable(SV_DEBUG_DUMP_JIT);
    if (strcmp(val, "op-warn") == 0  || strcmp(val, "all") == 0) sv_debug_enable(SV_DEBUG_JIT_WARN);
  }

  else if (strcmp(key, "dump/opt") == 0) {
    if (strcmp(val, "graph") == 0 || strcmp(val, "all") == 0) sv_debug_enable(SV_DEBUG_DUMP_OPT_GRAPH);
    if (strcmp(val, "stats") == 0 || strcmp(val, "all") == 0) sv_debug_enable(SV_DEBUG_DUMP_OPT_STATS);
  }

  else if (strcmp(key, "jit") == 0) {
    if (strcmp(val, "perf-map") == 0) sv_debug_enable(SV_DEBUG_PERF_MAP);
  }

  else if (strcmp(key, "shape") == 0) {
    if (strcmp(val, "verify") == 0) sv_debug_enable(SV_DEBUG_SHAPE_VERIFY);
  }

  else if (strcmp(key, "opt") == 0) {
    if (strcmp(val, "off") == 0) sv_debug_enable(SV_DEBUG_OPT_OFF);
    if (strcmp(val, "force") == 0) sv_debug_enable(SV_DEBUG_OPT_FORCE);
    if (strcmp(val, "verify") == 0) sv_debug_enable(SV_DEBUG_OPT_VERIFY);
  }

  else if (strcmp(key, "sandbox") == 0) {
    if (strcmp(val, "bypass-manifest") == 0) ant_sandbox_assets_bypass_manifest = true;
  }

  else if (strcmp(key, "compile") == 0) {
    if (strcmp(val, "bypass-abi") == 0) ant_compile_bypass_revision = true;
  }
}

static void parse_ant_debug_flags(void) {
  const char *env = getenv("ANT_DEBUG");
  if (!env || !*env) return;

  char *buf = strdup(env);
  if (!buf) return;

  char *sp = NULL, *vp = NULL;
  for (char *e = strtok_r(buf, " ", &sp); e; e = strtok_r(NULL, " ", &sp)) {
    char *sep = strchr(e, ':');
    if (!sep) { 
      crfprintf(stderr, msg.unknown_flag_warn, e); 
      continue; 
    }
    
    *sep++ = '\0';
    for (char *v = strtok_r(sep, ",", &vp); v; v = strtok_r(NULL, ",", &vp))
      ant_debug_apply(e, v);
  }

  free(buf);
}

static void setup_console_colors(void) {
  crprintf_var("version", ANT_VERSION);
  crprintf_var("fatal", "<bold+red>FATAL</bold>");
  crprintf_var("error", "<red>Error</red>");
  crprintf_var("warn", "<yellow>Warning</yellow>");
}

#ifdef ANT_PGO_TRAINING
extern int __llvm_profile_write_file(void);

static void pgo_flush_on_signal(int sig) {
  __llvm_profile_write_file();
  signal(sig, SIG_DFL);
  raise(sig);
}

static void pgo_ignore_sigterm_on_exit(void) {
  signal(SIGTERM, SIG_IGN);
}

static void pgo_install_flush_handler(void) {
  struct sigaction sa = {0};
  sa.sa_handler = pgo_flush_on_signal;
  sa.sa_flags = SA_RESTART;
  sigfillset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  atexit(pgo_ignore_sigterm_on_exit);
}
#endif

void ant_cli_process_init(int argc, char **argv, bool crash_report) {
  if (!crash_report && !getenv("ANT_NO_CRASH_HANDLER")) ant_crash_init(argc, argv);

  #ifdef _WIN32
  ant_output_init_console();
  #else
  signal(SIGPIPE, SIG_IGN);
  #ifdef ANT_PGO_TRAINING
  pgo_install_flush_handler();
  #endif
  #endif

  setup_console_colors();
  crprintf_set_color(io_should_color_fd(STDERR_FILENO));
  parse_ant_debug_flags();
}
