#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ant.h"
#include "gc.h"
#include "runtime.h"
#include "modules/builtin.h"

static struct {
  struct js *js;
  jsval_t handler;
} signal_handlers[NSIG] = {0};

static void general_signal_handler(int signum) {
  if (signum < 0 || signum >= NSIG) return;
  struct js *js = signal_handlers[signum].js;
  jsval_t handler = signal_handlers[signum].handler;
  
  if (js && js_type(handler) != JS_UNDEF) {
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
  (void) args; (void) nargs;

  size_t heap_before = GC_get_heap_size();
  size_t used_before = GC_get_heap_size() - GC_get_free_bytes();
  size_t arena_before = js_getbrk(js);
  
  size_t arena_freed = js_gc_compact(js);
  size_t arena_after = js_getbrk(js);
  
  size_t heap_after = GC_get_heap_size();
  size_t used_after = GC_get_heap_size() - GC_get_free_bytes();
  size_t freed = (used_before > used_after) ? (used_before - used_after) : 0;

  jsval_t result = js_mkobj(js);
  js_set(js, result, "heapBefore", js_mknum((double)heap_before));
  js_set(js, result, "heapAfter", js_mknum((double)heap_after));
  js_set(js, result, "usedBefore", js_mknum((double)used_before));
  js_set(js, result, "usedAfter", js_mknum((double)used_after));
  js_set(js, result, "freed", js_mknum((double)freed));
  js_set(js, result, "arenaBefore", js_mknum((double)arena_before));
  js_set(js, result, "arenaAfter", js_mknum((double)arena_after));
  js_set(js, result, "arenaFreed", js_mknum((double)arena_freed));

  return result;
}

// Ant.alloc()
static jsval_t js_alloc(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  
  jsval_t result = js_mkobj(js);
  size_t arena_size = js_getbrk(js);
  
  js_set(js, result, "arenaSize", js_mknum((double)arena_size));
  js_set(js, result, "heapSize", js_mknum((double)GC_get_heap_size()));
  js_set(js, result, "freeBytes", js_mknum((double)GC_get_free_bytes()));
  js_set(js, result, "usedBytes", js_mknum((double)(GC_get_heap_size() - GC_get_free_bytes())));
  js_set(js, result, "totalBytes", js_mknum((double)GC_get_total_bytes()));
  
  return result;
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
  
  size_t arena_total = 0, arena_lwm = 0, cstack = 0;
  js_stats(js, &arena_total, &arena_lwm, &cstack);
  jsval_t result = js_mkobj(js);
  
  js_set(js, result, "arenaUsed", js_mknum((double)arena_total));
  js_set(js, result, "arenaLwm", js_mknum((double)arena_lwm));
  js_set(js, result, "cstack", js_mknum((double)cstack));
  
  js_set(js, result, "gcHeapSize", js_mknum((double)GC_get_heap_size()));
  js_set(js, result, "gcFreeBytes", js_mknum((double)GC_get_free_bytes()));
  js_set(js, result, "gcUsedBytes", js_mknum((double)(GC_get_heap_size() - GC_get_free_bytes())));
  
  return result;
}

void init_builtin_module() {
  struct js *js = rt->js;
  jsval_t ant_obj = rt->ant_obj;

  js_set(js, ant_obj, "gc", js_mkfun(js_gc_trigger));
  js_set(js, ant_obj, "alloc", js_mkfun(js_alloc));
  js_set(js, ant_obj, "stats", js_mkfun(js_stats_fn));
  js_set(js, ant_obj, "signal", js_mkfun(js_signal));
  js_set(js, ant_obj, "sleep", js_mkfun(js_sleep));
  js_set(js, ant_obj, "msleep", js_mkfun(js_msleep));
  js_set(js, ant_obj, "usleep", js_mkfun(js_usleep));

  jsval_t raw_obj = js_mkobj(js);
  js_set(js, raw_obj, "typeof", js_mkfun(js_raw_typeof));
  js_set(js, ant_obj, "raw", raw_obj);
}