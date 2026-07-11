#include <compat.h> // IWYU pragma: keep

#include "sandbox/sandbox.h"
#include "sandbox/host.h"
#include "sandbox/vm.h"
#include "vm_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool ant_sandbox_vm_helper_is_process(const char *argv0) {
  if (!argv0) return false;
  const char *binary_name = strrchr(argv0, '/');
  binary_name = binary_name ? binary_name + 1 : argv0;
  return strcmp(binary_name, "ant-sandbox-vm-helper") == 0;
}

#ifdef _WIN32

int ant_sandbox_vm_helper_process_main(void) {
  return EXIT_FAILURE;
}

int ant_sandbox_vm_helper_create(
  const ant_sandbox_vm_backend_t *backend,
  const ant_sandbox_vm_config_t *config,
  ant_sandbox_vm_session_t **session_out
) {
  (void)backend;
  if (session_out) *session_out = NULL;
  if (config && config->result) {
    config->result->kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE;
    config->result->code = -ENOSYS;
  }
  return -ENOSYS;
}

int ant_sandbox_vm_helper_execute(
  ant_sandbox_vm_session_t *session,
  const ant_sandbox_vm_request_t *request
) {
  (void)session;
  if (request && request->result) {
    request->result->kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE;
    request->result->code = -ENOSYS;
  }
  return -ENOSYS;
}

int ant_sandbox_vm_helper_cancel(ant_sandbox_vm_session_t *session) {
  (void)session;
  return -ENOSYS;
}

int ant_sandbox_vm_helper_send(ant_sandbox_vm_session_t *session, const void *data, size_t len) {
  (void)session; (void)data; (void)len;
  return -ENOSYS;
}

int ant_sandbox_vm_helper_stats(ant_sandbox_vm_session_t *session, ant_sandbox_vm_stats_t *stats) {
  (void)session; (void)stats;
  return -ENOSYS;
}

void ant_sandbox_vm_helper_destroy(ant_sandbox_vm_session_t *session) {
  free(session);
}

#else

#include <signal.h>
#include <poll.h>
#include <spawn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/resource.h>
#endif

extern char **environ;

enum { ANT_SANDBOX_VM_HELPER_MAGIC = 0x48564d41u }; // AMVH
enum {
  ANT_SANDBOX_VM_HELPER_STATS_FD = 197,
  ANT_SANDBOX_VM_HELPER_CMD_FD = 198,
  ANT_SANDBOX_VM_HELPER_MSG_FD = 199,
};

typedef enum {
  ANT_SANDBOX_VM_HELPER_CMD_EXECUTE = 1,
  ANT_SANDBOX_VM_HELPER_CMD_DESTROY = 2,
  ANT_SANDBOX_VM_HELPER_CMD_SEND = 3,
  ANT_SANDBOX_VM_HELPER_CMD_CREATE = 4,
  ANT_SANDBOX_VM_HELPER_CMD_STATS = 5,
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

static uint64_t sandbox_vm_cpu_time_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) return 0;
  return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} sandbox_helper_buffer_t;

typedef struct {
  const uint8_t *p;
  const uint8_t *end;
} sandbox_helper_reader_t;

typedef struct {
  ant_sandbox_vm_config_t config;
  ant_sandbox_mount_t *mounts;
  ant_sandbox_port_forward_t *forwards;
  char *image_path;
  char *kernel_path;
} sandbox_helper_owned_config_t;

static bool sandbox_helper_append(sandbox_helper_buffer_t *buf, const void *data, size_t len) {
  if (!buf || (!data && len > 0) || buf->len > ANT_SANDBOX_FRAME_MAX_SIZE - len) return false;
  size_t needed = buf->len + len;
  if (needed > buf->cap) {
    size_t cap = buf->cap ? buf->cap * 2 : 512;
    while (cap < needed) cap *= 2;
    uint8_t *next = realloc(buf->data, cap);
    if (!next) return false;
    buf->data = next;
    buf->cap = cap;
  }
  if (len > 0) memcpy(buf->data + buf->len, data, len);
  buf->len = needed;
  return true;
}

static bool sandbox_helper_append_u8(sandbox_helper_buffer_t *buf, uint8_t value) {
  return sandbox_helper_append(buf, &value, sizeof(value));
}

static bool sandbox_helper_append_u16(sandbox_helper_buffer_t *buf, uint16_t value) {
  uint8_t raw[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
  return sandbox_helper_append(buf, raw, sizeof(raw));
}

static bool sandbox_helper_append_u32(sandbox_helper_buffer_t *buf, uint32_t value) {
  uint8_t raw[4] = {
    (uint8_t)value, (uint8_t)(value >> 8),
    (uint8_t)(value >> 16), (uint8_t)(value >> 24),
  };
  return sandbox_helper_append(buf, raw, sizeof(raw));
}

static bool sandbox_helper_append_u64(sandbox_helper_buffer_t *buf, uint64_t value) {
  uint8_t raw[8];
  for (unsigned i = 0; i < 8; i++) raw[i] = (uint8_t)(value >> (i * 8));
  return sandbox_helper_append(buf, raw, sizeof(raw));
}

static bool sandbox_helper_append_string(sandbox_helper_buffer_t *buf, const char *value) {
  size_t len = value ? strlen(value) : 0;
  return len <= UINT32_MAX && sandbox_helper_append_u32(buf, (uint32_t)len) &&
    sandbox_helper_append(buf, value, len);
}

static uint8_t *sandbox_helper_encode_config(const ant_sandbox_vm_config_t *config, size_t *len_out) {
  if (!config || !config->image_path || !config->kernel_path || !len_out ||
      config->mount_count > UINT32_MAX || config->forward_count > UINT32_MAX) return NULL;
  sandbox_helper_buffer_t buf = {0};
  bool ok =
    sandbox_helper_append_string(&buf, config->image_path) &&
    sandbox_helper_append_string(&buf, config->kernel_path) &&
    sandbox_helper_append_u32(&buf, config->capabilities) &&
    sandbox_helper_append_u8(&buf, config->network_enabled ? 1 : 0) &&
    sandbox_helper_append_u32(&buf, config->cpu_count) &&
    sandbox_helper_append_u64(&buf, config->memory_size) &&
    sandbox_helper_append_u32(&buf, config->timeout_ms) &&
    sandbox_helper_append_u32(&buf, config->boot_timeout_ms) &&
    sandbox_helper_append_u32(&buf, config->cpu_time_ms) &&
    sandbox_helper_append_u8(&buf, config->verbose ? 1 : 0) &&
    sandbox_helper_append_u32(&buf, (uint32_t)config->mount_count);
  for (size_t i = 0; ok && i < config->mount_count; i++) {
    ok = sandbox_helper_append_string(&buf, config->mounts[i].host_path) &&
      sandbox_helper_append_string(&buf, config->mounts[i].guest_path) &&
      sandbox_helper_append_string(&buf, config->mounts[i].tag) &&
      sandbox_helper_append_u8(&buf, config->mounts[i].readonly ? 1 : 0);
  }
  ok = ok && sandbox_helper_append_u32(&buf, (uint32_t)config->forward_count);
  for (size_t i = 0; ok && i < config->forward_count; i++) {
    ok = sandbox_helper_append_u16(&buf, config->forwards[i].host_port) &&
      sandbox_helper_append_u16(&buf, config->forwards[i].guest_port);
  }
  if (!ok) { free(buf.data); return NULL; }
  *len_out = buf.len;
  return buf.data;
}

static bool sandbox_helper_read(sandbox_helper_reader_t *r, void *out, size_t len) {
  if (!r || !out || len > (size_t)(r->end - r->p)) return false;
  memcpy(out, r->p, len);
  r->p += len;
  return true;
}

static bool sandbox_helper_read_u8(sandbox_helper_reader_t *r, uint8_t *out) {
  return sandbox_helper_read(r, out, sizeof(*out));
}

static bool sandbox_helper_read_u16(sandbox_helper_reader_t *r, uint16_t *out) {
  uint8_t raw[2];
  if (!sandbox_helper_read(r, raw, sizeof(raw))) return false;
  *out = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
  return true;
}

static bool sandbox_helper_read_u32(sandbox_helper_reader_t *r, uint32_t *out) {
  uint8_t raw[4];
  if (!sandbox_helper_read(r, raw, sizeof(raw))) return false;
  *out = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
    ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
  return true;
}

static bool sandbox_helper_read_u64(sandbox_helper_reader_t *r, uint64_t *out) {
  uint8_t raw[8];
  if (!sandbox_helper_read(r, raw, sizeof(raw))) return false;
  *out = 0;
  for (unsigned i = 0; i < 8; i++) *out |= (uint64_t)raw[i] << (i * 8);
  return true;
}

static char *sandbox_helper_read_string(sandbox_helper_reader_t *r) {
  uint32_t len = 0;
  if (!sandbox_helper_read_u32(r, &len) || len > (size_t)(r->end - r->p)) return NULL;
  char *value = malloc((size_t)len + 1);
  if (!value) return NULL;
  memcpy(value, r->p, len);
  value[len] = '\0';
  r->p += len;
  return value;
}

static void sandbox_helper_owned_config_free(sandbox_helper_owned_config_t *owned) {
  if (!owned) return;
  for (size_t i = 0; i < owned->config.mount_count; i++) {
    free((char *)owned->mounts[i].host_path);
    free((char *)owned->mounts[i].guest_path);
    free((char *)owned->mounts[i].tag);
  }
  free(owned->mounts);
  free(owned->forwards);
  free(owned->image_path);
  free(owned->kernel_path);
  memset(owned, 0, sizeof(*owned));
}

static bool sandbox_helper_decode_config(
  const uint8_t *data, size_t len,
  sandbox_helper_owned_config_t *owned
) {
  if (!data || !owned) return false;
  sandbox_helper_reader_t r = { data, data + len };
  uint8_t network = 0, verbose = 0;
  uint32_t mount_count = 0, forward_count = 0;
  uint64_t memory_size = 0;
  owned->image_path = sandbox_helper_read_string(&r);
  owned->kernel_path = sandbox_helper_read_string(&r);
  if (!owned->image_path || !owned->kernel_path ||
      !sandbox_helper_read_u32(&r, &owned->config.capabilities) ||
      !sandbox_helper_read_u8(&r, &network) ||
      !sandbox_helper_read_u32(&r, &owned->config.cpu_count) ||
      !sandbox_helper_read_u64(&r, &memory_size) ||
      !sandbox_helper_read_u32(&r, &owned->config.timeout_ms) ||
      !sandbox_helper_read_u32(&r, &owned->config.boot_timeout_ms) ||
      !sandbox_helper_read_u32(&r, &owned->config.cpu_time_ms) ||
      !sandbox_helper_read_u8(&r, &verbose) ||
      !sandbox_helper_read_u32(&r, &mount_count)) goto fail;
  if (mount_count > ANT_SANDBOX_MAX_MOUNTS) goto fail;
  if (mount_count > 0) {
    owned->mounts = calloc(mount_count, sizeof(*owned->mounts));
    if (!owned->mounts) goto fail;
  }
  owned->config.mount_count = mount_count;
  for (uint32_t i = 0; i < mount_count; i++) {
    uint8_t readonly = 0;
    owned->mounts[i].host_path = sandbox_helper_read_string(&r);
    owned->mounts[i].guest_path = sandbox_helper_read_string(&r);
    owned->mounts[i].tag = sandbox_helper_read_string(&r);
    if (!owned->mounts[i].host_path || !owned->mounts[i].guest_path ||
        !owned->mounts[i].tag || !sandbox_helper_read_u8(&r, &readonly)) goto fail;
    owned->mounts[i].readonly = readonly != 0;
  }
  if (!sandbox_helper_read_u32(&r, &forward_count) || forward_count > ANT_SANDBOX_MAX_FORWARDS) goto fail;
  if (forward_count > 0) {
    owned->forwards = calloc(forward_count, sizeof(*owned->forwards));
    if (!owned->forwards) goto fail;
  }
  owned->config.forward_count = forward_count;
  for (uint32_t i = 0; i < forward_count; i++) {
    if (!sandbox_helper_read_u16(&r, &owned->forwards[i].host_port) ||
        !sandbox_helper_read_u16(&r, &owned->forwards[i].guest_port)) goto fail;
  }
  if (r.p != r.end) goto fail;
  owned->config.image_path = owned->image_path;
  owned->config.kernel_path = owned->kernel_path;
  owned->config.mounts = owned->mounts;
  owned->config.network_enabled = network != 0;
  owned->config.forwards = owned->forwards;
  owned->config.memory_size = memory_size;
  owned->config.verbose = verbose != 0;
  return true;

fail:
  sandbox_helper_owned_config_free(owned);
  return false;
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

typedef struct {
  const ant_sandbox_vm_backend_t *backend;
  void *backend_session;
  int cmd_fd;
  int stop_fd;
  int stats_fd;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  uint8_t *execute_data;
  uint32_t execute_len;
  bool have_execute;
  bool destroy;
} sandbox_child_control_t;

static void *sandbox_child_command_main(void *opaque) {
  sandbox_child_control_t *control = opaque;
  for (;;) {
    struct pollfd fds[2] = {
      { .fd = control->cmd_fd, .events = POLLIN },
      { .fd = control->stop_fd, .events = POLLIN },
    };
    int poll_rc;
    do poll_rc = poll(fds, 2, -1); while (poll_rc < 0 && errno == EINTR);
    if (poll_rc <= 0 || (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) return NULL;
    if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

    ant_sandbox_vm_helper_header_t header;
    int rc = sandbox_vm_read_full(control->cmd_fd, &header, sizeof(header));
    if (rc != 0 || header.magic != ANT_SANDBOX_VM_HELPER_MAGIC) break;
    if (header.type == ANT_SANDBOX_VM_HELPER_CMD_DESTROY) {
      pthread_mutex_lock(&control->mutex);
      control->destroy = true;
      pthread_cond_signal(&control->cond);
      pthread_mutex_unlock(&control->mutex);
      if (control->backend->cancel_session)
        control->backend->cancel_session(control->backend_session);
      return NULL;
    }
    if (header.type == ANT_SANDBOX_VM_HELPER_CMD_STATS && header.length == 0) {
      uint64_t cpu_time_ns = sandbox_vm_cpu_time_ns();
      if (sandbox_vm_write_full(control->stats_fd, &cpu_time_ns, sizeof(cpu_time_ns)) != 0)
        break;
      continue;
    }
    if ((header.type != ANT_SANDBOX_VM_HELPER_CMD_EXECUTE &&
         header.type != ANT_SANDBOX_VM_HELPER_CMD_SEND) ||
        header.length == 0 || header.length > ANT_SANDBOX_FRAME_MAX_SIZE) break;

    uint8_t *data = malloc(header.length);
    if (!data) break;
    rc = sandbox_vm_read_full(control->cmd_fd, data, header.length);
    if (rc != 0) { free(data); break; }

    if (header.type == ANT_SANDBOX_VM_HELPER_CMD_SEND) {
      rc = control->backend->send_session
        ? control->backend->send_session(control->backend_session, data, header.length)
        : -ENOSYS;
      free(data);
      if (rc != 0) break;
      continue;
    }

    pthread_mutex_lock(&control->mutex);
    while (control->have_execute && !control->destroy)
      pthread_cond_wait(&control->cond, &control->mutex);
    if (control->destroy) {
      pthread_mutex_unlock(&control->mutex);
      free(data);
      return NULL;
    }
    control->execute_data = data;
    control->execute_len = header.length;
    control->have_execute = true;
    pthread_cond_signal(&control->cond);
    pthread_mutex_unlock(&control->mutex);
  }

  pthread_mutex_lock(&control->mutex);
  control->destroy = true;
  pthread_cond_signal(&control->cond);
  pthread_mutex_unlock(&control->mutex);
  if (control->backend->cancel_session)
    control->backend->cancel_session(control->backend_session);
  return NULL;
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
  int msg_fd,
  int stats_fd
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

  sandbox_child_control_t control = {
    .backend = backend,
    .backend_session = backend_session,
    .cmd_fd = cmd_fd,
    .stop_fd = -1,
    .stats_fd = stats_fd,
  };
  int stop_pipe[2] = { -1, -1 };
  if (pipe(stop_pipe) != 0) goto done;
  control.stop_fd = stop_pipe[0];
  if (pthread_mutex_init(&control.mutex, NULL) != 0) {
    close(stop_pipe[0]); close(stop_pipe[1]);
    goto done;
  }
  if (pthread_cond_init(&control.cond, NULL) != 0) {
    pthread_mutex_destroy(&control.mutex);
    close(stop_pipe[0]); close(stop_pipe[1]);
    goto done;
  }
  pthread_t command_thread;
  bool command_started = pthread_create(&command_thread, NULL, sandbox_child_command_main, &control) == 0;
  if (!command_started) {
    pthread_cond_destroy(&control.cond);
    pthread_mutex_destroy(&control.mutex);
    close(stop_pipe[0]); close(stop_pipe[1]);
    goto done;
  }

  for (;;) {
    pthread_mutex_lock(&control.mutex);
    while (!control.have_execute && !control.destroy)
      pthread_cond_wait(&control.cond, &control.mutex);
    if (control.destroy) {
      pthread_mutex_unlock(&control.mutex);
      break;
    }
    uint8_t *request_data = control.execute_data;
    uint32_t request_len = control.execute_len;
    control.execute_data = NULL;
    control.execute_len = 0;
    control.have_execute = false;
    pthread_cond_signal(&control.cond);
    pthread_mutex_unlock(&control.mutex);

    ant_sandbox_vm_result_t exec_result = {0};
    ant_sandbox_vm_request_t request = {
      .request_data = request_data,
      .request_len = request_len,
      .frame_handler = sandbox_vm_helper_child_frame,
      .frame_handler_user = &msg_fd,
      .result = &exec_result,
    };
    rc = backend->execute_session(backend_session, &request);
    free(request_data);
    if (sandbox_vm_send_result(msg_fd, ANT_SANDBOX_VM_HELPER_MSG_EXECUTE_RESULT, rc, &exec_result) != 0)
      break;
  }

  pthread_mutex_lock(&control.mutex);
  control.destroy = true;
  pthread_cond_signal(&control.cond);
  pthread_mutex_unlock(&control.mutex);
  unsigned char stop = 1;
  (void)write(stop_pipe[1], &stop, sizeof(stop));
  pthread_join(command_thread, NULL);
  free(control.execute_data);
  pthread_cond_destroy(&control.cond);
  pthread_mutex_destroy(&control.mutex);
  close(stop_pipe[0]);
  close(stop_pipe[1]);

done:
  if (backend_session) backend->destroy_session(backend_session);
  if (cmd_fd >= 0) close(cmd_fd);
  close(msg_fd);
  close(stats_fd);
  _exit(0);
}

int ant_sandbox_vm_helper_process_main(void) {
  ant_sandbox_vm_helper_header_t header;
  int rc = sandbox_vm_read_full(ANT_SANDBOX_VM_HELPER_CMD_FD, &header, sizeof(header));
  if (rc != 0 || header.magic != ANT_SANDBOX_VM_HELPER_MAGIC ||
      header.type != ANT_SANDBOX_VM_HELPER_CMD_CREATE || header.length == 0 ||
      header.length > ANT_SANDBOX_FRAME_MAX_SIZE) return EXIT_FAILURE;

  uint8_t *payload = malloc(header.length);
  if (!payload) return EXIT_FAILURE;
  rc = sandbox_vm_read_full(ANT_SANDBOX_VM_HELPER_CMD_FD, payload, header.length);
  sandbox_helper_owned_config_t owned = {0};
  bool decoded = rc == 0 && sandbox_helper_decode_config(payload, header.length, &owned);
  free(payload);
  if (!decoded) {
    ant_sandbox_vm_result_t result = {
      .kind = ANT_SANDBOX_VM_RESULT_CONFIG_ERROR,
      .code = -EINVAL,
    };
    sandbox_vm_send_result(ANT_SANDBOX_VM_HELPER_MSG_FD, ANT_SANDBOX_VM_HELPER_MSG_CREATE_RESULT, -EINVAL, &result);
    return EXIT_FAILURE;
  }

  const ant_sandbox_vm_backend_t *backend = ant_sandbox_vm_default_backend();
  if (!backend || !backend->create_session || !backend->execute_session || !backend->destroy_session) {
    ant_sandbox_vm_result_t result = {
      .kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE,
      .code = -ENOSYS,
    };
    sandbox_vm_send_result(ANT_SANDBOX_VM_HELPER_MSG_FD, ANT_SANDBOX_VM_HELPER_MSG_CREATE_RESULT, -ENOSYS, &result);
    sandbox_helper_owned_config_free(&owned);
    return EXIT_FAILURE;
  }

  sandbox_vm_helper_child(
    backend, &owned.config,
    ANT_SANDBOX_VM_HELPER_CMD_FD,
    ANT_SANDBOX_VM_HELPER_MSG_FD,
    ANT_SANDBOX_VM_HELPER_STATS_FD
  );
  return EXIT_SUCCESS;
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

  pid_t pid = -1;
  char *helper_argv[] = { "ant-sandbox-vm-helper", NULL };
  if (spawn_rc == 0) spawn_rc = posix_spawn(&pid, executable, &actions, NULL, helper_argv, environ);
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

#endif
