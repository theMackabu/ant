#include <gc.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "shapes.h"

#include "gc/objects.h"
#include "gc/bigints.h"
#include "gc/strings.h"
#include "gc/ropes.h"

bool gc_disabled = false;

static size_t   gc_tick = 0;
static uint64_t gc_last_run_ms = 0;
static uint64_t gc_last_major_ms = 0;

static size_t   gc_nursery_threshold = GC_NURSERY_THRESHOLD;
static uint32_t gc_major_every_n     = GC_MAJOR_EVERY_N_MINOR;

static uint32_t gc_major_live_growth_x256 = 384;
static uint32_t gc_major_pool_growth_x256 = 384;

static uint32_t gc_minor_surv_ewma = 128;
static uint32_t gc_major_recl_ewma =  26;

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
  ant_pool_stats_t rope_young_stats = js_pool_stats(&js->rope_gc.young);
  ant_pool_stats_t rope_old_stats = js_pool_stats(&js->rope_gc.old);
  ant_pool_stats_t symbol_stats = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t bigint_stats = js_class_pool_stats(&js->pool.bigint);
  ant_string_pool_stats_t string_stats = js_string_pool_stats(&js->pool.string);

  return rope_stats.used
    + rope_young_stats.used
    + rope_old_stats.used
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

  if (js->gc_use_nursery_major_floor) {
    if (nursery_sticky || (major_pays && !nursery_churn)) js->gc_use_nursery_major_floor = false;
  } else if (nursery_churn || major_wasteful) js->gc_use_nursery_major_floor = true;

  if (!js->gc_use_nursery_major_floor) return threshold;
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

static void gc_mark_str(ant_t *js, ant_value_t root) {
  static const void *dispatch[] = {
    [STR_HEAP_TAG_FLAT] = &&l_flat,
    [STR_HEAP_TAG_ROPE] = &&l_rope,
    [STR_HEAP_TAG_BUILDER] = &&l_builder,
  };

  ant_value_t local[32];
  ant_value_t *stack = local;
  
  size_t sp = 0, cap = 32;
  ant_value_t v = root;

  l_next:
  if (!is_tagged(v)) goto l_pop;
  uint8_t t = vtype_tagged(v);
  if (t != T_STR) goto l_pop;

  uintptr_t tag = (uintptr_t)(vdata(v) & STR_HEAP_TAG_MASK);
  uintptr_t data = (uintptr_t)vptr_masked(v, STR_HEAP_TAG_MASK);

  if (tag < sizeof(dispatch) / sizeof(*dispatch) && dispatch[tag])
    goto *dispatch[tag];
  goto l_flat;

  l_rope: {
    ant_rope_heap_t *rope = (ant_rope_heap_t *)data;
    if (!gc_ropes_contains(js, rope, sizeof(*rope), _Alignof(ant_rope_heap_t))) goto l_pop;
    if (!gc_ropes_mark(js, rope)) goto l_pop;

    if (vtype(rope->cached) == T_STR) {
      v = rope->cached;
      goto l_next;
    }

    if (!ant_value_stack_push_with_spill(
      &stack, &sp, &cap, local, rope->left
    )) gc_mark_str(js, rope->left);
    
    v = rope->right;
    goto l_next;
  }

  l_builder: {
    ant_string_builder_t *builder = (ant_string_builder_t *)data;
    if (!gc_ropes_contains(js, builder, sizeof(*builder), _Alignof(ant_string_builder_t))) goto l_pop;
    if (!gc_ropes_mark(js, builder)) goto l_pop;
    
    gc_mark_str(js, builder->snapshot);
    gc_mark_value(js, builder->cached);
    
    for (ant_builder_chunk_t *chunk = builder->head; chunk; chunk = chunk->next) {
      if (!gc_ropes_contains(js, chunk, sizeof(*chunk), _Alignof(ant_builder_chunk_t))) break;
      if (gc_ropes_mark(js, chunk)) gc_mark_value(js, chunk->value);
    }
    
    goto l_pop;
  }

  l_flat:
    if (data && !js->rope_gc.minor_marking)
      gc_strings_mark(js, (const void *)data);
  l_pop:
    if (sp > 0) {
      v = stack[--sp];
      goto l_next;
    }
    if (stack != local) free(stack);
    return;
}

static void gc_mark_flat_str(ant_t *js, ant_value_t value) {
  if (!is_tagged(value) || vtype(value) != T_STR) return;
  if ((vdata(value) & STR_HEAP_TAG_MASK) != STR_HEAP_TAG_FLAT) return;
  const void *ptr = vptr_masked(value, STR_HEAP_TAG_MASK);
  if (ptr) gc_strings_mark(js, ptr);
}

void gc_remember_builder(ant_t *js, ant_string_builder_t *builder) {
  if (!js || !builder || builder->in_remember_set) return;
  if (js->rope_gc.remembered_builder_len >= js->rope_gc.remembered_builder_cap) {
    size_t cap = js->rope_gc.remembered_builder_cap
      ? js->rope_gc.remembered_builder_cap * 2u : 64u;
    ant_string_builder_t **items = (ant_string_builder_t **)realloc(
      js->rope_gc.remembered_builders, cap * sizeof(*items)
    );
    if (!items) {
      js->gc_remember_overflow = true;
      return;
    }
    js->rope_gc.remembered_builders = items;
    js->rope_gc.remembered_builder_cap = cap;
  }
  builder->in_remember_set = 1;
  js->rope_gc.remembered_builders[js->rope_gc.remembered_builder_len++] = builder;
}

static void gc_clear_remembered_builders(ant_t *js) {
  for (size_t i = 0; i < js->rope_gc.remembered_builder_len; i++)
    js->rope_gc.remembered_builders[i]->in_remember_set = 0;
  js->rope_gc.remembered_builder_len = 0;
}

void gc_run(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  
  gc_ropes_begin_result_t rope_begin = gc_ropes_begin(js, false);
  ANT_ASSERT(
    rope_begin != GC_ROPES_BEGIN_RETRY_MAJOR,
    "major rope marking cannot request another major"
  );

  size_t live_before = js->obj_arena.live_count;

  gc_bigints_begin(js);
  gc_strings_begin(js);
  
  bool conservative = rope_begin == GC_ROPES_BEGIN_CONSERVATIVE_MAJOR;
  gc_objects_run(
    js, conservative ? gc_mark_flat_str : gc_mark_str,
    conservative ? gc_ropes_mark_conservative_roots : NULL
  );
  
  gc_clear_remembered_builders(js);
  ant_ic_epoch_bump();
  ant_ic_obj_epoch_bump();

  gc_bigints_sweep(js);
  gc_strings_sweep(js);
  gc_ropes_sweep(js, false);

  js->gc_last_live = js->obj_arena.live_count;
  js->old_live_count = js->obj_arena.live_count;
  js->minor_gc_count = 0;

  js->gc_pool_last_live = gc_pool_live_bytes(js);
  js->gc_pool_alloc = 0;
  js->rope_gc.young_alloc = 0;
  js->gc_closure_alloc = 0;
  js->gc_closure_at_minor = 0;
  js->gc_closure_wm_at_major = js->closure_arena.watermark;
  js->gc_closure_wm_minor_tried = js->closure_arena.watermark;
  js->gc_closure_promoted_since_major = 0;
  js->gc_remember_overflow = false;

  gc_adapt_major_interval(live_before, js->obj_arena.live_count);
  gc_last_run_ms = gc_now_ms();
  gc_last_major_ms = gc_last_run_ms;
}

void gc_run_minor(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;

  if (__builtin_expect(js->gc_remember_overflow, 0)) {
    gc_run(js);
    return;
  }
  if (gc_ropes_begin(js, true) != GC_ROPES_BEGIN_NORMAL) {
    gc_run(js);
    return;
  }

  size_t old_before   = js->old_live_count;
  size_t live_before  = js->obj_arena.live_count;
  size_t young_before = live_before > old_before ? live_before - old_before : 0;

  for (size_t i = 0; i < js->rope_gc.remembered_builder_len; i++)
    gc_mark_str(js, ant_mkbuilder_value(js->rope_gc.remembered_builders[i]));
  gc_objects_run_minor(js, gc_mark_str);
  gc_clear_remembered_builders(js);
  gc_ropes_sweep(js, true);

  ant_ic_obj_epoch_bump();

  js->gc_last_live = js->obj_arena.live_count;
  js->old_live_count = js->obj_arena.live_count;
  js->minor_gc_count++;

  size_t survivors = js->obj_arena.live_count > old_before
    ? js->obj_arena.live_count - old_before : 0;

  js->gc_closure_at_minor = js->gc_closure_alloc;
  gc_adapt_nursery(young_before, survivors);
  gc_last_run_ms = gc_now_ms();
}

void gc_pressure(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  gc_tick = GC_MIN_TICK;
  gc_maybe(js);
}

void gc_maybe(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  if (++gc_tick < GC_MIN_TICK) return;
  
  size_t live = js->obj_arena.live_count;
  size_t young_count = live > js->old_live_count ? live - js->old_live_count : 0;
  size_t closure_young = js->gc_closure_alloc > js->gc_closure_at_minor
    ? js->gc_closure_alloc - js->gc_closure_at_minor : 0;

  if (young_count >= gc_nursery_threshold ||
      js->rope_gc.young_alloc >= GC_ROPE_NURSERY_THRESHOLD ||
      closure_young >= GC_CLOSURE_NURSERY_THRESHOLD) {
    gc_tick = 0;
    size_t live_before_minor = js->obj_arena.live_count;
    size_t major_threshold = gc_live_major_threshold(js);
    size_t pool_threshold = gc_pool_major_threshold(js);

    gc_run_minor(js);

    if (js->minor_gc_count >= gc_major_every_n) {
      bool major_due = false;
      
      if (live_before_minor >= major_threshold) major_due = true;
      else if (js->gc_pool_alloc >= pool_threshold) major_due = true;
      else if (js->closure_arena.watermark - js->gc_closure_wm_at_major >= GC_CLOSURE_MAJOR_GROWTH) major_due = true;
      else if (js->gc_closure_promoted_since_major >= GC_CLOSURE_PROMOTED_MAJOR) major_due = true;
      
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
    if (young_count >= live / 4) {
      gc_run_minor(js);
      if (js->obj_arena.live_count < threshold) return;
    }
    gc_run(js);
    return;
  }

  if (js->closure_arena.watermark - js->gc_closure_wm_at_major >= GC_CLOSURE_MAJOR_GROWTH) {
    gc_tick = 0;
    if (js->closure_arena.watermark > js->gc_closure_wm_minor_tried) {
      js->gc_closure_wm_minor_tried = js->closure_arena.watermark;
      gc_run_minor(js);
      return;
    }
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
  if (gc_now_ms() - gc_last_major_ms >= GC_FORCE_MAJOR_INTERVAL_MS) gc_run(js);
  else gc_run_minor(js);
}
