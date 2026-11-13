#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <uv.h>
#include "types.h"

struct vm_interrupt {
  ant_t *js;
  uint32_t timeout_ms;
  bool sigint;
  _Atomic uint8_t fired;
  
  uv_thread_t thread;
  uv_mutex_t lock;
  uv_cond_t cond;
  bool done;
  bool watching;
  
  struct vm_interrupt *outer;
  struct vm_interrupt *outer_sigint;
#ifndef _WIN32
  struct sigaction prev_sigint;
#else
  void (*prev_sigint)(int);
#endif
};

void vm_interrupt_begin(vm_interrupt_t *it);
ant_value_t vm_interrupt_end(vm_interrupt_t *it, ant_value_t result);
