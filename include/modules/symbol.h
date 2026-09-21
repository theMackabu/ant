// TODO: move other symbol related stuff out of ant.c

#ifndef SYMBOL_H
#define SYMBOL_H

#include <stdbool.h>
#include "internal.h" // IWYU pragma: keep

void init_symbol_module(ant_t *js);
void js_define_species_getter(ant_t *js, ant_value_t ctor);

bool js_is_symbol_description_getter(ant_value_t getter);
ant_value_t js_symbol_description_value(ant_t *js, ant_value_t symbol);

ant_value_t maybe_call_symbol_method(
  ant_t *js, ant_value_t target, ant_value_t sym,
  ant_value_t this_arg, ant_value_t *args,
  int nargs, bool *called
);

static inline ant_value_t sym_this_cb(ant_params_t) {
  return js->this_val;
}

#endif
