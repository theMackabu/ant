#include "../platform/platform.h"
#include "desktop_core.h"

#include <limits.h>
#include <string.h>

ant_value_t DesktopAppReady(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_state_t *state = ant_desktop_state_from(js_getthis(js));
  if (!state) return js_mkerr(js, "invalid app receiver");
  ant_value_t ready_promise = js_get_slot(state->app, SLOT_DATA);
  if (!is_object_type(ready_promise)) {
    ready_promise = js_mkpromise(js);
    js_set_slot_wb(js, state->app, SLOT_DATA, ready_promise);
    if (state->ready) js_resolve_promise(js, ready_promise, js_mkundef());
  }
  return ready_promise;
}

ant_value_t DesktopAppQuit(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_platform_quit();
  return js_mkundef();
}

ant_value_t DesktopAppGetPath(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return js_mkerr(js, "app.getPath(name) requires a string");
  size_t length = 0;
  const char *name = js_getstr(js, args[0], &length);
  if (length == 0 || length >= 32) return js_mkerr(js, "unknown app path: %.*s", (int)length, name);
  char key[32];
  memcpy(key, name, length);
  key[length] = '\0';
  char path[PATH_MAX];
  if (!ant_desktop_platform_get_path(key, path, sizeof(path))) return js_mkerr(js, "unknown app path: %s", key);
  return js_mkstr(js, path, strlen(path));
}
