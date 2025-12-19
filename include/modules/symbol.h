#ifndef SYMBOL_H
#define SYMBOL_H

#include "ant.h"

void init_symbol_module(void);
jsval_t get_iterator_symbol(void);
const char *get_iterator_sym_key(void);

#endif
