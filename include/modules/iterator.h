#ifndef ITERATOR_H
#define ITERATOR_H

#include "types.h"
#include "internal.h"

typedef enum: uint32_t {
  ARR_ITER_VALUES = 0,
  ARR_ITER_KEYS = 1,
  ARR_ITER_ENTRIES = 2
} array_iter_kind_t;

typedef bool (*js_iter_advance_fn)(
  ant_t *js,
  iterator_t *it,
  ant_value_t *out
);

struct iterator {
  ant_value_t iterator;
  ant_value_t next_fn;
  js_iter_advance_fn advance;
};

static constexpr uint32_t ITER_STATE_KIND_SHIFT = 28;
static constexpr uint32_t ITER_STATE_INDEX_MASK = 0x0FFFFFFFU;

static inline uint32_t iter_state_pack(uint32_t kind, uint32_t index) {
  return (kind << ITER_STATE_KIND_SHIFT) | (index & ITER_STATE_INDEX_MASK);
}

static inline uint32_t iter_state_kind(uint32_t state) {
  return state >> ITER_STATE_KIND_SHIFT;
}

static inline uint32_t iter_state_index(uint32_t state) {
  return state & ITER_STATE_INDEX_MASK;
}

void init_iterator_module(ant_t *js);
void cleanup_iterator_module(ant_t *js);
void js_iter_close(ant_t *js, iterator_t *it);
void js_iter_register_advance(ant_t *js, ant_value_t proto, js_iter_advance_fn fn);

bool js_iter_open(ant_t *js, ant_value_t iterable, iterator_t *it);
bool js_iter_next(ant_t *js, iterator_t *it, ant_value_t *out);
bool js_iter_is_array_values(ant_value_t iterator, ant_value_t next, ant_value_t source);

ant_value_t js_iter_result(ant_t *js, bool has_value, ant_value_t value);
ant_value_t make_array_iterator(ant_t *js, ant_value_t array, array_iter_kind_t kind);

static inline ant_value_t js_iter_next_result(ant_t *js, js_iter_advance_fn advance) {
  iterator_t it = { .iterator = js->this_val };
  ant_value_t value = js_mkundef();
  bool has_value = advance(js, &it, &value);
  return js_iter_result(js, has_value, value);
}

#endif
