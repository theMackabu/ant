#include <stdlib.h>
#include <string.h>
#include <uthash.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "silver/call.h"

#include "modules/blob.h"
#include "modules/buffer.h"
#include "modules/collections.h"
#include "modules/date.h"
#include "modules/domexception.h"
#include "modules/iterator.h"
#include "modules/message_port.h"
#include "modules/regex.h"
#include "modules/wire.h"

enum {
  WIRE_UNDEFINED = 'u',
  WIRE_NULL = 'n',
  WIRE_TRUE = 'T',
  WIRE_FALSE = 'F',
  WIRE_NUMBER = 'N',
  WIRE_BIGINT = 'I',
  WIRE_STRING = 'S',
  WIRE_REF = 'R',
  WIRE_OBJECT = 'O',
  WIRE_ARRAY = 'A',
  WIRE_PROP = 'k',
  WIRE_END = '}',
  WIRE_BOXED = 'b',
  WIRE_DATE = 'D',
  WIRE_REGEXP = 'X',
  WIRE_MAP = 'M',
  WIRE_SET = 'E',
  WIRE_ERROR = 'Z',
  WIRE_ARRAY_BUFFER = 'Y',
  WIRE_SHARED_BUFFER = 'Q',
  WIRE_MOVED_BUFFER = 'W',
  WIRE_RESIZABLE_BUFFER = 'J',
  WIRE_VIEW = 'V',
  WIRE_BLOB = 'L',
  WIRE_PORT = 'P',
};

static constexpr uint8_t WIRE_VIEW_DATAVIEW = 0xff;
static constexpr int WIRE_MAX_DEPTH = 2048;

typedef struct {
  ant_value_t key;
  uint32_t id;
  UT_hash_handle hh;
} wire_ref_t;

typedef struct {
  ant_t *js;
  ant_wire_t *wire;
  wire_ref_t *refs;
  uint32_t next_id;
  ant_value_t seen;
  ant_value_t transfer;
  ant_value_t source_port;
  ArrayBufferData **moving;
  uint32_t moving_count;
  uint32_t moving_cap;
  int depth;
  bool error_props;
} wire_writer_t;

typedef struct {
  ant_t *js;
  ant_wire_t *wire;
  const uint8_t *p;
  const uint8_t *end;
  ant_value_t refs;
  ant_value_t ports;
  int depth;
} wire_reader_t;

ant_wire_t *ant_wire_retain(ant_wire_t *wire) {
  if (wire) atomic_fetch_add_explicit(&wire->refs, 1, memory_order_relaxed);
  return wire;
}

void ant_wire_release(ant_wire_t *wire) {
  if (!wire || atomic_fetch_sub_explicit(&wire->refs, 1, memory_order_acq_rel) != 1) return;
  
  for (uint32_t i = 0; i < wire->shared_count; i++) free_array_buffer_data(NULL, wire->shared[i]);
  for (uint32_t i = 0; i < wire->moved_count; i++) free_array_buffer_data(NULL, wire->moved[i]);
  for (uint32_t i = 0; i < wire->blob_count; i++) blob_storage_release(wire->blobs[i]);
  for (uint32_t i = 0; i < wire->port_count; i++) ant_port_end_release(wire->ports[i]);
  
  free(wire->shared);
  free(wire->moved);
  free(wire->blobs);
  free(wire->ports);
  free(wire->data);
  free(wire);
}

static bool wire_reserve(ant_wire_t *wire, size_t extra) {
  if (wire->len + extra <= wire->cap) return true;
  size_t cap = wire->cap ? wire->cap * 2 : 256;
  while (cap < wire->len + extra) cap *= 2;
  uint8_t *next = realloc(wire->data, cap);
  if (!next) return false;
  wire->data = next;
  wire->cap = cap;
  return true;
}

static bool put_bytes(wire_writer_t *w, const void *bytes, size_t len) {
  if (!wire_reserve(w->wire, len)) return false;
  if (len) memcpy(w->wire->data + w->wire->len, bytes, len);
  w->wire->len += len;
  return true;
}

static bool put_u8(wire_writer_t *w, uint8_t value) {
  return put_bytes(w, &value, 1);
}

static bool put_varint(wire_writer_t *w, uint64_t value) {
  uint8_t buf[10];
  size_t n = 0;
  do {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    buf[n++] = byte | (value ? 0x80 : 0);
  } while (value);
  return put_bytes(w, buf, n);
}

static bool put_f64(wire_writer_t *w, double value) {
  return put_bytes(w, &value, sizeof(value));
}

static bool put_str(wire_writer_t *w, const char *str, size_t len) {
  return put_varint(w, len) && put_bytes(w, str, len);
}

static ant_value_t wire_clone_error(ant_t *js, const char *message) {
  return js_throw(js, make_dom_exception(js, message, "DataCloneError"));
}

static ant_value_t wire_oom(ant_t *js) {
  return js_mkerr(js, "out of memory");
}

static ant_value_t wire_uncloneable(wire_writer_t *w, ant_value_t value) {
  ant_t *js = w->js;
  char message[160];
  const char *repr = "#<Object>";
  size_t repr_len = 9;
  char buf[96];

  if (vtype(value) == kTypeSymbol || vtype(value) == kTypeFunction || vtype(value) == kTypeBuiltin) {
    ant_value_t str = js_tostring_val(js, value);
    if (vtype(str) == kTypeString) {
      repr = js_getstr(js, str, &repr_len);
      if (repr_len > 64) repr_len = 64;
    } else if (is_err(str)) js_take_thrown(js, str);
  } else if (is_object_type(value)) {
    ant_value_t tag = js_get(js, value, "constructor");
    ant_value_t name = is_object_type(tag) || vtype(tag) == kTypeFunction ? js_get(js, tag, "name") : js_mkundef();
    if (vtype(name) == kTypeString) {
      size_t len = 0;
      const char *ptr = js_getstr(js, name, &len);
      int n = snprintf(buf, sizeof(buf), "#<%.*s>", (int)(len > 64 ? 64 : len), ptr);
      repr = buf;
      repr_len = n > 0 ? (size_t)n : 0;
    }
  }

  snprintf(message, sizeof(message), "%.*s could not be cloned.", (int)repr_len, repr);
  return wire_clone_error(js, message);
}

static int wire_port_index(wire_writer_t *w, ant_value_t port) {
  if (vtype(w->transfer) != kTypeArray) return -1;
  ant_offset_t len = js_arr_len(w->js, w->transfer);
  int index = 0;
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = js_arr_get(w->js, w->transfer, i);
    if (!ant_message_port_is(item)) continue;
    if (item == port) return index;
    index++;
  }
  return -1;
}

static bool wire_lookup(wire_writer_t *w, ant_value_t value, uint32_t *id) {
  wire_ref_t *ref = NULL;
  HASH_FIND(hh, w->refs, &value, sizeof(value), ref);
  if (!ref) return false;
  *id = ref->id;
  return true;
}

static bool wire_remember(wire_writer_t *w, ant_value_t value) {
  wire_ref_t *ref = calloc(1, sizeof(*ref));
  if (!ref) return false;
  ref->key = value;
  ref->id = w->next_id++;
  HASH_ADD(hh, w->refs, key, sizeof(ref->key), ref);
  js_arr_push(w->js, w->seen, value);
  return true;
}

static ant_value_t wire_write(wire_writer_t *w, ant_value_t value);

static ant_value_t wire_write_props(wire_writer_t *w, ant_value_t obj, ant_offset_t skip_indices) {
  ant_t *js = w->js;
  ant_object_t *ptr = js_obj_ptr(obj);
  ant_iter_t iter = js_prop_iter_begin(js, obj);
  ant_iter_key_t key;
  ant_value_t prop;

  while (ptr && js_prop_iter_next_key(&iter, &key, &prop)) {
    if (key.is_symbol || !js_is_own_enumerable_prop(js, obj, ptr, &key)) continue;
    const ant_shape_prop_t *meta = ant_shape_prop_at(ptr->shape, key.slot);
    bool plain = !ptr->flags.is_exotic && meta && !meta->has_getter && !meta->has_setter;
    if (skip_indices) {
      if (key.key_len == 6 && memcmp(key.str, "length", 6) == 0) continue;
      char *end = NULL;
      unsigned long long idx = key.key_len && key.str[0] >= '0' && key.str[0] <= '9' ? strtoull(key.str, &end, 10) : ~0ull;
      if (end == key.str + key.key_len && idx < skip_indices) continue;
    }

    ant_value_t value = plain ? prop : js_get(js, obj, key.str);
    if (is_err(value)) {
      js_prop_iter_end(&iter);
      return value;
    }
    if (!put_u8(w, WIRE_PROP) || !put_str(w, key.str, key.key_len)) {
      js_prop_iter_end(&iter);
      return wire_oom(js);
    }
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, value);
    ant_value_t r = wire_write(w, value);
    GC_ROOT_RESTORE(js, mark);
    if (is_err(r)) {
      js_prop_iter_end(&iter);
      return r;
    }
  }

  js_prop_iter_end(&iter);
  return put_u8(w, WIRE_END) ? js_mkundef() : wire_oom(js);
}

static bool wire_boxed_value(ant_value_t obj, ant_value_t *out) {
  if (vtype(obj) != kTypeObject) return false;
  ant_extra_slot_t *entry = ant_object_extra_slot(js_obj_ptr(obj), SLOT_PRIMITIVE);
  if (!entry) return false;
  uint8_t t = vtype(entry->value);
  if (t != kTypeNumber && t != kTypeBool && t != kTypeString && t != kTypeBigInt) return false;
  *out = entry->value;
  return true;
}

static ant_value_t wire_write_bigint(wire_writer_t *w, ant_value_t value) {
  ant_value_t str = js_tostring_val(w->js, value);
  if (is_err(str)) return str;
  size_t len = 0;
  const char *digits = js_getstr(w->js, str, &len);
  return put_u8(w, WIRE_BIGINT) && put_str(w, digits, len) ? js_mkundef() : wire_oom(w->js);
}

static ant_value_t wire_write_string(wire_writer_t *w, uint8_t tag, ant_value_t value) {
  size_t len = 0;
  const char *bytes = js_getstr(w->js, value, &len);
  return put_u8(w, tag) && put_str(w, bytes ? bytes : "", bytes ? len : 0) ? js_mkundef() : wire_oom(w->js);
}

static ant_value_t wire_write_primitive(wire_writer_t *w, ant_value_t value) {
  switch (vtype(value)) {
    case kTypeUndefined: return put_u8(w, WIRE_UNDEFINED) ? js_mkundef() : wire_oom(w->js);
    case kTypeNull: return put_u8(w, WIRE_NULL) ? js_mkundef() : wire_oom(w->js);
    case kTypeBool: return put_u8(w, js_truthy(w->js, value) ? WIRE_TRUE : WIRE_FALSE) ? js_mkundef() : wire_oom(w->js);
    case kTypeNumber: return put_u8(w, WIRE_NUMBER) && put_f64(w, js_getnum(value)) ? js_mkundef() : wire_oom(w->js);
    case kTypeBigInt: return wire_write_bigint(w, value);
    case kTypeString: return wire_write_string(w, WIRE_STRING, value);
    default: return wire_uncloneable(w, value);
  }
}

static bool wire_in_transfer(wire_writer_t *w, ant_value_t value) {
  if (vtype(w->transfer) != kTypeArray) return false;
  ant_offset_t len = js_arr_len(w->js, w->transfer);
  for (ant_offset_t i = 0; i < len; i++) if (js_arr_get(w->js, w->transfer, i) == value) return true;
  return false;
}

static ant_value_t wire_write_buffer(wire_writer_t *w, ant_value_t value, ArrayBufferData *data) {
  ant_t *js = w->js;
  if (data->is_detached) return wire_clone_error(js, "An ArrayBuffer is detached and could not be cloned.");

  if (data->is_shared) {
    ant_wire_t *wire = w->wire;
    if (wire->shared_count == wire->shared_cap) {
      uint32_t cap = wire->shared_cap ? wire->shared_cap * 2 : 4;
      ArrayBufferData **next = realloc(wire->shared, cap * sizeof(*next));
      if (!next) return wire_oom(js);
      wire->shared = next;
      wire->shared_cap = cap;
    }
    
    data->ref_count++;
    wire->shared[wire->shared_count] = data;
    
    if (!put_u8(w, WIRE_SHARED_BUFFER) || !put_varint(w, wire->shared_count++)) return wire_oom(js);
    return wire_remember(w, value) ? js_mkundef() : wire_oom(js);
  }

  if (buffer_can_transfer(data) && wire_in_transfer(w, value)) {
    if (w->moving_count == w->moving_cap) {
      uint32_t cap = w->moving_cap ? w->moving_cap * 2 : 4;
      ArrayBufferData **next = realloc(w->moving, cap * sizeof(*next));
      if (!next) return wire_oom(js);
      w->moving = next;
      w->moving_cap = cap;
    }
    
    w->moving[w->moving_count] = data;
    if (!put_u8(w, WIRE_MOVED_BUFFER) || !put_varint(w, w->moving_count++)) return wire_oom(js);
    return wire_remember(w, value) ? js_mkundef() : wire_oom(js);
  }

  bool ok = data->resizable
    ? put_u8(w, WIRE_RESIZABLE_BUFFER) && put_varint(w, data->capacity)
    : put_u8(w, WIRE_ARRAY_BUFFER);
  
  if (
    !ok || !put_varint(w, data->length) || 
    !put_bytes(w, data->data, data->length)
  ) return wire_oom(js);
  
  return wire_remember(w, value) ? js_mkundef() : wire_oom(js);
}

static ant_value_t wire_write_view(wire_writer_t *w, ant_value_t value, uint8_t kind, size_t offset, size_t length, bool tracking) {
  ant_value_t buffer = js_get_slot(value, SLOT_DATA);
  if (!put_u8(w, WIRE_VIEW) || !put_u8(w, kind) || !put_u8(w, tracking ? 1 : 0) || !put_varint(w, offset) || !put_varint(w, length))
    return wire_oom(w->js);
  ant_value_t r = wire_write(w, buffer);
  if (is_err(r)) return r;
  return wire_remember(w, value) ? js_mkundef() : wire_oom(w->js);
}

static ant_value_t wire_error_field(ant_t *js, ant_value_t error, const char *key, const char **out, size_t *len) {
  *out = NULL;
  *len = 0;
  ant_value_t value = js_get(js, error, key);
  if (is_err(value)) return value;
  if (vtype(value) == kTypeString) *out = js_getstr(js, value, len);
  return value;
}

static ant_value_t wire_write_error(wire_writer_t *w, ant_value_t value) {
  ant_t *js = w->js;
  const char *name, *message, *stack;
  size_t name_len, message_len, stack_len;
  GC_ROOT_SAVE(mark, js);

  ant_value_t r = wire_error_field(js, value, "name", &name, &name_len);
  if (is_err(r)) { GC_ROOT_RESTORE(js, mark); return r; }
  GC_ROOT_PIN(js, r);
  r = wire_error_field(js, value, "message", &message, &message_len);
  if (is_err(r)) { GC_ROOT_RESTORE(js, mark); return r; }
  GC_ROOT_PIN(js, r);
  r = wire_error_field(js, value, "stack", &stack, &stack_len);
  if (is_err(r)) { GC_ROOT_RESTORE(js, mark); return r; }
  GC_ROOT_PIN(js, r);

  bool ok = put_u8(w, WIRE_ERROR)
    && put_str(w, name ? name : "Error", name ? name_len : 5)
    && put_u8(w, message ? 1 : 0) && (!message || put_str(w, message, message_len))
    && put_u8(w, stack ? 1 : 0) && (!stack || put_str(w, stack, stack_len));
  GC_ROOT_RESTORE(js, mark);
  if (!ok) return wire_oom(js);
  if (!wire_remember(w, value)) return wire_oom(js);

  ant_value_t cause = js_mkundef();
  bool has_cause = js_try_get_own_data_prop(js, value, "cause", 5, &cause);
  if (!put_u8(w, has_cause ? 1 : 0)) return wire_oom(js);
  if (has_cause && is_err(r = wire_write(w, cause))) return r;

  ant_value_t errors = js_mkundef();
  bool has_errors = js_try_get_own_data_prop(js, value, "errors", 6, &errors);
  if (!put_u8(w, has_errors ? 1 : 0)) return wire_oom(js);
  if (has_errors && is_err(r = wire_write(w, errors))) return r;

  if (w->error_props) return wire_write_props(w, value, 0);
  return put_u8(w, WIRE_END) ? js_mkundef() : wire_oom(js);
}

static bool wire_add_blob(ant_wire_t *wire, blob_storage_t *storage) {
  if (wire->blob_count == wire->blob_cap) {
    uint32_t cap = wire->blob_cap ? wire->blob_cap * 2 : 2;
    blob_storage_t **next = realloc(wire->blobs, cap * sizeof(*next));
    if (!next) return false;
    wire->blobs = next;
    wire->blob_cap = cap;
  }
  
  wire->blobs[wire->blob_count++] = blob_storage_retain(storage);
  
  return true;
}

static ant_value_t wire_write_object(wire_writer_t *w, ant_value_t value) {
  ant_t *js = w->js;
  uint32_t id = 0;
  if (wire_lookup(w, value, &id)) return put_u8(w, WIRE_REF) && put_varint(w, id) ? js_mkundef() : wire_oom(js);
  if (ant_wire_is_uncloneable(js, value))
    return wire_clone_error(js, "Cannot clone object of unsupported type.");

  if (ant_message_port_is(value)) {
    int index = wire_port_index(w, value);
    if (index < 0) return wire_clone_error(js, "Object that needs transfer was found in message but not listed in transferList");
    if (!put_u8(w, WIRE_PORT) || !put_varint(w, (uint64_t)index)) return wire_oom(js);
    return wire_remember(w, value) ? js_mkundef() : wire_oom(js);
  }

  TypedArrayData *ta = buffer_get_typedarray_data(value);
  if (!ta && vtype(value) == kTypeTypedArray) ta = (TypedArrayData *)js_gettypedarray(value);
  if (ta) {
    if (!ta->buffer || !Ant_Buffer_ViewIsLive(ta)) return wire_clone_error(js, "An ArrayBuffer is detached and could not be cloned.");
    return wire_write_view(w, value, (uint8_t)ta->type, ta->byte_offset, ta->length, ta->length_tracking);
  }

  if (buffer_is_dataview(value)) {
    DataViewData *dv = buffer_get_dataview_data(value);
    if (!dv || !dv->buffer || !Ant_Buffer_ViewIsLive(dv)) return wire_clone_error(js, "An ArrayBuffer is detached and could not be cloned.");
    return wire_write_view(w, value, WIRE_VIEW_DATAVIEW, dv->byte_offset, dv->byte_length, dv->length_tracking);
  }

  ArrayBufferData *abd = buffer_get_arraybuffer_data(value);
  if (abd) return wire_write_buffer(w, value, abd);

  uint8_t t = vtype(value);
  if (t == kTypeFunction || t == kTypeBuiltin || t == kTypePromise || t == kTypeGenerator) return wire_uncloneable(w, value);
  if (js_is_arguments(value)) return wire_uncloneable(w, value);

  ant_object_t *ptr = js_obj_ptr(value);
  if (!ptr) return wire_uncloneable(w, value);
  if (ptr->type_tag == kTypeWeakMap || ptr->type_tag == kTypeWeakSet) return wire_uncloneable(w, value);
  if (js_proxy_is_host(value) || ptr->flags.is_exotic) {
    if (t != kTypeArray) return wire_uncloneable(w, value);
  }

  ant_value_t boxed;
  if (wire_boxed_value(value, &boxed)) {
    if (!put_u8(w, WIRE_BOXED)) return wire_oom(js);
    ant_value_t r = wire_write_primitive(w, boxed);
    if (is_err(r)) return r;
    return wire_remember(w, value) ? js_mkundef() : wire_oom(js);
  }

  if (is_date_instance(value)) {
    ant_value_t time = js_get_slot(value, SLOT_DATA);
    double ms = vtype(time) == kTypeNumber ? js_getnum(time) : 0;
    if (!put_u8(w, WIRE_DATE) || !put_f64(w, ms)) return wire_oom(js);
    return wire_remember(w, value) ? js_mkundef() : wire_oom(js);
  }

  if (ptr->flags.regexp_brand) {
    ant_value_t source = js_get_slot(value, SLOT_DATA);
    ant_value_t flags = js_get_slot(value, SLOT_REGEXP_FLAGS_STRING);
    size_t slen = 0, flen = 0;
    const char *s = vtype(source) == kTypeString ? js_getstr(js, source, &slen) : "";
    const char *f = vtype(flags) == kTypeString ? js_getstr(js, flags, &flen) : "";
    if (!put_u8(w, WIRE_REGEXP) || !put_str(w, s, slen) || !put_str(w, f, flen)) return wire_oom(js);
    return wire_remember(w, value) ? js_mkundef() : wire_oom(js);
  }

  if (js_get_slot(value, SLOT_ERROR_BRAND) == js_true) return wire_write_error(w, value);

  blob_data_t *blob = blob_is_blob(js, value) ? blob_get_data(value) : NULL;
  if (blob) {
    const char *type = blob->type ? blob->type : "";
    if (!wire_add_blob(w->wire, blob->storage)) return wire_oom(js);
    if (!put_u8(w, WIRE_BLOB) || !put_str(w, type, strlen(type)) || !put_varint(w, w->wire->blob_count - 1))
      return wire_oom(js);
    if (!put_u8(w, blob->name ? 1 : 0)) return wire_oom(js);
    if (blob->name && (!put_str(w, blob->name, strlen(blob->name)) || !put_f64(w, (double)blob->last_modified))) return wire_oom(js);
    return wire_remember(w, value) ? js_mkundef() : wire_oom(js);
  }

  if (ptr->type_tag == kTypeMap) {
    if (!put_u8(w, WIRE_MAP) || !wire_remember(w, value)) return wire_oom(js);
    map_entry_t **head = get_map_from_obj(value);
    if (head) {
      map_entry_t *e, *tmp;
      HASH_ITER(hh, *head, e, tmp) {
        if (!put_u8(w, WIRE_PROP)) return wire_oom(js);
        ant_value_t r = wire_write(w, e->key_val);
        if (is_err(r)) return r;
        r = wire_write(w, e->value);
        if (is_err(r)) return r;
      }
    }
    return put_u8(w, WIRE_END) ? js_mkundef() : wire_oom(js);
  }

  if (ptr->type_tag == kTypeSet) {
    if (!put_u8(w, WIRE_SET) || !wire_remember(w, value)) return wire_oom(js);
    set_entry_t **head = get_set_from_obj(value);
    if (head) {
      set_entry_t *e, *tmp;
      HASH_ITER(hh, *head, e, tmp) {
        if (!put_u8(w, WIRE_PROP)) return wire_oom(js);
        ant_value_t r = wire_write(w, e->value);
        if (is_err(r)) return r;
      }
    }
    return put_u8(w, WIRE_END) ? js_mkundef() : wire_oom(js);
  }

  if (t == kTypeArray) {
    ant_offset_t len = js_arr_len(js, value);
    if (!put_u8(w, WIRE_ARRAY) || !put_varint(w, len) || !wire_remember(w, value)) return wire_oom(js);
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t r = wire_write(w, js_arr_get(js, value, i));
      if (is_err(r)) return r;
    }
    return wire_write_props(w, value, len ? len : 1);
  }

  if (!put_u8(w, WIRE_OBJECT) || !wire_remember(w, value)) return wire_oom(js);
  return wire_write_props(w, value, 0);
}

static ant_value_t wire_write(wire_writer_t *w, ant_value_t value) {
  if (!is_object_type(value) && vtype(value) != kTypeTypedArray) return wire_write_primitive(w, value);
  if (++w->depth > WIRE_MAX_DEPTH) return Ant_Error_CreateStackOverflow(w->js);
  ant_value_t r = wire_write_object(w, value);
  w->depth--;
  return r;
}

static ant_value_t wire_check_transfer(ant_t *js, ant_value_t list, ant_value_t source_port, ant_value_t *out) {
  *out = js_mkundef();
  if (vtype(list) == kTypeUndefined || vtype(list) == kTypeNull) return js_mkundef();

  iterator_t it;
  if (!is_object_type(list) || !js_iter_open(js, list, &it)) return Ant_Exception_Pending(js)
    ? Ant_Exception_Current(js)
    : js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", "Optional transferList argument must be an iterable");

  ant_value_t items = js_mkarr(js);
  ant_value_t item;
  while (js_iter_next(js, &it, &item)) {
    ArrayBufferData *data = is_object_type(item) ? buffer_get_arraybuffer_data(item) : NULL;
    bool port = ant_message_port_is(item);
    ant_value_t error = js_mkundef();

    if (!data && !port) error = js_mkerr_typed(js, JS_ERR_TYPE, "Found invalid value in transferList.");
    else if (port && item == source_port) error = wire_clone_error(js, "Transfer list contains source port");
    else if (data && (data->untransferable || ant_wire_is_untransferable(js, item))) error = wire_clone_error(js, "An ArrayBuffer is marked as untransferable");
    else if (data && data->is_shared) error = wire_clone_error(js, "SharedArrayBuffer cannot be transferred");
    else if (data && data->is_detached) error = wire_clone_error(js, "An ArrayBuffer is detached and could not be transferred.");
    else {
      ant_offset_t len = js_arr_len(js, items);
      for (ant_offset_t i = 0; i < len; i++)
        if (js_arr_get(js, items, i) == item) error = wire_clone_error(js, "Transfer list contains duplicate");
    }

    if (is_err(error)) {
      js_iter_close(js, &it);
      return error;
    }
    js_arr_push(js, items, item);
  }

  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  *out = items;
  return js_mkundef();
}

static void wire_free_refs(wire_writer_t *w) {
  wire_ref_t *ref, *tmp;
  HASH_ITER(hh, w->refs, ref, tmp) {
    HASH_DEL(w->refs, ref);
    free(ref);
  }
}

static bool wire_take_moved(wire_writer_t *w) {
  ant_wire_t *wire = w->wire;
  if (!w->moving_count) return true;

  wire->moved = calloc(w->moving_count, sizeof(*wire->moved));
  if (!wire->moved) return false;
  
  for (uint32_t i = 0; i < w->moving_count; i++) {
    if ((wire->moved[i] = buffer_storage_holder_new())) continue;
    for (uint32_t j = 0; j < i; j++) free(wire->moved[j]);
    
    free(wire->moved);
    wire->moved = NULL;
    
    return false;
  }

  for (uint32_t i = 0; i < w->moving_count; i++) buffer_move_storage(w->js, w->moving[i], wire->moved[i]);
  wire->moved_count = w->moving_count;
  
  return true;
}

static bool wire_add_port(ant_wire_t *wire, ant_port_end_t *end) {
  if (wire->port_count == wire->port_cap) {
    uint32_t cap = wire->port_cap ? wire->port_cap * 2 : 2;
    ant_port_end_t **next = realloc(wire->ports, cap * sizeof(*next));
    if (!next) return false;
    wire->ports = next;
    wire->port_cap = cap;
  }
  wire->ports[wire->port_count++] = end;
  return true;
}

static ant_value_t wire_serialize(
  ant_t *js, ant_value_t value, ant_value_t transfer_list,
  ant_value_t source_port, bool error_props, ant_wire_t **out
) {
  *out = NULL;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);

  ant_value_t transfer = js_mkundef();
  ant_value_t r = wire_check_transfer(js, transfer_list, source_port, &transfer);
  if (is_err(r)) {
    GC_ROOT_RESTORE(js, root_mark);
    return r;
  }
  GC_ROOT_PIN(js, transfer);

  ant_wire_t *wire = calloc(1, sizeof(*wire));
  if (!wire) {
    GC_ROOT_RESTORE(js, root_mark);
    return wire_oom(js);
  }
  atomic_init(&wire->refs, 1);

  wire_writer_t w = {
    .js = js, .wire = wire, .seen = js_mkarr(js),
    .transfer = transfer, .source_port = source_port,
    .error_props = error_props,
  };
  
  GC_ROOT_PIN(js, w.seen);

  r = wire_write(&w, value);
  wire_free_refs(&w);
  if (is_err(r)) {
    free(w.moving);
    ant_wire_release(wire);
    GC_ROOT_RESTORE(js, root_mark);
    return r;
  }

  ant_offset_t count = vtype(transfer) == kTypeArray ? js_arr_len(js, transfer) : 0;
  for (ant_offset_t i = 0; i < count; i++) {
    ant_value_t item = js_arr_get(js, transfer, i);
    if (!ant_message_port_is(item)) continue;
    ant_port_end_t *end = ant_message_port_take(js, item);
    if (!end || !wire_add_port(wire, end)) {
      if (end) ant_port_end_release(end);
      ant_wire_release(wire);
      GC_ROOT_RESTORE(js, root_mark);
      return wire_clone_error(js, "MessagePort was found in message but could not be transferred");
    }
  }

  bool moved = wire_take_moved(&w);
  free(w.moving);
  if (!moved) {
    ant_wire_release(wire);
    GC_ROOT_RESTORE(js, root_mark);
    return wire_oom(js);
  }

  for (ant_offset_t i = 0; i < count; i++) {
    ArrayBufferData *data = buffer_get_arraybuffer_data(js_arr_get(js, transfer, i));
    if (data && !data->is_detached) buffer_detach(js, data);
  }

  GC_ROOT_RESTORE(js, root_mark);
  *out = wire;
  return js_mkundef();
}

ant_value_t ant_wire_serialize(
  ant_t *js, ant_value_t value, ant_value_t transfer_list,
  ant_value_t source_port, ant_wire_t **out
) {
  return wire_serialize(js, value, transfer_list, source_port, false, out);
}

ant_value_t ant_wire_serialize_error(ant_t *js, ant_value_t error, ant_wire_t **out) {
  return wire_serialize(js, error, js_mkundef(), js_mkundef(), true, out);
}

static bool get_u8(wire_reader_t *r, uint8_t *out) {
  if (r->p >= r->end) return false;
  *out = *r->p++;
  return true;
}

static bool get_varint(wire_reader_t *r, uint64_t *out) {
  uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    uint8_t byte;
    if (!get_u8(r, &byte)) return false;
    value |= (uint64_t)(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      *out = value;
      return true;
    }
  }
  return false;
}

static bool get_f64(wire_reader_t *r, double *out) {
  if ((size_t)(r->end - r->p) < sizeof(*out)) return false;
  memcpy(out, r->p, sizeof(*out));
  r->p += sizeof(*out);
  return true;
}

static bool get_bytes(wire_reader_t *r, const uint8_t **out, size_t len) {
  if ((size_t)(r->end - r->p) < len) return false;
  *out = r->p;
  r->p += len;
  return true;
}

static bool get_str(wire_reader_t *r, const char **out, size_t *len) {
  uint64_t n;
  const uint8_t *bytes;
  if (!get_varint(r, &n) || !get_bytes(r, &bytes, (size_t)n)) return false;
  *out = (const char *)bytes;
  *len = (size_t)n;
  return true;
}

static ant_value_t wire_corrupt(ant_t *js) {
  return js_mkerr(js, "malformed serialized message");
}

static ant_value_t wire_read(wire_reader_t *r);

static ant_value_t wire_track(wire_reader_t *r, ant_value_t value) {
  if (!is_err(value)) js_arr_push(r->js, r->refs, value);
  return value;
}

static ant_value_t wire_read_props(wire_reader_t *r, ant_value_t obj) {
  ant_t *js = r->js;
  
  for (;;) {
    uint8_t tag;
    
    if (!get_u8(r, &tag)) return wire_corrupt(js);
    if (tag == WIRE_END) return obj;
    if (tag != WIRE_PROP) return wire_corrupt(js);

    const char *key;
    size_t key_len;
    if (!get_str(r, &key, &key_len)) return wire_corrupt(js);
    
    ant_value_t value = wire_read(r);
    if (is_err(value)) return value;
    
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, value);
    
    ant_value_t defined = js_mkprop_fast(js, obj, key, key_len, value);
    GC_ROOT_RESTORE(js, mark);
    
    if (is_err(defined)) return defined;
  }
}

static ant_value_t wire_read_primitive_tag(wire_reader_t *r, uint8_t tag) {
  ant_t *js = r->js;
  switch (tag) {
    case WIRE_UNDEFINED: return js_mkundef();
    case WIRE_NULL: return js_mknull();
    case WIRE_TRUE: return js_true;
    case WIRE_FALSE: return js_false;
    case WIRE_NUMBER: {
      double d;
      return get_f64(r, &d) ? js_mknum(d) : wire_corrupt(js);
    }
    case WIRE_STRING: {
      const char *s;
      size_t len;
      return get_str(r, &s, &len) ? js_mkstr(js, s, len) : wire_corrupt(js);
    }
    case WIRE_BIGINT: {
      const char *s;
      size_t len;
      if (!get_str(r, &s, &len)) return wire_corrupt(js);
      bool negative = len > 0 && s[0] == '-';
      return js_mkbigint(js, s + negative, len - negative, negative);
    }
    default: return wire_corrupt(js);
  }
}

static ant_value_t wire_read_error(wire_reader_t *r) {
  ant_t *js = r->js;
  const char *name, *message = NULL, *stack = NULL;
  size_t name_len, message_len = 0, stack_len = 0;
  uint8_t has;

  if (!get_str(r, &name, &name_len)) return wire_corrupt(js);
  if (!get_u8(r, &has) || (has && !get_str(r, &message, &message_len))) return wire_corrupt(js);
  if (!get_u8(r, &has) || (has && !get_str(r, &stack, &stack_len))) return wire_corrupt(js);

  js_err_type_t type = JS_ERR_GENERIC;
  for (js_err_type_t t = JS_ERR_TYPE; t < JS_ERR_TYPE_COUNT; t++) {
    const char *tn = js_err_type_name(t);
    if (tn && strlen(tn) == name_len && memcmp(tn, name, name_len) == 0) type = t;
  }

  ant_value_t err = Ant_Object_CreateBare(js);
  if (is_err(err)) return err;
  js_set_proto_init(err, js->builtins.error_protos[type]);
  wire_track(r, err);

  if (stack) js_error_define_stack(js, err, js_mkundef(), js_mkstr(js, stack, stack_len));
  if (message) defhidden(js, err, "message", 7, js_mkstr(js, message, message_len));
  js_set_slot(err, SLOT_ERROR_BRAND, js_true);
  js_set_slot(err, SLOT_ERR_TYPE, js_mknum((double)type));
  if (type == JS_ERR_GENERIC && !(name_len == 5 && memcmp(name, "Error", 5) == 0))
    defhidden(js, err, "name", 4, js_mkstr(js, name, name_len));

  if (!get_u8(r, &has)) return wire_corrupt(js);
  if (has) {
    ant_value_t cause = wire_read(r);
    if (is_err(cause)) return cause;
    defhidden(js, err, "cause", 5, cause);
  }

  if (!get_u8(r, &has)) return wire_corrupt(js);
  if (has) {
    ant_value_t errors = wire_read(r);
    if (is_err(errors)) return errors;
    defhidden(js, err, "errors", 6, errors);
  }

  return wire_read_props(r, err);
}

static ant_value_t wire_read_view(wire_reader_t *r) {
  ant_t *js = r->js;
  uint8_t kind, tracking;
  uint64_t offset, length;
  if (!get_u8(r, &kind) || !get_u8(r, &tracking) || !get_varint(r, &offset) || !get_varint(r, &length)) return wire_corrupt(js);

  ant_value_t buffer_obj = wire_read(r);
  if (is_err(buffer_obj)) return buffer_obj;
  ArrayBufferData *buffer = buffer_get_arraybuffer_data(buffer_obj);
  if (!buffer) return wire_corrupt(js);

  if (kind == WIRE_VIEW_DATAVIEW) {
    if (offset + length > buffer->length) return wire_corrupt(js);
    return wire_track(r, create_dataview_with_buffer(js, buffer, (size_t)offset, (size_t)length, buffer_obj, tracking != 0));
  }

  if (kind >= TYPED_ARRAY_COUNT) return wire_corrupt(js);
  return wire_track(r, create_typed_array_with_buffer(
    js, (TypedArrayType)kind, buffer, (size_t)offset, (size_t)length,
    js->builtins.typedarray_protos[kind], buffer_obj, tracking != 0
  ));
}

static ant_value_t wire_read_object(wire_reader_t *r, uint8_t tag) {
  ant_t *js = r->js;

  switch (tag) {
    case WIRE_REF: {
      uint64_t id;
      if (!get_varint(r, &id) || id >= (uint64_t)js_arr_len(js, r->refs)) return wire_corrupt(js);
      return js_arr_get(js, r->refs, (ant_offset_t)id);
    }

    case WIRE_OBJECT: {
      ant_value_t obj = wire_track(r, Ant_Object_Create(js));
      return is_err(obj) ? obj : wire_read_props(r, obj);
    }

    case WIRE_ARRAY: {
      uint64_t len;
      if (!get_varint(r, &len)) return wire_corrupt(js);
      ant_value_t arr = wire_track(r, js_mkarr(js));
      if (is_err(arr)) return arr;
      for (uint64_t i = 0; i < len; i++) {
        ant_value_t item = wire_read(r);
        if (is_err(item)) return item;
        js_arr_push(js, arr, item);
      }
      return wire_read_props(r, arr);
    }

    case WIRE_BOXED: {
      uint8_t inner;
      if (!get_u8(r, &inner)) return wire_corrupt(js);
      ant_value_t prim = wire_read_primitive_tag(r, inner);
      if (is_err(prim)) return prim;
      return wire_track(r, js_normalize_sloppy_this(js, prim));
    }

    case WIRE_DATE: {
      double ms;
      return get_f64(r, &ms) ? wire_track(r, js_mkdate(js, ms)) : wire_corrupt(js);
    }

    case WIRE_REGEXP: {
      const char *s, *f;
      size_t slen, flen;
      if (!get_str(r, &s, &slen) || !get_str(r, &f, &flen)) return wire_corrupt(js);
      ant_value_t source = js_mkstr(js, s, slen);
      GC_ROOT_SAVE(mark, js);
      GC_ROOT_PIN(js, source);
      ant_value_t flags = js_mkstr(js, f, flen);
      GC_ROOT_PIN(js, flags);
      ant_value_t re = regexp_create_literal(js, source, flags);
      GC_ROOT_RESTORE(js, mark);
      return wire_track(r, re);
    }

    case WIRE_MAP: {
      map_entry_t **head = NULL;
      ant_value_t map = wire_track(r, collections_map_create(js, &head));
      if (is_err(map)) return map;
      for (;;) {
        uint8_t t;
        if (!get_u8(r, &t)) return wire_corrupt(js);
        if (t == WIRE_END) return map;
        if (t != WIRE_PROP) return wire_corrupt(js);
        ant_value_t key = wire_read(r);
        if (is_err(key)) return key;
        GC_ROOT_SAVE(mark, js);
        GC_ROOT_PIN(js, key);
        ant_value_t value = wire_read(r);
        bool ok = !is_err(value) && collections_map_store_cloned_entry(js, head, key, value);
        GC_ROOT_RESTORE(js, mark);
        if (is_err(value)) return value;
        if (!ok) return wire_oom(js);
      }
    }

    case WIRE_SET: {
      set_entry_t **head = NULL;
      ant_value_t set = wire_track(r, collections_set_create(js, &head));
      if (is_err(set)) return set;
      for (;;) {
        uint8_t t;
        if (!get_u8(r, &t)) return wire_corrupt(js);
        if (t == WIRE_END) return set;
        if (t != WIRE_PROP) return wire_corrupt(js);
        ant_value_t value = wire_read(r);
        if (is_err(value)) return value;
        if (!collections_set_store_cloned_entry(js, head, value)) return wire_oom(js);
      }
    }

    case WIRE_ERROR: return wire_read_error(r);

    case WIRE_ARRAY_BUFFER: {
      uint64_t len;
      const uint8_t *bytes;
      if (!get_varint(r, &len) || !get_bytes(r, &bytes, (size_t)len)) return wire_corrupt(js);
      return wire_track(r, create_arraybuffer_copy(js, bytes, (size_t)len));
    }

    case WIRE_RESIZABLE_BUFFER: {
      uint64_t max, len;
      const uint8_t *bytes;
      if (!get_varint(r, &max) || !get_varint(r, &len) || !get_bytes(r, &bytes, (size_t)len)) return wire_corrupt(js);
      return wire_track(r, create_resizable_arraybuffer_copy(js, bytes, (size_t)len, (size_t)max));
    }

    case WIRE_SHARED_BUFFER: {
      uint64_t index;
      if (!get_varint(r, &index) || index >= r->wire->shared_count) return wire_corrupt(js);
      return wire_track(r, create_shared_arraybuffer_obj(js, r->wire->shared[index]));
    }

    case WIRE_MOVED_BUFFER: {
      uint64_t index;
      if (!get_varint(r, &index) || index >= r->wire->moved_count) return wire_corrupt(js);
      return wire_track(r, create_arraybuffer_obj(js, r->wire->moved[index]));
    }

    case WIRE_VIEW: return wire_read_view(r);

    case WIRE_BLOB: {
      const char *type;
      size_t type_len;
      
      uint64_t index;
      uint8_t named;
      
      if (
        !get_str(r, &type, &type_len) || !get_varint(r, &index) || index >= r->wire->blob_count || !get_u8(r, &named))
        return wire_corrupt(js);
        
      char *type_z = strndup(type, type_len);
      if (!type_z) return wire_oom(js);
      
      ant_value_t blob = blob_create_shared(js, r->wire->blobs[index], type_z);
      free(type_z);
      
      if (is_err(blob)) return blob;
      
      if (named) {
        const char *name;
        size_t name_len;
        double modified;
        
        if (!get_str(r, &name, &name_len) || !get_f64(r, &modified)) return wire_corrupt(js);
        blob_data_t *bd = blob_get_data(blob);
        
        if (bd) {
          bd->name = strndup(name, name_len);
          bd->last_modified = (int64_t)modified;
        }
        
        js_set_proto_init(blob, js->builtins.file_proto);
      }
      
      return wire_track(r, blob);
    }

    case WIRE_PORT: {
      uint64_t index;
      if (!get_varint(r, &index) || vtype(r->ports) != kTypeArray || index >= (uint64_t)js_arr_len(js, r->ports))
        return wire_corrupt(js);
      return wire_track(r, js_arr_get(js, r->ports, (ant_offset_t)index));
    }

    default: return wire_read_primitive_tag(r, tag);
  }
}

static ant_value_t wire_read(wire_reader_t *r) {
  uint8_t tag;
  if (!get_u8(r, &tag)) return wire_corrupt(r->js);
  if (++r->depth > WIRE_MAX_DEPTH) return Ant_Error_CreateStackOverflow(r->js);
  ant_value_t value = wire_read_object(r, tag);
  r->depth--;
  return value;
}

ant_value_t ant_wire_deserialize(ant_t *js, ant_wire_t *wire, ant_value_t *ports_out) {
  GC_ROOT_SAVE(root_mark, js);
  wire_reader_t r = {
    .js = js, .wire = wire,
    .p = wire->data, .end = wire->data + wire->len,
    .refs = js_mkarr(js), .ports = js_mkarr(js),
  };
  GC_ROOT_PIN(js, r.refs);
  GC_ROOT_PIN(js, r.ports);

  if (wire->port_count) {
    for (uint32_t i = 0; i < wire->port_count; i++) {
      ant_port_end_t *end = wire->ports[i];
      wire->ports[i] = NULL;
      ant_value_t port = end ? ant_message_port_adopt(js, end) : js_mknull();
      if (is_err(port)) {
        GC_ROOT_RESTORE(js, root_mark);
        return port;
      }
      js_arr_push(js, r.ports, port);
    }
    wire->port_count = 0;
  }

  ant_value_t value = wire_read(&r);
  if (ports_out) *ports_out = r.ports;
  GC_ROOT_RESTORE(js, root_mark);
  return value;
}

static ant_value_t wire_weakset_add(ant_t *js, ant_value_t *root, ant_value_t value) {
  if (!is_object_type(*root)) {
    ant_value_t ctor = js_get(js, js->global, "WeakSet");
    ant_value_t set = sv_vm_call(js->vm, js, ctor, js_mkundef(), NULL, 0, NULL, ctor);
    if (is_err(set)) return set;
    *root = set;
  }
  
  ant_value_t add = js_get(js, *root, "add");
  ant_value_t r = sv_vm_call(js->vm, js, add, *root, &value, 1, NULL, js_mkundef());
  
  return is_err(r) ? r : js_mkundef();
}

static bool wire_weakset_has(ant_t *js, ant_value_t root, ant_value_t value) {
  if (!is_object_type(root) || !is_object_type(value)) return false;
  
  ant_value_t has = js_get(js, root, "has");
  ant_value_t r = sv_vm_call(js->vm, js, has, root, &value, 1, NULL, js_mkundef());
  
  if (is_err(r)) {
    js_take_thrown(js, r);
    return false;
  }
  
  return js_truthy(js, r);
}

ant_value_t ant_wire_mark_untransferable(ant_t *js, ant_value_t value) {
  ArrayBufferData *data = is_object_type(value) ? buffer_get_arraybuffer_data(value) : NULL;
  TypedArrayData *ta = !data && is_object_type(value) ? buffer_get_typedarray_data(value) : NULL;
  
  if (ta) data = ta->backing;
  if (data) data->untransferable = 1;
  if (!is_object_type(value)) return js_mkundef();
  
  return wire_weakset_add(js, &js->mutable_roots.untransferable, value);
}

bool ant_wire_is_untransferable(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return false;
  
  ArrayBufferData *data = buffer_get_arraybuffer_data(value);
  TypedArrayData *ta = !data ? buffer_get_typedarray_data(value) : NULL;
  
  if (ta) data = ta->backing;
  if (data && data->untransferable) return true;
  
  return wire_weakset_has(js, js->mutable_roots.untransferable, value);
}

ant_value_t ant_wire_mark_uncloneable(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return js_mkundef();
  js_set_slot(value, SLOT_UNCLONEABLE, js_true);
  js->uncloneable_marked = true;
  return js_mkundef();
}

bool ant_wire_is_uncloneable(ant_t *js, ant_value_t value) {
  return js->uncloneable_marked && js_get_slot(value, SLOT_UNCLONEABLE) == js_true;
}
