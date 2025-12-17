#ifndef CRYPTO_H
#define CRYPTO_H

#include "ant.h"

void init_crypto_module();
jsval_t crypto_library(struct js *js);

#endif