#pragma once

#include "types.h"
#include <stdbool.h>

__attribute__((cold)) ant_value_t readline_library(ant_t *js);
__attribute__((cold)) ant_value_t readline_promises_library(ant_t *js);

bool has_active_readline_interfaces(ant_t *js);
void cleanup_readline_module(ant_t *js);
