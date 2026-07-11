#include "desktop_core.h"

#include "../platform/platform.h"

ant_value_t DesktopNativeIpcReply(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_state_t *state = ant_desktop_state_from(js_getthis(js));
  if (!state) return js_mkerr(js, "invalid desktop IPC binding");
  (void)state;
  if (nargs != 4 || vtype(args[0]) != T_NUM || vtype(args[1]) != T_NUM || vtype(args[2]) != T_BOOL ||
      vtype(args[3]) != T_STR) {
    return js_mkerr(js, "invalid native IPC reply");
  }
  ant_desktop_window_state_t *window = ant_desktop_window_find((ant_desktop_window_id_t)js_getnum(args[0]));
  if (!window) return js_mkerr(js, "IPC BrowserWindow is closed");
  size_t payload_length = 0;
  const char *payload = js_getstr(js, args[3], &payload_length);
  int operation = js_truthy(js, args[2]) ? 0 : 1;
  if (!ant_desktop_platform_send_ipc(window, operation, (uint64_t)js_getnum(args[1]), NULL, 0, payload,
                                     payload_length)) {
    return js_mkerr(js, "Chromium is not running for this BrowserWindow");
  }
  return js_mkundef();
}

static const char *IpcChannelArgument(ant_t *js, ant_value_t *args, int nargs, size_t *length) {
  if (nargs < 1 || vtype(args[0]) != T_STR) return NULL;
  return js_getstr(js, args[0], length);
}

ant_value_t DesktopIpcMainHandle(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_state_t *state = ant_desktop_state_from(js_getthis(js));
  if (!state) return js_mkerr(js, "invalid ipcMain receiver");
  size_t length = 0;
  const char *channel = IpcChannelArgument(js, args, nargs, &length);
  if (!channel || length == 0 || nargs < 2 || !is_callable(args[1])) {
    return js_mkerr(js, "ipcMain.handle(channel, handler) requires a non-empty channel and function");
  }
  ant_value_t current = js_get(js, state->ipc_handlers, channel);
  if (vtype(current) != T_UNDEF) { return js_mkerr(js, "an IPC handler is already registered for %s", channel); }
  js_set(js, state->ipc_handlers, channel, args[1]);
  return js_mkundef();
}

ant_value_t DesktopIpcMainRemoveHandler(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_state_t *state = ant_desktop_state_from(js_getthis(js));
  if (!state) return js_mkerr(js, "invalid ipcMain receiver");
  size_t length = 0;
  const char *channel = IpcChannelArgument(js, args, nargs, &length);
  if (!channel || length == 0) { return js_mkerr(js, "ipcMain.removeHandler(channel) requires a non-empty channel"); }
  js_set(js, state->ipc_handlers, channel, js_mkundef());
  return js_mkundef();
}

ant_value_t DesktopIpcMainOn(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_state_t *state = ant_desktop_state_from(js_getthis(js));
  if (!state) return js_mkerr(js, "invalid ipcMain receiver");
  size_t length = 0;
  const char *channel = IpcChannelArgument(js, args, nargs, &length);
  if (!channel || length == 0 || nargs < 2 || !is_callable(args[1])) {
    return js_mkerr(js, "ipcMain.on(channel, listener) requires a non-empty channel and function");
  }
  ant_value_t listeners = js_get(js, state->ipc_listeners, channel);
  if (!is_array_value(listeners)) {
    listeners = js_mkarr(js);
    js_set(js, state->ipc_listeners, channel, listeners);
  }
  js_arr_push(js, listeners, args[1]);
  return js_mkundef();
}
