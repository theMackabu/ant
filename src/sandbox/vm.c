#include <compat.h> // IWYU pragma: keep

#include "sandbox/vm.h"

#include <errno.h>
#include <stdlib.h>

struct ant_sandbox_vm_session {
  const ant_sandbox_vm_backend_t *backend;
  void *backend_session;
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

int ant_sandbox_vm_start(const ant_sandbox_vm_config_t *config) {
  if (!config) return -EINVAL;

  const ant_sandbox_vm_backend_t *backend = ant_sandbox_vm_default_backend();
  if (!backend) return -ENOSYS;

  if (backend->create_session && backend->execute_session && backend->destroy_session) {
    ant_sandbox_vm_session_t *session = NULL;
    int rc = ant_sandbox_vm_session_create(config, &session);
    if (rc == 0) {
      ant_sandbox_vm_request_t request = {
        .request_data = config->request_data,
        .request_len = config->request_len,
        .frame_handler = config->frame_handler,
        .frame_handler_user = config->frame_handler_user,
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

  const ant_sandbox_vm_backend_t *backend = ant_sandbox_vm_default_backend();
  if (!backend || !backend->create_session || !backend->execute_session || !backend->destroy_session) return -ENOSYS;

  ant_sandbox_vm_session_t *session = calloc(1, sizeof(*session));
  if (!session) return -ENOMEM;

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
  if (!session || !session->backend || !session->backend_session || !request) return -EINVAL;
  if (!session->backend->execute_session) return -ENOSYS;
  return session->backend->execute_session(session->backend_session, request);
}

void ant_sandbox_vm_session_destroy(ant_sandbox_vm_session_t *session) {
  if (!session) return;
  if (session->backend && session->backend->destroy_session && session->backend_session) {
    session->backend->destroy_session(session->backend_session);
  }
  free(session);
}
