#include <compat.h> // IWYU pragma: keep

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <uv.h>

#include "errors.h"
#include "internal.h"
#include "interrupt.h"
#include "modules/vm_interrupt.h"

enum { VM_FIRED_TIMEOUT = 1, VM_FIRED_SIGINT = 2 };
static vm_interrupt_t *_Atomic vm_sigint_run;

static void vm_interrupt_fire(vm_interrupt_t *it, uint8_t why) {
  uint8_t none = 0;
  if (atomic_compare_exchange_strong(&it->fired, &none, why))
    atomic_store_explicit(&it->js->interrupt_pending, 1, memory_order_relaxed);
}

static void vm_watchdog(void *arg) {
  vm_interrupt_t *it = arg;
  uint64_t deadline = uv_hrtime() + (uint64_t)it->timeout_ms * 1000000u;
  
  uv_mutex_lock(&it->lock);
  while (!it->done) {
    uint64_t now = uv_hrtime();
    
    if (now >= deadline || uv_cond_timedwait(&it->cond, &it->lock, deadline - now) == UV_ETIMEDOUT) {
      if (!it->done) vm_interrupt_fire(it, VM_FIRED_TIMEOUT);
      break;
    }
  }
  
  uv_mutex_unlock(&it->lock);
}

static void vm_sigint_handler(int sig) {
  (void)sig;
  vm_interrupt_t *it = atomic_load(&vm_sigint_run);
  if (it) vm_interrupt_fire(it, VM_FIRED_SIGINT);
}

void vm_interrupt_begin(vm_interrupt_t *it) {
  if (!it->timeout_ms && !it->sigint) return;
  sv_jit_arm_interrupts(it->js);
  
  it->outer = it->js->interrupt_run;
  it->js->interrupt_run = it;

  if (it->timeout_ms) {
    uv_mutex_init(&it->lock);
    uv_cond_init(&it->cond);
    it->watching = uv_thread_create(&it->thread, vm_watchdog, it) == 0;
  }

  if (it->sigint) {
    it->outer_sigint = atomic_exchange(&vm_sigint_run, it);
  #ifndef _WIN32
    struct sigaction action = {0};
    action.sa_handler = vm_sigint_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &it->prev_sigint);
  #else
    it->prev_sigint = signal(SIGINT, vm_sigint_handler);
  #endif
  }
}

static bool vm_interrupt_outer_fired(vm_interrupt_t *it) {
  for (vm_interrupt_t *outer = it->outer; outer; outer = outer->outer)
    if (atomic_load(&outer->fired)) return true;
  return false;
}

static uint8_t vm_interrupt_stop(vm_interrupt_t *it) {
  ant_t *js = it->js;
  js->interrupt_run = it->outer;

  if (it->sigint) {
  #ifndef _WIN32
    sigaction(SIGINT, &it->prev_sigint, NULL);
  #else
    signal(SIGINT, it->prev_sigint);
  #endif
    atomic_store(&vm_sigint_run, it->outer_sigint);
  }

  if (it->timeout_ms) {
    uv_mutex_lock(&it->lock);
    it->done = true;
    uv_cond_signal(&it->cond);
    uv_mutex_unlock(&it->lock);
    
    if (it->watching) uv_thread_join(&it->thread);
    uv_cond_destroy(&it->cond);
    uv_mutex_destroy(&it->lock);
  }

  return atomic_load(&it->fired);
}

void vm_interrupt_unwind(ant_t *js, vm_interrupt_t *outer) {
  while (js->interrupt_run && js->interrupt_run != outer) vm_interrupt_stop(js->interrupt_run);
}

ant_value_t vm_interrupt_end(vm_interrupt_t *it, ant_value_t result) {
  if (!it->timeout_ms && !it->sigint) return result;
  ant_t *js = it->js;

  uint8_t fired = vm_interrupt_stop(it);
  if (!fired) return result;

  if (is_err(result)) js_take_thrown(js, result);
  atomic_store_explicit(&js->interrupt_pending, 0, memory_order_relaxed);
  
  js->terminating = vm_interrupt_outer_fired(it);
  if (js->terminating) atomic_store_explicit(&js->interrupt_pending, 1, memory_order_relaxed);

  char message[96];
  if (fired == VM_FIRED_TIMEOUT) {
    snprintf(message, sizeof(message), "Script execution timed out after %ums", (unsigned)it->timeout_ms);
    return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_SCRIPT_EXECUTION_TIMEOUT", message);
  }
  
  return js_mkerr_code(js, JS_ERR_GENERIC, "ERR_SCRIPT_EXECUTION_INTERRUPTED", "Script execution was interrupted by `SIGINT`");
}
