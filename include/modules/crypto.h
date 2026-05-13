#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include "types.h"

void init_crypto_module(void);
ant_value_t crypto_library(ant_t *js);

int crypto_fill_random(void *buf, size_t len);
int crypto_random_uuid(char out[37]);

#endif
