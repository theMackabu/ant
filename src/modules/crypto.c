#include <sodium.h>
#include <string.h>
#include <uuidv7.h>
#include <uuid/uuid.h>

#include "modules/crypto.h"

// Ant.crypto.random()
static jsval_t js_crypto_random(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  (void) js;
  
  static int initialized = 0;
  if (!initialized) {
    if (sodium_init() < 0) {
      return js_mkerr(js, "libsodium initialization failed");
    }
    initialized = 1;
  }
  
  unsigned int value = randombytes_random();
  return js_mknum((double)value);
}

// Ant.Crypto.randomBytes(length)
static jsval_t js_crypto_random_bytes(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "randomBytes requires a length argument");
  }
  
  static int initialized = 0;
  if (!initialized) {
    if (sodium_init() < 0) {
      return js_mkerr(js, "libsodium initialization failed");
    }
    initialized = 1;
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
  
  static int initialized = 0;
  if (!initialized) {
    if (sodium_init() < 0) {
      return js_mkerr(js, "libsodium initialization failed");
    }
    initialized = 1;
  }
  
  uuid_t uuid;
  char uuid_str[37];
  
  uuid_generate_random(uuid);
  uuid_unparse_lower(uuid, uuid_str);
  
  return js_mkstr(js, uuid_str, strlen(uuid_str));
}

void init_crypto_module(struct js *js, jsval_t ant_obj) {
  jsval_t crypto_obj = js_mkobj(js);
  js_set(js, ant_obj, "Crypto", crypto_obj);
  
  js_set(js, crypto_obj, "random", js_mkfun(js_crypto_random));
  js_set(js, crypto_obj, "randomBytes", js_mkfun(js_crypto_random_bytes));
  js_set(js, crypto_obj, "randomUUID", js_mkfun(js_crypto_random_uuid));
}

