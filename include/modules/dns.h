#pragma once

#include "types.h"

void cleanup_dns_module(ant_t *js);

__attribute__((cold)) ant_value_t dns_library(ant_t *js);
__attribute__((cold)) ant_value_t dns_promises_library(ant_t *js);
