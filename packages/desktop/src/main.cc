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
  int64_t delay = uv_backend_timeout(uv_default_loop());
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

  int descriptor = uv_backend_fd(uv_default_loop());
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

bool ImportEntry(ant_t *js, const std::string &entry) {
  char *resolved = realpath(entry.c_str(), nullptr);
  if (!resolved) {
    fprintf(stderr, "desktop entry does not exist: %s\n", entry.c_str());
    return false;
  }
  js_set_filename(js, resolved);
  js_setup_import_meta(js, resolved);
  ant_value_t imported = js_esm_import_sync(js, js_esm_make_file_url(js, resolved));
  if (!is_err(imported) && !Ant_Exception_Pending(js)) return true;
  if (!is_err(imported)) imported = Ant_Exception_Current(js);
  fprintf(stderr, "desktop entry import failed (%s): %s\n", resolved, ErrorDetail(js, imported).c_str());
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
      code = 0;
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
