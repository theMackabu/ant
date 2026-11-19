#include "views/system.hh"

#include <windows.h>

#include <shlobj.h>

#include <thread>

namespace desktop::system {

const char kBrowserLibrary[] = "libcef.dll";
const char kDesktopLibrary[] = "antdesktop.dll";

static std::wstring Wide(const std::string &value) {
  int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
  std::wstring result(size_t(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), length);
  return result;
}

static std::string Narrow(const wchar_t *value) {
  int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  std::string result(size_t(length > 0 ? length - 1 : 0), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
  return result;
}

static std::string KnownFolder(REFKNOWNFOLDERID folder) {
  PWSTR path = nullptr;
  std::string result = SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &path)) ? Narrow(path) : "";
  CoTaskMemFree(path);
  return result;
}

static std::wstring Quote(const std::string &argument) {
  std::wstring quoted = L"\"";
  size_t slashes = 0;
  for (wchar_t unit : Wide(argument)) {
    if (unit == L'"') quoted.append(slashes + 1, L'\\');
    slashes = unit == L'\\' ? slashes + 1 : 0;
    quoted.push_back(unit);
  }
  quoted.append(slashes, L'\\');
  return quoted + L"\"";
}

std::string ExecutablePath() {
  wchar_t buffer[32768];
  DWORD length = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
  return length > 0 && length < std::size(buffer) ? Narrow(buffer) : "";
}

std::string Path(const std::string &name) {
  if (name == "home") return KnownFolder(FOLDERID_Profile);
  if (name == "appData") return KnownFolder(FOLDERID_RoamingAppData);
  if (name == "desktop") return KnownFolder(FOLDERID_Desktop);
  if (name == "documents") return KnownFolder(FOLDERID_Documents);
  if (name == "downloads") return KnownFolder(FOLDERID_Downloads);
  if (name != "temp") return "";
  wchar_t buffer[MAX_PATH + 1];
  DWORD length = GetTempPathW(DWORD(std::size(buffer)), buffer);
  return length > 0 && length < std::size(buffer) ? Narrow(buffer) : "";
}

std::string SharedRuntimeDirectory() {
  return KnownFolder(FOLDERID_LocalAppData) + "\\Ant Desktop\\CEF";
}

void SetEnvironment(const char *name, const std::string &value) {
  SetEnvironmentVariableW(Wide(name).c_str(), Wide(value).c_str());
}

void *OpenLibrary(const std::string &path) {
  return LoadLibraryExW(Wide(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void *FindSymbol(void *library, const char *name) {
  return reinterpret_cast<void *>(GetProcAddress(HMODULE(library), name));
}

int Run(const std::vector<std::string> &command, bool visible) {
  std::wstring line;
  for (const std::string &argument : command) line += (line.empty() ? L"" : L" ") + Quote(argument);
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process;
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, visible ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return -1;
  }
  DWORD code = DWORD(-1);
  WaitForSingleObject(process.hProcess, INFINITE);
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return int(code);
}

void WaitBackend(intptr_t backend) {
  DWORD bytes;
  ULONG_PTR key;
  OVERLAPPED *overlapped = nullptr;
  GetQueuedCompletionStatus(HANDLE(backend), &bytes, &key, &overlapped, INFINITE);
  if (overlapped) PostQueuedCompletionStatus(HANDLE(backend), bytes, key, overlapped);
}

static std::wstring ReloadEvent(int64_t process) {
  return L"Local\\ant-desktop-reload-" + std::to_wstring(process);
}

void OnReloadSignal(void (*reload)()) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, ReloadEvent(GetCurrentProcessId()).c_str());
  if (!event) return;
  std::thread([event, reload] {
    while (WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0) reload();
  }).detach();
}

bool SignalReload(int64_t process) {
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, ReloadEvent(process).c_str());
  bool signaled = event && SetEvent(event);
  if (event) CloseHandle(event);
  return signaled;
}

void StyleWindow(void *handle, double opacity, bool protect) {
  HWND window = HWND(handle);
  if (opacity < 1) {
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(window, 0, BYTE(255 * (opacity < 0 ? 0 : opacity)), LWA_ALPHA);
  }
  if (protect) SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
}

}
