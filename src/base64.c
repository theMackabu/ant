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

char *ant_base64url_encode(const uint8_t *data, size_t len, size_t *out_len) {
  char *result = ant_base64_encode(data, len, out_len);
  if (!result) return NULL;

  for (size_t i = 0; i < *out_len; i++) {
    if (result[i] == '+') result[i] = '-';
    else if (result[i] == '/') result[i] = '_';
  }
  
  while (*out_len > 0 && result[*out_len - 1] == '=') (*out_len)--;
  result[*out_len] = '\0';
  
  return result;
}

static int base64_loose_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

uint8_t *ant_base64_decode_loose(const char *data, size_t len, size_t *out_len) {
  uint8_t *result = malloc((len / 4) * 3 + 3);
  if (!result) return NULL;
  if (base64_decode(data, len, (char *)result, out_len, 0)) return result;

  size_t written = 0;
  uint32_t acc = 0;
  int bits = 0;

  for (size_t i = 0; i < len && data[i] != '='; i++) {
    int value = base64_loose_value((unsigned char)data[i]);
    if (value < 0) continue;
    
    acc = (acc << 6) | (uint32_t)value;
    bits += 6;
    
    if (bits >= 8) {
      bits -= 8;
      result[written++] = (uint8_t)(acc >> bits);
    }
  }

  *out_len = written;
  return result;
}
