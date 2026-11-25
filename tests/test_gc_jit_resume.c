// meson test -C build gc-jit-resume
#include "internal.h"
#include "cfunc.h"
#include "gc.h"
#include "silver/glue.h"
#include <assert.h>
#include <stdio.h>

#if defined(__aarch64__) || defined(__x86_64__)
static bool finalized;

static void finalize(ant_t *js, ant_object_t *obj) {
  (void)js;
  (void)obj;
  finalized = true;
}

__attribute__((noinline)) static void leave_dead_references(ant_t *js) {
  ant_value_t target = Ant_Object_CreateBare(js);
  js_set_finalizer(target, finalize);
  ant_object_t *volatile words[64];
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) words[i] = js_obj_ptr(target);
}

static ant_value_t collect_on_resume(ant_params_t) {
  assert(!finalized);
  gc_run(js);
  return finalized ? Ant_Value_True : Ant_Value_False;
}

int main(void) {
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, __builtin_frame_address(0));
  js_set(js, js->global, "collectOnResume", Ant_Function_Create(collect_on_resume));
  const char source[] = "globalThis.afterBailout = () => collectOnResume();";
  ant_value_t compiled = js_eval_bytecode(js, source, sizeof(source) - 1);
  assert(!is_err(compiled));
  sv_closure_t *closure = js_func_closure(js_get(js, js->global, "afterBailout"));

  // Leave raw pointers where the next call will build its resume frame.
  // The resumed bytecode collects while that native frame is still live.
  leave_dead_references(js);
  ant_value_t result = jit_resume_in_interpreter(
    js->vm, closure, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined,
    nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0, false, nullptr);
  assert(result == Ant_Value_True && "bailout resume retained a dead object");
  Ant_Destroy(js);
  puts("PASS collection during bailout resume releases the unreachable object");
}
#else
int main(void) { puts("SKIP native stack residue test requires arm64 or x86-64"); }
#endif
