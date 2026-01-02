#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>

#include "ant.h"
#include "gc.h"
#include "runtime.h"
#include "modules/builtin.h"

static struct {
  struct js *js;
  jsval_t handler;
} signal_handlers[32] = {0};

static void general_signal_handler(int signum) {
  if (signum >= 0 && signum < 32 && signal_handlers[signum].js != NULL) {
    struct js *js = signal_handlers[signum].js;
    jsval_t handler = signal_handlers[signum].handler;
    
    if (js_type(handler) != JS_UNDEF) {
      jsval_t sig_num = js_mknum(signum);
      jsval_t args[1] = {sig_num};
      js_call(js, handler, args, 1);
    }
  }
  
  exit(0);
}

// Ant.signal(signal, handler)
static jsval_t js_signal(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    fprintf(stderr, "Error: Ant.signal() requires 2 arguments (signal, handler)\n");
    return js_mkundef();
  }
  
  char *signal_name = js_getstr(js, args[0], NULL);
  if (signal_name == NULL) {
    return js_mkerr(js, "signal name must be a string");
  }
  
  int signum = -1;
  if (strcmp(signal_name, "SIGINT") == 0 || strcmp(signal_name, "sigint") == 0) {
    signum = SIGINT;
  } else if (strcmp(signal_name, "SIGTERM") == 0 || strcmp(signal_name, "sigterm") == 0) {
    signum = SIGTERM;
  } else if (strcmp(signal_name, "SIGHUP") == 0 || strcmp(signal_name, "sighup") == 0) {
    signum = SIGHUP;
  } else if (strcmp(signal_name, "SIGUSR1") == 0 || strcmp(signal_name, "sigusr1") == 0) {
    signum = SIGUSR1;
  } else if (strcmp(signal_name, "SIGUSR2") == 0 || strcmp(signal_name, "sigusr2") == 0) {
    signum = SIGUSR2;
  } else {
    return js_mkerr(js, "unsupported signal: %s", signal_name);
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

// Ant.raw.typeof()
static jsval_t js_raw_typeof(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.raw.typeof() requires 1 argument");
  const uint8_t type = vtype(args[0]);
  return js_mknum((double)type);
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
  js_set(js, rt->ant_obj, "signal", js_mkfun(js_signal));

  jsval_t raw_obj = js_mkobj(js);
  js_set(js, raw_obj, "typeof", js_mkfun(js_raw_typeof));
  js_set(js, ant_obj, "raw", raw_obj);
}