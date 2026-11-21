#include "modules/bigint.h"
#include "napi_internal.h"

static napi_status BigInt_Store(napi_env env, ant_value_t value, napi_value *result) {
  if (Napi_Failed(env, value)) return Ant_Napi_CapturePending(env, value);
  *result = Ant_Napi_Pin(env, value);
  return Napi_Ok(env);
}

static bool BigInt_Check(napi_env env, napi_value value, napi_status *status) {
  if (!env || !env->js) *status = Napi_InvalidArg(env);
  else if (vtype(Napi_Decode(value)) != kTypeBigInt) *status = Ant_Napi_SetLastError(env, napi_bigint_expected, NULL);
  else return true;
  return false;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_bigint_int64(napi_env env, int64_t value, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  return BigInt_Store(env, bigint_from_int64(env->js, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  return BigInt_Store(env, bigint_from_uint64(env->js, value), result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_bigint_words(
  napi_env env, int sign_bit, size_t word_count, const uint64_t *words, napi_value *result
) {
  if (!env || !env->js || !result || (word_count > 0 && !words)) return Napi_InvalidArg(env);
  if (word_count > INT_MAX) return Ant_Napi_SetLastError(env, napi_invalid_arg, "word count overflow");

  size_t limb_count = word_count * 2;
  uint32_t *limbs = calloc(limb_count ? limb_count : 1, sizeof(*limbs));
  if (!limbs) return Napi_OutOfMemory(env);

  for (size_t i = 0; i < word_count; i++) {
    limbs[i * 2] = (uint32_t)words[i];
    limbs[i * 2 + 1] = (uint32_t)(words[i] >> 32);
  }

  ant_value_t value = bigint_from_limbs(env->js, limbs, limb_count, sign_bit != 0);
  free(limbs);

  return BigInt_Store(env, value, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_bigint_int64(napi_env env, napi_value value, int64_t *result, bool *lossless) {
  napi_status status;
  if (!BigInt_Check(env, value, &status)) return status;
  if (!result || !lossless) return Napi_InvalidArg(env);

  int64_t exact = 0;
  ant_value_t bigint = Napi_Decode(value);
  *lossless = bigint_to_int64_checked(env->js, bigint, &exact);
  bigint_to_int64_wrapping(env->js, bigint, result);

  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_bigint_uint64(napi_env env, napi_value value, uint64_t *result, bool *lossless) {
  napi_status status;
  if (!BigInt_Check(env, value, &status)) return status;
  if (!result || !lossless) return Napi_InvalidArg(env);

  uint64_t exact = 0;
  ant_value_t bigint = Napi_Decode(value);
  *lossless = bigint_to_uint64_checked(env->js, bigint, &exact);
  bigint_to_uint64_wrapping(env->js, bigint, result);

  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_bigint_words(
  napi_env env, napi_value value, int *sign_bit, size_t *word_count, uint64_t *words
) {
  napi_status status;
  if (!BigInt_Check(env, value, &status)) return status;
  if (!word_count) return Napi_InvalidArg(env);

  size_t limb_count = 0;
  ant_value_t bigint = Napi_Decode(value);
  const uint32_t *limbs = bigint_limbs(env->js, bigint, &limb_count);
  while (limb_count > 0 && limbs[limb_count - 1] == 0) limb_count--;

  size_t needed = (limb_count + 1) / 2;
  size_t copied = words && *word_count < needed ? *word_count : needed;

  for (size_t i = 0; words && i < copied; i++) {
    uint64_t low = limbs[i * 2];
    uint64_t high = i * 2 + 1 < limb_count ? limbs[i * 2 + 1] : 0;
    words[i] = low | (high << 32);
  }

  if (sign_bit) *sign_bit = bigint_is_negative(env->js, bigint) && limb_count > 0;
  *word_count = words ? copied : needed;
  return Napi_Ok(env);
}
