#ifndef ANT_UNICODE_H
#define ANT_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <utf8proc.h>

typedef enum {
  UNICODE_NFC,
  UNICODE_NFD,
  UNICODE_NFKC,
  UNICODE_NFKD,
} unicode_form_t;

int32_t unicode_toupper(int32_t cp);
int32_t unicode_tolower(int32_t cp);

int unicode_width(int32_t cp);
utf8proc_category_t unicode_category(int32_t cp);

bool unicode_is_id_start(uint32_t c);
bool unicode_is_id_continue(uint32_t c);

bool unicode_is_normalized(const char *str, size_t len, unicode_form_t form);
char *unicode_normalize(const char *str, size_t len, unicode_form_t form, size_t *out_len);

#endif
