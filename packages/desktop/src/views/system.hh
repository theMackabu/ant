#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace desktop::system {

extern const char kBrowserLibrary[];
extern const char kDesktopLibrary[];

std::string ExecutablePath();
std::string Path(const std::string &name);
std::string SharedRuntimeDirectory();
void SetEnvironment(const char *name, const std::string &value);

void *OpenLibrary(const std::string &path);
void *FindSymbol(void *library, const char *name);
int Run(const std::vector<std::string> &command, bool visible);

void WaitBackend(intptr_t backend);
void OnReloadSignal(void (*reload)());
bool SignalReload(int64_t process);
void StyleWindow(void *handle, double opacity, bool protect);

}
