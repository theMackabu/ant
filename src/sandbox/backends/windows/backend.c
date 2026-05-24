#if defined(_WIN32)

#include "sandbox/vm.h"

#include <errno.h>

static int ant_windows_sandbox_unavailable(const ant_sandbox_vm_config_t *config) {
  if (config && config->result) {
    config->result->kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE;
    config->result->code = -ENOSYS;
  }
  return -ENOSYS;
}

static int ant_windows_sandbox_unavailable_create(const ant_sandbox_vm_config_t *config,
                                                  void **session_out) {
  if (session_out) *session_out = NULL;
  return ant_windows_sandbox_unavailable(config);
}

const ant_sandbox_vm_backend_t ant_sandbox_vm_windows_backend = {
  .name = "windows-unavailable",
  .start = ant_windows_sandbox_unavailable,
  .create_session = ant_windows_sandbox_unavailable_create,
};

#endif
