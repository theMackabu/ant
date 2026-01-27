#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uthash.h>
#include <yyjson.h>

#include "ant.h"
#include "errors.h"
#include "arena.h"
#include "runtime.h"
#include "internal.h"

#include "modules/symbol.h"
#include "modules/localstorage.h"

typedef struct storage_entry {
  char *key;
  char *value;
  UT_hash_handle hh;
} storage_entry_t;

static storage_entry_t *local_storage = NULL;
static char *storage_file_path = NULL;

static void storage_save(void) {
  if (!storage_file_path) return;
  
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return;
  
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  
  storage_entry_t *entry, *tmp;
  HASH_ITER(hh, local_storage, entry, tmp) {
    yyjson_mut_obj_add_str(doc, root, entry->key, entry->value);
  }
  
  yyjson_write_err err;
  yyjson_mut_write_file(storage_file_path, doc, YYJSON_WRITE_PRETTY, NULL, &err);
  yyjson_mut_doc_free(doc);
}

static void storage_load(void) {
  if (!storage_file_path) return;
  
  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_file(storage_file_path, 0, NULL, &err);
  if (!doc) return;
  
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    yyjson_doc_free(doc);
    return;
  }
  
  yyjson_obj_iter iter;
  yyjson_obj_iter_init(root, &iter);
  yyjson_val *key, *val;
  
  while ((key = yyjson_obj_iter_next(&iter))) {
    val = yyjson_obj_iter_get_val(key);
    if (yyjson_is_str(key) && yyjson_is_str(val)) {
      const char *k = yyjson_get_str(key);
      const char *v = yyjson_get_str(val);
      size_t klen = yyjson_get_len(key);
      size_t vlen = yyjson_get_len(val);
      
      storage_entry_t *entry = ant_calloc(sizeof(storage_entry_t) + klen + 1 + vlen + 1);
      if (!entry) continue;
      
      entry->key = (char *)(entry + 1);
      entry->value = entry->key + klen + 1;
      
      memcpy(entry->key, k, klen);
      entry->key[klen] = '\0';
      memcpy(entry->value, v, vlen);
      entry->value[vlen] = '\0';
      
      HASH_ADD_KEYPTR(hh, local_storage, entry->key, klen, entry);
    }
  }
  
  yyjson_doc_free(doc);
}

static void storage_clear(void) {
  storage_entry_t *entry, *tmp;
  HASH_ITER(hh, local_storage, entry, tmp) {
    HASH_DEL(local_storage, entry);
    free(entry);
  }
}

static void storage_set_item(const char *key, size_t key_len, const char *value, size_t value_len) {
  storage_entry_t *entry = NULL;
  
  HASH_FIND(hh, local_storage, key, key_len, entry);
  
  if (entry) {
    HASH_DEL(local_storage, entry);
    free(entry);
  }
  
  entry = ant_calloc(sizeof(storage_entry_t) + key_len + 1 + value_len + 1);
  if (!entry) return;
  
  entry->key = (char *)(entry + 1);
  entry->value = entry->key + key_len + 1;
  
  memcpy(entry->key, key, key_len);
  entry->key[key_len] = '\0';
  memcpy(entry->value, value, value_len);
  entry->value[value_len] = '\0';
  
  HASH_ADD_KEYPTR(hh, local_storage, entry->key, key_len, entry);
  
  storage_save();
}

static char *storage_get_item(const char *key, size_t key_len) {
  storage_entry_t *entry = NULL;
  HASH_FIND(hh, local_storage, key, key_len, entry);
  return entry ? entry->value : NULL;
}

static void storage_remove_item(const char *key, size_t key_len) {
  storage_entry_t *entry = NULL;
  HASH_FIND(hh, local_storage, key, key_len, entry);
  
  if (entry) {
    HASH_DEL(local_storage, entry);
    free(entry);
    storage_save();
  }
}

static size_t storage_length(void) {
  return HASH_COUNT(local_storage);
}

static char *storage_key(size_t index) {
  storage_entry_t *entry;
  size_t i = 0;
  
  for (entry = local_storage; entry != NULL; entry = entry->hh.next) {
    if (i == index) return entry->key;
    i++;
  }
  
  return NULL;
}

#define CHECK_FILE_SET(js) \
  if (!storage_file_path) { \
    return js_mkerr(js, "Warning: `--localstorage-file` or `localStorage.setFile` were not provided with valid paths."); \
  }

// localStorage.setItem(key, value)
static jsval_t js_localstorage_setItem(struct js *js, jsval_t *args, int nargs) {
  CHECK_FILE_SET(js);
  
  if (nargs < 2) {
    return js_mkerr(js, "Failed to execute 'setItem' on 'Storage': 2 arguments required");
  }
  
  size_t key_len, value_len;
  char *key = js_getstr(js, args[0], &key_len);
  char *value = js_getstr(js, js_tostring_val(js, args[1]), &value_len);
  
  storage_set_item(key, key_len, value, value_len);
  
  return js_mkundef();
}

// localStorage.getItem(key)
static jsval_t js_localstorage_getItem(struct js *js, jsval_t *args, int nargs) {
  CHECK_FILE_SET(js);
  
  if (nargs < 1) {
    return js_mkerr(js, "Failed to execute 'getItem' on 'Storage': 1 argument required");
  }
  
  size_t key_len;
  char *key = js_getstr(js, args[0], &key_len);
  char *value = storage_get_item(key, key_len);
  
  if (!value) return js_mknull();
  
  return js_mkstr(js, value, strlen(value));
}

// localStorage.removeItem(key)
static jsval_t js_localstorage_removeItem(struct js *js, jsval_t *args, int nargs) {
  CHECK_FILE_SET(js);
  
  if (nargs < 1) {
    return js_mkerr(js, "Failed to execute 'removeItem' on 'Storage': 1 argument required");
  }
  
  size_t key_len;
  char *key = js_getstr(js, args[0], &key_len);
  storage_remove_item(key, key_len);
  
  return js_mkundef();
}

// localStorage.clear()
static jsval_t js_localstorage_clear(struct js *js, jsval_t *args, int nargs) {
  CHECK_FILE_SET(js);
  (void)args; (void)nargs;
  storage_clear();
  storage_save();
  return js_mkundef();
}

// localStorage.key(index)
static jsval_t js_localstorage_key(struct js *js, jsval_t *args, int nargs) {
  CHECK_FILE_SET(js);
  
  if (nargs < 1) {
    return js_mkerr(js, "Failed to execute 'key' on 'Storage': 1 argument required");
  }
  
  if (vtype(args[0]) != T_NUM) {
    return js_mknull();
  }
  
  double idx = js_getnum(args[0]);
  if (idx < 0) return js_mknull();
  
  size_t index = (size_t)idx;
  char *key = storage_key(index);
  
  if (!key) return js_mknull();
  
  return js_mkstr(js, key, strlen(key));
}

// localStorage.length
static jsval_t js_localstorage_length(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  CHECK_FILE_SET(js)
  return js_mknum((double)storage_length());
}

// localStorage.setFile(path)
static jsval_t js_localstorage_setFile(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Failed to execute 'setFile' on 'Storage': 1 argument required");
  }
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  
  if (storage_file_path) {
    free(storage_file_path);
  }
  
  storage_file_path = malloc(path_len + 1);
  if (!storage_file_path) {
    return js_mkerr(js, "Failed to allocate memory for file path");
  }
  
  memcpy(storage_file_path, path, path_len);
  storage_file_path[path_len] = '\0';
  
  // Load existing data from the new file
  storage_load();
  
  return js_mkundef();
}

void init_localstorage_module() {
  struct js *js = rt->js;
  
  jsval_t glob = js_glob(js);
  const char *file_path = rt->ls_fp;
  
  if (file_path) {
    storage_file_path = strdup(file_path);
    storage_load();
  }
  
  jsval_t storage_obj = js_mkobj(js);
  
  js_set(js, storage_obj, "setItem", js_mkfun(js_localstorage_setItem));
  js_set(js, storage_obj, "getItem", js_mkfun(js_localstorage_getItem));
  js_set(js, storage_obj, "removeItem", js_mkfun(js_localstorage_removeItem));
  js_set(js, storage_obj, "clear", js_mkfun(js_localstorage_clear));
  js_set(js, storage_obj, "key", js_mkfun(js_localstorage_key));
  js_set(js, storage_obj, "setFile", js_mkfun(js_localstorage_setFile));
  
  jsval_t length_getter = js_mkfun(js_localstorage_length);
  js_set_getter_desc(js, storage_obj, "length", 6, length_getter, JS_DESC_E);
  
  js_set(js, storage_obj, get_toStringTag_sym_key(), js_mkstr(js, "Storage", 7));
  js_set(js, glob, "localStorage", storage_obj);
}
