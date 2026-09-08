#if !defined(_WIN32) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include "silver/vm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(ANT_WASM_EMBED)
#include <sys/resource.h>
#include <pthread.h>
#endif

#if defined(__linux__) && !defined(ANT_WASM_EMBED)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define SV_DEFAULT_STACK_KB  984
#define SV_BYTES_PER_SLOT    ((int)sizeof(uint64_t))

int sv_user_stack_size_kb = 0;

size_t os_thread_stack_size(void) {
#ifdef ANT_WASM_EMBED
  return 1024 * 1024;
#elif defined(_WIN32)
  ULONG_PTR low, high;
  GetCurrentThreadStackLimits(&low, &high);
  return (size_t)(high - low);
#elif defined(__APPLE__)
  return pthread_get_stacksize_np(pthread_self());
#else
  struct rlimit rl;
#if defined(__linux__)
  // musl reports only the current mapping for the growable main-thread stack.
  // Worker stacks are fixed allocations and must retain their pthread size.
  if (syscall(SYS_gettid) == getpid() && getrlimit(RLIMIT_STACK, &rl) == 0) {
    return rl.rlim_cur == RLIM_INFINITY ? 8 * 1024 * 1024 : (size_t)rl.rlim_cur;
  }
#endif

  pthread_attr_t attr;
  size_t sz = 0;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    pthread_attr_getstacksize(&attr, &sz);
    pthread_attr_destroy(&attr);
    if (sz > 0) return sz;
  }

  if (
    getrlimit(RLIMIT_STACK, &rl) == 0 && 
    rl.rlim_cur != RLIM_INFINITY
  ) return (size_t)rl.rlim_cur;

  return 8 * 1024 * 1024;
#endif
}

void sv_vm_limits(int *out_stack_size, int *out_max_frames) {
  int stack_kb = (sv_user_stack_size_kb > 0)
    ? sv_user_stack_size_kb
    : SV_DEFAULT_STACK_KB;

  int slots = (stack_kb * 1024) / SV_BYTES_PER_SLOT;
  int frames = stack_kb * 10;
  if (frames > slots) frames = slots;

  *out_stack_size = slots;
  *out_max_frames = frames;
}
