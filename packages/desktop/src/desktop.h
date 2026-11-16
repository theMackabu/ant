#pragma once

#include <cstdint>
#include <string>

#include "engine.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_message_router.h"

#include "bridge.h"

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

bool ExtractArchive(const std::string &archive, const std::string &destination, std::string *error);

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

}
