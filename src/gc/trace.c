#include <gc.h>
#include <inttypes.h>
#include <stdio.h>
#include "gc/trace.h"

bool gc_trace_enabled = false;

static _Thread_local uint64_t gc_trace_origin_ns;
static _Thread_local uint64_t gc_trace_majors, gc_trace_minors;

static double gc_trace_ms(uint64_t ns) {
  return (double)ns / 1e6;
}

static double gc_trace_mb(size_t bytes) {
  return (double)bytes / 1e6;
}

static double gc_trace_at(uint64_t start_ns) {
  if (!gc_trace_origin_ns) gc_trace_origin_ns = start_ns;
  return gc_trace_ms(start_ns - gc_trace_origin_ns);
}

void gc_trace_major_snapshot(ant_t *js, gc_trace_major_t *trace) {
  trace->live_before = js->obj_arena.live_count;
  trace->arrays_before = js->alloc_bytes.arrays;
  trace->lap_ns = gc_now_ns();
}

__attribute__((noinline, cold)) void gc_trace_major_print(ant_t *js, const gc_trace_major_t *trace) {
  uint64_t n = ++gc_trace_majors;
  
  double at = gc_trace_at(trace->start_ns);
  double total = gc_trace_ms(trace->total_ns);
  double mark = gc_trace_ms(trace->mark_ns);
  double sweep = gc_trace_ms(trace->objects_ns - trace->mark_ns);
  double strings = gc_trace_ms(trace->strings_ns);
  double ropes = gc_trace_ms(trace->ropes_ns);
  double trim = gc_trace_ms(trace->trim_ns);

  fprintf(stderr,
    "gc: major n=%" PRIu64 " at=%.1fms ms=%.2f"
    " mark=%.2f sweep=%.2f strings=%.2f ropes=%.2f trim=%.2f"
    " live=%zu->%zu arrays=%.1f->%.1fMB"
    " growth=%u time_share=%u work_share=%u every_n=%u next=%zu units=%u\n",
    n, at, total,
    mark, sweep, strings, ropes, trim,
    trace->live_before, js->obj_arena.live_count, gc_trace_mb(trace->arrays_before), gc_trace_mb(js->alloc_bytes.arrays),
    js->gc.major_live_growth_x256, js->gc.major_time_share_ewma, js->gc.major_work_share_ewma,
    js->gc.major_every_n, gc_live_major_threshold(js), js->code_units.unit_count);
}

__attribute__((noinline, cold)) void gc_trace_minor_print(ant_t *js, uint64_t start_ns, size_t young, size_t survivors) {
  uint64_t n = ++gc_trace_minors;
  fprintf(stderr,
    "gc: minor n=%" PRIu64 " at=%.1fms ms=%.2f"
    " young=%zu survivors=%zu remembered=%zu skip=%d\n",
    n, gc_trace_at(start_ns), gc_trace_ms(js->gc.minor_cost_ns),
    young, survivors, js->remember_set_len, js->gc.skip_minors);
}
