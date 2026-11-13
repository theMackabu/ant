#pragma once

#include <stdbool.h>
#include <uthash.h>
#include "internal.h"

typedef struct sym_registry_entry {
  const char *key;
  ant_value_t sym;
  UT_hash_handle hh;
} sym_registry_entry_t;

bool js_is_symbol_description_getter(ant_value_t getter);

void init_symbol_module(ant_t *js);
void init_symbol_ctor(ant_t *js);
void init_well_known_symbols(ant_t *js);
void js_define_species_getter(ant_t *js, ant_value_t ctor);

ant_value_t js_return_this(ant_params_t);
ant_value_t js_symbol_description_value(ant_t *js, ant_value_t symbol);

ant_value_t maybe_call_symbol_method(
  ant_t *js, ant_value_t target, ant_value_t sym,
  ant_value_t this_arg, ant_args_t, bool *called
);
