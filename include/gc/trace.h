#ifndef ANT_GC_TRACE_H
#define ANT_GC_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gc/objects.h"

extern bool gc_trace_enabled;

typedef struct {
  bool on;
  uint64_t start_ns, lap_ns;
  uint64_t total_ns, objects_ns, mark_ns, strings_ns, ropes_ns, trim_ns;
  size_t live_before, arrays_before;
} gc_trace_major_t;

void gc_trace_major_snapshot(ant_t *js, gc_trace_major_t *trace);
void gc_trace_major_print(ant_t *js, const gc_trace_major_t *trace);
void gc_trace_minor_print(ant_t *js, uint64_t start_ns, size_t young, size_t survivors);

static inline gc_trace_major_t gc_trace_major_begin(ant_t *js, uint64_t start_ns) {
  gc_trace_major_t trace = {.on = __builtin_expect(gc_trace_enabled, 0), .start_ns = start_ns};
  if (trace.on) gc_trace_major_snapshot(js, &trace);
  return trace;
}

static inline void gc_trace_lap_start(gc_trace_major_t *trace) {
  if (__builtin_expect(trace->on, 0)) trace->lap_ns = gc_now_ns();
}

static inline void gc_trace_lap(gc_trace_major_t *trace, uint64_t *out) {
  if (__builtin_expect(trace->on, 0)) {
    uint64_t now = gc_now_ns();
    *out = now - trace->lap_ns;
    trace->lap_ns = now;
  }
}

static inline void gc_trace_major_end(ant_t *js, gc_trace_major_t *trace, uint64_t mark_ns, uint64_t end_ns) {
  if (__builtin_expect(trace->on, 0)) {
    trace->mark_ns = mark_ns;
    trace->total_ns = end_ns - trace->start_ns;
    gc_trace_major_print(js, trace);
  }
}

static inline void gc_trace_minor_end(ant_t *js, uint64_t start_ns, size_t young, size_t survivors) {
  if (__builtin_expect(gc_trace_enabled, 0)) gc_trace_minor_print(js, start_ns, young, survivors);
}

#endif
