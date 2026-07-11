#include "../platform.h"
#include "cef_runtime.h"
#include "embedded_browser.h"
#include "internal.h"

NSMutableDictionary<NSNumber *, AntDesktopWindow *> *g_windows;
static bool g_terminating;

static void PumpBrowserShutdown(void) {
  if (!g_terminating || ant_desktop_browser_count() == 0) return;
  ant_desktop_cef_do_message_loop_work();
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), dispatch_get_main_queue(),
                 ^{ PumpBrowserShutdown(); });
}

static void StopApplication(void) {
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp stop:nil];
    NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:NO];
  });
}

const char *ant_desktop_platform_resources_path(void) {
  return NSBundle.mainBundle.resourcePath.fileSystemRepresentation;
}

static NSString *SearchPath(NSSearchPathDirectory directory) {
  return [NSFileManager.defaultManager URLsForDirectory:directory inDomains:NSUserDomainMask].firstObject.path;
}

static NSString *ApplicationName(void) {
  NSBundle *bundle = NSBundle.mainBundle;
  return [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: [bundle objectForInfoDictionaryKey:@"CFBundleName"] ?:
                                                                   NSProcessInfo.processInfo.processName;
}

bool ant_desktop_platform_get_path(const char *name, char *path, size_t capacity) {
  NSString *key = [NSString stringWithUTF8String:name];
  NSString *value = nil;
  if ([key isEqualToString:@"home"]) value = NSHomeDirectory();
  else if ([key isEqualToString:@"temp"]) value = NSTemporaryDirectory();
  else if ([key isEqualToString:@"appData"]) value = SearchPath(NSApplicationSupportDirectory);
  else if ([key isEqualToString:@"userData"])
    value = [SearchPath(NSApplicationSupportDirectory) stringByAppendingPathComponent:ApplicationName()];
  else if ([key isEqualToString:@"desktop"]) value = SearchPath(NSDesktopDirectory);
  else if ([key isEqualToString:@"documents"]) value = SearchPath(NSDocumentDirectory);
  else if ([key isEqualToString:@"downloads"]) value = SearchPath(NSDownloadsDirectory);
  else if ([key isEqualToString:@"resources"]) value = NSBundle.mainBundle.resourcePath;
  else if ([key isEqualToString:@"exe"]) value = NSBundle.mainBundle.executablePath;
  if (!value) return false;
  const char *bytes = value.fileSystemRepresentation;
  size_t length = strlen(bytes);
  if (length >= capacity) return false;
  memcpy(path, bytes, length + 1);
  return true;
}

@implementation AntDesktopNSWindow
- (BOOL)canBecomeKeyWindow {
  return self.focusableOption;
}
- (BOOL)canBecomeMainWindow {
  return self.focusableOption;
}
@end

@implementation AntDesktopWindow

- (void)finalizeClosedWindow {
  ant_desktop_window_state_t *state = self.state;
  if (!state) return;
  [g_windows removeObjectForKey:@(state->identifier)];
  state->platform_data = NULL;
  self.state = NULL;
  ant_desktop_release_window_object(state);
  if (ant_desktop_window_count() == 0) {
    if (g_terminating) StopApplication();
    else [NSApp terminate:nil];
  }
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
  if (self.state && ant_desktop_browser_running(self.state) && !ant_desktop_browser_closing(self.state)) {
    ant_desktop_browser_close(self.state);
    return NO;
  }
  if (self.state) ant_desktop_browser_detach(self.state);
  return YES;
}

- (void)windowDidMove:(NSNotification *)notification {
  (void)notification;
  ant_desktop_emit_window_event(self.state, "move", "", 0, 0);
}

- (void)windowDidResize:(NSNotification *)notification {
  (void)notification;
  ant_desktop_emit_window_event(self.state, "resize", "", 0, 0);
}

- (void)windowWillClose:(NSNotification *)notification {
  (void)notification;
  ant_desktop_window_state_t *state = self.state;
  if (!state) return;
  ant_desktop_emit_window_event(state, "closed", "", 0, 0);
  self.windowClosed = YES;
  if (!ant_desktop_browser_running(state)) [self finalizeClosedWindow];
}

@end

AntDesktopWindow *MacWindowForState(ant_desktop_window_state_t *state) {
  return state && state->platform_data ? (__bridge AntDesktopWindow *)state->platform_data : nil;
}

bool ant_desktop_platform_browser_running(ant_desktop_window_state_t *state) {
  return ant_desktop_browser_running(state);
}

bool ant_desktop_platform_open_devtools(ant_desktop_window_state_t *state) {
  if (!ant_desktop_browser_running(state)) return false;
  ant_desktop_browser_open_devtools(state);
  return true;
}

bool ant_desktop_platform_close_devtools(ant_desktop_window_state_t *state) {
  if (!ant_desktop_browser_running(state)) return false;
  ant_desktop_browser_close_devtools(state);
  return true;
}

bool ant_desktop_platform_toggle_devtools(ant_desktop_window_state_t *state) {
  if (!ant_desktop_browser_running(state)) return false;
  ant_desktop_browser_toggle_devtools(state);
  return true;
}

bool ant_desktop_platform_inspect(ant_desktop_window_state_t *state, int x, int y) {
  if (!ant_desktop_browser_running(state)) return false;
  ant_desktop_browser_inspect(state, x, y);
  return true;
}

bool ant_desktop_platform_reload(ant_desktop_window_state_t *state) {
  if (!ant_desktop_browser_running(state)) return false;
  ant_desktop_browser_reload(state);
  return true;
}

bool ant_desktop_platform_send_ipc(ant_desktop_window_state_t *state, int operation, uint64_t request_id,
                                   const char *channel, size_t channel_length, const char *payload,
                                   size_t payload_length) {
  return ant_desktop_browser_send_ipc(state, operation, request_id, channel, channel_length, payload, payload_length);
}

bool ant_desktop_platform_get_bounds(ant_desktop_window_state_t *state, ant_desktop_window_bounds_t *bounds) {
  NSWindow *window = MacWindowForState(state).window;
  if (!window || !bounds) return false;
  NSScreen *screen = window.screen ?: NSScreen.mainScreen;
  NSRect frame = window.frame;
  NSRect content = [window contentRectForFrameRect:frame];
  NSRect visible = screen.visibleFrame;
  bounds->x = frame.origin.x - visible.origin.x;
  bounds->y = NSMaxY(visible) - NSMaxY(frame);
  bounds->width = content.size.width;
  bounds->height = content.size.height;
  return true;
}

void ant_desktop_platform_close(ant_desktop_window_state_t *state) {
  if (ant_desktop_browser_running(state)) ant_desktop_browser_close(state);
  else [MacWindowForState(state).window close];
}

void ant_desktop_platform_show(ant_desktop_window_state_t *state) {
  state->show_when_ready = false;
  [MacWindowForState(state).window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

void ant_desktop_platform_hide(ant_desktop_window_state_t *state) {
  state->show_when_ready = false;
  [MacWindowForState(state).window orderOut:nil];
}

void ant_desktop_platform_minimize(ant_desktop_window_state_t *state) {
  [MacWindowForState(state).window miniaturize:nil];
}

void ant_desktop_platform_restore(ant_desktop_window_state_t *state) {
  [MacWindowForState(state).window deminiaturize:nil];
}

void ant_desktop_platform_maximize(ant_desktop_window_state_t *state) {
  [MacWindowForState(state).window zoom:nil];
}

void ant_desktop_platform_set_always_on_top(ant_desktop_window_state_t *state, bool enabled) {
  MacWindowForState(state).window.level = enabled ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

void ant_desktop_platform_set_title(ant_desktop_window_state_t *state, const char *title, size_t length) {
  MacWindowForState(state).window.title = [[NSString alloc] initWithBytes:title
                                                                   length:length
                                                                 encoding:NSUTF8StringEncoding];
}

void ant_desktop_platform_set_full_screen(ant_desktop_window_state_t *state, bool enabled) {
  NSWindow *window = MacWindowForState(state).window;
  bool current = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
  if (enabled != current) [window toggleFullScreen:nil];
}

void ant_desktop_platform_quit(void) {
  [NSApp terminate:nil];
}

void ant_desktop_platform_shutdown_all_windows(void) {
  for (AntDesktopWindow *window in g_windows.allValues.copy) {
    ant_desktop_browser_close(window.state);
  }
  PumpBrowserShutdown();
}

void ant_desktop_platform_begin_browser_close(ant_desktop_window_state_t *state) {
  AntDesktopWindow *window = MacWindowForState(state);
  if (!window || window.windowClosed) return;
  ant_desktop_browser_detach(state);
  [window.window close];
}

void ant_desktop_platform_finish_browser_close(ant_desktop_window_state_t *state) {
  AntDesktopWindow *window = MacWindowForState(state);
  if (window.windowClosed) [window finalizeClosedWindow];
  if (g_terminating && ant_desktop_browser_count() == 0) StopApplication();
}

void ant_desktop_request_termination(void) {
  if (g_terminating) return;
  g_terminating = true;
  for (AntDesktopWindow *window in g_windows.allValues.copy) {
    ant_desktop_emit_window_event(window.state, "quit", "", 0, 0);
  }
  if (ant_desktop_window_count() == 0) {
    StopApplication();
    return;
  }
  for (AntDesktopWindow *window in g_windows.allValues.copy) {
    if (ant_desktop_browser_running(window.state)) ant_desktop_browser_close(window.state);
    else [window.window close];
  }
  PumpBrowserShutdown();
}
