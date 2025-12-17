#include <sodium.h>
#include <string.h>
#include <time.h>
#include <uuidv7.h>
#include <uuid/uuid.h>

#include "ant.h"
#include "runtime.h"
#include "modules/crypto.h"
#include "modules/buffer.h"

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

// crypto.random()
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

// crypto.randomBytes(length)
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

// crypto.randomUUID()
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

// crypto.randomUUIDv7()
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

// crypto.getRandomValues(typedArray)
static jsval_t js_crypto_get_random_values(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "getRandomValues requires a TypedArray argument");
  }
  
  if (ensure_crypto_init(js) < 0) {
    return js_mkerr(js, "libsodium initialization failed");
  }
  
  jsval_t ta_data_val = js_get(js, args[0], "_typedarray_data");
  if (js_type(ta_data_val) != JS_NUM) {
    return js_mkerr(js, "argument must be a TypedArray");
  }
  
  TypedArrayData *ta_data = (TypedArrayData *)(uintptr_t)js_getnum(ta_data_val);
  if (!ta_data || !ta_data->buffer) {
    return js_mkerr(js, "invalid TypedArray");
  }
  
  if (ta_data->byte_length > 65536) {
    return js_mkerr(js, "TypedArray byte length exceeds 65536");
  }
  
  uint8_t *ptr = ta_data->buffer->data + ta_data->byte_offset;
  randombytes_buf(ptr, ta_data->byte_length);
  
  sync_typedarray_indices(js, args[0], ta_data);
  
  return args[0];
}

static jsval_t create_crypto_obj(struct js *js) {
  jsval_t crypto_obj = js_mkobj(js);
  
  js_set(js, crypto_obj, "random", js_mkfun(js_crypto_random));
  js_set(js, crypto_obj, "randomBytes", js_mkfun(js_crypto_random_bytes));
  js_set(js, crypto_obj, "randomUUID", js_mkfun(js_crypto_random_uuid));
  js_set(js, crypto_obj, "randomUUIDv7", js_mkfun(js_crypto_random_uuidv7));
  js_set(js, crypto_obj, "getRandomValues", js_mkfun(js_crypto_get_random_values));
  
  js_set(js, crypto_obj, "@@toStringTag", js_mkstr(js, "Crypto", 6));
  return crypto_obj;
}

void init_crypto_module() {
  struct js *js = rt->js;
  jsval_t crypto_obj = create_crypto_obj(js);
  js_set(js, js_glob(js), "crypto", crypto_obj);
}

jsval_t crypto_library(struct js *js) {
  jsval_t lib = js_mkobj(js);
  jsval_t webcrypto = create_crypto_obj(js);
  
  js_set(js, lib, "webcrypto", webcrypto);
  js_set(js, lib, "randomBytes", js_mkfun(js_crypto_random_bytes));
  js_set(js, lib, "randomUUID", js_mkfun(js_crypto_random_uuid));
  js_set(js, lib, "getRandomValues", js_mkfun(js_crypto_get_random_values));
  js_set(js, lib, "@@toStringTag", js_mkstr(js, "crypto", 6));
  
  return lib;
}
