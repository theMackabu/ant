#import <AppKit/AppKit.h>

#include "cef_callbacks.h"
#include "cef_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>

#include "../../../browser/cef/app_scheme.h"
#include "include/cef_app.h"
#include "include/cef_application_mac.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_library_loader.h"

@interface AntDesktopApplication : NSApplication <CefAppProtocol> {
@private
  BOOL handlingSendEvent_;
}
@end

@implementation AntDesktopApplication
- (BOOL)isHandlingSendEvent {
  return handlingSendEvent_;
}
- (void)setHandlingSendEvent:(BOOL)value {
  handlingSendEvent_ = value;
}
- (void)sendEvent:(NSEvent *)event {
  CefScopedSendingEvent sending_event;
  [super sendEvent:event];
}
- (void)terminate:(id)sender {
  (void)sender;
  ant_desktop_request_termination();
}
@end

namespace {

using Clock = std::chrono::steady_clock;

std::unique_ptr<CefScopedLibraryLoader> g_library_loader;
bool g_custom_library;
std::atomic<int64_t> g_message_deadline_ns{-1};
CFRunLoopRef g_main_run_loop;
bool g_initialized;

int64_t NowNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

class DesktopCefApp final : public CefApp, public CefBrowserProcessHandler {
public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
    RegisterAntCustomSchemes(registrar);
  }

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnBeforeCommandLineProcessing(const CefString &process_type, CefRefPtr<CefCommandLine> command_line) override {
    if (!process_type.empty()) return;
    command_line->AppendSwitchWithValue("disable-features",
                                        "NativeNotifications,SystemNotifications,NewMacNotificationAPI");
    command_line->AppendSwitch("disable-notifications");
    command_line->AppendSwitch("use-mock-keychain");
    command_line->AppendSwitchWithValue("lang", "en-US");
    if (!ant_desktop_devtools_enabled()) command_line->AppendSwitch("disable-dev-tools");
  }

  void OnScheduleMessagePumpWork(int64_t delay_ms) override {
    int64_t deadline = NowNanoseconds() + std::max<int64_t>(0, delay_ms) * 1000000;
    int64_t previous = g_message_deadline_ns.load(std::memory_order_relaxed);
    while ((previous < 0 || deadline < previous) &&
           !g_message_deadline_ns.compare_exchange_weak(previous, deadline, std::memory_order_release,
                                                        std::memory_order_relaxed)) {}
    if (g_main_run_loop) CFRunLoopWakeUp(g_main_run_loop);
  }

private:
  IMPLEMENT_REFCOUNTING(DesktopCefApp);
};

NSString *RuntimeFrameworksPath(void) {
  const char *configured = getenv("ANT_DESKTOP_FRAMEWORKS");
  return configured ? [NSString stringWithUTF8String:configured]
                    : [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents/Frameworks"];
}

NSString *BrowserSubprocessPath(void) {
  NSString *frameworks = RuntimeFrameworksPath();
  NSString *name = @"Ant Desktop Helper";
  return
    [frameworks stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.app/Contents/MacOS/%@", name, name]];
}

NSString *ApplicationSupportPath(void) {
  NSString *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                           inDomains:NSUserDomainMask]
                        .firstObject.path;
  NSBundle *bundle = NSBundle.mainBundle;
  NSString *name = [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?:
                   [bundle objectForInfoDictionaryKey:@"CFBundleName"] ?: NSProcessInfo.processInfo.processName;
  return [[support stringByAppendingPathComponent:name] stringByAppendingPathComponent:@"Chromium"];
}

} // namespace

bool ant_desktop_devtools_enabled(void) {
  return getenv("ANT_DESKTOP_DEV") != nullptr;
}

bool ant_desktop_cef_initialize(int argc, char **argv) {
  if (g_initialized) return true;
  const char *framework = getenv("ANT_DESKTOP_CEF_FRAMEWORK");
  if (framework) {
    if (!cef_load_library(framework)) return false;
    g_custom_library = true;
  } else {
    g_library_loader = std::make_unique<CefScopedLibraryLoader>();
    if (!g_library_loader->LoadInMain()) return false;
  }

  [AntDesktopApplication sharedApplication];
  if (![NSApp isKindOfClass:AntDesktopApplication.class]) return false;
  g_main_run_loop = CFRunLoopGetMain();

  CefSettings settings;
  settings.external_message_pump = true;
  settings.no_sandbox = true;
  NSString *subprocess = BrowserSubprocessPath();
  if (![NSFileManager.defaultManager isExecutableFileAtPath:subprocess]) return false;
  CefString(&settings.browser_subprocess_path) = subprocess.fileSystemRepresentation;
  NSString *frameworks = RuntimeFrameworksPath();
  NSString *framework_bundle = [frameworks stringByAppendingPathComponent:@"Chromium Embedded Framework.framework"];
  CefString(&settings.framework_dir_path) = framework_bundle.fileSystemRepresentation;
  const char *configured_frameworks = getenv("ANT_DESKTOP_FRAMEWORKS");
  if (configured_frameworks) {
    NSString *bundle = [frameworks stringByDeletingLastPathComponent].stringByDeletingLastPathComponent;
    CefString(&settings.main_bundle_path) = bundle.fileSystemRepresentation;
  }
  NSString *resources = [framework_bundle stringByAppendingPathComponent:@"Resources"];
  CefString(&settings.resources_dir_path) = resources.fileSystemRepresentation;
  CefString(&settings.locales_dir_path) =
    [resources stringByAppendingPathComponent:@"locales"].fileSystemRepresentation;
  NSString *cache = ApplicationSupportPath();
  [NSFileManager.defaultManager createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil];
  CefString(&settings.root_cache_path) = cache.fileSystemRepresentation;

  CefMainArgs main_args(argc, argv);
  if (!CefInitialize(main_args, settings, new DesktopCefApp(), nullptr)) {
    g_library_loader.reset();
    return false;
  }
  g_initialized = true;
  return true;
}

void ant_desktop_cef_do_message_loop_work(void) {
  if (!g_initialized) return;
  g_message_deadline_ns.store(-1, std::memory_order_release);
  CefDoMessageLoopWork();
}

int64_t ant_desktop_cef_next_message_delay_ms(void) {
  int64_t deadline = g_message_deadline_ns.load(std::memory_order_acquire);
  if (deadline < 0) return -1;
  int64_t remaining = deadline - NowNanoseconds();
  return remaining <= 0 ? 0 : (remaining + 999999) / 1000000;
}

void ant_desktop_cef_shutdown(void) {
  if (!g_initialized) return;
  CefShutdown();
  g_initialized = false;
  g_main_run_loop = nullptr;
  g_library_loader.reset();
  if (g_custom_library) {
    cef_unload_library();
    g_custom_library = false;
  }
}
