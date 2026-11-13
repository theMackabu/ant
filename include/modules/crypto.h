#pragma once

#include <stddef.h>
#include "types.h"

__attribute__((cold)) void init_crypto_module(ant_t *js);
__attribute__((cold)) ant_value_t crypto_library(ant_t *js);

int crypto_fill_random(void *buf, size_t len);
int crypto_random_uuid(char out[37]);
