#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <ctype.h>
#include <stdio.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "modules/blob.h"
#include "modules/buffer.h"
#include "modules/symbol.h"
#include "streams/readable.h"

ant_value_t g_blob_proto = 0;
ant_value_t g_file_proto = 0;

bool blob_is_blob(ant_t *js, ant_value_t obj) {
  int id = js_brand_id(obj);
  return id == BRAND_BLOB || id == BRAND_FILE;
}

blob_data_t *blob_get_data(ant_value_t obj) {
  ant_value_t slot = js_get_slot(obj, SLOT_DATA);
  if (vtype(slot) != T_NUM) return NULL;
  return (blob_data_t *)(uintptr_t)(size_t)js_getnum(slot);
}

static blob_data_t *blob_data_new(const uint8_t *data, size_t size, const char *type) {
  blob_data_t *bd = calloc(1, sizeof(blob_data_t));
  
  if (!bd) return NULL;
  if (size > 0 && data) {
    bd->data = malloc(size);
    if (!bd->data) { free(bd); return NULL; }
    memcpy(bd->data, data, size);
  }
  
  bd->size = size;
  bd->type = type ? strdup(type) : strdup("");
  
  return bd;
}

static char *normalize_mime_type(const char *s) {
  if (!s) return strdup("");
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p < 0x20 || *p > 0x7E) return strdup("");
  }
  
  size_t len = strlen(s);
  char *out = malloc(len + 1);
  
  if (!out) return strdup("");
  for (size_t i = 0; i <= len; i++) out[i] = (char)tolower((unsigned char)s[i]);
  
  return out;
}

typedef struct {
  uint8_t *buf;
  size_t   size;
  size_t   cap;
} byte_buf_t;

static bool byte_buf_grow(byte_buf_t *b, size_t extra) {
  size_t needed = b->size + extra;
  if (needed <= b->cap) return true;
  size_t new_cap = b->cap ? b->cap * 2 : 64;
  while (new_cap < needed) new_cap *= 2;
  uint8_t *p = realloc(b->buf, new_cap);
  if (!p) return false;
  b->buf = p;
  b->cap = new_cap;
  return true;
}

static bool byte_buf_append(byte_buf_t *b, const uint8_t *data, size_t len) {
  if (!byte_buf_grow(b, len)) return false;
  memcpy(b->buf + b->size, data, len);
  b->size += len;
  return true;
}

static ant_value_t process_blob_part(ant_t *js, byte_buf_t *buf, ant_value_t part) {
  uint8_t t = vtype(part);

  if (t == T_TYPEDARRAY) {
    TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(part);
    if (!ta || !ta->buffer) return js_mkundef();
    if (!byte_buf_append(buf, ta->buffer->data + ta->byte_offset, ta->byte_length))
      return js_mkerr(js, "out of memory");
    return js_mkundef();
  }

  if (t == T_OBJ) {
    TypedArrayData *ta = buffer_get_typedarray_data(part);
    if (ta && ta->buffer && !ta->buffer->is_detached) {
      if (!byte_buf_append(buf, ta->buffer->data + ta->byte_offset, ta->byte_length)) return js_mkerr(js, "out of memory");
      return js_mkundef();
    }
    ArrayBufferData *abd = buffer_get_arraybuffer_data(part);
    if (abd && !abd->is_detached) {
      if (!byte_buf_append(buf, abd->data, abd->length)) return js_mkerr(js, "out of memory");
      return js_mkundef();
    }
    blob_data_t *bd = blob_get_data(part);
    if (bd && bd->size > 0) {
      if (!byte_buf_append(buf, bd->data, bd->size)) return js_mkerr(js, "out of memory");
      return js_mkundef();
    }
  }

  ant_value_t str = (t == T_STR) ? part : js_tostring_val(js, part);
  if (is_err(str)) return str;

  size_t len;
  char *s = js_getstr(js, str, &len);
  if (s && len > 0) {
    if (!byte_buf_append(buf, (const uint8_t *)s, len))
      return js_mkerr(js, "out of memory");
  }
  return js_mkundef();
}

static ant_value_t process_blob_parts(ant_t *js, byte_buf_t *buf, ant_value_t parts) {
  uint8_t t = vtype(parts);
  if (t == T_UNDEF || t == T_NULL) return js_mkundef();

  if (t != T_OBJ && t != T_ARR && t != T_FUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Failed to construct 'Blob': The provided value cannot be converted to a sequence.");

  js_iter_t it;
  if (!js_iter_open(js, parts, &it))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to construct 'Blob': The provided value is not of type 'BlobPart'");

  ant_value_t value;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t r = process_blob_part(js, buf, value);
    if (is_err(r)) { js_iter_close(js, &it); return r; }
  }
  
  return js_mkundef();
}

static void blob_finalize(ant_t *js, ant_object_t *obj) {
  ant_extra_slot_t *slot = ant_object_extra_slot(obj, SLOT_DATA);
  if (!slot || vtype(slot->value) != T_NUM) return;
  blob_data_t *bd = (blob_data_t *)(uintptr_t)(size_t)js_getnum(slot->value);
  if (bd) { free(bd->data); free(bd->type); free(bd->name); free(bd); }
}

ant_value_t blob_create(ant_t *js, const uint8_t *data, size_t size, const char *type) {
  blob_data_t *bd = blob_data_new(data, size, type);
  if (!bd) return js_mkerr(js, "out of memory");
  ant_value_t obj = js_mkobj(js);
  
  js_set_proto_init(obj, g_blob_proto);
  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_BLOB));
  js_set_slot(obj, SLOT_DATA, ANT_PTR(bd));
  js_set_finalizer(obj, blob_finalize);
  
  return obj;
}

static ant_value_t blob_get_size(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  blob_data_t *bd = blob_get_data(js->this_val);
  return js_mknum(bd ? (double)bd->size : 0);
}

static ant_value_t blob_get_type(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  blob_data_t *bd = blob_get_data(js->this_val);
  if (!bd || !bd->type) return js_mkstr(js, "", 0);
  return js_mkstr(js, bd->type, strlen(bd->type));
}

static ant_value_t file_get_name(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  blob_data_t *bd = blob_get_data(js->this_val);
  if (!bd || !bd->name) return js_mkstr(js, "", 0);
  return js_mkstr(js, bd->name, strlen(bd->name));
}

static ant_value_t file_get_last_modified(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  blob_data_t *bd = blob_get_data(js->this_val);
  return js_mknum(bd ? (double)bd->last_modified : 0);
}

static ant_value_t js_blob_text(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  blob_data_t *bd = blob_get_data(js->this_val);
  ant_value_t promise = js_mkpromise(js);
  ant_value_t str = (!bd || bd->size == 0)
    ? js_mkstr(js, "", 0)
    : js_mkstr(js, (const char *)bd->data, bd->size);
  js_resolve_promise(js, promise, str);
  return promise;
}

static ant_value_t js_blob_array_buffer(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  blob_data_t *bd = blob_get_data(js->this_val);
  ant_value_t promise = js_mkpromise(js);

  size_t sz = (bd && bd->data) ? bd->size : 0;
  ArrayBufferData *abd = create_array_buffer_data(sz);
  if (!abd) { js_reject_promise(js, promise, js_mkerr(js, "out of memory")); return promise; }
  if (sz > 0 && bd) memcpy(abd->data, bd->data, sz);

  js_resolve_promise(js, promise, create_arraybuffer_obj(js, abd));
  return promise;
}

static ant_value_t js_blob_bytes(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  blob_data_t *bd = blob_get_data(js->this_val);
  ant_value_t promise = js_mkpromise(js);

  size_t sz = (bd && bd->data) ? bd->size : 0;
  ArrayBufferData *abd = create_array_buffer_data(sz);
  if (!abd) { js_reject_promise(js, promise, js_mkerr(js, "out of memory")); return promise; }
  if (sz > 0 && bd) memcpy(abd->data, bd->data, sz);

  js_resolve_promise(js, promise,
    create_typed_array(js, TYPED_ARRAY_UINT8, abd, 0, sz, "Uint8Array"));
  return promise;
}

static ant_value_t js_blob_slice(ant_t *js, ant_value_t *args, int nargs) {
  blob_data_t *bd = blob_get_data(js->this_val);
  size_t blob_size = bd ? bd->size : 0;

  ssize_t start = 0;
  if (nargs >= 1 && vtype(args[0]) != T_UNDEF) {
    double d = js_to_number(js, args[0]);
    start = (ssize_t)d;
    if (start < 0) start = (ssize_t)blob_size + start;
    if (start < 0) start = 0;
    if ((size_t)start > blob_size) start = (ssize_t)blob_size;
  }

  ssize_t end = (ssize_t)blob_size;
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    double d = js_to_number(js, args[1]);
    end = (ssize_t)d;
    if (end < 0) end = (ssize_t)blob_size + end;
    if (end < 0) end = 0;
    if ((size_t)end > blob_size) end = (ssize_t)blob_size;
  }

  if (end < start) end = start;

  size_t new_size = (size_t)(end - start);
  const uint8_t *src = (bd && bd->data && new_size > 0) ? (bd->data + start) : NULL;

  const char *new_type = (bd && bd->type) ? bd->type : "";
  char *type_owned = NULL;
  if (nargs >= 3 && vtype(args[2]) != T_UNDEF) {
    ant_value_t tv = args[2];
    if (vtype(tv) != T_STR) { tv = js_tostring_val(js, tv); if (is_err(tv)) return tv; }
    type_owned = normalize_mime_type(js_getstr(js, tv, NULL));
    new_type = type_owned;
  }

  ant_value_t result = blob_create(js, src, new_size, new_type);
  free(type_owned);
  return result;
}

static ant_value_t blob_stream_pull(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t blob_obj = js_get_slot(js->current_func, SLOT_DATA);
  blob_data_t *bd = blob_get_data(blob_obj);
  ant_value_t ctrl = (nargs > 0) ? args[0] : js_mkundef();

  if (bd && bd->size > 0 && bd->data) {
  ArrayBufferData *ab = create_array_buffer_data(bd->size);
  if (ab) {
    memcpy(ab->data, bd->data, bd->size);
    rs_controller_enqueue(js, ctrl, create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, bd->size, "Uint8Array"));
  }}

  rs_controller_close(js, ctrl);
  return js_mkundef();
}

static ant_value_t js_blob_stream(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t pull_fn = js_heavy_mkfun(js, blob_stream_pull, js->this_val);
  return rs_create_stream(js, pull_fn, js_mkundef(), 1);
}

static ant_value_t js_blob_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Blob constructor requires 'new'");

  byte_buf_t buf = {NULL, 0, 0};

  if (nargs >= 1 && vtype(args[0]) != T_UNDEF) {
    uint8_t pt = vtype(args[0]);
    if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC)
      return js_mkerr_typed(js, JS_ERR_TYPE,
        "Failed to construct 'Blob': The provided value cannot be converted to a sequence.");
    ant_value_t r = process_blob_parts(js, &buf, args[0]);
    if (is_err(r)) { free(buf.buf); return r; }
  }

  const char *type_str = "";
  char *type_owned = NULL;

  if (nargs >= 2 && vtype(args[1]) != T_UNDEF && vtype(args[1]) != T_NULL) {
    uint8_t ot = vtype(args[1]);
    if (ot != T_OBJ && ot != T_ARR && ot != T_FUNC && ot != T_CFUNC) {
      free(buf.buf);
      return js_mkerr_typed(js, JS_ERR_TYPE,
        "Failed to construct 'Blob': The 'options' argument is not an object.");
    }
    // access "endings" before "type" per lexicographic order (WPT requirement)
    (void)js_get(js, args[1], "endings");
    ant_value_t type_v = js_get(js, args[1], "type");
    if (vtype(type_v) != T_UNDEF) {
      if (vtype(type_v) != T_STR) {
        type_v = js_tostring_val(js, type_v);
        if (is_err(type_v)) { free(buf.buf); return type_v; }
      }
      type_owned = normalize_mime_type(js_getstr(js, type_v, NULL));
      type_str = type_owned;
    }
  }

  blob_data_t *bd = blob_data_new(buf.buf, buf.size, type_str);
  free(buf.buf); free(type_owned);
  if (!bd) return js_mkerr(js, "out of memory");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_blob_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_BLOB));
  js_set_slot(obj, SLOT_DATA, ANT_PTR(bd));
  js_set_finalizer(obj, blob_finalize);
  
  return obj;
}

static ant_value_t js_file_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "File constructor requires 'new'");
  if (nargs < 2)
    return js_mkerr_typed(js, JS_ERR_TYPE, "File constructor requires at least 2 arguments");

  byte_buf_t buf = {NULL, 0, 0};

  if (vtype(args[0]) != T_UNDEF) {
    uint8_t pt = vtype(args[0]);
    if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC) {
      return js_mkerr_typed(js, JS_ERR_TYPE,
        "Failed to construct 'File': The provided value cannot be converted to a sequence.");
    }
    ant_value_t r = process_blob_parts(js, &buf, args[0]);
    if (is_err(r)) { free(buf.buf); return r; }
  }

  ant_value_t name_v = args[1];
  if (vtype(name_v) != T_STR) {
    name_v = js_tostring_val(js, name_v);
    if (is_err(name_v)) { free(buf.buf); return name_v; }
  }
  const char *name_str = js_getstr(js, name_v, NULL);

  const char *type_str = "";
  char *type_owned = NULL;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  int64_t last_modified = (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000);

  if (nargs >= 3 && vtype(args[2]) != T_UNDEF && vtype(args[2]) != T_NULL) {
    ant_value_t opts = args[2];
    uint8_t ot = vtype(opts);
    if (ot == T_OBJ || ot == T_ARR) {
      ant_value_t type_v = js_get(js, opts, "type");
      if (vtype(type_v) != T_UNDEF) {
        if (vtype(type_v) != T_STR) {
          type_v = js_tostring_val(js, type_v);
          if (is_err(type_v)) { free(buf.buf); return type_v; }
        }
        type_owned = normalize_mime_type(js_getstr(js, type_v, NULL));
        type_str = type_owned;
      }
      ant_value_t lm_v = js_get(js, opts, "lastModified");
      if (vtype(lm_v) != T_UNDEF) {
        double d = js_to_number(js, lm_v);
        if (d == d) last_modified = (int64_t)d;
      }
    }
  }

  blob_data_t *bd = blob_data_new(buf.buf, buf.size, type_str);
  free(buf.buf); free(type_owned);
  if (!bd) return js_mkerr(js, "out of memory");

  bd->name          = strdup(name_str ? name_str : "");
  bd->last_modified = last_modified;

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_file_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  js_set_slot(obj, SLOT_BRAND, js_mknum(BRAND_FILE));
  js_set_slot(obj, SLOT_DATA, ANT_PTR(bd));
  js_set_finalizer(obj, blob_finalize);
  
  return obj;
}

void init_blob_module(void) {
  ant_t *js     = rt->js;
  ant_value_t g = js_glob(js);
  g_blob_proto  = js_mkobj(js);

  js_set_getter_desc(js, g_blob_proto, "size", 4, js_mkfun(blob_get_size), JS_DESC_C);
  js_set_getter_desc(js, g_blob_proto, "type", 4, js_mkfun(blob_get_type), JS_DESC_C);

  js_set(js, g_blob_proto, "text",        js_mkfun(js_blob_text));
  js_set(js, g_blob_proto, "arrayBuffer", js_mkfun(js_blob_array_buffer));
  js_set(js, g_blob_proto, "bytes",       js_mkfun(js_blob_bytes));
  js_set(js, g_blob_proto, "slice",       js_mkfun(js_blob_slice));
  js_set(js, g_blob_proto, "stream",      js_mkfun(js_blob_stream));

  js_set_sym(js, g_blob_proto, get_toStringTag_sym(), js_mkstr(js, "Blob", 4));
  ant_value_t blob_ctor = js_make_ctor(js, js_blob_ctor, g_blob_proto, "Blob", 4);
  
  js_set(js, g, "Blob", blob_ctor);
  js_set_descriptor(js, g, "Blob", 4, JS_DESC_W | JS_DESC_C);

  g_file_proto = js_mkobj(js);
  js_set_proto_init(g_file_proto, g_blob_proto);

  js_set_getter_desc(js, g_file_proto, "name",         4,  js_mkfun(file_get_name),          JS_DESC_C);
  js_set_getter_desc(js, g_file_proto, "lastModified", 12, js_mkfun(file_get_last_modified),  JS_DESC_C);

  js_set_sym(js, g_file_proto, get_toStringTag_sym(), js_mkstr(js, "File", 4));
  ant_value_t file_ctor = js_make_ctor(js, js_file_ctor, g_file_proto, "File", 4);
  
  js_set(js, g, "File", file_ctor);
  js_set_descriptor(js, g, "File", 4, JS_DESC_W | JS_DESC_C);
}
