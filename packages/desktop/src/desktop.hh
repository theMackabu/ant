#pragma once

#include <cstdint>
#include <string>

#include "ant/engine.h"      // IWYU pragma: export
#include "platform.hh"       // IWYU pragma: export
#include "include/cef_app.h" // IWYU pragma: export
#include "include/wrapper/cef_message_router.h"

namespace desktop {

struct WindowConfig {
  std::string capabilities;
  std::string preload;
  bool sandbox = true;
  bool integration = false;
  bool context_isolation = true;
  bool transparent = false;
};

struct Versions {
  std::string ant;
  std::string desktop;
  std::string chrome;
};

extern Versions versions;

std::string Owned(char *value);
bool DevelopmentMode();

ant_t *StartRuntime(int argc, char **argv, void *stack_base);
void StopRuntime(ant_t *js);
int64_t PumpRuntime(ant_t *js);
int RuntimeDescriptor(ant_t *js);
bool ImportEntry(ant_t *js, const std::string &entry);
bool EntryFailed();

bool InstallModule(ant_t *js);
void EmitWindowEvent(int64_t window, const char *type, const std::string &detail = {}, int64_t code = 0);
void EmitIpc(int64_t window, int64_t query, const void *data, size_t size);
void EmitLoad(int64_t window);
void EmitDestroyed(int64_t window);
void EmitMenuClick(int64_t item);

bool InitializeBrowser(int argc, char **argv);
void ShutdownBrowser();
void DoBrowserWork();
int64_t NextBrowserWorkDelay();

int64_t CreateWindow(const char *options, WindowConfig config);
bool LoadWindow(int64_t window, const std::string &url, const std::string &root, const char **error);
bool BrowserCommand(int64_t window, const std::string &command, int x, int y, bool *result);
bool SendIpc(int64_t window, int64_t query, const void *data, size_t size);
void CloseWindow(int64_t window);
void CloseAllWindows();
void ReloadAllWindows();
void Quit();

void RegisterSchemes(CefRawPtr<CefSchemeRegistrar> registrar);
void RegisterAppScheme();
void SetAppRoot(int browser, const std::string &root, bool integration);
void ClearAppRoot(int browser);
bool RewriteImports(std::string *source, const std::string &require);

CefMessageRouterConfig IpcRouterConfig();
int RunHelper(int argc, char **argv, void *stack_base);
std::string RendererBridgeSource();

void SetIntegrationStackBase(void *base);
void ShutdownIntegration();
std::string DescribeModule(const std::string &specifier);
std::string CallModule(const std::string &specifier, const std::string &name, const std::string &arguments);

namespace platform {
  void StartLoop(void (*pump)(), int descriptor);
  void ScheduleLoop(int64_t delay);
  void WakeLoop();
  void OnReloadSignal(void (*reload)());
  
  bool ConfigureBrowser(CefSettings &settings);
  bool LoadHelperBrowser(int argc, char **argv);
  void ReleaseBrowser();
}

}
