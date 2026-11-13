#pragma once

#include "ant/base.h"

bool js_truthy(ant_t *, ant_value_t);

void *js_permanent_alloc(ant_t *, size_t size, size_t align);
char *js_getstr(ant_t *js, ant_value_t val, size_t *len);
ant_value_t Ant_Value_Describe(ant_t *js, ant_value_t value);

uint32_t js_to_uint32(double d);
int32_t js_to_int32(double d);

const char *js_sym_desc(ant_value_t sym);
const char *js_sym_key(ant_value_t sym);
const char *js_str(ant_t *, ant_value_t val);

ant_value_t js_mksym_for(ant_t *, const char *key);
ant_value_t js_symbol_to_string(ant_t *js, ant_value_t sym);

ant_value_t js_mkstr(ant_t *, const void *, size_t);
#define Ant_String_FromLiteral(js, s) js_mkstr(js, s, sizeof(s) - 1)

ant_value_t js_mkstr_permanent(ant_t *, const void *, size_t);
ant_value_t js_mkstr_utf8(ant_t *, const void *, size_t);
ant_value_t js_mkstr_utf8_strip_bom(ant_t *, const void *, size_t);
ant_value_t js_mkstr_wtf8_lossy(ant_t *, const void *, size_t);
ant_value_t js_mkstr_utf8_valid(ant_t *, const void *, size_t, bool ascii);
ant_value_t js_mkstr_utf8_try(ant_t *, const void *, size_t, size_t *out_valid);
ant_value_t js_mkstr_utf8_reserve(ant_t *, size_t len, char **out_bytes);
ant_value_t js_mkstr_utf8_commit(ant_t *, ant_value_t str);
ant_value_t js_mkbigint(ant_t *, const char *digits, size_t len, bool negative);
ant_value_t js_mkbigint_permanent(ant_t *, const char *digits, size_t len, bool negative);

ant_value_t js_mksym(ant_t *, const char *desc);
ant_value_t js_mksym_well_known(ant_t *, const char *desc);
ant_value_t js_tostring_val(ant_t *js, ant_value_t value);
