#import <AppKit/AppKit.h>

#include <ant.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../api/desktop_core.h"
#include "../../archive/archive.h"
#include "../../runtime/ant_runtime.h"
#include "../platform.h"
#include "cef_runtime.h"
#include "event_loop.h"

static NSString *BundledEntryPath(NSString **temporary_root) {
  NSString *relative = [NSBundle.mainBundle objectForInfoDictionaryKey:@"AntDesktopEntry"];
  if (!relative.length) return nil;
  NSString *archive = [NSBundle.mainBundle objectForInfoDictionaryKey:@"AntDesktopArchive"];
  if (archive.length) {
    NSString *template = [NSTemporaryDirectory() stringByAppendingPathComponent:@"ant-desktop.XXXXXX"];
    char *buffer = strdup(template.fileSystemRepresentation);
    char *directory = buffer ? mkdtemp(buffer) : NULL;
    if (!directory) {
      free(buffer);
      return nil;
    }
    NSString *root = [NSString stringWithUTF8String:directory];
    NSString *archive_path = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:archive];
    char error[256] = {0};
    if (!ant_desktop_extract_archive(archive_path.fileSystemRepresentation, directory, error, sizeof(error))) {
      fprintf(stderr, "failed to extract application archive: %s\n", error);
      [NSFileManager.defaultManager removeItemAtPath:root error:nil];
      free(buffer);
      return nil;
    }
    free(buffer);
    *temporary_root = root;
    return [root stringByAppendingPathComponent:relative];
  }
  return [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:relative];
}

static void RemoveTemporaryApplication(NSString *temporary_root) {
  if (temporary_root) { [NSFileManager.defaultManager removeItemAtPath:temporary_root error:nil]; }
}

static const char *RuntimeErrorDetail(ant_t *js, ant_value_t error) {
  if (vtype(error) != T_ERR || vdata(error) == 0) return js_str(js, error);
  ant_value_t object = mkval(T_OBJ, vdata(error));
  ant_value_t stack = js_get(js, object, "stack");
  if (vtype(stack) == T_STR) return js_str(js, stack);
  ant_value_t message = js_get(js, object, "message");
  return vtype(message) == T_STR ? js_str(js, message) : js_str(js, error);
}

static dispatch_source_t InstallDevelopmentReloadSignal(void) {
  if (!getenv("ANT_DESKTOP_DEV")) return nil;
  signal(SIGUSR1, SIG_IGN);
  dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0, dispatch_get_main_queue());
  dispatch_source_set_event_handler(source, ^{
    for (ant_desktop_window_state_t *window = ant_desktop_window_first(); window; window = window->next) {
      ant_desktop_platform_reload(window);
    }
  });
  dispatch_resume(source);
  return source;
}

int main(int argc, char **argv) {
  @autoreleasepool {
    NSString *temporary_root = nil;
    NSString *bundled_entry = BundledEntryPath(&temporary_root);
    if (argc < 2 && !bundled_entry.length) {
      fprintf(stderr, "usage: ant-desktop <main.js> [args...]\n");
      RemoveTemporaryApplication(temporary_root);
      return 64;
    }

    const char *entry_path = bundled_entry.length ? bundled_entry.fileSystemRepresentation : argv[1];
    int runtime_argc = argc - 1;
    char **runtime_argv = argv + 1;
    char **owned_runtime_argv = NULL;
    if (bundled_entry.length) {
      runtime_argc = argc;
      owned_runtime_argv = calloc((size_t)runtime_argc, sizeof(char *));
      if (!owned_runtime_argv) {
        fputs("failed to allocate bundled application arguments\n", stderr);
        RemoveTemporaryApplication(temporary_root);
        return 1;
      }
      owned_runtime_argv[0] = (char *)entry_path;
      for (int index = 1; index < runtime_argc; index++) {
        owned_runtime_argv[index] = argv[index];
      }
      runtime_argv = owned_runtime_argv;
    }

    if (!ant_desktop_cef_initialize(argc, argv)) {
      fputs("failed to initialize embedded Chromium\n", stderr);
      free(owned_runtime_argv);
      RemoveTemporaryApplication(temporary_root);
      return 1;
    }
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    volatile char stack_base;
    ant_t *js = js_create_dynamic();
    if (!js) {
      fputs("failed to create Ant runtime\n", stderr);
      ant_desktop_cef_shutdown();
      free(owned_runtime_argv);
      RemoveTemporaryApplication(temporary_root);
      return 1;
    }
    js_setstackbase(js, (void *)&stack_base);
    ant_runtime_init(js, runtime_argc, runtime_argv, NULL);
    ant_value_t initialized = AntInitializeRuntimeModules(js);
    if (is_err(initialized)) {
      fprintf(stderr, "desktop runtime initialization failed: %s\n", RuntimeErrorDetail(js, initialized));
      js_destroy(js);
      ant_desktop_cef_shutdown();
      free(owned_runtime_argv);
      RemoveTemporaryApplication(temporary_root);
      return 1;
    }

    ant_register_library(DesktopLibrary, "ant:desktop", NULL);
    AntRuntimePump *pump = [[AntRuntimePump alloc] initWithRuntime:js];
    dispatch_source_t development_reload = InstallDevelopmentReloadSignal();
    [NSApp finishLaunching];
    ant_value_t imported = AntImportModule(js, entry_path);
    if (is_err(imported)) {
      fprintf(stderr, "desktop entry import failed (%s): %s\n", entry_path, RuntimeErrorDetail(js, imported));
      js_destroy(js);
      ant_desktop_cef_shutdown();
      free(owned_runtime_argv);
      RemoveTemporaryApplication(temporary_root);
      return 1;
    }

    [pump pump];
    [NSApp run];

    ant_desktop_platform_shutdown_all_windows();
    ant_desktop_cef_shutdown();
    development_reload = nil;
    pump = nil;
    js_destroy(js);
    free(owned_runtime_argv);
    RemoveTemporaryApplication(temporary_root);
  }
  return 0;
}
