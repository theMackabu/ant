#include "internal.h"
#include "shapes.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/object_arenas.h"
#include "gc/parallel.h"
#include "gc/strings.h"

#include <stdlib.h>
#include <string.h>

#if defined(ANT_WASM_EMBED)
void gc_parallel_set_helpers(int) {}
bool gc_parallel_available(void) { return false; }
bool gc_parallel_drain(ant_t *, uint32_t, gc_parallel_scan_fn) { return false; }

#else
#include <uv.h>

static constexpr uint32_t GC_PAR_MAX_HELPERS = 15;
static constexpr uint32_t GC_PAR_DEFAULT_HELPERS = 3;
static constexpr uint32_t GC_PAR_CHUNK = 256;

static constexpr size_t GC_PAR_DONATE_MIN = 8;
static constexpr size_t GC_PAR_STACK_INIT = 4096;

typedef struct gc_par_chunk {
  struct gc_par_chunk *next;
  uint32_t len;
  ant_object_t *items[GC_PAR_CHUNK];
} gc_par_chunk_t;

typedef struct {
  ant_object_t **stack;
  size_t len, cap;
  ant_object_t **objects;
  size_t objects_len, objects_cap;
  ant_value_t *values;
  size_t values_len, values_cap;
  uint64_t waited_ns;
  uint64_t busy_ns;
} gc_par_worker_t;

typedef struct {
  uv_once_t once;
  uv_mutex_t lock;
  uv_mutex_t busy;
  uv_cond_t wake;
  uv_cond_t more;
  uv_cond_t done;
  uint32_t helpers;
  gc_par_worker_t workers[GC_PAR_MAX_HELPERS + 1];

  ant_t *js;
  uint32_t other_refs;
  uint8_t epoch;
  uint64_t job;
  uint32_t active;
  uint32_t idle;
  uint32_t exited;
  bool finished;
  gc_par_chunk_t *queue;
  uint32_t idle_hint;
} gc_par_pool_t;

static gc_par_pool_t gc_par = {.once = UV_ONCE_INIT};
static int gc_par_requested = -1;

static void gc_par_grow(void **items, size_t *cap, size_t elem) {
  size_t next = *cap ? *cap * 2 : GC_PAR_STACK_INIT;
  void *grown = realloc(*items, next * elem);
  Ant_Assert(grown, "out of memory growing a parallel mark list");
  *items = grown;
  *cap = next;
}

static inline void gc_par_push(gc_par_worker_t *w, ant_object_t *obj) {
  if (__builtin_expect(w->len >= w->cap, 0)) gc_par_grow((void **)&w->stack, &w->cap, sizeof(*w->stack));
  w->stack[w->len++] = obj;
}

static inline void gc_par_defer_object(gc_par_worker_t *w, ant_object_t *obj) {
  if (w->objects_len >= w->objects_cap) gc_par_grow((void **)&w->objects, &w->objects_cap, sizeof(*w->objects));
  w->objects[w->objects_len++] = obj;
}

static inline void gc_par_defer_value(gc_par_worker_t *w, ant_value_t value) {
  if (w->values_len >= w->values_cap) gc_par_grow((void **)&w->values, &w->values_cap, sizeof(*w->values));
  w->values[w->values_len++] = value;
}

static inline __attribute__((always_inline)) void gc_par_value(const ant_t *js, gc_par_worker_t *w, ant_value_t v) {
  if (!is_tagged(v)) return;
  uint32_t bit = 1u << vtype_tagged(v);

  if (__builtin_expect((bit & GC_OBJ_TYPE_MASK) != 0, 1)) {
    if (bit == (1u << kTypeError) && vdata(v) < 2) return;
    ant_object_t *obj = (ant_object_t *)vptr(v);
    if (!ant_object_holds_offset(js, vdata(v), obj)) return;

    uint8_t epoch = __atomic_load_n(&obj->mark_epoch, __ATOMIC_RELAXED);
    if (epoch == gc_par.epoch || epoch == ANT_GC_DEAD) return;
    if (!__atomic_compare_exchange_n(&obj->mark_epoch, &epoch, gc_par.epoch, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    gc_par_push(w, obj);
    return;
  }

  if (bit == (1u << kTypeString) && (vdata(v) & STR_HEAP_TAG_MASK) == STR_HEAP_TAG_FLAT) {
    const ant_flat_string_t *flat = vptr_masked(v, STR_HEAP_TAG_MASK);
    if (flat && !js->rope_gc.minor_marking && (js->rope_gc.untrusted_marking || !str_flat_is_permanent(flat)))
      gc_strings_mark_shared((ant_t *)js, flat);
    return;
  }

  if (bit & gc_par.other_refs) gc_par_defer_value(w, v);
}

static inline __attribute__((always_inline)) bool gc_par_scan_plain(ant_t *js, gc_par_worker_t *w, ant_object_t *obj) {
  uint8_t type = obj->type_tag;
  ant_shape_t *shape = obj->shape;

  if ((type != kTypeObject && type != kTypeArray) || !shape) return false;
  if (obj->extra_slots || obj->native_tag || ant_shape_may_have_gc_refs(shape)) return false;

  ant_gc_shapes_mark_shared(js, shape);
  gc_par_value(js, w, ant_object_proto(obj));
  if (type == kTypeObject) gc_par_value(js, w, obj->u.data.value);

  uint32_t count = ant_shape_count(shape);
  if (count > obj->prop_count) count = obj->prop_count;
  uint32_t inobj = ant_object_inobj_limit(obj);
  uint32_t direct = count < inobj ? count : inobj;

  for (uint32_t i = count; i > inobj; i--) gc_par_value(js, w, obj->overflow_prop[i - 1 - inobj]);
  for (uint32_t i = direct; i > 0; i--) gc_par_value(js, w, obj->inobj[i - 1]);

  if (type == kTypeArray && obj->u.array.data) {
    ant_value_t *data = obj->u.array.data;
    uint32_t n = obj->u.array.len < obj->u.array.cap ? obj->u.array.len : obj->u.array.cap;
    for (uint32_t i = n; i > 0; i--) gc_par_value(js, w, data[i - 1]);
  }

  return true;
}

static void gc_par_donate(gc_par_worker_t *w) {
  gc_par_chunk_t *chunk = malloc(sizeof(*chunk));
  if (!chunk) return;
  uint32_t give = (uint32_t)(w->len / 2);
  if (give > GC_PAR_CHUNK) give = GC_PAR_CHUNK;
  chunk->len = give;
  memcpy(chunk->items, w->stack, give * sizeof(*chunk->items));
  memmove(w->stack, w->stack + give, (w->len - give) * sizeof(*w->stack));
  w->len -= give;

  uv_mutex_lock(&gc_par.lock);
  chunk->next = gc_par.queue;
  gc_par.queue = chunk;
  uv_cond_signal(&gc_par.more);
  uv_mutex_unlock(&gc_par.lock);
}

static bool gc_par_take(gc_par_worker_t *w) {
  uv_mutex_lock(&gc_par.lock);
  for (;;) {
    gc_par_chunk_t *chunk = gc_par.queue;
    if (chunk) {
      gc_par.queue = chunk->next;
      uv_mutex_unlock(&gc_par.lock);
      for (uint32_t i = 0; i < chunk->len; i++) gc_par_push(w, chunk->items[i]);
      free(chunk);
      return true;
    }
    if (gc_par.finished) break;
    gc_par.idle++;
    __atomic_store_n(&gc_par.idle_hint, gc_par.idle, __ATOMIC_RELAXED);
    if (gc_par.idle == gc_par.active) {
      gc_par.finished = true;
      uv_cond_broadcast(&gc_par.more);
      break;
    }
    uint64_t wait_start = gc_now_ns();
    uv_cond_wait(&gc_par.more, &gc_par.lock);
    w->waited_ns += gc_now_ns() - wait_start;
    gc_par.idle--;
    __atomic_store_n(&gc_par.idle_hint, gc_par.idle, __ATOMIC_RELAXED);
  }
  uv_mutex_unlock(&gc_par.lock);
  return false;
}

static void gc_par_work(gc_par_worker_t *w) {
  ant_t *js = gc_par.js;
  uint64_t start = gc_now_ns();
  w->waited_ns = 0;
  do {
    while (w->len) {
      ant_object_t *obj = w->stack[--w->len];
      if (!gc_par_scan_plain(js, w, obj)) gc_par_defer_object(w, obj);
      if (w->len >= GC_PAR_DONATE_MIN && __atomic_load_n(&gc_par.idle_hint, __ATOMIC_RELAXED)) gc_par_donate(w);
    }
  } while (gc_par_take(w));
  uint64_t elapsed = gc_now_ns() - start;
  w->busy_ns = elapsed > w->waited_ns ? elapsed - w->waited_ns : 0;
}

static void gc_par_helper(void *arg) {
  gc_par_worker_t *w = arg;
  uint64_t seen = 0;
  uv_mutex_lock(&gc_par.lock);
  for (;;) {
    while (gc_par.job == seen) uv_cond_wait(&gc_par.wake, &gc_par.lock);
    seen = gc_par.job;
    uv_mutex_unlock(&gc_par.lock);
    gc_par_work(w);
    uv_mutex_lock(&gc_par.lock);
    if (++gc_par.exited == gc_par.helpers) uv_cond_signal(&gc_par.done);
  }
}

static void gc_par_init(void) {
  long cores = (long)uv_available_parallelism() - 1;
  long wanted = gc_par_requested >= 0 ? gc_par_requested : (cores < (long)GC_PAR_DEFAULT_HELPERS ? cores : (long)GC_PAR_DEFAULT_HELPERS);
  if (wanted < 0) wanted = 0;
  if (wanted > (long)GC_PAR_MAX_HELPERS) wanted = GC_PAR_MAX_HELPERS;

  if (uv_mutex_init(&gc_par.lock) || uv_mutex_init(&gc_par.busy)) return;
  if (uv_cond_init(&gc_par.wake) || uv_cond_init(&gc_par.more) || uv_cond_init(&gc_par.done)) return;

  uint32_t started = 0;
  for (uint32_t i = 0; i < (uint32_t)wanted; i++) {
    uv_thread_t thread;
    if (uv_thread_create(&thread, gc_par_helper, &gc_par.workers[i + 1])) break;
    started++;
  }
  gc_par.helpers = started;
}

void gc_parallel_set_helpers(int count) {
  gc_par_requested = count < 0 ? 0 : count;
}

bool gc_parallel_available(void) {
  uv_once(&gc_par.once, gc_par_init);
  return gc_par.helpers > 0;
}

bool gc_parallel_drain(ant_t *js, uint32_t other_refs, gc_parallel_scan_fn scan) {
  if (!gc_parallel_available() || uv_mutex_trylock(&gc_par.busy)) return false;

  gc_par_worker_t *main = &gc_par.workers[0];
  for (size_t i = 0; i < js->gc.mark_sp; i++) gc_par_push(main, js->gc.mark_stack[i]);
  js->gc.mark_sp = 0;

  uv_mutex_lock(&gc_par.lock);
  gc_par.js = js;
  gc_par.other_refs = other_refs;
  gc_par.epoch = js->gc.obj_epoch;
  gc_par.active = gc_par.helpers + 1;
  gc_par.idle = 0;
  gc_par.idle_hint = 0;
  gc_par.exited = 0;
  gc_par.finished = false;
  gc_par.job++;
  uv_cond_broadcast(&gc_par.wake);
  uv_mutex_unlock(&gc_par.lock);

  gc_par_work(main);

  uv_mutex_lock(&gc_par.lock);
  while (gc_par.exited < gc_par.helpers) uv_cond_wait(&gc_par.done, &gc_par.lock);
  uv_mutex_unlock(&gc_par.lock);

  for (uint32_t i = 1; i <= gc_par.helpers; i++) js->gc.parallel_cpu_ns += gc_par.workers[i].busy_ns;
  for (uint32_t i = 0; i <= gc_par.helpers; i++) {
    gc_par_worker_t *w = &gc_par.workers[i];
    for (size_t j = 0; j < w->objects_len; j++) scan(js, w->objects[j]);
    for (size_t j = 0; j < w->values_len; j++) gc_mark_value(js, w->values[j]);
    w->objects_len = 0;
    w->values_len = 0;
  }

  uv_mutex_unlock(&gc_par.busy);
  return true;
}

#endif
