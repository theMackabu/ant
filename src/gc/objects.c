#include "ptr.h"
#include "sugar.h"
#include "shapes.h"
#include "internal.h"

#include "silver/engine.h"
#include "silver/eval_env.h"
#include "modules/regex.h"
#include "modules/generator.h"
#include "modules/collections.h"

#include "gc.h"
#include "gc/bigints.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "gc/weak.h"
#include "gc/modules.h"

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/time.h>
#include <utarray.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define ANT_HAS_ASAN 1
#endif
#endif

#ifndef ANT_HAS_ASAN
#define ANT_HAS_ASAN 0
#endif

static inline bool gc_stack_word_readable(uintptr_t addr) {
#if ANT_HAS_ASAN
  return __asan_region_is_poisoned((void *)addr, sizeof(uint64_t)) == NULL;
#else
  return true;
#endif
}

static inline bool gc_get_stack_bounds(
  uintptr_t base, uintptr_t sp, uintptr_t *lo, uintptr_t *hi
) {
  if (base == 0 || sp == 0) return false;
  uintptr_t minp = (sp < base) ? sp : base;
  uintptr_t maxp = (sp < base) ? base : sp;

  uintptr_t aligned_lo = (minp + sizeof(uint64_t) - 1u) & ~(sizeof(uint64_t) - 1u);
  uintptr_t aligned_hi = maxp & ~(sizeof(uint64_t) - 1u);

  if (aligned_lo >= aligned_hi) return false;
  *lo = aligned_lo;
  *hi = aligned_hi;
  return true;
}

static gc_func_mark_profile_t g_gc_func_mark_profile = {0};
static gc_str_mark_fn g_str_mark = NULL;

static uint32_t g_gc_func_mark_profile_depth = 0;
static uint64_t g_gc_func_mark_profile_start_ns = 0;

static void gc_mark_coroutine(ant_t *js, coroutine_t *c);
static void gc_mark_closure(ant_t *js, sv_closure_t *c);

static uint64_t gc_epoch = 0;
static uint8_t gc_obj_epoch = 0;
static bool g_minor_gc = false;

static_assert(
  offsetof(sv_closure_t, call_flags) == 0,
  "closure arena free-list links must overlay call_flags"
);

static_assert(
  SV_CALL_HAS_BOUND_ARGS < _Alignof(sv_closure_t),
  "closure arena sweeps re-read call_flags from free-list links; "
  "HAS_BOUND_ARGS must remain in the pointer-alignment-zero low bits"
);

static uint64_t gc_now_ns(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;
}

void gc_func_mark_profile_enable(bool enabled) {
  g_gc_func_mark_profile.enabled = enabled;
}

void gc_func_mark_profile_reset(void) {
  bool enabled = g_gc_func_mark_profile.enabled;
  g_gc_func_mark_profile = (gc_func_mark_profile_t){ .enabled = enabled };
  g_gc_func_mark_profile_depth = 0;
  g_gc_func_mark_profile_start_ns = 0;
}

gc_func_mark_profile_t gc_func_mark_profile_get(void) {
  return g_gc_func_mark_profile;
}

bool gc_obj_is_marked(const ant_object_t *obj) {
  return obj && obj->mark_epoch == gc_obj_epoch;
}

static void gc_obj_epoch_wrapped(ant_t *js) {
  if (!js) return;
  for (ant_object_t *o = js->objects; o; o = o->next)
    if (o->mark_epoch != ANT_GC_DEAD) o->mark_epoch = 0;
  for (ant_object_t *o = js->objects_old; o; o = o->next)
    if (o->mark_epoch != ANT_GC_DEAD) o->mark_epoch = 0;
  for (ant_object_t *o = js->permanent_objects; o; o = o->next)
    if (o->mark_epoch != ANT_GC_DEAD) o->mark_epoch = 0;
}

void gc_root_pending_promise(ant_t *js, ant_object_t *obj) {
  ant_promise_state_t *pd = obj ? obj->promise_state : NULL;
  if (!js || !pd || pd->gc_pending_rooted) return;

  pd->gc_pending_rooted = true;
  pd->gc_pending_prev = NULL;
  pd->gc_pending_next = js->pending_promises;
  if (js->pending_promises && js->pending_promises->promise_state)
    js->pending_promises->promise_state->gc_pending_prev = obj;
  js->pending_promises = obj;
}

void gc_unroot_pending_promise(ant_t *js, ant_object_t *obj) {
  ant_promise_state_t *pd = obj ? obj->promise_state : NULL;
  if (!js || !pd || !pd->gc_pending_rooted) return;

  pd->gc_pending_rooted = false;
  ant_object_t *prev = pd->gc_pending_prev;
  ant_object_t *next = pd->gc_pending_next;

  if (prev && prev->promise_state) prev->promise_state->gc_pending_next = next;
  else if (js->pending_promises == obj) js->pending_promises = next;
  if (next && next->promise_state) next->promise_state->gc_pending_prev = prev;

  pd->gc_pending_next = NULL;
  pd->gc_pending_prev = NULL;
}

static constexpr size_t GC_REMEMBERED_ROSTER_RETAIN_CAP = 256;
static constexpr size_t GC_YOUNG_ROSTER_RETAIN_CAP = 32768;

static void *shrink_ptr_roster(void *entries, size_t *cap, size_t target_cap) {
  if (*cap <= target_cap * 2) return entries;
  void *shrunk = realloc(entries, target_cap * sizeof(void *));
  
  if (!shrunk) return entries;
  *cap = target_cap;
  
  return shrunk;
}

void gc_remember_add(ant_t *js, ant_object_t *obj) {
  if (!obj || obj->flags.in_remember_set) return;
  if (js->remember_set_len >= js->remember_set_cap) {
    size_t new_cap = js->remember_set_cap ? js->remember_set_cap * 2 : 64;
    ant_object_t **ns = realloc(js->remember_set, new_cap * sizeof(*ns));
    if (!ns) { js->gc_remember_overflow = true; return; }
    js->remember_set = ns;
    js->remember_set_cap = new_cap;
  }
  obj->flags.in_remember_set = 1;
  js->remember_set[js->remember_set_len++] = obj;
}

void gc_remember_upvalue(ant_t *js, struct sv_upvalue *uv) {
  if (!js || !uv || js->gc_objects_running || uv->in_remember_set) return;

  if (js->remembered_upvalue_len >= js->remembered_upvalue_cap) {
    size_t new_cap = js->remembered_upvalue_cap ? js->remembered_upvalue_cap * 2 : 64;
    struct sv_upvalue **entries = realloc(js->remembered_upvalues, new_cap * sizeof(*entries));
    if (!entries) { js->gc_remember_overflow = true; return; }
    js->remembered_upvalues = entries;
    js->remembered_upvalue_cap = new_cap;
  }

  uv->in_remember_set = 1;
  js->remembered_upvalues[js->remembered_upvalue_len++] = uv;
}

static void gc_mark_remembered_upvalues(ant_t *js) {
  for (size_t i = 0; i < js->remembered_upvalue_len; i++)
    gc_mark_value(js, *js->remembered_upvalues[i]->location);
}

static void gc_clear_remembered_upvalues(ant_t *js) {
  for (size_t i = 0; i < js->remembered_upvalue_len; i++)
    js->remembered_upvalues[i]->in_remember_set = 0;
  js->remembered_upvalue_len = 0;

  js->remembered_upvalues = shrink_ptr_roster(
    js->remembered_upvalues, &js->remembered_upvalue_cap,
    GC_REMEMBERED_ROSTER_RETAIN_CAP
  );
}

void gc_remember_closure(ant_t *js, struct sv_closure *c) {
  if (!js || !c || c->in_remember_set) return;

  if (js->remembered_closure_len >= js->remembered_closure_cap) {
    size_t new_cap = js->remembered_closure_cap ? js->remembered_closure_cap * 2 : 64;
    struct sv_closure **entries = realloc(js->remembered_closures, new_cap * sizeof(*entries));
    if (!entries) { js->gc_remember_overflow = true; return; }
    js->remembered_closures = entries;
    js->remembered_closure_cap = new_cap;
  }

  c->in_remember_set = 1;
  js->remembered_closures[js->remembered_closure_len++] = c;
}

static void gc_mark_remembered_closures(ant_t *js) {
  for (size_t i = 0; i < js->remembered_closure_len; i++)
    gc_mark_closure(js, js->remembered_closures[i]);
}

static void gc_clear_remembered_closures(ant_t *js) {
  for (size_t i = 0; i < js->remembered_closure_len; i++)
    js->remembered_closures[i]->in_remember_set = 0;
  js->remembered_closure_len = 0;

  js->remembered_closures = shrink_ptr_roster(
    js->remembered_closures, &js->remembered_closure_cap,
    GC_REMEMBERED_ROSTER_RETAIN_CAP
  );
}

void gc_track_young_closure_slow(ant_t *js, struct sv_closure *c) {
  if (js->young_closure_len >= js->young_closure_cap) {
    size_t new_cap = js->young_closure_cap ? js->young_closure_cap * 2 : 1024;
    struct sv_closure **entries = realloc(js->young_closures, new_cap * sizeof(*entries));
    if (!entries) return;
    js->young_closures = entries;
    js->young_closure_cap = new_cap;
  }
  js->young_closures[js->young_closure_len++] = c;
}

void gc_track_young_upvalue_slow(ant_t *js, struct sv_upvalue *uv) {
  if (js->young_upvalue_len >= js->young_upvalue_cap) {
    size_t new_cap = js->young_upvalue_cap ? js->young_upvalue_cap * 2 : 1024;
    struct sv_upvalue **entries = realloc(js->young_upvalues, new_cap * sizeof(*entries));
    if (!entries) return;
    js->young_upvalues = entries;
    js->young_upvalue_cap = new_cap;
  }
  js->young_upvalues[js->young_upvalue_len++] = uv;
}

static inline void gc_release_closure_payload(sv_closure_t *c) {
  if (!(c->call_flags & SV_CALL_BORROWED_UPVALS) && c->upvalues != c->inline_upvals) free(c->upvalues);
  c->upvalues = NULL;
  if (c->call_flags & SV_CALL_HAS_BOUND_ARGS) free(c->u.bound.argv);
  c->u.bound.argv = NULL;
}

static void gc_sweep_young_closures(ant_t *js) {
  ant_fixed_arena_t *ca = &js->closure_arena;
  for (size_t i = 0; i < js->young_closure_len; i++) {
    sv_closure_t *c = js->young_closures[i];
    if (c->gc_epoch == gc_epoch) {
      c->generation = 1;
      js->gc_closure_promoted_since_major++;
      continue;
    }
    
    gc_release_closure_payload(c);
    fixed_arena_free_elem(ca, c);
  }
  
  js->young_closure_len = 0;
  js->young_closure_trigger = GC_CLOSURE_NURSERY_THRESHOLD;

  js->young_closures = shrink_ptr_roster(
    js->young_closures, &js->young_closure_cap,
    GC_YOUNG_ROSTER_RETAIN_CAP
  );
}

static void gc_sweep_young_upvalues(ant_t *js) {
  ant_fixed_arena_t *ua = &js->upvalue_arena;
  for (size_t i = 0; i < js->young_upvalue_len; i++) {
    struct sv_upvalue *uv = js->young_upvalues[i];
    if (uv->gc_epoch == gc_epoch) continue;
    fixed_arena_free_elem(ua, uv);
  }
  js->young_upvalue_len = 0;

  js->young_upvalues = shrink_ptr_roster(
    js->young_upvalues, &js->young_upvalue_cap,
    GC_YOUNG_ROSTER_RETAIN_CAP
  );
}

void gc_remember_func_const(ant_t *js, sv_func_t *func, uint32_t slot, ant_value_t value) {
  if (!js || !func || !is_tagged(value)) return;
  uint8_t type = vtype_tagged(value);
  
  if (type != kTypeFunction) {
    if (type == kTypeString) goto remember;
    if (((1u << type) & GC_OBJ_TYPE_MASK) == 0) return;
    ant_object_t *obj = (ant_object_t *)vptr(value);
    if (!obj || obj->flags.generation != 0) return;
  }

remember:
  for (size_t i = 0; i < js->remembered_func_const_len; i++) 
    if (js->remembered_func_consts[i].func == func && js->remembered_func_consts[i].slot == slot) return;

  if (js->remembered_func_const_len >= js->remembered_func_const_cap) {
    size_t new_cap = js->remembered_func_const_cap ? js->remembered_func_const_cap * 2 : 64;
    void *entries = realloc(js->remembered_func_consts, new_cap * sizeof(*js->remembered_func_consts));
    if (!entries) return;
    js->remembered_func_consts = entries;
    js->remembered_func_const_cap = new_cap;
  }

  js->remembered_func_consts[js->remembered_func_const_len].func = func;
  js->remembered_func_consts[js->remembered_func_const_len].slot = slot;
  js->remembered_func_const_len++;
}

#define GC_MARK_STACK_INIT 4096

static ant_object_t **gc_mark_stack = NULL;
static void gc_mark_promise_handlers(ant_t *js, ant_promise_state_t *pd);

static size_t gc_mark_sp   = 0;
static size_t gc_mark_cap  = 0;

static void gc_mark_stack_push(ant_object_t *obj) {
  if (gc_mark_sp >= gc_mark_cap) {
    size_t new_cap = gc_mark_cap ? gc_mark_cap * 2 : GC_MARK_STACK_INIT;
    ant_object_t **ns = realloc(gc_mark_stack, new_cap * sizeof(*ns));
    if (!ns) return;
    gc_mark_stack = ns;
    gc_mark_cap = new_cap;
  }
  gc_mark_stack[gc_mark_sp++] = obj;
}

static inline void gc_grey_obj(ant_t *js, ant_object_t *obj) {
  if (!obj || !fixed_arena_contains(&js->obj_arena, obj)) return;
  if (obj->mark_epoch == gc_obj_epoch || obj->mark_epoch == ANT_GC_DEAD) return;
  if (g_minor_gc && obj->flags.generation == 1) return;
  obj->mark_epoch = gc_obj_epoch;
  gc_mark_stack_push(obj);
}

static void gc_mark_func(ant_t *js, sv_func_t *func) {
  if (!func) return;
  if (func->gc_epoch == gc_epoch) return;

  bool prof = __builtin_expect(g_gc_func_mark_profile.enabled, 0);
  if (prof) {
    if (g_gc_func_mark_profile_depth++ == 0) g_gc_func_mark_profile_start_ns = gc_now_ns();
    g_gc_func_mark_profile.func_visits++;
    g_gc_func_mark_profile.child_edges += (uint64_t)func->child_func_count;
    g_gc_func_mark_profile.const_slots += (uint64_t)func->gc_const_slot_count;
  }

  func->gc_epoch = gc_epoch;

  for (int i = 0; i < func->child_func_count; i++) 
    gc_mark_func(js, func->child_funcs[i]);
    
  for (uint32_t i = 0; i < func->obj_site_count; i++) {
    if (func->obj_sites) ant_gc_shapes_mark(func->obj_sites[i].shared_shape);
  }

  for (int i = 0; i < func->gc_const_slot_count; i++) {
    uint32_t idx = func->gc_const_slots[i];
    ant_value_t v = func->constants[idx];
    gc_mark_value(js, v);
  }

  if (prof && --g_gc_func_mark_profile_depth == 0)
    g_gc_func_mark_profile.time_ns += gc_now_ns() - g_gc_func_mark_profile_start_ns;
}

static void gc_mark_remembered_func_consts(ant_t *js) {
for (size_t i = 0; i < js->remembered_func_const_len; i++) {
  sv_func_t *func = js->remembered_func_consts[i].func;
  uint32_t slot = js->remembered_func_consts[i].slot;
  if (!func || !func->constants || slot >= (uint32_t)func->const_count) continue;
  gc_mark_value(js, func->constants[slot]);
}}

static void gc_clear_remembered_func_consts(ant_t *js) {
  js->remembered_func_const_len = 0;

  if (js->remembered_func_const_cap > 512) {
  size_t target = 256;
  void *entries = realloc(
    js->remembered_func_consts,
    target * sizeof(*js->remembered_func_consts)
  );
  
  if (entries) {
    js->remembered_func_consts = entries;
    js->remembered_func_const_cap = target;
  }}
}

void gc_mark_upvalue_cells(ant_t *js, sv_upvalue_t *const *cells, uint32_t count) {
  if (!cells) return;
  for (uint32_t i = 0; i < count; i++) {
    sv_upvalue_t *uv = cells[i];
    if (!uv) continue;
    uv->gc_epoch = gc_epoch;
    gc_mark_value(js, *uv->location);
  }
}

static void gc_mark_closure(ant_t *js, sv_closure_t *c) {
  if (!c) return;
  if (c->gc_epoch == gc_epoch) return;
  
  c->gc_epoch = gc_epoch;
  if (js->weak_gc.pending_active)
    gc_weak_key_marked(js, mkref(kTypeFunction, c));
  gc_mark_func(js, c->func);
  if (c->func_obj) gc_mark_value(js, c->func_obj);
  gc_mark_value(js, c->module_ctx);
  gc_mark_value(js, c->bound_this);
  gc_mark_value(js, c->super_val);

  if (c->func)
    gc_mark_upvalue_cells(js, c->upvalues, (uint32_t)c->func->upvalue_count);
  if (c->call_flags & SV_CALL_HAS_BOUND_ARGS) {
    gc_mark_value(js, c->u.bound.args_arr);
    if (c->u.bound.argv)
      for (int i = 0; i < c->bound_argc; i++) gc_mark_value(js, c->u.bound.argv[i]);
  }
}

void gc_mark_value(ant_t *js, ant_value_t v) {
  if (!is_tagged(v)) return;
  uint8_t t = vtype_tagged(v);

  if (t == kTypeFunction) {
    gc_mark_closure(js, (sv_closure_t *)vptr(v));
    return;
  }

  if (t == kTypeString && g_str_mark) {
    g_str_mark(js, v);
    return;
  }

  if (t == kTypeBigInt) {
    gc_bigints_mark((const void *)vptr(v));
    return;
  }

  if (t == kTypeSymbol) {
    bool newly_marked = js_symbol_gc_mark(v, gc_epoch);
    if (newly_marked && js->weak_gc.pending_active)
      gc_weak_key_marked(js, v);
    return;
  }

  if (!((1u << t) & GC_OBJ_TYPE_MASK)) return;
  ant_object_t *obj = (ant_object_t *)vptr(v);
  
  if (!obj) return;
  gc_grey_obj(js, obj);
}

static void gc_scan_obj(ant_t *js, ant_object_t *obj) {
  ant_gc_shapes_mark(obj->shape);
  gc_mark_value(js, obj->proto);

  if (obj->type_tag != kTypeArray) gc_mark_value(js, obj->u.data.value);
  if (obj->type_tag == kTypeGenerator) {
    coroutine_t *coro = generator_get_coro_for_gc(js_obj_from_ptr(obj));
    if (coro) gc_mark_coroutine(js, coro);
    generator_mark_for_gc(js, js_obj_from_ptr(obj));
  }

  if (obj->type_tag == kTypeMap) {
    ant_value_t value = js_obj_from_ptr(obj);
    map_entry_t **head = (map_entry_t **)js_get_native(value, MAP_NATIVE_TAG);
    if (head) {
    map_entry_t *e, *tmp;
    HASH_ITER(hh, *head, e, tmp) { 
      gc_mark_value(js, e->key_val); 
      gc_mark_value(js, e->value); 
    }}
  } 
  
  else if (obj->type_tag == kTypeSet) {
    ant_value_t value = js_obj_from_ptr(obj);
    set_entry_t **head = (set_entry_t **)js_get_native(value, SET_NATIVE_TAG);
    if (head) {
      set_entry_t *e, *tmp;
      HASH_ITER(hh, *head, e, tmp) { gc_mark_value(js, e->value); }
    }
  }

  if (obj->shape) {
  uint32_t count = ant_shape_count(obj->shape);
  for (uint32_t i = 0; i < count && i < obj->prop_count; i++)
    gc_mark_value(js, ant_object_prop_get_unchecked(obj, i));
    
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(obj->shape, i);
    if (prop && prop->type == ANT_SHAPE_KEY_SYMBOL)
      gc_mark_value(js, mkval(kTypeSymbol, prop->key.sym_off));
    if (prop && prop->has_getter) gc_mark_value(js, prop->getter);
    if (prop && prop->has_setter) gc_mark_value(js, prop->setter);
  }}

  uint8_t extra_count = 0;
  ant_extra_slot_t *extra_slots = ant_object_extra_slots(obj, &extra_count);
  if (extra_slots) {
    for (uint8_t i = 0; i < extra_count; i++) gc_mark_value(js, extra_slots[i].value);
  }

  if (obj->type_tag == kTypeArray && obj->u.array.data) {
    uint32_t n = obj->u.array.len < obj->u.array.cap ? obj->u.array.len : obj->u.array.cap;
    for (uint32_t i = 0; i < n; i++) gc_mark_value(js, obj->u.array.data[i]);
  }

  ant_promise_state_t *pd = obj->promise_state;
  if (pd) {
    gc_mark_value(js, pd->value);
    gc_mark_value(js, pd->trigger_parent);
    gc_mark_promise_handlers(js, pd);
  }

  ant_proxy_state_t *proxy_state = ant_object_proxy_state(obj);
  if (proxy_state) {
    gc_mark_value(js, proxy_state->target);
    gc_mark_value(js, proxy_state->handler);
  }

  ant_private_table_t *table = ant_object_private_table(obj);
  if (table && table->entries) for (uint32_t i = 0; i < table->cap; i++) {
    ant_private_entry_t *entry = &table->entries[i];
    if (!entry->occupied) continue;
    gc_mark_value(js, entry->token);
    gc_mark_value(js, entry->value);
    gc_mark_value(js, entry->getter);
    gc_mark_value(js, entry->setter);
  }

  if (obj->native.tag != 0 || ant_object_has_sidecar(obj)) {
    ant_value_t value = js_obj_from_ptr(obj);
    sv_eval_env_gc_mark(js, obj);
    gc_mark_abort_signal_object(js, value, gc_mark_value);
    gc_mark_eventemitter_object(js, value, gc_mark_value);
  }
}

static void gc_drain_mark_stack(ant_t *js) {
while (gc_mark_sp > 0) {
  ant_object_t *obj = gc_mark_stack[--gc_mark_sp];
  gc_scan_obj(js, obj);
}}

static bool gc_weak_key_alive(ant_t *js, ant_value_t key) {
  uint8_t type = vtype(key);
  if (type == kTypeBuiltin) return true;

  if (type == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(key);
    if (!closure || !fixed_arena_contains(&js->closure_arena, closure))
      return false;
    return (g_minor_gc && closure->generation == 1) ||
      closure->gc_epoch == gc_epoch;
  }
  
  if (type == kTypeSymbol) {
    if (g_minor_gc) return true;
    return js_symbol_gc_is_permanent(key) ||
      js_symbol_gc_is_marked(key, gc_epoch);
  }
  
  if (((1u << type) & GC_OBJ_TYPE_MASK) == 0) return false;
  ant_object_t *obj = js_obj_ptr(key);
  if (!obj || !fixed_arena_contains(&js->obj_arena, obj) ||
      obj->mark_epoch == ANT_GC_DEAD) return false;
  
  return (g_minor_gc && obj->flags.generation == 1) ||
    obj->mark_epoch == gc_obj_epoch || obj->flags.gc_permanent;
}

static bool gc_weak_collection_live(const ant_object_t *obj) {
  return obj && ((g_minor_gc && obj->flags.generation == 1) ||
    obj->mark_epoch == gc_obj_epoch || obj->flags.gc_permanent);
}

static ant_value_t gc_weak_key_from_marked_object(ant_object_t *obj) {
switch (obj->type_tag) {
  case kTypeArray:
  case kTypePromise:
  case kTypeGenerator: return mkref(obj->type_tag, obj);
  default: return js_obj_from_ptr(obj);
}}

static void gc_drain_mark_stack_weak(ant_t *js) {
while (gc_mark_sp > 0) {
  ant_object_t *obj = gc_mark_stack[--gc_mark_sp];
  gc_weak_key_marked(js, gc_weak_key_from_marked_object(obj));
  gc_weak_collection_marked(js, obj);
  gc_scan_obj(js, obj);
}}

static void gc_scan_frame_span(
  ant_t *js, ant_value_t *slots, int slot_count,
  sv_frame_t *frames, int frame_count, sv_upvalue_t *open_upvalues
) {
  for (int i = 0; i < slot_count; i++)
    gc_mark_value(js, slots[i]);

  for (int f = 0; f < frame_count; f++) {
    sv_frame_t *frame = &frames[f];
    gc_mark_func(js, frame->func);
    gc_mark_value(js, frame->callee);
    gc_mark_value(js, frame->this);
    gc_mark_value(js, frame->new_target);
    gc_mark_value(js, frame->super_val);
    gc_mark_value(js, frame->with_obj);
    gc_mark_value(js, frame->completion.value);
    gc_mark_value(js, frame->arguments_obj);
    gc_mark_value(js, frame->eval_env);
  }

  for (sv_upvalue_t *uv = open_upvalues; uv; uv = uv->next) {
    uv->gc_epoch = gc_epoch;
    if (uv->location == &uv->closed) gc_mark_value(js, uv->closed);
  }

  for (int f = 0; f < frame_count; f++) {
  sv_frame_t *frame = &frames[f];
  if (!frame->upvalues) continue;
  for (int j = 0; j < frame->upvalue_count; j++) {
    sv_upvalue_t *uv = frame->upvalues[j];
    if (!uv) continue;
    uv->gc_epoch = gc_epoch;
    if (uv->location == &uv->closed) gc_mark_value(js, uv->closed);
  }}
}

static void gc_scan_vm_stack(ant_t *js, sv_vm_t *vm) {
  if (!vm) return;
  gc_scan_frame_span(js, vm->stack, vm->sp, vm->frames, vm->fp + 1, vm->open_upvalues);
}

static void gc_scan_activation(ant_t *js, sv_activation_t *act) {
  if (!act || act->frame_count <= 0) return;
  gc_scan_frame_span(js, act->slots, act->stack_count, act->frames, act->frame_count, act->open_upvalues);
}

static void gc_scan_range(ant_t *js, uintptr_t lo, uintptr_t hi) {
  for (uintptr_t addr = lo; addr < hi; addr += sizeof(uint64_t)) {
    if (!gc_stack_word_readable(addr)) continue;
    uint64_t w;
    memcpy(&w, (void *)addr, sizeof(w));
    
    ant_object_t *raw_obj = (ant_object_t *)(uintptr_t)w;
    if (fixed_arena_contains(&js->obj_arena, raw_obj))
      gc_grey_obj(js, raw_obj);
      
    sv_closure_t *raw_closure = (sv_closure_t *)(uintptr_t)w;
    if (fixed_arena_contains(&js->closure_arena, raw_closure))
      gc_mark_closure(js, raw_closure);

    sv_upvalue_t *raw_uv = (sv_upvalue_t *)(uintptr_t)w;
    while (raw_uv && fixed_arena_contains(&js->upvalue_arena, raw_uv)) {
      if (raw_uv->gc_epoch == gc_epoch) break;
      raw_uv->gc_epoch = gc_epoch;
      if (raw_uv->location == &raw_uv->closed) gc_mark_value(js, raw_uv->closed);
      raw_uv = raw_uv->next;
    }

    if (!is_tagged(w)) continue;
    uint8_t type = vtype_tagged(w);
    
    if ((1u << type) & GC_OBJ_TYPE_MASK) {
      ant_object_t *obj = (ant_object_t *)vptr(w);
      if (obj) gc_grey_obj(js, obj);
    }
    
    if (type == kTypeFunction) {
      sv_closure_t *c = (sv_closure_t *)vptr(w);
      if (c && fixed_arena_contains(&js->closure_arena, c)) gc_mark_closure(js, c);
    }
    
    if (type == kTypeString && g_str_mark) g_str_mark(js, w);
    if (type == kTypeBigInt)
      gc_bigints_mark((const void *)vptr(w));
  }
}

void gc_mark_conservative_range(ant_t *js, const void *ptr, size_t size) {
  if (!js || !ptr || size < sizeof(uint64_t)) return;
  size_t bytes = size & ~(sizeof(uint64_t) - 1u);
  uintptr_t lo = (uintptr_t)ptr;
  if (lo > UINTPTR_MAX - bytes) return;
  gc_scan_range(js, lo, lo + bytes);
}

__attribute__((noinline))
static void gc_scan_current_stack(ant_t *js) {
  jmp_buf jb;
  if (setjmp(jb) != 0) return;
  
  volatile uint8_t sp_marker = 0;
  uintptr_t lo, hi;
  
  if (
    !gc_get_stack_bounds((uintptr_t)js->cstk.base, 
    (uintptr_t)&sp_marker, &lo, &hi)
  ) return;
  
  gc_scan_range(js, lo, hi);
}

static void gc_scan_other_stacks(ant_t *js) {
  if (js->cstk.main_base && js->cstk.main_lo) {
    uintptr_t lo, hi;
    if (gc_get_stack_bounds(
      (uintptr_t)js->cstk.main_base,
      (uintptr_t)js->cstk.main_lo, &lo, &hi)
    ) gc_scan_range(js, lo, hi);
  }
}

static void gc_mark_coroutine(ant_t *js, coroutine_t *c) {
  if (!c || c->gc_epoch == gc_epoch) return;
  c->gc_epoch = gc_epoch;
  
  gc_scan_activation(js, c->act);
  gc_mark_value(js, c->this_val);
  gc_mark_value(js, c->async_func);
  gc_mark_value(js, c->async_promise);
  gc_mark_value(js, c->awaited_promise);
  gc_mark_value(js, c->result);
  gc_mark_value(js, c->owner_gen);
  gc_mark_value(js, c->super_val);
  gc_mark_value(js, c->new_target);
  
  if (c->module_eval_ctx) {
    gc_mark_value(js, c->module_eval_ctx->module_ns);
    gc_mark_value(js, c->module_eval_ctx->module_ctx);
    gc_mark_value(js, c->module_eval_ctx->prev_import_meta_prop);
  }
  
  if (c->args) for (int i = 0; i < c->nargs; i++) gc_mark_value(js, c->args[i]);
}

static inline void gc_mark_promise_handler(ant_t *js, const promise_handler_t *h) {
  if (!h) return;
  
  gc_mark_value(js, h->onFulfilled);
  gc_mark_value(js, h->onRejected);
  gc_mark_value(js, h->nextPromise);
  
  if (h->await_coro) gc_mark_coroutine(js, h->await_coro);
}

static inline void gc_mark_promise_handlers(ant_t *js, ant_promise_state_t *pd) {
  if (!pd) return;

  if (pd->handler_count == 1) {
    gc_mark_promise_handler(js, &pd->inline_handler);
    return;
  }

  if (pd->handler_count <= 1 || !pd->handlers) return;
  promise_handler_t *h = NULL;
  
  while ((h = (promise_handler_t *)utarray_next(pd->handlers, h)))
    gc_mark_promise_handler(js, h);
}

static void gc_mark_roots(ant_t *js) {
  gc_scan_vm_stack(js, js->vm);

  for (coroutine_t *c = js->active_async_coro; c; c = c->active_parent) gc_mark_coroutine(js, c);

  gc_mark_value(js, js->global);
  gc_mark_value(js, js->Ant);
  
  gc_mark_value(js, js->esm.hooks);
  gc_mark_value(js, js->esm.import_meta);
  
  gc_mark_value(js, js->sym.object_proto);
  gc_mark_value(js, js->sym.array_proto);
  gc_mark_value(js, js->sym.function_proto);
  gc_mark_value(js, js->sym.string_proto);
  gc_mark_value(js, js->sym.number_proto);
  gc_mark_value(js, js->sym.boolean_proto);
  gc_mark_value(js, js->sym.promise_proto);
  gc_mark_value(js, js->sym.bigint_proto);
  gc_mark_value(js, js->sym.symbol_proto);
  gc_mark_value(js, js->sym.array_values_fn);
  
  gc_mark_value(js, js->this_val);
  gc_mark_value(js, js->new_target);
  gc_mark_value(js, js->current_func);
  gc_mark_value(js, js->thrown_value);
  gc_mark_value(js, js->thrown_stack);
  gc_mark_value(js, js->length_str);

  for (ant_module_t *ctx = js->esm.module_stack; ctx; ctx = ctx->prev) {
    gc_mark_value(js, ctx->module_ns);
    gc_mark_value(js, ctx->module_ctx);
    gc_mark_value(js, ctx->prev_import_meta_prop);
  }

  for (size_t i = 0; i < js->pending_rejections.len; i++)
    gc_mark_value(js, js->pending_rejections.items[i]);

  for (uint8_t i = 0; i < js->cfunc_promote_cache.len; i++)
    gc_mark_value(js, js->cfunc_promote_cache.promoted[i]);

  gc_weak_mark_kept_alive(js, gc_mark_value);
  gc_visit_roots(js, gc_mark_value);
  gc_mark_timers(js, gc_mark_value);
  gc_mark_cron(js, gc_mark_value);
  gc_mark_atomics(js, gc_mark_value);
  gc_mark_fetch(js, gc_mark_value);
  gc_mark_fs(js, gc_mark_value);
  gc_mark_child_process(js, gc_mark_value);
  gc_mark_readline(js, gc_mark_value);
  gc_mark_process(js, gc_mark_value);
  gc_mark_navigator(js, gc_mark_value);
  gc_mark_net(js, gc_mark_value);
  gc_mark_tls(js, gc_mark_value);
  gc_mark_server(js, gc_mark_value);
  gc_mark_websocket(js, gc_mark_value);
  gc_mark_eventsource(js, gc_mark_value);
  gc_mark_events(js, gc_mark_value);
  gc_mark_lmdb(js, gc_mark_value);
  gc_mark_symbols(js, gc_mark_value);
  gc_mark_esm(js, gc_mark_value);
  gc_mark_worker_threads(js, gc_mark_value);
  gc_mark_sandbox(js, gc_mark_value);
  gc_mark_abort(js, gc_mark_value);
  gc_mark_zlib(js, gc_mark_value);
  gc_mark_wasm(js, gc_mark_value);
  gc_mark_napi(js, gc_mark_value);
  gc_mark_rpc(js, gc_mark_value);

  for (ant_object_t *obj = js->pending_promises; obj;) {
    ant_promise_state_t *pd = obj->promise_state;
    ant_object_t *next = pd ? pd->gc_pending_next : NULL;
    gc_grey_obj(js, obj);
    obj = next;
  }

  gc_scan_current_stack(js);
  gc_scan_other_stacks(js);

  if (!g_minor_gc) {
    for (ant_object_t *obj = js->permanent_objects; obj; obj = obj->next) 
      gc_scan_obj(js, obj);
  }

  gc_drain_mark_stack(js);
}

#define GC_FREE_PAYLOAD_MASK                       \
  ((1u << kTypeArray) | (1u << kTypeMap) | (1u << kTypeSet) | \
   (1u << kTypeWeakMap) | (1u << kTypeWeakSet))

void gc_object_free(ant_t *js, ant_object_t *obj) {
  if (!obj) return;

  if ((((uintptr_t)obj->finalizer | (uintptr_t)obj->promise_state |
        (uintptr_t)obj->extra_slots | (uintptr_t)obj->overflow_prop |
        (uintptr_t)obj->exotic_ops) | obj->native.tag) == 0 &&
      (((1u << obj->type_tag) & GC_FREE_PAYLOAD_MASK) == 0)) {
    obj->mark_epoch = ANT_GC_DEAD;
    if (obj->shape) {
      ant_shape_release(obj->shape);
      obj->shape = NULL;
    }
    fixed_arena_free_elem(&js->obj_arena, obj);
    return;
  }

  if (obj->finalizer) obj->finalizer(js, obj);
  
  if (obj->native.tag != 0 || ant_object_has_sidecar(obj))
    gc_finalize_events_object(js, js_obj_from_ptr(obj));
  obj->mark_epoch = ANT_GC_DEAD;

  if (obj->shape) {
    ant_shape_release(obj->shape);
    obj->shape = NULL;
  }

  if (obj->promise_state && obj->promise_state->gc_pending_rooted)
    gc_unroot_pending_promise(js, obj);

  if (obj->promise_state) {
    if (obj->promise_state->handlers)
      utarray_free(obj->promise_state->handlers);
    free(obj->promise_state);
    obj->promise_state = NULL;
  }

  if (obj->type_tag == kTypeArray && obj->u.array.data) {
    size_t bytes = (size_t)obj->u.array.cap * sizeof(*obj->u.array.data);
    js->alloc_bytes.arrays = js->alloc_bytes.arrays > bytes ? js->alloc_bytes.arrays - bytes : 0;
    free(obj->u.array.data);
    obj->u.array.data = NULL;
  }

  switch (obj->type_tag) {
    case kTypeMap: {
      ant_value_t value = js_obj_from_ptr(obj);
      map_entry_t **head = (map_entry_t **)js_get_native(value, MAP_NATIVE_TAG);
      if (head) {
        map_entry_t *e, *tmp;
        HASH_ITER(hh, *head, e, tmp) { HASH_DEL(*head, e); free(e->key); free(e); }
        free(head);
        js_clear_native(value, MAP_NATIVE_TAG);
      }
      break;
    }
    case kTypeSet: {
      ant_value_t value = js_obj_from_ptr(obj);
      set_entry_t **head = (set_entry_t **)js_get_native(value, SET_NATIVE_TAG);
      if (head) {
        set_entry_t *e, *tmp;
        HASH_ITER(hh, *head, e, tmp) { HASH_DEL(*head, e); free(e->key); free(e); }
        free(head);
        js_clear_native(value, SET_NATIVE_TAG);
      }
      break;
    }
    case kTypeWeakMap: {
      ant_value_t value = js_obj_from_ptr(obj);
      weakmap_table_t *table = js_get_native(value, WEAKMAP_NATIVE_TAG);
      if (table) {
        weakmap_table_free(table);
        js_clear_native(value, WEAKMAP_NATIVE_TAG);
      }
      break;
    }
    case kTypeWeakSet: {
      ant_value_t value = js_obj_from_ptr(obj);
      weakset_entry_t **head = (weakset_entry_t **)js_get_native(value, WEAKSET_NATIVE_TAG);
      if (head) {
        weakset_entry_t *e, *tmp;
        HASH_ITER(hh, *head, e, tmp) { HASH_DEL(*head, e); free(e); }
        free(head);
        js_clear_native(value, WEAKSET_NATIVE_TAG);
      }
      break;
    }
    default: break;
  }

  if (ant_object_has_sidecar(obj)) {
    ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
    sv_eval_env_gc_free(obj);
    free(sidecar->extra_slots);
    free(sidecar->native_entries);
    free(sidecar->proxy_state);
    free(sidecar->private_table.entries);
    free(sidecar);
    obj->extra_slots = NULL;
  } 
  
  else if (obj->extra_slots) {
    free(obj->extra_slots);
    obj->extra_slots = NULL;
  }

  free(obj->overflow_prop);
  obj->overflow_prop = NULL;
  free((void *)obj->exotic_ops);
  obj->exotic_ops = NULL;
  fixed_arena_free_elem(&js->obj_arena, obj);
}

static void gc_sweep_young_and_promote(ant_t *js) {
  ant_object_t *obj = js->objects;
  js->objects = NULL;
  while (obj) {
    ant_object_t *next = obj->next;
    if (next) __builtin_prefetch(next);
    if (obj->mark_epoch == gc_obj_epoch) {
      obj->flags.generation = 1;
      obj->next = js->objects_old;
      js->objects_old = obj;
    } else gc_object_free(js, obj);
    obj = next;
  }
}

static void gc_promote_survivors(ant_t *js) {
  ant_object_t *obj = js->objects;
  while (obj) {
    ant_object_t *next = obj->next;
    obj->flags.generation = 1;
    obj->next = js->objects_old;
    js->objects_old = obj;
    obj = next;
  }
  js->objects = NULL;
}

static void gc_sweep(ant_t *js) {
  ant_object_t **pp = &js->objects;
  while (*pp) {
  ant_object_t *obj = *pp;
  if (obj->mark_epoch == gc_obj_epoch) pp = &obj->next; else {
    *pp = obj->next;
    gc_object_free(js, obj);
  }}

  pp = &js->objects_old;
  while (*pp) {
  ant_object_t *obj = *pp;
  if (obj->mark_epoch == gc_obj_epoch) pp = &obj->next; else {
    *pp = obj->next;
    gc_object_free(js, obj);
  }}
}

void gc_pin_existing_objects(ant_t *js) {
  if (!js) return;

  ant_fixed_arena_t *ua = &js->upvalue_arena;
  uint64_t stamp = gc_epoch ? gc_epoch : ~0ull;
  
  for (size_t off = 0; off < ua->watermark; off += ua->elem_size) {
    sv_upvalue_t *uv = (sv_upvalue_t *)(ua->base + off);
    if (uv->gc_epoch == 0) uv->gc_epoch = stamp;
  }

  ant_object_t *tail = NULL;
  for (ant_object_t *obj = js->objects; obj; obj = obj->next) {
    obj->flags.gc_permanent = 1;
    obj->flags.generation = 1;
    tail = obj;
  }
  
  if (tail) {
    tail->next = js->permanent_objects;
    js->permanent_objects = js->objects;
    js->objects = NULL;
  }
  
  tail = NULL;
  for (ant_object_t *obj = js->objects_old; obj; obj = obj->next) {
    obj->flags.gc_permanent = 1;
    tail = obj;
  }
  
  if (tail) {
    tail->next = js->permanent_objects;
    js->permanent_objects = js->objects_old;
    js->objects_old = NULL;
  }

  js->young_closure_len = 0;
  js->young_upvalue_len = 0;
  js->young_closure_trigger = GC_CLOSURE_NURSERY_THRESHOLD;
}

void gc_objects_run(
  ant_t *js, gc_str_mark_fn str_mark, gc_extra_roots_fn extra_roots
) {
  if (!js) return;
  js->gc_objects_running = true;

  g_str_mark = str_mark;
  if (g_gc_func_mark_profile.enabled) g_gc_func_mark_profile.collections++;
  
  gc_epoch++;
  if (gc_epoch == 0) gc_epoch = 1;

  gc_obj_epoch = (uint8_t)(gc_obj_epoch + 1u);
  if (gc_obj_epoch == 0 || gc_obj_epoch == ANT_GC_DEAD) {
    gc_obj_epoch = 1;
    gc_obj_epoch_wrapped(js);
  }

  ant_gc_shapes_begin();
  for (size_t i = 0; i < js->remember_set_len; i++)
    js->remember_set[i]->flags.in_remember_set = 0;
  js->remember_set_len = 0;
  
  gc_clear_remembered_func_consts(js);
  gc_clear_remembered_upvalues(js);
  gc_clear_remembered_closures(js);

  if (js->remember_set_cap > 512) {
    ant_object_t **ns = realloc(js->remember_set, 256 * sizeof(*ns));
    if (ns) { js->remember_set = ns; js->remember_set_cap = 256; }
  }

  if (extra_roots) extra_roots(js);
  gc_mark_roots(js);
  
  gc_weak_process(
    js, false, gc_mark_value, gc_drain_mark_stack_weak,
    gc_weak_key_alive, gc_weak_collection_live
  );
  
  gc_clear_napi_weak_refs(js, false);
  gc_age_regex_cache(js, false);
  gc_sweep(js);
  
  if (ant_gc_shapes_sweep()) ant_ic_epoch_bump();
  gc_promote_survivors(js);

  ant_fixed_arena_t *ca = &js->closure_arena;
  ca->free_list = NULL;
  ca->live_count = 0;
  
  for (size_t off = 0; off < ca->watermark; off += ca->elem_size) {
  sv_closure_t *c = (sv_closure_t *)(ca->base + off);
  
  if (c->gc_epoch == gc_epoch) {
    c->generation = 1;
    ca->live_count++;
  }
  else {
    gc_release_closure_payload(c);

    *(void **)c = ca->free_list;
    ca->free_list = c;
  }}

  ant_fixed_arena_t *ua = &js->upvalue_arena;
  ua->free_list = NULL;
  ua->live_count = 0;
  for (size_t off = 0; off < ua->watermark; off += ua->elem_size) {
    uint8_t *slot = ua->base + off;
    uint64_t epoch;
    memcpy(&epoch, slot + ua->epoch_offset, sizeof(epoch));
    if (epoch == gc_epoch) ua->live_count++;
    else {
      *(void **)slot = ua->free_list;
      ua->free_list = slot;
    }
  }

  js->young_closure_len = 0;
  js->young_upvalue_len = 0;
  js->young_closure_trigger = GC_CLOSURE_NURSERY_THRESHOLD;

  ant_fixed_arena_t *oa = &js->obj_arena;
  size_t new_wm = 0;

  for (size_t off = oa->watermark; off >= oa->elem_size; off -= oa->elem_size) {
    ant_object_t *slot = (ant_object_t *)(oa->base + off - oa->elem_size);
    if (slot->mark_epoch != ANT_GC_DEAD) { new_wm = off; break; }
  }

  if (new_wm < oa->watermark) {
    oa->free_list = NULL;
    
    for (size_t off = 0; off < new_wm; off += oa->elem_size) {
    ant_object_t *slot = (ant_object_t *)(oa->base + off);
    if (slot->mark_epoch == ANT_GC_DEAD) {
      *(void **)slot = oa->free_list;
      oa->free_list = slot;
    }}
    
    ant_arena_decommit(oa->base, oa->committed, new_wm);
    oa->committed = new_wm;
    oa->watermark = new_wm;
  }

  if (gc_mark_cap > GC_MARK_STACK_INIT) {
    size_t target = js->obj_arena.live_count * 2;
    if (target < GC_MARK_STACK_INIT) target = GC_MARK_STACK_INIT;
    if (target < gc_mark_cap / 2) {
      ant_object_t **ns = realloc(gc_mark_stack, target * sizeof(*ns));
      if (ns) { gc_mark_stack = ns; gc_mark_cap = target; }
    }
  }

  ANT_ASSERT(
    js->remembered_upvalue_len == 0,
    "upvalue remembered set mutated during collection"
  );
  js->gc_objects_running = false;
}

void gc_objects_run_minor(ant_t *js, gc_str_mark_fn str_mark) {
  if (!js) return;
  js->gc_objects_running = true;

  g_str_mark = str_mark;
  gc_epoch++;

  if (gc_epoch == 0) gc_epoch = 1;
  gc_obj_epoch = (uint8_t)(gc_obj_epoch + 1u);

  if (gc_obj_epoch == 0 || gc_obj_epoch == ANT_GC_DEAD) {
    gc_obj_epoch = 1;
    gc_obj_epoch_wrapped(js);
  }
  g_minor_gc = true;

  for (size_t i = 0; i < js->remember_set_len; i++)
    gc_scan_obj(js, js->remember_set[i]);

  gc_mark_remembered_func_consts(js);
  gc_mark_remembered_upvalues(js);
  gc_mark_remembered_closures(js);

  for (size_t i = 0; i < js->remember_set_len; i++)
    js->remember_set[i]->flags.in_remember_set = 0;
  js->remember_set_len = 0;

  gc_mark_roots(js);
  gc_weak_process(
    js, true, gc_mark_value, gc_drain_mark_stack_weak,
    gc_weak_key_alive, gc_weak_collection_live
  );
  
  gc_clear_napi_weak_refs(js, true);
  g_minor_gc = false;

  gc_age_regex_cache(js, true);
  gc_sweep_young_and_promote(js);

  gc_sweep_young_closures(js);
  gc_sweep_young_upvalues(js);
  gc_clear_remembered_func_consts(js);
  gc_clear_remembered_closures(js);
  gc_clear_remembered_upvalues(js);

  js->gc_objects_running = false;
}

uint64_t gc_get_epoch(void) { 
  return gc_epoch;
}
