#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>

#include "ant.h"
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
  size_t before_brk = js_getbrk(js);
  
  uint32_t freed = js_gc(js);
  size_t after_brk = js_getbrk(js);
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "before", js_mknum((double)before_brk));
  js_set(js, result, "after", js_mknum((double)after_brk));
  js_set(js, result, "freed", js_mknum((double)freed));
  
  return result;
}

// Ant.alloc()
static jsval_t js_alloc(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  
  size_t total = 0, min_free = 0, cstack = 0;
  js_stats(js, &total, &min_free, &cstack);
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "used", js_mknum((double)total));
  js_set(js, result, "minFree", js_mknum((double)min_free));
  
  return result;
}

// Ant.stats()
static jsval_t js_stats_fn(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  
  size_t total = 0, min_free = 0, cstack = 0;
  js_stats(js, &total, &min_free, &cstack);
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "used", js_mknum((double)total));
  js_set(js, result, "minFree", js_mknum((double)min_free));
  js_set(js, result, "cstack", js_mknum((double)cstack));
  
  return result;
}

void init_builtin_module() {
  struct js *js = rt->js;
  jsval_t ant_obj = rt->ant_obj;

  js_set(js, ant_obj, "gc", js_mkfun(js_gc_trigger));
  js_set(js, ant_obj, "alloc", js_mkfun(js_alloc));
  js_set(js, ant_obj, "stats", js_mkfun(js_stats_fn));
  js_set(js, rt->ant_obj, "signal", js_mkfun(js_signal));
}