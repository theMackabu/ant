#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "index.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "native_getter.h"
#include "buffer_internal.h"

DataViewData *buffer_get_dataview_data(ant_value_t value) {
  if (!is_object_type(value)) return NULL;
  return (DataViewData *)js_get_native(value, BUFFER_DATAVIEW_NATIVE_TAG);
}

static void dataview_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  DataViewData *dv_data = (DataViewData *)js_get_native(value, BUFFER_DATAVIEW_NATIVE_TAG);
  if (!dv_data) return;
  js_clear_native(value, BUFFER_DATAVIEW_NATIVE_TAG);

  buffer_view_release(js, dv_data->backing, dv_data);
  ta_meta_free(js, dv_data, sizeof(*dv_data));
}

bool buffer_is_dataview(ant_value_t obj) {
  return js_check_brand(obj, BRAND_DATAVIEW);
}

void dataview_view_refresh(DataViewData *dv) {
  ArrayBufferData *backing = dv->backing;
  size_t total = buffer_byte_length(backing);
  size_t room = dv->byte_offset <= total ? total - dv->byte_offset : 0;
  bool fits = dv->byte_offset <= total && (dv->length_tracking || dv->base_length <= room);

  dv->buffer = fits ? backing : &buffer_out_of_bounds;
  dv->byte_length = fits ? (dv->length_tracking ? room : dv->base_length) : 0;
}

static __attribute__((noinline, cold)) void dataview_view_register(ant_t *js, DataViewData *dv) {
  buffer_views_add(js, dv->backing, dv, true);
  dataview_view_refresh(dv);
}

static inline __attribute__((always_inline)) void dataview_view_attach(
  ant_t *js, DataViewData *dv, ArrayBufferData *buffer,
  size_t byte_offset, size_t byte_length, bool tracking
) {
  dv->backing = buffer;
  dv->buffer = buffer;
  dv->byte_offset = byte_offset;
  dv->base_length = byte_length;
  dv->length_tracking = tracking && buffer->resizable;
  dv->byte_length = byte_length;
  buffer->ref_count++;

  if (__builtin_expect(buffer->resizable, 0)) dataview_view_register(js, dv);
}

ant_value_t create_dataview_with_buffer(
  ant_t *js, ArrayBufferData *buffer,
  size_t byte_offset, size_t byte_length,
  ant_value_t arraybuffer_obj, bool tracking
) {
  DataViewData *dv_data = ta_meta_alloc(js, sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");

  dataview_view_attach(js, dv_data, buffer, byte_offset, byte_length, tracking);

  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js->builtins.dataview_proto;
  if (is_special_object(proto)) js_set_proto_init(obj, proto);

  js_set_native(obj, dv_data, BUFFER_DATAVIEW_NATIVE_TAG);
  js_set_slot(obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_DATAVIEW));
  js_set_slot_wb(js, obj, SLOT_DATA, arraybuffer_obj);
  js_set_finalizer(obj, dataview_finalize);

  return obj;
}

static ant_value_t js_dataview_constructor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "DataView constructor requires 'new'");
  if (nargs < 1) return js_mkerr(js, "DataView requires an ArrayBuffer");
  
  ArrayBufferData *buffer = buffer_get_arraybuffer_data(args[0]);
  if (!buffer) return js_mkerr(js, "First argument must be an ArrayBuffer");
  
  size_t byte_offset = 0;
  size_t byte_length = buffer->length;
  
  if (nargs > 1) {
    ant_value_t result = js_to_index_fast(js, args[1], &byte_offset);
    if (is_err(result)) return result;
  }
  
  if (buffer->is_detached)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot create a view of a detached ArrayBuffer");
  
  size_t buffer_length = buffer->length;
  if (byte_offset > buffer_length)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Start offset is outside the bounds of the buffer");
  
  bool tracking = buffer->resizable;
  if (nargs > 2 && vtype(args[2]) != kTypeUndefined) {
    ant_value_t result = js_to_index_fast(js, args[2], &byte_length);
    if (is_err(result)) return result;
    if (byte_length > buffer_length - byte_offset)
      return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid DataView length");
    tracking = false;
  } else byte_length = buffer_length - byte_offset;

  if (buffer->is_detached)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot create a view of a detached ArrayBuffer");

  DataViewData *dv_data = ta_meta_alloc(js, sizeof(DataViewData));
  if (!dv_data) return js_mkerr(js, "Failed to allocate DataView");

  dataview_view_attach(js, dv_data, buffer, byte_offset, byte_length, tracking);
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.dataview_proto, call_new_target);
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  
  js_set_native(obj, dv_data, BUFFER_DATAVIEW_NATIVE_TAG);
  js_set_slot(obj, SLOT_BRAND, Ant_Value_FromNumber(BRAND_DATAVIEW));
  js_set_slot_wb(js, obj, SLOT_DATA, args[0]);
  js_set_finalizer(obj, dataview_finalize);
  
  return obj;
}

static __attribute__((noinline)) ant_value_t dataview_receiver_error(ant_t *js, const char *method) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Method DataView.prototype.%s called on incompatible receiver", method);
}

static __attribute__((noinline)) ant_value_t dataview_range_error(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_RANGE, "Offset is outside the bounds of the DataView");
}

static __attribute__((noinline)) ant_value_t dataview_access_error(ant_t *js, DataViewData *dv, const char *method) {
  if (!Ant_Buffer_ViewIsLive(dv)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot perform DataView.prototype.%s on a detached or out-of-bounds ArrayBuffer", method
  );
  return dataview_range_error(js);
}

static inline __attribute__((always_inline)) uint8_t *dataview_access(DataViewData *dv, size_t index, size_t width) {
  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(dv, &data, &len)) return NULL;
  if (index > len || width > len - index) return NULL;
  return data + index;
}

static inline __attribute__((always_inline)) bool dataview_fast_ptr(DataViewData *dv, ant_value_t offset, size_t width, uint8_t **out) {
  double index = Ant_Value_AsNumberUnchecked(offset);
  if (!Ant_Buffer_IsLive(dv->buffer) || !(index >= 0.0 && index <= (double)dv->byte_length - (double)width)) return false;
  *out = dv->buffer->data + dv->byte_offset + (size_t)index;
  return true;
}

static inline __attribute__((always_inline)) ant_value_t dataview_read_value(
  ant_t *js, const uint8_t *ptr, buffer_num_kind_t kind, size_t width, bool little_endian
) {
  return buffer_num_decode(js, buffer_load_bytes(ptr, width, little_endian), kind, width);
}

static __attribute__((noinline)) ant_value_t dataview_get_slow(
  ant_native_params_t, DataViewData *dv, const char *method, buffer_num_kind_t kind, size_t width
) {
  size_t index;
  ant_value_t index_result = js_to_index_fast(js, nargs > 0 ? args[0] : Ant_Value_Undefined, &index);
  if (is_err(index_result)) return index_result;
  bool little_endian = nargs > 1 && js_truthy(js, args[1]);
  
  uint8_t *ptr = dataview_access(dv, index, width);
  if (!ptr) return dataview_access_error(js, dv, method);
  return dataview_read_value(js, ptr, kind, width, little_endian);
}

static inline __attribute__((always_inline)) ant_value_t dataview_get(
  ant_native_params_t, ant_value_t call_this, const char *method, buffer_num_kind_t kind, size_t width
) {
  DataViewData *dv = buffer_get_dataview_data(call_this);
  if (!dv) return dataview_receiver_error(js, method);
  
  uint8_t *ptr;
  if (__builtin_expect(!dataview_fast_ptr(dv, nargs > 0 ? args[0] : Ant_Value_Undefined, width, &ptr), 0))
    return dataview_get_slow(js, args, nargs, dv, method, kind, width);
  return dataview_read_value(js, ptr, kind, width, nargs > 1 && js_truthy(js, args[1]));
}

static inline __attribute__((always_inline)) uint64_t dataview_number_bits(double num, buffer_num_kind_t kind, size_t width) {
  return kind == BUFFER_NUM_FLOAT ? buffer_float_bits(num, width) : (uint64_t)js_to_uint32(num);
}

static __attribute__((noinline)) ant_value_t dataview_set_slow(
  ant_native_params_t, DataViewData *dv, const char *method, buffer_num_kind_t kind, size_t width
) {
  size_t index;
  ant_value_t index_result = js_to_index_fast(js, nargs > 0 ? args[0] : Ant_Value_Undefined, &index);
  if (is_err(index_result)) return index_result;
  
  ant_value_t value = nargs > 1 ? args[1] : Ant_Value_Undefined;
  uint64_t bits = 0;
  
  if (kind == BUFFER_NUM_BIGINT || kind == BUFFER_NUM_BIGUINT) {
    ant_value_t bigint = bigint_to_bigint(js, value);
    if (is_err(bigint)) return bigint;
    bool ok = kind == BUFFER_NUM_BIGINT
      ? bigint_to_int64_wrapping(js, bigint, (int64_t *)&bits)
      : bigint_to_uint64_wrapping(js, bigint, &bits);
    if (!ok) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert to BigInt");
  } else {
    ant_value_t number = buffer_to_number_value(js, value);
    if (is_err(number)) return number;
    bits = dataview_number_bits(Ant_Value_AsNumberUnchecked(number), kind, width);
  }
  
  bool little_endian = nargs > 2 && js_truthy(js, args[2]);
  uint8_t *ptr = dataview_access(dv, index, width);
  if (!ptr) return dataview_access_error(js, dv, method);
  
  buffer_store_bytes(ptr, width, little_endian, bits);
  return Ant_Value_Undefined;
}

static inline __attribute__((always_inline)) ant_value_t dataview_set(
  ant_native_params_t, ant_value_t call_this, const char *method, buffer_num_kind_t kind, size_t width
) {
  DataViewData *dv = buffer_get_dataview_data(call_this);
  if (!dv) return dataview_receiver_error(js, method);
  
  bool numeric = kind != BUFFER_NUM_BIGINT && kind != BUFFER_NUM_BIGUINT;
  ant_value_t value = nargs > 1 ? args[1] : Ant_Value_Undefined;
  
  uint8_t *ptr;
  if (__builtin_expect(!numeric || vtype(value) != kTypeNumber || !dataview_fast_ptr(dv, nargs > 0 ? args[0] : Ant_Value_Undefined, width, &ptr), 0))
    return dataview_set_slow(js, args, nargs, dv, method, kind, width);
  
  buffer_store_bytes(ptr, width, nargs > 2 && js_truthy(js, args[2]), dataview_number_bits(Ant_Value_AsNumberUnchecked(value), kind, width));
  return Ant_Value_Undefined;
}

#define DATAVIEW_METHOD(type, kind, width)                                                  \
  static ant_value_t js_dataview_get##type(ant_params_t) {                                  \
    return dataview_get(js, args, nargs, call_this, "get" #type, BUFFER_NUM_##kind, width); \
  }                                                                                         \
  static ant_value_t js_dataview_set##type(ant_params_t) {                                  \
    return dataview_set(js, args, nargs, call_this, "set" #type, BUFFER_NUM_##kind, width); \
  }
#include "buffer/dataview_methods.h"

static const ant_cfunc_meta_t dataview_methods[] = {
  #define DATAVIEW_METHOD(type, kind, width)      \
    { js_dataview_get##type, "get" #type, 1, 0 }, \
    { js_dataview_set##type, "set" #type, 2, 0 },
  #include "buffer/dataview_methods.h"
};

void dataview_initialize(ant_t *js) {
  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t dataview_ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t dataview_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(dataview_proto, object_proto);
  js->builtins.dataview_proto = dataview_proto;
  
  js_define_methods(js, dataview_proto, dataview_methods, countof(dataview_methods), ANT_PROP_ATTR_METHOD);
  
  js_set_getter_desc(js, dataview_proto, "buffer", 6, js_mkfun_meta(&dataview_buffer_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, dataview_proto, "byteLength", 10, js_mkfun_meta(&dataview_byte_length_getter.meta), JS_DESC_C);
  js_set_getter_desc(js, dataview_proto, "byteOffset", 10, js_mkfun_meta(&dataview_byte_offset_getter.meta), JS_DESC_C);
  js_set_tostringtag(js, dataview_proto, js_mkstr(js, "DataView", 8));

  js_set_slot(dataview_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(js_dataview_constructor, 1));
  mkprop_exact_attrs(js, dataview_ctor_obj, Ant_String_FromLiteral(js, "prototype"), dataview_proto, 0);
  mkprop_exact_attrs(js, dataview_ctor_obj, Ant_String_FromLiteral(js, "name"), Ant_String_FromLiteral(js, "DataView"), ANT_PROP_ATTR_CONFIGURABLE);
  
  ant_value_t dataview_ctor = js_obj_to_func(js, dataview_ctor_obj);
  defmethod(js, dataview_proto, "constructor", 11, dataview_ctor);
  js_set_global_builtin(js, "DataView", dataview_ctor);
}
