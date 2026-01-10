#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "ant.h"
#include "runtime.h"
#include "modules/symbol.h"
#include "modules/performance.h"

static double time_origin_ms = 0;

static double get_current_time_ms(void) {
#ifdef _WIN32
  LARGE_INTEGER freq, count;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&count);
  return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
#endif
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
  
  jsval_t glob = js_glob(js);
  jsval_t perf_obj = js_mkobj(js);

  time_origin_ms = get_current_time_ms();
  
  js_set(js, perf_obj, "now", js_mkfun(js_performance_now));
  js_set(js, perf_obj, "timeOrigin", js_mknum(time_origin_ms));
  
  js_set(js, perf_obj, get_toStringTag_sym_key(), ANT_STRING("Performance"));
  js_set(js, glob, "performance", perf_obj);
}
