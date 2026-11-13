#pragma once

#include <compat.h> // IWYU pragma: keep
#include "sandbox/vm.h"

#if defined(__aarch64__)

#include "hvf.h"
#include "ant_sandbox_devices.h" // IWYU pragma: keep

#include <Hypervisor/Hypervisor.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
  size_t limit;
  bool truncated;
} ant_hvf_uart_capture_t;

struct ant_hvf_vm {
  void *host_mem;
  size_t mem_size;
  uint64_t kernel_entry;
  uint64_t image_sectors;
  ant_devices_t *devices;

  hv_vcpu_t vcpu;
  hv_vcpu_exit_t *vcpu_exit;
  uint64_t cntfrq;

  pthread_mutex_t idle_lock;
  pthread_cond_t idle_cond;

  const ant_sandbox_port_forward_t *net_forwards;
  size_t net_forward_count;
  const uint16_t *net_allow_host_ports;
  size_t net_allow_host_port_count;
  size_t p9_count;

  ant_hvf_uart_capture_t uart;
  ant_sandbox_vm_frame_handler_t frame_handler;
  void *frame_handler_user;

  time_t rtc_load_host;
  uint64_t last_exit_pc;
  uint64_t last_exit_esr;
  uint64_t last_exit_ipa;
  uint64_t last_exit_va;

  int image_fd;
  unsigned int timeout_ms;
  unsigned int boot_timeout_ms;
  unsigned int cpu_time_ms;

  uint32_t rtc_load_value;
  uint32_t rtc_match;
  uint32_t rtc_imsc;
  uint32_t rtc_icr;

  uint32_t gic_msi_base;
  uint32_t gic_msi_count;
  uint32_t last_exit_reason;

  atomic_bool timed_out;
  atomic_bool cpu_timed_out;
  atomic_bool canceled;
  atomic_bool vcpu_running;
  atomic_bool vsock_wake_pending;

  bool idle_sync_init;
  bool idle_kicked;
  bool net_enabled;
  bool net_allow_host_all;
  bool net_allow_lan;
  bool rtc_enabled;
  bool gic_msi_enabled;
  bool verbose;

  uint8_t net_mac[ANT_HVF_MAC_BYTES];
};

typedef struct {
  ant_hvf_vm_t *vm;
  unsigned int timeout_ms;
  bool until_request_sent;
  bool stop;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct timespec started_at;
} ant_hvf_timeout_t;

void ant_hvf_uart_discard(ant_hvf_vm_t *vm);
void ant_hvf_uart_report_panic(ant_hvf_vm_t *vm);
bool ant_hvf_uart_has_panic(ant_hvf_vm_t *vm);
void ant_hvf_uart_put(ant_hvf_vm_t *vm, uint8_t byte);

int ant_hvf_check(hv_return_t ret, const char *op);
int ant_hvf_create_gic(ant_hvf_vm_t *vm);
int ant_hvf_send_msi(ant_hvf_vm_t *vm, uint64_t addr, uint32_t data);

void ant_hvf_wake_vcpu(ant_hvf_vm_t *vm);
int ant_hvf_check_file(const char *kind, const char *path, off_t *size_out);
void ant_hvf_verbose(ant_hvf_vm_t *vm, const char *message);

void ant_hvf_verbosef(ant_hvf_vm_t *vm, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

int ant_read_all(int fd, void *buf, size_t len, off_t off);
int ant_hvf_load_kernel(ant_hvf_vm_t *vm, const char *path);
int ant_hvf_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t value, const char *name);

int ant_hvf_guest_cntvct(ant_hvf_vm_t *vm, uint64_t *out);

int ant_hvf_init_vcpu(ant_hvf_vm_t *vm);
int ant_hvf_handle_wfx(ant_hvf_vm_t *vm);
int ant_hvf_sync_vtimer(ant_hvf_vm_t *vm);
int ant_hvf_raise_vtimer(ant_hvf_vm_t *vm);
void ant_hvf_report_exception(ant_hvf_vm_t *vm);
void ant_hvf_report_timeout(ant_hvf_vm_t *vm);

int ant_hvf_advance_pc(hv_vcpu_t vcpu);
int ant_hvf_handle_mmio(ant_hvf_vm_t *vm, hv_vcpu_exit_exception_t *ex);

#include "shared/bytes.h" // IWYU pragma: keep
#include "shared/dump.h" // IWYU pragma: keep
#include "shared/devices.h" // IWYU pragma: keep

#endif
