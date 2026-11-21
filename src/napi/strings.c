#include "utf8.h"
#include "napi_internal.h"

static size_t Utf16_Length(const char16_t *str) {
  size_t length = 0;
  while (str[length]) length++;
  return length;
}

static size_t Utf8_FloorToBoundary(const char *str, size_t length) {
  while (length > 0 && ((unsigned char)str[length] & 0xC0) == 0x80) length--;
  return length;
}

static napi_status String_Decode(
  napi_env env, const void *bytes, size_t byte_length,
  const char *encoding, napi_value *result
) {
  ant_value_t out = Ant_Value_Undefined;

  if (byte_length == 0) out = js_mkstr(env->js, "", 0);
  else if (!buffer_bytes_to_string(env->js, bytes, byte_length, encoding, strlen(encoding), &out))
    return Ant_Napi_SetLastError(env, napi_generic_failure, "string decoding failed");

  if (Napi_Failed(env, out)) return Ant_Napi_CapturePending(env, out);
  *result = Ant_Napi_Pin(env, out);
  return Napi_Ok(env);
}

static napi_status String_FromUtf8(napi_env env, const char *str, size_t length, napi_value *result) {
  if (!env || !env->js || !result || (!str && length != 0)) return Napi_InvalidArg(env);
  if (length == NAPI_AUTO_LENGTH) length = strlen(str);

  ant_value_t out = js_mkstr(env->js, str ? str : "", length);
  if (Napi_Failed(env, out)) return Ant_Napi_CapturePending(env, out);

  *result = Ant_Napi_Pin(env, out);
  return Napi_Ok(env);
}

static napi_status String_FromLatin1(napi_env env, const char *str, size_t length, napi_value *result) {
  if (!env || !env->js || !result || (!str && length != 0)) return Napi_InvalidArg(env);
  if (length == NAPI_AUTO_LENGTH) length = strlen(str);
  return String_Decode(env, str, length, "latin1", result);
}

static napi_status String_FromUtf16(napi_env env, const char16_t *str, size_t length, napi_value *result) {
  if (!env || !env->js || !result || (!str && length != 0)) return Napi_InvalidArg(env);
  if (length == NAPI_AUTO_LENGTH) length = Utf16_Length(str);
  if (length > SIZE_MAX / sizeof(char16_t)) return Ant_Napi_SetLastError(env, napi_invalid_arg, "string too long");
  return String_Decode(env, str, length * sizeof(char16_t), "utf16le", result);
}

static napi_status String_Read(napi_env env, napi_value value, const char **str, size_t *length) {
  if (!env || !env->js) return Napi_InvalidArg(env);
  if (vtype(Napi_Decode(value)) != kTypeString) return Ant_Napi_SetLastError(env, napi_string_expected, NULL);

  *str = js_getstr(env->js, Napi_Decode(value), length);
  return *str ? napi_ok : Ant_Napi_SetLastError(env, napi_string_expected, NULL);
}

static napi_status String_FinishExternal(
  napi_env env, napi_status status, void *str,
  node_api_basic_finalize finalize_callback, void *finalize_hint, bool *copied
) {
  if (status != napi_ok) return status;
  if (copied) *copied = true;
  if (finalize_callback) finalize_callback(env, str, finalize_hint);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_string_utf8(napi_env env, const char *str, size_t length, napi_value *result) {
  return String_FromUtf8(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_string_latin1(napi_env env, const char *str, size_t length, napi_value *result) {
  return String_FromLatin1(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_string_utf16(napi_env env, const char16_t *str, size_t length, napi_value *result) {
  return String_FromUtf16(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_property_key_utf8(napi_env env, const char *str, size_t length, napi_value *result) {
  return String_FromUtf8(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_property_key_latin1(napi_env env, const char *str, size_t length, napi_value *result) {
  return String_FromLatin1(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_property_key_utf16(napi_env env, const char16_t *str, size_t length, napi_value *result) {
  return String_FromUtf16(env, str, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_external_string_latin1(
  napi_env env, char *str, size_t length, node_api_basic_finalize finalize_callback,
  void *finalize_hint, napi_value *result, bool *copied
) {
  napi_status status = String_FromLatin1(env, str, length, result);
  return String_FinishExternal(env, status, str, finalize_callback, finalize_hint, copied);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_external_string_utf16(
  napi_env env, char16_t *str, size_t length, node_api_basic_finalize finalize_callback,
  void *finalize_hint, napi_value *result, bool *copied
) {
  napi_status status = String_FromUtf16(env, str, length, result);
  return String_FinishExternal(env, status, str, finalize_callback, finalize_hint, copied);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_string_utf8(napi_env env, napi_value value, char *buf, size_t bufsize, size_t *result) {
  size_t length = 0;
  const char *str = NULL;

  napi_status status = String_Read(env, value, &str, &length);
  if (status != napi_ok) return status;

  if (!buf) {
    if (!result) return Napi_InvalidArg(env);
    *result = length;
    return Napi_Ok(env);
  }

  size_t copied = 0;
  if (bufsize > 0) {
    copied = length < bufsize - 1 ? length : Utf8_FloorToBoundary(str, bufsize - 1);
    memcpy(buf, str, copied);
    buf[copied] = '\0';
  }

  if (result) *result = copied;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_string_latin1(napi_env env, napi_value value, char *buf, size_t bufsize, size_t *result) {
  size_t length = 0;
  const char *str = NULL;

  napi_status status = String_Read(env, value, &str, &length);
  if (status != napi_ok) return status;
  if (!buf && !result) return Napi_InvalidArg(env);

  size_t latin1_length = 0;
  uint8_t *latin1 = utf8_to_latin1(str, length, &latin1_length, NULL);
  if (!latin1) return Napi_OutOfMemory(env);

  size_t copied = latin1_length;
  if (buf) {
    copied = bufsize == 0 ? 0 : (latin1_length < bufsize - 1 ? latin1_length : bufsize - 1);
    if (bufsize > 0) {
      memcpy(buf, latin1, copied);
      buf[copied] = '\0';
    }
  }

  free(latin1);
  if (result) *result = copied;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_value_string_utf16(napi_env env, napi_value value, char16_t *buf, size_t bufsize, size_t *result) {
  size_t byte_length = 0;
  const char *str = NULL;

  napi_status status = String_Read(env, value, &str, &byte_length);
  if (status != napi_ok) return status;
  if (!buf && !result) return Napi_InvalidArg(env);

  size_t copied = (size_t)str_utf16_len(env->js, Napi_Decode(value));
  if (buf) {
    copied = bufsize == 0 ? 0 : (copied < bufsize - 1 ? copied : bufsize - 1);
    utf16_iterator_t units = utf16_iterator(str, byte_length);
    for (size_t i = 0; i < copied; i++) utf16_next_code_unit(&units, &buf[i]);
    if (bufsize > 0) buf[copied] = 0;
  }

  if (result) *result = copied;
  return Napi_Ok(env);
}
