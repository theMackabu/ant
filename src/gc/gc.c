#include <gc.h>
#include <stdbool.h>
#include <stdint.h>
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

static uint32_t gc_major_live_growth_x256 = 384;
static uint32_t gc_major_pool_growth_x256 = 384;

static uint32_t gc_minor_surv_ewma = 128;
static uint32_t gc_major_recl_ewma =  26;
static bool gc_use_nursery_major_floor = true;

static uint64_t gc_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static size_t gc_scaled_threshold(size_t base_live, uint32_t growth_x256, size_t floor) {
  size_t scaled = (base_live * (size_t)growth_x256) / 256u;
  if (scaled < floor) scaled = floor;
  return scaled;
}

static size_t gc_pool_live_bytes(ant_t *js) {
  ant_pool_stats_t rope_stats = js_pool_stats(&js->pool.rope);
  ant_pool_stats_t symbol_stats = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t bigint_stats = js_class_pool_stats(&js->pool.bigint);
  ant_string_pool_stats_t string_stats = js_string_pool_stats(&js->pool.string);

  return rope_stats.used
    + symbol_stats.used
    + bigint_stats.used
    + string_stats.total.used;
}

size_t gc_live_major_threshold(ant_t *js) {
  size_t threshold = gc_scaled_threshold(
    js->gc_last_live, 
    gc_major_live_growth_x256, GC_MAJOR_SCALE
  );

  bool nursery_churn = gc_minor_surv_ewma <= 64;   // <= 25% young survival
  bool nursery_sticky = gc_minor_surv_ewma >= 160; // >= 62.5% young survival
  bool major_pays = gc_major_recl_ewma >= 51;      // >= 20% old-gen reclaim
  bool major_wasteful = gc_major_recl_ewma <= 13;  // <= 5% old-gen reclaim

  if (gc_use_nursery_major_floor) {
    if (nursery_sticky || (major_pays && !nursery_churn)) gc_use_nursery_major_floor = false;
  } else if (nursery_churn || major_wasteful) gc_use_nursery_major_floor = true;

  if (!gc_use_nursery_major_floor) return threshold;
  size_t nursery_floor = js->old_live_count + gc_nursery_threshold;
  
  return threshold < nursery_floor ? nursery_floor : threshold;
}

size_t gc_pool_major_threshold(ant_t *js) {
  return gc_scaled_threshold(js->gc_pool_last_live, gc_major_pool_growth_x256, GC_POOL_PRESSURE_FLOOR);
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

  if (gen_ineffective && low_reclaim) {
    if (gc_major_live_growth_x256 < 1024) gc_major_live_growth_x256 += 96;
    if (gc_major_pool_growth_x256 < 1536) gc_major_pool_growth_x256 += 128;
  } else if (low_reclaim) {
    if (gc_major_live_growth_x256 < 896) gc_major_live_growth_x256 += 48;
    if (gc_major_pool_growth_x256 < 1280) gc_major_pool_growth_x256 += 64;
  } else if (high_reclaim) {
    if (gc_major_live_growth_x256 > 320) gc_major_live_growth_x256 -= 32;
    if (gc_major_pool_growth_x256 > 320) gc_major_pool_growth_x256 -= 32;
  } else {
    if (gc_major_live_growth_x256 > 384) gc_major_live_growth_x256 -= 16;
    else if (gc_major_live_growth_x256 < 384) gc_major_live_growth_x256 += 16;
    if (gc_major_pool_growth_x256 > 384) gc_major_pool_growth_x256 -= 16;
    else if (gc_major_pool_growth_x256 < 384) gc_major_pool_growth_x256 += 16;
  }
}

static void gc_mark_str(ant_t *js, ant_value_t v) {
  static const void *dispatch[] = {
    [STR_HEAP_TAG_FLAT] = &&l_flat,
    [STR_HEAP_TAG_ROPE] = &&l_rope,
    [STR_HEAP_TAG_BUILDER] = &&l_builder,
  };

  if (v <= NANBOX_PREFIX) return;
  uint8_t t = (v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
  if (t != T_STR) return;

  uintptr_t data = (uintptr_t)(v & NANBOX_DATA_MASK);
  uintptr_t tag = data & STR_HEAP_TAG_MASK;

  if (tag < sizeof(dispatch) / sizeof(*dispatch) && dispatch[tag])
    goto *dispatch[tag];
  goto l_flat;

  l_rope: {
    ant_rope_heap_t *rope = (ant_rope_heap_t *)(data & ~STR_HEAP_TAG_MASK);
    if (!rope || !gc_ropes_mark(rope)) return;
    gc_mark_str(js, rope->left);
    gc_mark_str(js, rope->right);
    gc_mark_str(js, rope->cached);
    return;
  }

  l_builder: {
    ant_string_builder_t *builder = (ant_string_builder_t *)(data & ~STR_HEAP_TAG_MASK);
    if (!builder || !gc_ropes_mark(builder)) return;
    gc_mark_value(js, builder->cached);
    for (ant_builder_chunk_t *chunk = builder->head; chunk; chunk = chunk->next) {
      if (gc_ropes_mark(chunk)) gc_mark_value(js, chunk->value);
    }
    return;
  }

  l_flat:
    if (data) gc_strings_mark(js, (const void *)data);
    return;
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
  js->old_live_count = js->obj_arena.live_count;
  js->minor_gc_count = 0;

  js->gc_pool_last_live = gc_pool_live_bytes(js);
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
  if (++gc_tick < GC_MIN_TICK) return;
  
  size_t live = js->obj_arena.live_count;
  size_t young_count = live > js->old_live_count ? live - js->old_live_count : 0;
  
  if (young_count >= gc_nursery_threshold) {
    gc_tick = 0;
    size_t live_before_minor = js->obj_arena.live_count;
    size_t major_threshold = gc_live_major_threshold(js);
    size_t pool_threshold = gc_pool_major_threshold(js);

    gc_run_minor(js);
    
    if (js->minor_gc_count >= gc_major_every_n) {
      bool major_due = 
        live_before_minor >= major_threshold ||
        js->gc_pool_alloc >= pool_threshold;
      
      if (major_due) {
        js->minor_gc_count = 0;
        gc_run(js);
      }
    }
    
    return;
  }

  size_t threshold = gc_live_major_threshold(js);
  
  if (live >= threshold) {
    gc_tick = 0;
    gc_run(js);
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
