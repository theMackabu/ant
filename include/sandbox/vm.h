#ifndef ANT_SANDBOX_VM_H
#define ANT_SANDBOX_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ANT_SANDBOX_DEFAULT_BOOT_TIMEOUT_MS 10000u
#define ANT_SANDBOX_MIN_MEMORY_SIZE (64ull * 1024ull * 1024ull)
#define ANT_SANDBOX_DEFAULT_MEMORY_SIZE (256ull * 1024ull * 1024ull)
#define ANT_SANDBOX_CPU_TIME_LIMIT_CODE (-1001)
typedef struct ant_sandbox_vm_session ant_sandbox_vm_session_t;

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

typedef bool (*ant_sandbox_vm_frame_handler_t)(
  uint8_t type,
  const void *payload,
  size_t payload_len,
  void *user
);

typedef enum {
  ANT_SANDBOX_VM_RESULT_NONE = 0,
  ANT_SANDBOX_VM_RESULT_GUEST_EXIT,
  ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE,
  ANT_SANDBOX_VM_RESULT_CONFIG_ERROR,
  ANT_SANDBOX_VM_RESULT_TIMEOUT,
  ANT_SANDBOX_VM_RESULT_CPU_TIME_LIMIT,
  ANT_SANDBOX_VM_RESULT_KERNEL_PANIC,
  ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR,
  ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR,
  ANT_SANDBOX_VM_RESULT_CANCELED,
  ANT_SANDBOX_VM_RESULT_VM_ERROR,
} ant_sandbox_vm_result_kind_t;

typedef struct {
  ant_sandbox_vm_result_kind_t kind;
  int code;
} ant_sandbox_vm_result_t;

typedef struct {
  uint64_t cpu_time_ns;
  uint64_t wall_time_ns;
  uint64_t resident_memory_bytes;
  bool resident_memory_available;
} ant_sandbox_vm_stats_t;

typedef struct {
  const void *request_data;
  size_t request_len;
  ant_sandbox_vm_frame_handler_t frame_handler;
  void *frame_handler_user;
  ant_sandbox_vm_result_t *result;
} ant_sandbox_vm_request_t;

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
  unsigned int boot_timeout_ms;
  unsigned int cpu_time_ms;
  bool verbose;
  ant_sandbox_vm_frame_handler_t frame_handler;
  void *frame_handler_user;
  ant_sandbox_vm_result_t *result;
} ant_sandbox_vm_config_t;

typedef struct ant_sandbox_vm_backend {
  const char *name;
  int (*start)(const ant_sandbox_vm_config_t *config);
  int (*create_session)(const ant_sandbox_vm_config_t *config, void **session_out);
  int (*execute_session)(void *session, const ant_sandbox_vm_request_t *request);
  int (*send_session)(void *session, const void *data, size_t len);
  int (*get_stats_session)(void *session, ant_sandbox_vm_stats_t *stats);
  int (*cancel_session)(void *session);
  void (*destroy_session)(void *session);
} ant_sandbox_vm_backend_t;

extern const ant_sandbox_vm_backend_t ant_sandbox_vm_darwin_backend;
extern const ant_sandbox_vm_backend_t ant_sandbox_vm_linux_backend;
extern const ant_sandbox_vm_backend_t ant_sandbox_vm_windows_backend;

const char *ant_sandbox_vm_backend_name(const ant_sandbox_vm_backend_t *backend);
const char *ant_sandbox_vm_result_name(ant_sandbox_vm_result_kind_t kind);
const ant_sandbox_vm_backend_t *ant_sandbox_vm_default_backend(void);

void ant_sandbox_vm_session_destroy(ant_sandbox_vm_session_t *session);
void ant_sandbox_vm_result_clear(ant_sandbox_vm_result_t *result);

bool ant_sandbox_vm_supported(void);
bool ant_sandbox_vm_helper_is_process(const char *argv0);
bool ant_sandbox_vm_result_is_infrastructure_failure(const ant_sandbox_vm_result_t *result);

int ant_sandbox_vm_helper_process_main(void);
int ant_sandbox_vm_start(const ant_sandbox_vm_config_t *config);
int ant_sandbox_vm_session_cancel(ant_sandbox_vm_session_t *session);
int ant_sandbox_vm_session_execute(ant_sandbox_vm_session_t *session, const ant_sandbox_vm_request_t *request);
int ant_sandbox_vm_session_send(ant_sandbox_vm_session_t *session, const void *data, size_t len);
int ant_sandbox_vm_session_stats(ant_sandbox_vm_session_t *session, ant_sandbox_vm_stats_t *stats);
int ant_sandbox_vm_session_create(const ant_sandbox_vm_config_t *config, ant_sandbox_vm_session_t **session_out);

#endif
