#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"
#include "renderer_app.h"

#if ANT_DESKTOP_RUNTIME_INTEGRATION
#include "renderer_ant_runtime.h"
#endif

#if defined(CEF_USE_SANDBOX)
#include "include/cef_sandbox_mac.h"
#endif

int main(int argc, char *argv[]) {
#if ANT_DESKTOP_RUNTIME_INTEGRATION
  volatile char stack_base;
  ant_renderer_runtime_set_stack_base((void *)&stack_base);
#endif
#if defined(CEF_USE_SANDBOX)
  CefScopedSandboxContext sandbox_context;
  if (!sandbox_context.Initialize(argc, argv)) return 1;
#endif
  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInHelper()) return 1;
  CefMainArgs main_args(argc, argv);
  int exit_code = CefExecuteProcess(main_args, CreateAntRendererApp(), nullptr);
#if ANT_DESKTOP_RUNTIME_INTEGRATION
  ant_renderer_runtime_shutdown();
#endif
  return exit_code;
}
