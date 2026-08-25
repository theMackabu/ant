#include "internal.h"

#include "native_menu.h"
void RefreshApplicationMenu(ant_t *js, ant_value_t menu) {
  ant_desktop_state_t *state = ant_desktop_state_from(menu);
  if (!state || state->application_menu != menu) return;
  ant_value_t template_value = js_get(js, menu, "_template");
  if (!is_array_value(template_value)) return;
  g_menu_targets = [NSMutableArray array];
  NSApp.mainMenu = BuildNativeMenu(js, template_value, @"");
}

ant_value_t DesktopMenuAppend(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t menu = js_getthis(js);
  ant_value_t template_value = js_get(js, menu, "_template");
  if (!is_array_value(template_value) || nargs < 1 || !is_object_type(args[0])) {
    return js_mkerr(js, "menu.append(item) requires a MenuItem");
  }
  js_arr_push(js, template_value, args[0]);
  RefreshApplicationMenu(js, menu);
  return js_mkundef();
}

ant_value_t DesktopMenuInsert(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t menu = js_getthis(js);
  ant_value_t template_value = js_get(js, menu, "_template");
  if (!is_array_value(template_value) || nargs < 2 || vtype(args[0]) != kTypeNumber || !is_object_type(args[1])) {
    return js_mkerr(js, "menu.insert(position, item) requires an index and MenuItem");
  }
  ant_offset_t count = js_arr_len(js, template_value);
  ant_offset_t position = (ant_offset_t)MAX(0, js_getnum(args[0]));
  if (position > count) position = count;
  ant_value_t replacement = js_mkarr(js);
  for (ant_offset_t index = 0; index < count; index++) {
    if (index == position) js_arr_push(js, replacement, args[1]);
    js_arr_push(js, replacement, js_arr_get(js, template_value, index));
  }
  if (position == count) js_arr_push(js, replacement, args[1]);
  js_set(js, menu, "_template", replacement);
  js_set(js, menu, "items", replacement);
  RefreshApplicationMenu(js, menu);
  return js_mkundef();
}

ant_value_t DesktopMenuBuildFromTemplate(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_array_value(args[0])) {
    return js_mkerr(js, "Menu.buildFromTemplate(template) requires an array");
  }
  ant_value_t menu = js_mkobj(js);
  ant_desktop_state_t *state = ant_desktop_state_from(js_getthis(js));
  if (!state) return js_mkerr(js, "invalid Menu receiver");
  ant_desktop_state_attach(menu, state);
  js_set(js, menu, "_template", args[0]);
  js_set(js, menu, "items", args[0]);
  js_set(js, menu, "append", js_mkfun(DesktopMenuAppend));
  js_set(js, menu, "insert", js_mkfun(DesktopMenuInsert));
  return menu;
}

ant_value_t DesktopGetApplicationMenu(ant_t *js, ant_value_t *args, int nargs) {
  (void)args;
  (void)nargs;
  ant_desktop_state_t *state = ant_desktop_state_from(js_getthis(js));
  return state ? state->application_menu : js_mkundef();
}

ant_value_t DesktopSetApplicationMenu(ant_t *js, ant_value_t *args, int nargs) {
  ant_desktop_state_t *state = ant_desktop_state_from(js_getthis(js));
  if (!state) return js_mkerr(js, "invalid application menu receiver");
  if (nargs < 1 || vtype(args[0]) == kTypeNull || vtype(args[0]) == kTypeUndefined) {
    NSApp.mainMenu = nil;
    state->application_menu = js_mkundef();
    js_set_slot_wb(js, state->app, SLOT_AUX, js_mkundef());
    return js_mkundef();
  }
  ant_value_t template_value = is_array_value(args[0]) ? args[0] : js_get(js, args[0], "_template");
  if (!is_array_value(template_value)) {
    return js_mkerr(js, "setApplicationMenu(menu) requires a Menu or template array");
  }
  g_menu_targets = [NSMutableArray array];
  NSApp.mainMenu = BuildNativeMenu(js, template_value, @"");
  state->application_menu = args[0];
  js_set_slot_wb(js, state->app, SLOT_AUX, args[0]);
  return js_mkundef();
}

ant_value_t DesktopMenuItemCtor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t item = js_getthis(js);
  if (!is_object_type(item)) item = js_newobj(js);
  if (nargs > 0 && is_object_type(args[0])) js_merge_obj(js, item, args[0]);
  return item;
}
