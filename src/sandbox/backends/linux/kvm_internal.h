#pragma once

#if defined(__linux__)

#include "backend.h"
#include "sandbox/sandbox.h"

#include <stdatomic.h>
#include <limits.h>

enum {
  ANT_KVM_HVM_START_MAGIC = 0x336ec578u,
  ANT_KVM_HVM_MEMMAP_TYPE_RAM = 1u,
  ANT_KVM_PIO_IN = 0,
  ANT_KVM_PIO_OUT = 1,
};

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t flags;
  uint32_t nr_modules;
  uint64_t modlist_paddr;
  uint64_t cmdline_paddr;
  uint64_t rsdp_paddr;
  uint64_t memmap_paddr;
  uint32_t memmap_entries;
  uint32_t reserved;
} __attribute__((packed)) ant_kvm_hvm_start_info_t;

typedef struct {
  uint64_t addr;
  uint64_t size;
  uint32_t type;
  uint32_t reserved;
} __attribute__((packed)) ant_kvm_hvm_memmap_entry_t;

typedef struct {
  ant_hvf_vm_t vm;
  bool irqchip_created;
  bool memory_registered;
} ant_kvm_session_t;

typedef struct {
  ant_hvf_vm_t *vm;
  struct timespec start;
  unsigned int timeout_ms;
  bool timeout_until_request_sent;
  atomic_bool stop;
} ant_kvm_deadline_t;

void ant_kvm_install_wakeup_signal(void);
void *ant_kvm_deadline_thread(void *opaque);
void ant_kvm_set_result(ant_sandbox_vm_result_t *result, ant_sandbox_vm_result_kind_t kind, int code);
void ant_kvm_classify_result(ant_hvf_vm_t *vm, ant_sandbox_vm_result_t *result, int rc);
int ant_kvm_session_send(void *opaque, const void *data, size_t len);

int ant_kvm_ioctl(int fd, unsigned long req, void *arg, const char *op);
int ant_kvm_find_symbol(const char *path, const char *name, uint64_t *value_out);
uint64_t ant_kvm_elapsed_ms(const struct timespec *start, const struct timespec *now);
uint64_t ant_kvm_cpu_time_ns(ant_hvf_vm_t *vm);

#endif
