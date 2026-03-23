#include <gc.h>
#include <stdbool.h>
#include <time.h>
#include "shapes.h"

#include "gc/objects.h"
#include "gc/refs.h"
#include "gc/strings.h"
#include "gc/ropes.h"

bool gc_disabled = false;

static size_t   gc_tick = 0;
static uint64_t gc_last_run_ms = 0;

static size_t   gc_nursery_threshold = GC_NURSERY_THRESHOLD;
static uint32_t gc_major_every_n     = GC_MAJOR_EVERY_N_MINOR;

static uint32_t gc_minor_surv_ewma = 128;
static uint32_t gc_major_recl_ewma =  26;

static uint64_t gc_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void gc_adapt_nursery(size_t young_before, size_t survivors) {
  if (young_before == 0) return;
  uint32_t rate = (uint32_t)((survivors * 256) / young_before);
  gc_minor_surv_ewma = (gc_minor_surv_ewma * 3 + rate) >> 2;
  if (gc_minor_surv_ewma < 64 && gc_nursery_threshold > GC_NURSERY_THRESHOLD / 2)
    gc_nursery_threshold -= gc_nursery_threshold / 4;
  else if (gc_nursery_threshold < GC_NURSERY_THRESHOLD)
    gc_nursery_threshold = GC_NURSERY_THRESHOLD;
}

static void gc_adapt_major_interval(size_t live_before, size_t live_after) {
  if (live_before == 0) return;
  size_t freed = live_before > live_after ? live_before - live_after : 0;
  uint32_t rate = (uint32_t)((freed * 256) / live_before);
  gc_major_recl_ewma = (gc_major_recl_ewma * 3 + rate) >> 2;

  bool gen_ineffective = gc_minor_surv_ewma  > 192; // >75% nursery survival
  bool high_reclaim    = gc_major_recl_ewma  >  51; // >20% old-gen freed
  bool low_reclaim     = gc_major_recl_ewma  <  13; // < 5% old-gen freed

  if ((gen_ineffective && !low_reclaim) || high_reclaim) {
    if (gc_major_every_n > 2) gc_major_every_n--;
  } else if (!gen_ineffective && low_reclaim) {
    if (gc_major_every_n < GC_MAJOR_EVERY_N_MINOR * 4) gc_major_every_n++;
  }
}

static void gc_mark_str(ant_t *js, ant_value_t v) {
  if (v <= NANBOX_PREFIX) return;
  uint8_t t = (v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
  
  if (t != T_STR) return;
  uintptr_t data = (uintptr_t)(v & NANBOX_DATA_MASK);
  
  if (data & 1ULL) {
    ant_rope_heap_t *rope = (ant_rope_heap_t *)(data & ~1ULL);
    
    if (!rope) return;
    if (!gc_ropes_mark(rope)) return;
    
    gc_mark_str(js, rope->left);
    gc_mark_str(js, rope->right);
    gc_mark_str(js, rope->cached);
    
  } else if (data) gc_strings_mark(js, (const void *)data);
}

void gc_run(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  size_t live_before = js->obj_arena.live_count;

  js->prop_refs_len = 0;
  gc_refs_shrink(js);

  gc_strings_begin(js);
  gc_ropes_begin(js);
  gc_objects_run(js, gc_mark_str);
  ant_ic_epoch_bump();

  gc_strings_sweep(js);
  gc_ropes_sweep(js);

  js->gc_last_live = js->obj_arena.live_count;
  js->old_live_count = js->obj_arena.live_count; /* after major GC all live = old */
  js->minor_gc_count = 0;

  ant_pool_stats_t pool_stats = js_class_pool_stats(&js->pool.string);
  js->gc_pool_last_live = pool_stats.used;
  js->gc_pool_alloc = 0;

  gc_adapt_major_interval(live_before, js->obj_arena.live_count);
  gc_last_run_ms = gc_now_ms();
}

void gc_run_minor(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;

  size_t old_before   = js->old_live_count;
  size_t live_before  = js->obj_arena.live_count;
  size_t young_before = live_before > old_before ? live_before - old_before : 0;

  gc_objects_run_minor(js, NULL);
  ant_ic_epoch_bump();

  js->gc_last_live = js->obj_arena.live_count;
  js->old_live_count = js->obj_arena.live_count;
  js->minor_gc_count++;

  size_t survivors = js->obj_arena.live_count > old_before
    ? js->obj_arena.live_count - old_before : 0;
    
  gc_adapt_nursery(young_before, survivors);
  gc_last_run_ms = gc_now_ms();
}

void gc_maybe(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
#ifdef ANT_JIT
  if (__builtin_expect(js->jit_active_depth > 0, 0)) return;
#endif
  if (++gc_tick < GC_MIN_TICK) return;
  size_t live = js->obj_arena.live_count;

  size_t young_count = live > js->old_live_count ? live - js->old_live_count : 0;
  if (young_count >= gc_nursery_threshold) {
    gc_tick = 0;
    gc_run_minor(js);
    if (js->minor_gc_count >= gc_major_every_n) {
      js->minor_gc_count = 0; gc_run(js);
    }
    return;
  }

  size_t threshold = GC_HEAP_GROWTH(js->gc_last_live);
  if (threshold < 2048) threshold = 2048;
  
  if (live >= threshold) {
    gc_tick = 0; gc_run(js);
    return;
  }

  if (gc_tick < 8192) return;

  if (young_count == 0 && js->gc_pool_alloc == 0) {
    gc_tick = 0;
    return;
  }

  if (gc_now_ms() - gc_last_run_ms < GC_FORCE_INTERVAL_MS) {
    gc_tick = 0;
    return;
  }

  gc_tick = 0;
  gc_run(js);
}
