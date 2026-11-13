#include "darwin/backend.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__APPLE__)

#define PRODUCER_COUNT 4
#define FRAMES_PER_PRODUCER 1000

typedef struct {
  ant_hvf_vm_t *vm;
  uint32_t producer;
} producer_ctx_t;

static void *producer_main(void *opaque) {
  producer_ctx_t *ctx = opaque;
  for (uint32_t i = 0; i < FRAMES_PER_PRODUCER; i++) {
    uint32_t payload[2] = { ctx->producer, i };
    assert(ant_hvf_vsock_queue_frame(ctx->vm, payload, sizeof(payload)) == 0);
  }
  return NULL;
}

int main(void) {
  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  uint32_t payload = 1;
  assert(ant_hvf_vsock_queue_frame(&vm, &payload, sizeof(payload)) != 0);

  ant_devices_add_vsock(ant_hvf_devices(&vm), ANT_HVF_VIRTIO_VSOCK_SLOT, ANT_HVF_VIRTIO_VSOCK_BAR);

  pthread_t threads[PRODUCER_COUNT];
  producer_ctx_t contexts[PRODUCER_COUNT];
  for (uint32_t i = 0; i < PRODUCER_COUNT; i++) {
    contexts[i] = (producer_ctx_t){ .vm = &vm, .producer = i };
    assert(pthread_create(&threads[i], NULL, producer_main, &contexts[i]) == 0);
  }
  for (uint32_t i = 0; i < PRODUCER_COUNT; i++)
    assert(pthread_join(threads[i], NULL) == 0);

  assert(ant_hvf_vsock_pending_frames(&vm) == PRODUCER_COUNT * FRAMES_PER_PRODUCER);
  assert(atomic_load_explicit(&vm.vsock_wake_pending, memory_order_acquire));
  assert(ant_hvf_vsock_maybe_send_request(&vm) == 0);
  assert(!ant_hvf_vsock_request_sent(&vm));

  ant_hvf_vsock_fail_transport(&vm);
  assert(ant_hvf_vsock_transport_error(&vm));
  ant_hvf_vsock_begin_request(&vm);
  assert(!ant_hvf_vsock_transport_error(&vm));
  assert(!ant_hvf_vsock_exit_received(&vm));

  ant_hvf_vsock_clear_frames(&vm);
  assert(ant_hvf_vsock_pending_frames(&vm) == 0);
  assert(!atomic_load_explicit(&vm.vsock_wake_pending, memory_order_acquire));
  ant_devices_free(vm.devices);
  return 0;
}

#else

int main(void) {
  return 0;
}

#endif
