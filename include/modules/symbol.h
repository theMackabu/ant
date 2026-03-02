#ifndef SYMBOL_H
#define SYMBOL_H

#include <stdbool.h>
#include "internal.h" // IWYU pragma: keep

void init_symbol_module(void);
void js_define_species_getter(ant_t *js, jsval_t ctor);
void symbol_gc_update_roots(GC_OP_VAL_ARGS);

jsval_t sym_lookup_by_id(uint32_t id);
void sym_gc_update_all(void (*op_val)(void *, jsval_t *), void *ctx);

jsval_t maybe_call_symbol_method(
  ant_t *js, jsval_t target, jsval_t sym,
  jsval_t this_arg, jsval_t *args,
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

#define DECL_GET_SYM(name, _desc) jsval_t get_##name##_sym(void);
WELLKNOWN_SYMBOLS(DECL_GET_SYM)
#undef DECL_GET_SYM

static inline jsval_t sym_this_cb(ant_t *js, jsval_t *args, int nargs) {
  return js->this_val;
}

#endif
