#pragma once

#include <compat.h> // IWYU pragma: keep

#include "virtio.h"

#define ANT_VIRTIO_RNG_QUEUE_SIZE 32u

int ant_hvf_virtio_rng_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev);
