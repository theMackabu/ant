#ifndef SYMBOL_H
#define SYMBOL_H

#include <stdbool.h>
#include "internal.h" // IWYU pragma: keep

void init_symbol_module(void);
void js_define_species_getter(ant_t *js, ant_value_t ctor);

#define ITER_STATE_PACK(kind, n)  ((uint32_t)(kind) << 28 | ((uint32_t)(n) & 0x0FFFFFFFU))
#define ITER_STATE_KIND(v)        ((uint32_t)(v) >> 28)
#define ITER_STATE_INDEX(v)       ((uint32_t)(v) & 0x0FFFFFFFU)

enum { ARR_ITER_VALUES = 0, ARR_ITER_KEYS = 1, ARR_ITER_ENTRIES = 2 };
ant_value_t make_array_iterator(ant_t *js, ant_value_t array, int kind);

typedef struct js_iter_t js_iter_t;
typedef bool (*js_iter_advance_fn)(ant_t *js, js_iter_t *it, ant_value_t *out);

struct js_iter_t {
  ant_value_t iterator;
  ant_value_t next_fn;
  js_iter_advance_fn advance;
};

void js_iter_register_advance(ant_value_t proto, js_iter_advance_fn fn);
bool js_iter_open(ant_t *js, ant_value_t iterable, js_iter_t *it);
bool js_iter_next(ant_t *js, js_iter_t *it, ant_value_t *out);
void js_iter_close(ant_t *js, js_iter_t *it);

ant_value_t maybe_call_symbol_method(
  ant_t *js, ant_value_t target, ant_value_t sym,
  ant_value_t this_arg, ant_value_t *args,
  int nargs, bool *called
);

#define WELLKNOWN_SYMBOLS(X)                           \
  X(iterator,            "Symbol.iterator")            \
  X(asyncIterator,       "Symbol.asyncIterator")       \
  X(inspect,             "Symbol.inspect")             \
  X(toStringTag,         "Symbol.toStringTag")         \
  X(hasInstance,         "Symbol.hasInstance")         \
  X(match,               "Symbol.match")               \
  X(replace,             "Symbol.replace")             \
  X(search,              "Symbol.search")              \
  X(split,               "Symbol.split")               \
  X(matchAll,            "Symbol.matchAll")            \
  X(isConcatSpreadable,  "Symbol.isConcatSpreadable")  \
  X(observable,          "Symbol.observable")          \
  X(toPrimitive,         "Symbol.toPrimitive")         \
  X(species,             "Symbol.species")             \
  X(unscopables,         "Symbol.unscopables")         \
  X(default,             "Symbol.default")

#define DECL_GET_SYM(name, _desc) ant_value_t get_##name##_sym(void);
WELLKNOWN_SYMBOLS(DECL_GET_SYM)
#undef DECL_GET_SYM

static inline ant_value_t sym_this_cb(ant_t *js, ant_value_t *args, int nargs) {
  return js->this_val;
}

static inline ant_value_t js_iter_result(ant_t *js, bool has_value, ant_value_t value) {
  ant_value_t result = js_mkobj(js);
  if (__builtin_expect(has_value, 1)) {
    js_set(js, result, "done", js_false);
    js_set(js, result, "value", value);
  } else {
    js_set(js, result, "done", js_true);
    js_set(js, result, "value", js_mkundef());
  }
  return result;
}

#endif
