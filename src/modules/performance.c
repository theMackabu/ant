#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "runtime.h"
#include "modules/performance.h"

static double time_origin_ms = 0;

static double get_current_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

// performance.now()
static jsval_t js_performance_now(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  double now = get_current_time_ms() - time_origin_ms;
  return js_mknum(now);
}

// performance.timeOrigin
static jsval_t js_performance_time_origin(struct js *js, jsval_t *args, int nargs) {
  (void) args; (void) nargs;
  return js_mknum(time_origin_ms);
}

void init_performance_module() {
  struct js *js = rt->js;
  jsval_t global = js_glob(js);
  
  time_origin_ms = get_current_time_ms();
  
  jsval_t perf_obj = js_mkobj(js);
  js_set(js, perf_obj, "now", js_mkfun(js_performance_now));
  js_set(js, perf_obj, "timeOrigin", js_mknum(time_origin_ms));
  
  js_set(js, global, "performance", perf_obj);
}
