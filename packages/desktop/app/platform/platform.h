#ifndef ANT_DESKTOP_PLATFORM_H
#define ANT_DESKTOP_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#include "../core/window_state.h"

typedef struct ant_desktop_window_bounds {
  double x;
  double y;
  double width;
  double height;
} ant_desktop_window_bounds_t;

bool ant_desktop_platform_browser_running(ant_desktop_window_state_t *window);
bool ant_desktop_platform_open_devtools(ant_desktop_window_state_t *window);
bool ant_desktop_platform_close_devtools(ant_desktop_window_state_t *window);
bool ant_desktop_platform_toggle_devtools(ant_desktop_window_state_t *window);
bool ant_desktop_platform_inspect(ant_desktop_window_state_t *window, int x, int y);
bool ant_desktop_platform_reload(ant_desktop_window_state_t *window);
bool ant_desktop_platform_send_ipc(ant_desktop_window_state_t *window, int operation, uint64_t request_id,
                                   const char *channel, size_t channel_length, const char *payload,
                                   size_t payload_length);
bool ant_desktop_platform_get_bounds(ant_desktop_window_state_t *window, ant_desktop_window_bounds_t *bounds);
bool ant_desktop_platform_get_path(const char *name, char *path, size_t capacity);
void ant_desktop_platform_close(ant_desktop_window_state_t *window);
void ant_desktop_platform_show(ant_desktop_window_state_t *window);
void ant_desktop_platform_hide(ant_desktop_window_state_t *window);
void ant_desktop_platform_minimize(ant_desktop_window_state_t *window);
void ant_desktop_platform_restore(ant_desktop_window_state_t *window);
void ant_desktop_platform_maximize(ant_desktop_window_state_t *window);
void ant_desktop_platform_set_always_on_top(ant_desktop_window_state_t *window, bool enabled);
void ant_desktop_platform_set_title(ant_desktop_window_state_t *window, const char *title, size_t length);
void ant_desktop_platform_set_full_screen(ant_desktop_window_state_t *window, bool enabled);
void ant_desktop_platform_quit(void);
void ant_desktop_platform_shutdown_all_windows(void);
void ant_desktop_platform_begin_browser_close(ant_desktop_window_state_t *window);
void ant_desktop_platform_finish_browser_close(ant_desktop_window_state_t *window);
const char *ant_desktop_platform_resources_path(void);

#endif
