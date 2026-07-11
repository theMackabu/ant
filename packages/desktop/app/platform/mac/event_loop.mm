#import <AppKit/AppKit.h>

#include "cef_runtime.h"
#include "event_loop.h"

@interface AntRuntimePump ()
@property(nonatomic, assign) ant_t *js;
@property(nonatomic, assign) BOOL pumping;
@property(nonatomic, assign) CFFileDescriptorRef backendDescriptor;
@property(nonatomic, assign) CFRunLoopSourceRef backendSource;
@property(nonatomic, assign) CFRunLoopObserverRef observer;
@property(nonatomic, assign) CFRunLoopTimerRef timer;
- (instancetype)initWithRuntime:(ant_t *)js;
- (void)pump;
@end

static void BackendReady(CFFileDescriptorRef descriptor, CFOptionFlags callback_types, void *info) {
  (void)callback_types;
  AntRuntimePump *pump = (__bridge AntRuntimePump *)info;
  [pump pump];
  CFFileDescriptorEnableCallBacks(descriptor, kCFFileDescriptorReadCallBack);
}

static void RunLoopObserved(CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info) {
  (void)observer;
  (void)activity;
  [(__bridge AntRuntimePump *)info pump];
}

static void RuntimeTimerFired(CFRunLoopTimerRef timer, void *info) {
  (void)timer;
  [(__bridge AntRuntimePump *)info pump];
}

@implementation AntRuntimePump

- (instancetype)initWithRuntime:(ant_t *)js {
  self = [super init];
  if (!self) return nil;
  _js = js;

  CFRunLoopRef run_loop = CFRunLoopGetMain();
  CFRunLoopObserverContext observer_context = {0, (__bridge void *)self, NULL, NULL, NULL};
  _observer = CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, 0, RunLoopObserved, &observer_context);
  CFRunLoopAddObserver(run_loop, _observer, kCFRunLoopCommonModes);

  CFRunLoopTimerContext timer_context = {0, (__bridge void *)self, NULL, NULL, NULL};
  _timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 3600.0, 0, 0, 0, RuntimeTimerFired, &timer_context);
  CFRunLoopAddTimer(run_loop, _timer, kCFRunLoopCommonModes);

  int backend_fd = uv_backend_fd(uv_default_loop());
  if (backend_fd >= 0) {
    CFFileDescriptorContext descriptor_context = {0, (__bridge void *)self, NULL, NULL, NULL};
    _backendDescriptor = CFFileDescriptorCreate(NULL, backend_fd, false, BackendReady, &descriptor_context);
    _backendSource = CFFileDescriptorCreateRunLoopSource(NULL, _backendDescriptor, 0);
    CFRunLoopAddSource(run_loop, _backendSource, kCFRunLoopCommonModes);
    CFFileDescriptorEnableCallBacks(_backendDescriptor, kCFFileDescriptorReadCallBack);
  }
  return self;
}

- (void)pump {
  if (_pumping) return;
  _pumping = YES;
  ant_desktop_cef_do_message_loop_work();
  js_reactor_pump_repl_nowait(_js);
  int timeout_ms = uv_backend_timeout(uv_default_loop());
  int64_t cef_timeout_ms = ant_desktop_cef_next_message_delay_ms();
  if (cef_timeout_ms >= 0 && (timeout_ms < 0 || cef_timeout_ms < timeout_ms)) timeout_ms = (int)cef_timeout_ms;
  CFAbsoluteTime fire =
    timeout_ms < 0 ? CFAbsoluteTimeGetCurrent() + 3600.0 : CFAbsoluteTimeGetCurrent() + ((double)timeout_ms / 1000.0);
  CFRunLoopTimerSetNextFireDate(_timer, fire);
  _pumping = NO;
}

- (void)dealloc {
  if (_backendSource) CFRelease(_backendSource);
  if (_backendDescriptor) CFRelease(_backendDescriptor);
  if (_observer) CFRelease(_observer);
  if (_timer) CFRelease(_timer);
}

@end
