#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "gc/modules.h"
#include "sugar.h"
#include "silver/engine.h"
#include "shapes.h"
#include "runtime.h"
#include "modules/collections.h"
#include "modules/regex.h"

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/time.h>
#include <utarray.h>

#define MCO_API extern
#include <minicoro.h>

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
static uint32_t g_gc_func_mark_profile_depth = 0;
static uint64_t g_gc_func_mark_profile_start_ns = 0;

static gc_str_mark_fn g_str_mark = NULL;
static ant_object_t *g_pending_promises = NULL;

static uint64_t gc_epoch = 0;
static uint8_t gc_obj_epoch = 0;
static bool g_minor_gc = false;

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

void gc_root_pending_promise(ant_object_t *obj) {
  if (!obj || obj->gc_pending_rooted) return;
  obj->gc_pending_rooted = true;
  obj->gc_pending_next = g_pending_promises;
  g_pending_promises = obj;
}

void gc_unroot_pending_promise(ant_object_t *obj) {
  if (!obj || !obj->gc_pending_rooted) return;
  obj->gc_pending_rooted = false;
  ant_object_t **pp = &g_pending_promises;
  while (*pp) {
    if (*pp == obj) { *pp = obj->gc_pending_next; break; }
    pp = &(*pp)->gc_pending_next;
  }
  obj->gc_pending_next = NULL;
}

void gc_remember_add(ant_t *js, ant_object_t *obj) {
  if (!obj || obj->in_remember_set) return;
  if (js->remember_set_len >= js->remember_set_cap) {
    size_t new_cap = js->remember_set_cap ? js->remember_set_cap * 2 : 64;
    ant_object_t **ns = realloc(js->remember_set, new_cap * sizeof(*ns));
    if (!ns) return;
    js->remember_set = ns;
    js->remember_set_cap = new_cap;
  }
  obj->in_remember_set = 1;
  js->remember_set[js->remember_set_len++] = obj;
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
  if (g_minor_gc && obj->generation == 1) return;
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
    
  for (int i = 0; i < func->obj_site_count; i++) {
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

static void gc_mark_closure(ant_t *js, sv_closure_t *c) {
  if (!c) return;
  if (c->gc_epoch == gc_epoch) return;
  
  c->gc_epoch = gc_epoch;
  gc_mark_func(js, c->func);
  gc_mark_value(js, c->func_obj);
  gc_mark_value(js, c->bound_this);
  gc_mark_value(js, c->bound_args);
  gc_mark_value(js, c->super_val);

  if (c->upvalues && c->func) {
  for (int i = 0; i < c->func->upvalue_count; i++) {
    sv_upvalue_t *uv = c->upvalues[i];
    if (!uv) continue;
    uv->gc_epoch = gc_epoch;
    if (uv->location == &uv->closed) gc_mark_value(js, uv->closed);
  }}
  
  if (c->bound_argv) {
    for (int i = 0; i < c->bound_argc; i++) gc_mark_value(js, c->bound_argv[i]);
  }
}

void gc_mark_value(ant_t *js, ant_value_t v) {
  if (v <= NANBOX_PREFIX) return;
  uint8_t t = (v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;

  if (t == T_FUNC) {
    gc_mark_closure(js, (sv_closure_t *)(uintptr_t)(v & NANBOX_DATA_MASK));
    return;
  }

  if (t == T_STR && g_str_mark) {
    g_str_mark(js, v);
    return;
  }

  if (!((1u << t) & GC_OBJ_TYPE_MASK)) return;
  ant_object_t *obj = (ant_object_t *)(uintptr_t)(v & NANBOX_DATA_MASK);
  
  if (!obj) return;
  gc_grey_obj(js, obj);
}

static void gc_scan_obj(ant_t *js, ant_object_t *obj) {
  ant_gc_shapes_mark(obj->shape);
  gc_mark_value(js, obj->proto);

  if (obj->type_tag != T_ARR) gc_mark_value(js, obj->u.data.value);

  if (obj->type_tag == T_MAP) {
    map_entry_t **head = (map_entry_t **)(uintptr_t)js_getnum(obj->u.data.value);
    if (head) {
    map_entry_t *e, *tmp;
    HASH_ITER(hh, *head, e, tmp) { 
      gc_mark_value(js, e->key_val); 
      gc_mark_value(js, e->value); 
    }}
  } else if (obj->type_tag == T_WEAKMAP) {
    weakmap_entry_t **head = (weakmap_entry_t **)(uintptr_t)js_getnum(obj->u.data.value);
    if (head) {
    weakmap_entry_t *e, *tmp;
    HASH_ITER(hh, *head, e, tmp) {
      gc_mark_value(js, e->key_obj);
      gc_mark_value(js, e->value);
    }}
  } else if (obj->type_tag == T_SET) {
    set_entry_t **head = (set_entry_t **)(uintptr_t)js_getnum(obj->u.data.value);
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
    if (prop && prop->has_getter) gc_mark_value(js, prop->getter);
    if (prop && prop->has_setter) gc_mark_value(js, prop->setter);
  }}

  if (obj->extra_slots) {
    ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;
    for (uint8_t i = 0; i < obj->extra_count; i++) gc_mark_value(js, entries[i].value);
  }

  if (obj->type_tag == T_ARR && obj->u.array.data) {
    uint32_t n = obj->u.array.len < obj->u.array.cap ? obj->u.array.len : obj->u.array.cap;
    for (uint32_t i = 0; i < n; i++) gc_mark_value(js, obj->u.array.data[i]);
  }

  ant_promise_state_t *pd = obj->promise_state;
  if (pd) {
    gc_mark_value(js, pd->value);
    gc_mark_value(js, pd->trigger_parent);
    gc_mark_promise_handlers(js, pd);
  }

  if (obj->proxy_state) {
    gc_mark_value(js, obj->proxy_state->target);
    gc_mark_value(js, obj->proxy_state->handler);
  }

  gc_mark_abort_signal_object(
    js, js_obj_from_ptr(obj), gc_mark_value
  );
}

static void gc_drain_mark_stack(ant_t *js) {
while (gc_mark_sp > 0) {
  ant_object_t *obj = gc_mark_stack[--gc_mark_sp];
  gc_scan_obj(js, obj);
}}

static void gc_scan_vm_stack(ant_t *js, sv_vm_t *vm) {
  if (!vm) return;

  for (int i = 0; i < vm->sp; i++)
    gc_mark_value(js, vm->stack[i]);

  for (int f = 0; f <= vm->fp; f++) {
    sv_frame_t *frame = &vm->frames[f];
    gc_mark_func(js, frame->func);
    gc_mark_value(js, frame->callee);
    gc_mark_value(js, frame->this);
    gc_mark_value(js, frame->new_target);
    gc_mark_value(js, frame->super_val);
    gc_mark_value(js, frame->with_obj);
    gc_mark_value(js, frame->completion.value);
  }

  for (sv_upvalue_t *uv = vm->open_upvalues; uv; uv = uv->next) {
    uv->gc_epoch = gc_epoch;
    if (uv->location == &uv->closed) gc_mark_value(js, uv->closed);
  }

  for (int f = 0; f <= vm->fp; f++) {
  sv_frame_t *frame = &vm->frames[f];
  if (!frame->upvalues) continue;
  for (int j = 0; j < frame->upvalue_count; j++) {
    sv_upvalue_t *uv = frame->upvalues[j];
    if (!uv) continue;
    uv->gc_epoch = gc_epoch;
    if (uv->location == &uv->closed) gc_mark_value(js, uv->closed);
  }}
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
      
    if (w <= NANBOX_PREFIX) continue;
    uint8_t type = (w >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
    
    if ((1u << type) & GC_OBJ_TYPE_MASK) {
      ant_object_t *obj = (ant_object_t *)(uintptr_t)(w & NANBOX_DATA_MASK);
      if (obj) gc_grey_obj(js, obj);
    }
    
    if (type == T_FUNC) {
      sv_closure_t *c = (sv_closure_t *)(uintptr_t)(w & NANBOX_DATA_MASK);
      if (c && fixed_arena_contains(&js->closure_arena, c)) gc_mark_closure(js, c);
    }
    
    if (type == T_STR && g_str_mark) g_str_mark(js, w);
  }
}

__attribute__((noinline))
static void gc_scan_current_stack(ant_t *js) {
  jmp_buf jb;
  if (setjmp(jb) != 0) return;
  volatile uint8_t sp_marker = 0;

  mco_coro *running = mco_running();
  uintptr_t base;
  
  if (running && running->stack_base && running->stack_size > 0) {
    base = (uintptr_t)running->stack_base + running->stack_size;
  } else base = (uintptr_t)js->cstk.base;

  uintptr_t lo, hi;
  if (!gc_get_stack_bounds(base, (uintptr_t)&sp_marker, &lo, &hi)) return;
  gc_scan_range(js, lo, hi);
}

static void gc_scan_mco_stack(ant_t *js, mco_coro *mco, mco_coro *skip) {
  if (!mco || mco == skip) return;
  if (!mco->stack_base || mco->stack_size == 0) return;
  uintptr_t lo = (uintptr_t)mco->stack_base;
  uintptr_t hi = lo + mco->stack_size;
  gc_scan_range(js, lo, hi);
}

static void gc_scan_other_stacks(ant_t *js) {
  mco_coro *running = mco_running();

  if (running) {
    for (mco_coro *a = running->prev_co; a; a = a->prev_co)
      gc_scan_mco_stack(js, a, running);
  }

  for (coroutine_t *c = pending_coroutines.head; c; c = c->next)
    gc_scan_mco_stack(js, c->mco, running);

  if (js->cstk.main_base && js->cstk.main_lo) {
    uintptr_t lo, hi;
    if (gc_get_stack_bounds(
      (uintptr_t)js->cstk.main_base,
      (uintptr_t)js->cstk.main_lo, &lo, &hi))
      gc_scan_range(js, lo, hi);
  }
}

static void gc_mark_coroutine(ant_t *js, coroutine_t *c) {
  if (!c) return;
  gc_scan_vm_stack(js, c->sv_vm);
  gc_mark_value(js, c->this_val);
  gc_mark_value(js, c->async_func);
  gc_mark_value(js, c->async_promise);
  gc_mark_value(js, c->awaited_promise);
  gc_mark_value(js, c->result);
  gc_mark_value(js, c->yield_value);
  gc_mark_value(js, c->super_val);
  gc_mark_value(js, c->new_target);
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

  for (coroutine_t *c = pending_coroutines.head; c; c = c->next) gc_mark_coroutine(js, c);
  for (coroutine_t *c = js->active_async_coro; c; c = c->active_parent) gc_mark_coroutine(js, c);

  gc_mark_value(js, js->global);
  gc_mark_value(js, js->object);
  gc_mark_value(js, js->array_proto);
  gc_mark_value(js, js->this_val);
  gc_mark_value(js, js->new_target);
  gc_mark_value(js, js->current_func);
  gc_mark_value(js, js->thrown_value);
  gc_mark_value(js, js->thrown_stack);
  gc_mark_value(js, js->length_str);

  if (rt && rt->js == js)
    gc_mark_value(js, rt->ant_obj);

  for (ant_module_t *ctx = js->module; ctx; ctx = ctx->prev) {
    gc_mark_value(js, ctx->module_ns);
    gc_mark_value(js, ctx->module_ctx);
    gc_mark_value(js, ctx->prev_import_meta_prop);
  }

  for (size_t i = 0; i < js->pending_rejections.len; i++)
    gc_mark_value(js, js->pending_rejections.items[i]);

  for (uint8_t i = 0; i < js->cfunc_promote_cache.len; i++)
    gc_mark_value(js, js->cfunc_promote_cache.promoted[i]);

  gc_visit_roots(js, gc_mark_value);
  gc_mark_timers(js, gc_mark_value);
  gc_mark_ffi(js, gc_mark_value);
  gc_mark_fetch(js, gc_mark_value);
  gc_mark_fs(js, gc_mark_value);
  gc_mark_child_process(js, gc_mark_value);
  gc_mark_readline(js, gc_mark_value);
  gc_mark_process(js, gc_mark_value);
  gc_mark_navigator(js, gc_mark_value);
  gc_mark_net(js, gc_mark_value);
  gc_mark_server(js, gc_mark_value);
  gc_mark_events(js, gc_mark_value);
  gc_mark_lmdb(js, gc_mark_value);
  gc_mark_symbols(js, gc_mark_value);
  gc_mark_esm(js, gc_mark_value);
  gc_mark_worker_threads(js, gc_mark_value);
  gc_mark_abort(js, gc_mark_value);
  gc_mark_domexception(js, gc_mark_value);
  gc_mark_queuing_strategies(js, gc_mark_value);
  gc_mark_readable_streams(js, gc_mark_value);
  gc_mark_writable_streams(js, gc_mark_value);
  gc_mark_transform_streams(js, gc_mark_value);
  gc_mark_codec_streams(js, gc_mark_value);
  gc_mark_compression_streams(js, gc_mark_value);
  gc_mark_zlib(js, gc_mark_value);
  gc_mark_wasm(js, gc_mark_value);
  gc_mark_napi(js, gc_mark_value);

  for (
    ant_object_t *obj = g_pending_promises; 
    obj; obj = obj->gc_pending_next
  ) gc_grey_obj(js, obj);

  gc_scan_current_stack(js);
  gc_scan_other_stacks(js);

  if (!g_minor_gc) {
    for (ant_object_t *obj = js->permanent_objects; obj; obj = obj->next) 
      gc_scan_obj(js, obj);
  }

  gc_drain_mark_stack(js);
}

void gc_object_free(ant_t *js, ant_object_t *obj) {
  if (!obj) return;
  if (obj->finalizer) obj->finalizer(js, obj);
  obj->mark_epoch = ANT_GC_DEAD;

  if (obj->shape) {
    ant_shape_release(obj->shape);
    obj->shape = NULL;
  }

  if (obj->extra_slots) {
    free(obj->extra_slots);
    obj->extra_slots = NULL;
  }

  if (obj->gc_pending_rooted)
    gc_unroot_pending_promise(obj);

  if (obj->promise_state) {
    if (obj->promise_state->handlers)
      utarray_free(obj->promise_state->handlers);
    free(obj->promise_state);
    obj->promise_state = NULL;
  }

  if (obj->proxy_state) {
    free(obj->proxy_state);
    obj->proxy_state = NULL;
  }

  if (obj->type_tag == T_ARR && obj->u.array.data) {
    free(obj->u.array.data);
    obj->u.array.data = NULL;
  }

  switch (obj->type_tag) {
    case T_MAP: {
      map_entry_t **head = (map_entry_t **)(uintptr_t)js_getnum(obj->u.data.value);
      if (head) {
        map_entry_t *e, *tmp;
        HASH_ITER(hh, *head, e, tmp) { HASH_DEL(*head, e); free(e->key); free(e); }
        free(head);
      }
      break;
    }
    case T_SET: {
      set_entry_t **head = (set_entry_t **)(uintptr_t)js_getnum(obj->u.data.value);
      if (head) {
        set_entry_t *e, *tmp;
        HASH_ITER(hh, *head, e, tmp) { HASH_DEL(*head, e); free(e->key); free(e); }
        free(head);
      }
      break;
    }
    case T_WEAKMAP: {
      weakmap_entry_t **head = (weakmap_entry_t **)(uintptr_t)js_getnum(obj->u.data.value);
      if (head) {
        weakmap_entry_t *e, *tmp;
        HASH_ITER(hh, *head, e, tmp) { HASH_DEL(*head, e); free(e); }
        free(head);
      }
      break;
    }
    case T_WEAKSET: {
      weakset_entry_t **head = (weakset_entry_t **)(uintptr_t)js_getnum(obj->u.data.value);
      if (head) {
        weakset_entry_t *e, *tmp;
        HASH_ITER(hh, *head, e, tmp) { HASH_DEL(*head, e); free(e); }
        free(head);
      }
      break;
    }
    default: break;
  }

  free(obj->overflow_prop);
  obj->overflow_prop = NULL;
  free((void *)obj->exotic_ops);
  obj->exotic_ops = NULL;
  fixed_arena_free_elem(&js->obj_arena, obj);
}

static void gc_sweep_young(ant_t *js) {
  ant_object_t **pp = &js->objects;
  while (*pp) {
  ant_object_t *obj = *pp;
  if (obj->mark_epoch == gc_obj_epoch) pp = &obj->next; else {
    *pp = obj->next;
    gc_object_free(js, obj);
  }}
}

static void gc_promote_survivors(ant_t *js) {
  ant_object_t *obj = js->objects;
  while (obj) {
    ant_object_t *next = obj->next;
    obj->generation = 1;
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

  ant_object_t *tail = NULL;
  for (ant_object_t *obj = js->objects; obj; obj = obj->next) {
    obj->gc_permanent = 1;
    obj->generation = 1;
    tail = obj;
  }
  
  if (tail) {
    tail->next = js->permanent_objects;
    js->permanent_objects = js->objects;
    js->objects = NULL;
  }
  
  tail = NULL;
  for (ant_object_t *obj = js->objects_old; obj; obj = obj->next) {
    obj->gc_permanent = 1;
    tail = obj;
  }
  
  if (tail) {
    tail->next = js->permanent_objects;
    js->permanent_objects = js->objects_old;
    js->objects_old = NULL;
  }
}

void gc_objects_run(ant_t *js, gc_str_mark_fn str_mark) {
  if (!js) return;

  g_str_mark = str_mark;
  if (g_gc_func_mark_profile.enabled) g_gc_func_mark_profile.collections++;
  
  gc_epoch++;
  if (gc_epoch == 0) gc_epoch = 1;

  gc_obj_epoch = (uint8_t)(gc_obj_epoch + 1u);
  if (gc_obj_epoch == 0 || gc_obj_epoch == ANT_GC_DEAD) gc_obj_epoch = 1;

  ant_gc_shapes_begin();
  for (size_t i = 0; i < js->remember_set_len; i++)
    js->remember_set[i]->in_remember_set = 0;
  js->remember_set_len = 0;

  if (js->remember_set_cap > 512) {
    ant_object_t **ns = realloc(js->remember_set, 256 * sizeof(*ns));
    if (ns) { js->remember_set = ns; js->remember_set_cap = 256; }
  }

  gc_mark_roots(js);
  gc_sweep_regex_cache();
  gc_sweep(js);
  
  if (ant_gc_shapes_sweep()) ant_ic_epoch_bump();
  gc_promote_survivors(js);

  ant_fixed_arena_t *ca = &js->closure_arena;
  ca->free_list = NULL;
  ca->live_count = 0;
  
  for (size_t off = 0; off < ca->watermark; off += ca->elem_size) {
  sv_closure_t *c = (sv_closure_t *)(ca->base + off);
  
  if (c->gc_epoch == gc_epoch) ca->live_count++;
  else {
    if (!(c->call_flags & SV_CALL_BORROWED_UPVALS)) {
      free(c->upvalues);
      c->upvalues = NULL;
    }
    
    free(c->bound_argv);
    c->bound_argv = NULL;
    
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
}

void gc_objects_run_minor(ant_t *js, gc_str_mark_fn str_mark) {
  if (!js) return;
  
  g_str_mark = str_mark;
  gc_epoch++;
  
  if (gc_epoch == 0) gc_epoch = 1;
  gc_obj_epoch = (uint8_t)(gc_obj_epoch + 1u);
  
  if (gc_obj_epoch == 0 || gc_obj_epoch == ANT_GC_DEAD) gc_obj_epoch = 1;
  g_minor_gc = true;

  for (size_t i = 0; i < js->remember_set_len; i++) gc_scan_obj(js, js->remember_set[i]);
  for (size_t i = 0; i < js->remember_set_len; i++) js->remember_set[i]->in_remember_set = 0;
  
  js->remember_set_len = 0;
  gc_mark_roots(js);
  g_minor_gc = false;

  gc_sweep_regex_cache();
  gc_sweep_young(js);
  gc_promote_survivors(js);
  
  // will NOT sweep closure/upvalue arenas here. old closures stored as T_FUNC
  // property values on old objects are not scanned during minor GC (old objects
  // are pre-marked but not traversed unless in the remember set), so their
  // gc_epoch would not be updated and they would be incorrectly freed.
  // closure/upvalue arenas are only swept on major GC `gc_objects_run`
}

uint64_t gc_get_epoch(void) { 
  return gc_epoch;
}
