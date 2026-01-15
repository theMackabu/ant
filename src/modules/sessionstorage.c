#include <stdlib.h>
#include <string.h>
#include <uthash.h>

#include "ant.h"
#include "arena.h"
#include "runtime.h"

#include "modules/symbol.h"
#include "modules/sessionstorage.h"

typedef struct storage_entry {
  char *key;
  char *value;
  UT_hash_handle hh;
} storage_entry_t;

static storage_entry_t *session_storage = NULL;

static void storage_set_item(const char *key, size_t key_len, const char *value, size_t value_len) {
  storage_entry_t *entry = NULL;
  
  HASH_FIND(hh, session_storage, key, key_len, entry);
  
  if (entry) {
    HASH_DEL(session_storage, entry);
    ANT_GC_FREE(entry);
  }
  
  entry = ANT_GC_MALLOC(sizeof(storage_entry_t) + key_len + 1 + value_len + 1);
  if (!entry) return;
  
  entry->key = (char *)(entry + 1);
  entry->value = entry->key + key_len + 1;
  
  memcpy(entry->key, key, key_len);
  entry->key[key_len] = '\0';
  memcpy(entry->value, value, value_len);
  entry->value[value_len] = '\0';
  
  HASH_ADD_KEYPTR(hh, session_storage, entry->key, key_len, entry);
}

static char *storage_get_item(const char *key, size_t key_len) {
  storage_entry_t *entry = NULL;
  HASH_FIND(hh, session_storage, key, key_len, entry);
  return entry ? entry->value : NULL;
}

static void storage_remove_item(const char *key, size_t key_len) {
  storage_entry_t *entry = NULL;
  HASH_FIND(hh, session_storage, key, key_len, entry);
  
  if (entry) {
    HASH_DEL(session_storage, entry);
    ANT_GC_FREE(entry);
  }
}

static void storage_clear(void) {
  storage_entry_t *entry, *tmp;
  HASH_ITER(hh, session_storage, entry, tmp) {
    HASH_DEL(session_storage, entry);
    ANT_GC_FREE(entry);
  }
}

static size_t storage_length(void) {
  return HASH_COUNT(session_storage);
}

static char *storage_key(size_t index) {
  storage_entry_t *entry;
  size_t i = 0;
  
  for (entry = session_storage; entry != NULL; entry = entry->hh.next) {
    if (i == index) return entry->key;
    i++;
  }
  
  return NULL;
}

// sessionStorage.setItem(key, value)
static jsval_t js_sessionstorage_setItem(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) {
    return js_mkerr(js, "Failed to execute 'setItem' on 'Storage': 2 arguments required");
  }
  
  size_t key_len, value_len;
  char *key = js_getstr(js, args[0], &key_len);
  char *value = js_getstr(js, js_tostring_val(js, args[1]), &value_len);
  
  storage_set_item(key, key_len, value, value_len);
  
  return js_mkundef();
}

// sessionStorage.getItem(key)
static jsval_t js_sessionstorage_getItem(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Failed to execute 'getItem' on 'Storage': 1 argument required");
  }
  
  size_t key_len;
  char *key = js_getstr(js, args[0], &key_len);
  char *value = storage_get_item(key, key_len);
  
  if (!value) return js_mknull();
  
  return js_mkstr(js, value, strlen(value));
}

// sessionStorage.removeItem(key)
static jsval_t js_sessionstorage_removeItem(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Failed to execute 'removeItem' on 'Storage': 1 argument required");
  }
  
  size_t key_len;
  char *key = js_getstr(js, args[0], &key_len);
  storage_remove_item(key, key_len);
  
  return js_mkundef();
}

// sessionStorage.clear()
static jsval_t js_sessionstorage_clear(struct js *js, jsval_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  storage_clear();
  return js_mkundef();
}

// sessionStorage.key(index)
static jsval_t js_sessionstorage_key(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "Failed to execute 'key' on 'Storage': 1 argument required");
  }
  
  if (js_type(args[0]) != JS_NUM) {
    return js_mknull();
  }
  
  double idx = js_getnum(args[0]);
  if (idx < 0) return js_mknull();
  
  size_t index = (size_t)idx;
  char *key = storage_key(index);
  
  if (!key) return js_mknull();
  
  return js_mkstr(js, key, strlen(key));
}

// sessionStorage.length
static jsval_t js_sessionstorage_length(struct js *js, jsval_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  return js_mknum((double)storage_length());
}

void init_sessionstorage_module(void) {
  struct js *js = rt->js;
  
  jsval_t glob = js_glob(js);
  jsval_t storage_obj = js_mkobj(js);
  
  js_set(js, storage_obj, "setItem", js_mkfun(js_sessionstorage_setItem));
  js_set(js, storage_obj, "getItem", js_mkfun(js_sessionstorage_getItem));
  js_set(js, storage_obj, "removeItem", js_mkfun(js_sessionstorage_removeItem));
  js_set(js, storage_obj, "clear", js_mkfun(js_sessionstorage_clear));
  js_set(js, storage_obj, "key", js_mkfun(js_sessionstorage_key));
  
  jsval_t length_getter = js_mkfun(js_sessionstorage_length);
  js_set_getter_desc(js, storage_obj, "length", 6, length_getter, JS_DESC_E);
  
  js_set(js, storage_obj, get_toStringTag_sym_key(), ANT_STRING("Storage"));
  js_set(js, glob, "sessionStorage", storage_obj);
}
