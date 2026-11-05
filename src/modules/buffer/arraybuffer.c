#include <stdlib.h>
#include <string.h>
#include <uthash.h>

#include "ant.h"
#include "gc.h"
#include "cfunc.h"
#include "ptr.h"
#include "index.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "modules/buffer.h"
#include "native_getter.h"
#include "modules/symbol.h"
#include "buffer_internal.h"

void *ta_meta_alloc(ant_t *js, size_t size) {
  void *ptr = calloc(1, size);
  if (!ptr) return NULL;
  js->buffers.meta_bytes += size;
  return ptr;
}

void ta_meta_free(ant_t *js, void *ptr, size_t size) {
  if (!ptr) return;
  js->buffers.meta_bytes = js->buffers.meta_bytes >= size ? js->buffers.meta_bytes - size : 0;
  free(ptr);
}

ArrayBufferData *buffer_get_arraybuffer_data(ant_value_t value) {
  if (!is_object_type(value)) return NULL;
  ArrayBufferData *ab = (ArrayBufferData *)js_get_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  return ab && !buffer_is_dataview(value) ? ab : NULL;
}

typedef struct {
  void *view;
  bool dataview;
} buffer_view_ref_t;

typedef struct buffer_view_set {
  ArrayBufferData *buffer;
  buffer_view_ref_t *items;
  uint32_t count;
  uint32_t cap;
  UT_hash_handle hh;
} buffer_view_set_t;

void buffer_views_add(ant_t *js, ArrayBufferData *buffer, void *view, bool dataview) {
  buffer_view_set_t *set = NULL;
  HASH_FIND_PTR(js->buffers.views, &buffer, set);
  
  if (!set) {
    set = calloc(1, sizeof(*set));
    if (!set) return;
    set->buffer = buffer;
    HASH_ADD_PTR(js->buffers.views, buffer, set);
  }

  if (set->count == set->cap) {
    uint32_t cap = set->cap ? set->cap * 2 : 4;
    buffer_view_ref_t *items = realloc(set->items, cap * sizeof(*items));
    if (!items) return;
    set->items = items;
    set->cap = cap;
  }

  set->items[set->count++] = (buffer_view_ref_t){ view, dataview };
}

void buffer_views_remove(ant_t *js, ArrayBufferData *buffer, void *view) {
  buffer_view_set_t *set = NULL;
  HASH_FIND_PTR(js->buffers.views, &buffer, set);
  if (!set) return;

  for (uint32_t i = 0; i < set->count; i++) if (set->items[i].view == view) {
    set->items[i] = set->items[--set->count];
    break;
  }

  if (set->count) return;
  HASH_DEL(js->buffers.views, set);
  free(set->items);
  free(set);
}

static void buffer_views_refresh(ant_t *js, ArrayBufferData *buffer) {
  buffer_view_set_t *set = NULL;
  HASH_FIND_PTR(js->buffers.views, &buffer, set);
  if (!set) return;

  for (uint32_t i = 0; i < set->count; i++) {
    if (set->items[i].dataview) dataview_view_refresh(set->items[i].view);
    else typedarray_view_refresh(set->items[i].view);
  }
}

static void buffer_views_free(ant_t *js) {
  buffer_view_set_t *set, *tmp;
  HASH_ITER(hh, js->buffers.views, set, tmp) {
    HASH_DEL(js->buffers.views, set);
    free(set->items);
    free(set);
  }
}

static void arraybuffer_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ArrayBufferData *data = (ArrayBufferData *)js_get_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  if (!data) return;
  js_clear_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  free_array_buffer_data(js, data);
}

bool buffer_is_binary_source(ant_value_t value) {
  if (buffer_get_typedarray_data(value)) return true;
  if (!is_object_type(value)) return false;
  return js_get_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG) != NULL || buffer_is_dataview(value);
}

bool buffer_source_get_bytes(ant_t *js, ant_value_t value, const uint8_t **out, size_t *len) {
  if (out) *out = NULL;
  if (len) *len = 0;

  TypedArrayData *ta = buffer_get_typedarray_data(value);
  if (ta) {
    Ant_Buffer_GetViewBytes(ta, out, len);
    return true;
  }

  if (!is_object_type(value)) return false;
  if (buffer_is_dataview(value)) {
    DataViewData *dv = buffer_get_dataview_data(value);
    Ant_Buffer_GetViewBytes(dv, out, len);
    return true;
  }

  ArrayBufferData *ab = (ArrayBufferData *)js_get_native(value, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  if (!ab) return false;
  
  if (!ab->is_detached) {
    *out = ab->data;
    *len = ab->length;
  }
  
  return true;
}

static inline size_t array_buffer_owned_bytes(const ArrayBufferData *data) {
  return data->storage == ARRAY_BUFFER_STORAGE_INLINE ? data->capacity : 0;
}

static void register_buffer(ant_t *js, ArrayBufferData *data) {
  if (js->buffers.count >= UINT32_MAX) return;
  
  if (js->buffers.count == js->buffers.cap) {
    size_t cap = js->buffers.cap ? (size_t)js->buffers.cap * 2 : 64;
    if (cap > UINT32_MAX) cap = UINT32_MAX;
    
    ArrayBufferData **entries = realloc(js->buffers.entries, cap * sizeof(*entries));
    if (!entries) return;
    
    js->buffers.entries = entries;
    js->buffers.cap = (uint32_t)cap;
  }
  
  js->buffers.entries[js->buffers.count++] = data;
  js->buffers.bytes += array_buffer_owned_bytes(data);
  data->registry_slot = js->buffers.count;
  
  gc_note_offheap_alloc(js, array_buffer_owned_bytes(data));
}

static void unregister_buffer(ant_t *js, ArrayBufferData *data) {
  if (data->registry_slot == 0) return;
  
  size_t idx = (size_t)data->registry_slot - 1;
  if (idx >= js->buffers.count || js->buffers.entries[idx] != data) return;
  
  ArrayBufferData *last = js->buffers.entries[--js->buffers.count];
  js->buffers.entries[idx] = last;
  last->registry_slot = (uint32_t)idx + 1;
  data->registry_slot = 0;
  js->buffers.bytes -= array_buffer_owned_bytes(data);
}

static ArrayBufferData *alloc_array_buffer_data(size_t length) {
  if (length > SIZE_MAX - sizeof(ArrayBufferData)) return NULL;
  ArrayBufferData *data = calloc(1, sizeof(ArrayBufferData) + length);
  if (!data) return NULL;
  
  data->data = (uint8_t *)(data + 1);
  data->length = length;
  data->capacity = length;
  data->ref_count = 1;
  data->storage = ARRAY_BUFFER_STORAGE_INLINE;
  return data;
}

ArrayBufferData *create_external_array_buffer_data(uint8_t *bytes, size_t length) {
  ArrayBufferData *data = calloc(1, sizeof(ArrayBufferData));
  if (!data) return NULL;
  
  data->data = bytes;
  data->length = length;
  data->capacity = length;
  return data;
}

ArrayBufferData *create_array_buffer_data(ant_t *js, size_t length) {
  ArrayBufferData *data = alloc_array_buffer_data(length);
  if (data) register_buffer(js, data);
  return data;
}

static ArrayBufferData *create_shared_array_buffer_data(size_t length) {
  ArrayBufferData *data = alloc_array_buffer_data(length);
  if (data) data->is_shared = 1;
  return data;
}

static ArrayBufferData *create_resizable_array_buffer_data(ant_t *js, size_t length, size_t max_length, bool shared) {
  ArrayBufferData *data = alloc_array_buffer_data(max_length);
  if (!data) return NULL;

  data->length = length;
  data->resizable = 1;
  data->is_shared = shared;
  data->views_slow = shared;
  if (!shared) register_buffer(js, data);

  return data;
}

void free_array_buffer_data(ant_t *js, ArrayBufferData *data) {
  if (!data || --data->ref_count > 0) return;
  ArrayBufferData *owner = data->owner;
  if (!data->is_shared) unregister_buffer(js, data);
  free(data);
  free_array_buffer_data(js, owner);
}

void buffer_detach(ant_t *js, ArrayBufferData *data) {
  ArrayBufferData *owner = data->owner;
  if (data->storage != ARRAY_BUFFER_STORAGE_INLINE) data->capacity = 0;
  if (owner) data->storage = ARRAY_BUFFER_STORAGE_INLINE;
  
  data->owner = NULL;
  data->data = NULL;
  data->length = 0;
  data->is_detached = 1;
  data->views_slow = 1;
  free_array_buffer_data(js, owner);
}

bool buffer_can_transfer(const ArrayBufferData *data) {
  return data && !data->is_shared && !data->is_detached && data->storage != ARRAY_BUFFER_STORAGE_EXTERNAL;
}

void buffer_transfer_storage(ant_t *js, ArrayBufferData *from, ArrayBufferData *to) {
  ArrayBufferData *owner = from->owner ? from->owner : from;
  owner->ref_count++;
  
  to->owner = owner;
  to->data = from->data;
  to->length = from->length;
  to->capacity = from->capacity;
  to->resizable = from->resizable;
  to->storage = ARRAY_BUFFER_STORAGE_BORROWED;
  
  buffer_detach(js, from);
}

ArrayBufferData *buffer_storage_holder_new(void) {
  ArrayBufferData *data = calloc(1, sizeof(*data));
  if (data) data->ref_count = 1;
  return data;
}

void buffer_move_storage(ant_t *js, ArrayBufferData *from, ArrayBufferData *to) {
  buffer_transfer_storage(js, from, to);
  unregister_buffer(js, to->owner);
}

static ant_value_t buffer_read_max_length(ant_t *js, ant_value_t options, size_t length, size_t *max_length, bool *resizable) {
  *resizable = false;
  if (!is_object_type(options)) return Ant_Value_Undefined;

  ant_value_t max = js_get(js, options, "maxByteLength");
  if (is_err(max) || vtype(max) == kTypeUndefined) return max;

  ant_value_t result = js_to_index_fast(js, max, max_length);
  if (is_err(result)) return result;
  if (length > *max_length) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array buffer max length");

  *resizable = true;
  return Ant_Value_Undefined;
}

static ant_value_t js_arraybuffer_constructor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "ArrayBuffer constructor requires 'new'");

  size_t length = 0;
  if (nargs > 0) {
    ant_value_t result = js_to_index_fast(js, args[0], &length);
    if (is_err(result)) return result;
  }

  size_t max_length = 0;
  bool resizable = false;
  ant_value_t options = buffer_read_max_length(js, nargs > 1 ? args[1] : Ant_Value_Undefined, length, &max_length, &resizable);
  if (is_err(options)) return options;

  ArrayBufferData *data = resizable
    ? create_resizable_array_buffer_data(js, length, max_length, false)
    : create_array_buffer_data(js, length);
  if (!data) return resizable
    ? js_mkerr_typed(js, JS_ERR_RANGE, "Array buffer allocation failed")
    : js_mkerr(js, "Failed to allocate ArrayBuffer");
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.arraybuffer_proto, call_new_target);

  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(obj, arraybuffer_finalize);

  return obj;
}

// ArrayBuffer.prototype.slice(begin, end)
static ant_value_t js_arraybuffer_slice(ant_params_t) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!data) return js_mkerr(js, "Invalid ArrayBuffer");
  if (data->is_detached) return js_mkerr(js, "Cannot slice a detached ArrayBuffer");
  
  ssize_t len = (ssize_t)data->length;
  ssize_t begin = 0, end = len;
  if (nargs > 0 && vtype(args[0]) == kTypeNumber) begin = (ssize_t)Ant_Value_AsNumberUnchecked(args[0]);
  if (nargs > 1 && vtype(args[1]) == kTypeNumber) end = (ssize_t)Ant_Value_AsNumberUnchecked(args[1]);
  
  begin = normalize_index(begin, len);
  end = normalize_index(end, len);
  if (end < begin) end = begin;
  
  size_t new_length = (size_t)(end - begin);
  ArrayBufferData *new_data = data->is_shared
    ? create_shared_array_buffer_data(new_length)
    : create_array_buffer_data(js, new_length);
  
  if (!new_data) return js_mkerr(js, "Failed to allocate new ArrayBuffer");
  memcpy(new_data->data, data->data + begin, new_length);

  ant_value_t new_obj = Ant_Object_CreateBare(js);
  ant_value_t proto = data->is_shared ? js->builtins.sharedarraybuffer_proto : js->builtins.arraybuffer_proto;

  if (is_special_object(proto)) js_set_proto_init(new_obj, proto);
  js_set_native(new_obj, new_data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(new_obj, arraybuffer_finalize);
  
  return new_obj;
}

static ant_value_t arraybuffer_transfer(ant_native_params_t, ant_value_t call_this, bool preserve) {
  
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!data) return js_mkerr(js, "Invalid ArrayBuffer");
  
  if (data->is_detached) return js_mkerr(js, "Cannot transfer a detached ArrayBuffer");
  if (data->is_shared) return js_mkerr(js, "Cannot transfer a SharedArrayBuffer");
  
  size_t new_length = data->length;
  if (nargs > 0 && vtype(args[0]) != kTypeUndefined) {
    ant_value_t result = js_to_index_fast(js, args[0], &new_length);
    if (is_err(result)) return result;
  }
  
  if (data->is_detached)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot transfer a detached ArrayBuffer");
  
  if (!buffer_can_transfer(data))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot transfer this ArrayBuffer");

  if (preserve && data->resizable) {
    if (new_length > data->capacity)
      return js_mkerr_typed(js, JS_ERR_RANGE, "ArrayBuffer.prototype.transfer: Invalid length parameter");

    ArrayBufferData *moved = create_array_buffer_data(js, 0);
    if (!moved) return js_mkerr(js, "Failed to allocate new ArrayBuffer");

    size_t old_length = data->length;
    buffer_transfer_storage(js, data, moved);
    if (new_length > old_length) memset(moved->data + old_length, 0, new_length - old_length);
    moved->length = new_length;

    ant_value_t moved_obj = create_arraybuffer_obj(js, moved);
    free_array_buffer_data(js, moved);
    return moved_obj;
  }

  bool same_length = new_length == data->length && !data->resizable;
  ArrayBufferData *new_data = create_array_buffer_data(js, same_length ? 0 : new_length);
  if (!new_data) return js_mkerr(js, "Failed to allocate new ArrayBuffer");
  
  if (same_length) buffer_transfer_storage(js, data, new_data);
  else {
    memcpy(new_data->data, data->data, data->length < new_length ? data->length : new_length);
    buffer_detach(js, data);
  }

  ant_value_t new_obj = create_arraybuffer_obj(js, new_data);
  free_array_buffer_data(js, new_data);
  return new_obj;
}

static ant_value_t js_arraybuffer_transfer(ant_params_t) {
  return arraybuffer_transfer(js, args, nargs, call_this, true);
}

static ant_value_t js_arraybuffer_transferToFixedLength(ant_params_t) {
  return arraybuffer_transfer(js, args, nargs, call_this, false);
}

static ant_value_t js_arraybuffer_resize(ant_params_t) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!data || data->is_shared || !data->resizable)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Method ArrayBuffer.prototype.resize called on incompatible receiver");

  size_t new_length = 0;
  ant_value_t result = js_to_index_fast(js, nargs > 0 ? args[0] : Ant_Value_Undefined, &new_length);
  if (is_err(result)) return result;

  if (data->is_detached) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot resize a detached ArrayBuffer");
  if (new_length > data->capacity)
    return js_mkerr_typed(js, JS_ERR_RANGE, "ArrayBuffer.prototype.resize: Invalid length parameter");

  if (new_length > data->length) memset(data->data + data->length, 0, new_length - data->length);
  data->length = new_length;
  buffer_views_refresh(js, data);

  return Ant_Value_Undefined;
}

static ant_value_t js_arraybuffer_resizable_getter(ant_params_t) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!data || data->is_shared)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Method get ArrayBuffer.prototype.resizable called on incompatible receiver");
  return Ant_Value_FromBool(data->resizable);
}

static ant_value_t buffer_max_length(ant_t *js, ant_value_t call_this, bool shared, const char *name) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!data || (bool)data->is_shared != shared)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Method get %s.prototype.maxByteLength called on incompatible receiver", name);
  if (data->is_detached) return Ant_Value_FromNumber(0);
  return Ant_Value_FromNumber((double)(data->resizable ? data->capacity : buffer_byte_length(data)));
}

static ant_value_t js_arraybuffer_maxByteLength_getter(ant_params_t) {
  return buffer_max_length(js, call_this, false, "ArrayBuffer");
}

static ant_value_t js_sharedarraybuffer_maxByteLength_getter(ant_params_t) {
  return buffer_max_length(js, call_this, true, "SharedArrayBuffer");
}

static ant_value_t js_sharedarraybuffer_growable_getter(ant_params_t) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!data || !data->is_shared)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Method get SharedArrayBuffer.prototype.growable called on incompatible receiver");
  return Ant_Value_FromBool(data->resizable);
}

static ant_value_t js_sharedarraybuffer_grow(ant_params_t) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!buffer_is_growable_shared(data))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Method SharedArrayBuffer.prototype.grow called on incompatible receiver");

  size_t new_length = 0;
  ant_value_t result = js_to_index_fast(js, nargs > 0 ? args[0] : Ant_Value_Undefined, &new_length);
  if (is_err(result)) return result;
  if (new_length > data->capacity)
    return js_mkerr_typed(js, JS_ERR_RANGE, "SharedArrayBuffer.prototype.grow: Invalid length parameter");

  size_t current = __atomic_load_n(&data->length, __ATOMIC_SEQ_CST);
  do {
    if (new_length < current)
      return js_mkerr_typed(js, JS_ERR_RANGE, "SharedArrayBuffer.prototype.grow: Invalid length parameter");
    if (new_length == current) break;
  } while (!__atomic_compare_exchange_n(&data->length, &current, new_length, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

  buffer_views_refresh(js, data);
  return Ant_Value_Undefined;
}

// ArrayBuffer.prototype.detached getter
static ant_value_t js_arraybuffer_detached_getter(ant_params_t) {
  ArrayBufferData *data = buffer_get_arraybuffer_data(call_this);
  if (!data) return Ant_Value_False;
  
  return Ant_Value_FromBool(data->is_detached);
}

// ArrayBuffer.isView(value)
static ant_value_t js_arraybuffer_isView(ant_params_t) {
  if (nargs < 1) return Ant_Value_False;
  return Ant_Value_FromBool(buffer_is_dataview(args[0]) || buffer_get_typedarray_data(args[0]) != NULL);
}

ant_value_t create_arraybuffer_obj(ant_t *js, ArrayBufferData *buffer) {
  ant_value_t ab_obj = Ant_Object_CreateBare(js);
  ant_value_t ab_proto = js->builtins.arraybuffer_proto;
  if (is_special_object(ab_proto)) js_set_proto_init(ab_obj, ab_proto);
  
  js_set_native(ab_obj, buffer, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(ab_obj, arraybuffer_finalize);
  buffer->ref_count++;
  
  return ab_obj;
}

ant_value_t create_shared_arraybuffer_obj(ant_t *js, ArrayBufferData *buffer) {
  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) return obj;
  ant_value_t proto = js->builtins.sharedarraybuffer_proto;
  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  
  js_set_native(obj, buffer, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(obj, arraybuffer_finalize);
  buffer->ref_count++;
  
  return obj;
}

ant_value_t create_arraybuffer_copy(ant_t *js, const void *bytes, size_t length) {
  ArrayBufferData *buffer = create_array_buffer_data(js, length);
  if (!buffer) return Ant_Error_CreateOutOfMemory(js);
  if (bytes && length) memcpy(buffer->data, bytes, length);

  ant_value_t ab_obj = create_arraybuffer_obj(js, buffer);
  free_array_buffer_data(js, buffer);

  return ab_obj;
}

ant_value_t create_resizable_arraybuffer_copy(ant_t *js, const void *bytes, size_t length, size_t max_length) {
  if (length > max_length) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array buffer max length");
  ArrayBufferData *buffer = create_resizable_array_buffer_data(js, length, max_length, false);
  if (!buffer) return js_mkerr_typed(js, JS_ERR_RANGE, "Array buffer allocation failed");
  if (bytes && length) memcpy(buffer->data, bytes, length);

  ant_value_t ab_obj = create_arraybuffer_obj(js, buffer);
  free_array_buffer_data(js, buffer);

  return ab_obj;
}

ant_value_t buffer_clone_arraybuffer(ant_t *js, const ArrayBufferData *data) {
  return data->resizable
    ? create_resizable_arraybuffer_copy(js, data->data, data->length, data->capacity)
    : create_arraybuffer_copy(js, data->data, data->length);
}

static ant_value_t js_sharedarraybuffer_constructor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "SharedArrayBuffer constructor requires 'new'");

  size_t length = 0;
  if (nargs > 0) {
    ant_value_t result = js_to_index_fast(js, args[0], &length);
    if (is_err(result)) return result;
  }

  size_t max_length = 0;
  bool growable = false;
  
  ant_value_t options = buffer_read_max_length(js, nargs > 1 ? args[1] : Ant_Value_Undefined, length, &max_length, &growable);
  if (is_err(options)) return options;

  ArrayBufferData *data = growable
    ? create_resizable_array_buffer_data(js, length, max_length, true)
    : create_shared_array_buffer_data(length);
  
  if (!data) return growable
    ? js_mkerr_typed(js, JS_ERR_RANGE, "Array buffer allocation failed")
    : js_mkerr(js, "Failed to allocate SharedArrayBuffer");
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js->builtins.sharedarraybuffer_proto;

  if (is_special_object(proto)) js_set_proto_init(obj, proto);
  js_set_native(obj, data, BUFFER_ARRAYBUFFER_NATIVE_TAG);
  js_set_finalizer(obj, arraybuffer_finalize);
  
  return obj;
}

void cleanup_buffer_module(ant_t *js) {
  buffer_views_free(js);
  for (size_t i = 0; i < js->buffers.count; i++) free(js->buffers.entries[i]);
  free(js->buffers.entries);
  js->buffers.entries = NULL;
  js->buffers.count = 0;
  js->buffers.cap = 0;
  js->buffers.bytes = 0;
  js->buffers.meta_bytes = 0;
}

size_t buffer_get_external_memory(ant_t *js) {
  return
    js->buffers.meta_bytes + js->buffers.bytes +
    js->buffers.cap * sizeof(ArrayBufferData *) +
    js->buffers.count * sizeof(ArrayBufferData);
}

void arraybuffer_initialize(ant_t *js) {
  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t arraybuffer_ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t arraybuffer_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(arraybuffer_proto, object_proto);
  js->builtins.arraybuffer_proto = arraybuffer_proto;
  
  defmethod(js, arraybuffer_proto, "slice", 5, Ant_Function_CreateWithArity(js_arraybuffer_slice, 2));
  defmethod(js, arraybuffer_proto, "transfer", 8, Ant_Function_Create(js_arraybuffer_transfer));
  defmethod(js, arraybuffer_proto, "transferToFixedLength", 21, Ant_Function_Create(js_arraybuffer_transferToFixedLength));
  defmethod(js, arraybuffer_proto, "resize", 6, Ant_Function_CreateWithArity(js_arraybuffer_resize, 1));
  js_set_getter_desc(js, arraybuffer_proto, "resizable", 9, Ant_Function_Create(js_arraybuffer_resizable_getter), JS_DESC_C);
  js_set_getter_desc(js, arraybuffer_proto, "maxByteLength", 13, Ant_Function_Create(js_arraybuffer_maxByteLength_getter), JS_DESC_C);
  
  js_set_getter_desc(js, arraybuffer_proto, "detached", 8, Ant_Function_Create(js_arraybuffer_detached_getter), JS_DESC_C);
  js_set_getter_desc(js, arraybuffer_proto, "byteLength", 10, js_mkfun_meta(&arraybuffer_byte_length_getter.meta), JS_DESC_C);
  js_set_tostringtag(js, arraybuffer_proto, js_mkstr(js, "ArrayBuffer", 11));
  
  js_set_slot(arraybuffer_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(js_arraybuffer_constructor, 1));
  mkprop_exact_attrs(js, arraybuffer_ctor_obj, Ant_String_FromLiteral(js, "prototype"), arraybuffer_proto, 0);
  mkprop_exact_attrs(js, arraybuffer_ctor_obj, Ant_String_FromLiteral(js, "name"), Ant_String_FromLiteral(js, "ArrayBuffer"), ANT_PROP_ATTR_CONFIGURABLE);
  defmethod(js, arraybuffer_ctor_obj, "isView", 6, Ant_Function_CreateWithArity(js_arraybuffer_isView, 1));
  js_define_species_getter(js, arraybuffer_ctor_obj);
  
  ant_value_t arraybuffer_ctor = js_obj_to_func(js, arraybuffer_ctor_obj);
  defmethod(js, arraybuffer_proto, "constructor", 11, arraybuffer_ctor);
  js_set_global_builtin(js, "ArrayBuffer", arraybuffer_ctor);
}

void sharedarraybuffer_initialize(ant_t *js) {
  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t sharedarraybuffer_ctor_obj = Ant_Object_CreateBare(js);
  ant_value_t sharedarraybuffer_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(sharedarraybuffer_proto, object_proto);
  js->builtins.sharedarraybuffer_proto = sharedarraybuffer_proto;
  
  defmethod(js, sharedarraybuffer_proto, "slice", 5, Ant_Function_CreateWithArity(js_arraybuffer_slice, 2));
  defmethod(js, sharedarraybuffer_proto, "grow", 4, Ant_Function_CreateWithArity(js_sharedarraybuffer_grow, 1));
  js_set_getter_desc(js, sharedarraybuffer_proto, "growable", 8, Ant_Function_Create(js_sharedarraybuffer_growable_getter), JS_DESC_C);
  js_set_getter_desc(js, sharedarraybuffer_proto, "maxByteLength", 13, Ant_Function_Create(js_sharedarraybuffer_maxByteLength_getter), JS_DESC_C);
  js_set_getter_desc(js, sharedarraybuffer_proto, "byteLength", 10, js_mkfun_meta(&sharedarraybuffer_byte_length_getter.meta), JS_DESC_C);
  js_set_tostringtag(js, sharedarraybuffer_proto, js_mkstr(js, "SharedArrayBuffer", 17));
  
  js_set_slot(sharedarraybuffer_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(js_sharedarraybuffer_constructor, 1));
  mkprop_exact_attrs(js, sharedarraybuffer_ctor_obj, Ant_String_FromLiteral(js, "prototype"), sharedarraybuffer_proto, 0);
  mkprop_exact_attrs(js, sharedarraybuffer_ctor_obj, Ant_String_FromLiteral(js, "name"), Ant_String_FromLiteral(js, "SharedArrayBuffer"), ANT_PROP_ATTR_CONFIGURABLE);
  js_define_species_getter(js, sharedarraybuffer_ctor_obj);
  
  ant_value_t sharedarraybuffer_ctor = js_obj_to_func(js, sharedarraybuffer_ctor_obj);
  defmethod(js, sharedarraybuffer_proto, "constructor", 11, sharedarraybuffer_ctor);
  js_set_global_builtin(js, "SharedArrayBuffer", sharedarraybuffer_ctor);
}
