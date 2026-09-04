#include "napi_internal.h"

#if defined(__has_include)
#if __has_include(<uchar.h>)
#include <uchar.h>
#else
typedef uint16_t char16_t;
#endif
#else
typedef uint16_t char16_t;
#endif

void ant_napi_link_values(void) {}

static bool napi_get_typedarray_data(
  ant_t *js,
  napi_value value,
  TypedArrayData **out
) {
  TypedArrayData *ta = buffer_get_typedarray_data((ant_value_t)value);
  if (!ta || !ta->buffer || ta->buffer->is_detached) return false;
  *out = ta;
  return true;
}

static bool napi_to_ant_typedarray_type(
  napi_typedarray_type in,
  TypedArrayType *out
) {
  switch (in) {
    case napi_int8_array: *out = TYPED_ARRAY_INT8; return true;
    case napi_uint8_array: *out = TYPED_ARRAY_UINT8; return true;
    case napi_uint8_clamped_array: *out = TYPED_ARRAY_UINT8_CLAMPED; return true;
    case napi_int16_array: *out = TYPED_ARRAY_INT16; return true;
    case napi_uint16_array: *out = TYPED_ARRAY_UINT16; return true;
    case napi_int32_array: *out = TYPED_ARRAY_INT32; return true;
    case napi_uint32_array: *out = TYPED_ARRAY_UINT32; return true;
    case napi_float32_array: *out = TYPED_ARRAY_FLOAT32; return true;
    case napi_float64_array: *out = TYPED_ARRAY_FLOAT64; return true;
    case napi_bigint64_array: *out = TYPED_ARRAY_BIGINT64; return true;
    case napi_biguint64_array: *out = TYPED_ARRAY_BIGUINT64; return true;
    default: return false;
  }
}

static napi_typedarray_type napi_from_ant_typedarray_type(TypedArrayType in) {
  switch (in) {
    case TYPED_ARRAY_INT8: return napi_int8_array;
    case TYPED_ARRAY_UINT8: return napi_uint8_array;
    case TYPED_ARRAY_UINT8_CLAMPED: return napi_uint8_clamped_array;
    case TYPED_ARRAY_INT16: return napi_int16_array;
    case TYPED_ARRAY_UINT16: return napi_uint16_array;
    case TYPED_ARRAY_INT32: return napi_int32_array;
    case TYPED_ARRAY_UINT32: return napi_uint32_array;
    case TYPED_ARRAY_FLOAT32: return napi_float32_array;
    case TYPED_ARRAY_FLOAT64: return napi_float64_array;
    case TYPED_ARRAY_BIGINT64: return napi_bigint64_array;
    case TYPED_ARRAY_BIGUINT64: return napi_biguint64_array;
    default: return napi_uint8_array;
  }
}

static ant_value_t napi_make_string(ant_t *js, const char *s, size_t len) {
  if (!s) return js_mkstr(js, "", 0);
  if (len == NAPI_AUTO_LENGTH) len = strlen(s);
  return js_mkstr(js, s, len);
}

static bool napi_checked_add_size(size_t a, size_t b, size_t *out) {
  if (a > SIZE_MAX - b) return false;
  *out = a + b;
  return true;
}

static bool napi_checked_mul_size(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a) return false;
  *out = a * b;
  return true;
}

static bool napi_make_bigint_limbs(
  ant_t *js,
  const uint32_t *limbs,
  size_t count,
  bool negative,
  ant_value_t *out
) {
  uint32_t zero = 0;
  if (!out) return false;

  if (!limbs || count == 0) {
    limbs = &zero;
    count = 1;
  }

  while (count > 1 && limbs[count - 1] == 0) count--;
  if (count == 1 && limbs[0] == 0) negative = false;
  if (count > UINT32_MAX) return false;

  size_t limbs_bytes = 0;
  if (!napi_checked_mul_size(count, sizeof(uint32_t), &limbs_bytes)) return false;

  size_t payload_size = 0;
  if (!napi_checked_add_size(offsetof(napi_bigint_payload_t, limbs), limbs_bytes, &payload_size)) {
    return false;
  }

  napi_bigint_payload_t *payload = (napi_bigint_payload_t *)js_type_alloc(
    js, ANT_ALLOC_BIGINT, payload_size,
    _Alignof(napi_bigint_payload_t)
  );

  if (!payload) return false;
  payload->sign = negative ? 1 : 0;
  payload->pad[0] = 0;
  payload->pad[1] = 0;
  payload->pad[2] = 0;
  payload->limb_count = (uint32_t)count;
  memcpy(payload->limbs, limbs, limbs_bytes);
  *out = mkref(kTypeBigInt, payload);

  return true;
}

static const napi_bigint_payload_t *napi_bigint_payload(napi_value value) {
  return (const napi_bigint_payload_t *)vptr((ant_value_t)value);
}

static const uint32_t *napi_bigint_limbs(napi_value value, size_t *count) {
  const napi_bigint_payload_t *payload = napi_bigint_payload(value);
  if (!payload) {
    if (count) *count = 0;
    return NULL;
  }

  size_t limb_count = payload->limb_count;
  if (limb_count == 0) limb_count = 1;

  while (limb_count > 1 && payload->limbs[limb_count - 1] == 0) limb_count--;
  if (count) *count = limb_count;

  return payload->limbs;
}

static bool napi_bigint_is_negative(napi_value value) {
  const napi_bigint_payload_t *payload = napi_bigint_payload(value);
  return payload && payload->sign == 1;
}

static bool napi_bigint_limbs_is_zero(const uint32_t *limbs, size_t count) {
  return count <= 1 && (!limbs || limbs[0] == 0);
}

static uint64_t napi_bigint_low_u64(const uint32_t *limbs, size_t count) {
  uint64_t out = 0;
  if (count > 0 && limbs) out |= (uint64_t)limbs[0];
  if (count > 1 && limbs) out |= ((uint64_t)limbs[1] << 32);
  return out;
}

static napi_status napi_create_date_common(napi_env env, double time, napi_value *result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ant_t *js = nenv->js;
  ant_value_t ctor = js_get(js, js_glob(js), "Date");
  if (!is_callable(ctor)) return napi_set_last(env, napi_generic_failure, "Date constructor unavailable");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  ant_value_t argv[1] = {js_mknum(time)};
  ant_value_t saved = js->new_target;
  js->new_target = ctor;
  ant_value_t out = sv_vm_call(js->vm, js, ctor, obj, argv, 1, NULL, true);
  js->new_target = saved;

  if (is_err(out) || js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, is_object_type(out) ? out : obj);
  return napi_set_last(env, napi_ok, NULL);
}

static napi_status napi_make_error_object(
  napi_env env,
  const char *name,
  napi_value msg,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ant_t *js = nenv->js;
  ant_value_t message = (ant_value_t)msg;
  if (vtype(message) != kTypeString) {
    message = coerce_to_str(js, message);
    if (is_err(message)) return napi_check_pending_from_result(env, message);
  }

  ant_value_t err = js_mkobj(js);
  js_set(js, err, "name", js_mkstr(js, name, strlen(name)));
  js_set(js, err, "message", message);

  ant_value_t proto = js_get_ctor_proto(js, name, strlen(name));
  if (is_object_type(proto)) js_set_proto_init(err, proto);

  *result = NAPI_RETURN(nenv, err);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mkundef();
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknull();
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value *result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, js_glob(nenv->js));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_bool(value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value *result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, js_mkobj(nenv->js));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value *result) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, js_mkarr(nenv->js));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_array_with_length(
  napi_env env,
  size_t length,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_value_t arr = js_mkarr(nenv->js);
  ant_value_t r = js_setprop(
    nenv->js, arr,
    js_mkstr(nenv->js, "length", 6),
    js_mknum((double)length)
  );
  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  *result = NAPI_RETURN(nenv, arr);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_double(napi_env env, double value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknum(value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknum((double)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknum((double)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_int64(napi_env env, int64_t value, napi_value *result) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = (napi_value)js_mknum((double)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_latin1(
  napi_env env,
  const char *str,
  size_t length,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !str) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, napi_make_string(nenv->js, str, length));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_string_utf8(
  napi_env env,
  const char *str,
  size_t length,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !str) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = NAPI_RETURN(nenv, napi_make_string(nenv->js, str, length));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_date(
  napi_env env,
  double time,
  napi_value *result
) {
  return napi_create_date_common(env, time, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_int64(
  napi_env env,
  int64_t value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  uint64_t magnitude = value < 0
    ? (uint64_t)(-(value + 1)) + 1
    : (uint64_t)value;

  uint32_t limbs[2] = {
    (uint32_t)(magnitude & 0xffffffffu),
    (uint32_t)(magnitude >> 32)
  };

  size_t count = limbs[1] == 0 ? 1 : 2;
  ant_value_t out = js_mkundef();

  if (!napi_make_bigint_limbs(nenv->js, limbs, count, value < 0, &out)) {
    return napi_set_last(env, napi_generic_failure, "out of memory");
  }

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_uint64(
  napi_env env,
  uint64_t value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  uint32_t limbs[2] = {
    (uint32_t)(value & 0xffffffffu),
    (uint32_t)(value >> 32)
  };
  size_t count = limbs[1] == 0 ? 1 : 2;

  ant_value_t out = js_mkundef();
  if (!napi_make_bigint_limbs(nenv->js, limbs, count, false, &out)) {
    return napi_set_last(env, napi_generic_failure, "out of memory");
  }

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_bigint_words(
  napi_env env,
  int sign_bit,
  size_t word_count,
  const uint64_t *words,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || (word_count > 0 && !words)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  size_t limb_count = 1;
  if (word_count > 0 && !napi_checked_mul_size(word_count, 2, &limb_count)) {
    return napi_set_last(env, napi_invalid_arg, "word count overflow");
  }
  uint32_t *limbs = (uint32_t *)calloc(limb_count, sizeof(uint32_t));
  if (!limbs) return napi_set_last(env, napi_generic_failure, "out of memory");

  if (word_count == 0) limbs[0] = 0;
  else for (size_t i = 0; i < word_count; i++) {
    limbs[i * 2] = (uint32_t)(words[i] & 0xffffffffu);
    limbs[(i * 2) + 1] = (uint32_t)(words[i] >> 32);
  }

  ant_value_t out = js_mkundef();
  bool ok = napi_make_bigint_limbs(nenv->js, limbs, limb_count, sign_bit != 0, &out);
  free(limbs);
  if (!ok) return napi_set_last(env, napi_generic_failure, "out of memory");

  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_symbol(
  napi_env env,
  napi_value description,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  const char *desc = NULL;
  if (vtype((ant_value_t)description) == kTypeString) {
    desc = js_getstr(nenv->js, (ant_value_t)description, NULL);
  } else if (!is_undefined((ant_value_t)description) && !is_null((ant_value_t)description)) {
    ant_value_t s = coerce_to_str(nenv->js, (ant_value_t)description);
    if (is_err(s) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, s);
    desc = js_getstr(nenv->js, s, NULL);
  }

  *result = NAPI_RETURN(nenv, js_mksym(nenv->js, desc));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_error(
  napi_env env,
  napi_value code,
  napi_value msg,
  napi_value *result
) {
  return napi_make_error_object(env, "Error", msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_type_error(
  napi_env env,
  napi_value code,
  napi_value msg,
  napi_value *result
) {
  return napi_make_error_object(env, "TypeError", msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_range_error(
  napi_env env,
  napi_value code,
  napi_value msg,
  napi_value *result
) {
  return napi_make_error_object(env, "RangeError", msg, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_promise(
  napi_env env,
  napi_deferred *deferred,
  napi_value *promise
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !deferred || !promise) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  struct napi_deferred__ *def = (struct napi_deferred__ *)calloc(1, sizeof(*def));
  if (!def) return napi_set_last(env, napi_generic_failure, "out of memory");

  ant_value_t p = js_mkpromise(nenv->js);
  def->env = nenv;
  def->promise_val = p;
  def->settled = false;

  def->prev = NULL;
  def->next = nenv->deferreds;
  if (nenv->deferreds) nenv->deferreds->prev = def;
  nenv->deferreds = def;

  *deferred = (napi_deferred)def;
  *promise = (napi_value)p;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_resolve_deferred(
  napi_env env,
  napi_deferred deferred,
  napi_value resolution
) {
  struct napi_deferred__ *def = (struct napi_deferred__ *)deferred;
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !def || def->settled) {
    return napi_set_last(env, napi_invalid_arg, "invalid deferred");
  }

  ant_value_t promise = def->promise_val;
  js_resolve_promise(nenv->js, promise, (ant_value_t)resolution);
  def->settled = true;
  def->promise_val = js_mkundef();

  if (def->prev) def->prev->next = def->next;
  else if (nenv->deferreds == def) nenv->deferreds = def->next;
  if (def->next) def->next->prev = def->prev;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_reject_deferred(
  napi_env env,
  napi_deferred deferred,
  napi_value rejection
) {
  struct napi_deferred__ *def = (struct napi_deferred__ *)deferred;
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !def || def->settled) {
    return napi_set_last(env, napi_invalid_arg, "invalid deferred");
  }

  ant_value_t promise = def->promise_val;
  js_reject_promise(nenv->js, promise, (ant_value_t)rejection);
  def->settled = true;
  def->promise_val = js_mkundef();

  if (def->prev) def->prev->next = def->next;
  else if (nenv->deferreds == def) nenv->deferreds = def->next;
  if (def->next) def->next->prev = def->prev;

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer(
  napi_env env,
  size_t length,
  void **data,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ArrayBufferData *buf = create_array_buffer_data(length);
  if (!buf) return napi_set_last(env, napi_generic_failure, "allocation failed");

  ant_value_t value = create_typed_array(nenv->js, TYPED_ARRAY_UINT8, buf, 0, length, "Buffer");
  if (is_err(value)) return napi_check_pending_from_result(env, value);

  if (data) *data = buf->data;
  *result = NAPI_RETURN(nenv, value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_buffer_copy(
  napi_env env,
  size_t length,
  const void *data,
  void **result_data,
  napi_value *result
) {
  void *buf_ptr = NULL;
  napi_status st = napi_create_buffer(env, length, &buf_ptr, result);
  if (st != napi_ok) return st;

  if (length > 0 && data && buf_ptr) memcpy(buf_ptr, data, length);
  if (result_data) *result_data = buf_ptr;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_buffer(
  napi_env env,
  size_t length,
  void *data,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_value *result
) {
  (void)finalize_cb;
  (void)finalize_hint;
  return napi_create_buffer_copy(env, length, data, NULL, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_arraybuffer(
  napi_env env,
  size_t byte_length,
  void **data,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ArrayBufferData *ab = create_array_buffer_data(byte_length);
  if (!ab) return napi_set_last(env, napi_generic_failure, "allocation failed");

  ant_value_t ab_obj = create_arraybuffer_obj(nenv->js, ab);
  free_array_buffer_data(ab);

  if (data) *data = ab->data;
  *result = NAPI_RETURN(nenv, ab_obj);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_external_arraybuffer(
  napi_env env,
  void *external_data,
  size_t byte_length,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint,
  napi_value *result
) {
  (void)finalize_cb;
  (void)finalize_hint;
  void *out = NULL;
  napi_status st = napi_create_arraybuffer(env, byte_length, &out, result);
  if (st != napi_ok) return st;
  if (external_data && out && byte_length > 0) memcpy(out, external_data, byte_length);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_set_instance_data(
  napi_env env,
  void *data,
  node_api_basic_finalize finalize_cb,
  void *finalize_hint
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv) return napi_set_last(env, napi_invalid_arg, "invalid env");

  nenv->instance_data = data;
  nenv->instance_data_finalize_cb = finalize_cb;
  nenv->instance_data_finalize_hint = finalize_hint;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_instance_data(
  napi_env env,
  void **data
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !data) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  *data = nenv->instance_data;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_typedarray(
  napi_env env,
  napi_typedarray_type type,
  size_t length,
  napi_value arraybuffer,
  size_t byte_offset,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result || !is_object_type((ant_value_t)arraybuffer)) {
    return napi_set_last(env, napi_invalid_arg, "invalid argument");
  }

  ArrayBufferData *ab = buffer_get_arraybuffer_data((ant_value_t)arraybuffer);
  if (!ab || ab->is_detached) return napi_set_last(env, napi_arraybuffer_expected, "invalid arraybuffer");

  TypedArrayType ta_type;
  if (!napi_to_ant_typedarray_type(type, &ta_type)) {
    return napi_set_last(env, napi_invalid_arg, "invalid typedarray type");
  }

  size_t element_size = 1;
  switch (ta_type) {
    case TYPED_ARRAY_INT16:
    case TYPED_ARRAY_UINT16: element_size = 2; break;
    case TYPED_ARRAY_INT32:
    case TYPED_ARRAY_UINT32:
    case TYPED_ARRAY_FLOAT32: element_size = 4; break;
    case TYPED_ARRAY_FLOAT64:
    case TYPED_ARRAY_BIGINT64:
    case TYPED_ARRAY_BIGUINT64: element_size = 8; break;
    default: break;
  }

  size_t byte_len = length * element_size;
  if (byte_offset + byte_len > ab->length) {
    return napi_set_last(env, napi_invalid_arg, "typedarray out of bounds");
  }

  ant_value_t out = create_typed_array_with_buffer(
    nenv->js,
    ta_type,
    ab,
    byte_offset,
    length,
    buffer_typedarray_type_name(ta_type),
    (ant_value_t)arraybuffer
  );

  if (is_err(out)) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}


NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_double(
  napi_env env,
  napi_value value,
  double *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != kTypeNumber) return napi_set_last(env, napi_number_expected, "number expected");
  *result = js_getnum((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int32(
  napi_env env,
  napi_value value,
  int32_t *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != kTypeNumber) return napi_set_last(env, napi_number_expected, "number expected");
  *result = (int32_t)js_getnum((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_uint32(
  napi_env env,
  napi_value value,
  uint32_t *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != kTypeNumber) return napi_set_last(env, napi_number_expected, "number expected");
  *result = (uint32_t)js_getnum((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_int64(
  napi_env env,
  napi_value value,
  int64_t *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != kTypeNumber) return napi_set_last(env, napi_number_expected, "number expected");
  *result = (int64_t)js_getnum((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bool(
  napi_env env,
  napi_value value,
  bool *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != kTypeBool) return napi_set_last(env, napi_boolean_expected, "boolean expected");
  *result = ((ant_value_t)value == js_true);
  return napi_set_last(env, napi_ok, NULL);
}

static napi_status napi_get_string_common(
  napi_env env,
  napi_value value,
  char *buf,
  size_t bufsize,
  size_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");
  if (vtype((ant_value_t)value) != kTypeString) return napi_set_last(env, napi_string_expected, "string expected");

  size_t len = 0;
  const char *str = js_getstr(nenv->js, (ant_value_t)value, &len);
  if (!str) return napi_set_last(env, napi_string_expected, "string expected");

  if (result) *result = len;
  if (!buf || bufsize == 0) return napi_set_last(env, napi_ok, NULL);

  size_t n = (len < (bufsize - 1)) ? len : (bufsize - 1);
  memcpy(buf, str, n);
  buf[n] = '\0';
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf8(
  napi_env env,
  napi_value value,
  char *buf,
  size_t bufsize,
  size_t *result
) {
  return napi_get_string_common(env, value, buf, bufsize, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_latin1(
  napi_env env,
  napi_value value,
  char *buf,
  size_t bufsize,
  size_t *result
) {
  return napi_get_string_common(env, value, buf, bufsize, result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_string_utf16(
  napi_env env,
  napi_value value,
  char16_t *buf,
  size_t bufsize,
  size_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");
  if (vtype((ant_value_t)value) != kTypeString) return napi_set_last(env, napi_string_expected, "string expected");

  size_t byte_len = 0;
  const char *str = js_getstr(nenv->js, (ant_value_t)value, &byte_len);
  if (!str) return napi_set_last(env, napi_string_expected, "string expected");

  size_t utf16_len = (size_t)str_utf16_len(nenv->js, (ant_value_t)value);
  if (result) *result = utf16_len;
  if (!buf || bufsize == 0) return napi_set_last(env, napi_ok, NULL);

  size_t n = utf16_len < (bufsize - 1) ? utf16_len : (bufsize - 1);
  for (size_t i = 0; i < n; i++) buf[i] = (char16_t)utf16_code_unit_at(str, byte_len, i);
  buf[n] = 0;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_date_value(
  napi_env env,
  napi_value value,
  double *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_date_instance((ant_value_t)value)) return napi_set_last(env, napi_date_expected, "date expected");

  ant_value_t time_val = js_get_slot((ant_value_t)value, SLOT_DATA);
  *result = vtype(time_val) == kTypeNumber ? js_getnum(time_val) : JS_NAN;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_int64(
  napi_env env,
  napi_value value,
  int64_t *result,
  bool *lossless
) {
  if (!env || !result || !lossless) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != kTypeBigInt) return napi_set_last(env, napi_bigint_expected, "bigint expected");

  size_t limb_count = 0;
  const uint32_t *limbs = napi_bigint_limbs(value, &limb_count);
  uint64_t magnitude = napi_bigint_low_u64(limbs, limb_count);
  bool negative = napi_bigint_is_negative(value) && !napi_bigint_limbs_is_zero(limbs, limb_count);
  uint64_t bits = negative ? (uint64_t)(~magnitude + 1) : magnitude;

  *result = (int64_t)bits;
  *lossless = limb_count <= 2
    && ((!negative && magnitude <= (uint64_t)INT64_MAX)
      || (negative && magnitude <= (UINT64_C(1) << 63)));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_uint64(
  napi_env env,
  napi_value value,
  uint64_t *result,
  bool *lossless
) {
  if (!env || !result || !lossless) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != kTypeBigInt) return napi_set_last(env, napi_bigint_expected, "bigint expected");

  size_t limb_count = 0;
  const uint32_t *limbs = napi_bigint_limbs(value, &limb_count);
  uint64_t magnitude = napi_bigint_low_u64(limbs, limb_count);
  bool negative = napi_bigint_is_negative(value) && !napi_bigint_limbs_is_zero(limbs, limb_count);

  *result = negative ? (uint64_t)(~magnitude + 1) : magnitude;
  *lossless = !negative && limb_count <= 2;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_value_bigint_words(
  napi_env env,
  napi_value value,
  int *sign_bit,
  size_t *word_count,
  uint64_t *words
) {
  if (!env || !word_count) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (vtype((ant_value_t)value) != kTypeBigInt) return napi_set_last(env, napi_bigint_expected, "bigint expected");

  size_t limb_count = 0;
  const uint32_t *limbs = napi_bigint_limbs(value, &limb_count);
  size_t actual_words = limb_count == 0 ? 0 : (limb_count + 1) / 2;
  size_t capacity = words ? *word_count : 0;

  if (sign_bit) {
    bool negative = napi_bigint_is_negative(value) && !napi_bigint_limbs_is_zero(limbs, limb_count);
    *sign_bit = negative ? 1 : 0;
  }

  if (words) {
  size_t n = capacity < actual_words ? capacity : actual_words;
  for (size_t i = 0; i < n; i++) {
    uint64_t lo = i * 2 < limb_count ? (uint64_t)limbs[i * 2] : 0;
    uint64_t hi = (i * 2 + 1) < limb_count ? (uint64_t)limbs[i * 2 + 1] : 0;
    words[i] = lo | (hi << 32);
  }}

  *word_count = actual_words;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_array_length(
  napi_env env,
  napi_value value,
  uint32_t *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ant_value_t v = (ant_value_t)value;
  if (vtype(v) == kTypeArray) {
    *result = (uint32_t)js_arr_len(nenv->js, v);
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t len = js_get(nenv->js, v, "length");
  if (is_err(len) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, len);
  if (vtype(len) != kTypeNumber) return napi_set_last(env, napi_array_expected, "array expected");
  *result = (uint32_t)js_getnum(len);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_buffer_info(
  napi_env env,
  napi_value value,
  void **data,
  size_t *length
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");

  TypedArrayData *ta = NULL;
  if (!napi_get_typedarray_data(nenv->js, value, &ta)) {
    return napi_set_last(env, napi_invalid_arg, "not a buffer");
  }

  if (data) *data = ta->buffer->data + ta->byte_offset;
  if (length) *length = ta->byte_length;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_arraybuffer_info(
  napi_env env,
  napi_value arraybuffer,
  void **data,
  size_t *byte_length
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");
  if (!is_object_type((ant_value_t)arraybuffer)) return napi_set_last(env, napi_arraybuffer_expected, "arraybuffer expected");

  ArrayBufferData *ab = buffer_get_arraybuffer_data((ant_value_t)arraybuffer);
  if (!ab || ab->is_detached) return napi_set_last(env, napi_arraybuffer_expected, "arraybuffer expected");

  if (data) *data = ab->data;
  if (byte_length) *byte_length = ab->length;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_typedarray_info(
  napi_env env,
  napi_value typedarray,
  napi_typedarray_type *type,
  size_t *length,
  void **data,
  napi_value *arraybuffer,
  size_t *byte_offset
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");

  TypedArrayData *ta = NULL;
  if (!napi_get_typedarray_data(nenv->js, typedarray, &ta)) {
    return napi_set_last(env, napi_invalid_arg, "typedarray expected");
  }

  if (type) *type = napi_from_ant_typedarray_type(ta->type);
  if (length) *length = ta->length;
  if (data) *data = ta->buffer->data + ta->byte_offset;
  if (arraybuffer) {
    ant_value_t buffer = js_get(nenv->js, (ant_value_t)typedarray, "buffer");
    if (is_err(buffer) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, buffer);
    *arraybuffer = NAPI_RETURN(nenv, buffer);
  }
  if (byte_offset) *byte_offset = ta->byte_offset;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_get_prototype(
  napi_env env,
  napi_value object,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_value_t proto = js_get_proto(nenv->js, (ant_value_t)object);
  if (is_err(proto) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, proto);
  *result = NAPI_RETURN(nenv, proto);
  return napi_set_last(env, napi_ok, NULL);
}


NAPI_EXTERN napi_status NAPI_CDECL napi_typeof(
  napi_env env,
  napi_value value,
  napi_valuetype *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  ant_value_t v = (ant_value_t)value;
  uint8_t t = vtype(v);

  if (ant_napi_is_external(nenv->js, value)) {
    *result = napi_external;
    return napi_set_last(env, napi_ok, NULL);
  }

  switch (t) {
    case kTypeUndefined: *result = napi_undefined; break;
    case kTypeNull: *result = napi_null; break;
    case kTypeBool: *result = napi_boolean; break;
    case kTypeNumber: *result = napi_number; break;
    case kTypeString: *result = napi_string; break;
    case kTypeSymbol: *result = napi_symbol; break;
    case kTypeFunction:
    case kTypeBuiltin: *result = napi_function; break;
    case kTypeBigInt: *result = napi_bigint; break;
    default: *result = napi_object; break;
  }

  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_array(
  napi_env env,
  napi_value value,
  bool *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;

  if (!nenv || !nenv->js) return napi_set_last(env, napi_invalid_arg, "invalid env");
  ant_value_t r = js_is_array_value_checked(nenv->js, (ant_value_t)value, result);

  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_date(
  napi_env env,
  napi_value value,
  bool *result
) {
  if (!env || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = is_date_instance((ant_value_t)value);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_arraybuffer(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type((ant_value_t)value)) { *result = false; return napi_set_last(env, napi_ok, NULL); }

  *result = buffer_get_arraybuffer_data((ant_value_t)value) != NULL;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_buffer(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  TypedArrayData *ta = NULL;
  if (!napi_get_typedarray_data(nenv->js, value, &ta)) {
    *result = false;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t buffer_proto = js_get_ctor_proto(nenv->js, "Buffer", 6);
  *result = is_object_type(buffer_proto)
    && proto_chain_contains(nenv->js, (ant_value_t)value, buffer_proto);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_typedarray(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  TypedArrayData *ta = NULL;
  *result = napi_get_typedarray_data(nenv->js, value, &ta);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_dataview(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type((ant_value_t)value)) {
    *result = false;
  } else {
    *result = buffer_get_dataview_data((ant_value_t)value) != NULL;
  }
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_error(
  napi_env env,
  napi_value value,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  if (!is_object_type((ant_value_t)value)) {
    *result = false;
  } else {
    ant_value_t et = js_get_slot((ant_value_t)value, SLOT_ERR_TYPE);
    *result = vtype(et) == kTypeNumber;
  }
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_is_promise(
  napi_env env,
  napi_value value,
  bool *is_promise
) {
  if (!env || !is_promise) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *is_promise = vtype((ant_value_t)value) == kTypePromise;
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_instanceof(
  napi_env env,
  napi_value object,
  napi_value constructor,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_value_t r = do_instanceof(nenv->js, (ant_value_t)object, (ant_value_t)constructor);
  if (is_err(r) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, r);
  *result = js_truthy(nenv->js, r);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_strict_equals(
  napi_env env,
  napi_value lhs,
  napi_value rhs,
  bool *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  *result = strict_eq_values(nenv->js, (ant_value_t)lhs, (ant_value_t)rhs);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_bool(
  napi_env env,
  napi_value value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  bool truthy = js_truthy(nenv->js, (ant_value_t)value);
  if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
  *result = NAPI_RETURN(nenv, js_bool(truthy));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_number(
  napi_env env,
  napi_value value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  double num = js_to_number(nenv->js, (ant_value_t)value);
  if (nenv->js->thrown_exists) return napi_check_pending_from_result(env, js_mkundef());
  *result = NAPI_RETURN(nenv, js_mknum(num));
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_object(
  napi_env env,
  napi_value value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");

  if (is_object_type((ant_value_t)value)) {
    *result = value;
    return napi_set_last(env, napi_ok, NULL);
  }

  ant_value_t obj_ctor = js_get(nenv->js, js_glob(nenv->js), "Object");
  if (is_err(obj_ctor) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, obj_ctor);
  if (!is_callable(obj_ctor)) return napi_set_last(env, napi_generic_failure, "Object constructor missing");
  ant_value_t arg = (ant_value_t)value;
  ant_value_t out = sv_vm_call(nenv->js->vm, nenv->js, obj_ctor, js_mkundef(), &arg, 1, NULL, false);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_coerce_to_string(
  napi_env env,
  napi_value value,
  napi_value *result
) {
  ant_napi_env_t *nenv = (ant_napi_env_t *)env;
  if (!nenv || !nenv->js || !result) return napi_set_last(env, napi_invalid_arg, "invalid argument");
  ant_value_t out = coerce_to_str(nenv->js, (ant_value_t)value);
  if (is_err(out) || nenv->js->thrown_exists) return napi_check_pending_from_result(env, out);
  *result = NAPI_RETURN(nenv, out);
  return napi_set_last(env, napi_ok, NULL);
}
