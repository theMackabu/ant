#ifndef CRYPTO_H
#define CRYPTO_H

#include "types.h"

void init_crypto_module(void);
int ensure_crypto_init(void);

ant_value_t crypto_library(ant_t *js);

#endif
