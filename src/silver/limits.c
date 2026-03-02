#include "silver/vm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <pthread.h>
#endif

#define SV_DEFAULT_STACK_KB  984
#define SV_ASYNC_STACK_KB    64
#define SV_BYTES_PER_SLOT    ((int)sizeof(uint64_t))

int sv_user_stack_size_kb = 0;

static size_t os_thread_stack_size(void) {
#ifdef _WIN32
  return 1024 * 1024;
#else
  struct rlimit rl;
  if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
    return (size_t)rl.rlim_cur;

  pthread_attr_t attr;
  size_t sz = 0;
  if (pthread_attr_init(&attr) == 0) {
    pthread_attr_getstacksize(&attr, &sz);
    pthread_attr_destroy(&attr);
    if (sz > 0) return sz;
  }

  return 8 * 1024 * 1024;
#endif
}

void sv_vm_limits(sv_vm_kind_t kind, int *out_stack_size, int *out_max_frames) {
  if (kind == SV_VM_ASYNC) {
    *out_stack_size = (SV_ASYNC_STACK_KB * 1024) / SV_BYTES_PER_SLOT;
    *out_max_frames = 256;
    return;
  }

  int stack_kb = (sv_user_stack_size_kb > 0)
    ? sv_user_stack_size_kb
    : SV_DEFAULT_STACK_KB;

  int slots = (stack_kb * 1024) / SV_BYTES_PER_SLOT;
  int frames = stack_kb * 10;
  if (frames > slots) frames = slots;

  *out_stack_size = slots;
  *out_max_frames = frames;
}
