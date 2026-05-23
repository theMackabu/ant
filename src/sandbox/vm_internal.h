#ifndef ANT_SANDBOX_VM_INTERNAL_H
#define ANT_SANDBOX_VM_INTERNAL_H

#include "sandbox/vm.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct ant_sandbox_vm_session {
  const ant_sandbox_vm_backend_t *backend;
  void *backend_session;
  pid_t helper_pid;
  int helper_cmd_fd;
  int helper_msg_fd;
  uint32_t capabilities;
  bool helper;
};

int ant_sandbox_vm_helper_create(
  const ant_sandbox_vm_backend_t *backend,
  const ant_sandbox_vm_config_t *config,
  ant_sandbox_vm_session_t **session_out
);
int ant_sandbox_vm_helper_execute(
  ant_sandbox_vm_session_t *session,
  const ant_sandbox_vm_request_t *request
);
void ant_sandbox_vm_helper_destroy(ant_sandbox_vm_session_t *session);

#endif
