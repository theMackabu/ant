#include "cef_callbacks.h"

#include "../../api/desktop_core.h"
#include "../platform.h"

void ant_desktop_cef_resolve_load(void *value) {
  ant_desktop_resolve_load(value);
}

void ant_desktop_cef_reject_load(void *value, const char *message) {
  ant_desktop_reject_load(value, message);
}

void ant_desktop_cef_emit_event(void *value, const char *type, const char *detail, size_t detail_length, int64_t code) {
  ant_desktop_emit_window_event(value, type, detail, detail_length, code);
}

void ant_desktop_cef_dispatch_ipc(void *value, int operation, uint64_t request_id, const char *channel,
                                  size_t channel_length, const char *payload, size_t payload_length) {
  ant_desktop_dispatch_renderer_ipc(value, operation, request_id, channel, channel_length, payload, payload_length);
}

void ant_desktop_cef_set_devtools(void *value, int opened) {
  ((ant_desktop_window_state_t *)value)->devtools_open = opened != 0;
}

void ant_desktop_cef_show_window(void *value) {
  ant_desktop_platform_show(value);
}

void ant_desktop_cef_begin_window_close(void *value) {
  ant_desktop_platform_begin_browser_close(value);
}

void ant_desktop_cef_browser_closed(void *value) {
  ant_desktop_platform_finish_browser_close(value);
}
