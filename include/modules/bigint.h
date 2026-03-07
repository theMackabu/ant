#ifndef BIGINT_MODULE_H
#define BIGINT_MODULE_H

#include "types.h"

void init_bigint_module(void);

ant_value_t bigint_add(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_sub(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_mul(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_div(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_mod(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_neg(ant_t *js, ant_value_t a);
ant_value_t bigint_exp(ant_t *js, ant_value_t base, ant_value_t exp);

ant_value_t bigint_shift_left(ant_t *js, ant_value_t value, uint64_t shift);
ant_value_t bigint_shift_right(ant_t *js, ant_value_t value, uint64_t shift);
ant_value_t bigint_shift_right_logical(ant_t *js, ant_value_t value, uint64_t shift);
ant_value_t bigint_bitand(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_bitor(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_bitxor(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_bitnot(ant_t *js, ant_value_t value);
ant_value_t bigint_asint_bits(ant_t *js, ant_value_t arg, uint64_t *bits_out);

bool bigint_is_negative(ant_t *js, ant_value_t v);
bool bigint_is_zero(ant_t *js, ant_value_t v);

size_t bigint_digits_len(ant_t *js, ant_value_t v);
size_t strbigint(ant_t *js, ant_value_t value, char *buf, size_t len);
int bigint_compare(ant_t *js, ant_value_t a, ant_value_t b);

#endif
