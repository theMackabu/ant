#include "kvm_internal.h" // IWYU pragma: keep
#if defined(__linux__) && !defined(__x86_64__) && !defined(__aarch64__)

static int ant_kvm_unavailable_start(const ant_sandbox_vm_config_t *config) {
  if (config && config->result) {
    config->result->kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE;
    config->result->code = -ENOSYS;
  }
  fprintf(stderr, "sandbox vm: Linux KVM sandbox backend requires x86_64 or aarch64\n");
  return -ENOSYS;
}

static int ant_kvm_unavailable_create(const ant_sandbox_vm_config_t *config, void **session_out) {
  if (session_out) *session_out = NULL;
  return ant_kvm_unavailable_start(config);
}

const ant_sandbox_vm_backend_t ant_sandbox_vm_linux_backend = {
  .name = "kvm",
  .start = ant_kvm_unavailable_start,
  .create_session = ant_kvm_unavailable_create,
};

#endif
