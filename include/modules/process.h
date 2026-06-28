#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

#include <stdint.h>
#include <stddef.h>

void init_process_module(void);
ant_value_t process_library(ant_t *js);

void process_refresh_sandbox_argv(void);
void process_set_sandbox_terminal(uint32_t capabilities, uint16_t rows, uint16_t cols);

void process_enable_keypress_events(void);
void emit_process_event(const char *event_type, ant_value_t *args, int nargs);

bool has_active_stdin(void);
bool process_has_event_listeners(const char *event_type);

typedef void (*stdin_byte_consumer_fn)(const char *buf, size_t len);
typedef void (*stdin_eof_fn)(void);

void process_stdin_attach_reader(stdin_byte_consumer_fn on_bytes, stdin_eof_fn on_eof);
void process_stdin_detach_reader(void);

#endif
