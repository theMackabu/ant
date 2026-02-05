#ifndef SYMBOL_H
#define SYMBOL_H

#include <stddef.h>

void init_symbol_module(void);

const char *get_iterator_sym_key(void);
const char *get_asyncIterator_sym_key(void);
const char *get_toStringTag_sym_key(void);
const char *get_observable_sym_key(void);
const char *get_toPrimitive_sym_key(void);
const char *get_hasInstance_sym_key(void);

const char *get_symbol_description_from_key(
  const char *sym_key,
  size_t key_len
);

#endif
