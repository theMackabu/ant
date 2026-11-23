#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "silver/engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32)
static constexpr uint64_t MASK = 0x5a5a5a5a5a5a5a5aull;
static constexpr size_t STALE_WORDS = 32;

static uint64_t g_masked;

static void run(ant_t *js, const char *source) {
  ant_value_t result = js_eval_bytecode(js, source, strlen(source));
  if (is_err(result)) fprintf(stderr, "%s\n", js_str(js, js_tostring_val(js, Ant_Exception_Current(js))));
  assert(!is_err(result));
}

__attribute__((noinline)) static sv_func_t *compiled_function(ant_t *js) {
  sv_func_t *func = js_func_closure(js_get(js, js->global, "use"))->func;
  g_masked = (uint64_t)(uintptr_t)func->jit_code ^ MASK;
  return func;
}

__attribute__((noinline)) static void plant(volatile uintptr_t *words) {
  for (size_t i = 0; i < STALE_WORDS; i++) words[i] = (uintptr_t)(g_masked ^ MASK);
}

__attribute__((noinline)) static void scrub_below(void) {
  volatile char region[64 * 1024];
  memset((char *)region, 0, sizeof(region));
}

__attribute__((noinline)) static void collect_over_stale_words(ant_t *js, bool in_interp_frame) {
  __builtin_unwind_init();
  volatile uintptr_t words[STALE_WORDS];
  gc_vm_seg_t seg;
  if (in_interp_frame) {
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t hi = fp - GC_VM_SEG_SAVED_REGS_BYTES;
    seg.prev = js->vm_segs;
    seg.lo = gc_native_sp();
    seg.hi = hi > seg.lo ? hi : seg.lo;
    seg.fp = fp;
    seg.jit_depth = js->jit_active_depth;
    seg.entry_vm_fp = 0;
    js->vm_segs = &seg;
    assert((uintptr_t)&words[0] >= seg.lo && (uintptr_t)&words[STALE_WORDS] <= seg.hi);
  }

  plant(words);
  scrub_below();
  gc_run(js);

  if (in_interp_frame) js->vm_segs = seg.prev;
}

int main(void) {
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, __builtin_frame_address(0));

  run(js,
    "globalThis.use = function use(o) { return o.get() + o.base; };"
    "(function () {"
    "  const proto = { base: 1, get() { return 2; } };"
    "  let total = 0;"
    "  for (let k = 0; k < 40; k++) {"
    "    const o = Object.create(proto);"
    "    for (let i = 0; i < 2000; i++) total += use(o);"
    "  }"
    "  if (total !== 240000) throw new Error('total ' + total);"
    "})();");

  sv_func_t *func = compiled_function(js);
  if (!func->jit_code) {
    puts("SKIP function was not compiled");
    return 0;
  }
  scrub_below();

  collect_over_stale_words(js, false);
  assert(((uint64_t)(uintptr_t)func->jit_code ^ MASK) == g_masked);

  collect_over_stale_words(js, true);
  assert(!func->jit_code);

  puts("PASS stale code addresses in an interpreter frame do not keep compiled code");
  return 0;
}
#else
int main(void) { puts("SKIP stack segments not tracked on this target"); return 0; }
#endif
