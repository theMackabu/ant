#ifndef ANT_API_ARRAYS_H
#define ANT_API_ARRAYS_H

#include "ant/base.h"

ant_value_t Ant_Array_Create(ant_t *);

ant_offset_t js_arr_len(ant_t *js, ant_value_t arr);
ant_value_t js_arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx);

typedef bool (*js_iter_fn)(ant_t *js, ant_value_t value, void *udata);
bool js_iter(ant_t *js, ant_value_t iterable, js_iter_fn callback, void *udata);

void js_arr_push(ant_t *, ant_value_t arr, ant_value_t val);
void js_arr_reserve(ant_t *, ant_value_t arr, uint32_t n);

#endif
