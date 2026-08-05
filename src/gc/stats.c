#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "internal.h"
#include "gc/stats.h"

bool gc_stats_enabled = false;

typedef struct gc_stats {
  bool exit_registered;
  uint64_t minor_count;
  uint64_t major_count;
  uint64_t young_before;
  uint64_t survivors;
  uint64_t promoted;
  uint64_t remember_adds;
  size_t remember_peak;
  uint64_t minor_time_ns;
  uint64_t major_time_ns;
  uint64_t major_freed;
  uint64_t major_after_minor;
  uint64_t major_live;
  uint64_t major_overflow;
  uint64_t major_pool;
  size_t live;
  size_t arena_watermark_peak;
  size_t arena_committed_peak;
} gc_stats_t;

static gc_stats_t gc_stats = {0};

static uint64_t gc_stats_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void gc_stats_dump(void) {
  if (!gc_stats_enabled) return;

  gc_policy_state_t policy;
  gc_policy_state_get(&policy);

  uint64_t survival_x100 = gc_stats.young_before
    ? (gc_stats.survivors * 10000ULL) / gc_stats.young_before
    : 0;
  uint64_t classified_majors = gc_stats.major_after_minor
    + gc_stats.major_live
    + gc_stats.major_overflow
    + gc_stats.major_pool;
  uint64_t major_other = gc_stats.major_count > classified_majors
    ? gc_stats.major_count - classified_majors
    : 0;
  fprintf(stderr,
    "gc-stats: {\"minor\":%llu,\"major\":%llu,\"young\":%llu,"
    "\"survivors\":%llu,\"survival_pct\":%llu.%02llu,\"promoted\":%llu,"
    "\"remember_adds\":%llu,\"remember_peak\":%zu,\"minor_ms\":%.3f,"
    "\"major_ms\":%.3f,\"major_freed\":%llu,"
    "\"major_after_minor\":%llu,\"major_live\":%llu,\"major_overflow\":%llu,"
    "\"major_pool\":%llu,\"major_other\":%llu,\"nursery\":%zu,"
    "\"major_every\":%u,\"live_growth_x256\":%u,\"pool_growth_x256\":%u,"
    "\"minor_surv_ewma\":%u,\"major_recl_ewma\":%u,\"remember_ewma\":%u,"
    "\"promoted_ewma\":%u,"
    "\"live\":%zu,"
    "\"arena_watermark_peak\":%zu,\"arena_committed_peak\":%zu}\n",
    (unsigned long long)gc_stats.minor_count,
    (unsigned long long)gc_stats.major_count,
    (unsigned long long)gc_stats.young_before,
    (unsigned long long)gc_stats.survivors,
    (unsigned long long)(survival_x100 / 100ULL),
    (unsigned long long)(survival_x100 % 100ULL),
    (unsigned long long)gc_stats.promoted,
    (unsigned long long)gc_stats.remember_adds,
    gc_stats.remember_peak,
    (double)gc_stats.minor_time_ns / 1000000.0,
    (double)gc_stats.major_time_ns / 1000000.0,
    (unsigned long long)gc_stats.major_freed,
    (unsigned long long)gc_stats.major_after_minor,
    (unsigned long long)gc_stats.major_live,
    (unsigned long long)gc_stats.major_overflow,
    (unsigned long long)gc_stats.major_pool,
    (unsigned long long)major_other,
    policy.nursery_threshold,
    policy.major_every_n,
    policy.live_growth_x256,
    policy.pool_growth_x256,
    policy.minor_surv_ewma,
    policy.major_recl_ewma,
    policy.remember_ewma,
    policy.promoted_ewma,
    gc_stats.live,
    gc_stats.arena_watermark_peak,
    gc_stats.arena_committed_peak);
}

void gc_stats_enable(bool enabled) {
  gc_stats_enabled = enabled;
  if (enabled && !gc_stats.exit_registered) {
    gc_stats.exit_registered = true;
    atexit(gc_stats_dump);
  }
}

uint64_t gc_stats_begin(void) {
  return gc_stats_enabled ? gc_stats_now_ns() : 0;
}

static void gc_stats_note_heap(ant_t *js) {
  gc_stats.live = js->obj_arena.live_count;
  if (js->obj_arena.watermark > gc_stats.arena_watermark_peak)
    gc_stats.arena_watermark_peak = js->obj_arena.watermark;
  if (js->obj_arena.committed > gc_stats.arena_committed_peak)
    gc_stats.arena_committed_peak = js->obj_arena.committed;
}

void gc_stats_note_minor(
  ant_t *js, size_t young_before, size_t survivors, uint64_t start_ns
) {
  if (!gc_stats_enabled) return;
  gc_stats.minor_count++;
  gc_stats.young_before += young_before;
  gc_stats.survivors += survivors;
  gc_stats.promoted += survivors;
  gc_stats.minor_time_ns += gc_stats_now_ns() - start_ns;
  gc_stats_note_heap(js);
}

void gc_stats_note_major(ant_t *js, size_t freed, uint64_t start_ns) {
  if (!gc_stats_enabled) return;
  gc_stats.major_count++;
  gc_stats.major_freed += freed;
  gc_stats.major_time_ns += gc_stats_now_ns() - start_ns;
  gc_stats_note_heap(js);
}

void gc_stats_note_major_cause(gc_stats_major_cause_t cause) {
  if (!gc_stats_enabled) return;
  switch (cause) {
    case GC_STATS_MAJOR_AFTER_MINOR: gc_stats.major_after_minor++; break;
    case GC_STATS_MAJOR_LIVE: gc_stats.major_live++; break;
    case GC_STATS_MAJOR_OVERFLOW: gc_stats.major_overflow++; break;
    case GC_STATS_MAJOR_POOL: gc_stats.major_pool++; break;
  }
}

void gc_stats_note_remember(size_t remember_set_len) {
  if (!gc_stats_enabled) return;
  gc_stats.remember_adds++;
  if (remember_set_len > gc_stats.remember_peak)
    gc_stats.remember_peak = remember_set_len;
}
