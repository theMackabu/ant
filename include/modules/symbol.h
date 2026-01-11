#ifndef SYMBOL_H
#define SYMBOL_H

#include "ant.h"

void init_symbol_module(void);
jsval_t get_iterator_symbol(void);
jsval_t get_asyncIterator_symbol(void);

const char *get_iterator_sym_key(void);
const char *get_asyncIterator_sym_key(void);
const char *get_toStringTag_sym_key(void);
const char *get_symbol_description_from_key(const char *sym_key, size_t key_len);

#endif
