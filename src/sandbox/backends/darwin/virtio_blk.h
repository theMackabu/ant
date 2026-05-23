#pragma once

#include <compat.h> // IWYU pragma: keep

#include "virtio.h"

#include <stdint.h>

#if defined(__aarch64__)

#define ANT_VIRTIO_BLK_T_IN 0u
#define ANT_VIRTIO_BLK_T_OUT 1u
#define ANT_VIRTIO_BLK_T_FLUSH 4u
#define ANT_VIRTIO_BLK_S_OK 0u
#define ANT_VIRTIO_BLK_S_IOERR 1u
#define ANT_VIRTIO_BLK_QUEUE_SIZE 8u

typedef struct {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
} ant_virtio_blk_req_t;

int ant_hvf_disk_read(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len);
int ant_hvf_disk_write(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len);
int ant_hvf_virtio_blk_request(ant_hvf_vm_t *vm,
                               uint64_t desc_base,
                               uint16_t head,
                               uint32_t *used_len);
int ant_hvf_virtio_blk_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev);

#endif
