#include "desktop.hh"
#include <vector>

namespace desktop {

static ant_t *runtime;
static bool pumping;

static void Pump() {
  if (pumping) return;
  pumping = true;
  
  DoBrowserWork();
  int64_t delay = PumpRuntime(runtime);
  int64_t browser_delay = NextBrowserWorkDelay();
  
  if (browser_delay >= 0 && (delay < 0 || browser_delay < delay)) delay = browser_delay;
  platform::ScheduleLoop(delay);
  pumping = false;
}

static int RunApplication(int argc, char **argv, void *stack_base) {
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

  runtime = StartRuntime(int(arguments.size()) - 1, arguments.data(), stack_base);
  if (!runtime) {
    fprintf(stderr, "failed to create Ant runtime\n");
    ShutdownBrowser();
    return 1;
  }

  int code = 1;
  if (!InstallModule(runtime)) fprintf(stderr, "failed to install the ant:desktop module\n");
  else {
    platform::StartLoop(Pump, RuntimeBackend(runtime));
    if (DevelopmentMode()) platform::OnReloadSignal(ReloadAllWindows);
    
    AntDesktop_App_FinishLaunching();
    if (ImportEntry(runtime, entry)) {
      Pump();
      AntDesktop_App_Run();
      code = EntryFailed() ? 1 : 0;
    }
  }

  ShutdownBrowser();
  StopRuntime(runtime);
  
  return code;
}

}

extern "C" ANT_DESKTOP_EXPORT int AntDesktop_Main(int argc, char **argv, void *stack_base) {
  for (int index = 1; index < argc; index++)
    if (strncmp(argv[index], "--type=", 7) == 0) return desktop::RunHelper(argc, argv, stack_base);
  return desktop::RunApplication(argc, argv, stack_base);
}
