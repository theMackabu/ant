// meson test -C build desktop-error-details (compiled as Objective-C on macOS)
// Include the application entrypoint to exercise its actual private formatter.
// The unused application entrypoint is removed by the linker's dead stripping.
#include "runtime.h"
#include "esm/library.h"
#include "gc/roots.h"
#define main AntDesktopApplicationMain
#include "../packages/desktop/app/platform/mac/main.mm"
#undef main

#include <assert.h>

// Objective-C retains the entrypoint's class reference even after dead stripping.
// The diagnostic test must never start the application's event loop.
@implementation AntRuntimePump
- (instancetype)initWithRuntime:(ant_t *)js { abort(); }
- (void)pump { abort(); }
@end

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);
  GC_ROOT_SAVE(mark, js);
  ant_value_t error = Ant_Error_Create(js, JS_ERR_TYPE | JS_ERR_NO_STACK, "desktop diagnostic");
  GC_ROOT_PIN(js, error);
  ant_value_t record = Ant_Exception_Raise(js, error, js_mkundef());
  GC_ROOT_PIN(js, record);
  Ant_Exception_Clear(js);
  assert(strstr(RuntimeErrorDetail(js, record), "desktop diagnostic"));
  assert(!Ant_Exception_Pending(js));
  assert(strstr(RuntimeErrorDetail(js, js_mkstr(js, "ordinary diagnostic", 19)), "ordinary diagnostic"));

  ant_value_t stack = js_mkstr(js, "captured desktop stack", 22);
  record = Ant_Exception_Raise(js, error, stack);
  ant_value_t newer = Ant_Exception_Raise(js, js_mknum(73), js_mkundef());
  assert(strstr(RuntimeErrorDetail(js, record), "captured desktop stack"));
  assert(Ant_Exception_Peek(js) == newer);
  record = newer;
  Ant_Exception_Clear(js);
  assert(strcmp(RuntimeErrorDetail(js, record), "73") == 0);
  record = Ant_Exception_Raise(js, js_mkundef(), js_mkundef());
  Ant_Exception_Clear(js);
  assert(strcmp(RuntimeErrorDetail(js, record), "undefined") == 0);
  assert(!Ant_Exception_Pending(js));

  GC_ROOT_RESTORE(js, mark);
  js_destroy(js);
  puts("PASS desktop exception record diagnostics");
}
