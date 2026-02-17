#ifndef BASE64_H
#define BASE64_H

#include <stdint.h>
#include <stddef.h>

char *ant_base64_encode(const uint8_t *data, size_t len, size_t *out_len);
uint8_t *ant_base64_decode(const char *data, size_t len, size_t *out_len);

#endif
