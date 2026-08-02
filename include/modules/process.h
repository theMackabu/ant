#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

#include <stdint.h>
#include <stddef.h>

void init_process_module(ant_t *js);
ant_value_t process_library(ant_t *js);

int process_signal_number(const char *name);
const char *process_signal_name(int signum);

void process_refresh_sandbox_argv(ant_t *js);
void process_set_sandbox_terminal(ant_t *js, uint32_t capabilities, uint16_t rows, uint16_t cols);

void process_enable_keypress_events(ant_t *js);
void emit_process_event(ant_t *js, const char *event_type, ant_value_t *args, int nargs);

bool has_active_stdin(ant_t *js);
bool process_has_event_listeners(ant_t *js, const char *event_type);

typedef void (*stdin_byte_consumer_fn)(ant_t *js, const char *buf, size_t len);
typedef void (*stdin_eof_fn)(ant_t *js);

void process_stdin_attach_reader(ant_t *js, stdin_byte_consumer_fn on_bytes, stdin_eof_fn on_eof);
void process_stdin_detach_reader(ant_t *js);

#endif
