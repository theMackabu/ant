#include "watch.h"
#include "messages.h"

#include <uv.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <crprintf.h>

#ifdef _WIN32
#include <direct.h>
#endif

#ifndef SIGKILL
#define SIGKILL SIGTERM
#endif

typedef struct watch_state {
  uv_loop_t loop;
  uv_fs_event_t fs_event;
  uv_signal_t sigint;
  uv_signal_t sigterm;
  uv_timer_t kill_timer;
  uv_process_t *child;
  char **child_argv;
  char *watch_path;
  bool no_clear_screen;
  bool stop_requested;
  bool restart_pending;
  bool child_running;
  bool fs_event_inited;
  bool sigint_inited;
  bool sigterm_inited;
  bool kill_timer_inited;
  bool kill_timer_running;
  int final_exit_code;
} watch_state_t;

static void watch_clear_screen(void) {
  fputs("\033[3J\033[2J\033[H", stdout);
  fflush(stdout);
}

static char *watch_resolve_path(const char *path) {
#ifdef _WIN32
  char *resolved = _fullpath(NULL, path, 0);
  if (resolved) return resolved;
  return _strdup(path);
#else
  char abs_path[PATH_MAX];
  const char *resolved = realpath(path, abs_path);
  return strdup(resolved ? resolved : path);
#endif
}

static char **watch_build_child_argv(int argc, char **argv) {
  char **child_argv = calloc((size_t)argc + 1, sizeof(char *));
  if (!child_argv) return NULL;

  int out = 0;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--watch") == 0) continue;
    if (strcmp(argv[i], "--no-clear-screen") == 0) continue;
    child_argv[out++] = argv[i];
  }

  child_argv[out] = NULL;
  if (out == 0) {
    free(child_argv);
    return NULL;
  }

  return child_argv;
}

static void watch_stop_kill_timer(watch_state_t *state) {
  if (!state->kill_timer_inited || !state->kill_timer_running) return;
  uv_timer_stop(&state->kill_timer);
  state->kill_timer_running = false;
}

static void watch_on_child_closed(uv_handle_t *handle) { free(handle); }
static void watch_request_shutdown(watch_state_t *state, int exit_code);
static void watch_try_spawn_child(watch_state_t *state);

static void watch_on_kill_timer(uv_timer_t *timer) {
  watch_state_t *state = (watch_state_t *)timer->data;
  state->kill_timer_running = false;

  if (state->child_running && state->child) 
    uv_process_kill(state->child, SIGKILL);
  if (state->stop_requested && !uv_is_closing((uv_handle_t *)&state->kill_timer)) 
    uv_close((uv_handle_t *)&state->kill_timer, NULL);
}

static void watch_on_child_exit(uv_process_t *proc, int64_t exit_status, int term_signal) {
  watch_state_t *state = (watch_state_t *)proc->data;
  state->child_running = false;
  state->child = NULL;
  watch_stop_kill_timer(state);

  if (state->stop_requested) {
    if (state->kill_timer_inited && !uv_is_closing((uv_handle_t *)&state->kill_timer)) 
      uv_close((uv_handle_t *)&state->kill_timer, NULL);
  } else if (state->restart_pending) {
    watch_try_spawn_child(state);
  } else if (term_signal > 0) {
    state->final_exit_code = 128 + term_signal;
  } else state->final_exit_code = (int)exit_status;

  uv_close((uv_handle_t *)proc, watch_on_child_closed);
}

static int watch_spawn_child(watch_state_t *state) {
  uv_process_t *proc = calloc(1, sizeof(*proc));
  if (!proc) return UV_ENOMEM;

  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_INHERIT_FD; stdio[0].data.fd = 0;
  stdio[1].flags = UV_INHERIT_FD; stdio[1].data.fd = 1;
  stdio[2].flags = UV_INHERIT_FD; stdio[2].data.fd = 2;

  uv_process_options_t options = {0};
  options.file = state->child_argv[0];
  options.args = state->child_argv;
  options.exit_cb = watch_on_child_exit;
  options.stdio_count = 3;
  options.stdio = stdio;

  int rc = uv_spawn(&state->loop, proc, &options);
  if (rc != 0) {
    free(proc);
    return rc;
  }

  proc->data = state;
  state->child = proc;
  state->child_running = true;
  state->restart_pending = false;
  return 0;
}

static void watch_try_spawn_child(watch_state_t *state) {
  if (state->stop_requested) return;
  if (!state->no_clear_screen && state->restart_pending) watch_clear_screen();
  int rc = watch_spawn_child(state);
  
  if (rc != 0) {
    crfprintf(stderr, msg.watch_spawn_failed, uv_strerror(rc));
    watch_request_shutdown(state, EXIT_FAILURE);
  }
}

static void watch_start_kill_timer(watch_state_t *state, uint64_t timeout_ms) {
  if (!state->kill_timer_inited || state->kill_timer_running) return;
  int rc = uv_timer_start(&state->kill_timer, watch_on_kill_timer, timeout_ms, 0);
  if (rc == 0) state->kill_timer_running = true;
}

static void watch_close_signals_and_watcher(watch_state_t *state) {
  if (state->fs_event_inited && !uv_is_closing((uv_handle_t *)&state->fs_event)) {
    uv_fs_event_stop(&state->fs_event);
    uv_close((uv_handle_t *)&state->fs_event, NULL);
  }

  if (state->sigint_inited && !uv_is_closing((uv_handle_t *)&state->sigint)) {
    uv_signal_stop(&state->sigint);
    uv_close((uv_handle_t *)&state->sigint, NULL);
  }

  if (state->sigterm_inited && !uv_is_closing((uv_handle_t *)&state->sigterm)) {
    uv_signal_stop(&state->sigterm);
    uv_close((uv_handle_t *)&state->sigterm, NULL);
  }
}

static void watch_request_shutdown(watch_state_t *state, int exit_code) {
  if (state->stop_requested) return;

  state->stop_requested = true;
  state->restart_pending = false;
  state->final_exit_code = exit_code;

  watch_close_signals_and_watcher(state);

  if (state->child_running && state->child) {
    int rc = uv_process_kill(state->child, SIGTERM);
    if (rc != 0 && rc != UV_ESRCH) {
      crfprintf(stderr, msg.watch_graceful_term, uv_strerror(rc));
    }
    watch_start_kill_timer(state, 1000);
  } else if (state->kill_timer_inited && !uv_is_closing((uv_handle_t *)&state->kill_timer)) {
    uv_close((uv_handle_t *)&state->kill_timer, NULL);
  }
}

static void watch_request_restart(watch_state_t *state) {
  if (state->stop_requested) return;
  if (!state->restart_pending) {}
  state->restart_pending = true;

  if (state->child_running && state->child) {
    int rc = uv_process_kill(state->child, SIGTERM);
    if (rc != 0 && rc != UV_ESRCH) {
      crfprintf(stderr, msg.watch_child_error, uv_strerror(rc));
      watch_request_shutdown(state, EXIT_FAILURE);
      return;
    }
    watch_start_kill_timer(state, 1000);
  } else watch_try_spawn_child(state);
}

static void watch_on_fs_event(uv_fs_event_t *handle, const char *filename, int events, int status) {
  watch_state_t *state = (watch_state_t *)handle->data;

  if (status < 0) {
    crfprintf(stderr, msg.watch_warn_normal, state->watch_path, uv_strerror(status));
    return;
  }

  if ((events & (UV_CHANGE | UV_RENAME)) == 0) return;
  watch_request_restart(state);
}

static void watch_on_signal(uv_signal_t *handle, int signum) {
  watch_state_t *state = (watch_state_t *)handle->data;
  watch_request_shutdown(state, 128 + signum);
}

int ant_watch_run(int argc, char **argv, const char *entry_file, bool no_clear_screen) {
  if (!entry_file || !*entry_file) {
    crfprintf(stderr, msg.watch_entrypoint_missing);
    return EXIT_FAILURE;
  }

  watch_state_t state = {0};
  state.child_argv = watch_build_child_argv(argc, argv);
  state.watch_path = watch_resolve_path(entry_file);
  state.no_clear_screen = no_clear_screen;
  state.final_exit_code = EXIT_SUCCESS;

  if (!state.child_argv || !state.watch_path) {
    free(state.child_argv);
    free(state.watch_path);
    crfprintf(stderr, msg.watch_start_fatal);
    return EXIT_FAILURE;
  }

  int rc = uv_loop_init(&state.loop);
  if (rc != 0) {
    crfprintf(stderr, msg.watch_loop_fatal, uv_strerror(rc));
    free(state.child_argv); free(state.watch_path);
    return EXIT_FAILURE;
  }

  rc = uv_fs_event_init(&state.loop, &state.fs_event);
  if (rc == 0) state.fs_event_inited = true;
  
  if (rc == 0) {
    state.fs_event.data = &state;
    rc = uv_fs_event_start(&state.fs_event, watch_on_fs_event, state.watch_path, 0);
  }
  
  if (rc != 0) {
    crfprintf(stderr, msg.watch_file_failed, state.watch_path, uv_strerror(rc));
    watch_request_shutdown(&state, EXIT_FAILURE);
  }

  if (rc == 0) {
    rc = uv_signal_init(&state.loop, &state.sigint);
    if (rc == 0) state.sigint_inited = true;
    if (rc == 0) {
      state.sigint.data = &state;
      rc = uv_signal_start(&state.sigint, watch_on_signal, SIGINT);
    }
  }
  
  if (rc == 0) {
    rc = uv_signal_init(&state.loop, &state.sigterm);
    if (rc == 0) state.sigterm_inited = true;
    if (rc == 0) {
      state.sigterm.data = &state;
      int sigterm_rc = uv_signal_start(&state.sigterm, watch_on_signal, SIGTERM);
      if (sigterm_rc != 0 && sigterm_rc != UV_ENOSYS && sigterm_rc != UV_EINVAL) {
        rc = sigterm_rc;
      } else if (sigterm_rc != 0) {
        uv_signal_stop(&state.sigterm);
        uv_close((uv_handle_t *)&state.sigterm, NULL);
        state.sigterm_inited = false;
      }
    }
  }
  
  if (rc == 0) {
    rc = uv_timer_init(&state.loop, &state.kill_timer);
    if (rc == 0) {
      state.kill_timer_inited = true;
      state.kill_timer.data = &state;
    }
  }
  
  if (rc != 0) {
    crfprintf(stderr, msg.watch_loop_handles_fatal, uv_strerror(rc));
    watch_request_shutdown(&state, EXIT_FAILURE);
  }

  if (!state.stop_requested) watch_try_spawn_child(&state);
  while (uv_run(&state.loop, UV_RUN_DEFAULT) != 0) {}

  if (state.fs_event_inited && !uv_is_closing((uv_handle_t *)&state.fs_event)) {
    uv_close((uv_handle_t *)&state.fs_event, NULL);
  }
  if (state.sigint_inited && !uv_is_closing((uv_handle_t *)&state.sigint)) {
    uv_close((uv_handle_t *)&state.sigint, NULL);
  }
  if (state.sigterm_inited && !uv_is_closing((uv_handle_t *)&state.sigterm)) {
    uv_close((uv_handle_t *)&state.sigterm, NULL);
  }
  if (state.kill_timer_inited && !uv_is_closing((uv_handle_t *)&state.kill_timer)) {
    uv_close((uv_handle_t *)&state.kill_timer, NULL);
  }
  while (uv_run(&state.loop, UV_RUN_DEFAULT) != 0) {}

  rc = uv_loop_close(&state.loop);
  if (rc != 0) crfprintf(stderr, msg.watch_loop_cleanup, uv_strerror(rc));

  free(state.child_argv);
  free(state.watch_path);
  return state.final_exit_code;
}
