#ifndef ANT_SANDBOX_VM_INTERNAL_H
#define ANT_SANDBOX_VM_INTERNAL_H

#include "sandbox/vm.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdatomic.h>

struct ant_sandbox_vm_session {
  const ant_sandbox_vm_backend_t *backend;
  void *backend_session;
  pid_t helper_pid;
  int helper_cmd_fd;
  int helper_msg_fd;
  int helper_stats_fd;
  uint32_t capabilities;
  bool verbose;
  bool helper;
  pthread_mutex_t helper_cmd_mutex;
  bool helper_cmd_mutex_init;
  atomic_bool helper_cancel_requested;
  uint64_t created_at_ns;
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

int ant_sandbox_vm_helper_send(ant_sandbox_vm_session_t *session, const void *data, size_t len);
int ant_sandbox_vm_helper_stats(ant_sandbox_vm_session_t *session, ant_sandbox_vm_stats_t *stats);
int ant_sandbox_vm_helper_cancel(ant_sandbox_vm_session_t *session);
void ant_sandbox_vm_helper_destroy(ant_sandbox_vm_session_t *session);

#endif
