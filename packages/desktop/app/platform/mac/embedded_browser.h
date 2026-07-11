#ifndef ANT_DESKTOP_EMBEDDED_BROWSER_H
#define ANT_DESKTOP_EMBEDDED_BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ant_desktop_browser_create(void *window, void *parent_view, const char *url, const char *app_root,
                                const char *capabilities, const char *preload_path, bool sandbox, bool node_integration,
                                bool context_isolation, bool transparent);
bool ant_desktop_browser_running(void *window);
bool ant_desktop_browser_closing(void *window);
void ant_desktop_browser_close(void *window);
void ant_desktop_browser_detach(void *window);
void ant_desktop_browser_open_devtools(void *window);
void ant_desktop_browser_close_devtools(void *window);
void ant_desktop_browser_toggle_devtools(void *window);
void ant_desktop_browser_inspect(void *window, int x, int y);
void ant_desktop_browser_reload(void *window);
bool ant_desktop_browser_send_ipc(void *window, int operation, uint64_t request_id, const char *channel,
                                  size_t channel_length, const char *payload, size_t payload_length);
size_t ant_desktop_browser_count(void);

#ifdef __cplusplus
}
#endif

#endif
