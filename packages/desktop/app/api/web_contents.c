#include "desktop_core.h"

#include "../platform/platform.h"

typedef bool (*WebContentsAction)(ant_desktop_window_state_t *window);

static ant_value_t DesktopWebContentsAction(ant_t *js, WebContentsAction action) {
  ant_desktop_window_state_t *window = ant_desktop_window_from_value(js, js_getthis(js));
  if (!window) return js_mkerr(js, "invalid WebContents receiver");
  if (!action(window)) return js_mkerr(js, "Chromium is not running for this BrowserWindow");
  return js_mkundef();
}

ant_value_t DesktopWebContentsOpenDevTools(ant_t *js, ant_value_t *args, int nargs) {
  return DesktopWebContentsAction(js, ant_desktop_platform_open_devtools);
}

ant_value_t DesktopWebContentsCloseDevTools(ant_t *js, ant_value_t *args, int nargs) {
  return DesktopWebContentsAction(js, ant_desktop_platform_close_devtools);
}

ant_value_t DesktopWebContentsToggleDevTools(ant_t *js, ant_value_t *args, int nargs) {
  return DesktopWebContentsAction(js, ant_desktop_platform_toggle_devtools);
}

ant_value_t DesktopWebContentsInspectElement(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = ant_desktop_window_from_value(js, js_getthis(js));
  if (!window) return js_mkerr(js, "invalid WebContents receiver");
  if (nargs < 2 || vtype(args[0]) != T_NUM || vtype(args[1]) != T_NUM)
    return js_mkerr(js, "inspectElement(x, y) requires coordinates");

  if (!ant_desktop_platform_inspect(window, (int)js_getnum(args[0]), (int)js_getnum(args[1])))
    return js_mkerr(js, "Chromium is not running for this BrowserWindow");

  return js_mkundef();
}

ant_value_t DesktopWebContentsIsDevToolsOpened(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = ant_desktop_window_from_value(js, js_getthis(js));
  if (!window) return js_mkerr(js, "invalid WebContents receiver");
  return window->devtools_open ? js_true : js_false;
}

ant_value_t DesktopWebContentsReload(ant_t *js, ant_value_t *args, int nargs) {
  return DesktopWebContentsAction(js, ant_desktop_platform_reload);
}

ant_value_t DesktopWebContentsSend(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_window_state_t *window = ant_desktop_window_from_value(js, js_getthis(js));
  if (!window) return js_mkerr(js, "invalid WebContents receiver");
  if (nargs < 1 || vtype(args[0]) != T_STR) return js_mkerr(js, "webContents.send(channel, value) requires a channel");

  size_t channel_length = 0;
  const char *channel = js_getstr(js, args[0], &channel_length);
  if (!ant_desktop_has_capability(window, "receive", channel, channel_length))
    return js_mkerr(js, "IPC receive channel is not granted: %.*s", (int)channel_length, channel);

  ant_value_t encode = window->desktop->encode;
  if (!is_callable(encode)) return js_mkerr(js, "desktop IPC bridge is unavailable");

  ant_value_t value = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t encoded = ant_desktop_call_function(js, encode, js_glob(js), &value, 1);

  if (is_err(encoded)) return encoded;
  if (vtype(encoded) != T_STR) return js_mkerr(js, "IPC encoding failed");

  size_t payload_length = 0;
  const char *payload = js_getstr(js, encoded, &payload_length);

  if (!ant_desktop_platform_send_ipc(window, 2, 0, channel, channel_length, payload, payload_length))
    return js_mkerr(js, "Chromium is not running for this BrowserWindow");

  return js_mkundef();
}
