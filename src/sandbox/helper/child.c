#include <compat.h> // IWYU pragma: keep
#include "protocol.h"

#include <poll.h>
#include <pthread.h>

bool ant_sandbox_vm_helper_is_process(const char *argv0) {
  if (!argv0) return false;
  const char *binary_name = strrchr(argv0, '/');
  binary_name = binary_name ? binary_name + 1 : argv0;
  return strcmp(binary_name, "ant-sandbox-vm-helper") == 0;
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

int ant_sandbox_vm_display_frame(uint32_t capabilities, uint8_t type, const unsigned char *payload, uint32_t len) {
  bool strip_ansi = (capabilities & ANT_SANDBOX_CAP_COLOR_STRIP) != 0;
  const char *display = NULL;
  size_t display_len = 0;
  FILE *stream = stdout;

  switch (type) {
    case ANT_SANDBOX_FRAME_STDOUT:
      sandbox_vm_write_output(stdout, payload, len, strip_ansi);
      return 0;
    case ANT_SANDBOX_FRAME_STDERR:
      sandbox_vm_write_output(stderr, payload, len, strip_ansi);
      return 0;
    case ANT_SANDBOX_FRAME_RESULT:
      if (!ant_sandbox_result_payload_display(payload, len, &display, &display_len)) return -EINVAL;
      break;
    case ANT_SANDBOX_FRAME_ERROR:
      if (!ant_sandbox_error_payload_display(payload, len, &display, &display_len)) return -EINVAL;
      stream = stderr;
      break;
    default: return -EINVAL;
  }

  if (display_len > 0) {
    sandbox_vm_write_output(stream, (const unsigned char *)display, (uint32_t)display_len, strip_ansi);
    if (display[display_len - 1] != '\n') fputc('\n', stream);
    fflush(stream);
  }
  
  return 0;
}

static void sandbox_vm_helper_child(
  const ant_sandbox_vm_backend_t *backend,
  const ant_sandbox_vm_config_t *config,
  int cmd_fd, int msg_fd, int stats_fd
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
    
    if (sandbox_vm_send_result(msg_fd, ANT_SANDBOX_VM_HELPER_MSG_EXECUTE_RESULT, rc, &exec_result) != 0) break;
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
