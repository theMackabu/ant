#include <gc.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "shapes.h"

#include "gc/objects.h"
#include "gc/verify.h"
#include "gc/bigints.h"
#include "gc/strings.h"
#include "gc/ropes.h"
#include "silver/engine.h"

#ifdef ANT_WASM_EMBED
#include "wasm_embed.h"
#endif

bool gc_disabled = false;

void gc_state_init(ant_t *js) {
  js->gc.nursery_threshold = GC_NURSERY_THRESHOLD;
  js->gc.major_every_n = GC_MAJOR_EVERY_N_MINOR;
  js->gc.major_live_growth_x256 = 384;
  js->gc.major_pool_growth_x256 = 384;
  js->gc.minor_surv_ewma = 128;
  js->gc.major_recl_ewma = 26;
}

static size_t gc_young_alloc_bytes(ant_t *js) {
  size_t live = js->obj_arena.live_count;
  size_t young = live > js->old_live_count ? live - js->old_live_count : 0;
  
  size_t arrays = 
    js->alloc_bytes.arrays > js->gc.arrays_at_collect ? 
    js->alloc_bytes.arrays - js->gc.arrays_at_collect : 0;
    
  return young * js->obj_arena.elem_size + arrays;
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
    + string_stats.total.used
    + js->code_units.held_bytes;
}

size_t gc_live_major_threshold(ant_t *js) {
  // major must wait until the old generation has grown by several nursery
  // promotions since the last major: a multiple of a small baseline is still
  // small, and the youngest old objects were live moments ago, so a major
  // right after a promotion reclaims nothing and only inflates the growth
  // factor. The floor is therefore in units of promotions, not a constant.
  size_t threshold = gc_scaled_threshold(
    js->gc.last_live, js->gc.major_live_growth_x256,
    js->gc.last_live + GC_MAJOR_MIN_PROMOTIONS * js->gc.nursery_threshold
  );

  bool nursery_churn = js->gc.minor_surv_ewma <= 64;   // <= 25% young survival
  bool nursery_sticky = js->gc.minor_surv_ewma >= 160; // >= 62.5% young survival
  bool major_pays = js->gc.major_recl_ewma >= 51;      // >= 20% old-gen reclaim
  bool major_wasteful = js->gc.major_recl_ewma <= 13;  // <= 5% old-gen reclaim

  if (js->gc_use_nursery_major_floor) {
    if (nursery_sticky || (major_pays && !nursery_churn)) js->gc_use_nursery_major_floor = false;
  } else if (nursery_churn || major_wasteful) js->gc_use_nursery_major_floor = true;

  if (!js->gc_use_nursery_major_floor) return threshold;
  size_t nursery_floor = js->old_live_count + js->gc.nursery_threshold;
  
  return threshold < nursery_floor ? nursery_floor : threshold;
}

size_t gc_pool_major_threshold(ant_t *js) {
  return gc_scaled_threshold(js->gc.pool_last_live, js->gc.major_pool_growth_x256, GC_POOL_PRESSURE_FLOOR);
}

static void gc_adapt_nursery(ant_t *js, size_t young_before, size_t survivors) {
  if (young_before == 0) return;
  uint32_t rate = (uint32_t)((survivors * 256) / young_before);
  js->gc.minor_surv_ewma = (js->gc.minor_surv_ewma * 3 + rate) >> 2;
  if (js->gc.minor_surv_ewma < 64 && js->gc.nursery_threshold > GC_NURSERY_THRESHOLD / 2)
    js->gc.nursery_threshold -= js->gc.nursery_threshold / 4;
  else if (js->gc.nursery_threshold < GC_NURSERY_THRESHOLD)
    js->gc.nursery_threshold = GC_NURSERY_THRESHOLD;
}

static size_t gc_array_growth(size_t base) {
  return base > GC_ARRAY_GROWTH_FLOOR ? base : GC_ARRAY_GROWTH_FLOOR;
}

static size_t gc_heap_bytes(ant_t *js, size_t pool_bytes) {
  return 
    js->obj_arena.live_count * 
    js->obj_arena.elem_size + 
    js->alloc_bytes.arrays + pool_bytes;
}

size_t gc_code_major_threshold(ant_t *js) {
  size_t pool = gc_pool_major_threshold(js);
  size_t heap = gc_heap_bytes(js, js->gc.pool_last_live) / 8;
  return pool > heap ? pool : heap;
}

static void gc_adapt_major_interval(
  ant_t *js, size_t bytes_before, size_t bytes_after, 
  uint64_t mark_ns, uint64_t end_ns
) {
  if (js->gc.last_major_end_ns != 0 && end_ns > js->gc.last_major_end_ns) {
    uint64_t interval_ns = end_ns - js->gc.last_major_end_ns;
    uint32_t share = (uint32_t)((mark_ns * 1024) / interval_ns);
    js->gc.major_time_share_ewma = (js->gc.major_time_share_ewma * 3 + share) >> 2;
  }
  
  js->gc.last_major_end_ns = end_ns;

  if (js->gc.alloc_since_major > 0) {
    uint64_t work = (uint64_t)bytes_after * 1024u / js->gc.alloc_since_major;
    uint32_t share = work > UINT16_MAX ? UINT16_MAX : (uint32_t)work;
    js->gc.major_work_share_ewma = (js->gc.major_work_share_ewma * 3 + share) >> 2;
  }
  
  js->gc.alloc_since_major = 0;
  if (bytes_before == 0) return;
  
  size_t freed = bytes_before > bytes_after ? bytes_before - bytes_after : 0;
  uint32_t rate = (uint32_t)((freed * 256) / bytes_before);
  js->gc.major_recl_ewma = (js->gc.major_recl_ewma * 3 + rate) >> 2;

  bool gen_ineffect = js->gc.minor_surv_ewma  > 192; // >75% nursery survival
  bool high_reclaim = js->gc.major_recl_ewma  >  51; // >20% old-gen freed
  bool low_reclaim  = js->gc.major_recl_ewma  <  13; // < 5% old-gen freed
  
  bool majors_costly = js->gc.major_time_share_ewma > GC_MAJOR_TIME_SHARE_HIGH 
    && js->gc.major_work_share_ewma > GC_MAJOR_WORK_SHARE_HIGH;
  bool majors_cheap  = js->gc.major_time_share_ewma < GC_MAJOR_TIME_SHARE_LOW;

  if (majors_costly) {
    if (js->gc.major_every_n < GC_MAJOR_EVERY_N_MINOR * 4) js->gc.major_every_n++;
    if (js->gc.major_live_growth_x256 < 1024) js->gc.major_live_growth_x256 += 64;
    if (js->gc.major_pool_growth_x256 < 1536) js->gc.major_pool_growth_x256 += 64;
    return;
  }

  if ((gen_ineffect && !low_reclaim) || (high_reclaim && majors_cheap)) {
    if (js->gc.major_every_n > 2) js->gc.major_every_n--;
  } else if (!gen_ineffect && low_reclaim) {
    if (js->gc.major_every_n < GC_MAJOR_EVERY_N_MINOR * 4) js->gc.major_every_n++;
  }

  if (gen_ineffect && low_reclaim) {
    if (js->gc.major_live_growth_x256 < 1024) js->gc.major_live_growth_x256 += 96;
    if (js->gc.major_pool_growth_x256 < 1536) js->gc.major_pool_growth_x256 += 128;
  } else if (low_reclaim) {
    if (js->gc.major_live_growth_x256 < 896) js->gc.major_live_growth_x256 += 48;
    if (js->gc.major_pool_growth_x256 < 1280) js->gc.major_pool_growth_x256 += 64;
  } else if (high_reclaim && majors_cheap) {
    if (js->gc.major_live_growth_x256 > 320) js->gc.major_live_growth_x256 -= 32;
    if (js->gc.major_pool_growth_x256 > 320) js->gc.major_pool_growth_x256 -= 32;
  } else {
    if (js->gc.major_live_growth_x256 > 384) js->gc.major_live_growth_x256 -= 16;
    else if (js->gc.major_live_growth_x256 < 384) js->gc.major_live_growth_x256 += 16;
    if (js->gc.major_pool_growth_x256 > 384) js->gc.major_pool_growth_x256 -= 16;
    else if (js->gc.major_pool_growth_x256 < 384) js->gc.major_pool_growth_x256 += 16;
  }
}

void gc_mark_str(ant_t *js, ant_value_t root) {
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
  if (t != kTypeString) goto l_pop;

  uintptr_t tag = (uintptr_t)(vdata(v) & STR_HEAP_TAG_MASK);
  uintptr_t data = (uintptr_t)vptr_masked(v, STR_HEAP_TAG_MASK);

  if (tag < sizeof(dispatch) / sizeof(*dispatch) && dispatch[tag])
    goto *dispatch[tag];
  goto l_pop;

  l_rope: {
    ant_rope_heap_t *rope = (ant_rope_heap_t *)data;
    constexpr size_t align_rope = _Alignof(ant_rope_heap_t);
    if (gc_ropes_mark(js, rope, sizeof(*rope), align_rope) != GC_ROPE_MARK_TRACE) goto l_pop;

    if (vtype(rope->cached) == kTypeString) {
      v = rope->cached;
      goto l_next;
    }

    if (!ant_value_stack_push_with_spill(&stack, &sp, &cap, local, rope->left)) 
      gc_mark_str(js, rope->left);
    
    v = rope->right;
    goto l_next;
  }

  l_builder: {
    ant_string_builder_t *builder = (ant_string_builder_t *)data;
    constexpr size_t align_string = _Alignof(ant_string_builder_t);
    if (gc_ropes_mark(js, builder, sizeof(*builder), align_string) != GC_ROPE_MARK_TRACE) goto l_pop;
    
    gc_mark_str(js, builder->snapshot);
    gc_mark_value(js, builder->cached);
    
    for (ant_builder_chunk_t *chunk = builder->head; chunk; chunk = chunk->next) {
      constexpr size_t align_builder = _Alignof(ant_builder_chunk_t);
      gc_rope_mark_result_t marked = gc_ropes_mark(js, chunk, sizeof(*chunk), align_builder);
      if (marked == GC_ROPE_MARK_INVALID) break;
      if (marked == GC_ROPE_MARK_TRACE) gc_mark_value(js, chunk->value);
    }
    
    goto l_pop;
  }

  l_flat:
    if (
      data && !js->rope_gc.minor_marking &&
      !str_flat_is_permanent((const ant_flat_string_t *)data)
    ) gc_strings_mark(js, (const void *)data);
  l_pop:
    if (sp > 0) {
      v = stack[--sp];
      goto l_next;
    }
    if (stack != local) free(stack);
    return;
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
  js->gc_running = true;
  
  uint64_t start_ns = gc_now_ns();
  js->gc.alloc_since_major += gc_young_alloc_bytes(js) + js->gc.pool_alloc + js->gc.code_alloc;
  
  gc_ropes_begin_result_t rope_begin = gc_ropes_begin(js, false);
  ANT_ASSERT(
    rope_begin != GC_ROPES_BEGIN_RETRY_MAJOR,
    "major rope marking cannot request another major"
  );

  size_t bytes_before = gc_heap_bytes(js, gc_pool_live_bytes(js));

  gc_bigints_begin(js);
  gc_strings_begin(js);
  
  bool conservative = rope_begin == GC_ROPES_BEGIN_CONSERVATIVE_MAJOR;
  uint64_t mark_ns = gc_objects_run(js, conservative ? gc_ropes_mark_conservative_roots : NULL);
  
  gc_clear_remembered_builders(js);
  ant_ic_epoch_bump(js);
  ant_ic_obj_epoch_bump(js);
  sv_gf_mega_clear(js);
  sv_ic_polys_release_shapes(js);

  gc_bigints_sweep(js);
  gc_strings_sweep(js);
  gc_ropes_sweep(js, false);
  gc_array_storage_trim(js);

  js->gc.last_live = js->obj_arena.live_count;
  js->old_live_count = js->obj_arena.live_count;
  js->minor_gc_count = 0;

  js->gc.pool_last_live = gc_pool_live_bytes(js);
  js->gc.pool_alloc = 0;
  js->gc.code_alloc = 0;
  js->rope_gc.young_alloc = 0;
  js->gc.closure_alloc = 0;
  js->gc.closure_at_minor = 0;
  js->gc.closure_wm_at_major = js->closure_arena.watermark;
  js->gc.closure_wm_minor_tried = js->closure_arena.watermark;
  js->gc_closure_promoted_since_major = 0;
  js->gc_remember_overflow = false;
  js->gc.arrays_at_collect = js->alloc_bytes.arrays;
  
  gc_array_limits_init(js);
  js->gc.idle_retry_at = 0;

  uint64_t end_ns = gc_now_ns();
  js->gc.major_cost_ns = end_ns - start_ns;
  
  gc_adapt_major_interval(
    js, bytes_before, 
    gc_heap_bytes(js, js->gc.pool_last_live), 
    mark_ns, end_ns
  );
  
  gc_refresh_alloc_limit(js);
  js->gc_running = false;
}

void gc_run_minor(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  js->gc_running = true;
  uint64_t start_ns = gc_now_ns();

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
  js->gc.alloc_since_major += gc_young_alloc_bytes(js);

  for (size_t i = 0; i < js->rope_gc.remembered_builder_len; i++)
    gc_mark_str(js, ant_mkbuilder_value(js->rope_gc.remembered_builders[i]));
  
  gc_objects_run_minor(js);
  gc_clear_remembered_builders(js);
  gc_ropes_sweep(js, true);
  ant_ic_obj_epoch_bump(js);

  js->old_live_count = js->obj_arena.live_count;
  js->minor_gc_count++;

  size_t survivors = js->obj_arena.live_count > old_before
    ? js->obj_arena.live_count - old_before : 0;

  js->gc.closure_at_minor = js->gc.closure_alloc;
  gc_adapt_nursery(js, young_before, survivors);
  
  js->gc.arrays_at_collect = js->alloc_bytes.arrays;
  js->gc.idle_retry_at = 0;
  js->gc.minor_cost_ns = gc_now_ns() - start_ns;
  
  gc_refresh_alloc_limit(js);
  js->gc_running = false;
}

void gc_pressure(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  js->gc.tick = GC_MIN_TICK;
  gc_maybe(js);
}

static void gc_decide(ant_t *js) {
  size_t live = js->obj_arena.live_count;
  size_t young_count = live > js->old_live_count ? live - js->old_live_count : 0;
  
  size_t closure_young = 
    js->gc.closure_alloc > js->gc.closure_at_minor ? 
    js->gc.closure_alloc - js->gc.closure_at_minor : 0;

  if (
    young_count >= js->gc.nursery_threshold                  ||
    js->rope_gc.young_alloc >= GC_ROPE_NURSERY_THRESHOLD ||
    closure_young >= GC_CLOSURE_NURSERY_THRESHOLD        ||
    js->alloc_bytes.arrays >= js->gc.array_limit
  ) {
    js->gc.tick = 0;
    size_t live_before_minor = js->obj_arena.live_count;
    size_t major_threshold = gc_live_major_threshold(js);
    size_t pool_threshold = gc_pool_major_threshold(js);

    gc_run_minor(js);

    if (js->minor_gc_count >= js->gc.major_every_n) {
      bool major_due = false;
      
      if (live_before_minor >= major_threshold) major_due = true;
      else if (js->gc.pool_alloc >= pool_threshold) major_due = true;
      else if (js->gc.code_alloc >= gc_code_major_threshold(js)) major_due = true;
      else if (js->closure_arena.watermark - js->gc.closure_wm_at_major >= GC_CLOSURE_MAJOR_GROWTH) major_due = true;
      else if (js->gc_closure_promoted_since_major >= GC_CLOSURE_PROMOTED_MAJOR) major_due = true;
      else if (js->alloc_bytes.arrays >= js->gc.array_major_limit) major_due = true;
      
      if (major_due) {
        js->minor_gc_count = 0;
        gc_run(js);
      }
    }

    return;
  }

  size_t threshold = gc_live_major_threshold(js);
  if (live >= threshold) {
    js->gc.tick = 0;
    
    if (young_count >= live / 4) {
      gc_run_minor(js);
      if (js->obj_arena.live_count < threshold) return;
    }
    
    gc_run(js);
    return;
  }

  if (js->closure_arena.watermark - js->gc.closure_wm_at_major >= GC_CLOSURE_MAJOR_GROWTH) {
    js->gc.tick = 0;
    
    if (js->closure_arena.watermark > js->gc.closure_wm_minor_tried) {
      js->gc.closure_wm_minor_tried = js->closure_arena.watermark;
      gc_run_minor(js);
      return;
    }
    
    gc_run(js);
    return;
  }

  // nothing is due: check again after another GC_MIN_TICK polls. Garbage
  // below every threshold is left for the idle hook (gc_idle), not collected
  // on a clock, so collections depend only on what the program allocates.
  js->gc.tick = 0;
  gc_refresh_alloc_limit(js);
}

void gc_refresh_alloc_limit(ant_t *js) {
  size_t nursery = js->old_live_count + js->gc.nursery_threshold;
  size_t major = gc_live_major_threshold(js);
  js->gc.alloc_limit = nursery < major ? nursery : major;
  js->gc.array_limit = js->gc.arrays_at_collect + 
    gc_array_growth(gc_heap_bytes(js, js->gc.pool_last_live));
}

void gc_array_limits_init(ant_t *js) {
  js->gc.array_major_limit = js->alloc_bytes.arrays + 
    gc_array_growth(gc_heap_bytes(js, js->gc.pool_last_live));
}

void gc_array_grew(ant_t *js) {
  if (js->alloc_bytes.arrays >= js->gc.array_limit) js->gc.alloc_limit = 0;
}

bool gc_alloc_due(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return false;
  return js->obj_arena.live_count >= js->gc.alloc_limit;
}

void gc_alloc_check(ant_t *js) {
  GC_VERIFY_STRESS(js);
  if (gc_alloc_due(js)) gc_decide(js);
}

void gc_maybe(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  if (++js->gc.tick < GC_MIN_TICK) return;
  gc_decide(js);
}

size_t gc_alloc_marker(ant_t *js) {
  return js->obj_arena.live_count + js->alloc_bytes.arrays
    + js->gc.pool_alloc + js->gc.code_alloc + js->gc.closure_alloc;
}

static bool gc_idle_major_due(ant_t *js) {
  size_t last = js->gc.last_live;
  size_t threshold = gc_live_major_threshold(js);
  return js->obj_arena.live_count >= last + (threshold - last) / 2
    || js->gc.pool_alloc >= gc_pool_major_threshold(js) / 2
    || js->gc.code_alloc >= gc_code_major_threshold(js) / 2;
}

static size_t gc_young_count(ant_t *js) {
  size_t live = js->obj_arena.live_count;
  return live > js->old_live_count ? live - js->old_live_count : 0;
}

bool gc_idle_wanted(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0) || js->gc_running) return false;
  if (gc_alloc_marker(js) < js->gc.idle_retry_at) return false;
  return gc_idle_major_due(js) || gc_young_count(js) >= js->gc.nursery_threshold / 4;
}

static bool gc_idle_fits(uint64_t cost_ns, bool known, int64_t budget_ms) {
  if (budget_ms < 0) return true;
  return known && cost_ns * 2 <= (uint64_t)budget_ms * 1000000u;
}

void gc_idle(ant_t *js, int64_t budget_ms) {
  if (!gc_idle_wanted(js)) return;

  js->gc.tick = 0;
  if (gc_idle_major_due(js) && gc_idle_fits(js->gc.major_cost_ns, js->gc.major_cost_ns != 0, budget_ms)) {
    js->minor_gc_count = 0;
    gc_run(js);
  }
  
  else if (
    gc_young_count(js) > 0 && gc_idle_fits(
    js->gc.minor_cost_ns ? js->gc.minor_cost_ns : 2000000, true, budget_ms)
  ) gc_run_minor(js);
  
  js->gc.idle_retry_at = gc_alloc_marker(js) + GC_NURSERY_THRESHOLD * sizeof(ant_object_t) / 8;
}
