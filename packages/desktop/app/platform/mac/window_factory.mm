#include "embedded_browser.h"
#include "internal.h"

#include "../platform.h"
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

ant_value_t DesktopBrowserWindowCtor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t receiver = js_getthis(js);
  ant_desktop_state_t *desktop = is_object_type(receiver) ? ant_desktop_state_from(js_get_proto(js, receiver)) : NULL;
  if (!desktop) return js_mkerr(js, "invalid BrowserWindow constructor");
  NSInteger width = 900;
  NSInteger height = 600;
  NSString *title = @"Ant Desktop";
  ant_value_t options = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t web_preferences = is_object_type(options) ? js_get(js, options, "webPreferences") : js_mkundef();
  BOOL sandbox = OptionBool(js, web_preferences, "sandbox", YES);
  BOOL node_integration = NO;
  if (is_object_type(web_preferences)) {
    ant_value_t ant_integration = js_get(js, web_preferences, "antIntegration");
    ant_value_t node_integration_value = js_get(js, web_preferences, "nodeIntegration");
    if (vtype(node_integration_value) != T_UNDEF && vtype(node_integration_value) != T_BOOL) {
      return js_mkerr(js, "webPreferences.nodeIntegration must be a boolean");
    }
    if (vtype(ant_integration) != T_UNDEF && vtype(ant_integration) != T_BOOL) {
      return js_mkerr(js, "webPreferences.antIntegration must be a boolean");
    }
    if (vtype(node_integration_value) == T_BOOL && vtype(ant_integration) == T_BOOL &&
        js_truthy(js, node_integration_value) != js_truthy(js, ant_integration)) {
      return js_mkerr(js, "webPreferences.antIntegration and nodeIntegration must match");
    }
    ant_value_t integration = vtype(ant_integration) == T_BOOL ? ant_integration : node_integration_value;
    if (vtype(integration) == T_BOOL) node_integration = js_truthy(js, integration);
  }
  BOOL context_isolation = OptionBool(js, web_preferences, "contextIsolation", YES);
  if (sandbox && node_integration) { return js_mkerr(js, "webPreferences.nodeIntegration requires sandbox: false"); }
  NSString *preload_path = nil;
  if (is_object_type(web_preferences)) {
    ant_value_t preload = js_get(js, web_preferences, "preload");
    if (vtype(preload) != T_UNDEF) {
      if (vtype(preload) != T_STR) { return js_mkerr(js, "webPreferences.preload must be a file path"); }
      preload_path = OptionString(js, web_preferences, "preload");
      if (!preload_path.isAbsolutePath) {
        preload_path = [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:preload_path];
      }
      preload_path = preload_path.stringByStandardizingPath;
      BOOL directory = NO;
      if (![NSFileManager.defaultManager fileExistsAtPath:preload_path isDirectory:&directory] || directory) {
        return js_mkerr(js, "preload file does not exist: %s", preload_path.UTF8String);
      }
    }
  }
  BOOL framed = OptionBool(js, options, "frame", YES);
  BOOL closable = OptionBool(js, options, "closable", YES);
  BOOL minimizable = OptionBool(js, options, "minimizable", YES);
  BOOL resizable = OptionBool(js, options, "resizable", YES);
  BOOL maximizable = OptionBool(js, options, "maximizable", YES);
  BOOL transparent = OptionBool(js, options, "transparent", NO);
  BOOL show = OptionBool(js, options, "show", YES);
  BOOL always_on_top = OptionBool(js, options, "alwaysOnTop", NO);
  BOOL focusable = OptionBool(js, options, "focusable", YES);
  NSString *title_bar_style = OptionString(js, options, "titleBarStyle");
  NSString *vibrancy = OptionString(js, options, "vibrancy");
  NSString *background = OptionString(js, options, "backgroundColor");
  NSString *border_color = OptionString(js, options, "borderColor");
  double border_width = MAX(0, OptionNumber(js, options, "borderWidth", 0));
  double corner_radius = MAX(0, OptionNumber(js, options, "cornerRadius", 10));
  NSString *capability_error = nil;
  NSString *capability_manifest = CapabilityManifest(js, options, &capability_error);
  if (!capability_manifest) { return js_mkerr(js, "%s", capability_error.UTF8String); }

  if (nargs > 0 && is_object_type(args[0])) {
    ant_value_t value = js_get(js, args[0], "width");
    if (vtype(value) == T_NUM && js_getnum(value) > 0) width = js_getnum(value);
    value = js_get(js, args[0], "height");
    if (vtype(value) == T_NUM && js_getnum(value) > 0) height = js_getnum(value);
    value = js_get(js, args[0], "title");
    if (vtype(value) == T_STR) {
      size_t length = 0;
      const char *text = js_getstr(js, value, &length);
      title = [[NSString alloc] initWithBytes:text length:length encoding:NSUTF8StringEncoding];
    }
  }

  NSWindowStyleMask style = framed ? NSWindowStyleMaskTitled : NSWindowStyleMaskBorderless;
  if (closable && framed) style |= NSWindowStyleMaskClosable;
  if (minimizable && framed) style |= NSWindowStyleMaskMiniaturizable;
  if (resizable) style |= NSWindowStyleMaskResizable;
  BOOL inline_titlebar = [title_bar_style isEqualToString:@"hidden"] ||
                         [title_bar_style isEqualToString:@"hiddenInset"] ||
                         [title_bar_style isEqualToString:@"customButtonsOnHover"];
  if (inline_titlebar) style |= NSWindowStyleMaskFullSizeContentView;
  AntDesktopNSWindow *window = [[AntDesktopNSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                                                     styleMask:style
                                                                       backing:NSBackingStoreBuffered
                                                                         defer:NO];
  window.focusableOption = focusable;
  window.title = title;
  window.releasedWhenClosed = NO;
  window.movable = OptionBool(js, options, "movable", YES);
  window.hasShadow = OptionBool(js, options, "hasShadow", YES);
  window.opaque = !transparent;
  window.backgroundColor = transparent ? NSColor.clearColor : (ColorFromHex(background) ?: NSColor.blackColor);
  window.level = always_on_top ? NSFloatingWindowLevel : NSNormalWindowLevel;
  window.alphaValue = MIN(1, MAX(0, OptionNumber(js, options, "opacity", 1)));
  window.minSize =
    NSMakeSize(MAX(0, OptionNumber(js, options, "minWidth", 0)), MAX(0, OptionNumber(js, options, "minHeight", 0)));
  double max_width = OptionNumber(js, options, "maxWidth", DBL_MAX);
  double max_height = OptionNumber(js, options, "maxHeight", DBL_MAX);
  window.maxSize = NSMakeSize(max_width > 0 ? max_width : DBL_MAX, max_height > 0 ? max_height : DBL_MAX);
  if (OptionBool(js, options, "contentProtection", NO)) { window.sharingType = NSWindowSharingNone; }
  if (OptionBool(js, options, "visibleOnAllWorkspaces", NO)) {
    window.collectionBehavior |= NSWindowCollectionBehaviorCanJoinAllSpaces;
  }
  if (OptionBool(js, options, "hiddenInMissionControl", NO) || OptionBool(js, options, "skipTaskbar", NO)) {
    window.collectionBehavior |= NSWindowCollectionBehaviorTransient;
    window.excludedFromWindowsMenu = YES;
  }
  NSString *tabbing_identifier = OptionString(js, options, "tabbingIdentifier");
  if (tabbing_identifier.length) window.tabbingIdentifier = tabbing_identifier;
  if (!maximizable) { [window standardWindowButton:NSWindowZoomButton].enabled = NO; }
  if (!OptionBool(js, options, "fullscreenable", YES)) {
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenNone;
  }
  if (inline_titlebar) {
    window.titleVisibility = NSWindowTitleHidden;
    window.titlebarAppearsTransparent = YES;
    if (@available(macOS 11.0, *)) window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
  }
  AntBrowserView *browser_view = [[AntBrowserView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
  browser_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  browser_view.acceptsFirstMouseOption = OptionBool(js, options, "acceptFirstMouse", NO);
  if (transparent || vibrancy) browser_view.layer.backgroundColor = NSColor.clearColor.CGColor;
  if (vibrancy) {
    NSVisualEffectView *effect = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    effect.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    NSString *visual_effect_state = OptionString(js, options, "visualEffectState");
    if ([visual_effect_state isEqualToString:@"active"]) {
      effect.state = NSVisualEffectStateActive;
    } else if ([visual_effect_state isEqualToString:@"inactive"]) {
      effect.state = NSVisualEffectStateInactive;
    } else {
      effect.state = NSVisualEffectStateFollowsWindowActiveState;
    }
    if ([vibrancy isEqualToString:@"sidebar"]) effect.material = NSVisualEffectMaterialSidebar;
    else if ([vibrancy isEqualToString:@"menu"]) effect.material = NSVisualEffectMaterialMenu;
    else if ([vibrancy isEqualToString:@"popover"]) effect.material = NSVisualEffectMaterialPopover;
    else if ([vibrancy isEqualToString:@"under-window"]) effect.material = NSVisualEffectMaterialUnderWindowBackground;
    else effect.material = NSVisualEffectMaterialWindowBackground;
    [effect addSubview:browser_view];
    window.contentView = effect;
  } else {
    window.contentView = browser_view;
  }
  window.contentView.wantsLayer = YES;
  if (border_width > 0) {
    window.contentView.layer.borderWidth = border_width;
    window.contentView.layer.borderColor = (ColorFromHex(border_color) ?: NSColor.separatorColor).CGColor;
  }
  if (!framed || OptionBool(js, options, "roundedCorners", YES)) {
    window.contentView.layer.cornerRadius = corner_radius;
    window.contentView.layer.masksToBounds = YES;
  }

  ant_value_t traffic_light_position =
    is_object_type(options) ? js_get(js, options, "trafficLightPosition") : js_mkundef();
  if (is_object_type(traffic_light_position)) {
    double traffic_x = OptionNumber(js, traffic_light_position, "x", 12);
    double traffic_y = OptionNumber(js, traffic_light_position, "y", 12);
    NSButton *close_button = [window standardWindowButton:NSWindowCloseButton];
    NSButton *mini_button = [window standardWindowButton:NSWindowMiniaturizeButton];
    NSButton *zoom_button = [window standardWindowButton:NSWindowZoomButton];
    if (close_button && mini_button && zoom_button) {
      CGFloat spacing = mini_button.frame.origin.x - close_button.frame.origin.x;
      CGFloat top = close_button.superview.bounds.size.height - traffic_y - close_button.frame.size.height;
      [close_button setFrameOrigin:NSMakePoint(traffic_x, top)];
      [mini_button setFrameOrigin:NSMakePoint(traffic_x + spacing, top)];
      [zoom_button setFrameOrigin:NSMakePoint(traffic_x + spacing * 2, top)];
    }
  }

  AntDesktopWindow *desktop_window = [AntDesktopWindow new];
  desktop_window.window = window;
  desktop_window.browserView = browser_view;
  const char *capability_text = capability_manifest.UTF8String ?: "";
  ant_desktop_window_state_t *state = ant_desktop_window_create(js, desktop, capability_text, strlen(capability_text),
                                                                transparent || vibrancy.length > 0);
  if (!state) {
    [window close];
    return js_mkerr(js, "failed to allocate BrowserWindow state");
  }
  desktop_window.state = state;
  state->show_when_ready = show;
  state->sandbox = sandbox;
  state->node_integration = node_integration;
  state->context_isolation = context_isolation;
  if (preload_path.length) {
    state->preload_path = strdup(preload_path.fileSystemRepresentation);
    if (!state->preload_path) {
      ant_desktop_window_destroy(state);
      [window close];
      return js_mkerr(js, "failed to allocate preload path");
    }
  }
  state->platform_data = (__bridge void *)desktop_window;
  window.delegate = desktop_window;

  ant_desktop_window_id_t identifier = state->identifier;
  if (!g_windows) g_windows = [NSMutableDictionary dictionary];
  g_windows[@(identifier)] = desktop_window;

  double x = OptionNumber(js, options, "x", NAN);
  double y = OptionNumber(js, options, "y", NAN);
  if (isfinite(x) || isfinite(y)) {
    NSScreen *screen = window.screen ?: NSScreen.mainScreen;
    NSRect frame = window.frame;
    NSRect visible = screen.visibleFrame;
    if (isfinite(x)) frame.origin.x = visible.origin.x + x;
    if (isfinite(y)) { frame.origin.y = NSMaxY(visible) - y - frame.size.height; }
    [window setFrameOrigin:frame.origin];
  } else if (OptionBool(js, options, "center", YES)) {
    [window center];
  }
  if (OptionBool(js, options, "fullscreen", NO)) { [window toggleFullScreen:nil]; }

  ant_value_t object = js_getthis(js);
  if (!is_object_type(object)) object = js_newobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, desktop->browser_window_proto);
  if (is_object_type(proto)) js_set_proto_init(object, proto);
  js_set(js, object, "_nativeId", js_mknum((double)identifier));
  js_set(js, object, "_events", js_mkobj(js));
  ant_value_t web_contents = js_mkobj(js);
  js_set(js, web_contents, "_nativeId", js_mknum((double)identifier));
  js_set(js, web_contents, "openDevTools", js_mkfun(DesktopWebContentsOpenDevTools));
  js_set(js, web_contents, "closeDevTools", js_mkfun(DesktopWebContentsCloseDevTools));
  js_set(js, web_contents, "toggleDevTools", js_mkfun(DesktopWebContentsToggleDevTools));
  js_set(js, web_contents, "inspectElement", js_mkfun(DesktopWebContentsInspectElement));
  js_set(js, web_contents, "isDevToolsOpened", js_mkfun(DesktopWebContentsIsDevToolsOpened));
  js_set(js, web_contents, "reload", js_mkfun(DesktopWebContentsReload));
  js_set(js, web_contents, "send", js_mkfun(DesktopWebContentsSend));
  js_set(js, object, "webContents", web_contents);
  char object_key[32];
  ant_desktop_window_key(identifier, object_key);
  ant_desktop_state_attach(object, desktop);
  ant_desktop_state_attach(web_contents, desktop);
  js_set(js, desktop->window_objects, object_key, object);
  if (nargs > 0 && is_object_type(args[0])) {
    ant_value_t web_preferences = js_get(js, args[0], "webPreferences");
    if (is_object_type(web_preferences)) {
      ant_value_t capabilities = js_get(js, web_preferences, "capabilities");
      if (vtype(capabilities) != T_UNDEF) { js_set(js, object, "preloadCapabilities", capabilities); }
    }
  }
  return object;
}

ant_value_t LoadURL(ant_t *js, ant_desktop_window_state_t *state, NSString *url, NSString *app_root) {
  AntDesktopWindow *desktop_window = MacWindowForState(state);
  if (ant_desktop_browser_running(state)) return js_mkerr(js, "this BrowserWindow already has Chromium content");
  ant_value_t promise = js_mkpromise(js);
  state->load_promise = promise;
  state->load_pending = true;
  if (!ant_desktop_browser_create(state, (__bridge void *)desktop_window.browserView, url.UTF8String,
                                  app_root.UTF8String, state->capability_manifest, state->preload_path, state->sandbox,
                                  state->node_integration, state->context_isolation, state->transparent_browser)) {
    state->load_pending = false;
    state->load_promise = js_mkundef();
    return js_mkerr(js, "failed to create embedded Chromium view");
  }
  return promise;
}

ant_value_t DesktopBrowserWindowLoadURL(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *state = ant_desktop_window_from_value(js, js_getthis(js));
  if (!state) return js_mkerr(js, "invalid BrowserWindow receiver");
  if (nargs < 1 || vtype(args[0]) != T_STR) { return js_mkerr(js, "loadURL(url) requires a string"); }
  size_t length = 0;
  const char *text = js_getstr(js, args[0], &length);
  NSString *url = [[NSString alloc] initWithBytes:text length:length encoding:NSUTF8StringEncoding];
  return LoadURL(js, state, url, nil);
}

ant_value_t DesktopBrowserWindowLoadFile(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *state = ant_desktop_window_from_value(js, js_getthis(js));
  if (!state) return js_mkerr(js, "invalid BrowserWindow receiver");
  if (nargs < 1 || vtype(args[0]) != T_STR) { return js_mkerr(js, "loadFile(path) requires a string"); }
  size_t length = 0;
  const char *text = js_getstr(js, args[0], &length);
  NSString *path = [[NSString alloc] initWithBytes:text length:length encoding:NSUTF8StringEncoding];
  if (!path.isAbsolutePath) {
    path = [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path];
  }
  path = path.stringByStandardizingPath;
  if (![NSFileManager.defaultManager fileExistsAtPath:path]) {
    return js_mkerr(js, "file does not exist: %s", path.UTF8String);
  }
  NSString *root = path.stringByDeletingLastPathComponent;
  NSString *entry = [path.lastPathComponent
    stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet];
  return LoadURL(js, state, [NSString stringWithFormat:@"ant://app/%@", entry], root);
}
