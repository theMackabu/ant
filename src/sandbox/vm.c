#include <compat.h> // IWYU pragma: keep

#include "sandbox/vm.h"

#include <errno.h>

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
  if (!backend || !backend->start) return -ENOSYS;

  return backend->start(config);
}
