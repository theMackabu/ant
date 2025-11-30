#include <sodium.h>
#include <string.h>
#include <time.h>
#include <uuidv7.h>
#include <uuid/uuid.h>

#include "modules/crypto.h"

static int ensure_crypto_init(struct js *js) {
  static int crypto_initialized = 0;
  
  if (!crypto_initialized) {
    if (sodium_init() < 0) return -1;
    crypto_initialized = 1;
  }
  return 0;
}

int uuidv7_new(uint8_t *uuid_out) {
  static uint8_t uuid_prev[16] = {0};
  static uint8_t rand_bytes[256] = {0};
  static size_t n_rand_consumed = sizeof(rand_bytes);
  
  struct timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);
  uint64_t unix_ts_ms = (uint64_t)tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
  
  if (n_rand_consumed > sizeof(rand_bytes) - 10) {
    randombytes_buf(rand_bytes, sizeof(rand_bytes));
    n_rand_consumed = 0;
  }
  
  int8_t status = uuidv7_generate(uuid_prev, unix_ts_ms, &rand_bytes[n_rand_consumed], uuid_prev);
  n_rand_consumed += uuidv7_status_n_rand_consumed(status);
  
  memcpy(uuid_out, uuid_prev, 16);
  return status;
}

// Ant.crypto.random()
static jsval_t js_crypto_random(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  (void) js;
  
  if (ensure_crypto_init(js) < 0) {
    return js_mkerr(js, "libsodium initialization failed");
  }
  
  unsigned int value = randombytes_random();
  return js_mknum((double)value);
}

// Ant.Crypto.randomBytes(length)
static jsval_t js_crypto_random_bytes(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "randomBytes requires a length argument");
  }
  
  if (ensure_crypto_init(js) < 0) {
    return js_mkerr(js, "libsodium initialization failed");
  }
  
  int length = (int)js_getnum(args[0]);
  
  if (length <= 0 || length > 65536) {
    return js_mkerr(js, "invalid length");
  }
  
  unsigned char *random_bytes = malloc(length);
  if (random_bytes == NULL) {
    return js_mkerr(js, "memory allocation failed");
  }
  
  randombytes_buf(random_bytes, length);
  
  jsval_t array = js_mkobj(js);
  js_set(js, array, "length", js_mknum((double)length));
  
  for (int i = 0; i < length; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%d", i);
    js_set(js, array, key, js_mknum((double)random_bytes[i]));
  }
  
  free(random_bytes);
  return array;
}

// Ant.Crypto.randomUUID()
static jsval_t js_crypto_random_uuid(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  
  if (ensure_crypto_init(js) < 0) {
    return js_mkerr(js, "libsodium initialization failed");
  }
  
  uuid_t uuid;
  char uuid_str[37];
  
  uuid_generate_random(uuid);
  uuid_unparse_lower(uuid, uuid_str);
  
  return js_mkstr(js, uuid_str, strlen(uuid_str));
}

// Ant.Crypto.randomUUIDv7()
static jsval_t js_crypto_random_uuidv7(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  
  if (ensure_crypto_init(js) < 0) {
    return js_mkerr(js, "libsodium initialization failed");
  }
  
  uint8_t uuid[16];
  char uuid_str[37];
  
  int result = uuidv7_new(uuid);
  if (result < 0) {
    return js_mkerr(js, "UUIDv7 generation failed");
  }
  
  uuidv7_to_string(uuid, uuid_str);
  
  return js_mkstr(js, uuid_str, strlen(uuid_str));
}

void init_crypto_module(struct js *js, jsval_t ant_obj) {
  jsval_t crypto_obj = js_mkobj(js);
  js_set(js, ant_obj, "Crypto", crypto_obj);
  
  js_set(js, crypto_obj, "random", js_mkfun(js_crypto_random));
  js_set(js, crypto_obj, "randomBytes", js_mkfun(js_crypto_random_bytes));
  js_set(js, crypto_obj, "randomUUID", js_mkfun(js_crypto_random_uuid));
  js_set(js, crypto_obj, "randomUUIDv7", js_mkfun(js_crypto_random_uuidv7));
}

