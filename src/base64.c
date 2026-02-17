#include <stdlib.h>
#include <libbase64.h>
#include "base64.h"

char *ant_base64_encode(const uint8_t *data, size_t len, size_t *out_len) {
  size_t encoded_len = 4 * ((len + 2) / 3);
  char *result = malloc(encoded_len + 1);
  if (!result) return NULL;
  
  base64_encode((const char *)data, len, result, out_len, 0);
  result[*out_len] = '\0';
  return result;
}

uint8_t *ant_base64_decode(const char *data, size_t len, size_t *out_len) {
  size_t decoded_len = (len / 4) * 3 + 3;
  uint8_t *result = malloc(decoded_len);
  if (!result) return NULL;
  
  if (!base64_decode(data, len, (char *)result, out_len, 0)) {
    free(result);
    return NULL;
  }
  
  return result;
}
