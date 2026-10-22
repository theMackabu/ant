#include <compat.h> // IWYU pragma: keep

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crprintf.h>

#include "utils.h"
#include "reactor.h"
#include "runtime.h"
#include "messages.h"
#include "modules/io.h"
#include "modules/tty.h"
#include "modules/process.h"
#include "sandbox/policy.h"
#include "sandbox/transport.h"
#include "cli/daemon.h"
#include "cli/entry.h"

static bool enter_cwd(const ant_sandbox_request_t *request) {
  if (!request->cwd || chdir(request->cwd) == 0) return true;
  fprintf(stderr, "sandbox daemon: failed to chdir to %s: %s\n", request->cwd, strerror(errno));
  return false;
}

bool ant_cli_daemon_prepare(const ant_sandbox_request_t *request) {
  if (!enter_cwd(request)) return false;
  if (ant_sandbox_transport_install_output_frames()) return true;
  fprintf(stderr, "sandbox daemon: failed to install framed output transport: %s\n", strerror(errno));
  return false;
}

void ant_cli_daemon_terminal(ant_t *js, const ant_sandbox_request_t *request) {
  io_set_sandbox_terminal(request->capabilities);
  process_set_sandbox_terminal(js, request->capabilities, request->tty_rows, request->tty_cols);
  tty_set_sandbox_terminal(request->capabilities, request->tty_rows, request->tty_cols);
  ant_sandbox_policy_set_forwards(request->forward_ports, request->forward_count);
}

static char **request_argv(const char *argv0, const ant_sandbox_request_t *request, int *argc_out) {
  bool run = request->mode == ANT_SANDBOX_REQUEST_RUN;
  int argc = run ? 2 + request->argc : 1;

  char **argv = try_oom(sizeof(*argv) * (size_t)(argc + 1));
  argv[0] = (char *)argv0;
  if (run) {
    argv[1] = request->entry;
    for (int i = 0; i < request->argc; i++) argv[2 + i] = request->argv[i];
  }

  argv[argc] = NULL;
  *argc_out = argc;
  return argv;
}

static int run_request_entry(ant_t *js, const ant_sandbox_request_t *request) {
  if (request->mode == ANT_SANDBOX_REQUEST_EVAL)
    return ant_sandbox_eval_module(js, request->source, strlen(request->source));

  if (request->mode != ANT_SANDBOX_REQUEST_RUN) {
    fprintf(stderr, "sandbox daemon: unsupported request mode\n");
    return EXIT_FAILURE;
  }

  char *resolved = resolve_js_file(request->entry);
  if (!resolved) {
    crfprintf(stderr, msg.module_not_found, request->entry);
    return EXIT_FAILURE;
  }

  int code = ant_entry_module(js, resolved, NULL);
  js_run_event_loop(js);
  free(resolved);
  return code;
}

static int run_request(ant_t *js, const ant_sandbox_request_t *request, const char *argv0) {
  int argc = 0;
  char **argv = request_argv(argv0, request, &argc);

  ant_runtime_set_argv(js, argc, argv);
  process_refresh_sandbox_argv(js);

  int code = EXIT_FAILURE;
  if (enter_cwd(request)) {
    ant_cli_daemon_terminal(js, request);
    code = run_request_entry(js, request);
  }

  free(argv);
  return code;
}

int ant_cli_daemon_run(ant_t *js, ant_sandbox_request_t *request, const char *argv0) {
  for (;;) {
    bool close = request->mode == ANT_SANDBOX_REQUEST_CLOSE;
    int code = close ? EXIT_SUCCESS : run_request(js, request, argv0);

    ant_sandbox_transport_send_exit(code);
    ant_sandbox_request_free(request);
    memset(request, 0, sizeof(*request));

    if (close) return code;
    if (!ant_sandbox_read_request_transport(request)) return EXIT_FAILURE;
  }
}
