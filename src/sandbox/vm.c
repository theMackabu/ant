#include <compat.h> // IWYU pragma: keep

#include "sandbox/sandbox.h"
#include "sandbox/vm.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ANT_SANDBOX_VM_HELPER_MAGIC 0x48564d41u /* AMVH */

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

struct ant_sandbox_vm_session {
  const ant_sandbox_vm_backend_t *backend;
  void *backend_session;
  pid_t helper_pid;
  int helper_cmd_fd;
  int helper_msg_fd;
  uint32_t capabilities;
  bool helper;
};

const char *ant_sandbox_vm_backend_name(const ant_sandbox_vm_backend_t *backend) {
  return backend ? backend->name : "none";
}

const ant_sandbox_vm_backend_t *ant_sandbox_vm_default_backend(void) {
#if defined(__APPLE__)
  return &ant_sandbox_vm_darwin_backend;
#else
  return NULL;
#endif
}

bool ant_sandbox_vm_supported(void) {
  return ant_sandbox_vm_default_backend() != NULL;
}

void ant_sandbox_vm_result_clear(ant_sandbox_vm_result_t *result) {
  if (!result) return;
  result->kind = ANT_SANDBOX_VM_RESULT_NONE;
  result->code = 0;
}

const char *ant_sandbox_vm_result_name(ant_sandbox_vm_result_kind_t kind) {
  switch (kind) {
    case ANT_SANDBOX_VM_RESULT_NONE: return "SandboxResultNone";
    case ANT_SANDBOX_VM_RESULT_GUEST_EXIT: return "SandboxScriptExit";
    case ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE: return "SandboxBackendUnavailable";
    case ANT_SANDBOX_VM_RESULT_CONFIG_ERROR: return "SandboxConfigError";
    case ANT_SANDBOX_VM_RESULT_TIMEOUT: return "SandboxTimeout";
    case ANT_SANDBOX_VM_RESULT_KERNEL_PANIC: return "SandboxKernelPanic";
    case ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR: return "SandboxProtocolError";
    case ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR: return "SandboxTransportError";
    case ANT_SANDBOX_VM_RESULT_VM_ERROR: return "SandboxVMError";
  }
  return "SandboxVMError";
}

bool ant_sandbox_vm_result_is_infrastructure_failure(const ant_sandbox_vm_result_t *result) {
  if (!result) return false;
  return result->kind != ANT_SANDBOX_VM_RESULT_NONE &&
         result->kind != ANT_SANDBOX_VM_RESULT_GUEST_EXIT;
}

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

static int sandbox_vm_helper_create(
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

  ant_sandbox_vm_session_t *session = calloc(1, sizeof(*session));
  if (!session) {
    close(to_child[1]);
    close(from_child[0]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return -ENOMEM;
  }

  session->backend = backend;
  session->helper_pid = pid;
  session->helper_cmd_fd = to_child[1];
  session->helper_msg_fd = from_child[0];
  session->capabilities = config->capabilities;
  session->helper = true;

  int create_rc = 0;
  int msg_type = sandbox_vm_helper_read_result(session->helper_msg_fd, config->result, &create_rc);
  if (msg_type != ANT_SANDBOX_VM_HELPER_MSG_CREATE_RESULT) {
    close(session->helper_cmd_fd);
    close(session->helper_msg_fd);
    waitpid(pid, NULL, 0);
    free(session);
    return msg_type < 0 ? msg_type : -EPROTO;
  }
  if (create_rc != 0) {
    close(session->helper_cmd_fd);
    close(session->helper_msg_fd);
    waitpid(pid, NULL, 0);
    free(session);
    return create_rc;
  }

  *session_out = session;
  return 0;
}

static int sandbox_vm_helper_execute(
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

int ant_sandbox_vm_start(const ant_sandbox_vm_config_t *config) {
  if (!config) return -EINVAL;
  ant_sandbox_vm_result_clear(config->result);

  const ant_sandbox_vm_backend_t *backend = ant_sandbox_vm_default_backend();
  if (!backend) {
    if (config->result) {
      config->result->kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE;
      config->result->code = -ENOSYS;
    }
    return -ENOSYS;
  }

  if (backend->create_session && backend->execute_session && backend->destroy_session) {
    ant_sandbox_vm_session_t *session = NULL;
    int rc = ant_sandbox_vm_session_create(config, &session);
    if (rc == 0) {
      ant_sandbox_vm_request_t request = {
        .request_data = config->request_data,
        .request_len = config->request_len,
        .frame_handler = config->frame_handler,
        .frame_handler_user = config->frame_handler_user,
        .result = config->result,
      };
      rc = ant_sandbox_vm_session_execute(session, &request);
    }
    ant_sandbox_vm_session_destroy(session);
    return rc;
  }

  if (!backend->start) return -ENOSYS;

  return backend->start(config);
}

int ant_sandbox_vm_session_create(const ant_sandbox_vm_config_t *config, ant_sandbox_vm_session_t **session_out) {
  if (!config || !session_out) return -EINVAL;
  *session_out = NULL;
  ant_sandbox_vm_result_clear(config->result);

  const ant_sandbox_vm_backend_t *backend = ant_sandbox_vm_default_backend();
  if (!backend || !backend->create_session || !backend->execute_session || !backend->destroy_session) {
    if (config->result) {
      config->result->kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE;
      config->result->code = -ENOSYS;
    }
    return -ENOSYS;
  }

#if defined(__APPLE__) && !defined(_WIN32)
  return sandbox_vm_helper_create(backend, config, session_out);
#endif

  ant_sandbox_vm_session_t *session = calloc(1, sizeof(*session));
  if (!session) return -ENOMEM;
  session->helper_cmd_fd = -1;
  session->helper_msg_fd = -1;

  int rc = backend->create_session(config, &session->backend_session);
  if (rc != 0) {
    free(session);
    return rc;
  }

  session->backend = backend;
  *session_out = session;
  return 0;
}

int ant_sandbox_vm_session_execute(ant_sandbox_vm_session_t *session, const ant_sandbox_vm_request_t *request) {
  if (!session || !session->backend || !request) return -EINVAL;
  ant_sandbox_vm_result_clear(request->result);
  if (session->helper) return sandbox_vm_helper_execute(session, request);
  if (!session->backend_session) return -EINVAL;
  if (!session->backend->execute_session) return -ENOSYS;
  return session->backend->execute_session(session->backend_session, request);
}

void ant_sandbox_vm_session_destroy(ant_sandbox_vm_session_t *session) {
  if (!session) return;
  if (session->helper) {
    if (session->helper_cmd_fd >= 0) {
      sandbox_vm_send_header(session->helper_cmd_fd, ANT_SANDBOX_VM_HELPER_CMD_DESTROY, 0, 0);
      close(session->helper_cmd_fd);
    }
    if (session->helper_msg_fd >= 0) close(session->helper_msg_fd);
    if (session->helper_pid > 0) waitpid(session->helper_pid, NULL, 0);
    free(session);
    return;
  }
  if (session->backend && session->backend->destroy_session && session->backend_session) {
    session->backend->destroy_session(session->backend_session);
  }
  free(session);
}
