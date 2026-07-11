#ifndef ANT_DESKTOP_CORE_H
#define ANT_DESKTOP_CORE_H

#include <ant.h>
#include <ptr.h>

#include "../core/window_state.h"

enum { ANT_DESKTOP_STATE_TAG = 0x41445354u }; // ADST

typedef struct ant_desktop_state {
  ant_t *js;
  bool ready;
  ant_value_t app;
  ant_value_t application_menu;
  ant_value_t browser_window_proto;
  ant_value_t browser_window_ctor;
  ant_value_t ipc_handlers;
  ant_value_t ipc_listeners;
  ant_value_t window_objects;
  ant_value_t encode;
  ant_value_t dispatch;
} ant_desktop_state_t;

static inline ant_desktop_state_t *ant_desktop_state_from(ant_value_t value) {
  return (ant_desktop_state_t *)js_get_native(value, ANT_DESKTOP_STATE_TAG);
}
static inline void ant_desktop_state_attach(ant_value_t value, ant_desktop_state_t *state) {
  js_set_native(value, state, ANT_DESKTOP_STATE_TAG);
}

ant_value_t DesktopLibrary(ant_t *js);
ant_value_t DesktopAppReady(ant_t *, ant_value_t *, int);
ant_value_t DesktopAppQuit(ant_t *, ant_value_t *, int);
ant_value_t DesktopAppGetPath(ant_t *, ant_value_t *, int);
ant_value_t DesktopIpcMainHandle(ant_t *, ant_value_t *, int);
ant_value_t DesktopIpcMainRemoveHandler(ant_t *, ant_value_t *, int);
ant_value_t DesktopIpcMainOn(ant_t *, ant_value_t *, int);
ant_value_t DesktopNativeIpcReply(ant_t *, ant_value_t *, int);
ant_value_t DesktopWebContentsOpenDevTools(ant_t *, ant_value_t *, int);
ant_value_t DesktopWebContentsCloseDevTools(ant_t *, ant_value_t *, int);
ant_value_t DesktopWebContentsToggleDevTools(ant_t *, ant_value_t *, int);
ant_value_t DesktopWebContentsInspectElement(ant_t *, ant_value_t *, int);
ant_value_t DesktopWebContentsIsDevToolsOpened(ant_t *, ant_value_t *, int);
ant_value_t DesktopWebContentsReload(ant_t *, ant_value_t *, int);
ant_value_t DesktopWebContentsSend(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowOn(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowGetBounds(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowClose(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowShow(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowHide(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowMinimize(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowRestore(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowMaximize(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowSetAlwaysOnTop(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowSetTitle(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowSetFullScreen(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowLoadURL(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowLoadFile(ant_t *, ant_value_t *, int);
ant_value_t DesktopBrowserWindowCtor(ant_t *, ant_value_t *, int);
ant_value_t DesktopMenuAppend(ant_t *, ant_value_t *, int);
ant_value_t DesktopMenuInsert(ant_t *, ant_value_t *, int);
ant_value_t DesktopMenuBuildFromTemplate(ant_t *, ant_value_t *, int);
ant_value_t DesktopGetApplicationMenu(ant_t *, ant_value_t *, int);
ant_value_t DesktopSetApplicationMenu(ant_t *, ant_value_t *, int);
ant_value_t DesktopMenuItemCtor(ant_t *, ant_value_t *, int);

#endif
