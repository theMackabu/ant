#ifndef ANT_SANDBOX_VM_H
#define ANT_SANDBOX_VM_H

#include <stdbool.h>

typedef struct {
  const char *image_path;
  const char *kernel_path;
  const char *request_json;
  const char *shared_dir_path;
  const char *shared_dir_tag;
  bool shared_dir_readonly;
  unsigned int cpu_count;
  unsigned long long memory_size;
  unsigned int timeout_ms;
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
