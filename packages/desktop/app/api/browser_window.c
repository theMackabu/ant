#include "../platform/platform.h"
#include "desktop_core.h"

ant_value_t DesktopBrowserWindowOn(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2 || vtype(args[0]) != kTypeString || !is_callable(args[1]))
    return js_mkerr(js, "BrowserWindow.on(event, listener) requires a name and function");

  ant_value_t object = js_getthis(js);
  if (!ant_desktop_window_from_value(js, object)) return js_mkerr(js, "invalid BrowserWindow receiver");

  ant_value_t events = js_get(js, object, "_events");
  if (!is_object_type(events)) {
    events = js_mkobj(js);
    js_set(js, object, "_events", events);
  }

  size_t length = 0;
  const char *name = js_getstr(js, args[0], &length);
  ant_value_t listeners = js_get(js, events, name);

  if (!is_array_value(listeners)) {
    listeners = js_mkarr(js);
    js_set(js, events, name, listeners);
  }

  js_arr_push(js, listeners, args[1]);
  return object;
}

static ant_desktop_window_state_t *RequireWindow(ant_t *js) {
  return ant_desktop_window_from_value(js, js_getthis(js));
}

ant_value_t DesktopBrowserWindowGetBounds(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  ant_desktop_window_bounds_t bounds;
  if (!ant_desktop_platform_get_bounds(window, &bounds)) return js_mkerr(js, "BrowserWindow is closed");
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "x", js_mknum(bounds.x));
  js_set(js, result, "y", js_mknum(bounds.y));
  js_set(js, result, "width", js_mknum(bounds.width));
  js_set(js, result, "height", js_mknum(bounds.height));
  return result;
}

ant_value_t DesktopBrowserWindowClose(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  ant_desktop_platform_close(window);
  return js_mkundef();
}

ant_value_t DesktopBrowserWindowShow(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  ant_desktop_platform_show(window);
  return js_mkundef();
}

ant_value_t DesktopBrowserWindowHide(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  ant_desktop_platform_hide(window);
  return js_mkundef();
}

ant_value_t DesktopBrowserWindowMinimize(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  ant_desktop_platform_minimize(window);
  return js_mkundef();
}

ant_value_t DesktopBrowserWindowRestore(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  ant_desktop_platform_restore(window);
  return js_mkundef();
}

ant_value_t DesktopBrowserWindowMaximize(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  ant_desktop_platform_maximize(window);
  return js_mkundef();
}

ant_value_t DesktopBrowserWindowSetAlwaysOnTop(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  if (nargs < 1 || vtype(args[0]) != kTypeBool) { return js_mkerr(js, "setAlwaysOnTop(flag) requires a boolean"); }
  ant_desktop_platform_set_always_on_top(window, js_truthy(js, args[0]));
  return js_mkundef();
}

ant_value_t DesktopBrowserWindowSetTitle(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  if (nargs < 1 || vtype(args[0]) != kTypeString) { return js_mkerr(js, "setTitle(title) requires a string"); }
  size_t length = 0;
  const char *title = js_getstr(js, args[0], &length);
  ant_desktop_platform_set_title(window, title, length);
  return js_mkundef();
}

ant_value_t DesktopBrowserWindowSetFullScreen(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = RequireWindow(js);
  if (!window) return js_mkerr(js, "invalid BrowserWindow receiver");
  if (nargs < 1 || vtype(args[0]) != kTypeBool) { return js_mkerr(js, "setFullScreen(flag) requires a boolean"); }
  ant_desktop_platform_set_full_screen(window, js_truthy(js, args[0]));
  return js_mkundef();
}
