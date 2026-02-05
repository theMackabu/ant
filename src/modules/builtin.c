#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "modules/builtin.h"
#include "modules/buffer.h"
#include "modules/collections.h"

static struct {
  struct js *js;
  jsval_t handler;
} signal_handlers[NSIG] = {0};

static void general_signal_handler(int signum) {
  if (signum < 0 || signum >= NSIG) return;
  struct js *js = signal_handlers[signum].js;
  jsval_t handler = signal_handlers[signum].handler;
  
  if (js && vtype(handler) != T_UNDEF) {
    jsval_t args[] = {js_mknum(signum)};
    js_call(js, handler, args, 1);
  }
  
  exit(0);
}

// Ant.signal(signal, handler)
static jsval_t js_signal(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Ant.signal() requires 2 arguments");
  
  int signum = (int)js_getnum(args[0]);
  if (signum <= 0 || signum >= NSIG) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid signal number: %d", signum);
  }
  
  signal_handlers[signum].js = js;
  signal_handlers[signum].handler = args[1];
  signal(signum, general_signal_handler);
  
  return js_mkundef();
}

// Ant.gc()
static jsval_t js_gc_trigger(struct js *js, jsval_t *args, int nargs) {
  js->needs_gc = true;
  return js_mkundef();
}

// Ant.raw.typeof(jsval_t)
static jsval_t js_raw_typeof(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.raw.typeof() requires 1 argument");
  const uint8_t type = vtype(args[0]);
  return js_mknum((double)type);
}

// Ant.sleep(seconds)
static jsval_t js_sleep(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.sleep() requires 1 argument");
  unsigned int seconds = (unsigned int)js_getnum(args[0]);
  sleep(seconds);
  return js_mkundef();
}

// Ant.msleep(milliseconds)
static jsval_t js_msleep(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.msleep() requires 1 argument");
  long ms = (long)js_getnum(args[0]);
  struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
  struct timespec rem;
  while (nanosleep(&ts, &rem) == -1 && errno == EINTR) ts = rem;
  return js_mkundef();
}

// Ant.usleep(microseconds)
static jsval_t js_usleep(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.usleep() requires 1 argument");
  useconds_t us = (useconds_t)js_getnum(args[0]);
  usleep(us);
  return js_mkundef();
}

// Ant.stats()
static jsval_t js_stats_fn(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  jsval_t result = js_newobj(js);
  
  js_set(js, result, "arenaUsed", js_mknum((double)js_getbrk(js)));
  js_set(js, result, "arenaSize", js_mknum((double)js->size));
  
  size_t buffer_mem = buffer_get_external_memory();
  size_t code_mem = code_arena_get_memory();
  size_t collections_mem = collections_get_external_memory();
  size_t external_total = buffer_mem + code_mem + collections_mem;
  
  jsval_t ext = js_newobj(js);
  js_set(js, ext, "buffers", js_mknum((double)buffer_mem));
  js_set(js, ext, "code", js_mknum((double)code_mem));
  js_set(js, ext, "collections", js_mknum((double)collections_mem));
  js_set(js, ext, "total", js_mknum((double)external_total));
  js_set(js, result, "external", ext);
  
  if (js->cstk != NULL) {
    volatile char marker;
    uintptr_t base = (uintptr_t)js->cstk;
    uintptr_t curr = (uintptr_t)&marker;
    size_t used = (base > curr) ? (base - curr) : (curr - base);
    js_set(js, result, "cstack", js_mknum((double)used));
  } else js_set(js, result, "cstack", js_mknum(0));
  
#ifdef __APPLE__
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
    js_set(js, result, "rss", js_mknum((double)info.resident_size));
    js_set(js, result, "virtualSize", js_mknum((double)info.virtual_size));
  }
#elif defined(__linux__)
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    js_set(js, result, "rss", js_mknum((double)usage.ru_maxrss * 1024));
  }
#endif
  
  return result;
}

void init_builtin_module() {
  struct js *js = rt->js;
  jsval_t ant_obj = rt->ant_obj;

  js_set(js, ant_obj, "gc", js_mkfun(js_gc_trigger));
  js_set(js, ant_obj, "stats", js_mkfun(js_stats_fn));
  js_set(js, ant_obj, "signal", js_mkfun(js_signal));
  js_set(js, ant_obj, "sleep", js_mkfun(js_sleep));
  js_set(js, ant_obj, "msleep", js_mkfun(js_msleep));
  js_set(js, ant_obj, "usleep", js_mkfun(js_usleep));

  jsval_t raw_obj = js_newobj(js);
  js_set(js, raw_obj, "typeof", js_mkfun(js_raw_typeof));
  js_set(js, ant_obj, "raw", raw_obj);
}