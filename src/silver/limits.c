#include "silver/vm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/resource.h>
#include <pthread.h>
#endif

#define SV_DEFAULT_STACK_KB  984
#define SV_ASYNC_STACK_KB    64
#define SV_BYTES_PER_SLOT    ((int)sizeof(uint64_t))

int sv_user_stack_size_kb = 0;

size_t os_thread_stack_size(void) {
#ifdef _WIN32
  ULONG_PTR low, high;
  GetCurrentThreadStackLimits(&low, &high);
  return (size_t)(high - low);
#elif defined(__APPLE__)
  return pthread_get_stacksize_np(pthread_self());
#else
  pthread_attr_t attr;
  size_t sz = 0;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    pthread_attr_getstacksize(&attr, &sz);
    pthread_attr_destroy(&attr);
    if (sz > 0) return sz;
  }

  struct rlimit rl;
  if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
    return (size_t)rl.rlim_cur;

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
