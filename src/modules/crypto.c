#include <compat.h> // IWYU pragma: keep

#include <string.h>
#include <time.h>
#include <limits.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <openssl/obj.h>
#include <openssl/rand.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-int-conversion"
#include <uuidv7.h>
#pragma GCC diagnostic pop

#include "ant.h"
#include "ptr.h"
#include "base64.h"
#include "errors.h"
#include "runtime.h"
#include "gc/roots.h"
#include "modules/crypto.h"
#include "modules/buffer.h"
#include "modules/symbol.h"
#include "silver/engine.h"

typedef enum {
  CRYPTO_TEXT_UTF8 = 0,
  CRYPTO_TEXT_HEX,
  CRYPTO_TEXT_BASE64,
  CRYPTO_TEXT_LATIN1
} crypto_text_encoding_t;

typedef struct {
  EVP_MD_CTX *ctx;
  const EVP_MD *md;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len;
  bool finalized;
} ant_hash_state_t;

typedef struct {
  HMAC_CTX *ctx;
  const EVP_MD *md;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len;
  bool finalized;
} ant_hmac_state_t;

typedef enum {
  CRYPTO_KEY_HMAC = 1,
  CRYPTO_KEY_AES_GCM,
} ant_crypto_key_kind_t;

typedef struct {
  ant_crypto_key_kind_t kind;
  const EVP_MD *md;
  uint8_t *key;
  size_t key_len;
} ant_crypto_key_t;

typedef struct {
  EVP_CIPHER_CTX *ctx;
  bool encrypt;
  bool finalized;
  bool is_gcm;
  uint8_t auth_tag[16];
  size_t auth_tag_len;
} ant_cipher_state_t;

enum { 
  CRYPTO_HASH_NATIVE_TAG = 0x48415348u,  // HASH
  CRYPTO_HMAC_NATIVE_TAG = 0x484d4143u,  // HMAC
  CRYPTO_KEY_NATIVE_TAG = 0x434b4559u,   // CKEY
  CRYPTO_CIPHER_NATIVE_TAG = 0x43495048u // CIPH
};

static const char *const k_crypto_cipher_names[] = {
  "aes-128-cbc",
  "aes-128-ctr",
  "aes-128-ecb",
  "aes-128-gcm",
  "aes-128-ofb",
  "aes-192-cbc",
  "aes-192-ctr",
  "aes-192-ecb",
  "aes-192-gcm",
  "aes-192-ofb",
  "aes-256-cbc",
  "aes-256-ctr",
  "aes-256-ecb",
  "aes-256-gcm",
  "aes-256-ofb",
  "des-cbc",
  "des-ecb",
  "des-ede",
  "des-ede-cbc",
  "des-ede3-cbc",
  "rc2-cbc",
  "rc4",
};

static const char *const k_crypto_hash_names[] = {
  "DSA-SHA",
  "DSA-SHA1",
  "MD4",
  "MD5",
  "MD5-SHA1",
  "RSA-MD5",
  "RSA-SHA1",
  "RSA-SHA224",
  "RSA-SHA256",
  "RSA-SHA384",
  "RSA-SHA512",
  "dsaWithSHA",
  "dsaWithSHA1",
  "ecdsa-with-SHA1",
  "md4",
  "md5",
  "md5-sha1",
  "md5WithRSAEncryption",
  "sha1",
  "sha1WithRSAEncryption",
  "sha224",
  "sha224WithRSAEncryption",
  "sha256",
  "sha256WithRSAEncryption",
  "sha384",
  "sha384WithRSAEncryption",
  "sha512",
  "sha512-256",
  "sha512WithRSAEncryption",
};

static const char *const k_crypto_curve_names[] = {
  "prime256v1",
  "secp224r1",
  "secp384r1",
  "secp521r1",
};

typedef enum {
  CRYPTO_NAME_CIPHER = 0,
  CRYPTO_NAME_HASH,
  CRYPTO_NAME_CURVE
} crypto_name_kind_t;

// TODO: keep WebCrypto AlgorithmIdentifier handling aligned with supported subtle algorithms.
static ant_value_t crypto_subtle_get_algorithm_name(ant_t *js, ant_value_t algorithm) {
  if (vtype(algorithm) == T_STR) return js_tostring_val(js, algorithm);
  if (is_object_type(algorithm)) {
    ant_value_t name = js_get(js, algorithm, "name");
    if (is_err(name)) return name;
    return js_tostring_val(js, name);
  }
  return js_mkerr_typed(js, JS_ERR_TYPE, "Algorithm must be a string or object with a name");
}

static void crypto_hash_state_free(ant_hash_state_t *state) {
  if (!state) return;
  if (state->ctx) EVP_MD_CTX_free(state->ctx);
  free(state);
}

static void crypto_hash_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_hash_state_t *state = (ant_hash_state_t *)js_get_native(value, CRYPTO_HASH_NATIVE_TAG);

  if (!state) return;
  js_clear_native(value, CRYPTO_HASH_NATIVE_TAG);
  crypto_hash_state_free(state);
}

static void crypto_hmac_state_free(ant_hmac_state_t *state) {
  if (!state) return;
  if (state->ctx) HMAC_CTX_free(state->ctx);
  free(state);
}

static void crypto_hmac_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_hmac_state_t *state = (ant_hmac_state_t *)js_get_native(value, CRYPTO_HMAC_NATIVE_TAG);

  if (!state) return;
  js_clear_native(value, CRYPTO_HMAC_NATIVE_TAG);
  crypto_hmac_state_free(state);
}

static void crypto_key_free(ant_crypto_key_t *key) {
  if (!key) return;
  if (key->key) {
    OPENSSL_cleanse(key->key, key->key_len);
    free(key->key);
  }
  free(key);
}

static void crypto_key_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_crypto_key_t *key = (ant_crypto_key_t *)js_get_native(value, CRYPTO_KEY_NATIVE_TAG);

  if (!key) return;
  js_clear_native(value, CRYPTO_KEY_NATIVE_TAG);
  crypto_key_free(key);
}

static void crypto_cipher_state_free(ant_cipher_state_t *state) {
  if (!state) return;
  if (state->ctx) EVP_CIPHER_CTX_free(state->ctx);
  free(state);
}

static void crypto_cipher_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_cipher_state_t *state = (ant_cipher_state_t *)js_get_native(value, CRYPTO_CIPHER_NATIVE_TAG);

  if (!state) return;
  js_clear_native(value, CRYPTO_CIPHER_NATIVE_TAG);
  crypto_cipher_state_free(state);
}

int crypto_fill_random(void *buf, size_t len) {
  if (len == 0) return 0;
  if (len > (size_t)INT_MAX) return -1;
  return RAND_bytes((uint8_t *)buf, (int)len) == 1 ? 0 : -1;
}

static inline ant_value_t crypto_random_error(ant_t *js) {
  return js_mkerr(js, "secure random generation failed");
}

static inline void crypto_format_uuid_v4(const uint8_t uuid[16], char out[37]) {
  static char lut[256][2];
  static bool lut_init = false;

  if (!lut_init) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 256; i++) {
      lut[i][0] = hex[(unsigned)i >> 4];
      lut[i][1] = hex[(unsigned)i & 0x0f];
    }
    lut_init = true;
  }

  memcpy(out + 0,  lut[uuid[0]], 2);
  memcpy(out + 2,  lut[uuid[1]], 2);
  memcpy(out + 4,  lut[uuid[2]], 2);
  memcpy(out + 6,  lut[uuid[3]], 2);
  
  out[8] = '-';
  memcpy(out + 9,  lut[uuid[4]], 2);
  memcpy(out + 11, lut[uuid[5]], 2);
  
  out[13] = '-';
  memcpy(out + 14, lut[uuid[6]], 2);
  memcpy(out + 16, lut[uuid[7]], 2);
  
  out[18] = '-';
  memcpy(out + 19, lut[uuid[8]], 2);
  memcpy(out + 21, lut[uuid[9]], 2);
  
  out[23] = '-';
  memcpy(out + 24, lut[uuid[10]], 2);
  memcpy(out + 26, lut[uuid[11]], 2);
  memcpy(out + 28, lut[uuid[12]], 2);
  memcpy(out + 30, lut[uuid[13]], 2);
  memcpy(out + 32, lut[uuid[14]], 2);
  memcpy(out + 34, lut[uuid[15]], 2);
  out[36] = '\0';
}

int crypto_random_uuid(char out[37]) {
  uint8_t uuid[16];
  if (!out || crypto_fill_random(uuid, sizeof(uuid)) < 0) return -1;

  uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40);
  uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);

  crypto_format_uuid_v4(uuid, out);
  return 0;
}

static ant_value_t crypto_make_buffer(ant_t *js, const uint8_t *data, size_t len) {
  ArrayBufferData *buffer = create_array_buffer_data(len);
  if (!buffer) return js_mkerr(js, "Failed to allocate Buffer");
  if (len > 0 && data) memcpy(buffer->data, data, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, len, "Buffer");
}

static bool crypto_get_mutable_bytes(ant_value_t value, uint8_t **out, size_t *len) {
  TypedArrayData *ta = buffer_get_typedarray_data(value);
  if (ta) {
    if (!ta->buffer || ta->buffer->is_detached) return false;
    *out = ta->buffer->data + ta->byte_offset;
    *len = ta->byte_length;
    return true;
  }

  ArrayBufferData *ab = buffer_get_arraybuffer_data(value);
  if (ab) {
    if (ab->is_detached) return false;
    *out = ab->data;
    *len = ab->length;
    return true;
  }

  return false;
}

static crypto_text_encoding_t crypto_parse_encoding(const char *str, size_t len) {
  if (len == 3 && strncasecmp(str, "hex", 3) == 0) return CRYPTO_TEXT_HEX;
  if (len == 6 && strncasecmp(str, "base64", 6) == 0) return CRYPTO_TEXT_BASE64;
  if (len == 9 && strncasecmp(str, "base64url", 9) == 0) return CRYPTO_TEXT_BASE64;
  
  if (
    (len == 5 && strncasecmp(str, "ascii", 5)  == 0) ||
    (len == 6 && strncasecmp(str, "latin1", 6) == 0) ||
    (len == 6 && strncasecmp(str, "binary", 6) == 0)
  ) return CRYPTO_TEXT_LATIN1;
  
  return CRYPTO_TEXT_UTF8;
}

static uint8_t crypto_hex_nibble(char ch) {
  if (ch >= '0' && ch <= '9') return (uint8_t)(ch - '0');
  if (ch >= 'a' && ch <= 'f') return (uint8_t)(10 + ch - 'a');
  if (ch >= 'A' && ch <= 'F') return (uint8_t)(10 + ch - 'A');
  return 0xFFu;
}

static uint8_t *crypto_decode_hex(const char *str, size_t len, size_t *out_len) {
  if ((len & 1u) != 0) return NULL;

  uint8_t *buf = malloc(len / 2u);
  if (!buf) return NULL;

  for (size_t i = 0; i < len; i += 2u) {
    uint8_t hi = crypto_hex_nibble(str[i]);
    uint8_t lo = crypto_hex_nibble(str[i + 1u]);
    if (hi == 0xFFu || lo == 0xFFu) {
      free(buf);
      return NULL;
    }
    buf[i / 2u] = (uint8_t)((hi << 4u) | lo);
  }

  *out_len = len / 2u;
  return buf;
}

static ant_value_t crypto_get_input_bytes(
  ant_t *js, ant_value_t value, ant_value_t encoding_val,
  const uint8_t **out_bytes, size_t *out_len, uint8_t **owned
) {
  const uint8_t *bytes = NULL;
  size_t len = 0;
  uint8_t *buf = NULL;
  
  ant_value_t string_val;
  size_t str_len = 0;
  
  const char *str = NULL;
  crypto_text_encoding_t enc = CRYPTO_TEXT_UTF8;

  if (buffer_source_get_bytes(js, value, &bytes, &len)) {
    *out_bytes = bytes;
    *out_len = len;
    *owned = NULL;
    return js_mkundef();
  }

  string_val = js_tostring_val(js, value);
  if (is_err(string_val)) return string_val;

  str = js_getstr(js, string_val, &str_len);
  if (!str) return js_mkerr(js, "Failed to convert hash input to string");

  if (vtype(encoding_val) == T_STR) {
    size_t enc_len = 0;
    const char *enc_str = js_getstr(js, encoding_val, &enc_len);
    if (enc_str) enc = crypto_parse_encoding(enc_str, enc_len);
  }

  switch (enc) {
    case CRYPTO_TEXT_HEX:
      buf = crypto_decode_hex(str, str_len, &len);
      if (!buf) return js_mkerr(js, "Invalid hex string");
      bytes = buf;
      break;
    case CRYPTO_TEXT_BASE64:
      buf = ant_base64_decode(str, str_len, &len);
      if (!buf) return js_mkerr(js, "Invalid base64 string");
      bytes = buf;
      break;
    case CRYPTO_TEXT_LATIN1:
      buf = malloc(str_len);
      if (!buf) return js_mkerr(js, "Out of memory");
      for (size_t i = 0; i < str_len; i++) buf[i] = (uint8_t)str[i];
      bytes = buf;
      len = str_len;
      break;
    case CRYPTO_TEXT_UTF8:
    default:
      bytes = (const uint8_t *)str;
      len = str_len;
      break;
  }

  *out_bytes = bytes;
  *out_len = len;
  *owned = buf;
  return js_mkundef();
}

static const EVP_MD *crypto_digest_from_name(const char *algo, size_t algo_len) {
  if (!algo || algo_len == 0) return NULL;
  char *algo_name = strndup(algo, algo_len);
  if (!algo_name) return NULL;
  const EVP_MD *md = EVP_get_digestbyname(algo_name);
  free(algo_name);
  return md;
}

static const EVP_MD *crypto_digest_from_algorithm(ant_t *js, ant_value_t algorithm) {
  ant_value_t algo_val = crypto_subtle_get_algorithm_name(js, algorithm);
  if (is_err(algo_val)) return NULL;

  size_t algo_len = 0;
  const char *algo = js_getstr(js, algo_val, &algo_len);
  if (!algo || algo_len == 0) return NULL;

  if (
    (algo_len == 5 && strncasecmp(algo, "SHA-1", 5) == 0) ||
    (algo_len == 4 && strncasecmp(algo, "SHA1", 4) == 0)
  ) return EVP_get_digestbyname("sha1");
  if (
    (algo_len == 7 && strncasecmp(algo, "SHA-256", 7) == 0) ||
    (algo_len == 6 && strncasecmp(algo, "SHA256", 6) == 0)
  ) return EVP_get_digestbyname("sha256");
  if (
    (algo_len == 7 && strncasecmp(algo, "SHA-384", 7) == 0) ||
    (algo_len == 6 && strncasecmp(algo, "SHA384", 6) == 0)
  ) return EVP_get_digestbyname("sha384");
  if (
    (algo_len == 7 && strncasecmp(algo, "SHA-512", 7) == 0) ||
    (algo_len == 6 && strncasecmp(algo, "SHA512", 6) == 0)
  ) return EVP_get_digestbyname("sha512");

  return crypto_digest_from_name(algo, algo_len);
}

static const EVP_MD *crypto_hmac_digest_from_algorithm(ant_t *js, ant_value_t algorithm) {
  if (is_object_type(algorithm)) {
    ant_value_t hash = js_get(js, algorithm, "hash");
    if (is_object_type(hash) || vtype(hash) == T_STR) {
      const EVP_MD *md = crypto_digest_from_algorithm(js, hash);
      if (md) return md;
    }
  }
  return crypto_digest_from_algorithm(js, algorithm);
}

static ant_value_t crypto_make_arraybuffer(ant_t *js, const uint8_t *data, size_t len) {
  ArrayBufferData *buffer = create_array_buffer_data(len);
  if (!buffer) return js_mkerr(js, "Out of memory");
  if (len > 0 && data) memcpy(buffer->data, data, len);
  return create_arraybuffer_obj(js, buffer);
}

static bool crypto_algorithm_is(ant_t *js, ant_value_t algorithm, const char *expected) {
  ant_value_t name_val = crypto_subtle_get_algorithm_name(js, algorithm);
  if (is_err(name_val)) return false;
  size_t len = 0;
  const char *name = js_getstr(js, name_val, &len);
  return name && strlen(expected) == len && strncasecmp(name, expected, len) == 0;
}

static const EVP_CIPHER *crypto_aes_gcm_cipher_for_key(size_t key_len) {
  switch (key_len) {
    case 16: return EVP_aes_128_gcm();
    case 24: return EVP_aes_192_gcm();
    case 32: return EVP_aes_256_gcm();
    default: return NULL;
  }
}

static const EVP_CIPHER *crypto_cipher_from_name(const char *name, size_t len) {
  char *copy = strndup(name, len);
  if (!copy) return NULL;
  const EVP_CIPHER *cipher = EVP_get_cipherbyname(copy);
  free(copy);
  return cipher;
}

static ant_hash_state_t *crypto_get_hash_state(ant_value_t value) {
  ant_hash_state_t *state = (ant_hash_state_t *)js_get_native(value, CRYPTO_HASH_NATIVE_TAG);
  return (state && state->ctx) ? state : NULL;
}

static ant_value_t crypto_require_hash_state(
  ant_t *js, ant_value_t value, ant_hash_state_t **out_state
) {
  ant_hash_state_t *state = crypto_get_hash_state(value);
  if (!state) return js_mkerr(js, "Invalid Hash state");
  *out_state = state;
  return js_mkundef();
}

static ant_value_t crypto_digest_result(
  ant_t *js, const uint8_t *digest, size_t digest_len, ant_value_t encoding_val
) {
  if (vtype(encoding_val) != T_STR) return crypto_make_buffer(js, digest, digest_len);

  size_t enc_len = 0;
  const char *enc = js_getstr(js, encoding_val, &enc_len);
  if (!enc) return crypto_make_buffer(js, digest, digest_len);

  crypto_text_encoding_t encoding = crypto_parse_encoding(enc, enc_len);

  if (encoding == CRYPTO_TEXT_HEX) {
    char *hex = malloc(digest_len * 2u + 1u);
    if (!hex) return js_mkerr(js, "Out of memory");
    for (size_t i = 0; i < digest_len; i++) {
      snprintf(hex + (i * 2u), 3, "%02x", digest[i]);
    }
    ant_value_t result = js_mkstr(js, hex, digest_len * 2u);
    free(hex);
    return result;
  }

  if (encoding == CRYPTO_TEXT_BASE64) {
    size_t out_len = 0;
    char *encoded = ant_base64_encode(digest, digest_len, &out_len);
    if (!encoded) return js_mkerr(js, "Failed to encode base64");
    
    if (enc_len == 9 && strncasecmp(enc, "base64url", 9) == 0) {
      for (size_t i = 0; i < out_len; i++) {
        if (encoded[i] == '+') encoded[i] = '-';
        else if (encoded[i] == '/') encoded[i] = '_';
      }
      while (out_len > 0 && encoded[out_len - 1u] == '=') out_len--;
    }
    
    ant_value_t result = js_mkstr(js, encoded, out_len);
    free(encoded);
    
    return result;
  }

  return crypto_make_buffer(js, digest, digest_len);
}

int uuidv7_new(uint8_t *uuid_out) {
  static uint8_t uuid_prev[16] = {0};
  static uint8_t rand_bytes[256] = {0};
  static size_t n_rand_consumed = sizeof(rand_bytes);
  
  struct timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);
  uint64_t unix_ts_ms = (uint64_t)tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
  
  if (n_rand_consumed > sizeof(rand_bytes) - 10) {
    if (crypto_fill_random(rand_bytes, sizeof(rand_bytes)) < 0) return -1;
    n_rand_consumed = 0;
  }
  
  int8_t status = uuidv7_generate(uuid_prev, unix_ts_ms, &rand_bytes[n_rand_consumed], uuid_prev);
  n_rand_consumed += uuidv7_status_n_rand_consumed(status);
  
  memcpy(uuid_out, uuid_prev, 16);
  return status;
}

// crypto.random()
static ant_value_t js_crypto_random(ant_t *js, ant_value_t *args, int nargs) {
  unsigned int value = 0;
  if (crypto_fill_random(&value, sizeof(value)) < 0) return crypto_random_error(js);
  return js_mknum((double)value);
}

// crypto.randomBytes(length)
static ant_value_t js_crypto_random_bytes(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "randomBytes requires a length argument");
  }

  int length = (int)js_getnum(args[0]);
  if (length <= 0 || length > 65536) {
    return js_mkerr(js, "invalid length");
  }
  
  unsigned char *random_bytes = malloc(length);
  if (random_bytes == NULL) {
    return js_mkerr(js, "memory allocation failed");
  }
  
  if (crypto_fill_random(random_bytes, (size_t)length) < 0) {
    free(random_bytes);
    return crypto_random_error(js);
  }
  
  ant_value_t array = crypto_make_buffer(js, random_bytes, (size_t)length);
  free(random_bytes);
  
  return array;
}

// crypto.randomUUID()
static ant_value_t js_crypto_random_uuid(ant_t *js, ant_value_t *args, int nargs) {
  char uuid[37];
  if (crypto_random_uuid(uuid) < 0) return crypto_random_error(js);
  return js_mkstr(js, uuid, 36);
}

// crypto.randomUUIDv7()
static ant_value_t js_crypto_random_uuidv7(ant_t *js, ant_value_t *args, int nargs) {
  uint8_t uuid[16];
  char uuid_str[37];
  
  int result = uuidv7_new(uuid);
  if (result < 0) return js_mkerr(js, "UUIDv7 generation failed");
  
  uuidv7_to_string(uuid, uuid_str);
  return js_mkstr(js, uuid_str, strlen(uuid_str));
}

// crypto.getRandomValues(typedArray)
static ant_value_t js_crypto_get_random_values(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "getRandomValues requires a TypedArray argument");
  }

  TypedArrayData *ta_data = buffer_get_typedarray_data(args[0]);
  if (!ta_data || !ta_data->buffer) {
    return js_mkerr(js, "argument must be a TypedArray");
  }
  
  if (ta_data->byte_length > 65536) {
    return js_mkerr(js, "TypedArray byte length exceeds 65536");
  }
  
  uint8_t *ptr = ta_data->buffer->data + ta_data->byte_offset;
  if (crypto_fill_random(ptr, ta_data->byte_length) < 0) return crypto_random_error(js);
  
  return args[0];
}

static ant_value_t js_crypto_random_fill_sync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "randomFillSync requires a target");

  uint8_t *bytes = NULL;
  size_t len = 0;
  if (!crypto_get_mutable_bytes(args[0], &bytes, &len)) {
    return js_mkerr(js, "randomFillSync target must be a Buffer, TypedArray, or ArrayBuffer");
  }

  size_t offset = 0;
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    double num = js_to_number(js, args[1]);
    if (num < 0) return js_mkerr(js, "randomFillSync offset must be non-negative");
    offset = (size_t)num;
  }

  size_t size = len - offset;
  if (nargs >= 3 && vtype(args[2]) != T_UNDEF) {
    double num = js_to_number(js, args[2]);
    if (num < 0) return js_mkerr(js, "randomFillSync size must be non-negative");
    size = (size_t)num;
  }

  if (offset > len || size > len - offset) {
    return js_mkerr(js, "randomFillSync range is out of bounds");
  }

  if (crypto_fill_random(bytes + offset, size) < 0) return crypto_random_error(js);
  return args[0];
}

static ant_value_t crypto_make_filtered_name_array(
  ant_t *js,
  const char *const *names,
  size_t count,
  crypto_name_kind_t kind
) {
  ant_value_t result = js_mkarr(js);
  if (is_err(result)) return result;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);

  for (size_t i = 0; i < count; i++) {
    const char *name = names[i];
    bool supported = false;
  
  switch (kind) {
    case CRYPTO_NAME_CIPHER:
      supported = EVP_get_cipherbyname(name) != NULL;
      break;
    case CRYPTO_NAME_HASH:
      supported = EVP_get_digestbyname(name) != NULL;
      break;
    case CRYPTO_NAME_CURVE: {
      int nid = OBJ_sn2nid(name);
      if (nid == NID_undef) break;
      EC_GROUP *group = EC_GROUP_new_by_curve_name(nid);
      if (!group) break;
      EC_GROUP_free(group);
      supported = true;
      break;
    }}
    
    if (!supported) continue;
    ant_value_t name_val = js_mkstr(js, name, strlen(name));
    
    if (is_err(name_val)) {
      GC_ROOT_RESTORE(js, root_mark);
      return name_val;
    }
    
    js_arr_push(js, result, name_val);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t js_crypto_get_ciphers(ant_t *js, ant_value_t *args, int nargs) {
return crypto_make_filtered_name_array(
  js, k_crypto_cipher_names,
  sizeof(k_crypto_cipher_names) / sizeof(k_crypto_cipher_names[0]),
  CRYPTO_NAME_CIPHER
);}

static ant_value_t js_crypto_get_hashes(ant_t *js, ant_value_t *args, int nargs) {
return crypto_make_filtered_name_array(
  js, k_crypto_hash_names,
  sizeof(k_crypto_hash_names) / sizeof(k_crypto_hash_names[0]),
  CRYPTO_NAME_HASH
);}

static ant_value_t js_crypto_get_curves(ant_t *js, ant_value_t *args, int nargs) {
return crypto_make_filtered_name_array(
  js, k_crypto_curve_names,
  sizeof(k_crypto_curve_names) / sizeof(k_crypto_curve_names[0]),
  CRYPTO_NAME_CURVE
);}

static ant_value_t js_crypto_timing_safe_equal(ant_t *js, ant_value_t *args, int nargs) {
  const uint8_t *left = NULL;
  const uint8_t *right = NULL;
  
  size_t left_len = 0;
  size_t right_len = 0;

  if (
    nargs < 2 ||
    !buffer_source_get_bytes(js, args[0], &left, &left_len) ||
    !buffer_source_get_bytes(js, args[1], &right, &right_len)
  ) return js_mkerr_typed(
    js, JS_ERR_TYPE,
    "timingSafeEqual arguments must be Buffer, TypedArray, DataView, or ArrayBuffer"
  );

  if (left_len != right_len)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Input buffers must have the same byte length");

  return js_bool(CRYPTO_memcmp(left, right, left_len) == 0);
}

static ant_value_t crypto_subtle_digest_impl(
  ant_t *js, ant_value_t algorithm, ant_value_t data
) {
  ant_value_t algo_val = crypto_subtle_get_algorithm_name(js, algorithm);
  if (is_err(algo_val)) return algo_val;

  size_t algo_len = 0;
  const char *algo = js_getstr(js, algo_val, &algo_len);
  if (!algo || algo_len == 0) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid digest algorithm");
  }

  char algo_name[16];
  if (algo_len >= sizeof(algo_name)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported digest algorithm");
  }

  for (size_t i = 0; i < algo_len; i++) {
    char ch = algo[i];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - ('a' - 'A'));
    algo_name[i] = ch;
  }
  algo_name[algo_len] = '\0';

  const char *evp_name = NULL;
  if (strcmp(algo_name, "SHA-1") == 0 || strcmp(algo_name, "SHA1") == 0) evp_name = "sha1";
  else if (strcmp(algo_name, "SHA-256") == 0 || strcmp(algo_name, "SHA256") == 0) evp_name = "sha256";
  else if (strcmp(algo_name, "SHA-384") == 0 || strcmp(algo_name, "SHA384") == 0) evp_name = "sha384";
  else if (strcmp(algo_name, "SHA-512") == 0 || strcmp(algo_name, "SHA512") == 0) evp_name = "sha512";
  else return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported digest algorithm");

  const EVP_MD *md = EVP_get_digestbyname(evp_name);
  if (!md) return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported digest algorithm");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  if (!buffer_source_get_bytes(js, data, &bytes, &len)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "digest data must be an ArrayBuffer, TypedArray, DataView, or Buffer");
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (EVP_Digest(bytes, len, digest, &digest_len, md, NULL) != 1) {
    return js_mkerr(js, "Digest failed");
  }

  ArrayBufferData *buffer = create_array_buffer_data(digest_len);
  if (!buffer) return js_mkerr(js, "Out of memory");
  if (digest_len > 0) memcpy(buffer->data, digest, digest_len);
  
  return create_arraybuffer_obj(js, buffer);
}

static ant_value_t js_crypto_subtle_digest(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  if (nargs < 2) {
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "subtle.digest requires algorithm and data"));
    return promise;
  }

  ant_value_t result = crypto_subtle_digest_impl(js, args[0], args[1]);
  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, result);
  
  return promise;
}

static ant_value_t crypto_make_key_object(
  ant_t *js, ant_crypto_key_kind_t kind, const EVP_MD *md,
  const uint8_t *key_bytes, size_t key_len, ant_value_t algorithm
) {
  ant_crypto_key_t *key = calloc(1, sizeof(*key));
  if (!key) return js_mkerr(js, "Out of memory");
  key->kind = kind;
  key->md = md;
  key->key = malloc(key_len ? key_len : 1);
  if (!key->key) {
    crypto_key_free(key);
    return js_mkerr(js, "Out of memory");
  }
  if (key_len) memcpy(key->key, key_bytes, key_len);
  key->key_len = key_len;

  ant_value_t obj = js_mkobj(js);
  js_set(js, obj, "type", js_mkstr(js, "secret", 6));
  js_set(js, obj, "extractable", js_false);
  js_set(js, obj, "algorithm", algorithm);
  js_set_native(obj, key, CRYPTO_KEY_NATIVE_TAG);
  js_set_finalizer(obj, crypto_key_finalize);
  return obj;
}

static ant_value_t crypto_subtle_import_key_impl(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3 || vtype(args[0]) != T_STR) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "subtle.importKey requires format, keyData, and algorithm");
  }

  size_t format_len = 0;
  const char *format = js_getstr(js, args[0], &format_len);
  if (!format || format_len != 3 || strncasecmp(format, "raw", 3) != 0) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Only raw CryptoKey import is supported");
  }

  const uint8_t *bytes = NULL;
  size_t len = 0;
  if (!buffer_source_get_bytes(js, args[1], &bytes, &len)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "keyData must be an ArrayBuffer, TypedArray, DataView, or Buffer");
  }

  if (crypto_algorithm_is(js, args[2], "HMAC")) {
    const EVP_MD *md = crypto_hmac_digest_from_algorithm(js, args[2]);
    if (!md) return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported HMAC hash algorithm");
    return crypto_make_key_object(js, CRYPTO_KEY_HMAC, md, bytes, len, args[2]);
  }

  if (crypto_algorithm_is(js, args[2], "AES-GCM")) {
    if (!crypto_aes_gcm_cipher_for_key(len)) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid AES-GCM key length");
    return crypto_make_key_object(js, CRYPTO_KEY_AES_GCM, NULL, bytes, len, args[2]);
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported CryptoKey algorithm");
}

static ant_value_t js_crypto_subtle_import_key(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = crypto_subtle_import_key_impl(js, args, nargs);
  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, result);
  return promise;
}

static ant_crypto_key_t *crypto_require_key(ant_value_t value) {
  return (ant_crypto_key_t *)js_get_native(value, CRYPTO_KEY_NATIVE_TAG);
}

static ant_value_t crypto_hmac_once(
  ant_t *js, ant_crypto_key_t *key, ant_value_t data, ant_value_t encoding,
  bool arraybuffer_result
) {
  const uint8_t *bytes = NULL;
  size_t len = 0;
  uint8_t *owned = NULL;
  ant_value_t err = crypto_get_input_bytes(js, data, encoding, &bytes, &len, &owned);
  if (is_err(err)) return err;

  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int out_len = 0;
  unsigned char *res = HMAC(key->md, key->key, (int)key->key_len, bytes, len, out, &out_len);
  if (owned) free(owned);
  if (!res) return js_mkerr(js, "HMAC failed");
  return arraybuffer_result ? crypto_make_arraybuffer(js, out, out_len) : crypto_make_buffer(js, out, out_len);
}

static ant_value_t crypto_subtle_sign_impl(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_mkerr_typed(js, JS_ERR_TYPE, "subtle.sign requires algorithm, key, and data");
  ant_crypto_key_t *key = crypto_require_key(args[1]);
  if (!key || key->kind != CRYPTO_KEY_HMAC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "subtle.sign requires an HMAC CryptoKey");
  }
  return crypto_hmac_once(js, key, args[2], js_mkundef(), true);
}

static ant_value_t js_crypto_subtle_sign(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = crypto_subtle_sign_impl(js, args, nargs);
  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, result);
  return promise;
}

static ant_value_t crypto_subtle_verify_impl(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 4) return js_mkerr_typed(js, JS_ERR_TYPE, "subtle.verify requires algorithm, key, signature, and data");
  ant_crypto_key_t *key = crypto_require_key(args[1]);
  if (!key || key->kind != CRYPTO_KEY_HMAC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "subtle.verify requires an HMAC CryptoKey");
  }

  const uint8_t *sig = NULL;
  size_t sig_len = 0;
  if (!buffer_source_get_bytes(js, args[2], &sig, &sig_len)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "signature must be an ArrayBuffer, TypedArray, DataView, or Buffer");
  }

  ant_value_t expected = crypto_hmac_once(js, key, args[3], js_mkundef(), false);
  if (is_err(expected)) return expected;

  const uint8_t *expected_bytes = NULL;
  size_t expected_len = 0;
  if (!buffer_source_get_bytes(js, expected, &expected_bytes, &expected_len)) return js_false;
  if (sig_len != expected_len) return js_false;
  return js_bool(CRYPTO_memcmp(sig, expected_bytes, sig_len) == 0);
}

static ant_value_t js_crypto_subtle_verify(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = crypto_subtle_verify_impl(js, args, nargs);
  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, result);
  return promise;
}

static ant_value_t crypto_aes_gcm_crypt(
  ant_t *js, bool encrypt, ant_value_t algorithm, ant_crypto_key_t *key, ant_value_t data
) {
  if (!key || key->kind != CRYPTO_KEY_AES_GCM) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "AES-GCM requires an AES CryptoKey");
  }

  ant_value_t iv_val = js_get(js, algorithm, "iv");
  const uint8_t *iv = NULL;
  size_t iv_len = 0;
  if (!buffer_source_get_bytes(js, iv_val, &iv, &iv_len)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "AES-GCM iv must be an ArrayBuffer, TypedArray, DataView, or Buffer");
  }

  const uint8_t *input = NULL;
  size_t input_len = 0;
  if (!buffer_source_get_bytes(js, data, &input, &input_len)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "AES-GCM data must be an ArrayBuffer, TypedArray, DataView, or Buffer");
  }

  int tag_len = 16;
  ant_value_t tag_len_val = js_get(js, algorithm, "tagLength");
  if (vtype(tag_len_val) == T_NUM) {
    tag_len = (int)(js_getnum(tag_len_val) / 8.0);
    if (tag_len <= 0 || tag_len > 16) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid AES-GCM tagLength");
  }

  const uint8_t *aad = NULL;
  size_t aad_len = 0;
  ant_value_t aad_val = js_get(js, algorithm, "additionalData");
  bool has_aad = buffer_source_get_bytes(js, aad_val, &aad, &aad_len);

  if (!encrypt && input_len < (size_t)tag_len) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "AES-GCM ciphertext is shorter than tagLength");
  }

  const EVP_CIPHER *cipher = crypto_aes_gcm_cipher_for_key(key->key_len);
  if (!cipher) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid AES-GCM key length");

  size_t text_len = encrypt ? input_len : input_len - (size_t)tag_len;
  size_t out_cap = encrypt ? input_len + (size_t)tag_len : text_len;
  uint8_t *out = malloc(out_cap ? out_cap : 1);
  if (!out) return js_mkerr(js, "Out of memory");

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    free(out);
    return js_mkerr(js, "AES-GCM initialization failed");
  }

  int ok = encrypt
    ? EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL)
    : EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL);
  ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)iv_len, NULL);
  ok = ok && (encrypt
    ? EVP_EncryptInit_ex(ctx, NULL, NULL, key->key, iv)
    : EVP_DecryptInit_ex(ctx, NULL, NULL, key->key, iv));

  int out_len = 0;
  int total = 0;
  if (ok && has_aad && aad_len > 0) {
    ok = encrypt
      ? EVP_EncryptUpdate(ctx, NULL, &out_len, aad, (int)aad_len)
      : EVP_DecryptUpdate(ctx, NULL, &out_len, aad, (int)aad_len);
  }

  if (ok && text_len > 0) {
    ok = encrypt
      ? EVP_EncryptUpdate(ctx, out, &out_len, input, (int)text_len)
      : EVP_DecryptUpdate(ctx, out, &out_len, input, (int)text_len);
    total += out_len;
  }

  if (!encrypt && ok) {
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag_len, (void *)(input + text_len));
  }

  if (ok) {
    ok = encrypt
      ? EVP_EncryptFinal_ex(ctx, out + total, &out_len)
      : EVP_DecryptFinal_ex(ctx, out + total, &out_len);
    total += out_len;
  }

  if (ok && encrypt) {
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag_len, out + total);
    total += tag_len;
  }

  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    free(out);
    return js_mkerr_typed(js, JS_ERR_TYPE, "AES-GCM operation failed");
  }

  ant_value_t result = crypto_make_arraybuffer(js, out, (size_t)total);
  free(out);
  return result;
}

static ant_value_t crypto_subtle_encrypt_impl(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_mkerr_typed(js, JS_ERR_TYPE, "subtle.encrypt requires algorithm, key, and data");
  ant_crypto_key_t *key = crypto_require_key(args[1]);
  if (!crypto_algorithm_is(js, args[0], "AES-GCM")) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported encryption algorithm");
  }
  return crypto_aes_gcm_crypt(js, true, args[0], key, args[2]);
}

static ant_value_t crypto_subtle_decrypt_impl(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_mkerr_typed(js, JS_ERR_TYPE, "subtle.decrypt requires algorithm, key, and data");
  ant_crypto_key_t *key = crypto_require_key(args[1]);
  if (!crypto_algorithm_is(js, args[0], "AES-GCM")) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported decryption algorithm");
  }
  return crypto_aes_gcm_crypt(js, false, args[0], key, args[2]);
}

static ant_value_t js_crypto_subtle_encrypt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = crypto_subtle_encrypt_impl(js, args, nargs);
  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, result);
  return promise;
}

static ant_value_t js_crypto_subtle_decrypt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t promise = js_mkpromise(js);
  ant_value_t result = crypto_subtle_decrypt_impl(js, args, nargs);
  if (is_err(result)) js_reject_promise(js, promise, result);
  else js_resolve_promise(js, promise, result);
  return promise;
}

static ant_value_t js_hash_update(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  ant_hash_state_t *state = NULL;
  
  const uint8_t *bytes = NULL;
  size_t len = 0;
  
  uint8_t *owned = NULL;
  ant_value_t err;

  err = crypto_require_hash_state(js, this_val, &state);
  if (is_err(err)) return err;
  if (state->finalized) return js_mkerr(js, "Hash digest already called");
  if (nargs < 1) return js_mkerr(js, "Hash.update requires data");

  ant_value_t encoding = (nargs >= 2) ? args[1] : js_mkundef();
  err = crypto_get_input_bytes(js, args[0], encoding, &bytes, &len, &owned);
  if (is_err(err)) goto cleanup;

  if (EVP_DigestUpdate(state->ctx, bytes, len) != 1) {
    err = js_mkerr(js, "Hash update failed");
    goto cleanup;
  }

  err = this_val;

cleanup:
  if (owned) free(owned);
  return err;
}

static ant_value_t js_hash_digest(ant_t *js, ant_value_t *args, int nargs) {
  ant_hash_state_t *state = NULL;
  ant_value_t err = crypto_require_hash_state(js, js_getthis(js), &state);
  
  if (is_err(err)) return err;
  if (state->finalized) return js_mkerr(js, "Hash digest already called");

  if (EVP_DigestFinal_ex(state->ctx, state->digest, &state->digest_len) != 1) {
    return js_mkerr(js, "Hash digest failed");
  }

  state->finalized = true;
  EVP_MD_CTX_free(state->ctx);
  state->ctx = NULL;

  return crypto_digest_result(js, state->digest, state->digest_len, nargs >= 1 ? args[0] : js_mkundef());
}

static ant_value_t js_crypto_create_hash(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "createHash requires an algorithm");

  ant_value_t algo_val = js_tostring_val(js, args[0]);
  if (is_err(algo_val)) return algo_val;

  size_t algo_len = 0;
  const char *algo = js_getstr(js, algo_val, &algo_len);
  if (!algo || algo_len == 0) return js_mkerr(js, "Invalid hash algorithm");

  char *algo_name = strndup(algo, algo_len);
  if (!algo_name) return js_mkerr(js, "Out of memory");

  const EVP_MD *md = EVP_get_digestbyname(algo_name);
  free(algo_name);
  if (!md) return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported hash algorithm");

  ant_hash_state_t *state = calloc(1, sizeof(*state));
  if (!state) return js_mkerr(js, "Out of memory");

  state->ctx = EVP_MD_CTX_new();
  state->md = md;
  
  if (!state->ctx || EVP_DigestInit_ex(state->ctx, md, NULL) != 1) {
    crypto_hash_state_free(state);
    return js_mkerr(js, "Failed to initialize hash");
  }

  ant_value_t obj = js_mkobj(js);
  js_set(js, obj, "update", js_mkfun(js_hash_update));
  js_set(js, obj, "digest", js_mkfun(js_hash_digest));
  
  js_set_native(obj, state, CRYPTO_HASH_NATIVE_TAG);
  js_set_finalizer(obj, crypto_hash_finalize);
  js_set_sym(js, obj, get_toStringTag_sym(), js_mkstr(js, "Hash", 4));
  
  return obj;
}

static ant_hmac_state_t *crypto_get_hmac_state(ant_value_t value) {
  ant_hmac_state_t *state = (ant_hmac_state_t *)js_get_native(value, CRYPTO_HMAC_NATIVE_TAG);
  return (state && state->ctx) ? state : NULL;
}

static ant_value_t crypto_require_hmac_state(
  ant_t *js, ant_value_t value, ant_hmac_state_t **out_state
) {
  ant_hmac_state_t *state = crypto_get_hmac_state(value);
  if (!state) return js_mkerr(js, "Invalid Hmac state");
  *out_state = state;
  return js_mkundef();
}

static ant_value_t js_hmac_update(ant_t *js, ant_value_t *args, int nargs) {
  ant_hmac_state_t *state = NULL;
  const uint8_t *bytes = NULL;
  size_t len = 0;
  uint8_t *owned = NULL;

  ant_value_t err = crypto_require_hmac_state(js, js_getthis(js), &state);
  if (is_err(err)) return err;
  if (state->finalized) return js_mkerr(js, "Hmac digest already called");
  if (nargs < 1) return js_mkerr(js, "Hmac.update requires data");

  ant_value_t encoding = (nargs >= 2) ? args[1] : js_mkundef();
  err = crypto_get_input_bytes(js, args[0], encoding, &bytes, &len, &owned);
  if (is_err(err)) goto cleanup;

  if (HMAC_Update(state->ctx, bytes, len) != 1) {
    err = js_mkerr(js, "Hmac update failed");
    goto cleanup;
  }

  err = js_getthis(js);

cleanup:
  if (owned) free(owned);
  return err;
}

static ant_value_t js_hmac_digest(ant_t *js, ant_value_t *args, int nargs) {
  ant_hmac_state_t *state = NULL;
  ant_value_t err = crypto_require_hmac_state(js, js_getthis(js), &state);
  if (is_err(err)) return err;
  if (state->finalized) return js_mkerr(js, "Hmac digest already called");

  if (HMAC_Final(state->ctx, state->digest, &state->digest_len) != 1) {
    return js_mkerr(js, "Hmac digest failed");
  }

  state->finalized = true;
  HMAC_CTX_free(state->ctx);
  state->ctx = NULL;

  return crypto_digest_result(js, state->digest, state->digest_len, nargs >= 1 ? args[0] : js_mkundef());
}

static ant_value_t js_crypto_create_hmac(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "createHmac requires an algorithm and key");

  ant_value_t algo_val = js_tostring_val(js, args[0]);
  if (is_err(algo_val)) return algo_val;
  size_t algo_len = 0;
  const char *algo = js_getstr(js, algo_val, &algo_len);
  const EVP_MD *md = crypto_digest_from_name(algo, algo_len);
  if (!md) return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported HMAC algorithm");

  const uint8_t *key = NULL;
  size_t key_len = 0;
  uint8_t *owned = NULL;
  ant_value_t err = crypto_get_input_bytes(js, args[1], js_mkundef(), &key, &key_len, &owned);
  if (is_err(err)) return err;

  ant_hmac_state_t *state = calloc(1, sizeof(*state));
  if (!state) {
    if (owned) free(owned);
    return js_mkerr(js, "Out of memory");
  }

  state->ctx = HMAC_CTX_new();
  state->md = md;
  if (!state->ctx || HMAC_Init_ex(state->ctx, key, (int)key_len, md, NULL) != 1) {
    if (owned) free(owned);
    crypto_hmac_state_free(state);
    return js_mkerr(js, "Failed to initialize Hmac");
  }
  if (owned) free(owned);

  ant_value_t obj = js_mkobj(js);
  js_set(js, obj, "update", js_mkfun(js_hmac_update));
  js_set(js, obj, "digest", js_mkfun(js_hmac_digest));
  js_set_native(obj, state, CRYPTO_HMAC_NATIVE_TAG);
  js_set_finalizer(obj, crypto_hmac_finalize);
  js_set_sym(js, obj, get_toStringTag_sym(), js_mkstr(js, "Hmac", 4));
  return obj;
}

static ant_value_t js_crypto_hash(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t algo_val;
  ant_value_t output_encoding;
  
  const char *algo;
  size_t algo_len = 0;
  char *algo_name = NULL;
  
  const EVP_MD *md = NULL;
  const uint8_t *bytes = NULL;
  
  size_t len = 0;
  uint8_t *owned = NULL;
  
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  ant_value_t err;

  if (nargs < 2) return js_mkerr(js, "hash requires algorithm and data");
  output_encoding = nargs >= 3 ? args[2] : js_mkundef();

  algo_val = js_tostring_val(js, args[0]);
  if (is_err(algo_val)) return algo_val;

  algo = js_getstr(js, algo_val, &algo_len);
  if (!algo || algo_len == 0) return js_mkerr(js, "Invalid hash algorithm");

  algo_name = strndup(algo, algo_len);
  if (!algo_name) return js_mkerr(js, "Out of memory");

  md = EVP_get_digestbyname(algo_name);
  free(algo_name);
  if (!md) return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported hash algorithm");

  err = crypto_get_input_bytes(js, args[1], js_mkundef(), &bytes, &len, &owned);
  if (is_err(err)) goto cleanup;

  if (EVP_Digest(bytes, len, digest, &digest_len, md, NULL) != 1) {
    err = js_mkerr(js, "Hash failed");
    goto cleanup;
  }

  err = crypto_digest_result(js, digest, digest_len, output_encoding);

cleanup:
  if (owned) free(owned);
  return err;
}

static ant_cipher_state_t *crypto_get_cipher_state(ant_value_t value) {
  ant_cipher_state_t *state = (ant_cipher_state_t *)js_get_native(value, CRYPTO_CIPHER_NATIVE_TAG);
  return (state && state->ctx) ? state : NULL;
}

static ant_value_t crypto_require_cipher_state(
  ant_t *js, ant_value_t value, ant_cipher_state_t **out_state
) {
  ant_cipher_state_t *state = crypto_get_cipher_state(value);
  if (!state) return js_mkerr(js, "Invalid Cipher state");
  *out_state = state;
  return js_mkundef();
}

static ant_value_t crypto_cipher_bytes_result(
  ant_t *js, uint8_t *bytes, size_t len, ant_value_t encoding
) {
  if (vtype(encoding) == T_STR) return crypto_digest_result(js, bytes, len, encoding);
  return crypto_make_buffer(js, bytes, len);
}

static ant_value_t js_cipher_update(ant_t *js, ant_value_t *args, int nargs) {
  ant_cipher_state_t *state = NULL;
  ant_value_t err = crypto_require_cipher_state(js, js_getthis(js), &state);
  if (is_err(err)) return err;
  if (state->finalized) return js_mkerr(js, "Cipher already finalized");
  if (nargs < 1) return js_mkerr(js, "Cipher.update requires data");

  const uint8_t *input = NULL;
  size_t input_len = 0;
  uint8_t *owned = NULL;
  ant_value_t input_encoding = nargs >= 2 ? args[1] : js_mkundef();
  err = crypto_get_input_bytes(js, args[0], input_encoding, &input, &input_len, &owned);
  if (is_err(err)) return err;

  int block_size = EVP_CIPHER_CTX_block_size(state->ctx);
  size_t out_cap = input_len + (block_size > 0 ? (size_t)block_size : 16u);
  uint8_t *out = malloc(out_cap ? out_cap : 1);
  if (!out) {
    if (owned) free(owned);
    return js_mkerr(js, "Out of memory");
  }

  int out_len = 0;
  int ok = state->encrypt
    ? EVP_EncryptUpdate(state->ctx, out, &out_len, input, (int)input_len)
    : EVP_DecryptUpdate(state->ctx, out, &out_len, input, (int)input_len);

  if (owned) free(owned);
  if (!ok) {
    free(out);
    return js_mkerr(js, "Cipher update failed");
  }

  ant_value_t result = crypto_cipher_bytes_result(js, out, (size_t)out_len, nargs >= 3 ? args[2] : js_mkundef());
  free(out);
  return result;
}

static ant_value_t js_cipher_final(ant_t *js, ant_value_t *args, int nargs) {
  ant_cipher_state_t *state = NULL;
  ant_value_t err = crypto_require_cipher_state(js, js_getthis(js), &state);
  if (is_err(err)) return err;
  if (state->finalized) return js_mkerr(js, "Cipher already finalized");

  int block_size = EVP_CIPHER_CTX_block_size(state->ctx);
  size_t out_cap = block_size > 0 ? (size_t)block_size : 16u;
  uint8_t out[EVP_MAX_BLOCK_LENGTH + 16];
  int out_len = 0;

  int ok = state->encrypt
    ? EVP_EncryptFinal_ex(state->ctx, out, &out_len)
    : EVP_DecryptFinal_ex(state->ctx, out, &out_len);
  (void)out_cap;

  state->finalized = true;
  if (!ok) return js_mkerr(js, "Cipher final failed");

  if (state->encrypt && state->is_gcm) {
    state->auth_tag_len = 16;
    EVP_CIPHER_CTX_ctrl(state->ctx, EVP_CTRL_GCM_GET_TAG, 16, state->auth_tag);
  }

  return crypto_cipher_bytes_result(js, out, (size_t)out_len, nargs >= 1 ? args[0] : js_mkundef());
}

static ant_value_t js_cipher_set_aad(ant_t *js, ant_value_t *args, int nargs) {
  ant_cipher_state_t *state = NULL;
  ant_value_t err = crypto_require_cipher_state(js, js_getthis(js), &state);
  if (is_err(err)) return err;
  if (nargs < 1) return js_mkerr(js, "setAAD requires data");

  const uint8_t *aad = NULL;
  size_t aad_len = 0;
  if (!buffer_source_get_bytes(js, args[0], &aad, &aad_len)) {
    return js_mkerr(js, "setAAD data must be a Buffer, TypedArray, DataView, or ArrayBuffer");
  }

  int out_len = 0;
  int ok = state->encrypt
    ? EVP_EncryptUpdate(state->ctx, NULL, &out_len, aad, (int)aad_len)
    : EVP_DecryptUpdate(state->ctx, NULL, &out_len, aad, (int)aad_len);
  if (!ok) return js_mkerr(js, "setAAD failed");
  return js_getthis(js);
}

static ant_value_t js_cipher_get_auth_tag(ant_t *js, ant_value_t *args, int nargs) {
  ant_cipher_state_t *state = NULL;
  ant_value_t err = crypto_require_cipher_state(js, js_getthis(js), &state);
  if (is_err(err)) return err;
  if (!state->is_gcm || !state->encrypt || state->auth_tag_len == 0) {
    return js_mkerr(js, "Auth tag is not available");
  }
  return crypto_make_buffer(js, state->auth_tag, state->auth_tag_len);
}

static ant_value_t js_cipher_set_auth_tag(ant_t *js, ant_value_t *args, int nargs) {
  ant_cipher_state_t *state = NULL;
  ant_value_t err = crypto_require_cipher_state(js, js_getthis(js), &state);
  if (is_err(err)) return err;
  if (nargs < 1) return js_mkerr(js, "setAuthTag requires a tag");

  const uint8_t *tag = NULL;
  size_t tag_len = 0;
  if (!buffer_source_get_bytes(js, args[0], &tag, &tag_len) || tag_len > sizeof(state->auth_tag)) {
    return js_mkerr(js, "Invalid authentication tag");
  }

  memcpy(state->auth_tag, tag, tag_len);
  state->auth_tag_len = tag_len;
  if (!state->encrypt && state->is_gcm) {
    EVP_CIPHER_CTX_ctrl(state->ctx, EVP_CTRL_GCM_SET_TAG, (int)tag_len, state->auth_tag);
  }
  return js_getthis(js);
}

static ant_value_t js_cipher_set_auto_padding(ant_t *js, ant_value_t *args, int nargs) {
  ant_cipher_state_t *state = NULL;
  ant_value_t err = crypto_require_cipher_state(js, js_getthis(js), &state);
  if (is_err(err)) return err;
  int padding = nargs < 1 || js_truthy(js, args[0]);
  EVP_CIPHER_CTX_set_padding(state->ctx, padding);
  return js_getthis(js);
}

static ant_value_t crypto_create_cipheriv(ant_t *js, ant_value_t *args, int nargs, bool encrypt) {
  if (nargs < 3) return js_mkerr(js, "createCipheriv requires algorithm, key, and iv");

  ant_value_t algo_val = js_tostring_val(js, args[0]);
  if (is_err(algo_val)) return algo_val;
  size_t algo_len = 0;
  const char *algo = js_getstr(js, algo_val, &algo_len);
  const EVP_CIPHER *cipher = crypto_cipher_from_name(algo, algo_len);
  if (!cipher) return js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported cipher algorithm");

  const uint8_t *key = NULL;
  size_t key_len = 0;
  uint8_t *key_owned = NULL;
  ant_value_t err = crypto_get_input_bytes(js, args[1], js_mkundef(), &key, &key_len, &key_owned);
  if (is_err(err)) return err;

  const uint8_t *iv = NULL;
  size_t iv_len = 0;
  uint8_t *iv_owned = NULL;
  if (vtype(args[2]) != T_NULL && vtype(args[2]) != T_UNDEF) {
    err = crypto_get_input_bytes(js, args[2], js_mkundef(), &iv, &iv_len, &iv_owned);
    if (is_err(err)) {
      if (key_owned) free(key_owned);
      return err;
    }
  }

  ant_cipher_state_t *state = calloc(1, sizeof(*state));
  if (!state) {
    if (key_owned) free(key_owned);
    if (iv_owned) free(iv_owned);
    return js_mkerr(js, "Out of memory");
  }

  state->ctx = EVP_CIPHER_CTX_new();
  state->encrypt = encrypt;
  state->is_gcm = EVP_CIPHER_mode(cipher) == EVP_CIPH_GCM_MODE;
  if (!state->ctx) {
    crypto_cipher_state_free(state);
    if (key_owned) free(key_owned);
    if (iv_owned) free(iv_owned);
    return js_mkerr(js, "Cipher initialization failed");
  }

  int ok = encrypt
    ? EVP_EncryptInit_ex(state->ctx, cipher, NULL, NULL, NULL)
    : EVP_DecryptInit_ex(state->ctx, cipher, NULL, NULL, NULL);
  if (ok && state->is_gcm && iv) {
    ok = EVP_CIPHER_CTX_ctrl(state->ctx, EVP_CTRL_GCM_SET_IVLEN, (int)iv_len, NULL);
  }
  ok = ok && (encrypt
    ? EVP_EncryptInit_ex(state->ctx, NULL, NULL, key, iv)
    : EVP_DecryptInit_ex(state->ctx, NULL, NULL, key, iv));

  if (key_owned) free(key_owned);
  if (iv_owned) free(iv_owned);
  if (!ok) {
    crypto_cipher_state_free(state);
    return js_mkerr(js, "Cipher initialization failed");
  }

  ant_value_t obj = js_mkobj(js);
  js_set(js, obj, "update", js_mkfun(js_cipher_update));
  js_set(js, obj, "final", js_mkfun(js_cipher_final));
  js_set(js, obj, "setAAD", js_mkfun(js_cipher_set_aad));
  js_set(js, obj, "getAuthTag", js_mkfun(js_cipher_get_auth_tag));
  js_set(js, obj, "setAuthTag", js_mkfun(js_cipher_set_auth_tag));
  js_set(js, obj, "setAutoPadding", js_mkfun(js_cipher_set_auto_padding));
  js_set_native(obj, state, CRYPTO_CIPHER_NATIVE_TAG);
  js_set_finalizer(obj, crypto_cipher_finalize);
  return obj;
}

static ant_value_t js_crypto_create_cipheriv(ant_t *js, ant_value_t *args, int nargs) {
  return crypto_create_cipheriv(js, args, nargs, true);
}

static ant_value_t js_crypto_create_decipheriv(ant_t *js, ant_value_t *args, int nargs) {
  return crypto_create_cipheriv(js, args, nargs, false);
}

static ant_value_t crypto_pbkdf2_result(
  ant_t *js, ant_value_t password_val, ant_value_t salt_val,
  ant_value_t iterations_val, ant_value_t keylen_val, ant_value_t digest_val
) {
  const uint8_t *password = NULL, *salt = NULL;
  size_t password_len = 0, salt_len = 0;
  uint8_t *password_owned = NULL, *salt_owned = NULL;
  ant_value_t err = crypto_get_input_bytes(js, password_val, js_mkundef(), &password, &password_len, &password_owned);
  if (is_err(err)) return err;
  err = crypto_get_input_bytes(js, salt_val, js_mkundef(), &salt, &salt_len, &salt_owned);
  if (is_err(err)) goto cleanup_err;

  int iterations = (int)js_getnum(iterations_val);
  int keylen = (int)js_getnum(keylen_val);
  if (iterations <= 0 || keylen < 0) {
    err = js_mkerr(js, "Invalid PBKDF2 parameters");
    goto cleanup_err;
  }

  ant_value_t digest_str = js_tostring_val(js, digest_val);
  if (is_err(digest_str)) {
    err = digest_str;
    goto cleanup_err;
  }
  size_t digest_len = 0;
  const char *digest = js_getstr(js, digest_str, &digest_len);
  const EVP_MD *md = crypto_digest_from_name(digest, digest_len);
  if (!md) {
    err = js_mkerr_typed(js, JS_ERR_TYPE, "Unsupported PBKDF2 digest");
    goto cleanup_err;
  }

  uint8_t *out = malloc((size_t)keylen ? (size_t)keylen : 1);
  if (!out) {
    err = js_mkerr(js, "Out of memory");
    goto cleanup_err;
  }
  if (PKCS5_PBKDF2_HMAC((const char *)password, (int)password_len, salt, (int)salt_len, iterations, md, keylen, out) != 1) {
    free(out);
    err = js_mkerr(js, "PBKDF2 failed");
    goto cleanup_err;
  }

  ant_value_t result = crypto_make_buffer(js, out, (size_t)keylen);
  free(out);
  if (password_owned) free(password_owned);
  if (salt_owned) free(salt_owned);
  return result;

cleanup_err:
  if (password_owned) free(password_owned);
  if (salt_owned) free(salt_owned);
  return err;
}

static ant_value_t js_crypto_pbkdf2_sync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 5) return js_mkerr(js, "pbkdf2Sync requires password, salt, iterations, keylen, and digest");
  return crypto_pbkdf2_result(js, args[0], args[1], args[2], args[3], args[4]);
}

static ant_value_t js_crypto_pbkdf2(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 6 || (vtype(args[5]) != T_FUNC && vtype(args[5]) != T_CFUNC)) {
    return js_mkerr(js, "pbkdf2 requires a callback");
  }
  ant_value_t result = crypto_pbkdf2_result(js, args[0], args[1], args[2], args[3], args[4]);
  ant_value_t cb_args[2] = { is_err(result) ? result : js_mknull(), is_err(result) ? js_mkundef() : result };
  ant_value_t cb_result = sv_vm_call(js->vm, js, args[5], js_mkundef(), cb_args, 2, NULL, false);
  return is_err(cb_result) ? cb_result : js_mkundef();
}

static ant_value_t crypto_scrypt_result(ant_t *js, ant_value_t *args, int nargs, int options_index) {
  const uint8_t *password = NULL, *salt = NULL;
  size_t password_len = 0, salt_len = 0;
  uint8_t *password_owned = NULL, *salt_owned = NULL;
  ant_value_t err = crypto_get_input_bytes(js, args[0], js_mkundef(), &password, &password_len, &password_owned);
  if (is_err(err)) return err;
  err = crypto_get_input_bytes(js, args[1], js_mkundef(), &salt, &salt_len, &salt_owned);
  if (is_err(err)) goto cleanup_err;

  int keylen = (int)js_getnum(args[2]);
  if (keylen < 0) {
    err = js_mkerr(js, "Invalid scrypt key length");
    goto cleanup_err;
  }

  uint64_t N = 16384, r = 8, p = 1, maxmem = 32 * 1024 * 1024;
  if (options_index >= 0 && options_index < nargs && is_object_type(args[options_index])) {
    ant_value_t v = js_get(js, args[options_index], "N");
    if (vtype(v) == T_UNDEF) v = js_get(js, args[options_index], "cost");
    if (vtype(v) == T_NUM) N = (uint64_t)js_getnum(v);
    v = js_get(js, args[options_index], "r");
    if (vtype(v) == T_UNDEF) v = js_get(js, args[options_index], "blockSize");
    if (vtype(v) == T_NUM) r = (uint64_t)js_getnum(v);
    v = js_get(js, args[options_index], "p");
    if (vtype(v) == T_UNDEF) v = js_get(js, args[options_index], "parallelization");
    if (vtype(v) == T_NUM) p = (uint64_t)js_getnum(v);
    v = js_get(js, args[options_index], "maxmem");
    if (vtype(v) == T_NUM) maxmem = (uint64_t)js_getnum(v);
  }

  uint8_t *out = malloc((size_t)keylen ? (size_t)keylen : 1);
  if (!out) {
    err = js_mkerr(js, "Out of memory");
    goto cleanup_err;
  }
  if (EVP_PBE_scrypt((const char *)password, password_len, salt, salt_len, N, r, p, maxmem, out, (size_t)keylen) != 1) {
    free(out);
    err = js_mkerr(js, "scrypt failed");
    goto cleanup_err;
  }

  ant_value_t result = crypto_make_buffer(js, out, (size_t)keylen);
  free(out);
  if (password_owned) free(password_owned);
  if (salt_owned) free(salt_owned);
  return result;

cleanup_err:
  if (password_owned) free(password_owned);
  if (salt_owned) free(salt_owned);
  return err;
}

static ant_value_t js_crypto_scrypt_sync(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_mkerr(js, "scryptSync requires password, salt, and keylen");
  return crypto_scrypt_result(js, args, nargs, 3);
}

static ant_value_t js_crypto_scrypt(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 4) return js_mkerr(js, "scrypt requires a callback");
  int callback_index = (vtype(args[3]) == T_FUNC || vtype(args[3]) == T_CFUNC) ? 3 : 4;
  if (callback_index >= nargs || (vtype(args[callback_index]) != T_FUNC && vtype(args[callback_index]) != T_CFUNC)) {
    return js_mkerr(js, "scrypt requires a callback");
  }
  ant_value_t result = crypto_scrypt_result(js, args, nargs, callback_index == 3 ? -1 : 3);
  ant_value_t cb_args[2] = { is_err(result) ? result : js_mknull(), is_err(result) ? js_mkundef() : result };
  ant_value_t cb_result = sv_vm_call(js->vm, js, args[callback_index], js_mkundef(), cb_args, 2, NULL, false);
  return is_err(cb_result) ? cb_result : js_mkundef();
}

static ant_value_t create_crypto_obj(ant_t *js) {
  ant_value_t crypto_obj = js_mkobj(js);
  ant_value_t subtle_obj = js_mkobj(js);
  
  js_set(js, crypto_obj, "random", js_mkfun(js_crypto_random));
  js_set(js, crypto_obj, "randomBytes", js_mkfun(js_crypto_random_bytes));
  js_set(js, crypto_obj, "randomFillSync", js_mkfun(js_crypto_random_fill_sync));
  js_set(js, crypto_obj, "randomUUID", js_mkfun(js_crypto_random_uuid));
  js_set(js, crypto_obj, "randomUUIDv7", js_mkfun(js_crypto_random_uuidv7));
  js_set(js, crypto_obj, "getRandomValues", js_mkfun(js_crypto_get_random_values));
  // TODO: add remaining WebCrypto key export, key derivation, wrapping, and asymmetric algorithms.
  js_set(js, subtle_obj, "digest", js_mkfun(js_crypto_subtle_digest));
  js_set(js, subtle_obj, "importKey", js_mkfun(js_crypto_subtle_import_key));
  js_set(js, subtle_obj, "sign", js_mkfun(js_crypto_subtle_sign));
  js_set(js, subtle_obj, "verify", js_mkfun(js_crypto_subtle_verify));
  js_set(js, subtle_obj, "encrypt", js_mkfun(js_crypto_subtle_encrypt));
  js_set(js, subtle_obj, "decrypt", js_mkfun(js_crypto_subtle_decrypt));
  js_set(js, subtle_obj, "timingSafeEqual", js_mkfun(js_crypto_timing_safe_equal));
  js_set_sym(js, subtle_obj, get_toStringTag_sym(), js_mkstr(js, "SubtleCrypto", 12));
  js_set(js, crypto_obj, "subtle", subtle_obj);
  
  js_set_sym(js, crypto_obj, get_toStringTag_sym(), js_mkstr(js, "Crypto", 6));
  return crypto_obj;
}

void init_crypto_module() {
  ant_t *js = rt->js;
  ant_value_t crypto_obj = create_crypto_obj(js);
  js_set(js, js_glob(js), "crypto", crypto_obj);
}

ant_value_t crypto_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  ant_value_t webcrypto = create_crypto_obj(js);
  
  js_set(js, lib, "webcrypto", webcrypto);
  js_set(js, lib, "subtle", js_get(js, webcrypto, "subtle"));
  js_set(js, lib, "hash", js_mkfun(js_crypto_hash));
  js_set(js, lib, "createHash", js_mkfun(js_crypto_create_hash));
  js_set(js, lib, "createHmac", js_mkfun(js_crypto_create_hmac));
  js_set(js, lib, "createCipheriv", js_mkfun(js_crypto_create_cipheriv));
  js_set(js, lib, "createDecipheriv", js_mkfun(js_crypto_create_decipheriv));
  js_set(js, lib, "pbkdf2", js_mkfun(js_crypto_pbkdf2));
  js_set(js, lib, "pbkdf2Sync", js_mkfun(js_crypto_pbkdf2_sync));
  js_set(js, lib, "scrypt", js_mkfun(js_crypto_scrypt));
  js_set(js, lib, "scryptSync", js_mkfun(js_crypto_scrypt_sync));
  js_set(js, lib, "randomBytes", js_mkfun(js_crypto_random_bytes));
  js_set(js, lib, "randomFillSync", js_mkfun(js_crypto_random_fill_sync));
  js_set(js, lib, "randomUUID", js_mkfun(js_crypto_random_uuid));
  js_set(js, lib, "getRandomValues", js_mkfun(js_crypto_get_random_values));
  js_set(js, lib, "getCiphers", js_mkfun(js_crypto_get_ciphers));
  js_set(js, lib, "getCurves", js_mkfun(js_crypto_get_curves));
  js_set(js, lib, "getHashes", js_mkfun(js_crypto_get_hashes));
  js_set(js, lib, "timingSafeEqual", js_mkfun(js_crypto_timing_safe_equal));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "crypto", 6));

  return lib;
}
