#ifndef ANT_SANDBOX_VM_H
#define ANT_SANDBOX_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint16_t host_port;
  uint16_t guest_port;
} ant_sandbox_port_forward_t;

typedef struct {
  const char *host_path;
  const char *guest_path;
  const char *tag;
  bool readonly;
} ant_sandbox_mount_t;

typedef struct {
  const char *image_path;
  const char *kernel_path;
  const void *request_data;
  size_t request_len;
  uint32_t capabilities;
  const ant_sandbox_mount_t *mounts;
  size_t mount_count;
  bool network_enabled;
  const ant_sandbox_port_forward_t *forwards;
  size_t forward_count;
  unsigned int cpu_count;
  unsigned long long memory_size;
  unsigned int timeout_ms;
  bool verbose;
} ant_sandbox_vm_config_t;

typedef struct ant_sandbox_vm_backend {
  const char *name;
  int (*start)(const ant_sandbox_vm_config_t *config);
} ant_sandbox_vm_backend_t;

extern const ant_sandbox_vm_backend_t ant_sandbox_vm_darwin_backend;

const char *ant_sandbox_vm_backend_name(const ant_sandbox_vm_backend_t *backend);
const ant_sandbox_vm_backend_t *ant_sandbox_vm_default_backend(void);
bool ant_sandbox_vm_supported(void);
int ant_sandbox_vm_start(const ant_sandbox_vm_config_t *config);

#endif
