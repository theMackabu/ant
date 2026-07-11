#ifndef ANT_DESKTOP_CEF_RENDERER_ANT_RUNTIME_H
#define ANT_DESKTOP_CEF_RENDERER_ANT_RUNTIME_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ant_renderer_runtime_initialize(void);
void ant_renderer_runtime_set_stack_base(void *stack_base);
char *ant_renderer_runtime_describe(const char *specifier);
char *ant_renderer_runtime_call(const char *specifier, const char *name, const char *arguments_json);
void ant_renderer_runtime_free(char *value);
void ant_renderer_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
