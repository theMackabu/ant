#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include <uv.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "list.h"
#include "ptr.h"
#include "modules/bigint.h"
#include "modules/collections.h"
#include "modules/performance.h"

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

static double perf_now(ant_t *js) {
  return get_current_time_ms() - js->perf_time_origin_ms;
}

// performance.now()
static ant_value_t js_performance_now(ant_params_t) {
  return js_mknum(perf_now(js));
}

void Ant_Performance_MarkLoopStart(ant_t *js) {
  if (js->perf_loop_start_ms < 0) js->perf_loop_start_ms = perf_now(js);
}

typedef struct {
  double idle;
  double active;
} perf_elu_t;

static bool perf_elu_read(ant_t *js, ant_value_t value, perf_elu_t *out) {
  if (!is_object_type(value)) return false;
  ant_value_t idle = js_get(js, value, "idle");
  ant_value_t active = js_get(js, value, "active");
  out->idle = vtype(idle) == kTypeNumber ? js_getnum(idle) : 0;
  out->active = vtype(active) == kTypeNumber ? js_getnum(active) : 0;
  return true;
}

static ant_value_t perf_elu_result(ant_t *js, double idle, double active, double utilization) {
  ant_value_t obj = Ant_Object_Create(js);
  js_set(js, obj, "idle", js_mknum(idle));
  js_set(js, obj, "active", js_mknum(active));
  js_set(js, obj, "utilization", js_mknum(utilization));
  return obj;
}

static ant_value_t perf_elu_delta(ant_t *js, perf_elu_t now, perf_elu_t prev) {
  double idle = now.idle - prev.idle;
  double active = now.active - prev.active;
  return perf_elu_result(js, idle, active, active / (idle + active));
}

ant_value_t ant_performance_elu_zero(ant_t *js) {
  return perf_elu_result(js, 0, 0, 0);
}

bool ant_performance_elu_pair(ant_native_params_t, ant_value_t *out) {
  perf_elu_t first, second;
  if (nargs < 2 || !perf_elu_read(js, args[0], &first) || !perf_elu_read(js, args[1], &second)) return false;
  *out = perf_elu_delta(js, first, second);
  return true;
}

ant_value_t ant_performance_elu_from(ant_t *js, double idle, double active, ant_args_t) {
  perf_elu_t first, current = { idle, active };
  if (nargs > 0 && perf_elu_read(js, args[0], &first)) return perf_elu_delta(js, current, first);
  return perf_elu_result(js, idle, active, active / (idle + active));
}

// performance.eventLoopUtilization([utilization1[, utilization2]])
static ant_value_t js_performance_elu(ant_params_t) {
  if (js->perf_loop_start_ms < 0) return ant_performance_elu_zero(js);

  ant_value_t pair;
  if (ant_performance_elu_pair(js, args, nargs, &pair)) return pair;

  if (js->perf_elu_start_ms < 0) {
    uv_loop_configure(js->loop, UV_METRICS_IDLE_TIME);
    js->perf_elu_start_ms = perf_now(js);
  }

  double since = js->perf_elu_start_ms > js->perf_loop_start_ms ? js->perf_elu_start_ms : js->perf_loop_start_ms;
  double idle = (double)uv_metrics_idle_time(js->loop) / 1e6;
  
  return ant_performance_elu_from(js, idle, perf_now(js) - since - idle, args, nargs);
}

// performance.toJSON()
static ant_value_t js_performance_to_json(ant_params_t) {
  ant_value_t obj = Ant_Object_Create(js);
  js_set(js, obj, "timeOrigin", js_mknum(js->perf_time_origin_ms));
  return obj;
}

enum {
  HIST_LINEAR = 128,
  HIST_SUB_BITS = 6,
  HIST_SUB = 1 << HIST_SUB_BITS,
  HIST_BUCKETS = HIST_LINEAR + (64 - 7) * HIST_SUB,
};

typedef struct {
  uint64_t counts[HIST_BUCKETS];
  uint64_t count;
  uint64_t min;
  uint64_t max;
  double sum;
  double sum_sq;
} perf_histogram_t;

struct perf_eld_monitor {
  uv_timer_t timer;
  uint64_t resolution_ms;
  uint64_t prev;
  bool enabled;
  bool timer_open;
  bool closing;
  bool obj_alive;
  perf_histogram_t hist;
  perf_eld_monitor_t *next;
  perf_eld_monitor_t **pprev;
};

static constexpr uint32_t ELD_NATIVE_TAG = Ant_NativeTag("ELDH");

static void hist_reset(perf_histogram_t *h) {
  memset(h->counts, 0, sizeof(h->counts));
  h->count = 0;
  h->min = INT64_MAX;
  h->max = 0;
  h->sum = 0;
  h->sum_sq = 0;
}

static size_t hist_index(uint64_t value) {
  if (value < HIST_LINEAR) return (size_t)value;
  unsigned msb = 63u - (unsigned)__builtin_clzll(value);
  unsigned shift = msb - HIST_SUB_BITS;
  return HIST_LINEAR + (size_t)(msb - 7) * HIST_SUB + (size_t)((value >> shift) - HIST_SUB);
}

static uint64_t hist_bucket_high(size_t index) {
  if (index < HIST_LINEAR) return index;
  size_t k = index - HIST_LINEAR;
  unsigned shift = (unsigned)(k / HIST_SUB) + 7 - HIST_SUB_BITS;
  uint64_t low = (uint64_t)(HIST_SUB + k % HIST_SUB) << shift;
  return low + ((uint64_t)1 << shift) - 1;
}

static void hist_record(perf_histogram_t *h, uint64_t value) {
  h->counts[hist_index(value)]++;
  h->count++;
  if (value < h->min) h->min = value;
  if (value > h->max) h->max = value;
  h->sum += (double)value;
  h->sum_sq += (double)value * (double)value;
}

static uint64_t hist_percentile(const perf_histogram_t *h, double percentile) {
  if (h->count == 0) return 0;
  uint64_t target = (uint64_t)ceil(percentile / 100.0 * (double)h->count);
  if (target == 0) target = 1;

  uint64_t seen = 0;
  for (size_t i = 0; i < HIST_BUCKETS; i++) {
    seen += h->counts[i];
    if (seen < target) continue;
    uint64_t value = hist_bucket_high(i);
    if (value > h->max) value = h->max;
    if (value < h->min) value = h->min;
    return value;
  }
  return h->max;
}

static inline perf_eld_monitor_t *eld_monitor(ant_value_t value) {
  return js_get_native(value, ELD_NATIVE_TAG);
}

static void eld_timer_closed(uv_handle_t *handle) {
  perf_eld_monitor_t *m = handle->data;
  m->timer_open = false;
  if (!m->obj_alive) free(m);
}

static void eld_close(perf_eld_monitor_t *m) {
  if (!m->timer_open || m->closing) return;
  m->closing = true;
  m->enabled = false;
  Ant_List_Remove(m);
  uv_close((uv_handle_t *)&m->timer, eld_timer_closed);
}

static void eld_tick(uv_timer_t *timer) {
  perf_eld_monitor_t *m = timer->data;
  uint64_t now = uv_hrtime();
  if (m->prev && now > m->prev) hist_record(&m->hist, now - m->prev);
  m->prev = now;
}

static void eld_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  perf_eld_monitor_t *m = eld_monitor(value);
  if (!m) return;
  js_clear_native(value, ELD_NATIVE_TAG);
  m->obj_alive = false;
  if (m->timer_open) eld_close(m);
  else free(m);
}

static ant_value_t eld_this(ant_t *js, perf_eld_monitor_t **out) {
  *out = eld_monitor(js_getthis(js));
  return *out ? js_mkundef() : js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
}

static ant_value_t js_eld_enable(ant_params_t) {
  perf_eld_monitor_t *m;
  ant_value_t err = eld_this(js, &m);
  if (is_err(err)) return err;
  if (m->enabled || m->closing || js->destroying) return js_false;

  if (!m->timer_open) {
    if (uv_timer_init(js->loop, &m->timer) != 0) return js_mkerr(js, "failed to start event loop delay monitor");
    m->timer.data = m;
    m->timer_open = true;
    uv_unref((uv_handle_t *)&m->timer);
    Ant_List_Insert(&js->eld_monitors, m);
  }

  m->prev = 0;
  m->enabled = true;
  uv_timer_start(&m->timer, eld_tick, m->resolution_ms, m->resolution_ms);
  return js_true;
}

static ant_value_t js_eld_disable(ant_params_t) {
  perf_eld_monitor_t *m;
  ant_value_t err = eld_this(js, &m);
  if (is_err(err)) return err;
  if (!m->enabled) return js_false;

  m->enabled = false;
  uv_timer_stop(&m->timer);
  return js_true;
}

static ant_value_t js_eld_reset(ant_params_t) {
  perf_eld_monitor_t *m;
  ant_value_t err = eld_this(js, &m);
  if (is_err(err)) return err;
  hist_reset(&m->hist);
  m->prev = 0;
  return js_mkundef();
}

static ant_value_t eld_read_percentile(ant_t *js, ant_value_t arg, double *out) {
  if (vtype(arg) != kTypeNumber)
    return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", "The \"percentile\" argument must be of type number");
  double p = js_getnum(arg);
  if (!(p > 0 && p <= 100)) {
    char msg[128];
    snprintf(msg, sizeof(msg), "The value of \"percentile\" is out of range. It must be > 0 && <= 100. Received %g", p);
    return js_mkerr_code(js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE", msg);
  }
  *out = p;
  return js_mkundef();
}

static ant_value_t js_eld_percentile(ant_params_t) {
  perf_eld_monitor_t *m;
  ant_value_t err = eld_this(js, &m);
  if (is_err(err)) return err;
  double p = 0;
  err = eld_read_percentile(js, nargs > 0 ? args[0] : js_mkundef(), &p);
  if (is_err(err)) return err;
  return js_mknum((double)hist_percentile(&m->hist, p));
}

static ant_value_t js_eld_percentile_bigint(ant_params_t) {
  perf_eld_monitor_t *m;
  ant_value_t err = eld_this(js, &m);
  if (is_err(err)) return err;
  double p = 0;
  err = eld_read_percentile(js, nargs > 0 ? args[0] : js_mkundef(), &p);
  if (is_err(err)) return err;
  return bigint_from_uint64(js, hist_percentile(&m->hist, p));
}

static ant_value_t js_eld_percentiles(ant_params_t) {
  perf_eld_monitor_t *m;
  ant_value_t err = eld_this(js, &m);
  if (is_err(err)) return err;

  map_entry_t **entries = NULL;
  ant_value_t map = collections_map_create(js, &entries);
  if (is_err(map)) return map;
  const perf_histogram_t *h = &m->hist;

  if (h->count == 0) {
    collections_map_store_cloned_entry(js, entries, js_mknum(100), js_mknum(0));
    return map;
  }

  collections_map_store_cloned_entry(js, entries, js_mknum(0), js_mknum((double)h->min));
  for (int i = 1; i < 64; i++) {
    double p = 100.0 - 100.0 / (double)((uint64_t)1 << i);
    uint64_t value = hist_percentile(h, p);
    collections_map_store_cloned_entry(js, entries, js_mknum(p), js_mknum((double)value));
    if (value >= h->max) break;
  }
  collections_map_store_cloned_entry(js, entries, js_mknum(100), js_mknum((double)h->max));
  return map;
}

#define ELD_GETTER(name, expr)            \
  static ant_value_t name(ant_params_t) { \
    perf_eld_monitor_t *m;                \
    ant_value_t err = eld_this(js, &m);   \
    if (is_err(err)) return err;          \
    const perf_histogram_t *h = &m->hist; \
    (void)h;                              \
    return expr;                          \
  }

ELD_GETTER(js_eld_min, js_mknum((double)h->min))
ELD_GETTER(js_eld_max, js_mknum((double)h->max))
ELD_GETTER(js_eld_count, js_mknum((double)h->count))
ELD_GETTER(js_eld_exceeds, js_mknum(0))
ELD_GETTER(js_eld_min_bigint, bigint_from_uint64(js, h->min))
ELD_GETTER(js_eld_max_bigint, bigint_from_uint64(js, h->max))
ELD_GETTER(js_eld_count_bigint, bigint_from_uint64(js, h->count))
ELD_GETTER(js_eld_exceeds_bigint, bigint_from_uint64(js, 0))
ELD_GETTER(js_eld_mean, js_mknum(h->count ? h->sum / (double)h->count : JS_NAN))
ELD_GETTER(js_eld_stddev, js_mknum(h->count ? sqrt(fmax(0, h->sum_sq / (double)h->count - (h->sum / (double)h->count) * (h->sum / (double)h->count))) : JS_NAN))

#undef ELD_GETTER

static ant_value_t js_eld_ctor(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal constructor");
}

static ant_value_t eld_histogram_proto(ant_t *js) {
  if (is_object_type(js->builtins.eld_histogram_proto)) return js->builtins.eld_histogram_proto;

  ant_value_t proto = Ant_Object_Create(js);
  js->builtins.eld_histogram_proto = proto;
  Ant_Function_CreateConstructor(js, js_eld_ctor, proto, "ELDHistogram", 0);

  js_set(js, proto, "enable", Ant_Function_Create(js_eld_enable));
  js_set(js, proto, "disable", Ant_Function_Create(js_eld_disable));
  js_set(js, proto, "reset", Ant_Function_Create(js_eld_reset));
  js_set(js, proto, "percentile", Ant_Function_CreateWithArity(js_eld_percentile, 1));
  js_set(js, proto, "percentileBigInt", Ant_Function_CreateWithArity(js_eld_percentile_bigint, 1));

  static const ant_cfunc_meta_t getters[] = {
    { js_eld_min, "min", 0, 0 }, { js_eld_max, "max", 0, 0 },
    { js_eld_mean, "mean", 0, 0 }, { js_eld_stddev, "stddev", 0, 0 },
    { js_eld_count, "count", 0, 0 }, { js_eld_exceeds, "exceeds", 0, 0 },
    { js_eld_percentiles, "percentiles", 0, 0 },
    { js_eld_min_bigint, "minBigInt", 0, 0 }, { js_eld_max_bigint, "maxBigInt", 0, 0 },
    { js_eld_count_bigint, "countBigInt", 0, 0 }, { js_eld_exceeds_bigint, "exceedsBigInt", 0, 0 },
  };
  for (size_t i = 0; i < sizeof(getters) / sizeof(getters[0]); i++)
    js_set_getter_desc(js, proto, getters[i].name, strlen(getters[i].name), js_mkfun_meta(&getters[i]), JS_DESC_C);

  return proto;
}

static ant_value_t eld_read_resolution(ant_t *js, ant_value_t options, uint64_t *out) {
  *out = 10;
  if (vtype(options) == kTypeUndefined) return js_mkundef();
  if (!is_object_type(options))
    return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", "The \"options\" argument must be of type object");

  ant_value_t value = js_get(js, options, "resolution");
  if (is_err(value)) return value;
  if (vtype(value) == kTypeUndefined) return js_mkundef();
  if (vtype(value) != kTypeNumber)
    return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", "The \"options.resolution\" property must be of type number");

  double resolution = js_getnum(value);
  if (resolution != floor(resolution) || resolution < 1 || resolution > 9007199254740991.0) {
    char msg[160];
    snprintf(msg, sizeof(msg), resolution != floor(resolution)
      ? "The value of \"options.resolution\" is out of range. It must be an integer. Received %g"
      : "The value of \"options.resolution\" is out of range. It must be >= 1 && <= 9007199254740991. Received %g", resolution);
    return js_mkerr_code(js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE", msg);
  }
  *out = (uint64_t)resolution;
  return js_mkundef();
}

static ant_value_t js_monitor_event_loop_delay(ant_params_t) {
  uint64_t resolution = 10;
  ant_value_t err = eld_read_resolution(js, nargs > 0 ? args[0] : js_mkundef(), &resolution);
  if (is_err(err)) return err;

  perf_eld_monitor_t *m = calloc(1, sizeof(*m));
  if (!m) return Ant_Error_CreateOutOfMemory(js);
  m->resolution_ms = resolution;
  m->obj_alive = true;
  hist_reset(&m->hist);

  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) {
    free(m);
    return obj;
  }
  
  js_set_proto_init(obj, eld_histogram_proto(js));
  js_set_native(obj, m, ELD_NATIVE_TAG);
  js_set_finalizer(obj, eld_finalize);
  
  return obj;
}

void cleanup_performance_module(ant_t *js) {
  while (js->eld_monitors) eld_close(js->eld_monitors);
}

ant_value_t perf_hooks_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);
  js_set(js, lib, "performance", js_get(js, js_glob(js), "performance"));
  js_set(js, lib, "monitorEventLoopDelay", Ant_Function_CreateWithArity(js_monitor_event_loop_delay, 0));
  js_set_tostringtag(js, lib, ANT_STRING("perf_hooks"));
  return lib;
}

void init_performance_module(ant_t *js) {
  ant_value_t glob = js_glob(js);
  ant_value_t perf_obj = Ant_Object_Create(js);

  js->perf_time_origin_ms = get_current_time_ms();
  js->perf_loop_start_ms = -1;
  js->perf_elu_start_ms = -1;
  
  js_set(js, perf_obj, "now", Ant_Function_Create(js_performance_now));
  js_set(js, perf_obj, "timeOrigin", js_mknum(js->perf_time_origin_ms));
  js_set(js, perf_obj, "toJSON", Ant_Function_Create(js_performance_to_json));
  js_set(js, perf_obj, "eventLoopUtilization", Ant_Function_CreateWithArity(js_performance_elu, 2));
  js_set(js, perf_obj, "markResourceTiming", Ant_Function_CreateWithArity(js_cfunc_noop, 7));
  
  js_set_tostringtag(js, perf_obj, ANT_STRING("Performance"));
  js_set(js, glob, "performance", perf_obj);
}
