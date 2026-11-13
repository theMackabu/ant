#include <compat.h> // IWYU pragma: keep
#include "protocol.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/resource.h>
#endif

extern char **environ;

static void sandbox_vm_verbose_helper_pid(const ant_sandbox_vm_config_t *config, pid_t pid) {
  if (!config || !config->verbose) return;
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    fprintf(stderr, "[0.000000] ant: sandbox helper pid=%ld\n", (long)pid);
    return;
  }
  fprintf(stderr, "[%lld.%06ld] ant: sandbox helper pid=%ld\n",
          (long long)ts.tv_sec,
          ts.tv_nsec / 1000,
          (long)pid);
}

static void sandbox_vm_verbose_session(ant_sandbox_vm_session_t *session, const char *message) {
  if (!session || !session->verbose) return;
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    fprintf(stderr, "[0.000000] ant: %s\n", message);
    return;
  }
  fprintf(stderr, "[%lld.%06ld] ant: %s\n",
          (long long)ts.tv_sec,
          ts.tv_nsec / 1000,
          message);
}

static bool sandbox_vm_wait_pid(pid_t pid, unsigned int timeout_ms) {
  struct timespec start;
  bool have_start = clock_gettime(CLOCK_MONOTONIC, &start) == 0;

  for (;;) {
    int status = 0;
    pid_t rc = waitpid(pid, &status, WNOHANG);
    if (rc == pid) return true;
    if (rc < 0) return errno == ECHILD;
    if (timeout_ms == 0) return false;

    if (have_start) {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        uint64_t elapsed_ms = (uint64_t)(now.tv_sec - start.tv_sec) * 1000ull;
        if (now.tv_nsec >= start.tv_nsec) elapsed_ms += (uint64_t)(now.tv_nsec - start.tv_nsec) / 1000000ull;
        else elapsed_ms -= (uint64_t)(start.tv_nsec - now.tv_nsec) / 1000000ull;
        if (elapsed_ms >= timeout_ms) return false;
      }
    }

    usleep(10000);
  }
}

static void sandbox_vm_stop_helper(ant_sandbox_vm_session_t *session, bool graceful) {
  if (!session || session->helper_pid <= 0) return;
  pid_t pid = session->helper_pid;

  if (graceful && sandbox_vm_wait_pid(pid, 1000)) {
    session->helper_pid = -1;
    return;
  }

  sandbox_vm_verbose_session(session, "terminating sandbox helper");
  kill(pid, SIGTERM);
  if (sandbox_vm_wait_pid(pid, 1000)) {
    session->helper_pid = -1;
    return;
  }

  sandbox_vm_verbose_session(session, "force-killing sandbox helper");
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  session->helper_pid = -1;
}

static int sandbox_vm_self_executable(char *path, size_t path_len) {
  if (!path || path_len == 0) return -EINVAL;
#if defined(__APPLE__)
  uint32_t size = (uint32_t)path_len;
  if (_NSGetExecutablePath(path, &size) != 0) return -ENAMETOOLONG;
  return 0;
#elif defined(__linux__)
  ssize_t len = readlink("/proc/self/exe", path, path_len - 1);
  if (len < 0) return -errno;
  if ((size_t)len >= path_len - 1) return -ENAMETOOLONG;
  path[len] = '\0';
  return 0;
#else
  return -ENOSYS;
#endif
}

static void sandbox_vm_set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int sandbox_vm_move_reserved_fd(int *fd) {
  if (!fd || (*fd != ANT_SANDBOX_VM_HELPER_STATS_FD &&
              *fd != ANT_SANDBOX_VM_HELPER_CMD_FD &&
              *fd != ANT_SANDBOX_VM_HELPER_MSG_FD)) return 0;
  int moved = fcntl(*fd, F_DUPFD_CLOEXEC, ANT_SANDBOX_VM_HELPER_MSG_FD + 1);
  if (moved < 0) return -errno;
  close(*fd);
  *fd = moved;
  return 0;
}

static int sandbox_vm_helper_read_result(
  int fd,
  ant_sandbox_vm_result_t *result,
  int *rc_out
) {
  ant_sandbox_vm_helper_header_t header;
  int rc = sandbox_vm_read_full(fd, &header, sizeof(header));
  if (rc != 0) return rc;
  if (header.magic != ANT_SANDBOX_VM_HELPER_MAGIC ||
      header.length != sizeof(ant_sandbox_vm_helper_result_t)) {
    return -EPROTO;
  }

  ant_sandbox_vm_helper_result_t payload;
  rc = sandbox_vm_read_full(fd, &payload, sizeof(payload));
  if (rc != 0) return rc;
  if (result) {
    result->kind = (ant_sandbox_vm_result_kind_t)payload.kind;
    result->code = payload.code;
  }
  if (rc_out) *rc_out = payload.rc;
  return (int)header.type;
}

int ant_sandbox_vm_helper_create(
  const ant_sandbox_vm_backend_t *backend,
  const ant_sandbox_vm_config_t *config,
  ant_sandbox_vm_session_t **session_out
) {
  int to_child[2] = { -1, -1 };
  int from_child[2] = { -1, -1 };
  int stats_child[2] = { -1, -1 };
  if (pipe(to_child) != 0) return -errno;
  if (pipe(from_child) != 0) {
    int rc = -errno;
    close(to_child[0]);
    close(to_child[1]);
    return rc;
  }
  if (pipe(stats_child) != 0) {
    int rc = -errno;
    close(to_child[0]); close(to_child[1]);
    close(from_child[0]); close(from_child[1]);
    return rc;
  }
  int *pipe_fds[] = {
    &to_child[0], &to_child[1], &from_child[0], &from_child[1],
    &stats_child[0], &stats_child[1],
  };
  for (size_t i = 0; i < sizeof(pipe_fds) / sizeof(pipe_fds[0]); i++) {
    int move_rc = sandbox_vm_move_reserved_fd(pipe_fds[i]);
    if (move_rc != 0) {
      close(to_child[0]); close(to_child[1]);
      close(from_child[0]); close(from_child[1]);
      close(stats_child[0]); close(stats_child[1]);
      return move_rc;
    }
  }

  size_t encoded_len = 0;
  uint8_t *encoded = sandbox_helper_encode_config(config, &encoded_len);
  if (!encoded || encoded_len > UINT32_MAX) {
    free(encoded);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    close(stats_child[0]);
    close(stats_child[1]);
    return -ENOMEM;
  }

  char executable[PATH_MAX];
  int rc = sandbox_vm_self_executable(executable, sizeof(executable));
  if (rc != 0) {
    free(encoded);
    close(to_child[0]); close(to_child[1]);
    close(from_child[0]); close(from_child[1]);
    close(stats_child[0]); close(stats_child[1]);
    return rc;
  }

  sandbox_vm_set_cloexec(to_child[0]);
  sandbox_vm_set_cloexec(to_child[1]);
  sandbox_vm_set_cloexec(from_child[0]);
  sandbox_vm_set_cloexec(from_child[1]);
  sandbox_vm_set_cloexec(stats_child[0]);
  sandbox_vm_set_cloexec(stats_child[1]);

  posix_spawn_file_actions_t actions;
  int spawn_rc = posix_spawn_file_actions_init(&actions);
  if (spawn_rc != 0) {
    free(encoded);
    close(to_child[0]); close(to_child[1]);
    close(from_child[0]); close(from_child[1]);
    close(stats_child[0]); close(stats_child[1]);
    return -spawn_rc;
  }
  if (spawn_rc == 0) spawn_rc = posix_spawn_file_actions_adddup2(&actions, to_child[0], ANT_SANDBOX_VM_HELPER_CMD_FD);
  if (spawn_rc == 0) spawn_rc = posix_spawn_file_actions_adddup2(&actions, from_child[1], ANT_SANDBOX_VM_HELPER_MSG_FD);
  if (spawn_rc == 0) spawn_rc = posix_spawn_file_actions_adddup2(&actions, stats_child[1], ANT_SANDBOX_VM_HELPER_STATS_FD);
  int inherited_fds[] = {
    to_child[0], to_child[1], from_child[0], from_child[1],
    stats_child[0], stats_child[1],
  };
  for (size_t i = 0; spawn_rc == 0 && i < sizeof(inherited_fds) / sizeof(inherited_fds[0]); i++)
    spawn_rc = posix_spawn_file_actions_addclose(&actions, inherited_fds[i]);

  posix_spawnattr_t attr;
  bool attr_ready = spawn_rc == 0 && posix_spawnattr_init(&attr) == 0;

  if (attr_ready) {
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    
    if (
      posix_spawnattr_setsigdefault(&attr, &defaults) != 0 || 
      posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF) != 0
    ) {
      posix_spawnattr_destroy(&attr);
      attr_ready = false;
    }
  }

  pid_t pid = -1;
  char *helper_argv[] = { "ant-sandbox-vm-helper", NULL };
  if (spawn_rc == 0)
    spawn_rc = posix_spawn(&pid, executable, &actions, attr_ready ? &attr : NULL, helper_argv, environ);
  if (attr_ready) posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_rc != 0) {
    free(encoded);
    close(to_child[0]); close(to_child[1]);
    close(from_child[0]); close(from_child[1]);
    close(stats_child[0]); close(stats_child[1]);
    return -spawn_rc;
  }

  close(to_child[0]);
  close(from_child[1]);
  close(stats_child[1]);
  sandbox_vm_verbose_helper_pid(config, pid);

  ant_sandbox_vm_session_t *session = calloc(1, sizeof(*session));
  if (!session) {
    free(encoded);
    close(to_child[1]);
    close(from_child[0]);
    close(stats_child[0]);
    kill(pid, SIGTERM);
    if (!sandbox_vm_wait_pid(pid, 1000)) {
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
    }
    return -ENOMEM;
  }

  session->backend = backend;
  session->helper_pid = pid;
  session->helper_cmd_fd = to_child[1];
  session->helper_msg_fd = from_child[0];
  session->helper_stats_fd = stats_child[0];
  session->capabilities = config->capabilities;
  session->verbose = config->verbose;
  session->helper = true;
  if (pthread_mutex_init(&session->helper_cmd_mutex, NULL) != 0) {
    free(encoded);
    close(session->helper_cmd_fd);
    close(session->helper_msg_fd);
    close(session->helper_stats_fd);
    sandbox_vm_stop_helper(session, false);
    free(session);
    return -ENOMEM;
  }
  session->helper_cmd_mutex_init = true;

  rc = sandbox_vm_send_header(
    session->helper_cmd_fd,
    ANT_SANDBOX_VM_HELPER_CMD_CREATE,
    0,
    (uint32_t)encoded_len
  );
  if (rc == 0) rc = sandbox_vm_write_full(session->helper_cmd_fd, encoded, encoded_len);
  free(encoded);
  if (rc != 0) {
    sandbox_vm_stop_helper(session, false);
    close(session->helper_cmd_fd);
    close(session->helper_msg_fd);
    close(session->helper_stats_fd);
    if (session->helper_cmd_mutex_init) pthread_mutex_destroy(&session->helper_cmd_mutex);
    free(session);
    return rc;
  }

  int create_rc = 0;
  int msg_type = sandbox_vm_helper_read_result(session->helper_msg_fd, config->result, &create_rc);
  if (msg_type != ANT_SANDBOX_VM_HELPER_MSG_CREATE_RESULT) {
    close(session->helper_cmd_fd);
    session->helper_cmd_fd = -1;
    close(session->helper_msg_fd);
    session->helper_msg_fd = -1;
    close(session->helper_stats_fd);
    session->helper_stats_fd = -1;
    sandbox_vm_stop_helper(session, true);
    if (session->helper_cmd_mutex_init) pthread_mutex_destroy(&session->helper_cmd_mutex);
    free(session);
    return msg_type < 0 ? msg_type : -EPROTO;
  }
  if (create_rc != 0) {
    close(session->helper_cmd_fd);
    session->helper_cmd_fd = -1;
    close(session->helper_msg_fd);
    session->helper_msg_fd = -1;
    close(session->helper_stats_fd);
    session->helper_stats_fd = -1;
    sandbox_vm_stop_helper(session, true);
    if (session->helper_cmd_mutex_init) pthread_mutex_destroy(&session->helper_cmd_mutex);
    free(session);
    return create_rc;
  }

  *session_out = session;
  return 0;
}

int ant_sandbox_vm_helper_execute(
  ant_sandbox_vm_session_t *session,
  const ant_sandbox_vm_request_t *request
) {
  if (request->request_len > UINT32_MAX) return -E2BIG;
  pthread_mutex_lock(&session->helper_cmd_mutex);
  int cmd_fd = session->helper_cmd_fd;
  int rc = cmd_fd >= 0 ? sandbox_vm_send_header(cmd_fd,
                                  ANT_SANDBOX_VM_HELPER_CMD_EXECUTE,
                                  0,
                                  (uint32_t)request->request_len) : -ECANCELED;
  if (rc == 0) rc = sandbox_vm_write_full(cmd_fd, request->request_data, request->request_len);
  pthread_mutex_unlock(&session->helper_cmd_mutex);
  if (rc != 0) return rc;

  for (;;) {
    ant_sandbox_vm_helper_header_t header;
    rc = sandbox_vm_read_full(session->helper_msg_fd, &header, sizeof(header));
    if (rc != 0) {
      if (atomic_load_explicit(&session->helper_cancel_requested, memory_order_acquire)) {
        if (request->result) {
          request->result->kind = ANT_SANDBOX_VM_RESULT_CANCELED;
          request->result->code = -ECANCELED;
        }
        return -ECANCELED;
      }
      return rc;
    }
    if (header.magic != ANT_SANDBOX_VM_HELPER_MAGIC ||
        header.length > ANT_SANDBOX_FRAME_MAX_SIZE) {
      return -EPROTO;
    }

    if (header.type == ANT_SANDBOX_VM_HELPER_MSG_FRAME) {
      unsigned char *payload = NULL;
      if (header.length > 0) {
        payload = malloc(header.length);
        if (!payload) return -ENOMEM;
        rc = sandbox_vm_read_full(session->helper_msg_fd, payload, header.length);
        if (rc != 0) {
          free(payload);
          return rc;
        }
      }
      bool handled = false;
      if (request->frame_handler) handled = request->frame_handler(header.frame_type, payload, header.length, request->frame_handler_user);
      if (!handled) ant_sandbox_vm_display_frame(session->capabilities, header.frame_type, payload, header.length);
      free(payload);
      continue;
    }

    if (header.type == ANT_SANDBOX_VM_HELPER_MSG_EXECUTE_RESULT &&
        header.length == sizeof(ant_sandbox_vm_helper_result_t)) {
      ant_sandbox_vm_helper_result_t payload;
      rc = sandbox_vm_read_full(session->helper_msg_fd, &payload, sizeof(payload));
      if (rc != 0) return rc;
      if (request->result) {
        request->result->kind = (ant_sandbox_vm_result_kind_t)payload.kind;
        request->result->code = payload.code;
      }
      return payload.rc;
    }

    return -EPROTO;
  }
}

int ant_sandbox_vm_helper_send(ant_sandbox_vm_session_t *session, const void *data, size_t len) {
  if (!session || !data || len == 0 || len > UINT32_MAX) return -EINVAL;
  pthread_mutex_lock(&session->helper_cmd_mutex);
  int cmd_fd = session->helper_cmd_fd;
  int rc = cmd_fd >= 0
    ? sandbox_vm_send_header(cmd_fd, ANT_SANDBOX_VM_HELPER_CMD_SEND, 0, (uint32_t)len)
    : -ECANCELED;
  if (rc == 0) rc = sandbox_vm_write_full(cmd_fd, data, len);
  pthread_mutex_unlock(&session->helper_cmd_mutex);
  return rc;
}

int ant_sandbox_vm_helper_stats(ant_sandbox_vm_session_t *session, ant_sandbox_vm_stats_t *stats) {
  if (!session || !stats || session->helper_pid <= 0) return -EINVAL;
  pthread_mutex_lock(&session->helper_cmd_mutex);
  int rc = session->helper_cmd_fd >= 0 && session->helper_stats_fd >= 0
    ? sandbox_vm_send_header(session->helper_cmd_fd, ANT_SANDBOX_VM_HELPER_CMD_STATS, 0, 0)
    : -ECANCELED;
  uint64_t cpu_time_ns = 0;
  if (rc == 0)
    rc = sandbox_vm_read_full(session->helper_stats_fd, &cpu_time_ns, sizeof(cpu_time_ns));
  pthread_mutex_unlock(&session->helper_cmd_mutex);
  if (rc != 0) return rc;
  stats->cpu_time_ns = cpu_time_ns;
#if defined(__APPLE__)
  struct rusage_info_v2 usage;
  memset(&usage, 0, sizeof(usage));
  if (proc_pid_rusage(session->helper_pid, RUSAGE_INFO_V2, (rusage_info_t *)&usage) != 0)
    return -errno;
  stats->resident_memory_bytes = usage.ri_resident_size;
  stats->resident_memory_available = true;
  return 0;
#else
  return -ENOSYS;
#endif
}

int ant_sandbox_vm_helper_cancel(ant_sandbox_vm_session_t *session) {
  if (!session || !session->helper) return -EINVAL;
  atomic_store_explicit(&session->helper_cancel_requested, true, memory_order_release);
  pthread_mutex_lock(&session->helper_cmd_mutex);
  int cmd_fd = session->helper_cmd_fd;
  session->helper_cmd_fd = -1;
  if (cmd_fd >= 0)
    (void)sandbox_vm_send_header(cmd_fd, ANT_SANDBOX_VM_HELPER_CMD_DESTROY, 0, 0);
  pthread_mutex_unlock(&session->helper_cmd_mutex);
  if (cmd_fd >= 0) {
    sandbox_vm_stop_helper(session, true);
    close(cmd_fd);
  }
  return 0;
}

void ant_sandbox_vm_helper_destroy(ant_sandbox_vm_session_t *session) {
  if (!session) return;
  pthread_mutex_lock(&session->helper_cmd_mutex);
  int cmd_fd = session->helper_cmd_fd;
  session->helper_cmd_fd = -1;
  if (cmd_fd >= 0)
    (void)sandbox_vm_send_header(cmd_fd, ANT_SANDBOX_VM_HELPER_CMD_DESTROY, 0, 0);
  pthread_mutex_unlock(&session->helper_cmd_mutex);
  if (cmd_fd >= 0) close(cmd_fd);
  if (session->helper_msg_fd >= 0) {
    close(session->helper_msg_fd);
    session->helper_msg_fd = -1;
  }
  if (session->helper_stats_fd >= 0) {
    close(session->helper_stats_fd);
    session->helper_stats_fd = -1;
  }
  sandbox_vm_stop_helper(session, true);
  if (session->helper_cmd_mutex_init) pthread_mutex_destroy(&session->helper_cmd_mutex);
  free(session);
}
