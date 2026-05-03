#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <crprintf.h>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

#include "ant.h"
#include "gc.h"
#include "crash.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "highlight.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/builtin.h"
#include "modules/buffer.h"
#include "modules/symbol.h"

static struct {
  ant_t *js;
  ant_value_t handler;
} signal_handlers[NSIG] = {0};

static void general_signal_handler(int signum) {
  if (signum < 0 || signum >= NSIG) return;
  ant_t *js = signal_handlers[signum].js;
  ant_value_t handler = signal_handlers[signum].handler;
  
  if (js && vtype(handler) != T_UNDEF) {
    ant_value_t args[] = {js_mknum(signum)};
    sv_vm_call(js->vm, js, handler, js_mkundef(), args, 1, NULL, false);
  }
  
  exit(0);
}

// Ant.signal(signal, handler)
static ant_value_t js_signal(ant_t *js, ant_value_t *args, int nargs) {
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

// Ant.raw.stack()
static ant_value_t js_raw_stack(ant_t *js, ant_value_t *args, int nargs) {
  return js_capture_raw_stack(js);
}

// Ant.raw.typeof(ant_value_t)
static ant_value_t js_raw_typeof(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.raw.typeof() requires 1 argument");
  const uint8_t type = vtype(args[0]);
  return js_mknum((double)type);
}

// Ant.raw.ctorPropFeedback(constructorFn)
static ant_value_t js_raw_ctor_prop_feedback(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.raw.ctorPropFeedback() requires 1 argument");
  if (vtype(args[0]) != T_FUNC) return js_mkerr(js, "constructor must be a function");

#ifndef ANT_JIT
  return js_mkerr(js, "constructor property feedback requires ANT_JIT");
#else
  ant_value_t ctor = args[0];
  ant_value_t ctor_obj = js_func_obj(ctor);
  ant_value_t target_func = js_get_slot(ctor_obj, SLOT_TARGET_FUNC);
  if (vtype(target_func) == T_FUNC) ctor = target_func;

  sv_closure_t *closure = js_func_closure(ctor);
  if (!closure || !closure->func) return js_mkundef();
  sv_func_t *fn = closure->func;

  ant_value_t out = js_newobj(js);
  js_set(js, out, "samples", js_mknum((double)sv_tfb_ctor_prop_samples(fn)));
  js_set(js, out, "overflowFrom", js_mknum((double)SV_TFB_CTOR_PROP_OVERFLOW_FROM));
  js_set(js, out, "inobjLimit", js_mknum((double)sv_tfb_ctor_inobj_limit(ctor)));
  js_set(js, out, "inobjLimitFrozen", js_bool(sv_tfb_ctor_inobj_limit_frozen(ctor)));
  js_set(js, out, "slackRemaining", js_mknum((double)sv_tfb_ctor_inobj_slack_remaining(ctor)));

  ant_value_t bins = js_mkarr(js);
  for (uint32_t i = 0; i < SV_TFB_CTOR_PROP_BINS; i++) 
    js_arr_push(js, bins, js_mknum((double)sv_tfb_ctor_prop_bin(fn, i)));
  js_set(js, out, "bins", bins);

  if (fn->name) js_set(js, out, "name", js_mkstr(js, fn->name, strlen(fn->name)));
  if (fn->filename) js_set(js, out, "filename", js_mkstr(js, fn->filename, strlen(fn->filename)));

  return out;
#endif
}

static ant_value_t js_raw_gc_mark_profile(ant_t *js, ant_value_t *args, int nargs) {
  gc_func_mark_profile_t p = gc_func_mark_profile_get();
  ant_value_t out = js_newobj(js);
  
  js_set(js, out, "enabled", js_bool(p.enabled));
  js_set(js, out, "collections", js_mknum((double)p.collections));
  js_set(js, out, "funcVisits", js_mknum((double)p.func_visits));
  js_set(js, out, "childEdges", js_mknum((double)p.child_edges));
  js_set(js, out, "constSlots", js_mknum((double)p.const_slots));
  js_set(js, out, "timeNs", js_mknum((double)p.time_ns));
  js_set(js, out, "timeMs", js_mknum((double)p.time_ns / 1000000.0));
  
  return out;
}

static ant_value_t js_raw_gc_mark_profile_enable(ant_t *js, ant_value_t *args, int nargs) {
  bool enabled = true;
  if (nargs > 0) enabled = js_truthy(js, args[0]);
  gc_func_mark_profile_enable(enabled);
  return js_bool(enabled);
}

static ant_value_t js_raw_gc_mark_profile_reset(ant_t *js, ant_value_t *args, int nargs) {
  gc_func_mark_profile_reset();
  return js_mkundef();
}

// Ant.sleep(seconds)
static ant_value_t js_sleep(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.sleep() requires 1 argument");
  unsigned int seconds = (unsigned int)js_getnum(args[0]);
  sleep(seconds);
  return js_mkundef();
}

// Ant.msleep(milliseconds)
static ant_value_t js_msleep(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.msleep() requires 1 argument");
  long ms = (long)js_getnum(args[0]);
  struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
  struct timespec rem;
  while (nanosleep(&ts, &rem) == -1 && errno == EINTR) ts = rem;
  return js_mkundef();
}

// Ant.usleep(microseconds)
static ant_value_t js_usleep(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.usleep() requires 1 argument");
  useconds_t us = (useconds_t)js_getnum(args[0]);
  usleep(us);
  return js_mkundef();
}

// Ant.suppressReporting()
static ant_value_t js_suppress_reporting(ant_t *js, ant_value_t *args, int nargs) {
  ant_crash_suppress_reporting();
  return js_mkundef();
}

// Ant.stats()
static ant_value_t js_stats_fn(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t result = js_newobj(js);
  
  ant_pool_stats_t rope_s = js_pool_stats(&js->pool.rope);
  ant_pool_stats_t sym_s = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t bigint_s = js_class_pool_stats(&js->pool.bigint);
  ant_string_pool_stats_t string_s = js_string_pool_stats(&js->pool.string);

  ant_value_t pools = js_newobj(js);
  ant_value_t rope_obj = js_newobj(js);
  js_set(js, rope_obj, "used", js_mknum((double)rope_s.used));
  js_set(js, rope_obj, "capacity", js_mknum((double)rope_s.capacity));
  js_set(js, rope_obj, "blocks", js_mknum((double)rope_s.blocks));
  js_set(js, pools, "rope", rope_obj);

  ant_value_t sym_obj = js_newobj(js);
  js_set(js, sym_obj, "used", js_mknum((double)sym_s.used));
  js_set(js, sym_obj, "capacity", js_mknum((double)sym_s.capacity));
  js_set(js, sym_obj, "blocks", js_mknum((double)sym_s.blocks));
  js_set(js, pools, "symbol", sym_obj);

  ant_value_t bigint_obj = js_newobj(js);
  js_set(js, bigint_obj, "used", js_mknum((double)bigint_s.used));
  js_set(js, bigint_obj, "capacity", js_mknum((double)bigint_s.capacity));
  js_set(js, bigint_obj, "blocks", js_mknum((double)bigint_s.blocks));
  js_set(js, pools, "bigint", bigint_obj);

  ant_value_t string_obj = js_newobj(js);
  js_set(js, string_obj, "used", js_mknum((double)string_s.total.used));
  js_set(js, string_obj, "capacity", js_mknum((double)string_s.total.capacity));
  js_set(js, string_obj, "blocks", js_mknum((double)string_s.total.blocks));

  ant_value_t string_pooled_obj = js_newobj(js);
  js_set(js, string_pooled_obj, "used", js_mknum((double)string_s.pooled.used));
  js_set(js, string_pooled_obj, "capacity", js_mknum((double)string_s.pooled.capacity));
  js_set(js, string_pooled_obj, "blocks", js_mknum((double)string_s.pooled.blocks));
  js_set(js, string_obj, "pooled", string_pooled_obj);

  ant_value_t string_large_live_obj = js_newobj(js);
  js_set(js, string_large_live_obj, "used", js_mknum((double)string_s.large_live.used));
  js_set(js, string_large_live_obj, "capacity", js_mknum((double)string_s.large_live.capacity));
  js_set(js, string_large_live_obj, "blocks", js_mknum((double)string_s.large_live.blocks));
  js_set(js, string_obj, "largeLive", string_large_live_obj);

  ant_value_t string_large_reusable_obj = js_newobj(js);
  js_set(js, string_large_reusable_obj, "used", js_mknum((double)string_s.large_reusable.used));
  js_set(js, string_large_reusable_obj, "capacity", js_mknum((double)string_s.large_reusable.capacity));
  js_set(js, string_large_reusable_obj, "blocks", js_mknum((double)string_s.large_reusable.blocks));
  js_set(js, string_obj, "largeReusable", string_large_reusable_obj);

  ant_value_t string_large_quarantine_obj = js_newobj(js);
  js_set(js, string_large_quarantine_obj, "used", js_mknum((double)string_s.large_quarantine.used));
  js_set(js, string_large_quarantine_obj, "capacity", js_mknum((double)string_s.large_quarantine.capacity));
  js_set(js, string_large_quarantine_obj, "blocks", js_mknum((double)string_s.large_quarantine.blocks));
  js_set(js, string_obj, "largeQuarantine", string_large_quarantine_obj);
  
  size_t pool_used = rope_s.used + sym_s.used + bigint_s.used + string_s.total.used;
  size_t pool_cap = rope_s.capacity + sym_s.capacity + bigint_s.capacity + string_s.total.capacity;
  
  js_set(js, pools, "totalUsed", js_mknum((double)pool_used));
  js_set(js, pools, "totalCapacity", js_mknum((double)pool_cap));
  js_set(js, result, "pools", pools);
  js_set(js, pools, "string", string_obj);

  size_t obj_count      = 0;
  size_t obj_bytes      = 0;
  size_t overflow_bytes = 0;
  size_t extra_bytes    = 0;
  size_t promise_bytes  = 0;
  size_t proxy_bytes    = 0;
  size_t exotic_bytes   = 0;
  size_t array_bytes    = 0;
  
  for (int pass = 0; pass < 3; pass++) {
    ant_object_t *head = pass == 0 ? js->objects : pass == 1 ? js->objects_old : js->permanent_objects;
    for (ant_object_t *obj = head; obj; obj = obj->next) {
      obj_count++;
      obj_bytes += sizeof(ant_object_t);
      uint32_t inobj_limit = ant_object_inobj_limit(obj);
      if (obj->overflow_prop && obj->prop_count > inobj_limit)
        overflow_bytes += (obj->prop_count - inobj_limit) * sizeof(ant_value_t);
      uint8_t extra_count = 0;
      ant_extra_slot_t *slots = ant_object_extra_slots(obj, &extra_count);
      if (slots) extra_bytes += extra_count * sizeof(ant_extra_slot_t);
      if (obj->promise_state) promise_bytes += sizeof(ant_promise_state_t);
      if (ant_object_proxy_state(obj)) proxy_bytes += sizeof(ant_proxy_state_t);
      if (ant_object_has_sidecar(obj)) extra_bytes += sizeof(ant_object_sidecar_t);
      if (obj->exotic_ops) exotic_bytes += sizeof(ant_exotic_ops_t);
      if (obj->type_tag == T_ARR && obj->u.array.data)
        array_bytes += obj->u.array.cap * sizeof(ant_value_t);
    }
  }

  ant_value_t alloc = js_newobj(js);
  js_set(js, alloc, "objectCount", js_mknum((double)obj_count));
  js_set(js, alloc, "objects", js_mknum((double)obj_bytes));
  js_set(js, alloc, "overflow", js_mknum((double)overflow_bytes));
  js_set(js, alloc, "extraSlots", js_mknum((double)extra_bytes));
  js_set(js, alloc, "promises", js_mknum((double)promise_bytes));
  js_set(js, alloc, "proxies", js_mknum((double)proxy_bytes));
  js_set(js, alloc, "exotic", js_mknum((double)exotic_bytes));
  js_set(js, alloc, "arrays", js_mknum((double)array_bytes));
  
  size_t shape_bytes = ant_shape_total_bytes();
  js_set(js, alloc, "shapes", js_mknum((double)shape_bytes));
  js_set(js, alloc, "closures", js_mknum((double)js->alloc_bytes.closures));
  js_set(js, alloc, "upvalues", js_mknum((double)js->alloc_bytes.upvalues));
  js_set(js, alloc, "propRefs", js_mknum((double)(js->prop_refs_cap * sizeof(ant_prop_ref_t))));

  size_t alloc_total = obj_bytes + overflow_bytes + extra_bytes
    + promise_bytes + proxy_bytes + exotic_bytes + array_bytes
    + shape_bytes + js->alloc_bytes.closures + js->alloc_bytes.upvalues
    + js->prop_refs_cap * sizeof(ant_prop_ref_t);
  
  js_set(js, alloc, "total", js_mknum((double)alloc_total));
  js_set(js, result, "alloc", alloc);

  size_t buffer_mem = buffer_get_external_memory();
  size_t code_mem = code_arena_get_memory();
  size_t parse_mem = parse_arena_get_memory();
  size_t external_total = buffer_mem + code_mem + parse_mem;
  
  ant_value_t ext = js_newobj(js);
  js_set(js, ext, "buffers", js_mknum((double)buffer_mem));
  js_set(js, ext, "code", js_mknum((double)code_mem));
  js_set(js, ext, "parse", js_mknum((double)parse_mem));
  js_set(js, ext, "total", js_mknum((double)external_total));
  js_set(js, result, "external", ext);
  
  js_intern_stats_t intern_stats = js_intern_stats();
  ant_value_t intern = js_newobj(js);
  
  js_set(js, intern, "count", js_mknum((double)intern_stats.count));
  js_set(js, intern, "bytes", js_mknum((double)intern_stats.bytes));
  js_set(js, result, "intern", intern);
  
  sv_vm_t *vm = sv_vm_get_active(js);
  if (vm) {
    ant_value_t vmobj = js_newobj(js);
    js_set(js, vmobj, "stackSize", js_mknum((double)vm->stack_size));
    js_set(js, vmobj, "stackUsed", js_mknum((double)vm->sp));
    js_set(js, vmobj, "maxFrames", js_mknum((double)vm->max_frames));
    js_set(js, vmobj, "framesUsed", js_mknum((double)(vm->fp + 1)));
    js_set(js, result, "vm", vmobj);
  }

  if (js->cstk.base != NULL) {
    volatile char marker;
    uintptr_t base = (uintptr_t)js->cstk.base;
    uintptr_t curr = (uintptr_t)&marker;
    size_t used = (base > curr) ? (base - curr) : (curr - base);
    ant_value_t cstk = js_newobj(js);
    js_set(js, cstk, "used", js_mknum((double)used));
    js_set(js, cstk, "limit", js_mknum((double)js->cstk.limit));
    js_set(js, result, "cstack", cstk);
  } else js_set(js, result, "cstack", js_mknum(0));
  
#ifdef __APPLE__
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
    js_set(js, result, "residentSize", js_mknum((double)info.resident_size));
    js_set(js, result, "virtualSize", js_mknum((double)info.virtual_size));
  }

  task_vm_info_data_t vm_info;
  mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm_info, &vm_count) == KERN_SUCCESS) {
    js_set(js, result, "rss", js_mknum((double)vm_info.phys_footprint));
    js_set(js, result, "physFootprint", js_mknum((double)vm_info.phys_footprint));
  } else if (vtype(js_get(js, result, "rss")) == T_UNDEF) {
    ant_value_t resident = js_get(js, result, "residentSize");
    if (vtype(resident) != T_UNDEF) js_set(js, result, "rss", resident);
  }
#elif defined(__linux__)
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    js_set(js, result, "rss", js_mknum((double)usage.ru_maxrss * 1024));
  }
#endif
  
  return result;
}

static inline ant_value_t match_resolve_arm(ant_t *js, ant_value_t arm, ant_value_t value) {
  if (!is_callable(arm)) return arm;
  return sv_vm_call(js->vm, js, arm, js_mkundef(), &value, 1, NULL, false);
}

static ant_value_t js_match(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Ant.match() requires 2 arguments");

  ant_value_t value = args[0];
  ant_value_t arms = args[1];

  if (is_callable(arms)) {
    arms = sv_vm_call(js->vm, js, arms, js_mkundef(), &value, 1, NULL, false);
    if (is_err(arms)) return arms;
  }

  if (!is_object_type(arms)) return js_mkundef();
  ant_value_t value_str = js_tostring_val(js, value);
  const char *vs = js_getstr(js, value_str, NULL);

  ant_iter_t iter = js_prop_iter_begin(js, arms);
  ant_value_t guard_arm = js_mkundef();
  
  ant_value_t key = js_mkundef();
  ant_value_t arm = js_mkundef();

  while (js_prop_iter_next_val(&iter, &key, &arm)) {
    if (vtype(key) == T_SYMBOL) continue;
    
    const char *ks = js_getstr(js, key, NULL);
    if (!ks) continue;
    
    if (strcmp(ks, vs) == 0) {
      js_prop_iter_end(&iter);
      return match_resolve_arm(js, arm, value);
    }
    
    if (
      strcmp(ks, "true") == 0 
      && vtype(guard_arm) == T_UNDEF
    ) guard_arm = arm;
  }

  js_prop_iter_end(&iter);
  if (vtype(guard_arm) != T_UNDEF) return match_resolve_arm(js, guard_arm, value);
  
  ant_value_t fallback = js_get_sym(js, arms, get_default_sym());
  if (vtype(fallback) != T_UNDEF) return match_resolve_arm(js, fallback, value);

  return js_mkundef();
}

static ant_value_t hl_get_tagged(ant_t *js, ant_value_t *args, int nargs) {
  size_t input_len;
  char *input = js_getstr(js, args[0], &input_len);

  size_t out_size = input_len * 8 + 1;
  char *out = malloc(out_size);
  if (!out) return js_mkerr(js, "out of memory");

  int len = ant_highlight(input, input_len, out, out_size);
  ant_value_t result = js_mkstr(js, out, (size_t)len);
  free(out);
  
  return result;
}

static ant_value_t hl_render_tagged(ant_t *js, ant_value_t tagged) {
  size_t tagged_len;
  char *tagged_str = js_getstr(js, tagged, &tagged_len);

  size_t ansi_size = tagged_len * 2 + 1;
  char *ansi = malloc(ansi_size);
  if (!ansi) return js_mkerr(js, "out of memory");

  int len = crsprintf_stateful(ansi, ansi_size, NULL, tagged_str);
  ant_value_t result = js_mkstr(js, ansi, len < 0 ? 0 : (size_t)len);
  free(ansi);
  
  return result;
}

static ant_value_t js_highlight(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.highlight() requires 1 argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Ant.highlight() argument must be a string");

  ant_value_t tagged = hl_get_tagged(js, args, nargs);
  if (is_err(tagged)) return tagged;
  return hl_render_tagged(js, tagged);
}

static ant_value_t js_highlight_render(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.highlight.render() requires 1 argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Ant.highlight.render() argument must be a string");
  return hl_render_tagged(js, args[0]);
}

static ant_value_t js_highlight_tags(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Ant.highlight.tags() requires 1 argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "Ant.highlight.tags() argument must be a string");
  return hl_get_tagged(js, args, nargs);
}

void init_builtin_module() {
  ant_t *js = rt->js;
  ant_value_t ant_obj = rt->ant_obj;

  js_set(js, ant_obj, "match", js_mkfun(js_match));
  js_set(js, ant_obj, "stats", js_mkfun(js_stats_fn));
  js_set(js, ant_obj, "signal", js_mkfun(js_signal));
  js_set(js, ant_obj, "sleep", js_mkfun(js_sleep));
  js_set(js, ant_obj, "msleep", js_mkfun(js_msleep));
  js_set(js, ant_obj, "usleep", js_mkfun(js_usleep));
  js_set(js, ant_obj, "suppressReporting", js_mkfun(js_suppress_reporting));
  
  ant_value_t hl_obj = js_newobj(js);
  ant_value_t hl_fn = js_obj_to_func(hl_obj);
  
  js_set_slot(hl_obj, SLOT_CFUNC, js_mkfun(js_highlight));
  js_set(js, hl_fn, "render", js_mkfun(js_highlight_render));
  js_set(js, hl_fn, "tags", js_mkfun(js_highlight_tags));
  js_set(js, ant_obj, "highlight", hl_fn);

  ant_value_t raw_obj = js_newobj(js);
  js_set_getter_desc(js, js_as_obj(raw_obj), "stack", 5, js_mkfun(js_raw_stack), JS_DESC_C);
  js_set(js, raw_obj, "typeof", js_mkfun(js_raw_typeof));
  js_set(js, raw_obj, "ctorPropFeedback", js_mkfun(js_raw_ctor_prop_feedback));
  js_set(js, raw_obj, "gcMarkProfile", js_mkfun(js_raw_gc_mark_profile));
  js_set(js, raw_obj, "gcMarkProfileEnable", js_mkfun(js_raw_gc_mark_profile_enable));
  js_set(js, raw_obj, "gcMarkProfileReset", js_mkfun(js_raw_gc_mark_profile_reset));
  js_set(js, ant_obj, "raw", raw_obj);
}
