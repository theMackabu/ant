#include "napi_internal.h"

static const TypedArrayType typedarray_kinds[] = {
  [napi_int8_array]          = TYPED_ARRAY_INT8,
  [napi_uint8_array]         = TYPED_ARRAY_UINT8,
  [napi_uint8_clamped_array] = TYPED_ARRAY_UINT8_CLAMPED,
  [napi_int16_array]         = TYPED_ARRAY_INT16,
  [napi_uint16_array]        = TYPED_ARRAY_UINT16,
  [napi_int32_array]         = TYPED_ARRAY_INT32,
  [napi_uint32_array]        = TYPED_ARRAY_UINT32,
  [napi_float32_array]       = TYPED_ARRAY_FLOAT32,
  [napi_float64_array]       = TYPED_ARRAY_FLOAT64,
  [napi_bigint64_array]      = TYPED_ARRAY_BIGINT64,
  [napi_biguint64_array]     = TYPED_ARRAY_BIGUINT64,
};

static constexpr unsigned TYPEDARRAY_KIND_COUNT = sizeof(typedarray_kinds) / sizeof(typedarray_kinds[0]);

static napi_typedarray_type TypedArray_KindFromAnt(TypedArrayType type) {
  for (unsigned i = 0; i < TYPEDARRAY_KIND_COUNT; i++)
    if (typedarray_kinds[i] == type) return (napi_typedarray_type)i;
  return napi_uint8_array;
}

static TypedArrayData *TypedArray_Data(napi_value value) {
  TypedArrayData *view = buffer_get_typedarray_data(Napi_Decode(value));
  return Ant_Buffer_ViewIsLive(view) ? view : NULL;
}

static DataViewData *DataView_Data(napi_value value) {
  ant_value_t view = Napi_Decode(value);
  return is_object_type(view) ? buffer_get_dataview_data(view) : NULL;
}

static bool Range_Fits(const ArrayBufferData *buffer, size_t byte_offset, size_t byte_length) {
  return byte_offset <= buffer->length && byte_length <= buffer->length - byte_offset;
}

static napi_status Range_Throw(napi_env env, const char *code, const char *message) {
  Ant_Napi_ThrowMessage(env, JS_ERR_RANGE, code, message);
  return Ant_Napi_CheckPending(env);
}

static napi_status Result_Store(napi_env env, ant_value_t value, napi_value *result) {
  if (Napi_Failed(env, value)) return Ant_Napi_CapturePending(env, value);
  *result = Ant_Napi_Pin(env, value);
  return Napi_Ok(env);
}

static napi_status Buffer_CreateView(
  napi_env env, ArrayBufferData *buffer, ant_value_t arraybuffer,
  size_t byte_offset, size_t byte_length, napi_value *result
) {
  ant_t *js = env->js;
  ant_value_t view = is_object_type(arraybuffer)
    ? create_typed_array_with_buffer(
        js, TYPED_ARRAY_UINT8, buffer, byte_offset, byte_length,
        js->builtins.buffer_proto, arraybuffer, false
      )
    : create_typed_array(js, TYPED_ARRAY_UINT8, buffer, byte_offset, byte_length, js->builtins.buffer_proto);

  return Result_Store(env, view, result);
}

static napi_status External_Create(
  napi_env env, void *bytes, size_t byte_length, bool shared,
  napi_external_buffer_finalizer_t finalizer, void *finalize_hint,
  ArrayBufferData **buffer_out, ant_value_t *object_out
) {
  napi_external_buffer_t *entry = calloc(1, sizeof(*entry));
  ArrayBufferData *buffer = entry ? create_external_array_buffer_data(bytes, byte_length) : NULL;
  if (!buffer) {
    free(entry);
    return Napi_OutOfMemory(env);
  }

  buffer->is_shared = shared;
  buffer->ref_count = 1;

  ant_value_t object = shared
    ? create_shared_arraybuffer_obj(env->js, buffer)
    : create_arraybuffer_obj(env->js, buffer);

  if (Napi_Failed(env, object)) {
    free_array_buffer_data(env->js, buffer);
    free(entry);
    return Ant_Napi_CapturePending(env, object);
  }

  entry->data = buffer;
  entry->bytes = bytes;
  entry->finalizer = finalizer;
  entry->finalize_hint = finalize_hint;
  entry->next = env->external_buffers;
  env->external_buffers = entry;

  Ant_Napi_Pin(env, object);
  *buffer_out = buffer;
  *object_out = object;
  return napi_ok;
}

static void External_Finalize(napi_env env, const napi_external_buffer_t *entry) {
  if (entry->data->is_shared) {
    if (entry->finalizer.without_env) entry->finalizer.without_env(entry->bytes, entry->finalize_hint);
  } else if (entry->finalizer.with_env) entry->finalizer.with_env(env, entry->bytes, entry->finalize_hint);
}

void Ant_Napi_SweepExternalBuffers(napi_env env, bool teardown) {
  napi_external_buffer_t **link = &env->external_buffers;

  while (*link) {
    napi_external_buffer_t *entry = *link;
    bool unreferenced = atomic_load(&entry->data->ref_count) == 1;

    if (!unreferenced && !teardown) {
      link = &entry->next;
      continue;
    }

    *link = entry->next;
    if (unreferenced) External_Finalize(env, entry);
    free_array_buffer_data(env->js, entry->data);
    free(entry);
  }
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_arraybuffer(napi_env env, size_t byte_length, void **data, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t arraybuffer = create_arraybuffer_copy(env->js, NULL, byte_length);
  if (Napi_Failed(env, arraybuffer)) return Ant_Napi_CapturePending(env, arraybuffer);

  if (data) *data = buffer_get_arraybuffer_data(arraybuffer)->data;
  *result = Ant_Napi_Pin(env, arraybuffer);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_external_arraybuffer(
  napi_env env, void *external_data, size_t byte_length, node_api_basic_finalize finalize_cb,
  void *finalize_hint, napi_value *result
) {
  if (!env || !env->js || !result || (!external_data && byte_length != 0)) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = NULL;
  ant_value_t arraybuffer = Ant_Value_Undefined;
  napi_external_buffer_finalizer_t finalizer = { .with_env = finalize_cb };

  napi_status status = External_Create(
    env, external_data, byte_length, false,
    finalizer, finalize_hint, &buffer, &arraybuffer
  );
  if (status != napi_ok) return status;

  *result = Napi_Encode(arraybuffer);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_arraybuffer_info(napi_env env, napi_value arraybuffer, void **data, size_t *byte_length) {
  if (!env || !env->js) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = Napi_ArrayBufferData(arraybuffer);
  if (!buffer) return Ant_Napi_SetLastError(env, napi_arraybuffer_expected, NULL);

  if (data) *data = buffer->data;
  if (byte_length) *byte_length = buffer->length;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_arraybuffer(napi_env env, napi_value value, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = Napi_ArrayBufferData(value);
  *result = buffer && !buffer->is_shared;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_detach_arraybuffer(napi_env env, napi_value arraybuffer) {
  if (!env || !env->js) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = Napi_ArrayBufferData(arraybuffer);
  if (!buffer || buffer->is_shared) return Ant_Napi_SetLastError(env, napi_arraybuffer_expected, NULL);
  if (buffer->is_detached) return Napi_Ok(env);
  if (!buffer_can_transfer(buffer)) return Ant_Napi_SetLastError(env, napi_detachable_arraybuffer_expected, NULL);

  buffer_detach(env->js, buffer);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_detached_arraybuffer(napi_env env, napi_value value, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = Napi_ArrayBufferData(value);
  *result = buffer && buffer->is_detached;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_sharedarraybuffer(napi_env env, size_t byte_length, void **data, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t shared = create_shared_arraybuffer_copy(env->js, NULL, byte_length);
  if (Napi_Failed(env, shared)) return Ant_Napi_CapturePending(env, shared);

  if (data) *data = buffer_get_arraybuffer_data(shared)->data;
  *result = Ant_Napi_Pin(env, shared);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_external_sharedarraybuffer(
  napi_env env, void *external_data, size_t byte_length, node_api_noenv_finalize finalize_cb,
  void *finalize_hint, napi_value *result
) {
  if (!env || !env->js || !result || (!external_data && byte_length != 0)) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = NULL;
  ant_value_t shared = Ant_Value_Undefined;
  napi_external_buffer_finalizer_t finalizer = { .without_env = finalize_cb };

  napi_status status = External_Create(
    env, external_data, byte_length, true,
    finalizer, finalize_hint, &buffer, &shared
  );
  if (status != napi_ok) return status;

  *result = Napi_Encode(shared);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_is_sharedarraybuffer(napi_env env, napi_value value, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = Napi_ArrayBufferData(value);
  *result = buffer && buffer->is_shared;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_buffer(napi_env env, size_t length, void **data, napi_value *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = create_array_buffer_data(env->js, length);
  if (!buffer) return Napi_OutOfMemory(env);

  napi_status status = Buffer_CreateView(env, buffer, Ant_Value_Undefined, 0, length, result);
  if (status == napi_ok && data) *data = buffer->data;
  return status;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_buffer_copy(napi_env env, size_t length, const void *data, void **result_data, napi_value *result) {
  void *bytes = NULL;
  napi_status status = napi_create_buffer(env, length, &bytes, result);
  if (status != napi_ok) return status;

  if (length > 0 && data) memcpy(bytes, data, length);
  if (result_data) *result_data = bytes;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_external_buffer(
  napi_env env, size_t length, void *data, node_api_basic_finalize finalize_cb,
  void *finalize_hint, napi_value *result
) {
  if (!env || !env->js || !result || (!data && length != 0)) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = NULL;
  ant_value_t arraybuffer = Ant_Value_Undefined;
  napi_external_buffer_finalizer_t finalizer = { .with_env = finalize_cb };

  napi_status status = External_Create(env, data, length, false, finalizer, finalize_hint, &buffer, &arraybuffer);
  if (status != napi_ok) return status;

  return Buffer_CreateView(env, buffer, arraybuffer, 0, length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
node_api_create_buffer_from_arraybuffer(
  napi_env env, napi_value arraybuffer, size_t byte_offset, size_t byte_length,
  napi_value *result
) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = Napi_ArrayBufferData(arraybuffer);
  if (!buffer || buffer->is_detached) return Ant_Napi_SetLastError(env, napi_arraybuffer_expected, NULL);
  if (!Range_Fits(buffer, byte_offset, byte_length))
    return Range_Throw(env, "ERR_OUT_OF_RANGE", "The byte offset + length is out of range");

  return Buffer_CreateView(env, buffer, Napi_Decode(arraybuffer), byte_offset, byte_length, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_buffer_info(napi_env env, napi_value value, void **data, size_t *length) {
  if (!env || !env->js) return Napi_InvalidArg(env);

  TypedArrayData *view = TypedArray_Data(value);
  if (!view) return Ant_Napi_SetLastError(env, napi_invalid_arg, "not a buffer");

  uint8_t *bytes; size_t size;
  Ant_Buffer_GetViewBytes(view, &bytes, &size);

  if (data) *data = bytes;
  if (length) *length = size;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_buffer(napi_env env, napi_value value, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ant_value_t buffer_proto = env->js->builtins.buffer_proto;
  *result = TypedArray_Data(value)
    && is_object_type(buffer_proto)
    && proto_chain_contains(env->js, Napi_Decode(value), buffer_proto);
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_typedarray(
  napi_env env, napi_typedarray_type type, size_t length, napi_value arraybuffer,
  size_t byte_offset, napi_value *result
) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  if ((unsigned)type >= TYPEDARRAY_KIND_COUNT) return Ant_Napi_SetLastError(env, napi_invalid_arg, "invalid typedarray type");

  ArrayBufferData *buffer = Napi_ArrayBufferData(arraybuffer);
  if (!buffer || buffer->is_detached) return Ant_Napi_SetLastError(env, napi_arraybuffer_expected, NULL);

  TypedArrayType kind = typedarray_kinds[type];
  size_t element_size = get_element_size(kind);

  if (byte_offset % element_size != 0)
    return Range_Throw(env, "ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT", "start offset must be a multiple of the element size");
  if (byte_offset > buffer->length || length > (buffer->length - byte_offset) / element_size)
    return Range_Throw(env, "ERR_NAPI_INVALID_TYPEDARRAY_LENGTH", "Invalid typed array length");

  ant_value_t view = create_typed_array_with_buffer(
    env->js, kind, buffer, byte_offset, length,
    env->js->builtins.typedarray_protos[kind], Napi_Decode(arraybuffer), false
  );

  return Result_Store(env, view, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_typedarray_info(
  napi_env env, napi_value typedarray, napi_typedarray_type *type, size_t *length, void **data,
  napi_value *arraybuffer, size_t *byte_offset
) {
  if (!env || !env->js) return Napi_InvalidArg(env);

  TypedArrayData *view = TypedArray_Data(typedarray);
  if (!view) return Ant_Napi_SetLastError(env, napi_invalid_arg, "typedarray expected");

  uint8_t *bytes; size_t size;
  Ant_Buffer_GetViewBytes(view, &bytes, &size);

  if (arraybuffer) {
    ant_value_t buffer = js_get(env->js, Napi_Decode(typedarray), "buffer");
    if (Napi_Failed(env, buffer)) return Ant_Napi_CapturePending(env, buffer);
    *arraybuffer = Ant_Napi_Pin(env, buffer);
  }

  if (type) *type = TypedArray_KindFromAnt(view->type);
  if (length) *length = view->length;
  if (data) *data = bytes;
  if (byte_offset) *byte_offset = view->byte_offset;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_typedarray(napi_env env, napi_value value, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  *result = TypedArray_Data(value) != NULL;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_dataview(
  napi_env env, size_t length, napi_value arraybuffer, size_t byte_offset, napi_value *result
) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);

  ArrayBufferData *buffer = Napi_ArrayBufferData(arraybuffer);
  if (!buffer || buffer->is_detached) return Ant_Napi_SetLastError(env, napi_arraybuffer_expected, NULL);

  if (!Range_Fits(buffer, byte_offset, length)) return Range_Throw(
    env, "ERR_NAPI_INVALID_DATAVIEW_ARGS",
    "byte_offset + byte_length should be less than or equal to the size in bytes of the array passed in"
  );

  ant_value_t view = create_dataview_with_buffer(env->js, buffer, byte_offset, length, Napi_Decode(arraybuffer), false);
  return Result_Store(env, view, result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_dataview_info(
  napi_env env, napi_value dataview, size_t *bytelength, void **data, napi_value *arraybuffer,
  size_t *byte_offset
) {
  if (!env || !env->js) return Napi_InvalidArg(env);

  DataViewData *view = DataView_Data(dataview);
  if (!view) return Ant_Napi_SetLastError(env, napi_invalid_arg, "dataview expected");

  uint8_t *bytes; size_t size;
  Ant_Buffer_GetViewBytes(view, &bytes, &size);

  if (bytelength) *bytelength = size;
  if (data) *data = bytes;
  if (arraybuffer) *arraybuffer = Ant_Napi_Pin(env, js_get_slot(Napi_Decode(dataview), SLOT_DATA));
  if (byte_offset) *byte_offset = view->byte_offset;
  return Napi_Ok(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_is_dataview(napi_env env, napi_value value, bool *result) {
  if (!env || !env->js || !result) return Napi_InvalidArg(env);
  *result = DataView_Data(value) != NULL;
  return Napi_Ok(env);
}
