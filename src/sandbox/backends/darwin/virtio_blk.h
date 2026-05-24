#pragma once

#include <compat.h> // IWYU pragma: keep

#include "virtio.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)

#define ANT_VIRTIO_BLK_F_SEG_MAX (1ull << 2)
#define ANT_VIRTIO_BLK_F_BLK_SIZE (1ull << 6)
#define ANT_VIRTIO_BLK_F_FLUSH (1ull << 9)

#define ANT_VIRTIO_BLK_T_IN 0u
#define ANT_VIRTIO_BLK_T_OUT 1u
#define ANT_VIRTIO_BLK_T_FLUSH 4u
#define ANT_VIRTIO_BLK_S_OK 0u
#define ANT_VIRTIO_BLK_S_IOERR 1u
#define ANT_VIRTIO_BLK_S_UNSUPP 2u
#define ANT_VIRTIO_BLK_SECTOR_SIZE 512u
#define ANT_VIRTIO_BLK_QUEUE_SIZE 64u
#define ANT_VIRTIO_BLK_SEG_MAX (ANT_VIRTIO_BLK_QUEUE_SIZE - 2u)
#define ANT_VIRTIO_BLK_CONFIG_LEN 24u
#define ANT_VIRTIO_BLK_DESC_BYTES 16u
#define ANT_VIRTIO_BLK_REQUEST_BYTES 16u
#define ANT_VIRTIO_BLK_FEATURES \
  (ANT_VIRTIO_BLK_F_SEG_MAX | ANT_VIRTIO_BLK_F_BLK_SIZE | ANT_VIRTIO_BLK_F_FLUSH)

typedef struct {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
} ant_virtio_blk_req_t;

void ant_hvf_virtio_blk_config(const ant_hvf_vm_t *vm, unsigned char *cfg, size_t cfg_len);
int ant_hvf_disk_read(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len);
int ant_hvf_disk_write(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len);
int ant_hvf_virtio_blk_request(ant_hvf_vm_t *vm,
                               uint64_t desc_base,
                               uint16_t queue_size,
                               uint16_t head,
                               uint32_t *used_len);
int ant_hvf_virtio_blk_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev);

#endif
