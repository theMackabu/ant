#ifndef READLINE_H
#define READLINE_H

#include "types.h"
#include <stdbool.h>

ant_value_t readline_library(ant_t *js);
ant_value_t readline_promises_library(ant_t *js);

bool has_active_readline_interfaces(void);

#endif
