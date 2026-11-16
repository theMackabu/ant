#include "desktop.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <signal.h>

#include <vector>

namespace desktop {

namespace {

ant_t *runtime;
CFRunLoopTimerRef pump_timer;
bool pumping;

void Pump() {
  if (pumping) return;
  pumping = true;
  DoBrowserWork();
  js_reactor_pump_repl_nowait(runtime);
  int64_t delay = uv_backend_timeout(runtime->loop);
  int64_t browser_delay = NextBrowserWorkDelay();
  if (browser_delay >= 0 && (delay < 0 || browser_delay < delay)) delay = browser_delay;
  CFRunLoopTimerSetNextFireDate(pump_timer, CFAbsoluteTimeGetCurrent() + (delay < 0 ? 3600.0 : double(delay) / 1000.0));
  pumping = false;
}

void StartPump() {
  CFRunLoopRef loop = CFRunLoopGetMain();
  pump_timer = CFRunLoopTimerCreateWithHandler(nullptr, CFAbsoluteTimeGetCurrent() + 3600.0, 0, 0, 0, ^(CFRunLoopTimerRef) { Pump(); });
  CFRunLoopAddTimer(loop, pump_timer, kCFRunLoopCommonModes);
  CFRunLoopObserverRef observer =
    CFRunLoopObserverCreateWithHandler(nullptr, kCFRunLoopBeforeWaiting, true, 0, ^(CFRunLoopObserverRef, CFRunLoopActivity) { Pump(); });
  CFRunLoopAddObserver(loop, observer, kCFRunLoopCommonModes);

  int descriptor = uv_backend_fd(runtime->loop);
  if (descriptor < 0) return;
  dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, uintptr_t(descriptor), 0, dispatch_get_main_queue());
  dispatch_source_set_event_handler(source, ^{ Pump(); });
  dispatch_resume(source);
}

void WatchReloadSignal() {
  if (!DevelopmentMode()) return;
  signal(SIGUSR1, SIG_IGN);
  dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0, dispatch_get_main_queue());
  dispatch_source_set_event_handler(source, ^{ ReloadAllWindows(); });
  dispatch_resume(source);
}

std::string ErrorDetail(ant_t *js, ant_value_t error) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, error);
  ant_value_t detail = Ant_Exception_Stack(js, error);
  if (vtype(detail) != kTypeString) detail = Ant_Exception_Value(js, error);
  ant_value_t message;
  if (is_object_type(detail) && js_try_get_own_data_prop(js, detail, "message", 7, &message)) detail = message;
  std::string text = js_str(js, detail);
  GC_ROOT_RESTORE(js, mark);
  return text;
}

const char kImportSource[] = "(url, fail) => import(url).catch(fail)";
bool entry_failed;

ant_value_t EntryFailed(ant_params_t) {
  ant_value_t error = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t stack = is_object_type(error) ? js_get(js, error, "stack") : Ant_Value_Undefined;
  fprintf(stderr, "desktop entry failed: %s\n", js_str(js, vtype(stack) == kTypeString ? stack : error));
  entry_failed = true;
  Quit();
  return Ant_Value_Undefined;
}

bool ImportEntry(ant_t *js, const std::string &entry) {
  char *resolved = realpath(entry.c_str(), nullptr);
  if (!resolved) {
    fprintf(stderr, "desktop entry does not exist: %s\n", entry.c_str());
    return false;
  }
  ant_value_t args[] = {js_esm_make_file_url(js, resolved), Ant_Function_Create(EntryFailed)};
  free(resolved);

  ant_value_t import = js_eval_bytecode_eval(js, kImportSource, sizeof(kImportSource) - 1, true);
  ant_value_t started = is_err(import) ? import : sv_vm_call_explicit_this(js->vm, js, import, Ant_Value_Undefined, args, 2);
  if (!is_err(started)) return true;
  fprintf(stderr, "desktop entry import failed (%s): %s\n", entry.c_str(), ErrorDetail(js, started).c_str());
  return false;
}

int RunApplication(int argc, char **argv, void *stack_base) {
  AntDesktop_App_Initialize();

  std::string bundled = Owned(AntDesktop_App_Info("AntDesktopEntry"));
  if (bundled.empty() && argc < 2) {
    fprintf(stderr, "usage: ant-desktop <main.js> [args...]\n");
    return 64;
  }
  std::string entry = bundled.empty() ? argv[1] : Owned(AntDesktop_App_Path("resources")) + "/" + bundled;
  std::vector<char *> arguments = {entry.data()};
  for (int index = bundled.empty() ? 2 : 1; index < argc; index++) arguments.push_back(argv[index]);
  arguments.push_back(nullptr);

  if (!InitializeBrowser(argc, argv)) {
    fprintf(stderr, "failed to initialize embedded Chromium\n");
    return 1;
  }

  ant_t *js = Ant_Create(nullptr);
  if (!js) {
    fprintf(stderr, "failed to create Ant runtime\n");
    ShutdownBrowser();
    return 1;
  }
  js_setstackbase(js, stack_base);
  js_setstacklimit(js, os_thread_stack_size() * 3 / 4);
  ant_runtime_init(js, int(arguments.size()) - 1, arguments.data(), nullptr);
  ant_bootstrap_modules(js);
  runtime = js;

  int code = 1;
  if (!InstallModule(js)) {
    fprintf(stderr, "failed to install the ant:desktop module\n");
  } else {
    StartPump();
    WatchReloadSignal();
    AntDesktop_App_FinishLaunching();
    if (ImportEntry(js, entry)) {
      Pump();
      AntDesktop_App_Run();
      code = entry_failed ? 1 : 0;
    }
  }

  ShutdownBrowser();
  js->process_exiting = true;
  Ant_Destroy(js);
  return code;
}

}

}

extern "C" __attribute__((visibility("default"))) int AntDesktop_Main(int argc, char **argv, void *stack_base) {
  for (int index = 1; index < argc; index++) {
    if (strncmp(argv[index], "--type=", 7) == 0) return desktop::RunHelper(argc, argv, stack_base);
  }
  return desktop::RunApplication(argc, argv, stack_base);
}
