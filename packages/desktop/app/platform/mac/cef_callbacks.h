#ifndef ANT_DESKTOP_CEF_CALLBACKS_H
#define ANT_DESKTOP_CEF_CALLBACKS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ant_desktop_cef_resolve_load(void *window);
void ant_desktop_cef_reject_load(void *window, const char *message);
void ant_desktop_cef_emit_event(void *window, const char *type, const char *detail, size_t detail_length, int64_t code);
void ant_desktop_cef_dispatch_ipc(void *window, int operation, uint64_t request_id, const char *channel,
                                  size_t channel_length, const char *payload, size_t payload_length);
void ant_desktop_cef_set_devtools(void *window, int opened);
void ant_desktop_cef_show_window(void *window);
void ant_desktop_cef_begin_window_close(void *window);
void ant_desktop_cef_browser_closed(void *window);
void ant_desktop_request_termination(void);

#ifdef __cplusplus
}
#endif

#endif
