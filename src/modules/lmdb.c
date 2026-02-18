#include "ant.h"
#include "arena.h"
#include "errors.h"
#include "internal.h"
#include "modules/lmdb.h"
#include "modules/buffer.h"
#include "modules/symbol.h"

#include <lmdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct lmdb_env_handle {
  MDB_env *env;
  bool closed;
  bool read_only;
  char *path;
  lmdb_db_handle_t *db_head;
  lmdb_txn_handle_t *txn_head;
  lmdb_env_handle_t *next_global;
};

struct lmdb_db_handle {
  MDB_dbi dbi;
  bool closed;
  char *name;
  lmdb_env_handle_t *env;
  lmdb_db_handle_t *next_in_env;
  lmdb_db_handle_t *next_global;
};

struct lmdb_txn_handle {
  MDB_txn *txn;
  bool closed;
  bool read_only;
  lmdb_env_handle_t *env;
  lmdb_txn_handle_t *next_in_env;
  lmdb_txn_handle_t *next_global;
};

struct lmdb_env_ref {
  jsval_t obj;
  lmdb_env_handle_t *env;
  lmdb_env_ref_t *next;
};

struct lmdb_db_ref {
  jsval_t obj;
  lmdb_db_handle_t *db;
  lmdb_db_ref_t *next;
};

struct lmdb_txn_ref {
  jsval_t obj;
  lmdb_txn_handle_t *txn;
  lmdb_txn_ref_t *next;
};

static lmdb_js_types_t lmdb_types = {0};
static lmdb_env_handle_t *env_handles = NULL;
static lmdb_db_handle_t *db_handles = NULL;
static lmdb_txn_handle_t *txn_handles = NULL;
static lmdb_env_ref_t *env_refs = NULL;
static lmdb_db_ref_t *db_refs = NULL;
static lmdb_txn_ref_t *txn_refs = NULL;

static jsval_t make_env_obj(struct js *js, lmdb_env_handle_t *env);
static jsval_t make_db_obj(struct js *js, lmdb_db_handle_t *db);
static jsval_t make_txn_obj(struct js *js, lmdb_txn_handle_t *txn);

static void list_remove_db(lmdb_env_handle_t *env, lmdb_db_handle_t *target) {
  if (!env || !target) return;
  lmdb_db_handle_t **cur = &env->db_head;
  while (*cur) {
    if (*cur == target) {
      *cur = target->next_in_env;
      target->next_in_env = NULL;
      return;
    }
    cur = &(*cur)->next_in_env;
  }
}

static void list_remove_txn(lmdb_env_handle_t *env, lmdb_txn_handle_t *target) {
  if (!env || !target) return;
  lmdb_txn_handle_t **cur = &env->txn_head;
  while (*cur) {
    if (*cur == target) {
      *cur = target->next_in_env;
      target->next_in_env = NULL;
      return;
    }
    cur = &(*cur)->next_in_env;
  }
}

static void register_env_ref(jsval_t obj, lmdb_env_handle_t *env) {
  lmdb_env_ref_t *ref = ant_calloc(sizeof(lmdb_env_ref_t));
  if (!ref) return;
  ref->obj = obj;
  ref->env = env;
  ref->next = env_refs;
  env_refs = ref;
}

static void register_db_ref(jsval_t obj, lmdb_db_handle_t *db) {
  lmdb_db_ref_t *ref = ant_calloc(sizeof(lmdb_db_ref_t));
  if (!ref) return;
  ref->obj = obj;
  ref->db = db;
  ref->next = db_refs;
  db_refs = ref;
}

static void register_txn_ref(jsval_t obj, lmdb_txn_handle_t *txn) {
  lmdb_txn_ref_t *ref = ant_calloc(sizeof(lmdb_txn_ref_t));
  if (!ref) return;
  ref->obj = obj;
  ref->txn = txn;
  ref->next = txn_refs;
  txn_refs = ref;
}

static lmdb_env_handle_t *find_env_by_obj(jsval_t obj) {
  for (lmdb_env_ref_t *ref = env_refs; ref; ref = ref->next)
    if (ref->obj == obj) return ref->env;
  return NULL;
}

static lmdb_db_handle_t *find_db_by_obj(jsval_t obj) {
  for (lmdb_db_ref_t *ref = db_refs; ref; ref = ref->next)
    if (ref->obj == obj) return ref->db;
  return NULL;
}

static lmdb_txn_handle_t *find_txn_by_obj(jsval_t obj) {
  for (lmdb_txn_ref_t *ref = txn_refs; ref; ref = ref->next)
    if (ref->obj == obj) return ref->txn;
  return NULL;
}

static void unregister_env_ref_by_obj(jsval_t obj) {
  lmdb_env_ref_t **cur = &env_refs;
  while (*cur) {
    if ((*cur)->obj == obj) {
      lmdb_env_ref_t *next = (*cur)->next;
      free(*cur);
      *cur = next;
      return;
    }
    cur = &(*cur)->next;
  }
}

static void unregister_db_ref_by_obj(jsval_t obj) {
  lmdb_db_ref_t **cur = &db_refs;
  while (*cur) {
    if ((*cur)->obj == obj) {
      lmdb_db_ref_t *next = (*cur)->next;
      free(*cur);
      *cur = next;
      return;
    }
    cur = &(*cur)->next;
  }
}

static void unregister_txn_ref_by_obj(jsval_t obj) {
  lmdb_txn_ref_t **cur = &txn_refs;
  while (*cur) {
    if ((*cur)->obj == obj) {
      lmdb_txn_ref_t *next = (*cur)->next;
      free(*cur);
      *cur = next;
      return;
    }
    cur = &(*cur)->next;
  }
}

static void unregister_db_refs_by_env(lmdb_env_handle_t *env) {
  lmdb_db_ref_t **cur = &db_refs;
  while (*cur) {
    if ((*cur)->db && (*cur)->db->env == env) {
      lmdb_db_ref_t *next = (*cur)->next;
      free(*cur);
      *cur = next;
      continue;
    }
    cur = &(*cur)->next;
  }
}

static void unregister_txn_refs_by_env(lmdb_env_handle_t *env) {
  lmdb_txn_ref_t **cur = &txn_refs;
  while (*cur) {
    if ((*cur)->txn && (*cur)->txn->env == env) {
      lmdb_txn_ref_t *next = (*cur)->next;
      free(*cur);
      *cur = next;
      continue;
    }
    cur = &(*cur)->next;
  }
}

static void env_handle_close(lmdb_env_handle_t *env) {
  if (!env || env->closed) return;

  lmdb_txn_handle_t *txn = env->txn_head;
  while (txn) {
    if (!txn->closed && txn->txn) {
      mdb_txn_abort(txn->txn);
      txn->txn = NULL;
      txn->closed = true;
    }
    txn = txn->next_in_env;
  }

  lmdb_db_handle_t *db = env->db_head;
  while (db) {
    if (!db->closed) {
      mdb_dbi_close(env->env, db->dbi);
      db->closed = true;
    }
    db = db->next_in_env;
  }

  mdb_env_close(env->env);
  env->env = NULL;
  env->closed = true;
  env->db_head = NULL;
  env->txn_head = NULL;

  unregister_db_refs_by_env(env);
  unregister_txn_refs_by_env(env);
}

static lmdb_env_handle_t *get_env_handle(struct js *js, jsval_t obj, bool open_required) {
  lmdb_env_handle_t *env = find_env_by_obj(obj);
  if (!env) return NULL;
  if (open_required && env->closed) return NULL;
  return env;
}

static lmdb_db_handle_t *get_db_handle(struct js *js, jsval_t obj, bool open_required) {
  lmdb_db_handle_t *db = find_db_by_obj(obj);
  if (!db) return NULL;
  if (open_required && (db->closed || !db->env || db->env->closed)) return NULL;
  return db;
}

static lmdb_txn_handle_t *get_txn_handle(struct js *js, jsval_t obj, bool open_required) {
  lmdb_txn_handle_t *txn = find_txn_by_obj(obj);
  if (!txn) return NULL;
  if (open_required && (txn->closed || !txn->txn)) return NULL;
  return txn;
}

static bool option_bool(struct js *js, jsval_t options, const char *key, bool fallback) {
  if (vtype(options) != T_OBJ) return fallback;
  jsval_t val = js_get(js, options, key);
  if (vtype(val) == T_UNDEF) return fallback;
  return js_truthy(js, val);
}

static unsigned int option_uint(struct js *js, jsval_t options, const char *key, unsigned int fallback) {
  if (vtype(options) != T_OBJ) return fallback;
  jsval_t val = js_get(js, options, key);
  if (vtype(val) != T_NUM) return fallback;
  double n = js_getnum(val);
  if (n < 0.0) return fallback;
  return (unsigned int)n;
}

static size_t option_size(struct js *js, jsval_t options, const char *key, size_t fallback) {
  if (vtype(options) != T_OBJ) return fallback;
  jsval_t val = js_get(js, options, key);
  if (vtype(val) != T_NUM) return fallback;
  double n = js_getnum(val);
  if (n < 0.0) return fallback;
  return (size_t)n;
}

static bool js_to_mdb_val(struct js *js, jsval_t input, MDB_val *out) {
  if (vtype(input) == T_STR) {
    size_t len = 0;
    const char *str = js_getstr(js, input, &len);
    if (!str) return false;
    out->mv_data = (void *)str;
    out->mv_size = len;
    return true;
  }

  if (vtype(input) == T_OBJ) {
    jsval_t slot = js_get_slot(js, input, SLOT_BUFFER);

    if (vtype(slot) == T_TYPEDARRAY) {
      TypedArrayData *ta = (TypedArrayData *)js_gettypedarray(slot);
      if (!ta || !ta->buffer || !ta->buffer->data) return false;
      out->mv_data = (void *)(ta->buffer->data + ta->byte_offset);
      out->mv_size = ta->byte_length;
      return true;
    }

    if (vtype(slot) == T_NUM) {
      ArrayBufferData *ab = (ArrayBufferData *)(uintptr_t)js_getnum(slot);
      if (!ab || !ab->data) return false;
      out->mv_data = (void *)ab->data;
      out->mv_size = ab->length;
      return true;
    }
  }

  return false;
}

static jsval_t mdb_val_to_js(struct js *js, MDB_val *val, bool as_string) {
  if (as_string) {
    return js_mkstr(js, val->mv_data, val->mv_size);
  }

  ArrayBufferData *ab = create_array_buffer_data(val->mv_size);
  if (!ab) return js_mkerr(js, "Failed to allocate LMDB read buffer");

  if (val->mv_size > 0 && val->mv_data) {
    memcpy(ab->data, val->mv_data, val->mv_size);
  }

  jsval_t out = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, ab->length, "Uint8Array");
  if (vtype(out) == T_ERR) free_array_buffer_data(ab);
  return out;
}

static bool parse_get_encoding(struct js *js, jsval_t encoding, bool *as_string) {
  if (vtype(encoding) == T_UNDEF) return true;
  if (vtype(encoding) != T_STR) return false;

  size_t len = 0;
  const char *mode = js_getstr(js, encoding, &len);
  if (!mode) return false;

  if (len == 5 && memcmp(mode, "bytes", 5) == 0) {
    *as_string = false;
    return true;
  }
  if (len == 4 && memcmp(mode, "utf8", 4) == 0) {
    *as_string = true;
    return true;
  }

  return false;
}

static jsval_t lmdb_open(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "lmdb.open(path, options?) requires a string path");
  }

  size_t path_len = 0;
  const char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) {
    return js_mkerr(js, "lmdb.open path cannot be empty");
  }

  jsval_t options = nargs > 1 ? args[1] : js_mkundef();

  bool read_only = option_bool(js, options, "readOnly", false);
  bool no_subdir = option_bool(js, options, "noSubdir", false);
  bool no_sync = option_bool(js, options, "noSync", false);
  bool no_meta_sync = option_bool(js, options, "noMetaSync", false);
  bool no_read_ahead = option_bool(js, options, "noReadAhead", false);
  bool no_lock = option_bool(js, options, "noLock", false);
  bool write_map = option_bool(js, options, "writeMap", false);
  bool map_async = option_bool(js, options, "mapAsync", false);

  size_t map_size = option_size(js, options, "mapSize", 0);
  unsigned int max_readers = option_uint(js, options, "maxReaders", 0);
  unsigned int max_dbs = option_uint(js, options, "maxDbs", 0);
  unsigned int mode = option_uint(js, options, "mode", 0644U);

  unsigned int flags = 0;
  if (read_only) flags |= MDB_RDONLY;
  if (no_subdir) flags |= MDB_NOSUBDIR;
  if (no_sync) flags |= MDB_NOSYNC;
  if (no_meta_sync) flags |= MDB_NOMETASYNC;
  if (no_read_ahead) flags |= MDB_NORDAHEAD;
  if (no_lock) flags |= MDB_NOLOCK;
  if (write_map) flags |= MDB_WRITEMAP;
  if (map_async) flags |= MDB_MAPASYNC;

  MDB_env *env = NULL;
  int rc = mdb_env_create(&env);
  if (rc != 0) return js_mkerr(js, "lmdb_env_create failed: %s", mdb_strerror(rc));

  if (map_size > 0) {
    rc = mdb_env_set_mapsize(env, map_size);
    if (rc != 0) {
      mdb_env_close(env);
      return js_mkerr(js, "lmdb_env_set_mapsize failed: %s", mdb_strerror(rc));
    }
  }

  if (max_readers > 0) {
    rc = mdb_env_set_maxreaders(env, max_readers);
    if (rc != 0) {
      mdb_env_close(env);
      return js_mkerr(js, "lmdb_env_set_maxreaders failed: %s", mdb_strerror(rc));
    }
  }

  if (max_dbs > 0) {
    rc = mdb_env_set_maxdbs(env, max_dbs);
    if (rc != 0) {
      mdb_env_close(env);
      return js_mkerr(js, "lmdb_env_set_maxdbs failed: %s", mdb_strerror(rc));
    }
  }

  rc = mdb_env_open(env, path, flags, (mdb_mode_t)mode);
  if (rc != 0) {
    mdb_env_close(env);
    return js_mkerr(js, "lmdb_env_open('%s') failed: %s", path, mdb_strerror(rc));
  }

  lmdb_env_handle_t *handle = ant_calloc(sizeof(lmdb_env_handle_t));
  if (!handle) {
    mdb_env_close(env);
    return js_mkerr(js, "Out of memory");
  }

  handle->env = env;
  handle->closed = false;
  handle->read_only = read_only;
  handle->path = strndup(path, path_len);
  handle->next_global = env_handles;
  env_handles = handle;

  return make_env_obj(js, handle);
}

static jsval_t lmdb_env_open_db(struct js *js, jsval_t *args, int nargs) {
  lmdb_env_handle_t *env = get_env_handle(js, js_getthis(js), true);
  if (!env) return js_mkerr(js, "Invalid or closed LMDB env");

  const char *name = NULL;
  size_t name_len = 0;
  jsval_t options = js_mkundef();

  if (nargs > 0 && vtype(args[0]) == T_STR) {
    name = js_getstr(js, args[0], &name_len);
    if (nargs > 1) options = args[1];
  } else if (nargs > 0 && vtype(args[0]) == T_OBJ) {
    options = args[0];
  }

  bool create = option_bool(js, options, "create", false);
  bool dup_sort = option_bool(js, options, "dupSort", false);
  bool dup_fixed = option_bool(js, options, "dupFixed", false);
  bool integer_key = option_bool(js, options, "integerKey", false);
  bool integer_dup = option_bool(js, options, "integerDup", false);

  if (env->read_only && create) {
    return js_mkerr(js, "Cannot open DB with create=true on a read-only env");
  }

  unsigned int dbi_flags = 0;
  if (create) dbi_flags |= MDB_CREATE;
  if (dup_sort) dbi_flags |= MDB_DUPSORT;
  if (dup_fixed) dbi_flags |= MDB_DUPFIXED;
  if (integer_key) dbi_flags |= MDB_INTEGERKEY;
  if (integer_dup) dbi_flags |= MDB_INTEGERDUP;

  unsigned int txn_flags = env->read_only ? MDB_RDONLY : 0U;
  MDB_txn *txn = NULL;
  int rc = mdb_txn_begin(env->env, NULL, txn_flags, &txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_begin failed: %s", mdb_strerror(rc));

  MDB_dbi dbi = 0;
  rc = mdb_dbi_open(txn, name, dbi_flags, &dbi);
  if (rc != 0) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "lmdb_dbi_open failed: %s", mdb_strerror(rc));
  }

  if (txn_flags & MDB_RDONLY) {
    mdb_txn_abort(txn);
  } else {
    rc = mdb_txn_commit(txn);
    if (rc != 0) return js_mkerr(js, "lmdb_txn_commit failed: %s", mdb_strerror(rc));
  }

  lmdb_db_handle_t *db = ant_calloc(sizeof(lmdb_db_handle_t));
  if (!db) {
    mdb_dbi_close(env->env, dbi);
    return js_mkerr(js, "Out of memory");
  }

  db->dbi = dbi;
  db->closed = false;
  db->env = env;
  db->name = name ? strndup(name, name_len) : NULL;

  db->next_in_env = env->db_head;
  env->db_head = db;
  db->next_global = db_handles;
  db_handles = db;

  return make_db_obj(js, db);
}

static jsval_t lmdb_env_begin_txn(struct js *js, jsval_t *args, int nargs) {
  lmdb_env_handle_t *env = get_env_handle(js, js_getthis(js), true);
  if (!env) return js_mkerr(js, "Invalid or closed LMDB env");

  jsval_t options = nargs > 0 ? args[0] : js_mkundef();
  bool read_only = option_bool(js, options, "readOnly", env->read_only);

  if (env->read_only && !read_only) {
    return js_mkerr(js, "Cannot create read-write transaction on read-only env");
  }

  MDB_txn *txn = NULL;
  int rc = mdb_txn_begin(env->env, NULL, read_only ? MDB_RDONLY : 0U, &txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_begin failed: %s", mdb_strerror(rc));

  lmdb_txn_handle_t *handle = ant_calloc(sizeof(lmdb_txn_handle_t));
  if (!handle) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "Out of memory");
  }

  handle->txn = txn;
  handle->closed = false;
  handle->read_only = read_only;
  handle->env = env;

  handle->next_in_env = env->txn_head;
  env->txn_head = handle;
  handle->next_global = txn_handles;
  txn_handles = handle;

  return make_txn_obj(js, handle);
}

static jsval_t lmdb_env_close_method(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  jsval_t self = js_getthis(js);
  lmdb_env_handle_t *env = get_env_handle(js, self, false);
  if (!env) return js_mkerr(js, "Invalid LMDB env");

  env_handle_close(env);
  unregister_env_ref_by_obj(self);
  return js_mkundef();
}

static jsval_t lmdb_env_sync_method(struct js *js, jsval_t *args, int nargs) {
  lmdb_env_handle_t *env = get_env_handle(js, js_getthis(js), true);
  if (!env) return js_mkerr(js, "Invalid or closed LMDB env");

  bool force = true;
  if (nargs > 0) force = js_truthy(js, args[0]);

  int rc = mdb_env_sync(env->env, force ? 1 : 0);
  if (rc != 0) return js_mkerr(js, "lmdb_env_sync failed: %s", mdb_strerror(rc));
  return js_mkundef();
}

static jsval_t lmdb_env_stat_method(struct js *js, jsval_t *args, int nargs) {
  lmdb_env_handle_t *env = get_env_handle(js, js_getthis(js), true);
  if (!env) return js_mkerr(js, "Invalid or closed LMDB env");

  MDB_txn *txn = NULL;
  int rc = mdb_txn_begin(env->env, NULL, MDB_RDONLY, &txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_begin failed: %s", mdb_strerror(rc));

  MDB_stat stat;
  rc = mdb_stat(txn, 0, &stat);
  mdb_txn_abort(txn);
  if (rc != 0) return js_mkerr(js, "lmdb_stat failed: %s", mdb_strerror(rc));

  jsval_t out = js_mkobj(js);
  js_set(js, out, "psize", js_mknum((double)stat.ms_psize));
  js_set(js, out, "depth", js_mknum((double)stat.ms_depth));
  js_set(js, out, "branchPages", js_mknum((double)stat.ms_branch_pages));
  js_set(js, out, "leafPages", js_mknum((double)stat.ms_leaf_pages));
  js_set(js, out, "overflowPages", js_mknum((double)stat.ms_overflow_pages));
  js_set(js, out, "entries", js_mknum((double)stat.ms_entries));
  
  return out;
}

static jsval_t lmdb_env_info_method(struct js *js, jsval_t *args, int nargs) {
  lmdb_env_handle_t *env = get_env_handle(js, js_getthis(js), true);
  if (!env) return js_mkerr(js, "Invalid or closed LMDB env");

  MDB_envinfo info;
  int rc = mdb_env_info(env->env, &info);
  if (rc != 0) return js_mkerr(js, "lmdb_env_info failed: %s", mdb_strerror(rc));

  jsval_t out = js_mkobj(js);
  js_set(js, out, "mapSize", js_mknum((double)info.me_mapsize));
  js_set(js, out, "lastPgNo", js_mknum((double)info.me_last_pgno));
  js_set(js, out, "lastTxnId", js_mknum((double)info.me_last_txnid));
  js_set(js, out, "maxReaders", js_mknum((double)info.me_maxreaders));
  js_set(js, out, "numReaders", js_mknum((double)info.me_numreaders));
  
  return out;
}

static jsval_t lmdb_txn_get_impl(struct js *js, jsval_t *args, int nargs, bool as_string) {
  if (nargs < 2) return js_mkerr(js, "txn.getBytes/getString(db, key) requires db and key");

  lmdb_txn_handle_t *txn = get_txn_handle(js, js_getthis(js), true);
  if (!txn) return js_mkerr(js, "Invalid or closed LMDB transaction");

  lmdb_db_handle_t *db = get_db_handle(js, args[0], true);
  if (!db) return js_mkerr(js, "Invalid or closed LMDB database handle");
  if (db->env != txn->env) return js_mkerr(js, "Database and transaction belong to different envs");

  MDB_val key;
  if (!js_to_mdb_val(js, args[1], &key)) {
    return js_mkerr(js, "LMDB key must be string, ArrayBuffer, or TypedArray");
  }

  MDB_val value;
  int rc = mdb_get(txn->txn, db->dbi, &key, &value);
  if (rc == MDB_NOTFOUND) return js_mkundef();
  if (rc != 0) return js_mkerr(js, "lmdb_get failed: %s", mdb_strerror(rc));

  return mdb_val_to_js(js, &value, as_string);
}

static jsval_t lmdb_txn_get(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "txn.get(db, key, encoding?) requires db and key");
  bool as_string = false;
  if (nargs > 2 && !parse_get_encoding(js, args[2], &as_string)) {
    return js_mkerr(js, "txn.get encoding must be 'utf8' or 'bytes'");
  }
  return lmdb_txn_get_impl(js, args, nargs, as_string);
}

static jsval_t lmdb_txn_get_bytes(struct js *js, jsval_t *args, int nargs) {
  return lmdb_txn_get_impl(js, args, nargs, false);
}

static jsval_t lmdb_txn_get_string(struct js *js, jsval_t *args, int nargs) {
  return lmdb_txn_get_impl(js, args, nargs, true);
}

static jsval_t lmdb_txn_put(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 3) return js_mkerr(js, "txn.put(db, key, value, options?) requires db, key, and value");

  lmdb_txn_handle_t *txn = get_txn_handle(js, js_getthis(js), true);
  if (!txn) return js_mkerr(js, "Invalid or closed LMDB transaction");
  if (txn->read_only) return js_mkerr(js, "Cannot write in read-only transaction");

  lmdb_db_handle_t *db = get_db_handle(js, args[0], true);
  if (!db) return js_mkerr(js, "Invalid or closed LMDB database handle");
  if (db->env != txn->env) return js_mkerr(js, "Database and transaction belong to different envs");

  MDB_val key;
  MDB_val value;
  if (!js_to_mdb_val(js, args[1], &key)) return js_mkerr(js, "LMDB key must be string, ArrayBuffer, or TypedArray");
  if (!js_to_mdb_val(js, args[2], &value)) return js_mkerr(js, "LMDB value must be string, ArrayBuffer, or TypedArray");

  jsval_t options = nargs > 3 ? args[3] : js_mkundef();
  unsigned int flags = 0;
  if (option_bool(js, options, "noOverwrite", false)) flags |= MDB_NOOVERWRITE;
  if (option_bool(js, options, "noDupData", false)) flags |= MDB_NODUPDATA;
  if (option_bool(js, options, "append", false)) flags |= MDB_APPEND;
  if (option_bool(js, options, "appendDup", false)) flags |= MDB_APPENDDUP;

  int rc = mdb_put(txn->txn, db->dbi, &key, &value, flags);
  if (rc == MDB_KEYEXIST) return js_false;
  if (rc != 0) return js_mkerr(js, "lmdb_put failed: %s", mdb_strerror(rc));
  return js_true;
}

static jsval_t lmdb_txn_del(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "txn.del(db, key, value?) requires db and key");

  lmdb_txn_handle_t *txn = get_txn_handle(js, js_getthis(js), true);
  if (!txn) return js_mkerr(js, "Invalid or closed LMDB transaction");
  if (txn->read_only) return js_mkerr(js, "Cannot delete in read-only transaction");

  lmdb_db_handle_t *db = get_db_handle(js, args[0], true);
  if (!db) return js_mkerr(js, "Invalid or closed LMDB database handle");
  if (db->env != txn->env) return js_mkerr(js, "Database and transaction belong to different envs");

  MDB_val key;
  if (!js_to_mdb_val(js, args[1], &key)) return js_mkerr(js, "LMDB key must be string, ArrayBuffer, or TypedArray");

  MDB_val value;
  MDB_val *value_ptr = NULL;
  if (nargs > 2 && vtype(args[2]) != T_UNDEF) {
    if (!js_to_mdb_val(js, args[2], &value)) return js_mkerr(js, "LMDB value must be string, ArrayBuffer, or TypedArray");
    value_ptr = &value;
  }

  int rc = mdb_del(txn->txn, db->dbi, &key, value_ptr);
  if (rc == MDB_NOTFOUND) return js_false;
  if (rc != 0) return js_mkerr(js, "lmdb_del failed: %s", mdb_strerror(rc));
  return js_true;
}

static jsval_t lmdb_txn_commit(struct js *js, jsval_t *args, int nargs) {
  jsval_t self = js_getthis(js);
  lmdb_txn_handle_t *txn = get_txn_handle(js, self, true);
  if (!txn) return js_mkerr(js, "Invalid or closed LMDB transaction");

  int rc = txn->read_only ? MDB_SUCCESS : mdb_txn_commit(txn->txn);
  if (txn->read_only) mdb_txn_abort(txn->txn);

  txn->txn = NULL;
  txn->closed = true;
  if (txn->env) list_remove_txn(txn->env, txn);
  unregister_txn_ref_by_obj(self);

  if (rc != 0) return js_mkerr(js, "lmdb_txn_commit failed: %s", mdb_strerror(rc));
  return js_mkundef();
}

static jsval_t lmdb_txn_abort(struct js *js, jsval_t *args, int nargs) {
  jsval_t self = js_getthis(js);
  lmdb_txn_handle_t *txn = get_txn_handle(js, self, false);
  if (!txn) return js_mkerr(js, "Invalid LMDB transaction");
  if (txn->closed || !txn->txn) {
    unregister_txn_ref_by_obj(self);
    return js_mkundef();
  }

  mdb_txn_abort(txn->txn);
  txn->txn = NULL;
  txn->closed = true;
  if (txn->env) list_remove_txn(txn->env, txn);
  unregister_txn_ref_by_obj(self);
  return js_mkundef();
}

static jsval_t lmdb_db_get_impl(struct js *js, jsval_t *args, int nargs, bool as_string) {
  if (nargs < 1) return js_mkerr(js, "db.getBytes/getString(key) requires key");
  lmdb_db_handle_t *db = get_db_handle(js, js_getthis(js), true);
  if (!db) return js_mkerr(js, "Invalid or closed LMDB database handle");

  MDB_txn *txn = NULL;
  int rc = mdb_txn_begin(db->env->env, NULL, MDB_RDONLY, &txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_begin failed: %s", mdb_strerror(rc));

  MDB_val key;
  MDB_val value;
  if (!js_to_mdb_val(js, args[0], &key)) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "LMDB key must be string, ArrayBuffer, or TypedArray");
  }

  rc = mdb_get(txn, db->dbi, &key, &value);
  if (rc == MDB_NOTFOUND) {
    mdb_txn_abort(txn);
    return js_mkundef();
  }
  if (rc != 0) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "lmdb_get failed: %s", mdb_strerror(rc));
  }

  jsval_t out = mdb_val_to_js(js, &value, as_string);
  mdb_txn_abort(txn);
  return out;
}

static jsval_t lmdb_db_get(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "db.get(key, encoding?) requires key");
  bool as_string = false;
  if (nargs > 1 && !parse_get_encoding(js, args[1], &as_string)) {
    return js_mkerr(js, "db.get encoding must be 'utf8' or 'bytes'");
  }
  return lmdb_db_get_impl(js, args, nargs, as_string);
}

static jsval_t lmdb_db_get_bytes(struct js *js, jsval_t *args, int nargs) {
  return lmdb_db_get_impl(js, args, nargs, false);
}

static jsval_t lmdb_db_get_string(struct js *js, jsval_t *args, int nargs) {
  return lmdb_db_get_impl(js, args, nargs, true);
}

static jsval_t lmdb_db_put(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "db.put(key, value, options?) requires key and value");
  lmdb_db_handle_t *db = get_db_handle(js, js_getthis(js), true);
  if (!db) return js_mkerr(js, "Invalid or closed LMDB database handle");
  if (db->env->read_only) return js_mkerr(js, "Cannot write on read-only LMDB env");

  MDB_txn *txn = NULL;
  int rc = mdb_txn_begin(db->env->env, NULL, 0, &txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_begin failed: %s", mdb_strerror(rc));

  MDB_val key;
  MDB_val value;
  if (!js_to_mdb_val(js, args[0], &key) || !js_to_mdb_val(js, args[1], &value)) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "LMDB key/value must be string, ArrayBuffer, or TypedArray");
  }

  jsval_t options = nargs > 2 ? args[2] : js_mkundef();
  unsigned int flags = 0;
  if (option_bool(js, options, "noOverwrite", false)) flags |= MDB_NOOVERWRITE;
  if (option_bool(js, options, "noDupData", false)) flags |= MDB_NODUPDATA;
  if (option_bool(js, options, "append", false)) flags |= MDB_APPEND;
  if (option_bool(js, options, "appendDup", false)) flags |= MDB_APPENDDUP;

  rc = mdb_put(txn, db->dbi, &key, &value, flags);
  if (rc == MDB_KEYEXIST) {
    mdb_txn_abort(txn);
    return js_false;
  }
  if (rc != 0) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "lmdb_put failed: %s", mdb_strerror(rc));
  }

  rc = mdb_txn_commit(txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_commit failed: %s", mdb_strerror(rc));
  return js_true;
}

static jsval_t lmdb_db_del(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "db.del(key, value?) requires key");
  lmdb_db_handle_t *db = get_db_handle(js, js_getthis(js), true);
  if (!db) return js_mkerr(js, "Invalid or closed LMDB database handle");
  if (db->env->read_only) return js_mkerr(js, "Cannot delete on read-only LMDB env");

  MDB_txn *txn = NULL;
  int rc = mdb_txn_begin(db->env->env, NULL, 0, &txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_begin failed: %s", mdb_strerror(rc));

  MDB_val key;
  if (!js_to_mdb_val(js, args[0], &key)) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "LMDB key must be string, ArrayBuffer, or TypedArray");
  }

  MDB_val value;
  MDB_val *value_ptr = NULL;
  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
    if (!js_to_mdb_val(js, args[1], &value)) {
      mdb_txn_abort(txn);
      return js_mkerr(js, "LMDB value must be string, ArrayBuffer, or TypedArray");
    }
    value_ptr = &value;
  }

  rc = mdb_del(txn, db->dbi, &key, value_ptr);
  if (rc == MDB_NOTFOUND) {
    mdb_txn_abort(txn);
    return js_false;
  }
  if (rc != 0) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "lmdb_del failed: %s", mdb_strerror(rc));
  }

  rc = mdb_txn_commit(txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_commit failed: %s", mdb_strerror(rc));
  return js_true;
}

static jsval_t lmdb_db_clear(struct js *js, jsval_t *args, int nargs) {
  lmdb_db_handle_t *db = get_db_handle(js, js_getthis(js), true);
  if (!db) return js_mkerr(js, "Invalid or closed LMDB database handle");
  if (db->env->read_only) return js_mkerr(js, "Cannot clear on read-only LMDB env");

  MDB_txn *txn = NULL;
  int rc = mdb_txn_begin(db->env->env, NULL, 0, &txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_begin failed: %s", mdb_strerror(rc));

  rc = mdb_drop(txn, db->dbi, 0);
  if (rc != 0) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "lmdb_drop(clear) failed: %s", mdb_strerror(rc));
  }

  rc = mdb_txn_commit(txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_commit failed: %s", mdb_strerror(rc));
  return js_mkundef();
}

static jsval_t lmdb_db_drop(struct js *js, jsval_t *args, int nargs) {
  jsval_t self = js_getthis(js);
  lmdb_db_handle_t *db = get_db_handle(js, self, true);
  if (!db) return js_mkerr(js, "Invalid or closed LMDB database handle");
  if (db->env->read_only) return js_mkerr(js, "Cannot drop on read-only LMDB env");

  bool del_db = true;
  if (nargs > 0 && vtype(args[0]) == T_OBJ) {
    del_db = option_bool(js, args[0], "delete", true);
  } else if (nargs > 0 && vtype(args[0]) != T_UNDEF) {
    del_db = js_truthy(js, args[0]);
  }

  MDB_txn *txn = NULL;
  int rc = mdb_txn_begin(db->env->env, NULL, 0, &txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_begin failed: %s", mdb_strerror(rc));

  rc = mdb_drop(txn, db->dbi, del_db ? 1 : 0);
  if (rc != 0) {
    mdb_txn_abort(txn);
    return js_mkerr(js, "lmdb_drop failed: %s", mdb_strerror(rc));
  }

  rc = mdb_txn_commit(txn);
  if (rc != 0) return js_mkerr(js, "lmdb_txn_commit failed: %s", mdb_strerror(rc));

  if (del_db) {
    db->closed = true;
    if (db->env) list_remove_db(db->env, db);
    unregister_db_ref_by_obj(self);
  }
  return js_mkundef();
}

static jsval_t lmdb_db_close(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  jsval_t self = js_getthis(js);
  lmdb_db_handle_t *db = get_db_handle(js, self, false);
  if (!db) return js_mkerr(js, "Invalid LMDB database handle");
  if (db->closed) {
    unregister_db_ref_by_obj(self);
    return js_mkundef();
  }

  if (db->env && !db->env->closed) {
    mdb_dbi_close(db->env->env, db->dbi);
    list_remove_db(db->env, db);
  }

  db->closed = true;
  unregister_db_ref_by_obj(self);
  return js_mkundef();
}

static jsval_t lmdb_strerror_fn(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_NUM) {
    return js_mkerr(js, "lmdb.strerror(code) requires a numeric code");
  }
  int code = (int)js_getnum(args[0]);
  const char *err = mdb_strerror(code);
  return js_mkstr(js, err, strlen(err));
}

static jsval_t lmdb_env_constructor(struct js *js, jsval_t *args, int nargs) {
  return js_mkerr(js, "LMDBEnv cannot be constructed directly; use lmdb.open()");
}

static jsval_t lmdb_db_constructor(struct js *js, jsval_t *args, int nargs) {
  return js_mkerr(js, "LMDBDatabase cannot be constructed directly; use env.openDB()");
}

static jsval_t lmdb_txn_constructor(struct js *js, jsval_t *args, int nargs) {
  return js_mkerr(js, "LMDBTxn cannot be constructed directly; use env.beginTxn()");
}

static void ensure_lmdb_prototypes(struct js *js) {
  if (lmdb_types.ready) return;
  jsval_t object_proto = js->object;

  jsval_t env_ctor_obj = js_mkobj(js);
  jsval_t env_proto = js_mkobj(js);
  
  js_set_proto(js, env_proto, object_proto);
  js_set(js, env_proto, "openDB", js_mkfun(lmdb_env_open_db));
  js_set(js, env_proto, "beginTxn", js_mkfun(lmdb_env_begin_txn));
  js_set(js, env_proto, "close", js_mkfun(lmdb_env_close_method));
  js_set(js, env_proto, "sync", js_mkfun(lmdb_env_sync_method));
  js_set(js, env_proto, "stat", js_mkfun(lmdb_env_stat_method));
  js_set(js, env_proto, "info", js_mkfun(lmdb_env_info_method));
  js_set(js, env_proto, get_toStringTag_sym_key(), js_mkstr(js, "LMDBEnv", 7));
  js_set_slot(js, env_ctor_obj, SLOT_CFUNC, js_mkfun(lmdb_env_constructor));
  js_mkprop_fast(js, env_ctor_obj, "prototype", 9, env_proto);
  js_mkprop_fast(js, env_ctor_obj, "name", 4, ANT_STRING("LMDBEnv"));
  js_set_descriptor(js, env_ctor_obj, "name", 4, 0);

  jsval_t db_ctor_obj = js_mkobj(js);
  jsval_t db_proto = js_mkobj(js);
  
  js_set_proto(js, db_proto, object_proto);
  js_set(js, db_proto, "get", js_mkfun(lmdb_db_get));
  js_set(js, db_proto, "getBytes", js_mkfun(lmdb_db_get_bytes));
  js_set(js, db_proto, "getString", js_mkfun(lmdb_db_get_string));
  js_set(js, db_proto, "put", js_mkfun(lmdb_db_put));
  js_set(js, db_proto, "del", js_mkfun(lmdb_db_del));
  js_set(js, db_proto, "clear", js_mkfun(lmdb_db_clear));
  js_set(js, db_proto, "drop", js_mkfun(lmdb_db_drop));
  js_set(js, db_proto, "close", js_mkfun(lmdb_db_close));
  js_set(js, db_proto, get_toStringTag_sym_key(), js_mkstr(js, "LMDBDatabase", 12));
  js_set_slot(js, db_ctor_obj, SLOT_CFUNC, js_mkfun(lmdb_db_constructor));
  js_mkprop_fast(js, db_ctor_obj, "prototype", 9, db_proto);
  js_mkprop_fast(js, db_ctor_obj, "name", 4, ANT_STRING("LMDBDatabase"));
  js_set_descriptor(js, db_ctor_obj, "name", 4, 0);

  jsval_t txn_ctor_obj = js_mkobj(js);
  jsval_t txn_proto = js_mkobj(js);
  
  js_set_proto(js, txn_proto, object_proto);
  js_set(js, txn_proto, "get", js_mkfun(lmdb_txn_get));
  js_set(js, txn_proto, "getBytes", js_mkfun(lmdb_txn_get_bytes));
  js_set(js, txn_proto, "getString", js_mkfun(lmdb_txn_get_string));
  js_set(js, txn_proto, "put", js_mkfun(lmdb_txn_put));
  js_set(js, txn_proto, "del", js_mkfun(lmdb_txn_del));
  js_set(js, txn_proto, "commit", js_mkfun(lmdb_txn_commit));
  js_set(js, txn_proto, "abort", js_mkfun(lmdb_txn_abort));
  js_set(js, txn_proto, get_toStringTag_sym_key(), js_mkstr(js, "LMDBTxn", 7));
  js_set_slot(js, txn_ctor_obj, SLOT_CFUNC, js_mkfun(lmdb_txn_constructor));
  js_mkprop_fast(js, txn_ctor_obj, "prototype", 9, txn_proto);
  js_mkprop_fast(js, txn_ctor_obj, "name", 4, ANT_STRING("LMDBTxn"));
  js_set_descriptor(js, txn_ctor_obj, "name", 4, 0);

  lmdb_types.env_ctor = js_obj_to_func(env_ctor_obj);
  lmdb_types.db_ctor = js_obj_to_func(db_ctor_obj);
  lmdb_types.txn_ctor = js_obj_to_func(txn_ctor_obj);
  lmdb_types.env_proto = env_proto;
  lmdb_types.db_proto = db_proto;
  lmdb_types.txn_proto = txn_proto;
  lmdb_types.ready = true;
}

static jsval_t make_env_obj(struct js *js, lmdb_env_handle_t *env) {
  ensure_lmdb_prototypes(js);
  jsval_t obj = js_mkobj(js);
  js_set_slot(js, obj, SLOT_DATA, ANT_PTR(env));
  register_env_ref(obj, env);
  if (is_special_object(lmdb_types.env_proto)) js_set_proto(js, obj, lmdb_types.env_proto);
  return obj;
}

static jsval_t make_db_obj(struct js *js, lmdb_db_handle_t *db) {
  ensure_lmdb_prototypes(js);
  jsval_t obj = js_mkobj(js);
  js_set_slot(js, obj, SLOT_DATA, ANT_PTR(db));
  register_db_ref(obj, db);
  if (is_special_object(lmdb_types.db_proto)) js_set_proto(js, obj, lmdb_types.db_proto);
  return obj;
}

static jsval_t make_txn_obj(struct js *js, lmdb_txn_handle_t *txn) {
  ensure_lmdb_prototypes(js);
  jsval_t obj = js_mkobj(js);
  js_set_slot(js, obj, SLOT_DATA, ANT_PTR(txn));
  register_txn_ref(obj, txn);
  if (is_special_object(lmdb_types.txn_proto)) js_set_proto(js, obj, lmdb_types.txn_proto);
  return obj;
}

jsval_t lmdb_library(struct js *js) {
  ensure_lmdb_prototypes(js);
  jsval_t lib = js_mkobj(js);
  js_set(js, lib, "open", js_mkfun(lmdb_open));
  js_set(js, lib, "strerror", js_mkfun(lmdb_strerror_fn));
  js_set(js, lib, "Env", lmdb_types.env_ctor);
  js_set(js, lib, "Database", lmdb_types.db_ctor);
  js_set(js, lib, "Txn", lmdb_types.txn_ctor);

  int major = 0; int minor = 0; int patch = 0;
  const char *version = mdb_version(&major, &minor, &patch);

  js_set(js, lib, "version", js_mkstr(js, version, strlen(version)));
  js_set(js, lib, "versionMajor", js_mknum((double)major));
  js_set(js, lib, "versionMinor", js_mknum((double)minor));
  js_set(js, lib, "versionPatch", js_mknum((double)patch));

  jsval_t constants = js_mkobj(js);
  js_set(js, constants, "NOOVERWRITE", js_mknum((double)MDB_NOOVERWRITE));
  js_set(js, constants, "NODUPDATA", js_mknum((double)MDB_NODUPDATA));
  js_set(js, constants, "APPEND", js_mknum((double)MDB_APPEND));
  js_set(js, constants, "APPENDDUP", js_mknum((double)MDB_APPENDDUP));
  js_set(js, constants, "NOSUBDIR", js_mknum((double)MDB_NOSUBDIR));
  js_set(js, constants, "NOSYNC", js_mknum((double)MDB_NOSYNC));
  js_set(js, constants, "NOMETASYNC", js_mknum((double)MDB_NOMETASYNC));
  js_set(js, constants, "WRITEMAP", js_mknum((double)MDB_WRITEMAP));
  js_set(js, constants, "MAPASYNC", js_mknum((double)MDB_MAPASYNC));
  js_set(js, constants, "NOTLS", js_mknum((double)MDB_NOTLS));
  js_set(js, constants, "NOLOCK", js_mknum((double)MDB_NOLOCK));
  js_set(js, constants, "NORDAHEAD", js_mknum((double)MDB_NORDAHEAD));
  js_set(js, constants, "NOMEMINIT", js_mknum((double)MDB_NOMEMINIT));
  js_set(js, lib, "constants", constants);

  js_set(js, lib, get_toStringTag_sym_key(), js_mkstr(js, "lmdb", 4));
  return lib;
}

void lmdb_gc_update_roots(GC_OP_VAL_ARGS) {
  if (lmdb_types.ready) {
    op_val(ctx, &lmdb_types.env_ctor);
    op_val(ctx, &lmdb_types.db_ctor);
    op_val(ctx, &lmdb_types.txn_ctor);
    op_val(ctx, &lmdb_types.env_proto);
    op_val(ctx, &lmdb_types.db_proto);
    op_val(ctx, &lmdb_types.txn_proto);
  }
  for (lmdb_env_ref_t *ref = env_refs; ref; ref = ref->next) op_val(ctx, &ref->obj);
  for (lmdb_db_ref_t *ref = db_refs; ref; ref = ref->next) op_val(ctx, &ref->obj);
  for (lmdb_txn_ref_t *ref = txn_refs; ref; ref = ref->next) op_val(ctx, &ref->obj);
}

void cleanup_lmdb_module(void) {
  lmdb_txn_handle_t *txn = txn_handles;
  while (txn) {
    if (!txn->closed && txn->txn) {
      mdb_txn_abort(txn->txn);
      txn->txn = NULL;
      txn->closed = true;
    }
    txn = txn->next_global;
  }

  lmdb_db_handle_t *db = db_handles;
  while (db) {
    if (!db->closed && db->env && !db->env->closed) {
      mdb_dbi_close(db->env->env, db->dbi);
      db->closed = true;
    }
    db = db->next_global;
  }

  lmdb_env_handle_t *env = env_handles;
  while (env) {
    if (!env->closed) env_handle_close(env);
    env = env->next_global;
  }

  txn = txn_handles;
  while (txn) {
    lmdb_txn_handle_t *next = txn->next_global;
    free(txn);
    txn = next;
  }
  txn_handles = NULL;

  db = db_handles;
  while (db) {
    lmdb_db_handle_t *next = db->next_global;
    free(db->name);
    free(db);
    db = next;
  }
  db_handles = NULL;

  env = env_handles;
  while (env) {
    lmdb_env_handle_t *next = env->next_global;
    free(env->path);
    free(env);
    env = next;
  }
  env_handles = NULL;

  while (env_refs) {
    lmdb_env_ref_t *next = env_refs->next;
    free(env_refs);
    env_refs = next;
  }

  while (db_refs) {
    lmdb_db_ref_t *next = db_refs->next;
    free(db_refs);
    db_refs = next;
  }

  while (txn_refs) {
    lmdb_txn_ref_t *next = txn_refs->next;
    free(txn_refs);
    txn_refs = next;
  }

  lmdb_types = (lmdb_js_types_t){0};
}
