#include <compat.h> // IWYU pragma: keep

#include <string.h>
#include <time.h>
#include <limits.h>
#include <openssl/evp.h>
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
#include "modules/crypto.h"
#include "modules/buffer.h"
#include "modules/symbol.h"

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

enum { CRYPTO_HASH_NATIVE_TAG = 0x48415348u }; // HASH

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

int crypto_fill_random(void *buf, size_t len) {
  if (len == 0) return 0;
  if (len > (size_t)INT_MAX) return -1;
  return RAND_bytes((uint8_t *)buf, (int)len) == 1 ? 0 : -1;
}

static inline ant_value_t crypto_random_error(ant_t *js) {
  return js_mkerr(js, "secure random generation failed");
}

static ant_value_t crypto_format_uuid_v4(ant_t *js, const uint8_t uuid[16]) {
  static char lut[256][2];
  static bool lut_init = false;
  char uuid_str[36];

  if (!lut_init) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 256; i++) {
      lut[i][0] = hex[(unsigned)i >> 4];
      lut[i][1] = hex[(unsigned)i & 0x0f];
    }
    lut_init = true;
  }

  memcpy(uuid_str + 0,  lut[uuid[0]], 2);
  memcpy(uuid_str + 2,  lut[uuid[1]], 2);
  memcpy(uuid_str + 4,  lut[uuid[2]], 2);
  memcpy(uuid_str + 6,  lut[uuid[3]], 2);
  
  uuid_str[8] = '-';
  memcpy(uuid_str + 9,  lut[uuid[4]], 2);
  memcpy(uuid_str + 11, lut[uuid[5]], 2);
  
  uuid_str[13] = '-';
  memcpy(uuid_str + 14, lut[uuid[6]], 2);
  memcpy(uuid_str + 16, lut[uuid[7]], 2);
  
  uuid_str[18] = '-';
  memcpy(uuid_str + 19, lut[uuid[8]], 2);
  memcpy(uuid_str + 21, lut[uuid[9]], 2);
  
  uuid_str[23] = '-';
  memcpy(uuid_str + 24, lut[uuid[10]], 2);
  memcpy(uuid_str + 26, lut[uuid[11]], 2);
  memcpy(uuid_str + 28, lut[uuid[12]], 2);
  memcpy(uuid_str + 30, lut[uuid[13]], 2);
  memcpy(uuid_str + 32, lut[uuid[14]], 2);
  memcpy(uuid_str + 34, lut[uuid[15]], 2);

  return js_mkstr(js, uuid_str, sizeof(uuid_str));
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
  uint8_t uuid[16];
  if (crypto_fill_random(uuid, sizeof(uuid)) < 0) return crypto_random_error(js);

  uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40);
  uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);

  return crypto_format_uuid_v4(js, uuid);
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

// TODO: extend subtle
static ant_value_t crypto_subtle_get_algorithm_name(ant_t *js, ant_value_t algorithm) {
  if (vtype(algorithm) == T_STR) return js_tostring_val(js, algorithm);
  if (is_object_type(algorithm)) {
    ant_value_t name = js_get(js, algorithm, "name");
    if (is_err(name)) return name;
    return js_tostring_val(js, name);
  }
  return js_mkerr_typed(js, JS_ERR_TYPE, "Algorithm must be a string or object with a name");
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

static ant_value_t create_crypto_obj(ant_t *js) {
  ant_value_t crypto_obj = js_mkobj(js);
  ant_value_t subtle_obj = js_mkobj(js);
  
  js_set(js, crypto_obj, "random", js_mkfun(js_crypto_random));
  js_set(js, crypto_obj, "randomBytes", js_mkfun(js_crypto_random_bytes));
  js_set(js, crypto_obj, "randomFillSync", js_mkfun(js_crypto_random_fill_sync));
  js_set(js, crypto_obj, "randomUUID", js_mkfun(js_crypto_random_uuid));
  js_set(js, crypto_obj, "randomUUIDv7", js_mkfun(js_crypto_random_uuidv7));
  js_set(js, crypto_obj, "getRandomValues", js_mkfun(js_crypto_get_random_values));
  js_set(js, subtle_obj, "digest", js_mkfun(js_crypto_subtle_digest));
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
  js_set(js, lib, "hash", js_mkfun(js_crypto_hash));
  js_set(js, lib, "createHash", js_mkfun(js_crypto_create_hash));
  js_set(js, lib, "randomBytes", js_mkfun(js_crypto_random_bytes));
  js_set(js, lib, "randomFillSync", js_mkfun(js_crypto_random_fill_sync));
  js_set(js, lib, "randomUUID", js_mkfun(js_crypto_random_uuid));
  js_set(js, lib, "getRandomValues", js_mkfun(js_crypto_get_random_values));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "crypto", 6));

  return lib;
}
