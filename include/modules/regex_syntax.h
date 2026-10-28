#ifndef REGEX_SYNTAX_H
#define REGEX_SYNTAX_H

#include <stdbool.h>
#include <stddef.h>

const char *regexp_syntax_error(const char *src, size_t len, bool unicode, bool unicode_sets);

#endif
