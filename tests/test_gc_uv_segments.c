// meson test -C build gc-uv-segments
//
// libuv's frames between uv_run() and an ant callback hold no JS values, so a
// callback marked with GC_UV_CALLBACK has the collector skip them (see
// gc_uv_seg_t). Here a stand-in for libuv holds the only pointer to an object
// in a stack buffer, like stale poll-buffer contents, and calls a callback
// that runs a full collection: the object must be freed under a marked
// callback and kept under an unmarked one. The segment's lower end must also
// be exactly the libuv frame's stack pointer at the call.
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/uv.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32)
static constexpr uint64_t MASK = 0x5a5a5a5a5a5a5a5aull;

static ant_t *g_js;
static uint64_t g_masked; // the object's address, masked so no scan finds it here
static uintptr_t g_call_sp;
static bool g_survived, g_sp_exact;

static void check_object(void) {
  gc_run(g_js);
  ant_object_t *obj = (ant_object_t *)(uintptr_t)(g_masked ^ MASK);
  g_survived = obj->mark_epoch != ANT_GC_DEAD;
}

__attribute__((noinline)) static void marked_callback(void) {
  GC_UV_CALLBACK();
  g_sp_exact = gc_uv_segs->lo == g_call_sp;
  check_object();
}

__attribute__((noinline)) static void unmarked_callback(void) {
  check_object();
}

// libuv's frame: the object's only pointer sits in its stack buffer
__attribute__((noinline)) static void fake_libuv(void (*callback)(void)) {
  volatile uint64_t events[64];
  memset((void *)events, 0, sizeof(events));
  events[17] = g_masked ^ MASK;
  g_call_sp = gc_native_sp();
  callback();
  __asm__ volatile("" :: "r"(events) : "memory");
}

// ant_uv_run's recording, around the stand-in for libuv
__attribute__((noinline)) static void fake_uv_run(void (*callback)(void)) {
  __builtin_unwind_init();
  uintptr_t prev = gc_uv_run_sp;
  gc_uv_run_sp = gc_native_sp();
  fake_libuv(callback);
  gc_uv_run_sp = prev;
}

__attribute__((noinline)) static void make_object(ant_t *js) {
  ant_value_t obj = js_mkobj(js);
  g_masked = (uint64_t)(uintptr_t)js_obj_ptr(obj) ^ MASK;
}

__attribute__((noinline)) static void scrub_below(void) {
  volatile char region[64 * 1024];
  memset((char *)region, 0, sizeof(region));
}

static bool run(ant_t *js, void (*callback)(void)) {
  make_object(js);
  scrub_below();
  fake_uv_run(callback);
  return g_survived;
}

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, __builtin_frame_address(0));
  g_js = js;

  // an unmarked callback leaves libuv's frames scanned: the object stays
  assert(run(js, unmarked_callback));
  // a marked one skips them: the object goes
  assert(!run(js, marked_callback));
  // and the skipped range starts exactly at libuv's stack pointer
  assert(g_sp_exact);
  assert(gc_uv_segs == NULL);

  puts("PASS libuv frames are skipped under a marked callback and scanned otherwise");
  return 0;
}
#else
int main(void) { puts("SKIP libuv segments not scanned on this target"); return 0; }
#endif
