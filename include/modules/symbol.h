#ifndef SYMBOL_H
#define SYMBOL_H

#include <stdbool.h>
#include "internal.h" // IWYU pragma: keep

void init_symbol_module(void);
void js_define_species_getter(ant_t *js, ant_value_t ctor);

#define ARR_ITER_PACK(kind, idx) ((uint32_t)(kind) << 28 | (uint32_t)(idx))
#define ARR_ITER_KIND(v)         ((uint32_t)(v) >> 28)
#define ARR_ITER_INDEX(v)        ((uint32_t)(v) & 0x0FFFFFFFU)

enum { ARR_ITER_VALUES = 0, ARR_ITER_KEYS = 1, ARR_ITER_ENTRIES = 2 };
ant_value_t make_array_iterator(ant_t *js, ant_value_t array, int kind);

ant_value_t maybe_call_symbol_method(
  ant_t *js, ant_value_t target, ant_value_t sym,
  ant_value_t this_arg, ant_value_t *args,
  int nargs, bool *called
);

#define WELLKNOWN_SYMBOLS(X)                           \
  X(iterator,            "Symbol.iterator")            \
  X(asyncIterator,       "Symbol.asyncIterator")       \
  X(toStringTag,         "Symbol.toStringTag")         \
  X(hasInstance,         "Symbol.hasInstance")         \
  X(match,               "Symbol.match")               \
  X(replace,             "Symbol.replace")             \
  X(search,              "Symbol.search")              \
  X(split,               "Symbol.split")               \
  X(isConcatSpreadable,  "Symbol.isConcatSpreadable")  \
  X(observable,          "Symbol.observable")          \
  X(toPrimitive,         "Symbol.toPrimitive")         \
  X(species,             "Symbol.species")             \
  X(unscopables,         "Symbol.unscopables")

#define DECL_GET_SYM(name, _desc) ant_value_t get_##name##_sym(void);
WELLKNOWN_SYMBOLS(DECL_GET_SYM)
#undef DECL_GET_SYM

static inline ant_value_t sym_this_cb(ant_t *js, ant_value_t *args, int nargs) {
  return js->this_val;
}

#endif
