#include <gc.h>
#include <stdbool.h>
#include <stdint.h>
#include "shapes.h"

#include "gc/objects.h"
#include "gc/refs.h"
#include "gc/stats.h"
#include "gc/strings.h"
#include "gc/ropes.h"

bool gc_disabled = false;

static size_t   gc_tick = 0;

static size_t   gc_nursery_threshold = GC_NURSERY_THRESHOLD;
static uint32_t gc_major_every_n     = GC_MAJOR_EVERY_N_MINOR;

static uint32_t gc_major_live_growth_x256 = 384;
static uint32_t gc_major_pool_growth_x256 = 384;

static uint32_t gc_minor_surv_ewma = 128;
static uint32_t gc_major_recl_ewma =  26;
static uint32_t gc_remember_ewma   =   0;
static uint32_t gc_promoted_ewma   =   0;
static bool gc_use_nursery_major_floor = true;

void gc_policy_state_get(gc_policy_state_t *out) {
  out->nursery_threshold = gc_nursery_threshold;
  out->major_every_n = gc_major_every_n;
  out->live_growth_x256 = gc_major_live_growth_x256;
  out->pool_growth_x256 = gc_major_pool_growth_x256;
  out->minor_surv_ewma = gc_minor_surv_ewma;
  out->major_recl_ewma = gc_major_recl_ewma;
  out->remember_ewma = gc_remember_ewma;
  out->promoted_ewma = gc_promoted_ewma;
}

static uint32_t gc_grow_toward(uint32_t value, uint32_t amount, uint32_t limit) {
  if (value >= limit) return limit;
  return amount >= limit - value ? limit : value + amount;
}

static uint32_t gc_shrink_toward(uint32_t value, uint32_t amount, uint32_t limit) {
  if (value <= limit) return limit;
  return amount >= value - limit ? limit : value - amount;
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

  bool nursery_sticky = gc_minor_surv_ewma >= 160;
  bool major_wasteful = gc_major_recl_ewma <= 13;

  if (gc_use_nursery_major_floor) {
    if (nursery_sticky) gc_use_nursery_major_floor = false;
  } else if (gc_minor_surv_ewma <= 128 || major_wasteful) {
    gc_use_nursery_major_floor = true;
  }

  if (!gc_use_nursery_major_floor) return threshold;
  size_t nursery_floor = js->old_live_count + gc_nursery_threshold;
  return threshold < nursery_floor ? nursery_floor : threshold;
}

size_t gc_pool_major_threshold(ant_t *js) {
  return gc_scaled_threshold(
    js->gc_pool_last_live, gc_major_pool_growth_x256, GC_POOL_PRESSURE_FLOOR
  );
}

static void gc_adapt_nursery(
  size_t young_before, size_t survivors, size_t remembered_writers
) {
  if (young_before == 0) return;
  uint32_t rate = (uint32_t)((survivors * 256) / young_before);
  gc_minor_surv_ewma = (gc_minor_surv_ewma * 3 + rate) >> 2;
  uint32_t promoted = survivors > UINT32_MAX ? UINT32_MAX : (uint32_t)survivors;
  gc_promoted_ewma = (gc_promoted_ewma * 3 + promoted) >> 2;
  uint32_t remembered = remembered_writers > 64 ? 64 : (uint32_t)remembered_writers;
  gc_remember_ewma = (gc_remember_ewma * 3 + remembered) >> 2;

  size_t step = GC_NURSERY_THRESHOLD / 4;
  size_t minimum = GC_NURSERY_THRESHOLD / 2;
  size_t maximum = GC_NURSERY_THRESHOLD * 2;
  if (gc_remember_ewma >= 12 && gc_nursery_threshold < maximum) {
    size_t growth = gc_nursery_threshold / 2;
    if (growth < step) growth = step;
    size_t remaining = maximum - gc_nursery_threshold;
    gc_nursery_threshold += growth < remaining ? growth : remaining;
  } else if (gc_remember_ewma <= 4) {
    size_t target = gc_minor_surv_ewma < 160 ? minimum : GC_NURSERY_THRESHOLD;
    if (gc_nursery_threshold > target) {
      size_t excess = gc_nursery_threshold - target;
      gc_nursery_threshold -= step < excess ? step : excess;
    } else if (gc_nursery_threshold < target) {
      size_t remaining = target - gc_nursery_threshold;
      gc_nursery_threshold += step < remaining ? step : remaining;
    }
  }
}

static void gc_adapt_major_interval(size_t live_before, size_t live_after) {
  if (live_before == 0) return;
  size_t freed = live_before > live_after ? live_before - live_after : 0;
  uint32_t rate = (uint32_t)((freed * 256) / live_before);
  gc_major_recl_ewma = (gc_major_recl_ewma * 3 + rate) >> 2;

  bool gen_ineffective = gc_minor_surv_ewma  > 192; // >75% nursery survival
  bool nursery_churn   = gc_minor_surv_ewma  <  64; // <25% nursery survival
  bool promotion_pressure = gc_promoted_ewma >= GC_NURSERY_THRESHOLD / 2;
  bool high_reclaim    = gc_major_recl_ewma  >  51; // >20% old-gen freed
  bool low_reclaim     = gc_major_recl_ewma  <  13; // < 5% old-gen freed

  if ((gen_ineffective && !low_reclaim) ||
      (high_reclaim && (!nursery_churn || promotion_pressure))) {
    if (gc_major_every_n > 2) gc_major_every_n--;
  } else if (!gen_ineffective &&
             (low_reclaim || (nursery_churn && !promotion_pressure))) {
    uint32_t limit = low_reclaim
      ? GC_MAJOR_EVERY_N_MINOR * 4
      : GC_MAJOR_EVERY_N_MINOR;
    if (gc_major_every_n < limit) gc_major_every_n++;
  }

  if (gen_ineffective && low_reclaim) {
    gc_major_live_growth_x256 = gc_grow_toward(gc_major_live_growth_x256, 96, 1024);
    gc_major_pool_growth_x256 = gc_grow_toward(gc_major_pool_growth_x256, 128, 1536);
  } else if (low_reclaim) {
    gc_major_live_growth_x256 = gc_grow_toward(gc_major_live_growth_x256, 48, 896);
    gc_major_pool_growth_x256 = gc_grow_toward(gc_major_pool_growth_x256, 64, 1280);
  } else if (high_reclaim) {
    if (nursery_churn)
      gc_major_live_growth_x256 = gc_grow_toward(gc_major_live_growth_x256, 16, 384);
    else
      gc_major_live_growth_x256 = gc_shrink_toward(gc_major_live_growth_x256, 32, 320);
    gc_major_pool_growth_x256 = gc_shrink_toward(gc_major_pool_growth_x256, 32, 320);
  } else {
    if (gc_major_live_growth_x256 > 384)
      gc_major_live_growth_x256 = gc_shrink_toward(gc_major_live_growth_x256, 16, 384);
    else
      gc_major_live_growth_x256 = gc_grow_toward(gc_major_live_growth_x256, 16, 384);
    if (gc_major_pool_growth_x256 > 384)
      gc_major_pool_growth_x256 = gc_shrink_toward(gc_major_pool_growth_x256, 16, 384);
    else
      gc_major_pool_growth_x256 = gc_grow_toward(gc_major_pool_growth_x256, 16, 384);
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
    if (!gc_ropes_contains(rope, sizeof(*rope), _Alignof(ant_rope_heap_t))) return;
    if (!gc_ropes_mark(rope)) return;
    gc_mark_str(js, rope->left);
    gc_mark_str(js, rope->right);
    gc_mark_str(js, rope->cached);
    return;
  }

  l_builder: {
    ant_string_builder_t *builder = (ant_string_builder_t *)(data & ~STR_HEAP_TAG_MASK);
    if (!gc_ropes_contains(builder, sizeof(*builder), _Alignof(ant_string_builder_t))) return;
    if (!gc_ropes_mark(builder)) return;
    gc_mark_value(js, builder->cached);
    for (ant_builder_chunk_t *chunk = builder->head; chunk; chunk = chunk->next) {
      if (!gc_ropes_contains(chunk, sizeof(*chunk), _Alignof(ant_builder_chunk_t))) break;
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
  uint64_t start_ns = gc_stats_begin();
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
  js->gc_remember_overflow = false;

  gc_adapt_major_interval(live_before, js->obj_arena.live_count);
  gc_stats_note_major(js,
    live_before > js->obj_arena.live_count
      ? live_before - js->obj_arena.live_count
      : 0,
    start_ns);
}

void gc_run_minor(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;

  if (__builtin_expect(js->gc_remember_overflow, 0)) {
    gc_stats_note_major_cause(GC_STATS_MAJOR_OVERFLOW);
    gc_run(js);
    return;
  }

  size_t old_before   = js->old_live_count;
  size_t live_before  = js->obj_arena.live_count;
  size_t young_before = live_before > old_before ? live_before - old_before : 0;
  size_t remembered_writers = js->remember_set_len;
  uint64_t start_ns = gc_stats_begin();

  gc_objects_run_minor(js, NULL);
  ant_ic_epoch_bump();

  js->gc_last_live = js->obj_arena.live_count;
  js->old_live_count = js->obj_arena.live_count;
  js->minor_gc_count++;

  size_t survivors = js->obj_arena.live_count > old_before
    ? js->obj_arena.live_count - old_before : 0;
    
  gc_adapt_nursery(young_before, survivors, remembered_writers);
  gc_stats_note_minor(js, young_before, survivors, start_ns);
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
        gc_stats_note_major_cause(GC_STATS_MAJOR_AFTER_MINOR);
        gc_run(js);
      }
    }

    return;
  }

  size_t threshold = gc_live_major_threshold(js);

  if (live >= threshold) {
    gc_tick = 0;
    gc_stats_note_major_cause(GC_STATS_MAJOR_LIVE);
    gc_run(js);
    return;
  }

}
