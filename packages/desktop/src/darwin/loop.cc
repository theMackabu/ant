#include "desktop.hh"

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <signal.h>

namespace desktop::platform {

static CFRunLoopTimerRef timer;

void StartLoop(void (*pump)(), intptr_t descriptor) {
  CFRunLoopRef loop = CFRunLoopGetMain();
  timer = CFRunLoopTimerCreateWithHandler(nullptr, CFAbsoluteTimeGetCurrent() + 3600.0, 0, 0, 0, ^(CFRunLoopTimerRef) { pump(); });
  CFRunLoopAddTimer(loop, timer, kCFRunLoopCommonModes);
  CFRunLoopObserverRef observer =
    CFRunLoopObserverCreateWithHandler(nullptr, kCFRunLoopBeforeWaiting, true, 0, ^(CFRunLoopObserverRef, CFRunLoopActivity) { pump(); });
  CFRunLoopAddObserver(loop, observer, kCFRunLoopCommonModes);

  if (descriptor < 0) return;
  dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, uintptr_t(descriptor), 0, dispatch_get_main_queue());
  dispatch_source_set_event_handler(source, ^{ pump(); });
  dispatch_resume(source);
}

void ScheduleLoop(int64_t delay) {
  CFRunLoopTimerSetNextFireDate(timer, CFAbsoluteTimeGetCurrent() + (delay < 0 ? 3600.0 : double(delay) / 1000.0));
}

void WakeLoop() {
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

void OnReloadSignal(void (*reload)()) {
  signal(SIGUSR1, SIG_IGN);
  dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0, dispatch_get_main_queue());
  dispatch_source_set_event_handler(source, ^{ reload(); });
  dispatch_resume(source);
}

}
