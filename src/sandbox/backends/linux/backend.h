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

#define ANT_HVF_GUEST_BASE 0x00000000ull
#define ANT_HVF_PVH_START_INFO 0x00100000ull
#define ANT_HVF_PVH_MEMMAP 0x00101000ull
#define ANT_HVF_KERNEL_BASE 0x00200000ull
#define ANT_HVF_VIRTIO_BLK_BAR 0xf0001000ull
#define ANT_HVF_VIRTIO_BLK_SLOT 1u
#define ANT_HVF_VIRTIO_NET_BAR 0xf0002000ull
#define ANT_HVF_VIRTIO_NET_SLOT 2u
#define ANT_HVF_VIRTIO_9P_BAR_BASE 0xf0003000ull
#define ANT_HVF_VIRTIO_9P_SLOT_BASE 3u
#define ANT_HVF_VIRTIO_9P_MAX 8u
#define ANT_HVF_VIRTIO_VSOCK_BAR 0xf000b000ull
#define ANT_HVF_VIRTIO_VSOCK_SLOT 11u
#define ANT_HVF_PAGE_SIZE 0x1000ull

#define ANT_HVF_BYTES_U16 2u
#define ANT_HVF_BYTES_U32 4u
#define ANT_HVF_ELF_IDENT_BYTES 16u
#define ANT_HVF_MAC_BYTES 6u
#define ANT_HVF_ELF_MACHINE 62u
#define ANT_HVF_ELF_MACHINE_NAME "x86_64"

#define ANT_KVM_COM1_PORT 0x3f8u
#define ANT_KVM_PCI_ADDR_PORT 0x0cf8u
#define ANT_KVM_PCI_DATA_PORT 0x0cfcu

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
  uint64_t pvh_entry32;
  int image_fd;
  uint64_t image_sectors;
  int kvm_fd;
  int vm_fd;
  int vcpu_fd;
  struct kvm_run *run;
  size_t run_mmap_size;
  uint32_t pci_addr;
  pthread_t vcpu_thread;
  bool vcpu_thread_valid;
  ant_hvf_virtio_device_t blk;
  ant_hvf_virtio_device_t net;
  bool net_enabled;
  bool net_started;
  ant_hvf_nat_t *net_nat;
  pthread_mutex_t net_lock;
  bool net_lock_init;
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
  bool timed_out;
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
