#pragma once

#include <compat.h> // IWYU pragma: keep
#include "sandbox/vm.h"

#if defined(__aarch64__)

#include "fdt.h" // IWYU pragma: keep
#include "gic.h" // IWYU pragma: keep
#include "hvf.h"
#include "sandbox_backend/pci.h" // IWYU pragma: keep
#include "sandbox_backend/forward.h"
#include "sandbox_backend/nat.h"
#include "sandbox_backend/virtio.h"
#include "sandbox_backend/virtio_9p.h"
#include "sandbox_backend/virtio_blk.h" // IWYU pragma: keep
#include "sandbox_backend/virtio_net.h" // IWYU pragma: keep
#include "sandbox_backend/virtio_rng.h" // IWYU pragma: keep
#include "sandbox_backend/virtio_vsock.h"

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

#define ANT_HVF_ELF_MACHINE 183u
#define ANT_HVF_ELF_MACHINE_NAME "aarch64"

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
  int image_fd;
  uint64_t image_sectors;
  
  hv_vcpu_t vcpu;
  hv_vcpu_exit_t *vcpu_exit;
  ant_hvf_virtio_device_t blk;
  ant_hvf_virtio_device_t net;
  ant_hvf_virtio_device_t rng;
  
  bool net_enabled;
  bool net_started;
  
  ant_hvf_nat_t *net_nat;
  pthread_mutex_t net_lock;
  pthread_mutex_t virtio_lock;
  pthread_mutex_t vsock_lock;
  
  bool net_lock_init;
  bool virtio_lock_init;
  bool vsock_lock_init;
  bool net_rx_wake;
  
  uint8_t net_mac[ANT_HVF_MAC_BYTES];
  uint8_t net_guest_mac[ANT_HVF_MAC_BYTES];
  bool net_guest_mac_seen;
  
  uint32_t net_max_packet_size;
  uint32_t net_rx_head;
  uint32_t net_rx_count;
  
  ant_hvf_net_packet_t net_rx_packets[ANT_HVF_NET_RX_BACKLOG];
  const ant_sandbox_port_forward_t *net_forwards;
  size_t net_forward_count;
  
  ant_hvf_9p_device_t p9[ANT_HVF_VIRTIO_9P_MAX];
  size_t p9_count;
  
  ant_hvf_vsock_device_t vsock;
  ant_hvf_uart_capture_t uart;
  
  bool verbose;
  ant_sandbox_vm_frame_handler_t frame_handler;
  void *frame_handler_user;
  
  unsigned int timeout_ms;
  unsigned int boot_timeout_ms;
  unsigned int cpu_time_ms;
  atomic_bool timed_out;
  atomic_bool cpu_timed_out;
  atomic_bool canceled;
  atomic_bool vcpu_running;
  atomic_bool vsock_wake_pending;
  
  uint32_t rtc_load_value;
  time_t rtc_load_host;
  
  uint32_t rtc_match;
  uint32_t rtc_imsc;
  uint32_t rtc_icr;
  
  bool rtc_enabled;
  bool gic_msi_enabled;
  
  uint32_t gic_msi_base;
  uint32_t gic_msi_count;
  
  uint64_t cntfrq;
  uint32_t last_exit_reason;
  uint64_t last_exit_pc;
  uint64_t last_exit_esr;
  uint64_t last_exit_ipa;
  uint64_t last_exit_va;
};

typedef struct {
  ant_hvf_vm_t *vm;
  unsigned int timeout_ms;
  bool until_request_sent;
  atomic_bool stop;
  struct timespec started_at;
} ant_hvf_timeout_t;

void ant_hvf_uart_discard(ant_hvf_vm_t *vm);
void ant_hvf_uart_report_panic(ant_hvf_vm_t *vm);
bool ant_hvf_uart_has_panic(ant_hvf_vm_t *vm);
void ant_hvf_uart_put(ant_hvf_vm_t *vm, uint8_t byte);

uint32_t ant_bswap32(uint32_t x);
uint64_t ant_bswap64(uint64_t x);

size_t ant_align4(size_t n);
size_t ant_align_page(size_t n);

int ant_hvf_check(hv_return_t ret, const char *op);
int ant_hvf_send_msi(ant_hvf_vm_t *vm, uint64_t addr, uint32_t data);

void ant_hvf_wake_vcpu(ant_hvf_vm_t *vm);
int ant_hvf_check_file(const char *kind, const char *path, off_t *size_out);
void ant_hvf_verbose(ant_hvf_vm_t *vm, const char *message);

void ant_hvf_verbosef(ant_hvf_vm_t *vm, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

uint16_t ant_hvf_load16(const unsigned char *p);
uint32_t ant_hvf_load32(const unsigned char *p);
uint64_t ant_hvf_load64(const unsigned char *p);

void ant_hvf_store16(unsigned char *p, uint16_t value);
void ant_hvf_store32(unsigned char *p, uint32_t value);
void ant_hvf_store64(unsigned char *p, uint64_t value);

void *ant_hvf_guest_ptr(ant_hvf_vm_t *vm, uint64_t guest_addr, size_t len);
int ant_hvf_guest_read(ant_hvf_vm_t *vm, uint64_t guest_addr, void *out, size_t len);
int ant_hvf_guest_write(ant_hvf_vm_t *vm, uint64_t guest_addr, const void *src, size_t len);
int ant_read_all(int fd, void *buf, size_t len, off_t off);
int ant_hvf_load_kernel(ant_hvf_vm_t *vm, const char *path);

void *ant_hvf_sym(const char *name);
int ant_hvf_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t value, const char *name);

uint64_t ant_hvf_host_cntfrq(void);
uint64_t ant_hvf_host_cntvct(void);

int ant_hvf_init_vcpu(ant_hvf_vm_t *vm);
int ant_hvf_handle_wfx(ant_hvf_vm_t *vm);
int ant_hvf_sync_vtimer(ant_hvf_vm_t *vm);
int ant_hvf_raise_vtimer(ant_hvf_vm_t *vm, const char *where);

uint64_t ant_hvf_select_width(uint64_t value, unsigned offset, unsigned size);
void ant_hvf_assign_width(uint32_t *target, unsigned offset, unsigned size, uint64_t value);
void ant_hvf_assign_width16(uint16_t *target, unsigned offset, unsigned size, uint64_t value);
void ant_hvf_assign_width64(uint64_t *target, unsigned offset, unsigned size, uint64_t value);

bool ant_hvf_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value);
bool ant_hvf_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value);

int ant_hvf_advance_pc(hv_vcpu_t vcpu);
int ant_hvf_handle_mmio(ant_hvf_vm_t *vm, hv_vcpu_exit_exception_t *ex);

#endif
