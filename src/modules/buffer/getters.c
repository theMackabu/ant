#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "protectors.h"
#include "modules/buffer.h"
#include "buffer_internal.h"

static ant_value_t js_arraybuffer_byteLength_getter(ant_params_t) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!data || data->is_detached) return Ant_Value_FromNumber(0);
  return Ant_Value_FromNumber((double)data->length);
}

static __attribute__((noinline, cold)) ant_value_t typedarray_receiver_error(ant_t *js, const char *getter) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Method get TypedArray.prototype.%s called on incompatible receiver", getter);
}

static ant_value_t js_typedarray_length_getter(ant_params_t) {
  TypedArrayData *data = buffer_get_typedarray_data(call_this);
  if (!data) return typedarray_receiver_error(js, "length");
  return Ant_Value_FromNumber(typedarray_view_live(data) ? (double)data->length : 0);
}

bool buffer_typedarray_length_fast(ant_t *js, ant_object_t *ptr, double *out) {
  if (ptr->native_tag != BUFFER_TYPEDARRAY_NATIVE_TAG || !ant_protector_intact(js, ANT_PROTECTOR_TYPED_ARRAY_LENGTH)) return false;

  TypedArrayData *ta = (TypedArrayData *)ptr->native_ptr;
  if (!ta || (unsigned)ta->type >= TYPED_ARRAY_COUNT) return false;

  ant_value_t proto = ant_object_proto(ptr);
  if (proto != js->builtins.typedarray_protos[ta->type] && !(ta->type == TYPED_ARRAY_UINT8 && proto == js->builtins.buffer_proto)) return false;

  *out = typedarray_view_live(ta) ? (double)ta->length : 0;
  return true;
}

bool buffer_fast_length_getter(ant_value_t getter, ant_value_t obj, double *out) {
  if (vtype(getter) != kTypeBuiltin || js_as_cfunc(getter) != js_typedarray_length_getter) return false;
  TypedArrayData *data = buffer_get_typedarray_data(obj);
  if (!data) return false;
  *out = typedarray_view_live(data) ? (double)data->length : 0;
  return true;
}

static ant_value_t js_typedarray_byteLength_getter(ant_params_t) {
  TypedArrayData *data = buffer_get_typedarray_data(call_this);
  if (!data) return typedarray_receiver_error(js, "byteLength");
  return Ant_Value_FromNumber(typedarray_view_live(data) ? (double)data->byte_length : 0);
}

static ant_value_t js_typedarray_byteOffset_getter(ant_params_t) {
  TypedArrayData *data = buffer_get_typedarray_data(call_this);
  if (!data) return typedarray_receiver_error(js, "byteOffset");
  return Ant_Value_FromNumber(typedarray_view_live(data) ? (double)data->byte_offset : 0);
}

static ant_value_t js_dataview_buffer_getter(ant_params_t) {
  DataViewData *dv = buffer_get_dataview_data(call_this);
  if (!dv) return js_mkerr_typed(js, JS_ERR_TYPE, "Not a DataView");
  return js_get_slot(call_this, SLOT_DATA);
}

static ant_value_t js_typedarray_buffer_getter(ant_params_t) {
  if (!buffer_get_typedarray_data(call_this)) return typedarray_receiver_error(js, "buffer");
  return js_get_slot(call_this, SLOT_DATA);
}

static ant_value_t dataview_live_view(ant_t *js, ant_value_t call_this, const char *getter, DataViewData **out) {
  *out = buffer_get_dataview_data(call_this);
  if (!*out) return js_mkerr_typed(js, JS_ERR_TYPE, "Not a DataView");
  if (Ant_Buffer_ViewIsLive(*out)) return Ant_Value_Undefined;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform get DataView.prototype.%s on a detached or out-of-bounds ArrayBuffer", getter);
}

static ant_value_t js_dataview_byteLength_getter(ant_params_t) {
  DataViewData *dv;
  ant_value_t error = dataview_live_view(js, call_this, "byteLength", &dv);
  return is_err(error) ? error : Ant_Value_FromNumber((double)dv->byte_length);
}

static ant_value_t js_dataview_byteOffset_getter(ant_params_t) {
  DataViewData *dv;
  ant_value_t error = dataview_live_view(js, call_this, "byteOffset", &dv);
  return is_err(error) ? error : Ant_Value_FromNumber((double)dv->byte_offset);
}

#define BUFFER_GETTER_REF_REF_SLOW(owner) offsetof(owner, buffer)
#define BUFFER_GETTER_REF_SELF(owner) 0

#define BUFFER_SIZE_GETTER(name, fn, tag, owner, mode, flag, field)                                \
  const ant_native_getter_t name = {                                                               \
    .meta = { fn, NULL, 0, CFUNC_NATIVE_GETTER }, .native_tag = BUFFER_##tag##_NATIVE_TAG,         \
    .kind = ANT_NATIVE_GETTER_SIZE, .detach = ANT_NATIVE_DETACH_##mode,                            \
    .ref_offset = BUFFER_GETTER_REF_##mode(owner), .flag_offset = offsetof(ArrayBufferData, flag), \
    .field_offset = offsetof(owner, field) };

#define BUFFER_SLOT_GETTER(name, fn, tag)                                                  \
  const ant_native_getter_t name = {                                                       \
    .meta = { fn, NULL, 0, CFUNC_NATIVE_GETTER }, .native_tag = BUFFER_##tag##_NATIVE_TAG, \
    .kind = ANT_NATIVE_GETTER_SLOT_DATA };

#include "buffer/native_getters.h"

#undef BUFFER_GETTER_REF_REF_SLOW
#undef BUFFER_GETTER_REF_SELF
