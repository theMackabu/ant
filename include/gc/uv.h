#ifndef ANT_GC_UV_H
#define ANT_GC_UV_H

#include <stdint.h>

/*
 * ant's callbacks run under libuv's frames, and the conservative stack scan
 * would see whatever stale data libuv leaves there. libuv never holds a JS
 * value, so those frames can be skipped.
 *
 * The vendored libuv-poll-buffers.patch moves the big poll buffers off the
 * stack and into the loop, which leaves uv_run()'s frames small:
 *
 *   kqueue.c    uv__io_poll()   struct kevent events[1024]
 *   linux.c     uv__io_poll()   struct epoll_event events[1024], prep[256]
 *   win/core.c  uv__poll()      OVERLAPPED_ENTRY overlappeds[128]
 *
 * What's left is skipped per callback. ant_uv_run() records its stack pointer
 * in gc_uv_run_sp before entering uv_run(), and a callback that starts with
 * GC_UV_CALLBACK() pushes a segment from its caller's stack pointer up to that
 * mark, which is exactly libuv's frames. The collector skips each segment
 * like an interpreter segment. ant_uv_run() is noinline and spills its
 * callee-saved registers above the mark, so its caller's live values stay
 * scanned even when libuv saves those registers below it.
 *
 * Segments nest: a blocking await inside a callback runs uv_run() again, and
 * the inner ant_uv_run() and callbacks push on top. A callback without the
 * mark, or one that runs outside ant_uv_run(), gets no segment and its libuv
 * frames are scanned as before, which is always safe.
 */

typedef struct gc_uv_seg {
  struct gc_uv_seg *prev;
  uintptr_t lo, hi;
} gc_uv_seg_t;

extern _Thread_local gc_uv_seg_t *gc_uv_segs;
extern _Thread_local uintptr_t gc_uv_run_sp;

#if defined(__clang__) || defined(__x86_64__)
#define GC_CALLER_SP() ((uintptr_t)__builtin_frame_address(0) + 16)
#else
#define GC_CALLER_SP() ((uintptr_t)__builtin_dwarf_cfa())
#endif

static inline void gc_uv_seg_enter(gc_uv_seg_t *seg, uintptr_t caller_sp) {
  uintptr_t hi = gc_uv_run_sp;
  seg->prev = gc_uv_segs;
  seg->lo = caller_sp;
  seg->hi = hi > caller_sp ? hi : caller_sp;
  gc_uv_segs = seg;
}

static inline void gc_uv_seg_leave(gc_uv_seg_t *seg) {
  gc_uv_segs = seg->prev;
}

#define GC_UV_CALLBACK()                                                \
  gc_uv_seg_t gc_uv_seg_ __attribute__((cleanup(gc_uv_seg_leave)));     \
  gc_uv_seg_enter(&gc_uv_seg_, GC_CALLER_SP())

#endif
