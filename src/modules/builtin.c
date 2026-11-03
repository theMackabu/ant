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
#include "cfunc.h"
#include "gc.h"
#include "crash.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "highlight.h"
#include "descriptors.h"

#include "silver/call.h"
#include "modules/builtin.h"
#include "modules/buffer.h"
#include "modules/cjit.h"
#include "modules/server.h"

static struct {
  ant_t *js;
  ant_value_t handler;
} signal_handlers[NSIG] = {0};

static void general_signal_handler(int signum) {
  if (signum < 0 || signum >= NSIG) return;
  ant_t *js = signal_handlers[signum].js;
  ant_value_t handler = signal_handlers[signum].handler;
  
  if (js && vtype(handler) != kTypeUndefined) {
    ant_value_t args[] = {Ant_Value_FromNumber(signum)};
    sv_vm_call(js->vm, js, handler, Ant_Value_Undefined, args, 1, NULL, Ant_Value_Undefined);
  }
  
  exit(0);
}

// Ant.signal(signal, handler)
static ant_value_t js_signal(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "Ant.signal() requires 2 arguments");
  
  int signum = (int)Ant_Value_AsNumberUnchecked(args[0]);
  if (signum <= 0 || signum >= NSIG) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid signal number: %d", signum);
  }
  
  signal_handlers[signum].js = js;
  signal_handlers[signum].handler = args[1];
  signal(signum, general_signal_handler);
  
  return Ant_Value_Undefined;
}

// Ant.raw.stack()
static ant_value_t js_raw_stack(ant_params_t) {
  return js_capture_raw_stack(js);
}

// Ant.raw.typeof(ant_value_t)
static ant_value_t js_raw_typeof(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Ant.raw.typeof() requires 1 argument");
  const uint8_t type = vtype(args[0]);
  return Ant_Value_FromNumber((double)type);
}

static ant_value_t js_raw_gc_mark_profile(ant_params_t) {
  gc_func_mark_profile_t p = gc_func_mark_profile_get(js);
  ant_value_t out = Ant_Object_Create(js);
  
  js_set(js, out, "enabled", Ant_Value_FromBool(p.enabled));
  js_set(js, out, "collections", Ant_Value_FromNumber((double)p.collections));
  js_set(js, out, "funcVisits", Ant_Value_FromNumber((double)p.func_visits));
  js_set(js, out, "childEdges", Ant_Value_FromNumber((double)p.child_edges));
  js_set(js, out, "constSlots", Ant_Value_FromNumber((double)p.const_slots));
  js_set(js, out, "timeNs", Ant_Value_FromNumber((double)p.time_ns));
  js_set(js, out, "timeMs", Ant_Value_FromNumber((double)p.time_ns / 1000000.0));
  
  return out;
}

static ant_value_t js_raw_gc_mark_profile_enable(ant_params_t) {
  bool enabled = true;
  if (nargs > 0) enabled = js_truthy(js, args[0]);
  gc_func_mark_profile_enable(js, enabled);
  return Ant_Value_FromBool(enabled);
}

static ant_value_t js_raw_gc_mark_profile_reset(ant_params_t) {
  gc_func_mark_profile_reset(js);
  return Ant_Value_Undefined;
}

// Ant.sleep(seconds)
static ant_value_t js_sleep(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Ant.sleep() requires 1 argument");
  unsigned int seconds = (unsigned int)Ant_Value_AsNumberUnchecked(args[0]);
  sleep(seconds);
  return Ant_Value_Undefined;
}

// Ant.msleep(milliseconds)
static ant_value_t js_msleep(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Ant.msleep() requires 1 argument");
  long ms = (long)Ant_Value_AsNumberUnchecked(args[0]);
  struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
  struct timespec rem;
  while (nanosleep(&ts, &rem) == -1 && errno == EINTR) ts = rem;
  return Ant_Value_Undefined;
}

// Ant.usleep(microseconds)
static ant_value_t js_usleep(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Ant.usleep() requires 1 argument");
  useconds_t us = (useconds_t)Ant_Value_AsNumberUnchecked(args[0]);
  usleep(us);
  return Ant_Value_Undefined;
}

// Ant.suppressReporting()
static ant_value_t js_suppress_reporting(ant_params_t) {
  ant_crash_suppress_reporting();
  return Ant_Value_Undefined;
}

// Ant.serve(options)
static ant_value_t js_serve(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Ant.serve() requires 1 argument");
  return server_start_from_export(js, args[0]);
}

// Ant.stats()
static ant_value_t js_stats_fn(ant_params_t) {
  ant_value_t result = Ant_Object_Create(js);
  
  ant_pool_stats_t rope_s = js_rope_pool_stats(js);
  ant_pool_stats_t sym_s = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t bigint_s = js_class_pool_stats(&js->pool.bigint);
  ant_string_pool_stats_t string_s = js_string_pool_stats(&js->pool.string);

  ant_value_t pools = Ant_Object_Create(js);
  ant_value_t rope_obj = Ant_Object_Create(js);
  js_set(js, rope_obj, "used", Ant_Value_FromNumber((double)rope_s.used));
  js_set(js, rope_obj, "capacity", Ant_Value_FromNumber((double)rope_s.capacity));
  js_set(js, rope_obj, "blocks", Ant_Value_FromNumber((double)rope_s.blocks));
  js_set(js, pools, "rope", rope_obj);

  ant_value_t sym_obj = Ant_Object_Create(js);
  js_set(js, sym_obj, "used", Ant_Value_FromNumber((double)sym_s.used));
  js_set(js, sym_obj, "capacity", Ant_Value_FromNumber((double)sym_s.capacity));
  js_set(js, sym_obj, "blocks", Ant_Value_FromNumber((double)sym_s.blocks));
  js_set(js, pools, "symbol", sym_obj);

  ant_value_t bigint_obj = Ant_Object_Create(js);
  js_set(js, bigint_obj, "used", Ant_Value_FromNumber((double)bigint_s.used));
  js_set(js, bigint_obj, "capacity", Ant_Value_FromNumber((double)bigint_s.capacity));
  js_set(js, bigint_obj, "blocks", Ant_Value_FromNumber((double)bigint_s.blocks));
  js_set(js, pools, "bigint", bigint_obj);

  ant_value_t string_obj = Ant_Object_Create(js);
  js_set(js, string_obj, "used", Ant_Value_FromNumber((double)string_s.total.used));
  js_set(js, string_obj, "capacity", Ant_Value_FromNumber((double)string_s.total.capacity));
  js_set(js, string_obj, "blocks", Ant_Value_FromNumber((double)string_s.total.blocks));

  ant_value_t string_pooled_obj = Ant_Object_Create(js);
  js_set(js, string_pooled_obj, "used", Ant_Value_FromNumber((double)string_s.pooled.used));
  js_set(js, string_pooled_obj, "capacity", Ant_Value_FromNumber((double)string_s.pooled.capacity));
  js_set(js, string_pooled_obj, "blocks", Ant_Value_FromNumber((double)string_s.pooled.blocks));
  js_set(js, string_obj, "pooled", string_pooled_obj);

  ant_value_t string_large_live_obj = Ant_Object_Create(js);
  js_set(js, string_large_live_obj, "used", Ant_Value_FromNumber((double)string_s.large_live.used));
  js_set(js, string_large_live_obj, "capacity", Ant_Value_FromNumber((double)string_s.large_live.capacity));
  js_set(js, string_large_live_obj, "blocks", Ant_Value_FromNumber((double)string_s.large_live.blocks));
  js_set(js, string_obj, "largeLive", string_large_live_obj);

  ant_value_t string_large_reusable_obj = Ant_Object_Create(js);
  js_set(js, string_large_reusable_obj, "used", Ant_Value_FromNumber((double)string_s.large_reusable.used));
  js_set(js, string_large_reusable_obj, "capacity", Ant_Value_FromNumber((double)string_s.large_reusable.capacity));
  js_set(js, string_large_reusable_obj, "blocks", Ant_Value_FromNumber((double)string_s.large_reusable.blocks));
  js_set(js, string_obj, "largeReusable", string_large_reusable_obj);

  ant_value_t string_large_quarantine_obj = Ant_Object_Create(js);
  js_set(js, string_large_quarantine_obj, "used", Ant_Value_FromNumber((double)string_s.large_quarantine.used));
  js_set(js, string_large_quarantine_obj, "capacity", Ant_Value_FromNumber((double)string_s.large_quarantine.capacity));
  js_set(js, string_large_quarantine_obj, "blocks", Ant_Value_FromNumber((double)string_s.large_quarantine.blocks));
  js_set(js, string_obj, "largeQuarantine", string_large_quarantine_obj);
  
  size_t pool_used = rope_s.used + sym_s.used + bigint_s.used + string_s.total.used;
  size_t pool_cap = rope_s.capacity + sym_s.capacity + bigint_s.capacity + string_s.total.capacity;
  
  js_set(js, pools, "totalUsed", Ant_Value_FromNumber((double)pool_used));
  js_set(js, pools, "totalCapacity", Ant_Value_FromNumber((double)pool_cap));
  js_set(js, result, "pools", pools);
  js_set(js, pools, "string", string_obj);

  size_t obj_count      = 0;
  size_t obj_bytes      = 0;
  size_t overflow_bytes = 0;
  size_t extra_bytes    = 0;
  size_t promise_bytes  = 0;
  size_t proxy_bytes    = 0;
  size_t exotic_bytes   = 0;

  for (int pass = 0; pass < 3; pass++) {
    ant_object_t *head = pass == 0 
      ? js->objects : pass == 1 
      ? js->objects_old : js->permanent_objects;
    
    for (ant_object_t *obj = head; obj; obj = obj->next) {
      obj_count++;
      obj_bytes += ant_object_cell_size(ant_object_inobj_limit(obj));
      
      uint32_t inobj_limit = ant_object_inobj_limit(obj);
      
      if (obj->overflow_prop && obj->prop_count > inobj_limit)
        overflow_bytes += (obj->prop_count - inobj_limit) * sizeof(ant_value_t);
        
      uint8_t extra_count = 0;
      ant_extra_slot_t *slots = ant_object_extra_slots(obj, &extra_count);
      
      if (slots) extra_bytes += ant_object_extra_capacity(obj) * sizeof(ant_extra_slot_t);
      if (ant_object_promise_state(obj)) promise_bytes += sizeof(ant_promise_state_t);
      if (ant_object_proxy_state(obj)) proxy_bytes += sizeof(ant_proxy_state_t);
      if (ant_object_has_sidecar(obj)) extra_bytes += sizeof(ant_object_sidecar_t);
      if (ant_object_owned_exotic_ops(obj)) exotic_bytes += sizeof(ant_exotic_ops_t);
    }
  }

  ant_value_t alloc = Ant_Object_Create(js);
  js_set(js, alloc, "objectCount", Ant_Value_FromNumber((double)obj_count));
  js_set(js, alloc, "objects", Ant_Value_FromNumber((double)obj_bytes));
  js_set(js, alloc, "overflow", Ant_Value_FromNumber((double)overflow_bytes));
  js_set(js, alloc, "extraSlots", Ant_Value_FromNumber((double)extra_bytes));
  js_set(js, alloc, "promises", Ant_Value_FromNumber((double)promise_bytes));
  js_set(js, alloc, "proxies", Ant_Value_FromNumber((double)proxy_bytes));
  js_set(js, alloc, "exotic", Ant_Value_FromNumber((double)exotic_bytes));
  js_set(js, alloc, "arrays", Ant_Value_FromNumber((double)js->alloc_bytes.arrays));
  
  size_t shape_bytes = ant_shape_total_bytes(js);
  js_set(js, alloc, "shapes", Ant_Value_FromNumber((double)shape_bytes));
  js_set(js, alloc, "closures", Ant_Value_FromNumber((double)js->alloc_bytes.closures));
  js_set(js, alloc, "upvalues", Ant_Value_FromNumber((double)js->alloc_bytes.upvalues));

  size_t alloc_total = obj_bytes + overflow_bytes + extra_bytes
    + promise_bytes + proxy_bytes + exotic_bytes + js->alloc_bytes.arrays
    + shape_bytes + js->alloc_bytes.closures + js->alloc_bytes.upvalues;
  
  js_set(js, alloc, "total", Ant_Value_FromNumber((double)alloc_total));
  js_set(js, result, "alloc", alloc);

  size_t buffer_mem = buffer_get_external_memory(js);
  size_t code_mem = code_arena_get_memory(js);
  size_t parse_mem = parse_arena_get_memory(js);
  size_t external_total = buffer_mem + code_mem + parse_mem;
  
  ant_value_t ext = Ant_Object_Create(js);
  js_set(js, ext, "buffers", Ant_Value_FromNumber((double)buffer_mem));
  js_set(js, ext, "code", Ant_Value_FromNumber((double)code_mem));
  js_set(js, ext, "parse", Ant_Value_FromNumber((double)parse_mem));
  js_set(js, ext, "total", Ant_Value_FromNumber((double)external_total));
  js_set(js, result, "external", ext);
  
  js_intern_stats_t intern_stats = js_intern_stats(js);
  ant_value_t intern = Ant_Object_Create(js);
  
  js_set(js, intern, "count", Ant_Value_FromNumber((double)intern_stats.count));
  js_set(js, intern, "bytes", Ant_Value_FromNumber((double)intern_stats.bytes));
  js_set(js, result, "intern", intern);
  
  sv_vm_t *vm = js->vm;
  if (vm) {
    ant_value_t vmobj = Ant_Object_Create(js);
    js_set(js, vmobj, "stackSize", Ant_Value_FromNumber((double)vm->stack_size));
    js_set(js, vmobj, "stackUsed", Ant_Value_FromNumber((double)vm->sp));
    js_set(js, vmobj, "maxFrames", Ant_Value_FromNumber((double)vm->max_frames));
    js_set(js, vmobj, "framesUsed", Ant_Value_FromNumber((double)(vm->fp + 1)));
    js_set(js, result, "vm", vmobj);
  }

  if (js->cstk.base != NULL) {
    volatile char marker;
    uintptr_t base = (uintptr_t)js->cstk.base;
    uintptr_t curr = (uintptr_t)&marker;
    size_t used = (base > curr) ? (base - curr) : (curr - base);
    ant_value_t cstk = Ant_Object_Create(js);
    js_set(js, cstk, "used", Ant_Value_FromNumber((double)used));
    js_set(js, cstk, "limit", Ant_Value_FromNumber((double)js->cstk.limit));
    js_set(js, result, "cstack", cstk);
  } else js_set(js, result, "cstack", Ant_Value_FromNumber(0));
  
#ifdef __APPLE__
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
    js_set(js, result, "residentSize", Ant_Value_FromNumber((double)info.resident_size));
    js_set(js, result, "virtualSize", Ant_Value_FromNumber((double)info.virtual_size));
  }

  task_vm_info_data_t vm_info;
  mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm_info, &vm_count) == KERN_SUCCESS) {
    js_set(js, result, "rss", Ant_Value_FromNumber((double)vm_info.phys_footprint));
    js_set(js, result, "physFootprint", Ant_Value_FromNumber((double)vm_info.phys_footprint));
  } else if (vtype(js_get(js, result, "rss")) == kTypeUndefined) {
    ant_value_t resident = js_get(js, result, "residentSize");
    if (vtype(resident) != kTypeUndefined) js_set(js, result, "rss", resident);
  }
#elif defined(__linux__)
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    js_set(js, result, "rss", Ant_Value_FromNumber((double)usage.ru_maxrss * 1024));
  }
#endif
  
  return result;
}

static inline ant_value_t match_resolve_arm(ant_t *js, ant_value_t arm, ant_value_t value) {
  if (!is_callable(arm)) return arm;
  return sv_vm_call(js->vm, js, arm, Ant_Value_Undefined, &value, 1, NULL, Ant_Value_Undefined);
}

static ant_value_t js_match(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "Ant.match() requires 2 arguments");

  ant_value_t value = args[0];
  ant_value_t arms = args[1];

  if (is_callable(arms)) {
    arms = sv_vm_call(js->vm, js, arms, Ant_Value_Undefined, &value, 1, NULL, Ant_Value_Undefined);
    if (is_err(arms)) return arms;
  }

  if (!is_object_type(arms)) return Ant_Value_Undefined;
  ant_value_t value_str = js_tostring_val(js, value);
  const char *vs = js_getstr(js, value_str, NULL);

  ant_iter_t iter = js_prop_iter_begin(js, arms);
  ant_value_t guard_arm = Ant_Value_Undefined;
  
  ant_value_t key = Ant_Value_Undefined;
  ant_value_t arm = Ant_Value_Undefined;

  while (js_prop_iter_next_val(&iter, &key, &arm)) {
    if (vtype(key) == kTypeSymbol) continue;
    
    const char *ks = js_getstr(js, key, NULL);
    if (!ks) continue;
    
    if (strcmp(ks, vs) == 0) {
      js_prop_iter_end(&iter);
      return match_resolve_arm(js, arm, value);
    }
    
    if (
      strcmp(ks, "true") == 0 
      && vtype(guard_arm) == kTypeUndefined
    ) guard_arm = arm;
  }

  js_prop_iter_end(&iter);
  if (vtype(guard_arm) != kTypeUndefined) return match_resolve_arm(js, guard_arm, value);
  
  ant_value_t fallback = js_get_sym(js, arms, js->sym.default_sym);
  if (vtype(fallback) != kTypeUndefined) return match_resolve_arm(js, fallback, value);

  return Ant_Value_Undefined;
}

static ant_value_t hl_get_tagged(ant_native_params_t) {
  size_t input_len;
  char *input = js_getstr(js, args[0], &input_len);

  size_t out_size = input_len * 8 + 1;
  char *out = malloc(out_size);
  if (!out) return Ant_Error_CreateOutOfMemory(js);

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
  if (!ansi) return Ant_Error_CreateOutOfMemory(js);

  int len = crsprintf_stateful(ansi, ansi_size, NULL, tagged_str);
  ant_value_t result = js_mkstr(js, ansi, len < 0 ? 0 : (size_t)len);
  free(ansi);
  
  return result;
}

static ant_value_t js_highlight(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Ant.highlight() requires 1 argument");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Ant.highlight() argument must be a string");

  ant_value_t tagged = hl_get_tagged(js, args, nargs);
  if (is_err(tagged)) return tagged;
  return hl_render_tagged(js, tagged);
}

static ant_value_t js_highlight_render(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Ant.highlight.render() requires 1 argument");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Ant.highlight.render() argument must be a string");
  return hl_render_tagged(js, args[0]);
}

static ant_value_t js_highlight_tags(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "Ant.highlight.tags() requires 1 argument");
  if (vtype(args[0]) != kTypeString) return js_mkerr(js, "Ant.highlight.tags() argument must be a string");
  return hl_get_tagged(js, args, nargs);
}

void init_builtin_module(ant_t *js) {
  ant_value_t ant_obj = js->Ant;
  js_set_sym(js, ant_obj, js->sym.toStringTag_sym, Ant_String_FromLiteral(js, "Ant"));

  js_set_exact(js, ant_obj, "version", Ant_String_FromLiteral(js, ANT_VERSION));
  js_set_exact(js, ant_obj, "channel", ANT_CANARY ? Ant_String_FromLiteral(js, "canary") : Ant_String_FromLiteral(js, "stable"));

  js_set(js, ant_obj, "match", Ant_Function_Create(js_match));
  js_set(js, ant_obj, "serve", Ant_Function_CreateWithArity(js_serve, 1));
  js_set(js, ant_obj, "stats", Ant_Function_Create(js_stats_fn));
  js_set(js, ant_obj, "signal", Ant_Function_Create(js_signal));
  js_set(js, ant_obj, "sleep", Ant_Function_Create(js_sleep));
  js_set(js, ant_obj, "msleep", Ant_Function_Create(js_msleep));
  js_set(js, ant_obj, "usleep", Ant_Function_Create(js_usleep));
  js_set(js, ant_obj, "suppressReporting", Ant_Function_Create(js_suppress_reporting));
  
  ant_value_t hl_fn = Ant_Function_CreateWithData(js, js_highlight, "highlight", 1, Ant_Value_Undefined);
  js_set(js, hl_fn, "render", Ant_Function_Create(js_highlight_render));
  js_set(js, hl_fn, "tags", Ant_Function_Create(js_highlight_tags));
  js_set(js, ant_obj, "highlight", hl_fn);

  ant_value_t unsafe_obj = Ant_Object_Create(js);
  init_cjit_module(js, unsafe_obj);
  js_set(js, ant_obj, "unsafe", unsafe_obj);

  ant_value_t raw_obj = Ant_Object_Create(js);
  js_set_getter_desc(js, js_as_obj(raw_obj), "stack", 5, Ant_Function_Create(js_raw_stack), JS_DESC_C);
  js_set(js, raw_obj, "typeof", Ant_Function_Create(js_raw_typeof));
  js_set(js, raw_obj, "gcMarkProfile", Ant_Function_Create(js_raw_gc_mark_profile));
  js_set(js, raw_obj, "gcMarkProfileEnable", Ant_Function_Create(js_raw_gc_mark_profile_enable));
  js_set(js, raw_obj, "gcMarkProfileReset", Ant_Function_Create(js_raw_gc_mark_profile_reset));
  js_set(js, ant_obj, "raw", raw_obj);
}
