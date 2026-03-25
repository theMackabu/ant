#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "descriptors.h"

#include "silver/engine.h"
#include "modules/formdata.h"
#include "modules/blob.h"
#include "modules/symbol.h"

typedef struct fd_entry {
  char *name;
  bool is_file;
  char *str_value;
  size_t val_idx;
  struct fd_entry *next;
} fd_entry_t;

typedef struct {
  fd_entry_t *head;
  fd_entry_t **tail;
  size_t count;
} fd_data_t;

typedef struct {
  size_t index;
  int kind;
} fd_iter_t;

enum {
  FD_ITER_ENTRIES = 0,
  FD_ITER_KEYS = 1,
  FD_ITER_VALUES = 2
};

static ant_value_t g_formdata_proto      = 0;
static ant_value_t g_formdata_iter_proto = 0;

static fd_data_t *fd_data_new(void) {
  fd_data_t *d = calloc(1, sizeof(fd_data_t));
  if (!d) return NULL;
  d->tail = &d->head;
  return d;
}

static void fd_entry_free(fd_entry_t *e) {
  if (!e) return;
  free(e->name);
  free(e->str_value);
  free(e);
}

static void fd_data_free(fd_data_t *d) {
  if (!d) return;
  for (fd_entry_t *e = d->head; e; ) {
    fd_entry_t *n = e->next;
    fd_entry_free(e);
    e = n;
  }
  free(d);
}

static fd_data_t *get_fd_data(ant_value_t obj) {
  ant_value_t slot = js_get_slot(obj, SLOT_DATA);
  if (vtype(slot) != T_NUM) return NULL;
  return (fd_data_t *)(uintptr_t)(size_t)js_getnum(slot);
}

static ant_value_t get_fd_values(ant_value_t obj) {
  return js_get_slot(obj, SLOT_ENTRIES);
}

static void formdata_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;

  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_DATA && vtype(entries[i].value) == T_NUM) {
    fd_data_t *d = (fd_data_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
    fd_data_free(d);
    return;
  }}
}

static bool fd_append_str(fd_data_t *d, const char *name, const char *value) {
  fd_entry_t *e = calloc(1, sizeof(fd_entry_t));
  if (!e) return false;

  e->name = strdup(name);
  e->str_value = strdup(value);

  if (!e->name || !e->str_value) { fd_entry_free(e); return false; }

  *d->tail = e;
  d->tail = &e->next;
  d->count++;
  return true;
}

static bool fd_append_file(fd_data_t *d, const char *name, size_t val_idx) {
  fd_entry_t *e = calloc(1, sizeof(fd_entry_t));
  if (!e) return false;

  e->is_file = true;
  e->name = strdup(name);
  e->val_idx = val_idx;

  if (!e->name) { free(e); return false; }

  *d->tail = e;
  d->tail = &e->next;
  d->count++;
  return true;
}

static void fd_delete_name(fd_data_t *d, const char *name) {
  fd_entry_t **pp = &d->head;
  d->tail = &d->head;

  while (*pp) {
  if (strcmp((*pp)->name, name) == 0) {
    fd_entry_t *dead = *pp;
    *pp = dead->next;
    d->count--;
    fd_entry_free(dead);
  } else {
    d->tail = &(*pp)->next;
    pp = &(*pp)->next;
  }}
}

static ant_value_t entry_to_js_value(ant_t *js, ant_value_t values_arr, fd_entry_t *e) {
  if (!e->is_file)
    return js_mkstr(js, e->str_value ? e->str_value : "", strlen(e->str_value ? e->str_value : ""));
  return js_arr_get(js, values_arr, (ant_offset_t)e->val_idx);
}

static const char *resolve_name(ant_t *js, ant_value_t *name_v) {
  if (vtype(*name_v) != T_STR) {
    *name_v = js_tostring_val(js, *name_v);
    if (is_err(*name_v)) return NULL;
  }
  return js_getstr(js, *name_v, NULL);
}

static ant_value_t extract_file_entry(
  ant_t *js, fd_data_t *d, ant_value_t values_arr,
  const char *name, ant_value_t val, ant_value_t filename_v, bool is_set
) {
  blob_data_t *bd = blob_get_data(val);
  if (!bd) return js_mkerr_typed(js, JS_ERR_TYPE, "FormData value must be a string, Blob, or File");

  bool is_file = (bd->name != NULL);
  bool no_filename_override = (vtype(filename_v) == T_UNDEF);

  ant_value_t stored_val;

  if (is_file && no_filename_override) {
    stored_val = val;
  } else {
    const char *fname = NULL;
    char *fname_owned = NULL;
    
    if (!no_filename_override) {
      ant_value_t fv = filename_v;
      if (vtype(fv) != T_STR) { fv = js_tostring_val(js, fv); if (is_err(fv)) return fv; }
      fname_owned = strdup(js_getstr(js, fv, NULL));
      fname = fname_owned;
    } 
    
    else if (bd->name && bd->name[0]) fname = bd->name;
    else fname = "blob";
    
    int64_t last_modified = bd->last_modified;
    if (!last_modified) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      last_modified = (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000);
    }
    
    ant_value_t file_obj = blob_create(js, bd->data, bd->size, bd->type);
    if (is_err(file_obj)) { free(fname_owned); return file_obj; }
    
    blob_data_t *nbd = blob_get_data(file_obj);
    if (nbd) {
      nbd->name = strdup(fname);
      nbd->last_modified = last_modified;
    }
    
    free(fname_owned);
    js_set_proto_init(file_obj, g_file_proto);
    stored_val = file_obj;
  }

  if (is_set) fd_delete_name(d, name);
  size_t idx = (size_t)js_arr_len(js, values_arr);
  js_arr_push(js, values_arr, stored_val);

  return fd_append_file(d, name, idx) ? js_mkundef() : js_mkerr(js, "out of memory");
}

static ant_value_t js_formdata_append(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "FormData.append requires 2 arguments");
  fd_data_t *d = get_fd_data(js->this_val);
  if (!d) return js_mkerr(js, "Invalid FormData object");

  ant_value_t name_v = args[0];
  const char *name = resolve_name(js, &name_v);
  if (!name) return name_v;

  ant_value_t val = args[1];
  if (is_object_type(val) && blob_get_data(val)) {
    ant_value_t fname = (nargs >= 3) ? args[2] : js_mkundef();
    ant_value_t values_arr = get_fd_values(js->this_val);
    return extract_file_entry(js, d, values_arr, name, val, fname, false);
  }

  if (vtype(val) != T_STR) { val = js_tostring_val(js, val); if (is_err(val)) return val; }
  return fd_append_str(d, name, js_getstr(js, val, NULL)) ? js_mkundef() : js_mkerr(js, "out of memory");
}

static ant_value_t js_formdata_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "FormData.set requires 2 arguments");
  fd_data_t *d = get_fd_data(js->this_val);
  if (!d) return js_mkerr(js, "Invalid FormData object");

  ant_value_t name_v = args[0];
  const char *name = resolve_name(js, &name_v);
  if (!name) return name_v;

  ant_value_t val = args[1];
  if (is_object_type(val) && blob_get_data(val)) {
    ant_value_t fname = (nargs >= 3) ? args[2] : js_mkundef();
    ant_value_t values_arr = get_fd_values(js->this_val);
    return extract_file_entry(js, d, values_arr, name, val, fname, true);
  }

  if (vtype(val) != T_STR) { val = js_tostring_val(js, val); if (is_err(val)) return val; }
  fd_delete_name(d, name);
  return fd_append_str(d, name, js_getstr(js, val, NULL)) ? js_mkundef() : js_mkerr(js, "out of memory");
}

static ant_value_t js_formdata_get(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mknull();
  fd_data_t *d = get_fd_data(js->this_val);
  if (!d) return js_mknull();

  ant_value_t name_v = args[0];
  const char *name = resolve_name(js, &name_v);
  if (!name) return name_v;

  ant_value_t values_arr = get_fd_values(js->this_val);
  for (fd_entry_t *e = d->head; e; e = e->next) {
    if (strcmp(e->name, name) == 0)
      return entry_to_js_value(js, values_arr, e);
  }
  return js_mknull();
}

static ant_value_t js_formdata_get_all(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t result = js_mkarr(js);
  if (nargs < 1) return result;
  fd_data_t *d = get_fd_data(js->this_val);
  if (!d) return result;

  ant_value_t name_v = args[0];
  const char *name = resolve_name(js, &name_v);
  if (!name) return name_v;

  ant_value_t values_arr = get_fd_values(js->this_val);
  for (fd_entry_t *e = d->head; e; e = e->next) {
  if (strcmp(e->name, name) == 0) {
    ant_value_t v = entry_to_js_value(js, values_arr, e);
    if (is_err(v)) return v;
    js_arr_push(js, result, v);
  }}
  return result;
}

static ant_value_t js_formdata_has(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  fd_data_t *d = get_fd_data(js->this_val);
  if (!d) return js_false;

  ant_value_t name_v = args[0];
  const char *name = resolve_name(js, &name_v);
  if (!name) return name_v;

  for (fd_entry_t *e = d->head; e; e = e->next) {
    if (strcmp(e->name, name) == 0) return js_true;
  }
  return js_false;
}

static ant_value_t js_formdata_delete(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  fd_data_t *d = get_fd_data(js->this_val);
  if (!d) return js_mkundef();

  ant_value_t name_v = args[0];
  const char *name = resolve_name(js, &name_v);
  if (!name) return name_v;

  fd_delete_name(d, name);
  return js_mkundef();
}

static ant_value_t js_formdata_foreach(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "FormData.forEach requires a function");
  fd_data_t *d = get_fd_data(js->this_val);
  if (!d) return js_mkundef();

  ant_value_t fn = args[0];
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  ant_value_t self = js->this_val;
  ant_value_t values_arr = get_fd_values(self);

  for (fd_entry_t *e = d->head; e; e = e->next) {
    ant_value_t val = entry_to_js_value(js, values_arr, e);
    if (is_err(val)) return val;
    ant_value_t name = js_mkstr(js, e->name, strlen(e->name));
    ant_value_t cb_args[3] = { val, name, self };
    ant_value_t r = sv_vm_call(js->vm, js, fn, this_arg, cb_args, 3, NULL, false);
    if (is_err(r)) return r;
  }
  return js_mkundef();
}

static ant_value_t formdata_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state_v = js_get_slot(js->this_val, SLOT_ITER_STATE);
  if (vtype(state_v) != T_NUM) return js_iter_result(js, false, js_mkundef());

  fd_iter_t *st = (fd_iter_t *)(uintptr_t)(size_t)js_getnum(state_v);
  ant_value_t fd_obj = js_get_slot(js->this_val, SLOT_DATA);
  fd_data_t *d = get_fd_data(fd_obj);
  if (!d) return js_iter_result(js, false, js_mkundef());

  ant_value_t values_arr = get_fd_values(fd_obj);

  size_t idx = 0;
  for (fd_entry_t *e = d->head; e; e = e->next, idx++) {
  if (idx == st->index) {
    st->index++;
    ant_value_t out;
    switch (st->kind) {
    case FD_ITER_KEYS:
      out = js_mkstr(js, e->name, strlen(e->name));
      break;
    case FD_ITER_VALUES:
      out = entry_to_js_value(js, values_arr, e);
      break;
    default: {
      ant_value_t v = entry_to_js_value(js, values_arr, e);
      if (is_err(v)) return v;
      out = js_mkarr(js);
      js_arr_push(js, out, js_mkstr(js, e->name, strlen(e->name)));
      js_arr_push(js, out, v);
      break;
    }}
    if (is_err(out)) return out;
    return js_iter_result(js, true, out);
  }}

  return js_iter_result(js, false, js_mkundef());
}

static void formdata_iter_finalize(ant_t *js, ant_object_t *obj) {
  if (!obj->extra_slots) return;
  ant_extra_slot_t *entries = (ant_extra_slot_t *)obj->extra_slots;

  for (uint8_t i = 0; i < obj->extra_count; i++) {
  if (entries[i].slot == SLOT_ITER_STATE && vtype(entries[i].value) == T_NUM) {
    fd_iter_t *st = (fd_iter_t *)(uintptr_t)(size_t)js_getnum(entries[i].value);
    free(st);
    return;
  }}
}

static ant_value_t make_formdata_iter(ant_t *js, ant_value_t fd_obj, int kind) {
  fd_iter_t *st = calloc(1, sizeof(fd_iter_t));
  if (!st) return js_mkerr(js, "out of memory");
  st->kind = kind;

  ant_value_t iter = js_mkobj(js);
  js_set_proto_init(iter, g_formdata_iter_proto);
  js_set_slot(iter, SLOT_ITER_STATE, ANT_PTR(st));
  js_set_slot_wb(js, iter, SLOT_DATA, fd_obj);
  js_set_finalizer(iter, formdata_iter_finalize);
  return iter;
}

static ant_value_t js_formdata_entries(ant_t *js, ant_value_t *args, int nargs) {
  return make_formdata_iter(js, js->this_val, FD_ITER_ENTRIES);
}

static ant_value_t js_formdata_keys(ant_t *js, ant_value_t *args, int nargs) {
  return make_formdata_iter(js, js->this_val, FD_ITER_KEYS);
}

static ant_value_t js_formdata_values(ant_t *js, ant_value_t *args, int nargs) {
  return make_formdata_iter(js, js->this_val, FD_ITER_VALUES);
}

static ant_value_t js_formdata_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "FormData constructor requires 'new'");
  if (nargs >= 1 && vtype(args[0]) != T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "FormData does not support a form element argument");

  fd_data_t *d = fd_data_new();
  if (!d) return js_mkerr(js, "out of memory");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, g_formdata_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  js_set_slot(obj, SLOT_DATA, ANT_PTR(d));
  ant_value_t vals = js_mkarr(js);
  js_set_slot_wb(js, obj, SLOT_ENTRIES, vals);
  js_set_finalizer(obj, formdata_finalize);
  return obj;
}

void init_formdata_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);
  g_formdata_proto = js_mkobj(js);

  js_set(js, g_formdata_proto, "append",  js_mkfun(js_formdata_append));
  js_set(js, g_formdata_proto, "set",     js_mkfun(js_formdata_set));
  js_set(js, g_formdata_proto, "get",     js_mkfun(js_formdata_get));
  js_set(js, g_formdata_proto, "getAll",  js_mkfun(js_formdata_get_all));
  js_set(js, g_formdata_proto, "has",     js_mkfun(js_formdata_has));
  js_set(js, g_formdata_proto, "delete",  js_mkfun(js_formdata_delete));
  js_set(js, g_formdata_proto, "forEach", js_mkfun(js_formdata_foreach));
  js_set(js, g_formdata_proto, "entries", js_mkfun(js_formdata_entries));
  js_set(js, g_formdata_proto, "keys",    js_mkfun(js_formdata_keys));
  js_set(js, g_formdata_proto, "values",  js_mkfun(js_formdata_values));

  js_set_sym(js, g_formdata_proto, get_iterator_sym(),    js_mkfun(js_formdata_entries));
  js_set_sym(js, g_formdata_proto, get_toStringTag_sym(), js_mkstr(js, "FormData", 8));

  ant_value_t ctor_obj = js_mkobj(js);
  js_set_slot(ctor_obj, SLOT_CFUNC, js_mkfun(js_formdata_ctor));
  js_mkprop_fast(js, ctor_obj, "prototype", 9, g_formdata_proto);
  js_mkprop_fast(js, ctor_obj, "name", 4, js_mkstr(js, "FormData", 8));
  js_set_descriptor(js, ctor_obj, "name", 4, 0);

  ant_value_t ctor = js_obj_to_func(ctor_obj);
  js_set(js, g_formdata_proto, "constructor", ctor);
  js_set_descriptor(js, g_formdata_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, g, "FormData", ctor);
  js_set_descriptor(js, g, "FormData", 8, JS_DESC_W | JS_DESC_C);

  g_formdata_iter_proto = js_mkobj(js);
  js_set_proto_init(g_formdata_iter_proto, js->sym.iterator_proto);
  js_set(js, g_formdata_iter_proto, "next", js_mkfun(formdata_iter_next));
  js_set_descriptor(js, g_formdata_iter_proto, "next", 4, JS_DESC_W | JS_DESC_E | JS_DESC_C);
  js_set_sym(js, g_formdata_iter_proto, get_iterator_sym(), js_mkfun(sym_this_cb));
}
