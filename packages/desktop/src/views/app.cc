#include "views/views.hh"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <semaphore>
#include <thread>

#include "include/base/cef_callback.h"
#include "include/wrapper/cef_closure_task.h"

namespace desktop::views {

std::string ApplicationDirectory() {
  return std::filesystem::path(system::ExecutablePath()).parent_path().string();
}

std::string ApplicationInfo(const std::string &key) {
  std::ifstream input(std::filesystem::path(ApplicationDirectory()) / "resources" / "app.info");
  for (std::string line; std::getline(input, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with(key + "=")) return line.substr(key.size() + 1);
  }
  return "";
}

std::string ApplicationName() {
  std::string name = ApplicationInfo("AntDesktopName");
  return name.empty() ? "Ant Desktop" : name;
}

}

namespace desktop::platform {

static void (*pump)();
static void (*reload)();
static std::atomic<uint64_t> timer;
static std::atomic<bool> watching;
static std::binary_semaphore idle{0};

static void RunTimer(uint64_t scheduled) {
  if (scheduled == timer.load()) pump();
}

static void Watch(intptr_t backend) {
  for (;;) {
    idle.acquire();
    system::WaitBackend(backend);
    watching.store(false);
    if (!CefPostTask(TID_UI, base::BindOnce(pump))) return;
  }
}

void StartLoop(void (*callback)(), intptr_t backend) {
  pump = callback;
  std::thread(Watch, backend).detach();
}

void ScheduleLoop(int64_t delay) {
  uint64_t scheduled = ++timer;
  if (delay >= 0) CefPostDelayedTask(TID_UI, base::BindOnce(RunTimer, scheduled), delay);
  if (!watching.exchange(true)) idle.release();
}

void WakeLoop() {
  CefPostTask(TID_UI, base::BindOnce(pump));
}

void OnReloadSignal(void (*callback)()) {
  reload = callback;
  system::OnReloadSignal([] { CefPostTask(TID_UI, base::BindOnce(reload)); });
}

bool ConfigureBrowser(CefSettings &settings) {
  const char *runtime = getenv("ANT_DESKTOP_CEF_DIR");
  if (!runtime) return false;
  std::filesystem::path directory = runtime;
  settings.external_message_pump = false;
  CefString(&settings.resources_dir_path) = directory.string();
  CefString(&settings.locales_dir_path) = (directory / "locales").string();
  return true;
}

bool LoadHelperBrowser(int argc, char **argv) {
  return true;
}

void ReleaseBrowser() {}

}

using namespace desktop;

void AntDesktop_App_Initialize(void) {}

void AntDesktop_App_FinishLaunching(void) {}

void AntDesktop_App_Run(void) {
  CefRunMessageLoop();
}

void AntDesktop_App_Stop(void) {
  CefQuitMessageLoop();
}

void AntDesktop_App_Terminate(void) {
  AntDesktop_OnTerminate();
}

char *AntDesktop_App_Info(const char *key) {
  std::string value = views::ApplicationInfo(key);
  return value.empty() ? nullptr : strdup(value.c_str());
}

char *AntDesktop_App_Path(const char *key) {
  std::string name = key;
  std::filesystem::path application = views::ApplicationDirectory();
  std::string path = name == "resources"  ? (application / "resources").string()
                     : name == "exe"      ? system::ExecutablePath()
                     : name == "bundle"   ? application.string()
                     : name == "userData" ? (std::filesystem::path(system::Path("appData")) / views::ApplicationName()).string()
                                          : system::Path(name);
  return path.empty() ? nullptr : strdup(path.c_str());
}

void AntDesktop_Menu_Set(const char *json) {
  views::SetMenu(json);
}
