#ifndef ANT_TTY_MODULE_H
#define ANT_TTY_MODULE_H

#include "types.h"

ant_value_t tty_library(ant_t *js);

void init_tty_module(void);
void tty_set_sandbox_terminal(uint32_t capabilities, uint16_t rows, uint16_t cols);

bool tty_set_raw_mode(int fd, bool enable);
bool tty_is_raw_mode(int fd);

#endif
