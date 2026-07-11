#import <AppKit/AppKit.h>

#include "embedded_browser.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../../../browser/cef/app_scheme.h"
#include "../../../browser/cef/ipc.h"
#include "cef_callbacks.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_drag_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_permission_handler.h"
#include "include/cef_process_message.h"
#include "include/cef_request_context.h"
#include "include/cef_request_handler.h"
#include "include/wrapper/cef_helpers.h"

@interface AntDraggableRegionView : NSView
@property(nonatomic, copy) NSArray<NSDictionary *> *regions;
@end

@implementation AntDraggableRegionView

- (BOOL)isFlipped {
  return YES;
}

- (NSView *)hitTest:(NSPoint)point {
  if (self.hidden || self.alphaValue == 0) return nil;
  BOOL draggable = NO;
  for (NSDictionary *region in self.regions) {
    if (NSPointInRect(point, [region[@"bounds"] rectValue])) draggable = [region[@"draggable"] boolValue];
  }
  return draggable ? self : nil;
}

- (void)mouseDown:(NSEvent *)event {
  [self.window performWindowDragWithEvent:event];
}

@end

namespace {

class EmbeddedClient;
std::map<void *, CefRefPtr<EmbeddedClient>> g_clients;

void Emit(void *window, const char *type, int code, const CefString &detail = CefString()) {
  std::string value = detail.ToString();
  ant_desktop_cef_emit_event(window, type, value.data(), value.size(), code);
}

class EmbeddedClient final : public CefClient,
                             public CefLifeSpanHandler,
                             public CefLoadHandler,
                             public CefDisplayHandler,
                             public CefDragHandler,
                             public CefPermissionHandler,
                             public CefRequestHandler {
public:
  EmbeddedClient(void *window, NSView *parent) : window_(window), parent_(parent) {}

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override {
    return this;
  }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
    return this;
  }
  CefRefPtr<CefDragHandler> GetDragHandler() override {
    return this;
  }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
    return this;
  }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override {
    return this;
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    if (source_process != PID_RENDERER || message->GetName() != "ant.ipc" || !frame || !frame->IsMain()) return false;
    CefRefPtr<CefListValue> values = message->GetArgumentList();
    if (!values || values->GetSize() < 4) return true;
    std::string channel = values->GetString(2);
    std::string payload = values->GetString(3);
    ant_desktop_cef_dispatch_ipc(window_, values->GetInt(0), (uint64_t)values->GetDouble(1), channel.data(),
                                 channel.size(), payload.data(), payload.size());
    return true;
  }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    if (!browser_) {
      browser_ = browser;
      NSView *view = (__bridge NSView *)browser->GetHost()->GetWindowHandle();
      view.frame = parent_.bounds;
      view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      [parent_ addSubview:view];
      draggable_view_ = [[AntDraggableRegionView alloc] initWithFrame:parent_.bounds];
      draggable_view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      [parent_ addSubview:draggable_view_];
      [parent_.window makeFirstResponder:view];
      ant_desktop_cef_show_window(window_);
      ant_desktop_cef_resolve_load(window_);
    } else {
      devtools_ = browser;
      SetDevToolsOpen(true);
    }
  }

  void OnBeforeDevToolsPopup(CefRefPtr<CefBrowser> browser, CefWindowInfo &window_info, CefRefPtr<CefClient> &client,
                             CefBrowserSettings &settings, CefRefPtr<CefDictionaryValue> &extra_info,
                             bool *use_default_window) override {
    CEF_REQUIRE_UI_THREAD();
    *use_default_window = true;
    SetDevToolsOpen(true);
  }

  bool DoClose(CefRefPtr<CefBrowser> browser) override {
    if (browser_ && browser_->IsSame(browser)) {
      closing_ = true;
      ant_desktop_cef_begin_window_close(window_);
    }
    return false;
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    CEF_REQUIRE_UI_THREAD();
    if (devtools_ && devtools_->IsSame(browser)) {
      devtools_ = nullptr;
      SetDevToolsOpen(false);
      return;
    }
    if (!browser_ || !browser_->IsSame(browser)) return;
    browser_ = nullptr;
    g_clients.erase(window_);
    ant_desktop_cef_browser_closed(window_);
  }

  void Detach() {
    if (!browser_) return;
    NSView *view = (__bridge NSView *)browser_->GetHost()->GetWindowHandle();
    [draggable_view_ removeFromSuperview];
    draggable_view_ = nil;
    [view removeFromSuperview];
    parent_ = nil;
  }

  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool loading, bool can_go_back,
                            bool can_go_forward) override {
    Emit(window_, loading ? "loading" : "ready", 0, browser->GetMainFrame()->GetURL());
  }

  void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override {
    if (frame->IsMain()) Emit(window_, "navigation-start", 0, frame->GetURL());
  }

  void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int status) override {
    if (frame->IsMain()) Emit(window_, "navigation-commit", status, frame->GetURL());
  }

  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error_code,
                   const CefString &error_text, const CefString &failed_url) override {
    if (error_code != ERR_ABORTED) Emit(window_, "navigation-error", error_code, failed_url);
  }

  void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString &title) override {
    Emit(window_, "title", 0, title);
  }

  bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser, uint64_t prompt_id, const CefString &origin,
                              uint32_t permissions, CefRefPtr<CefPermissionPromptCallback> callback) override {
    Emit(window_, "permission-request", permissions, origin);
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }

  bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString &origin,
                                      uint32_t permissions, CefRefPtr<CefMediaAccessCallback> callback) override {
    Emit(window_, "permission-request", permissions, origin);
    callback->Cancel();
    return true;
  }

  void OnDraggableRegionsChanged(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                 const std::vector<CefDraggableRegion> &regions) override {
    CEF_REQUIRE_UI_THREAD();
    NSMutableArray<NSDictionary *> *values = [NSMutableArray arrayWithCapacity:regions.size()];
    for (const CefDraggableRegion &region : regions) {
      const CefRect &bounds = region.bounds;
      [values addObject:@{
        @"bounds" : [NSValue valueWithRect:NSMakeRect(bounds.x, bounds.y, bounds.width, bounds.height)],
        @"draggable" : @(region.draggable),
      }];
    }
    draggable_view_.regions = values;
  }

  CefRefPtr<CefBrowser> browser() const {
    return browser_;
  }
  bool closing() const {
    return closing_;
  }
  void DevToolsClosed() {
    SetDevToolsOpen(false);
  }

private:
  void SetDevToolsOpen(bool open) {
    if (devtools_open_ == open) return;
    devtools_open_ = open;
    ant_desktop_cef_set_devtools(window_, open ? 1 : 0);
    Emit(window_, open ? "devtools-opened" : "devtools-closed", 0);
  }

  void *window_;
  NSView *__strong parent_;
  AntDraggableRegionView *__strong draggable_view_;
  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<CefBrowser> devtools_;
  bool devtools_open_ = false;
  bool closing_ = false;
  IMPLEMENT_REFCOUNTING(EmbeddedClient);
};

CefRefPtr<EmbeddedClient> Client(void *window) {
  auto found = g_clients.find(window);
  return found == g_clients.end() ? nullptr : found->second;
}

} // namespace

bool ant_desktop_browser_create(void *window, void *parent_view, const char *url, const char *app_root,
                                const char *capabilities, const char *preload_path, bool sandbox, bool node_integration,
                                bool context_isolation, bool transparent) {
  if (!window || !parent_view || !url || g_clients.contains(window)) return false;
  if (app_root && app_root[0] && !RegisterAntAppSchemeHandler(app_root, node_integration)) return false;
  NSView *parent = (__bridge NSView *)parent_view;
  CefWindowInfo window_info;
  window_info.SetAsChild((__bridge void *)parent,
                         CefRect(0, 0, (int)parent.bounds.size.width, (int)parent.bounds.size.height));
  CefBrowserSettings settings;
  settings.background_color = transparent ? CefColorSetARGB(0, 0, 0, 0) : CefColorSetARGB(255, 8, 10, 18);
  CefRefPtr<CefDictionaryValue> extra = CefDictionaryValue::Create();
  extra->SetString("antCapabilities", capabilities ? capabilities : "");
  extra->SetBool("antSandbox", sandbox);
  extra->SetBool("antNodeIntegration", node_integration);
  extra->SetBool("antContextIsolation", context_isolation);
  if (preload_path && preload_path[0]) {
    std::ifstream input(preload_path, std::ios::binary);
    std::ostringstream source;
    source << input.rdbuf();
    if (!input.good() && !input.eof()) return false;
    extra->SetString("antPreloadPath", preload_path);
    extra->SetString("antPreloadSource", source.str());
  }
  CefRefPtr<EmbeddedClient> client = new EmbeddedClient(window, parent);
  g_clients[window] = client;
  if (!CefBrowserHost::CreateBrowser(window_info, client, url, settings, extra,
                                     CefRequestContext::GetGlobalContext())) {
    g_clients.erase(window);
    return false;
  }
  return true;
}

bool ant_desktop_browser_running(void *window) {
  return Client(window) != nullptr;
}

bool ant_desktop_browser_closing(void *window) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  return client && client->closing();
}

void ant_desktop_browser_close(void *window) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  if (client && client->browser()) client->browser()->GetHost()->CloseBrowser(false);
}

void ant_desktop_browser_detach(void *window) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  if (client) client->Detach();
}

void ant_desktop_browser_open_devtools(void *window) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  if (client && client->browser())
    client->browser()->GetHost()->ShowDevTools(CefWindowInfo(), nullptr, CefBrowserSettings(), CefPoint());
}

void ant_desktop_browser_close_devtools(void *window) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  if (client && client->browser()) {
    client->browser()->GetHost()->CloseDevTools();
    client->DevToolsClosed();
  }
}

void ant_desktop_browser_toggle_devtools(void *window) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  if (!client || !client->browser()) return;
  CefRefPtr<CefBrowserHost> host = client->browser()->GetHost();
  if (host->HasDevTools()) {
    host->CloseDevTools();
    client->DevToolsClosed();
  } else host->ShowDevTools(CefWindowInfo(), nullptr, CefBrowserSettings(), CefPoint());
}

void ant_desktop_browser_inspect(void *window, int x, int y) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  if (client && client->browser())
    client->browser()->GetHost()->ShowDevTools(CefWindowInfo(), nullptr, CefBrowserSettings(), CefPoint(x, y));
}

void ant_desktop_browser_reload(void *window) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  if (client && client->browser()) client->browser()->Reload();
}

bool ant_desktop_browser_send_ipc(void *window, int operation, uint64_t request_id, const char *channel,
                                  size_t channel_length, const char *payload, size_t payload_length) {
  CefRefPtr<EmbeddedClient> client = Client(window);
  if (!client || !client->browser()) return false;
  SendRendererIpc(client->browser(), operation, (double)request_id,
                  channel ? std::string(channel, channel_length) : std::string(),
                  payload ? std::string(payload, payload_length) : std::string());
  return true;
}

size_t ant_desktop_browser_count(void) {
  return g_clients.size();
}
