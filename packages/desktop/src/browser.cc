#include "desktop.h"

#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_library_loader.h"
#include "include/cef_request_context.h"
#include "include/wrapper/cef_helpers.h"

namespace desktop {

Versions versions = {"", ANT_DESKTOP_VERSION, ANT_DESKTOP_CHROMIUM_VERSION};

std::string Owned(char *value) {
  std::string result = value ? value : "";
  free(value);
  return result;
}

bool DevelopmentMode() {
  return getenv("ANT_DESKTOP_DEV") != nullptr;
}

namespace {

class Window;
using IpcCallback = CefMessageRouterBrowserSide::Callback;

CefRefPtr<CefMessageRouterBrowserSide> router;

std::map<int64_t, CefRefPtr<Window>> windows;
std::atomic<int64_t> work_deadline{-1};
int64_t next_window = 1;
int browser_count;
bool terminating;
bool initialized;

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Window final : public CefClient,
                     public CefContextMenuHandler,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler,
                     public CefDragHandler,
                     public CefPermissionHandler,
                     public CefRequestHandler {
public:
  Window(int64_t id, WindowConfig config) : id_(id), config_(std::move(config)) {}

  int64_t Id() const { return id_; }
  bool Owns(CefRefPtr<CefBrowser> browser) const { return IsMain(browser); }

  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefDragHandler> GetDragHandler() override { return this; }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  bool Load(const std::string &url, const std::string &root, const char **error) {
    if (running_) return Fail(error, "this BrowserWindow already has Chromium content");
    void *parent = AntDesktop_Window_ContentView(id_);
    double bounds[4];
    if (!parent || !AntDesktop_Window_Bounds(id_, bounds)) return Fail(error, "BrowserWindow is closed");

    CefRefPtr<CefDictionaryValue> names = CefDictionaryValue::Create();
    names->SetString("ant", versions.ant);
    names->SetString("desktop", versions.desktop);
    names->SetString("chrome", versions.chrome);
    CefRefPtr<CefDictionaryValue> extra = CefDictionaryValue::Create();
    extra->SetDictionary("versions", names);
    extra->SetString("capabilities", config_.capabilities);
    extra->SetBool("sandbox", config_.sandbox);
    extra->SetBool("integration", config_.integration);
    extra->SetBool("contextIsolation", config_.context_isolation);
    if (!config_.preload.empty()) {
      std::ifstream input(config_.preload, std::ios::binary);
      std::ostringstream contents;
      contents << input.rdbuf();
      std::string source = contents.str();
      if (!input || !RewriteImports(&source, "__antPreload.require")) return Fail(error, "preload could not be loaded");
      extra->SetString("preloadPath", config_.preload);
      extra->SetString("preloadSource", source);
    }

    root_ = root;
    if (!root_.empty()) SetAppRoot(0, root_, config_.integration);
    CefWindowInfo info;
    info.SetAsChild(parent, CefRect(0, 0, static_cast<int>(bounds[2]), static_cast<int>(bounds[3])));
    CefBrowserSettings settings;
    settings.background_color = config_.transparent ? CefColorSetARGB(0, 0, 0, 0) : CefColorSetARGB(255, 8, 10, 18);
    running_ = true;
    if (!CefBrowserHost::CreateBrowser(info, this, url, settings, extra, CefRequestContext::GetGlobalContext())) {
      running_ = false;
      return Fail(error, "failed to create embedded Chromium view");
    }
    browser_count++;
    return true;
  }

  void Close() {
    if (!running_) return AntDesktop_Window_Close(id_);
    close_requested_ = true;
    if (!browser_) return;
    CefRefPtr<CefBrowserHost> host = browser_->GetHost();
    if (devtools_ || host->HasDevTools()) {
      close_after_devtools_ = true;
      if (devtools_) devtools_->GetHost()->CloseBrowser(true);
      else host->CloseDevTools();
      return;
    }
    host->CloseBrowser(false);
  }

  bool ShouldClose() {
    if (!running_ || closing_) return true;
    Close();
    return false;
  }

  void NativeClosed() {
    EmitWindowEvent(id_, "closed");
    native_closed_ = true;
    if (!running_) Destroy();
  }

  bool Command(const std::string &command, int x, int y, bool *result) {
    if (command == "isDevToolsOpened") {
      *result = devtools_open_;
      return true;
    }
    if (!running_) return false;
    if (!browser_) return true;
    CefRefPtr<CefBrowserHost> host = browser_->GetHost();
    bool open = command == "openDevTools" || command == "inspect" || (command == "toggleDevTools" && !host->HasDevTools());
    if (command == "reload") {
      browser_->Reload();
    } else if (open) {
      if (DevelopmentMode()) host->ShowDevTools(CefWindowInfo(), nullptr, CefBrowserSettings(), CefPoint(x, y));
    } else if (command == "closeDevTools" || command == "toggleDevTools") {
      host->CloseDevTools();
      SetDevToolsOpen(false);
    } else {
      return false;
    }
    return true;
  }

  bool Send(int64_t query, const void *data, size_t size) {
    if (query == 0) {
      if (events_) events_->Success(data, size);
      return browser_ != nullptr;
    }
    auto found = queries_.find(query);
    if (found == queries_.end()) return false;
    found->second->Success(data, size);
    queries_.erase(found);
    return true;
  }

  void Query(int64_t query, CefRefPtr<const CefBinaryBuffer> request, bool persistent, CefRefPtr<IpcCallback> callback) {
    if (persistent) {
      events_ = callback;
      return;
    }
    queries_[query] = callback;
    EmitIpc(id_, query, request->GetData(), request->GetSize());
  }

  void CancelQuery(int64_t query) {
    queries_.erase(query);
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                                CefRefPtr<CefProcessMessage> message) override {
    return IsMain(browser) && router->OnProcessMessageReceived(browser, frame, source, message);
  }

  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool gesture,
                      bool redirect) override {
    if (!IsMain(browser)) return false;
    if (frame->IsMain()) events_ = nullptr;
    router->OnBeforeBrowse(browser, frame);
    return false;
  }

  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int code,
                                 const CefString &error) override {
    if (!IsMain(browser)) return;
    events_ = nullptr;
    queries_.clear();
    router->OnRenderProcessTerminated(browser);
  }

  void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefContextMenuParams> params,
                           CefRefPtr<CefMenuModel> model) override {
    model->Clear();
  }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    if (browser_) {
      if (!DevelopmentMode()) return browser->GetHost()->CloseBrowser(true);
      devtools_ = browser;
      return SetDevToolsOpen(true);
    }
    browser_ = browser;
    if (!root_.empty()) SetAppRoot(browser->GetIdentifier(), root_, config_.integration);
    AntDesktop_Window_Attach(id_, browser->GetHost()->GetWindowHandle());
    AntDesktop_Window_Perform(id_, "ready", "", false);
    EmitLoad(id_);
    if (close_requested_) Close();
  }

  void OnBeforeDevToolsPopup(CefRefPtr<CefBrowser> browser, CefWindowInfo &info, CefRefPtr<CefClient> &client,
                             CefBrowserSettings &settings, CefRefPtr<CefDictionaryValue> &extra,
                             bool *use_default_window) override {
    client = this;
    if (!DevelopmentMode()) return;
    *use_default_window = true;
    SetDevToolsOpen(true);
  }

  bool DoClose(CefRefPtr<CefBrowser> browser) override {
    if (!browser_ || !browser_->IsSame(browser)) return false;
    closing_ = true;
    if (!native_closed_) AntDesktop_Window_Close(id_);
    return false;
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    if (devtools_ && devtools_->IsSame(browser)) {
      devtools_ = nullptr;
      SetDevToolsOpen(false);
      if (close_after_devtools_ && browser_) browser_->GetHost()->CloseBrowser(false);
      close_after_devtools_ = false;
      return;
    }
    if (!browser_ || !browser_->IsSame(browser)) return;
    router->OnBeforeClose(browser);
    events_ = nullptr;
    queries_.clear();
    ClearAppRoot(browser->GetIdentifier());
    browser_ = nullptr;
    running_ = false;
    browser_count--;
    if (native_closed_) Destroy();
    else if (terminating && browser_count == 0) AntDesktop_App_Stop();
  }

  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool loading, bool back, bool forward) override {
    if (IsMain(browser)) EmitWindowEvent(id_, loading ? "loading" : "ready", browser->GetMainFrame()->GetURL());
  }

  void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition) override {
    if (IsMain(browser) && frame->IsMain()) EmitWindowEvent(id_, "navigation-start", frame->GetURL());
  }

  void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int status) override {
    if (IsMain(browser) && frame->IsMain()) EmitWindowEvent(id_, "navigation-commit", frame->GetURL(), status);
  }

  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode code, const CefString &text,
                   const CefString &url) override {
    if (IsMain(browser) && code != ERR_ABORTED) EmitWindowEvent(id_, "navigation-error", url, code);
  }

  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString &title) override {
    if (IsMain(browser)) EmitWindowEvent(id_, "title", title);
  }

  bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser, uint64_t prompt, const CefString &origin, uint32_t permissions,
                              CefRefPtr<CefPermissionPromptCallback> callback) override {
    EmitWindowEvent(id_, "permission-request", origin, permissions);
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }

  bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString &origin,
                                      uint32_t permissions, CefRefPtr<CefMediaAccessCallback> callback) override {
    EmitWindowEvent(id_, "permission-request", origin, permissions);
    callback->Cancel();
    return true;
  }

  void OnDraggableRegionsChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                 const std::vector<CefDraggableRegion> &regions) override {
    std::vector<AntDesktopRegion> values;
    for (const CefDraggableRegion &region : regions) {
      const CefRect &bounds = region.bounds;
      values.push_back({double(bounds.x), double(bounds.y), double(bounds.width), double(bounds.height), region.draggable != 0});
    }
    AntDesktop_Window_SetRegions(id_, values.data(), values.size());
  }

private:
  static bool Fail(const char **error, const char *message) {
    *error = message;
    return false;
  }

  bool IsMain(CefRefPtr<CefBrowser> browser) const {
    return browser_ && browser_->IsSame(browser);
  }

  void SetDevToolsOpen(bool open) {
    if (devtools_open_ == open) return;
    devtools_open_ = open;
    EmitWindowEvent(id_, open ? "devtools-opened" : "devtools-closed");
  }

  void Destroy() {
    CefRefPtr<Window> self = this;
    windows.erase(id_);
    EmitDestroyed(id_);
    if (!windows.empty()) return;
    if (terminating) AntDesktop_App_Stop();
    else AntDesktop_App_Terminate();
  }

  const int64_t id_;
  const WindowConfig config_;
  std::string root_;
  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<CefBrowser> devtools_;
  CefRefPtr<IpcCallback> events_;
  std::map<int64_t, CefRefPtr<IpcCallback>> queries_;
  bool running_ = false;
  bool close_requested_ = false;
  bool closing_ = false;
  bool native_closed_ = false;
  bool close_after_devtools_ = false;
  bool devtools_open_ = false;
  IMPLEMENT_REFCOUNTING(Window);
};

class BrowserApp final : public CefApp, public CefBrowserProcessHandler {
public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override { RegisterSchemes(registrar); }
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  void OnContextInitialized() override { RegisterAppScheme(); }

  void OnBeforeCommandLineProcessing(const CefString &process_type, CefRefPtr<CefCommandLine> command_line) override {
    if (!process_type.empty()) return;
    command_line->AppendSwitchWithValue("disable-features", "NativeNotifications,SystemNotifications,NewMacNotificationAPI");
    command_line->AppendSwitch("disable-notifications");
    command_line->AppendSwitch("use-mock-keychain");
    command_line->AppendSwitchWithValue("lang", "en-US");
    if (!DevelopmentMode()) command_line->AppendSwitch("disable-dev-tools");
  }

  void OnScheduleMessagePumpWork(int64_t delay) override {
    int64_t deadline = Now() + std::max<int64_t>(0, delay);
    int64_t previous = work_deadline.load();
    while ((previous < 0 || deadline < previous) && !work_deadline.compare_exchange_weak(previous, deadline)) {}
    CFRunLoopWakeUp(CFRunLoopGetMain());
  }

private:
  IMPLEMENT_REFCOUNTING(BrowserApp);
};

class IpcHandler final : public CefMessageRouterBrowserSide::Handler {
public:
  bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query, CefRefPtr<const CefBinaryBuffer> request,
               bool persistent, CefRefPtr<Callback> callback) override {
    CefRefPtr<Window> window = frame->IsMain() ? Owner(browser) : nullptr;
    if (window) window->Query(query, request, persistent, callback);
    return window != nullptr;
  }

  void OnQueryCanceled(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query) override {
    if (CefRefPtr<Window> window = Owner(browser)) window->CancelQuery(query);
  }

private:
  static CefRefPtr<Window> Owner(CefRefPtr<CefBrowser> browser) {
    for (const auto &entry : windows) {
      if (entry.second->Owns(browser)) return entry.second;
    }
    return nullptr;
  }
};

IpcHandler ipc_handler;

CefRefPtr<Window> Find(int64_t id) {
  auto found = windows.find(id);
  return found == windows.end() ? nullptr : found->second;
}

std::vector<CefRefPtr<Window>> AllWindows() {
  std::vector<CefRefPtr<Window>> values;
  for (const auto &entry : windows) values.push_back(entry.second);
  return values;
}

}

bool InitializeBrowser(int argc, char **argv) {
  std::string framework =
    Owned(AntDesktop_CEF_Resolve(ANT_DESKTOP_CEF_VERSION, ANT_DESKTOP_CEF_ARCHIVE, ANT_DESKTOP_CEF_SHA1, ANT_DESKTOP_CEF_SIZE));
  if (framework.empty()) return false;
  if (!cef_load_library((framework + "/Chromium Embedded Framework").c_str())) return false;
  setenv("ANT_DESKTOP_CEF_FRAMEWORK", framework.c_str(), 1);

  std::string bundle = Owned(AntDesktop_App_Path("bundle"));
  std::string helper = bundle + "/Contents/Frameworks/Ant Desktop Helper.app/Contents/MacOS/Ant Desktop Helper";
  std::string cache = Owned(AntDesktop_App_Path("userData")) + "/Chromium";
  std::error_code error;
  std::filesystem::create_directories(cache, error);
  if (access(helper.c_str(), X_OK) != 0) {
    fprintf(stderr, "missing helper application: %s\n", helper.c_str());
    return false;
  }

  CefSettings settings;
  settings.external_message_pump = true;
  settings.no_sandbox = true;
  CefString(&settings.browser_subprocess_path) = helper;
  CefString(&settings.framework_dir_path) = framework;
  CefString(&settings.main_bundle_path) = bundle;
  CefString(&settings.resources_dir_path) = framework + "/Resources";
  CefString(&settings.locales_dir_path) = framework + "/Resources/locales";
  CefString(&settings.root_cache_path) = cache;

  initialized = CefInitialize(CefMainArgs(argc, argv), settings, new BrowserApp, nullptr);
  if (!initialized) return false;
  router = CefMessageRouterBrowserSide::Create(IpcRouterConfig());
  router->AddHandler(&ipc_handler, true);
  return true;
}

void ShutdownBrowser() {
  if (!initialized) return;
  CloseAllWindows();
  for (int attempt = 0; browser_count > 0 && attempt < 400; attempt++) {
    CefDoMessageLoopWork();
    usleep(5000);
  }
  windows.clear();
  router = nullptr;
  CefShutdown();
  cef_unload_library();
  initialized = false;
}

void DoBrowserWork() {
  if (!initialized) return;
  work_deadline.store(-1);
  CefDoMessageLoopWork();
}

int64_t NextBrowserWorkDelay() {
  int64_t deadline = work_deadline.load();
  int64_t delay = deadline < 0 ? -1 : std::max<int64_t>(0, deadline - Now());
  if (terminating && browser_count > 0 && (delay < 0 || delay > 10)) return 10;
  return delay;
}

int64_t CreateWindow(const char *options, WindowConfig config) {
  int64_t id = next_window++;
  if (!AntDesktop_Window_Create(id, options)) return 0;
  windows[id] = new Window(id, std::move(config));
  return id;
}

bool LoadWindow(int64_t id, const std::string &url, const std::string &root, const char **error) {
  CefRefPtr<Window> window = Find(id);
  if (window) return window->Load(url, root, error);
  *error = "BrowserWindow is closed";
  return false;
}

bool BrowserCommand(int64_t id, const std::string &command, int x, int y, bool *result) {
  CefRefPtr<Window> window = Find(id);
  return window && window->Command(command, x, y, result);
}

bool SendIpc(int64_t id, int64_t query, const void *data, size_t size) {
  CefRefPtr<Window> window = Find(id);
  return window && window->Send(query, data, size);
}

void CloseWindow(int64_t id) {
  if (CefRefPtr<Window> window = Find(id)) window->Close();
}

void CloseAllWindows() {
  for (const CefRefPtr<Window> &window : AllWindows()) window->Close();
}

void ReloadAllWindows() {
  bool result;
  for (const CefRefPtr<Window> &window : AllWindows()) window->Command("reload", 0, 0, &result);
}

void Quit() {
  AntDesktop_App_Terminate();
}

}

void AntDesktop_OnWindowEvent(int64_t window, const char *type) {
  desktop::EmitWindowEvent(window, type);
}

bool AntDesktop_OnWindowShouldClose(int64_t id) {
  CefRefPtr<desktop::Window> window = desktop::Find(id);
  return !window || window->ShouldClose();
}

void AntDesktop_OnWindowClosed(int64_t id) {
  if (CefRefPtr<desktop::Window> window = desktop::Find(id)) window->NativeClosed();
}

void AntDesktop_OnMenuClick(int64_t item) {
  desktop::EmitMenuClick(item);
}

void AntDesktop_OnTerminate(void) {
  using namespace desktop;
  if (terminating) return;
  terminating = true;
  for (const auto &entry : AllWindows()) EmitWindowEvent(entry->Id(), "quit");
  if (windows.empty()) return AntDesktop_App_Stop();
  CloseAllWindows();
}
