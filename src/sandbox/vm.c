#include <compat.h> // IWYU pragma: keep

#include "sandbox/vm.h"
#include "vm_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t sandbox_vm_monotonic_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

const char *ant_sandbox_vm_backend_name(const ant_sandbox_vm_backend_t *backend) {
  return backend ? backend->name : "none";
}

const ant_sandbox_vm_backend_t *ant_sandbox_vm_default_backend(void) {
#if defined(__APPLE__)
  return &ant_sandbox_vm_darwin_backend;
#elif defined(__linux__)
  return &ant_sandbox_vm_linux_backend;
#elif defined(_WIN32)
  return &ant_sandbox_vm_windows_backend;
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
    case ANT_SANDBOX_VM_RESULT_CPU_TIME_LIMIT: return "SandboxCpuTimeLimit";
    case ANT_SANDBOX_VM_RESULT_KERNEL_PANIC: return "SandboxKernelPanic";
    case ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR: return "SandboxProtocolError";
    case ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR: return "SandboxTransportError";
    case ANT_SANDBOX_VM_RESULT_CANCELED: return "SandboxCanceled";
    case ANT_SANDBOX_VM_RESULT_VM_ERROR: return "SandboxVMError";
  }
  return "SandboxVMError";
}

bool ant_sandbox_vm_result_is_infrastructure_failure(const ant_sandbox_vm_result_t *result) {
  if (!result) return false;
  return 
    result->kind != ANT_SANDBOX_VM_RESULT_NONE &&
    result->kind != ANT_SANDBOX_VM_RESULT_GUEST_EXIT;
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
  uint64_t created_at_ns = sandbox_vm_monotonic_ns();
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
  int helper_rc = ant_sandbox_vm_helper_create(backend, config, session_out);
  if (helper_rc == 0 && *session_out) (*session_out)->created_at_ns = created_at_ns;
  return helper_rc;
#endif

  ant_sandbox_vm_session_t *session = calloc(1, sizeof(*session));
  if (!session) return -ENOMEM;
  session->helper_cmd_fd = -1;
  session->helper_msg_fd = -1;
  session->helper_stats_fd = -1;

  int rc = backend->create_session(config, &session->backend_session);
  if (rc != 0) {
    free(session);
    return rc;
  }

  session->backend = backend;
  session->created_at_ns = created_at_ns;
  *session_out = session;
  return 0;
}

int ant_sandbox_vm_session_execute(ant_sandbox_vm_session_t *session, const ant_sandbox_vm_request_t *request) {
  if (!session || !session->backend || !request) return -EINVAL;
  ant_sandbox_vm_result_clear(request->result);
  if (session->helper) return ant_sandbox_vm_helper_execute(session, request);
  if (!session->backend_session) return -EINVAL;
  if (!session->backend->execute_session) return -ENOSYS;
  return session->backend->execute_session(session->backend_session, request);
}

int ant_sandbox_vm_session_send(ant_sandbox_vm_session_t *session, const void *data, size_t len) {
  if (!session || !session->backend || !data || len == 0) return -EINVAL;
  if (session->helper) return ant_sandbox_vm_helper_send(session, data, len);
  if (!session->backend_session || !session->backend->send_session) return -ENOSYS;
  return session->backend->send_session(session->backend_session, data, len);
}

int ant_sandbox_vm_session_stats(ant_sandbox_vm_session_t *session, ant_sandbox_vm_stats_t *stats) {
  if (!session || !stats) return -EINVAL;
  memset(stats, 0, sizeof(*stats));
  uint64_t now = sandbox_vm_monotonic_ns();
  if (now >= session->created_at_ns) stats->wall_time_ns = now - session->created_at_ns;
  if (session->helper) return ant_sandbox_vm_helper_stats(session, stats);
  if (!session->backend_session || !session->backend || !session->backend->get_stats_session)
    return -ENOSYS;
  uint64_t wall_time_ns = stats->wall_time_ns;
  int rc = session->backend->get_stats_session(session->backend_session, stats);
  stats->wall_time_ns = wall_time_ns;
  return rc;
}

int ant_sandbox_vm_session_cancel(ant_sandbox_vm_session_t *session) {
  if (!session) return -EINVAL;
  if (session->helper) return ant_sandbox_vm_helper_cancel(session);
  if (session->backend && session->backend->cancel_session && session->backend_session) {
    return session->backend->cancel_session(session->backend_session);
  }
  return -ENOSYS;
}

void ant_sandbox_vm_session_destroy(ant_sandbox_vm_session_t *session) {
  if (!session) return;
  if (session->helper) {
    ant_sandbox_vm_helper_destroy(session);
    return;
  }
  if (session->backend && session->backend->destroy_session && session->backend_session)
    session->backend->destroy_session(session->backend_session);
  free(session);
}
