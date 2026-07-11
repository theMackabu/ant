#ifndef ANT_DESKTOP_CEF_RUNTIME_H
#define ANT_DESKTOP_CEF_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ant_desktop_cef_initialize(int argc, char **argv);
bool ant_desktop_devtools_enabled(void);
void ant_desktop_cef_do_message_loop_work(void);
int64_t ant_desktop_cef_next_message_delay_ms(void);
void ant_desktop_cef_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
