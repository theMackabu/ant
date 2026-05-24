#include <compat.h> // IWYU pragma: keep

#include "sandbox/sandbox.h"
#include "sandbox/vm.h"
#include "vm_internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ANT_SANDBOX_VM_HELPER_MAGIC = 0x48564d41u }; // AMVH

typedef enum {
  ANT_SANDBOX_VM_HELPER_CMD_EXECUTE = 1,
  ANT_SANDBOX_VM_HELPER_CMD_DESTROY = 2,
} ant_sandbox_vm_helper_cmd_t;

typedef enum {
  ANT_SANDBOX_VM_HELPER_MSG_CREATE_RESULT = 1,
  ANT_SANDBOX_VM_HELPER_MSG_FRAME = 2,
  ANT_SANDBOX_VM_HELPER_MSG_EXECUTE_RESULT = 3,
} ant_sandbox_vm_helper_msg_t;

typedef struct {
  uint32_t magic;
  uint8_t type;
  uint8_t frame_type;
  uint16_t reserved;
  uint32_t length;
} ant_sandbox_vm_helper_header_t;

typedef struct {
  int32_t rc;
  int32_t kind;
  int32_t code;
} ant_sandbox_vm_helper_result_t;

static int sandbox_vm_read_full(int fd, void *buf, size_t len) {
  unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n == 0) return -EPIPE;
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static int sandbox_vm_write_full(int fd, const void *buf, size_t len) {
  const unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static int sandbox_vm_send_header(int fd, uint8_t type, uint8_t frame_type, uint32_t length) {
  ant_sandbox_vm_helper_header_t header = {
    .magic = ANT_SANDBOX_VM_HELPER_MAGIC,
    .type = type,
    .frame_type = frame_type,
    .reserved = 0,
    .length = length,
  };
  return sandbox_vm_write_full(fd, &header, sizeof(header));
}

static int sandbox_vm_send_result(
  int fd,
  uint8_t type,
  int rc,
  const ant_sandbox_vm_result_t *result
) {
  ant_sandbox_vm_helper_result_t payload = {
    .rc = rc,
    .kind = result ? (int32_t)result->kind : ANT_SANDBOX_VM_RESULT_NONE,
    .code = result ? result->code : 0,
  };
  int send_rc = sandbox_vm_send_header(fd, type, 0, sizeof(payload));
  if (send_rc != 0) return send_rc;
  return sandbox_vm_write_full(fd, &payload, sizeof(payload));
}

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

static bool sandbox_vm_helper_child_frame(uint8_t type, const void *payload, size_t payload_len, void *user) {
  int fd = *(int *)user;
  if (payload_len > UINT32_MAX) return false;
  if (sandbox_vm_send_header(fd, ANT_SANDBOX_VM_HELPER_MSG_FRAME, type, (uint32_t)payload_len) != 0) {
    return false;
  }
  if (payload_len > 0 && sandbox_vm_write_full(fd, payload, payload_len) != 0) return false;
  return type != ANT_SANDBOX_FRAME_EXIT;
}

static void sandbox_vm_write_output(FILE *stream, const unsigned char *payload, uint32_t len, bool strip_ansi) {
  if (!strip_ansi) {
    if (len > 0) fwrite(payload, 1, len, stream);
    fflush(stream);
    return;
  }

  for (uint32_t i = 0; i < len; i++) {
    if (payload[i] == '\x1b' && i + 1 < len && payload[i + 1] == '[') {
      i += 2;
      while (i < len && ((payload[i] >= '0' && payload[i] <= '9') || payload[i] == ';')) i++;
      if (i < len && payload[i] == 'm') continue;
      fputc('\x1b', stream);
      fputc('[', stream);
      if (i < len) fputc(payload[i], stream);
      continue;
    }
    fputc(payload[i], stream);
  }
  fflush(stream);
}

static void sandbox_vm_default_frame_handler(
  uint32_t capabilities,
  uint8_t type,
  const unsigned char *payload,
  uint32_t len
) {
  bool strip_ansi = (capabilities & ANT_SANDBOX_CAP_COLOR_STRIP) != 0;
  switch (type) {
    case ANT_SANDBOX_FRAME_STDOUT:
      sandbox_vm_write_output(stdout, payload, len, strip_ansi);
      break;
    case ANT_SANDBOX_FRAME_STDERR:
      sandbox_vm_write_output(stderr, payload, len, strip_ansi);
      break;
    case ANT_SANDBOX_FRAME_RESULT:
    {
      const char *display = NULL;
      size_t display_len = 0;
      if (ant_sandbox_result_payload_display(payload, len, &display, &display_len) && display_len > 0) {
        sandbox_vm_write_output(stdout, (const unsigned char *)display, (uint32_t)display_len, strip_ansi);
        if (display[display_len - 1] != '\n') fputc('\n', stdout);
        fflush(stdout);
      }
      break;
    }
    case ANT_SANDBOX_FRAME_ERROR:
    {
      const char *display = NULL;
      size_t display_len = 0;
      if (ant_sandbox_error_payload_display(payload, len, &display, &display_len) && display_len > 0) {
        sandbox_vm_write_output(stderr, (const unsigned char *)display, (uint32_t)display_len, strip_ansi);
        if (display[display_len - 1] != '\n') fputc('\n', stderr);
        fflush(stderr);
      }
      break;
    }
    default:
      break;
  }
}

static void sandbox_vm_helper_child(
  const ant_sandbox_vm_backend_t *backend,
  const ant_sandbox_vm_config_t *config,
  int cmd_fd,
  int msg_fd
) {
  void *backend_session = NULL;
  ant_sandbox_vm_result_t create_result = {0};
  ant_sandbox_vm_config_t child_config = *config;
  child_config.frame_handler = sandbox_vm_helper_child_frame;
  child_config.frame_handler_user = &msg_fd;
  child_config.result = &create_result;
  int rc = backend->create_session(&child_config, &backend_session);
  sandbox_vm_send_result(msg_fd, ANT_SANDBOX_VM_HELPER_MSG_CREATE_RESULT, rc, &create_result);
  if (rc != 0) goto done;

  for (;;) {
    ant_sandbox_vm_helper_header_t header;
    rc = sandbox_vm_read_full(cmd_fd, &header, sizeof(header));
    if (rc != 0) break;
    if (header.magic != ANT_SANDBOX_VM_HELPER_MAGIC) break;
    if (header.type == ANT_SANDBOX_VM_HELPER_CMD_DESTROY) break;
    if (header.type != ANT_SANDBOX_VM_HELPER_CMD_EXECUTE ||
        header.length == 0 ||
        header.length > ANT_SANDBOX_FRAME_MAX_SIZE) {
      break;
    }

    uint8_t *request_data = malloc(header.length);
    if (!request_data) break;
    rc = sandbox_vm_read_full(cmd_fd, request_data, header.length);
    if (rc != 0) {
      free(request_data);
      break;
    }

    ant_sandbox_vm_result_t exec_result = {0};
    ant_sandbox_vm_request_t request = {
      .request_data = request_data,
      .request_len = header.length,
      .frame_handler = sandbox_vm_helper_child_frame,
      .frame_handler_user = &msg_fd,
      .result = &exec_result,
    };
    rc = backend->execute_session(backend_session, &request);
    free(request_data);
    if (sandbox_vm_send_result(msg_fd, ANT_SANDBOX_VM_HELPER_MSG_EXECUTE_RESULT, rc, &exec_result) != 0) {
      break;
    }
  }

done:
  if (backend_session) backend->destroy_session(backend_session);
  close(cmd_fd);
  close(msg_fd);
  _exit(0);
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
  if (pipe(to_child) != 0) return -errno;
  if (pipe(from_child) != 0) {
    int rc = -errno;
    close(to_child[0]);
    close(to_child[1]);
    return rc;
  }

  pid_t pid = fork();
  if (pid < 0) {
    int rc = -errno;
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    return rc;
  }

  if (pid == 0) {
    close(to_child[1]);
    close(from_child[0]);
    sandbox_vm_helper_child(backend, config, to_child[0], from_child[1]);
  }

  close(to_child[0]);
  close(from_child[1]);
  sandbox_vm_verbose_helper_pid(config, pid);

  ant_sandbox_vm_session_t *session = calloc(1, sizeof(*session));
  if (!session) {
    close(to_child[1]);
    close(from_child[0]);
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
  session->capabilities = config->capabilities;
  session->verbose = config->verbose;
  session->helper = true;

  int create_rc = 0;
  int msg_type = sandbox_vm_helper_read_result(session->helper_msg_fd, config->result, &create_rc);
  if (msg_type != ANT_SANDBOX_VM_HELPER_MSG_CREATE_RESULT) {
    close(session->helper_cmd_fd);
    session->helper_cmd_fd = -1;
    close(session->helper_msg_fd);
    session->helper_msg_fd = -1;
    sandbox_vm_stop_helper(session, true);
    free(session);
    return msg_type < 0 ? msg_type : -EPROTO;
  }
  if (create_rc != 0) {
    close(session->helper_cmd_fd);
    session->helper_cmd_fd = -1;
    close(session->helper_msg_fd);
    session->helper_msg_fd = -1;
    sandbox_vm_stop_helper(session, true);
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
  int rc = sandbox_vm_send_header(session->helper_cmd_fd,
                                  ANT_SANDBOX_VM_HELPER_CMD_EXECUTE,
                                  0,
                                  (uint32_t)request->request_len);
  if (rc != 0) return rc;
  rc = sandbox_vm_write_full(session->helper_cmd_fd, request->request_data, request->request_len);
  if (rc != 0) return rc;

  for (;;) {
    ant_sandbox_vm_helper_header_t header;
    rc = sandbox_vm_read_full(session->helper_msg_fd, &header, sizeof(header));
    if (rc != 0) return rc;
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
      if (request->frame_handler) {
        handled = request->frame_handler(header.frame_type, payload, header.length, request->frame_handler_user);
      }
      if (!handled) {
        sandbox_vm_default_frame_handler(session->capabilities, header.frame_type, payload, header.length);
      }
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

int ant_sandbox_vm_helper_cancel(ant_sandbox_vm_session_t *session) {
  if (!session || !session->helper) return -EINVAL;
  if (session->helper_cmd_fd >= 0) {
    close(session->helper_cmd_fd);
    session->helper_cmd_fd = -1;
  }
  if (session->helper_msg_fd >= 0) {
    close(session->helper_msg_fd);
    session->helper_msg_fd = -1;
  }
  sandbox_vm_stop_helper(session, true);
  return 0;
}

void ant_sandbox_vm_helper_destroy(ant_sandbox_vm_session_t *session) {
  if (!session) return;
  if (session->helper_cmd_fd >= 0) {
    sandbox_vm_send_header(session->helper_cmd_fd, ANT_SANDBOX_VM_HELPER_CMD_DESTROY, 0, 0);
    close(session->helper_cmd_fd);
    session->helper_cmd_fd = -1;
  }
  if (session->helper_msg_fd >= 0) {
    close(session->helper_msg_fd);
    session->helper_msg_fd = -1;
  }
  sandbox_vm_stop_helper(session, true);
  free(session);
}
