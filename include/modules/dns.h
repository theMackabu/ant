#pragma once

#include "types.h"

ant_value_t dns_library(ant_t *js);
ant_value_t dns_promises_library(ant_t *js);
void cleanup_dns_module(ant_t *js);
