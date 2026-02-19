#ifndef SYMBOL_H
#define SYMBOL_H

#include <stddef.h>
#include <stdbool.h>
#include "types.h"


void init_symbol_module(void);
void js_define_species_getter(ant_t *js, jsval_t ctor);

bool is_symbol_key(const char *key, size_t key_len);
int sym_to_prop_key(jsval_t sym, char *buf, size_t bufsz);

const char *get_iterator_sym_key(void);
const char *get_asyncIterator_sym_key(void);
const char *get_toStringTag_sym_key(void);
const char *get_observable_sym_key(void);
const char *get_toPrimitive_sym_key(void);
const char *get_hasInstance_sym_key(void);
const char *get_match_sym_key(void);
const char *get_replace_sym_key(void);
const char *get_search_sym_key(void);
const char *get_split_sym_key(void);
const char *get_isConcatSpreadable_sym_key(void);
const char *get_species_sym_key(void);
const char *get_unscopables_sym_key(void);

jsval_t get_wellknown_sym_by_key(const char *key, size_t key_len);
const char *get_symbol_description_from_key(const char *sym_key, size_t key_len);

#endif
