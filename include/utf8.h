#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdint.h>

int utf8_sequence_length(unsigned char first_byte);
int utf8_encode(uint32_t codepoint, char *out);
uint32_t utf8_decode(const unsigned char *buf, size_t len, int *seq_len);

size_t utf8_strlen(const char *str, size_t byte_len);
size_t utf16_strlen(const char *str, size_t byte_len);

#endif
