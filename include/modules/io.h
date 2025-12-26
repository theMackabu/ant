#ifndef IO_H
#define IO_H

#include <stdio.h>
#include <stdbool.h>

extern bool io_no_color;

void init_console_module(void);
void print_value_colored(const char *str, FILE *stream);

#endif
