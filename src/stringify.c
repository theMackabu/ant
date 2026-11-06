#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ant.h"
#include "internal.h"
#include "stringify.h"
#include "builder.h"
#include "utils.h"
#include "cfunc.h"
#include "numbers.h"
#include "silver/call.h"
#include "modules/collections.h"
#include "modules/bigint.h"
#include "modules/date.h"
#include "arrays.h"
#include "convert.h"
#include "props.h"

static size_t stringify_hex_bytes(const uint8_t *data, size_t count, bool lead_space, char *buf, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < count; i++) {
    if (lead_space || i > 0) n += cpy(buf + n, Ant_Remaining(n, len), " ", 1);
    char hex[2];
    hex_encode(data + i, 1, hex);
    n += cpy(buf + n, Ant_Remaining(n, len), hex, 2);
  }
  return n;
}

static size_t stringify_typed_element(ant_t *js, const TypedArrayData *ta, size_t i, char *buf, size_t len) {
  ant_value_t value;
  if (!buffer_typedarray_data_read_index(js, ta, i, &value)) return 0;
  if (vtype(value) == kTypeNumber && Ant_Value_AsNumberUnchecked(value) == 0 && signbit(Ant_Value_AsNumberUnchecked(value))) return cpy(buf, len, "-0", 2);
  return tostr(js, value, buf, len);
}

static bool stringify_is_buffer(ant_t *js, ant_value_t obj) {
  ant_value_t proto = js_get_proto(js, obj);
  ant_value_t buffer_proto = js->builtins.buffer_proto;
  return vtype(proto) == kTypeObject && vtype(buffer_proto) == kTypeObject && vdata(proto) == vdata(buffer_proto);
}

static size_t stringify_more_bytes(size_t remaining, char *buf, size_t len) {
  return (size_t)snprintf(buf, len, " ... %zu more byte%s", remaining, remaining == 1 ? "" : "s");
}

static size_t stringify_node_buffer(const uint8_t *data, size_t byte_length, char *buf, size_t len) {
  size_t shown = byte_length < BUFFER_INSPECT_MAX_BYTES ? byte_length : BUFFER_INSPECT_MAX_BYTES;
  size_t n = cpy(buf, len, "<Buffer", 7);
  n += stringify_hex_bytes(data, shown, true, buf + n, Ant_Remaining(n, len));
  if (shown < byte_length) n += stringify_more_bytes(byte_length - shown, buf + n, Ant_Remaining(n, len));
  return n + cpy(buf + n, Ant_Remaining(n, len), shown == 0 ? " >" : ">", shown == 0 ? 2 : 1);
}

size_t stringify_typedarray(ant_t *js, ant_value_t obj, const TypedArrayData *ta, char *buf, size_t len) {
  const uint8_t *data; size_t byte_length;
  bool detached = !Ant_Buffer_GetViewBytes(ta, &data, &byte_length);
  size_t length = detached ? 0 : ta->length;

  if (stringify_is_buffer(js, obj)) return stringify_node_buffer(data, byte_length, buf, len);
  
  const char *name = ta->type < TYPED_ARRAY_COUNT ? buffer_typedarray_type_name(ta->type) : "TypedArray";
  size_t n = cpy(buf, len, name, strlen(name));
  n += (size_t)snprintf(buf + n, Ant_Remaining(n, len), "(%zu) [", length);
  if (length == 0) return n + cpy(buf + n, Ant_Remaining(n, len), "]", 1);
  
  size_t shown = length < ANT_INSPECT_MAX_ENTRIES ? length : ANT_INSPECT_MAX_ENTRIES;
  for (size_t i = 0; i < shown; i++) {
    n += cpy(buf + n, Ant_Remaining(n, len), i ? ", " : " ", i ? 2 : 1);
    n += stringify_typed_element(js, ta, i, buf + n, Ant_Remaining(n, len));
  }
  
  if (shown < length) n += cpy(buf + n, Ant_Remaining(n, len), ", ...", 5);
  return n + cpy(buf + n, Ant_Remaining(n, len), " ]", 2);
}

static size_t stringify_arraybuffer_body(const ArrayBufferData *ab, const char *indent, char *buf, size_t len) {
  const char *name = ab->is_shared ? "SharedArrayBuffer" : "ArrayBuffer";
  if (ab->is_detached) return (size_t)snprintf(buf, len, "%s { (detached), [byteLength]: 0 }", name);
  
  size_t shown = ab->length < ANT_INSPECT_MAX_ENTRIES ? ab->length : ANT_INSPECT_MAX_ENTRIES;
  size_t n = (size_t)snprintf(buf, len, "%s {\n%s  [Uint8Contents]: <", name, indent);
  
  if (ab->data) n += stringify_hex_bytes(ab->data, shown, false, buf + n, Ant_Remaining(n, len));
  if (shown < ab->length) n += stringify_more_bytes(ab->length - shown, buf + n, Ant_Remaining(n, len));
  
  return n + (size_t)snprintf(buf + n, Ant_Remaining(n, len), ">,\n%s  [byteLength]: %zu\n%s}", indent, ab->length, indent);
}

size_t stringify_arraybuffer(const ArrayBufferData *ab, char *buf, size_t len) {
  return stringify_arraybuffer_body(ab, "", buf, len);
}

size_t stringify_dataview(const DataViewData *dv, char *buf, size_t len) {
  size_t n = !Ant_Buffer_ViewIsLive(dv)
    ? (size_t)snprintf(buf, len, "DataView {\n  [byteLength]: 0,\n  [byteOffset]: undefined,\n  [buffer]: ")
    : (size_t)snprintf(
      buf, len, "DataView {\n  [byteLength]: %zu,\n  [byteOffset]: %zu,\n  [buffer]: ",
      dv->byte_length, dv->byte_offset
    );
  
  n += stringify_arraybuffer_body(dv->buffer, "  ", buf + n, Ant_Remaining(n, len));
  return n + cpy(buf + n, Ant_Remaining(n, len), "\n}", 2);
}

size_t stringify_blob(const blob_data_t *bd, bool is_file, char *buf, size_t len) {
  size_t n = (size_t)snprintf(buf, len, "%s { size: %zu, type: '%s'", is_file ? "File" : "Blob", bd->size, bd->type ? bd->type : "");
  if (is_file) n += (size_t)snprintf(
    buf + n, Ant_Remaining(n, len), ", name: '%s', lastModified: %" PRId64,
    bd->name ? bd->name : "", bd->last_modified
  );
  
  return n + cpy(buf + n, Ant_Remaining(n, len), " }", 2);
}

static int find_multiref(ant_t *js, ant_value_t obj) {
  for (int i = 0; i < js->stringify->multiref_count; i++)
    if (js->stringify->multiref_objs[i] == obj) return js->stringify->multiref_ids[i];
  return 0;
}

static bool is_on_stack(ant_t *js, ant_value_t obj) {
  for (int i = 0; i < js->stringify->depth; i++)
    if (js->stringify->stack[i] == obj) return true;
  return false;
}

static void mark_multiref(ant_t *js, ant_value_t obj) {
  if (find_multiref(js, obj)) return;
  
  if (js->stringify->multiref_count < MAX_MULTIREF_OBJS) {
    js->stringify->multiref_objs[js->stringify->multiref_count] = obj;
    js->stringify->multiref_ids[js->stringify->multiref_count] = ++js->stringify->multiref_next_id;
    js->stringify->multiref_count++;
  }
}

static void stringify_begin_pass(ant_t *js) {
  js->stringify->depth = 0;
  js->stringify->indent = 0;
  js->stringify->multiref_count = 0;
  js->stringify->multiref_next_id = 0;
}

static size_t stringify_ref_label(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  int id = find_multiref(js, obj);
  if (!id) return n;

  char label[24];
  size_t label_len = (size_t)snprintf(label, sizeof(label), "<ref *%d> ", id);
  if (len == 0) return n + label_len;

  size_t cap = len - 1;
  size_t have = n < cap ? n : cap;
  if (label_len < cap) memmove(buf + label_len, buf, have < cap - label_len ? have : cap - label_len);
  memcpy(buf, label, label_len < cap ? label_len : cap);

  size_t total = n + label_len;
  buf[total < cap ? total : cap] = '\0';
  return total;
}

static bool stringify_has_shown_props(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->shape) return false;
  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++)
    if (ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE) return true;
  return false;
}

static bool stringify_too_deep(ant_t *js) {
  return js->stringify->depth - 1 > js->stringify->max_depth;
}

static size_t stringify_depth_marker(ant_t *js, ant_value_t obj, const char *fallback, char *buf, size_t len) {
  if (vtype(get_proto(js, obj)) == kTypeNull) return cpy(buf, len, "[Object: null prototype]", 24);
  ant_offset_t name_len = 0;
  const char *name = get_class_name(js, obj, &name_len, NULL);
  if (!name) { name = fallback; name_len = (ant_offset_t)strlen(fallback); }
  size_t n = cpy(buf, len, "[", 1);
  n += cpy(buf + n, Ant_Remaining(n, len), name, name_len);
  return n + cpy(buf + n, Ant_Remaining(n, len), "]", 1);
}

static int get_circular_ref(ant_t *js, ant_value_t obj) {
  if (!is_on_stack(js, obj)) return 0;
  mark_multiref(js, obj);
  int ref = find_multiref(js, obj);
  return ref ? ref : -1;
}

static void push_stringify(ant_t *js, ant_value_t obj) {
  if (js->stringify->depth < MAX_STRINGIFY_DEPTH)
    js->stringify->stack[js->stringify->depth++] = obj;
}

static void pop_stringify(ant_t *js) {
  if (js->stringify->depth > 0) js->stringify->depth--;
}

static size_t add_indent(char *buf, size_t len, int level) {
  size_t wanted = (size_t)(level * 2);
  size_t n = 0;
  for (int i = 0; i < level * 2 && n < len; i++) {
    buf[n++] = ' ';
  }
  return wanted;
}

const char *get_str_prop(ant_t *js, ant_value_t obj, const char *key, ant_offset_t klen, ant_offset_t *out_len) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  ant_value_t v = js_getprop_fallback_len(js, obj, key, (size_t)klen);
  
  GC_ROOT_PIN(js, v);
  if (vtype(v) != kTypeString) {
    GC_ROOT_RESTORE(js, root_mark);
    return NULL;
  }
  
  const char *str = (const char *)(uintptr_t)(vstr(js, v, out_len));
  GC_ROOT_RESTORE(js, root_mark);
  
  return str;
}

static ant_value_t get_obj_ctor(ant_t *js, ant_value_t obj) {
  ant_value_t ctor = js_get_slot(obj, SLOT_CTOR);
  if (vtype(ctor) == kTypeFunction) return ctor;
  ant_value_t proto = js_get_slot(obj, SLOT_PROTO);
  if (vtype(proto) != kTypeObject) return Ant_Value_Undefined;
  return lkp_interned_val(js, proto, js->intern.constructor);
}

const char *get_func_name(ant_t *js, ant_value_t func, ant_offset_t *out_len) {
  if (vtype(func) == kTypeBuiltin) {
    const ant_cfunc_meta_t *meta = js_as_cfunc_meta(func);
    if (!meta || !meta->name) return NULL;
    if (out_len) *out_len = (ant_offset_t)strlen(meta->name);
    return meta->name;
  }

  if (vtype(func) != kTypeFunction) return NULL;
  ant_value_t func_obj = js_func_obj(func);
  ant_value_t name = lkp_val(js, func_obj, "name", 4);
  
  if (vtype(name) == kTypeString) {
    ant_offset_t str_off = vstr(js, name, out_len);
    return (const char *)(uintptr_t)(str_off);
  }

  ant_value_t cfunc_slot = js_get_slot(func_obj, SLOT_CFUNC);
  if (vtype(cfunc_slot) == kTypeBuiltin) return get_func_name(js, cfunc_slot, out_len);
  
  return NULL;
}

bool js_cfunc_try_get_own(ant_t *js, ant_value_t cfunc, const char *key, size_t key_len, ant_value_t *out) {
  if (key_len == 4 && memcmp(key, "name", 4) == 0) {
    ant_value_t name = js_cfunc_name_value(js, cfunc);
    if (vtype(name) != kTypeUndefined) {
      *out = name;
      return true;
    }
    return false;
  }

  if (key_len == 6 && memcmp(key, "length", 6) == 0) {
    *out = js_cfunc_length_value(cfunc);
    return true;
  }

  if (key_len == 9 && memcmp(key, "prototype", 9) == 0 && js_cfunc_has_prototype(cfunc)) {
    ant_value_t promoted = js_cfunc_promote(js, cfunc);
    if (is_err(promoted)) {
      *out = promoted;
      return true;
    }
    *out = js_get(js, promoted, "prototype");
    return true;
  }

  return false;
}

const char *get_class_name(ant_t *js, ant_value_t obj, ant_offset_t *out_len, const char *skip) {
  const char *name = get_func_name(js, get_obj_ctor(js, obj), out_len);
  if (!name) return NULL;
  if (skip && *out_len == (ant_offset_t)strlen(skip) && memcmp(name, skip, *out_len) == 0) return NULL;
  return name;
}

ant_offset_t dense_iterable_length(ant_t *js, ant_value_t obj) {
  ant_offset_t doff = get_dense_buf(obj);
  if (!doff) return 0;
  ant_offset_t dense_len = dense_capacity(doff);
  ant_offset_t semantic_len = get_array_length(js, obj);
  return dense_len < semantic_len ? dense_len : semantic_len;
}

typedef struct {
  ant_offset_t index;
  ant_value_t value;
  const char *accessor;
} arr_sparse_entry_t;

typedef bool (*arr_entry_fn)(
  ant_t *js, void *ctx, ant_offset_t index, 
  ant_offset_t holes, ant_value_t value, const char *accessor
);

static const char *const arr_accessor_labels[] = {
  NULL, "[Getter]", "[Setter]", "[Getter/Setter]",
};

static int arr_sparse_entry_cmp(const void *a, const void *b) {
  ant_offset_t x = ((const arr_sparse_entry_t *)a)->index;
  ant_offset_t y = ((const arr_sparse_entry_t *)b)->index;
  return x < y ? -1 : x > y;
}

static void arr_for_each_entry(ant_t *js, ant_value_t obj, arr_entry_fn fn, void *ctx) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  ant_offset_t length = get_array_length(js, obj);
  ant_offset_t d_len = dense_iterable_length(js, obj);
  ant_offset_t iter_len = d_len < length ? d_len : length;
  ant_offset_t doff = get_dense_buf(obj);

  arr_sparse_entry_t *sparse = NULL;
  size_t sparse_len = 0, sparse_cap = 0;
  uint32_t shape_count = ptr && ptr->shape ? ant_shape_count(ptr->shape) : 0;
  
  for (uint32_t i = 0; i < shape_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
    
    const char *key = prop->key.interned;
    unsigned long idx;
    if (!parse_array_index(key, intern_length(key), length, &idx)) continue;
    
    if (sparse_len == sparse_cap) {
      size_t cap = sparse_cap ? sparse_cap * 2 : 8;
      arr_sparse_entry_t *grown = realloc(sparse, cap * sizeof(*grown));
      if (!grown) break;
      sparse = grown;
      sparse_cap = cap;
    }
    
    sparse[sparse_len++] = (arr_sparse_entry_t){
      (ant_offset_t)idx,
      i < ptr->prop_count ? ant_object_prop_get_unchecked(ptr, i) : Ant_Value_Undefined,
      arr_accessor_labels[(prop->has_getter ? 1 : 0) | (prop->has_setter ? 2 : 0)],
    };
  }
  
  if (sparse_len > 1) qsort(sparse, sparse_len, sizeof(*sparse), arr_sparse_entry_cmp);

  size_t k = 0;
  ant_offset_t pos = 0, holes = 0;
  
  while (pos < length) {
    while (k < sparse_len && sparse[k].index < pos) k++;
    
    ant_value_t value = T_EMPTY;
    const char *accessor = NULL;
    if (pos < iter_len && doff) value = dense_get(doff, pos);
    if (is_empty_slot(value) && k < sparse_len && sparse[k].index == pos) {
      value = sparse[k].accessor ? Ant_Value_Undefined : sparse[k].value;
      accessor = sparse[k].accessor;
    }
    
    if (!is_empty_slot(value)) {
      if (holes && !fn(js, ctx, pos - holes, holes, Ant_Value_Undefined, NULL)) goto done;
      holes = 0;
      if (!fn(js, ctx, pos, 0, value, accessor)) goto done;
      pos++;
      continue;
    }
    
    ant_offset_t next = pos + 1;
    if (pos >= iter_len) next = k < sparse_len ? sparse[k].index : length;
    holes += next - pos;
    pos = next;
  }
  
  if (holes) fn(js, ctx, length - holes, holes, Ant_Value_Undefined, NULL);

done:
  free(sparse);
}

typedef struct {
  int count;
  bool nested;
} arr_small_ctx_t;

static bool arr_small_entry(
  ant_t *js, void *ctx, ant_offset_t index, 
  ant_offset_t holes, ant_value_t value, const char *accessor
) {
  arr_small_ctx_t *small = ctx;
  uint8_t t = vtype(value);
  if (!holes && (t == kTypeObject || t == kTypeArray || t == kTypeFunction)) small->nested = true;
  return ++small->count <= 4 && !small->nested;
}

static bool is_small_array(ant_t *js, ant_value_t obj) {
  if (js->stringify && (js->stringify->flags & ANT_INSPECT_EXPAND)) return false;
  arr_small_ctx_t small = {0};
  arr_for_each_entry(js, obj, arr_small_entry, &small);
  return small.count <= 4 && !small.nested;
}

typedef struct {
  char *buf;
  size_t len;
  size_t n;
  bool inline_mode;
  bool printed_first;
} arr_print_ctx_t;

static bool arr_print_entry(
  ant_t *js, void *ctx, ant_offset_t index, 
  ant_offset_t holes, ant_value_t value, const char *accessor
) {
  arr_print_ctx_t *p = ctx;
  if (p->printed_first) p->n += cpy(p->buf + p->n, Ant_Remaining(p->n, p->len), p->inline_mode ? ", " : ",\n", 2);
  if (!p->inline_mode) p->n += add_indent(p->buf + p->n, Ant_Remaining(p->n, p->len), js->stringify->indent);
  
  if (holes) p->n += (size_t) snprintf(
    p->buf + p->n, Ant_Remaining(p->n, p->len), "<%llu empty item%s>", 
    (unsigned long long)holes, holes == 1 ? "" : "s"
  );
  
  else if (accessor) p->n += cpy(p->buf + p->n, Ant_Remaining(p->n, p->len), accessor, strlen(accessor));
  else p->n += tostr(js, value, p->buf + p->n, Ant_Remaining(p->n, p->len));
    
  p->printed_first = true;
  return true;
}

static size_t strarr_body(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  ant_offset_t length = get_array_length(js, obj);
  
  ant_offset_t d_len = dense_iterable_length(js, obj);
  ant_offset_t iter_len = (d_len >= length) ? length : d_len;
  
  bool arguments = ptr && ptr->flags.arguments_object;
  ant_offset_t class_len = 0;
  const char *class_name = arguments ? NULL : get_class_name(js, obj, &class_len, "Array");
  
  bool hidden_nested = false;
  int hidden = js_inspect_hidden_count(js, obj, &hidden_nested);
  bool inline_mode = is_small_array(js, obj) && !hidden_nested;
  size_t n = 0;
  
  if (arguments) n += cpy(buf + n, Ant_Remaining(n, len), "[Arguments] ", 12);
  if (class_name) {
    n += cpy(buf + n, Ant_Remaining(n, len), class_name, class_len);
    n += (size_t) snprintf(buf + n, Ant_Remaining(n, len), "(%u) ", (unsigned) length);
  }
  
  char open = arguments ? '{' : '[';
  char close = arguments ? '}' : ']';
  if (length == 0 && hidden == 0 && !(arguments && stringify_has_shown_props(obj)))
    return n + (size_t) snprintf(buf + n, Ant_Remaining(n, len), "%c%c", open, close);
  
  n += (size_t) snprintf(buf + n, Ant_Remaining(n, len), inline_mode ? "%c " : "%c\n", open);
  if (!inline_mode) js->stringify->indent++;
  
  bool printed_first = false;
  if (arguments) for (ant_offset_t i = 0; i < iter_len; i++) {
    if (!arr_has(js, obj, i)) continue;
    if (printed_first) n += cpy(buf + n, Ant_Remaining(n, len), inline_mode ? ", " : ",\n", 2);
    if (!inline_mode) n += add_indent(buf + n, Ant_Remaining(n, len), js->stringify->indent);

    n += (size_t) snprintf(buf + n, Ant_Remaining(n, len), "'%u': ", (unsigned) i);
    n += tostr(js, arr_get(js, obj, i), buf + n, Ant_Remaining(n, len));
    printed_first = true;
  } else {
    arr_print_ctx_t print = { buf, len, n, inline_mode, false };
    arr_for_each_entry(js, obj, arr_print_entry, &print);
    n = print.n;
    printed_first = print.printed_first;
  }

  if (ptr && ptr->shape) {
    uint32_t shape_count = ant_shape_count(ptr->shape);
    for (uint32_t i = 0; i < shape_count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
      
      const char *key = prop->key.interned;
      ant_offset_t klen = (ant_offset_t)intern_length(key);
      
      if (is_length_key(key, klen)) continue;
      if (!(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) continue;
      if (!arguments && is_array_index(key, klen)) continue;
      
      if (printed_first) n += cpy(buf + n, Ant_Remaining(n, len), inline_mode ? ", " : ",\n", 2);
      if (!inline_mode) n += add_indent(buf + n, Ant_Remaining(n, len), js->stringify->indent);
      
      ant_value_t val = (i < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, i) : Ant_Value_Undefined;
      if (arguments && is_array_index(key, klen)) {
        n += (size_t) snprintf(buf + n, Ant_Remaining(n, len), "'%.*s': ", (int)klen, key);
        n += tostr(js, val, buf + n, Ant_Remaining(n, len));
      } else {
        n += cpy(buf + n, Ant_Remaining(n, len), key, klen);
        n += cpy(buf + n, Ant_Remaining(n, len), ": ", 2);
        n += tostr(js, val, buf + n, Ant_Remaining(n, len));
      }
      
      printed_first = true;
    }
  }

  if (hidden) {
    js_inspect_builder_t hidden_builder;
    js_inspect_builder_init_fixed(&hidden_builder, js, buf, len, n);
    hidden_builder.inline_mode = inline_mode;
    hidden_builder.first = !printed_first;
    hidden_builder.did_indent = true;
    js_inspect_hidden_props(&hidden_builder, obj);
    n = hidden_builder.n;
  }
  
  if (!inline_mode) {
    js->stringify->indent--;
    n += cpy(buf + n, Ant_Remaining(n, len), "\n", 1);
    n += add_indent(buf + n, Ant_Remaining(n, len), js->stringify->indent);
  }
  
  return n + (size_t) snprintf(buf + n, Ant_Remaining(n, len), inline_mode ? " %c" : "%c", close);
}

static size_t strarr(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  int ref = get_circular_ref(js, obj);
  if (ref) return ref > 0 ? (size_t) snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);

  push_stringify(js, obj);
  bool arguments = js_obj_ptr(js_as_obj(obj))->flags.arguments_object;
  bool has_contents = get_array_length(js, obj) > 0 || stringify_has_shown_props(obj);
  
  size_t n = stringify_too_deep(js) && has_contents
    ? stringify_depth_marker(js, obj, arguments ? "Arguments" : "Array", buf, len)
    : strarr_body(js, obj, buf, len);
  
  pop_stringify(js);
  return stringify_ref_label(js, obj, buf, len, n);
}

static size_t strdate(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  ant_value_t time_val = js_get_slot(obj, SLOT_DATA);
  if (vtype(time_val) != kTypeNumber || !isfinite(Ant_Value_AsNumberUnchecked(time_val)))
    return cpy(buf, len, "Invalid Date", 12);

  static const date_string_spec_t kSpec = {DATE_STRING_FMT_ISO, DATE_STRING_PART_ALL};
  ant_value_t iso = get_date_string(js, obj, kSpec);
  if (is_err(iso) || vtype(iso) != kTypeString) return cpy(buf, len, "Invalid Date", 12);

  ant_offset_t slen;
  ant_offset_t soff = vstr(js, iso, &slen);
  
  return cpy(buf, len, (const char *)(uintptr_t)(soff), slen);
}

static size_t strobj(ant_t *js, ant_value_t obj, char *buf, size_t len);

static size_t strobj_host_proxy(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t copy = Ant_Object_CreateBare(js);
  
  GC_ROOT_PIN(js, copy);
  (void)js_copy_data_props(js, copy, obj, NULL, 0);
  
  size_t n = strobj(js, copy, buf, len);
  GC_ROOT_RESTORE(js, root_mark);
  
  return n;
}

static size_t strobj_error(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_error(&builder, obj);
  return builder.n;
}

static size_t strobj_map(ant_t *js, ant_value_t obj, map_entry_t **map, char *buf, size_t len, size_t n) {
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_map(&builder, obj, map);
  return builder.n;
}

static size_t strobj_set(ant_t *js, ant_value_t obj, set_entry_t **set, char *buf, size_t len, size_t n) {
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_set(&builder, obj, set);
  return builder.n;
}

static ant_value_t strobj_inspect_tag(ant_t *js, ant_value_t obj) {
  ant_offset_t sym_off = (ant_offset_t)vdata(js->sym.toStringTag_sym);
  prop_meta_t meta;
  ant_value_t cur = obj;
  
  for (int hops = 0; is_object_type(cur) && !is_proxy(cur) && hops < 64; hops++) {
    if (lookup_symbol_prop_meta(js, cur, sym_off, &meta)) {
      if (cur == obj && meta.enumerable) return Ant_Value_Undefined;
      if (!meta.has_getter) {
        ant_prop_loc_t loc = lkp_sym(cur, sym_off);
        return loc.obj ? js_prop_load(loc) : Ant_Value_Undefined;
      }
      
      GC_ROOT_SAVE(mark, js);
      ant_value_t saved = Ant_Exception_Peek(js);
      GC_ROOT_PIN(js, saved);
      ant_value_t tag = js_call_getter(js, obj, meta.getter);
      if (is_err(tag) || Ant_Exception_Pending(js)) tag = Ant_Value_Undefined;
      Ant_Exception_Set(js, saved);
      GC_ROOT_RESTORE(js, mark);
      return tag;
    }
    cur = js_get_proto(js, cur);
  }
  
  return Ant_Value_Undefined;
}

static size_t strobj_trailing_props(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_trailing_props(&builder, obj);
  return builder.n;
}

static size_t strobj_inspect(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  ant_value_t tag = strobj_inspect_tag(js, obj);
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  
  bool ok;
  if (vtype(tag) == kTypeString) {
    ant_offset_t tag_len = 0;
    const char *tag_str = (const char *)(uintptr_t)vstr(js, tag, &tag_len);
    ok = js_inspect_tagged_header(&builder, obj, tag_str, (size_t)tag_len);
  } else ok = js_inspect_plain_header(&builder, obj);
  
  if (ok) ok = js_inspect_object_body(&builder, obj);
  if (ok) js_inspect_close(&builder);
  return builder.n;
}

static size_t strregexp(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  ant_value_t source = regexp_source_value(js, obj);
  ant_value_t flags = regexp_flags_value(js, obj);

  size_t source_len = 0, flags_len = 0;
  const char *source_str = vtype(source) == kTypeString ? js_getstr(js, source, &source_len) : "(?:)";
  const char *flags_str = vtype(flags) == kTypeString ? js_getstr(js, flags, &flags_len) : "";
  if (vtype(source) != kTypeString) source_len = 4;

  size_t n = cpy(buf, len, "/", 1);
  n += cpy(buf + n, Ant_Remaining(n, len), source_str, source_len);
  n += cpy(buf + n, Ant_Remaining(n, len), "/", 1);
  
  return n + cpy(buf + n, Ant_Remaining(n, len), flags_str, flags_len);
}

static size_t strobj_body(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  if (js_get_slot(obj, SLOT_ERROR_BRAND) == Ant_Value_True || vtype(js_get_slot(obj, SLOT_ERR_TYPE)) == kTypeNumber)
    return strobj_error(js, obj, buf, len, n);
  if (is_date_instance(obj)) return strobj_trailing_props(js, obj, buf, len, n + strdate(js, obj, buf + n, Ant_Remaining(n, len)));
  if (js_obj_ptr(obj)->flags.regexp_brand) return strobj_trailing_props(js, obj, buf, len, n + strregexp(js, obj, buf + n, Ant_Remaining(n, len)));
  
  TypedArrayData *ta = buffer_get_typedarray_data(obj);
  if (ta && ta->buffer) return n + stringify_typedarray(js, obj, ta, buf + n, Ant_Remaining(n, len));
  
  ArrayBufferData *ab = buffer_get_arraybuffer_data(obj);
  if (ab) return n + stringify_arraybuffer(ab, buf + n, Ant_Remaining(n, len));
  
  DataViewData *dv = buffer_get_dataview_data(obj);
  if (dv && dv->buffer) return n + stringify_dataview(dv, buf + n, Ant_Remaining(n, len));
  
  blob_data_t *bd = blob_get_data(obj);
  if (bd) return n + stringify_blob(bd, js_brand_id(obj) == BRAND_FILE, buf + n, Ant_Remaining(n, len));
  
  bool too_deep = stringify_too_deep(js);
  map_entry_t **map = get_map_from_obj(obj);
  if (map) return too_deep && *map ? n + cpy(buf + n, Ant_Remaining(n, len), "[Map]", 5) : strobj_map(js, obj, map, buf, len, n);
  
  set_entry_t **set = get_set_from_obj(obj);
  if (set) return too_deep && *set ? n + cpy(buf + n, Ant_Remaining(n, len), "[Set]", 5) : strobj_set(js, obj, set, buf, len, n);
  if (too_deep && stringify_has_shown_props(obj)) return n + stringify_depth_marker(js, obj, "Object", buf + n, Ant_Remaining(n, len));
  
  return strobj_inspect(js, obj, buf, len, n);
}

static ant_value_t strobj_call_custom_inspect(ant_t *js, ant_value_t obj) {
  ant_value_t inspect_sym = js->sym.inspect_sym;
  if (vtype(inspect_sym) != kTypeSymbol) return Ant_Value_Undefined;

  ant_value_t inspect_fn = lkp_sym_proto_val(js, obj, (ant_offset_t)vdata(inspect_sym));
  if (!is_callable(inspect_fn)) return Ant_Value_Undefined;

  GC_ROOT_SAVE(exception_mark, js);
  ant_value_t saved = Ant_Exception_Peek(js);
  GC_ROOT_PIN(js, saved);

  ant_value_t depth_arg = Ant_Value_FromNumber((double)(MAX_STRINGIFY_DEPTH - js->stringify->depth));
  ant_value_t result = sv_call_value(js, inspect_fn, obj, &depth_arg, 1);

  if (is_err(result) || Ant_Exception_Pending(js) || vtype(result) != kTypeString) {
    Ant_Exception_Set(js, saved);
    GC_ROOT_RESTORE(js, exception_mark);
    return Ant_Value_Undefined;
  }

  Ant_Exception_Set(js, saved);
  GC_ROOT_RESTORE(js, exception_mark);

  return result;
}

static size_t strobj(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  if (js_proxy_is_host(obj)) return strobj_host_proxy(js, obj, buf, len);
  
  int ref = get_circular_ref(js, obj);
  if (ref) return ref > 0 ? (size_t)snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);
  
  push_stringify(js, obj);
  
  size_t n = 0;
  ant_value_t custom = strobj_call_custom_inspect(js, obj);
  if (vtype(custom) == kTypeString) {
    size_t slen = 0;
    const char *s = js_getstr(js, custom, &slen);
    n = cpy(buf, len, s ? s : "", slen);
  } else n = strobj_body(js, obj, buf, len, 0);
  
  pop_stringify(js);
  return stringify_ref_label(js, obj, buf, len, n);
}

static bool strstring_has_template(const char *str, size_t slen) {
  for (const char *p = str; (p = memchr(p, '$', (size_t)(str + slen - p))); p++)
    if (p + 1 < str + slen && p[1] == '{') return true;
  return false;
}

static char strstring_quote(const char *str, ant_offset_t slen) {
  size_t n = (size_t)slen;

  if (!memchr(str, '\'', n)) return '\'';
  if (!memchr(str, '"', n)) return '"';
  if (!memchr(str, '`', n) && !strstring_has_template(str, n)) return '`';

  return '\'';
}

static const uint8_t strstring_escape_tbl[256] = {
  [0x00] = 1, [0x01] = 1, [0x02] = 1, [0x03] = 1, [0x04] = 1, [0x05] = 1, [0x06] = 1,
  [0x07] = 1, [0x08] = 1, [0x09] = 1, [0x0a] = 1, [0x0b] = 1, [0x0c] = 1, [0x0d] = 1,
  [0x0e] = 1, [0x0f] = 1, [0x10] = 1, [0x11] = 1, [0x12] = 1, [0x13] = 1, [0x14] = 1,
  [0x15] = 1, [0x16] = 1, [0x17] = 1, [0x18] = 1, [0x19] = 1, [0x1a] = 1, [0x1b] = 1,
  [0x1c] = 1, [0x1d] = 1, [0x1e] = 1, [0x1f] = 1, [0x7f] = 1, ['\\'] = 1,
};

size_t strstring(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t slen, off = vstr(js, value, &slen);
  const char *str = (const char *)(uintptr_t)off;
  char quote = strstring_quote(str, slen);

  size_t n = 0; size_t avail = 0;
  char *dst = fixed_buf_write_ptr(buf, len, n, &avail);
  
  ant_offset_t i = 0;
  n += cpy(dst, avail, &quote, 1);

  while (i < slen) {
    ant_offset_t start = i;
    const char *escaped = NULL;
    
    size_t escaped_len = 2;
    char scratch[4];

    while (
      i < slen &&
      !strstring_escape_tbl[(unsigned char)str[i]] &&
      str[i] != quote
    ) i++;

    if (i > start) {
      dst = fixed_buf_write_ptr(buf, len, n, &avail);
      n += cpy(dst, avail, str + start, (size_t)(i - start));
    }

    if (i >= slen) break;

    switch (str[i]) {
      case '\n': escaped = "\\n";  break;
      case '\r': escaped = "\\r";  break;
      case '\t': escaped = "\\t";  break;
      case '\b': escaped = "\\b";  break;
      case '\f': escaped = "\\f";  break;
      case '\v': escaped = "\\v";  break;
      case '\\': escaped = "\\\\"; break;
      
      default: {
        unsigned char c = (unsigned char)str[i];
        scratch[0] = '\\';

        if (c == (unsigned char)quote) scratch[1] = quote;
        else {
          scratch[1] = 'x';
          scratch[2] = "0123456789ABCDEF"[c >> 4];
          scratch[3] = "0123456789ABCDEF"[c & 0x0f];
          escaped_len = 4;
        }

        escaped = scratch;
        break;
      }
    }

    dst = fixed_buf_write_ptr(buf, len, n, &avail);
    n += cpy(dst, avail, escaped, escaped_len);
    i++;
  }

  dst = fixed_buf_write_ptr(buf, len, n, &avail);
  n += cpy(dst, avail, &quote, 1);

  return n;
}

struct func_format {
  const char *prefix;
  size_t prefix_len;
  const char *anon;
  size_t anon_len;
};

// TODO: migrate to Symbol.inspect
static const struct func_format formats[] = {
  [0] = { "[Function: ",               11, "[Function (anonymous)]",               22 },
  [1] = { "[AsyncFunction: ",          16, "[AsyncFunction (anonymous)]",          27 },
  [2] = { "[GeneratorFunction: ",      20, "[GeneratorFunction (anonymous)]",      31 },
  [3] = { "[AsyncGeneratorFunction: ", 25, "[AsyncGeneratorFunction (anonymous)]", 36 },
};

bool func_has_code(ant_value_t code_val) {
  uint8_t type = vtype(code_val);
  return type == kTypeSourceCode || type == kTypeString;
}

const char *func_code_text(ant_t *js, ant_value_t code_val) {
  if (vtype(code_val) == kTypeSourceCode) return (const char *)vptr(code_val);
  if (vtype(code_val) != kTypeString) return NULL;
  ant_offset_t len = 0;
  return (const char *)(uintptr_t)vstr(js, code_val, &len);
}

static size_t strclass(ant_t *js, ant_value_t value, const char *name, ant_offset_t name_len, char *buf, size_t len) {
  size_t n = cpy(buf, len, "[class ", 7);
  n += name && name_len > 0
    ? cpy(buf + n, Ant_Remaining(n, len), name, name_len)
    : cpy(buf + n, Ant_Remaining(n, len), "(anonymous)", 11);

  ant_offset_t parent_len = 0;
  const char *parent = get_func_name(js, get_proto(js, value), &parent_len);
  if (parent && parent_len > 0) {
    n += cpy(buf + n, Ant_Remaining(n, len), " extends ", 9);
    n += cpy(buf + n, Ant_Remaining(n, len), parent, parent_len);
  }

  return n + cpy(buf + n, Ant_Remaining(n, len), "]", 1);
}

// todo: make it work with bytecode NAME
static size_t strfunc(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t name_len = 0;
  const char *name = get_func_name(js, value, &name_len);
  
  ant_value_t func_obj = js_func_obj(value);
  ant_value_t code_slot = js_get_slot(func_obj, SLOT_CODE);
  ant_value_t builtin_slot = js_get_slot(func_obj, SLOT_BUILTIN);
  ant_value_t async_slot = js_get_slot(func_obj, SLOT_ASYNC);
  sv_closure_t *closure = js_func_closure(value);
  
  if (
    closure && ((closure->call_flags & SV_CALL_IS_DEFAULT_CTOR) || 
    (closure->func && closure->func->is_class_ctor))
  ) return strclass(js, value, name, name_len, buf, len);
  
  bool is_async = (async_slot == Ant_Value_True);
  bool has_code = func_has_code(code_slot);
  bool is_generator = closure != NULL && closure->func != NULL && closure->func->is_generator;
  
  const struct func_format *fmt = &formats[(is_generator ? 2 : 0) | (is_async ? 1 : 0)];
  
  if (vtype(builtin_slot) == kTypeNumber) {
    if (name && name_len > 0) {
      size_t n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
      n += cpy(buf + n, Ant_Remaining(n, len), name, name_len);
      n += cpy(buf + n, Ant_Remaining(n, len), "]", 1);
      return n;
    }
    return cpy(buf, len, fmt->anon, fmt->anon_len);
  }
  
  if (!has_code) {
    ant_value_t cfunc_slot = js_get_slot(func_obj, SLOT_CFUNC);
    bool is_native = (vtype(cfunc_slot) == kTypeBuiltin);
    size_t n;
    
    if (name && name_len > 0) {
      n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
      n += cpy(buf + n, Ant_Remaining(n, len), name, name_len);
      n += cpy(buf + n, Ant_Remaining(n, len), "]", 1);
    } else {
      n = cpy(buf, len, fmt->anon, fmt->anon_len);
    }
    
    if (!is_native) return n;
    
    ant_value_t proto = js_get_slot(func_obj, SLOT_PROTO);
    uint8_t pt = vtype(proto);
    if (pt != kTypeObject && pt != kTypeFunction) return n;
    
    ant_value_t ctor = lkp_val(js, proto, "constructor", 11);
    uint8_t ct = vtype(ctor);
    if (ct != kTypeFunction && ct != kTypeBuiltin) return n;
    
    ant_offset_t ctor_name_len = 0;
    const char *ctor_name = get_func_name(js, ctor, &ctor_name_len);
    if (ctor_name && ctor_name_len > 0) {
      n += cpy(buf + n, Ant_Remaining(n, len), " ", 1);
      n += cpy(buf + n, Ant_Remaining(n, len), ctor_name, ctor_name_len);
    }
    return n;
  }
  
  if (name && name_len > 0) {
    size_t n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
    n += cpy(buf + n, Ant_Remaining(n, len), name, name_len);
    n += cpy(buf + n, Ant_Remaining(n, len), "]", 1);
    return n;
  }
  
  return cpy(buf, len, fmt->anon, fmt->anon_len);
}

static size_t strfunc_inspect(ant_t *js, ant_value_t value, char *buf, size_t len) {
  int ref = get_circular_ref(js, value);
  if (ref) return ref > 0 ? (size_t)snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);

  push_stringify(js, value);
  size_t n = strobj_trailing_props(js, value, buf, len, strfunc(js, value, buf, len));
  pop_stringify(js);
  return stringify_ref_label(js, value, buf, len, n);
}

static size_t strcfunc(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t name_len = 0;
  const char *name = get_func_name(js, value, &name_len);
  const struct func_format *fmt = &formats[0];

  if (name && name_len > 0) {
    size_t n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
    n += cpy(buf + n, Ant_Remaining(n, len), name, name_len);
    n += cpy(buf + n, Ant_Remaining(n, len), "]", 1);
    return n;
  }

  return cpy(buf, len, fmt->anon, fmt->anon_len);
}

static size_t strcfunc_inspect(ant_t *js, ant_value_t value, char *buf, size_t len) {
  size_t n = strcfunc(js, value, buf, len);
  if (!js->stringify || !(js->stringify->flags & ANT_INSPECT_SHOW_HIDDEN)) return n;

  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_hidden_trailer(&builder, value);
  return builder.n;
}

static size_t strpromise(ant_t *js, ant_value_t value, char *buf, size_t len);

size_t tostr(ant_t *js, ant_value_t value, char *buf, size_t len) {
  if (is_object_type(value) && sv_check_c_stack_overflow(js))
    return Ant_CopyLiteral(buf, len, "[Object: Inspection interrupted prematurely. Maximum call stack size exceeded.]");
  
  switch (vtype(value)) {
    case kTypeUndefined:   return Ant_CopyLiteral(buf, len, "undefined");
    case kTypeNull:    return Ant_CopyLiteral(buf, len, "null");
    
    case kTypeBool: {
      bool b = vdata(value) & 1;
      return b ? Ant_CopyLiteral(buf, len, "true") : Ant_CopyLiteral(buf, len, "false");
    }
    
    case kTypeArray:    return strarr(js, value, buf, len);
    case kTypeObject:   return strobj(js, value, buf, len);
    case kTypeString:   return strstring(js, value, buf, len);
    case kTypePromise:  return strpromise(js, value, buf, len);
    case kTypeFunction: return strfunc_inspect(js, value, buf, len);
    case kTypeBuiltin:  return strcfunc_inspect(js, value, buf, len);
    
    case kTypeNumber: {
      if (Ant_Value_AsNumberUnchecked(value) == 0 && signbit(Ant_Value_AsNumberUnchecked(value))) return Ant_CopyLiteral(buf, len, "-0");
      return js_number_to_chars(value, buf, len);
    }
    
    case kTypeBigInt: {
      size_t n = strbigint(js, value, buf, len);
      return n + cpy(buf + n, Ant_Remaining(n, len), "n", 1);
    }
    
    case kTypeError: {
      if (is_err(value)) {
        ant_value_t stack = Ant_Exception_Stack(js, value);
        if (vtype(stack) == kTypeString) {
          ant_offset_t slen;
          ant_offset_t off = vstr(js, stack, &slen);
          return cpy(buf, len, (const char *)(uintptr_t)(off), slen);
        }
      }
      return Ant_CopyLiteral(buf, len, "Error");
    }
    
    case kTypeSymbol: {
      const char *desc = js_sym_desc(value);
      if (desc) return (size_t) snprintf(buf, len, "Symbol(%s)", desc);
      return Ant_CopyLiteral(buf, len, "Symbol()");
    }
    
    case kTypeGenerator: return strobj(js, mkval(kTypeObject, vdata(value)), buf, len);
    default: return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }
}

static char *tostr_alloc(ant_t *js, ant_value_t value) {
  size_t cap = 64;
  char *buf = calloc(1, cap);
  size_t n = tostr(js, value, buf, cap);
  if (n >= cap) {
    free(buf);
    buf = calloc(1, n + 1);
    tostr(js, value, buf, n + 1);
  }
  return buf;
}

static js_cstr_t js_cstr_render(
  ant_t *js, ant_value_t value,
  char *stack_buf, size_t stack_size, bool quote_strings
) {
  js_cstr_t out = { .ptr = "", .len = 0, .needs_free = false };

  if (is_err(value)) {
    if (is_err(value)) {
      ant_value_t stack = Ant_Exception_Stack(js, value);
      if (vtype(stack) == kTypeString) {
        ant_offset_t slen;
        ant_offset_t off = vstr(js, stack, &slen);
        out.ptr = (const char *)(uintptr_t)(off);
        out.len = slen;
        return out;
      }
    }
    out.ptr = "Error";
    out.len = 5;
    return out;
  }

  if (!quote_strings && vtype(value) == kTypeString) {
    size_t len = 0;
    char *str = js_getstr(js, value, &len);
    out.ptr = str ? str : ""; out.len = len;
    return out;
  }

  size_t capacity = stack_size;
  char *buf = stack_buf;
  out.needs_free = false;

  if (!buf || capacity == 0) {
    capacity = 64;
    buf = calloc(1, capacity);
    if (!buf) return out;
    out.needs_free = true;
  }

  for (;;) {
    stringify_begin_pass(js);
    size_t len = tostr(js, value, buf, capacity);

    if (len < capacity - 1) {
      out.ptr = buf;
      out.len = len;
      return out;
    }

    size_t new_capacity = capacity * 2;
    char *next = out.needs_free 
      ? realloc(buf, new_capacity) 
      : calloc(1, new_capacity);
    
    if (!next) {
      if (out.needs_free) free(buf);
      out.ptr = ""; out.len = 0;
      out.needs_free = false;
      return out;
    }

    if (!out.needs_free) {
      memcpy(next, buf, capacity);
      out.needs_free = true;
    }

    buf = next;
    capacity = new_capacity;
  }
}

static js_cstr_t js_cstr_impl(
  ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size, 
  bool quote_strings, int max_depth, uint8_t flags
) {
  ant_stringify_ctx_t ctx;
  ant_stringify_enter(js, &ctx, max_depth);
  ctx.flags = flags;
  
  js_cstr_t out = js_cstr_render(js, value, stack_buf, stack_size, quote_strings);
  Ant_List_Remove(&ctx);
  
  return out;
}

js_cstr_t js_to_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size) {
  return js_cstr_impl(js, value, stack_buf, stack_size, false, ANT_INSPECT_MAX_DEPTH, 0);
}

js_cstr_t js_inspect_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size) {
  return js_cstr_impl(js, value, stack_buf, stack_size, true, ANT_INSPECT_MAX_DEPTH, 0);
}

js_cstr_t js_inspect_cstr_depth(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size, int max_depth) {
  return js_cstr_impl(js, value, stack_buf, stack_size, true, max_depth, 0);
}

js_cstr_t js_inspect_cstr_opts(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size, int max_depth, uint8_t flags) {
  return js_cstr_impl(js, value, stack_buf, stack_size, true, max_depth, flags);
}

ant_value_t js_tostring_val(ant_t *js, ant_value_t value) {
  uint8_t t = vtype(value);
  char *buf; size_t len, buflen;
  
  static const void *jump_table[] = {
    [kTypeObject] = &&L_OBJ, [kTypeString] = &&L_STR,
    [kTypeUndefined] = &&L_UNDEF, [kTypeNull] = &&L_NULL, [kTypeNumber] = &&L_NUM,
    [kTypeBool] = &&L_BOOL, [kTypeFunction] = &&L_OBJ, [kTypeBuiltin] = &&L_OBJ,
    [kTypeError] = &&L_DEFAULT, [kTypeArray] = &&L_OBJ,
    [kTypePromise] = &&L_DEFAULT, [kTypeTypedArray] = &&L_DEFAULT,
    [kTypeBigInt] = &&L_BIGINT, [kTypeSymbol] = &&L_DEFAULT,
    [kTypeGenerator] = &&L_DEFAULT
  };
  
  if (t < sizeof(jump_table) / sizeof(jump_table[0])) goto *jump_table[t];
  goto L_DEFAULT;

  L_STR:   return value;
  L_UNDEF: return js_mkstr(js, "undefined", 9);
  L_NULL:  return js_mkstr(js, "null", 4);
  L_BOOL:  return vdata(value) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
  L_OBJ:   return coerce_to_str_hint(js, value, 1);
  
  L_NUM: {
    int32_t integer;
    if (ant_number_as_int32(Ant_Value_AsNumberUnchecked(value), &integer)) return js_mkstr_int32(js, integer);
    char num_buf[32];
    len = js_number_to_chars(value, num_buf, sizeof(num_buf));
    return js_mkstr_ascii(js, num_buf, len);
  }
    
  L_BIGINT: {
    buflen = bigint_digits_len(js, value);
    char bigint_stack_buf[128];
    
    size_t capacity = buflen + 2;
    bool needs_free = capacity > sizeof(bigint_stack_buf);
    buf = needs_free ? (char *)calloc(1, capacity) : bigint_stack_buf;
    
    if (!buf) return js_mkerr(js, "oom");
    len = strbigint(js, value, buf, capacity);
    
    ant_value_t result = js_mkstr(js, buf, len);
    if (needs_free) free(buf);
    
    return result;
  }
    
  L_DEFAULT: {
    buf = tostr_alloc(js, value);
    ant_value_t result = js_mkstr(js, buf, strlen(buf));
    free(buf); return result;
  }
}

static const char *js_str_render(ant_t *js, ant_value_t value) {
  if (is_err(value)) {
    if (is_err(value)) {
      ant_value_t stack = Ant_Exception_Stack(js, value);
      if (vtype(stack) == kTypeString) {
        ant_offset_t slen, off = vstr(js, stack, &slen);
        return (const char *)(uintptr_t)off;
      }
    }
    return "Error";
  }
  
  size_t capacity = 4096;
  char *buf = (char *)calloc(1, capacity);
  if (!buf) return "";
  
  size_t len;
  for (;;) {
    stringify_begin_pass(js);
    len = tostr(js, value, buf, capacity);
    
    if (len < capacity - 1) break;
    
    capacity *= 2;
    buf = (char *)realloc(buf, capacity);
    if (!buf) return "";
  }
  
  ant_value_t str = js_mkstr(js, buf, len);
  free(buf);
  
  if (is_err(str)) return "";
  ant_offset_t off = vstr(js, str, NULL);
  return (const char *)(uintptr_t)off;
}

const char *js_str(ant_t *js, ant_value_t value) {
  ant_stringify_ctx_t ctx;
  ant_stringify_enter(js, &ctx, ANT_INSPECT_MAX_DEPTH);
  
  const char *out = js_str_render(js, value);
  Ant_List_Remove(&ctx);
  
  return out;
}

static size_t strpromise(ant_t *js, ant_value_t value, char *buf, size_t len) {
  uint32_t pid = js_promise_id(js, value);
  ant_promise_state_t *pd = js_promise_data(js, value, false);
  
  const char *content;
  char *allocated = NULL;
  
  if (!pd || pd->state == 0) {
    content = "<pending>";
  } else if (pd->state == 2) {
    char *val = tostr_alloc(js, pd->value);
    allocated = calloc(1, strlen(val) + 12);
    sprintf(allocated, "<rejected> %s", val);
    free(val);
    content = allocated;
  } else { content = allocated = tostr_alloc(js, pd->value); }
  
  uint32_t trigger_pid = 0;
  if (pd && vtype(pd->trigger_parent) == kTypePromise) {
    trigger_pid = js_promise_id(js, pd->trigger_parent);
  }
  size_t result = trigger_pid
    ? (size_t)snprintf(buf, len, "Promise {\n  %s,\n  Symbol(async_id): %u,\n  Symbol(trigger_async_id): %u\n}", content, pid, trigger_pid)
    : (size_t)snprintf(buf, len, "Promise {\n  %s,\n  Symbol(async_id): %u\n}", content, pid);
  
  if (allocated) free(allocated);
  return result;
}
