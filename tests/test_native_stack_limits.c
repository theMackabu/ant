#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#include "silver/vm.h"

static void *check_worker_stack(void *unused) {
  (void)unused;
  pthread_attr_t attr;
  size_t expected;
  assert(pthread_getattr_np(pthread_self(), &attr) == 0);
  assert(pthread_attr_getstacksize(&attr, &expected) == 0);
  assert(pthread_attr_destroy(&attr) == 0);
  assert(os_thread_stack_size() == expected);
  assert(expected < 1024 * 1024);
  return NULL;
}

int main(void) {
  struct rlimit original;
  assert(getrlimit(RLIMIT_STACK, &original) == 0);
  struct rlimit limit = original;
  limit.rlim_cur = 1024 * 1024;
  assert(setrlimit(RLIMIT_STACK, &limit) == 0);
  assert(os_thread_stack_size() == limit.rlim_cur);

  pthread_attr_t attr;
  pthread_t worker;
  assert(pthread_attr_init(&attr) == 0);
  assert(pthread_attr_setstacksize(&attr, 256 * 1024) == 0);
  int error = pthread_create(&worker, &attr, check_worker_stack, NULL);
  if (error != 0) {
    fprintf(stderr, "pthread_create: %s (%d)\n", strerror(error), error);
    return 1;
  }
  assert(pthread_attr_destroy(&attr) == 0);
  assert(pthread_join(worker, NULL) == 0);

  if (original.rlim_max == RLIM_INFINITY) {
    limit.rlim_cur = RLIM_INFINITY;
    assert(setrlimit(RLIMIT_STACK, &limit) == 0);
    assert(os_thread_stack_size() == 8 * 1024 * 1024);
  }
  assert(setrlimit(RLIMIT_STACK, &original) == 0);
  puts("native stack limits passed");
  return 0;
}
