#pragma once

#if defined(__linux__)

#include <compat.h> // IWYU pragma: keep

#include "sandbox/vm.h"
#include "sandbox_backend/forward.h"
#include "sandbox_backend/nat.h"
#include "sandbox_backend/virtio.h"
#include "sandbox_backend/virtio_9p.h"
#include "sandbox_backend/virtio_blk.h"
#include "sandbox_backend/virtio_net.h"
#include "sandbox_backend/virtio_rng.h"
#include "sandbox_backend/virtio_vsock.h"
#include "sandbox_backend/pci.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
#  define ANT_HVF_GUEST_BASE 0x00000000ull
#  define ANT_HVF_PVH_START_INFO 0x00100000ull
#  define ANT_HVF_PVH_MEMMAP 0x00101000ull
#  define ANT_HVF_KERNEL_BASE 0x00200000ull
#  define ANT_HVF_VIRTIO_BLK_BAR 0xf0001000ull
#  define ANT_HVF_VIRTIO_BLK_SLOT 1u
#  define ANT_HVF_VIRTIO_NET_BAR 0xf0002000ull
#  define ANT_HVF_VIRTIO_NET_SLOT 2u
#  define ANT_HVF_VIRTIO_9P_BAR_BASE 0xf0003000ull
#  define ANT_HVF_VIRTIO_9P_SLOT_BASE 3u
#  define ANT_HVF_VIRTIO_VSOCK_BAR 0xf000b000ull
#  define ANT_HVF_VIRTIO_VSOCK_SLOT 11u
#  define ANT_HVF_VIRTIO_RNG_BAR 0xf000c000ull
#  define ANT_HVF_VIRTIO_RNG_SLOT 12u
#  define ANT_HVF_ELF_MACHINE 62u
#  define ANT_HVF_ELF_MACHINE_NAME "x86_64"
#  define ANT_KVM_COM1_PORT 0x3f8u
#  define ANT_KVM_PCI_ADDR_PORT 0x0cf8u
#  define ANT_KVM_PCI_DATA_PORT 0x0cfcu
#elif defined(__aarch64__)
#  define ANT_HVF_GUEST_BASE 0x40000000ull
#  define ANT_HVF_DTB_BASE 0x40000000ull
#  define ANT_HVF_KERNEL_BASE 0x40400000ull
#  define ANT_HVF_UART_BASE 0x09000000ull
#  define ANT_HVF_RTC_BASE 0x09010000ull
#  define ANT_HVF_PCIE_MMIO_BASE 0x10000000ull
#  define ANT_HVF_PCIE_PIO_BASE 0x3eff0000ull
#  define ANT_HVF_PCIE_ECAM_BASE 0x3f000000ull
#  define ANT_HVF_PCIE_ECAM_SIZE 0x01000000ull
#  define ANT_HVF_VIRTIO_BLK_BAR 0x10041000ull
#  define ANT_HVF_VIRTIO_BLK_SLOT 1u
#  define ANT_HVF_VIRTIO_NET_BAR 0x10042000ull
#  define ANT_HVF_VIRTIO_NET_SLOT 2u
#  define ANT_HVF_VIRTIO_9P_BAR_BASE 0x10043000ull
#  define ANT_HVF_VIRTIO_9P_SLOT_BASE 3u
#  define ANT_HVF_VIRTIO_VSOCK_BAR 0x1004b000ull
#  define ANT_HVF_VIRTIO_VSOCK_SLOT 11u
#  define ANT_HVF_VIRTIO_RNG_BAR 0x1004c000ull
#  define ANT_HVF_VIRTIO_RNG_SLOT 12u
#  define ANT_HVF_GIC_DIST_BASE 0x08000000ull
#  define ANT_HVF_GIC_CPU_BASE 0x08010000ull
#  define ANT_HVF_GIC_REDIST_BASE 0x080a0000ull
#  define ANT_HVF_GIC_MSI_BASE 0x08020000ull
#  define ANT_HVF_GIC_MSI_SIZE 0x1000ull
#  define ANT_HVF_GIC_MSI_VECTOR_BASE 48u
#  define ANT_HVF_GIC_MSI_VECTOR_COUNT 64u
#  define ANT_HVF_ELF_MACHINE 183u
#  define ANT_HVF_ELF_MACHINE_NAME "aarch64"
#else
#  define ANT_HVF_GUEST_BASE 0x00000000ull
#  define ANT_HVF_KERNEL_BASE 0x00200000ull
#  define ANT_HVF_VIRTIO_BLK_BAR 0xf0001000ull
#  define ANT_HVF_VIRTIO_BLK_SLOT 1u
#  define ANT_HVF_VIRTIO_NET_BAR 0xf0002000ull
#  define ANT_HVF_VIRTIO_NET_SLOT 2u
#  define ANT_HVF_VIRTIO_9P_BAR_BASE 0xf0003000ull
#  define ANT_HVF_VIRTIO_9P_SLOT_BASE 3u
#  define ANT_HVF_VIRTIO_VSOCK_BAR 0xf000b000ull
#  define ANT_HVF_VIRTIO_VSOCK_SLOT 11u
#  define ANT_HVF_VIRTIO_RNG_BAR 0xf000c000ull
#  define ANT_HVF_VIRTIO_RNG_SLOT 12u
#  define ANT_HVF_ELF_MACHINE 0u
#  define ANT_HVF_ELF_MACHINE_NAME "supported Linux"
#endif

#define ANT_HVF_VIRTIO_9P_MAX 8u
#define ANT_HVF_PAGE_SIZE 0x1000ull

#define ANT_HVF_BYTES_U16 2u
#define ANT_HVF_BYTES_U32 4u
#define ANT_HVF_ELF_IDENT_BYTES 16u
#define ANT_HVF_MAC_BYTES 6u

typedef struct {
  unsigned char ident[ANT_HVF_ELF_IDENT_BYTES];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
} ant_elf64_ehdr_t;

typedef struct {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
} ant_elf64_phdr_t;

typedef struct {
  uint32_t name;
  unsigned char info;
  unsigned char other;
  uint16_t shndx;
  uint64_t value;
  uint64_t size;
} ant_elf64_sym_t;

typedef struct {
  uint32_t name;
  uint32_t type;
  uint64_t flags;
  uint64_t addr;
  uint64_t offset;
  uint64_t size;
  uint32_t link;
  uint32_t info;
  uint64_t addralign;
  uint64_t entsize;
} ant_elf64_shdr_t;

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
  uint64_t kas_offset_symbol;
  uint64_t kas_kern_offset_symbol;
  uint64_t pcie_ecam_base_symbol;
  uint64_t pvh_entry32;
  
  int image_fd;
  uint64_t image_sectors;
  int kvm_fd;
  int vm_fd;
  int vcpu_fd;
  int gic_fd;
  
  struct kvm_run *run;
  size_t run_mmap_size;
  uint32_t pci_addr;
  
  pthread_t vcpu_thread;
  atomic_bool vcpu_thread_valid;
  atomic_bool vcpu_running;
  atomic_bool vsock_wake_pending;
  clockid_t vcpu_clock_id;
  atomic_bool cpu_run_active;
  atomic_uint_fast64_t cpu_time_ns;
  atomic_uint_fast64_t cpu_run_base_ns;
  atomic_uint_fast64_t cpu_run_start_ns;
  
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
  
  atomic_bool canceled;
  atomic_bool cpu_timed_out;
  atomic_bool timeout_disarmed;
  atomic_bool timed_out;
  
  uint32_t rtc_load_value;
  time_t rtc_load_host;
  uint32_t rtc_match;
  uint32_t rtc_imsc;
  uint32_t rtc_icr;
  
  bool rtc_enabled;
  bool gic_msi_enabled;
  int gic_version;
  
  uint32_t gic_msi_base;
  uint32_t gic_msi_count;
  uint64_t cntfrq;
};

void ant_hvf_uart_discard(ant_hvf_vm_t *vm);
void ant_hvf_uart_report_panic(ant_hvf_vm_t *vm);
bool ant_hvf_uart_has_panic(ant_hvf_vm_t *vm);
void ant_hvf_uart_put(ant_hvf_vm_t *vm, uint8_t byte);
int ant_hvf_send_msi(ant_hvf_vm_t *vm, uint64_t addr, uint32_t data);
void ant_hvf_wake_vcpu(ant_hvf_vm_t *vm);

uint32_t ant_bswap32(uint32_t x);
uint64_t ant_bswap64(uint64_t x);
size_t ant_align4(size_t n);
size_t ant_align_page(size_t n);

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

uint64_t ant_hvf_select_width(uint64_t value, unsigned offset, unsigned size);
void ant_hvf_assign_width(uint32_t *target, unsigned offset, unsigned size, uint64_t value);
void ant_hvf_assign_width16(uint16_t *target, unsigned offset, unsigned size, uint64_t value);
void ant_hvf_assign_width64(uint64_t *target, unsigned offset, unsigned size, uint64_t value);

#endif
