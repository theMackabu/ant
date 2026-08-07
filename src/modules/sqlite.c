#include <compat.h> // IWYU pragma: keep

#include "ant.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "modules/sqlite.h"
#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/symbol.h"
#include "descriptors.h"
#include "gc/modules.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// node:sqlite-shaped module: DatabaseSync / StatementSync with synchronous
// execution, anonymous and named parameter binding, and typed row values.

#define JS_MAX_SAFE_INTEGER 9007199254740991LL

struct sqlite_db_handle {
  sqlite3 *db;
  bool open;
  char *location;
  sqlite_stmt_handle_t *stmt_head;
  sqlite_db_handle_t *next_global;
};

struct sqlite_stmt_handle {
  sqlite3_stmt *stmt;
  bool finalized;
  bool locked; // held by a live iterator
  bool read_bigints;
  bool allow_bare_named;
  bool allow_unknown_named;
  sqlite_db_handle_t *db;
  sqlite_stmt_handle_t *next_in_db;
  sqlite_stmt_handle_t *next_global;
};

typedef struct sqlite_iter_state {
  sqlite_stmt_handle_t *stmt;
  bool done;
} sqlite_iter_state_t;

typedef struct {
  ant_value_t db_ctor;
  ant_value_t stmt_ctor;
  ant_value_t db_proto;
  ant_value_t stmt_proto;
  ant_value_t iter_proto;
  bool ready;
} sqlite_js_types_t;

static sqlite_js_types_t sqlite_types = {0};
static sqlite_db_handle_t *db_handles = NULL;
static sqlite_stmt_handle_t *stmt_handles = NULL;
static bool sqlite_initialized = false;

enum {
  SQLITE_DB_NATIVE_TAG = 0x53514442u,  // SQDB
  SQLITE_STMT_NATIVE_TAG = 0x53515354u, // SQST
  SQLITE_ITER_NATIVE_TAG = 0x53514954u  // SQIT
};

static void ensure_sqlite_initialized(void) {
  if (!sqlite_initialized) {
    sqlite3_initialize();
    sqlite_initialized = true;
  }
}

static ant_value_t sqlite_error(ant_t *js, sqlite3 *db, const char *prefix) {
  int errcode = db ? sqlite3_extended_errcode(db) : SQLITE_ERROR;
  const char *errstr = db ? sqlite3_errmsg(db) : "unknown error";

  ant_value_t props = js_mkobj(js);
  js_set(js, props, "code", ANT_STRING("ERR_SQLITE_ERROR"));
  js_set(js, props, "errcode", js_mknum((double)errcode));
  js_set(js, props, "errstr", js_mkstr(js, errstr, strlen(errstr)));
  return js_mkerr_props(js, JS_ERR_GENERIC, props, "%s: %s", prefix, errstr);
}

static void list_remove_stmt(sqlite_db_handle_t *db, sqlite_stmt_handle_t *target) {
  if (!db || !target) return;
  sqlite_stmt_handle_t **cur = &db->stmt_head;
  while (*cur) {
    if (*cur == target) {
      *cur = target->next_in_db;
      target->next_in_db = NULL;
      return;
    }
    cur = &(*cur)->next_in_db;
  }
}

static void stmt_handle_finalize(sqlite_stmt_handle_t *stmt) {
  if (!stmt || stmt->finalized) return;
  if (stmt->stmt) {
    sqlite3_finalize(stmt->stmt);
    stmt->stmt = NULL;
  }
  stmt->finalized = true;
  stmt->locked = false;
  list_remove_stmt(stmt->db, stmt);
}

static void db_handle_close(sqlite_db_handle_t *db) {
  if (!db || !db->open) return;

  sqlite_stmt_handle_t *stmt = db->stmt_head;
  while (stmt) {
    sqlite_stmt_handle_t *next = stmt->next_in_db;
    if (!stmt->finalized && stmt->stmt) {
      sqlite3_finalize(stmt->stmt);
      stmt->stmt = NULL;
    }
    stmt->finalized = true;
    stmt->locked = false;
    stmt->next_in_db = NULL;
    stmt = next;
  }
  db->stmt_head = NULL;

  sqlite3_close_v2(db->db);
  db->db = NULL;
  db->open = false;
}

static sqlite_db_handle_t *get_db_handle(ant_value_t obj) {
  return (sqlite_db_handle_t *)js_get_native(obj, SQLITE_DB_NATIVE_TAG);
}

static sqlite_stmt_handle_t *get_stmt_handle(ant_value_t obj) {
  return (sqlite_stmt_handle_t *)js_get_native(obj, SQLITE_STMT_NATIVE_TAG);
}

static void sqlite_db_finalizer(ant_t *js, ant_object_t *obj) {
  (void)js;
  ant_value_t value = js_obj_from_ptr(obj);
  sqlite_db_handle_t *db = get_db_handle(value);
  if (!db) return;

  db_handle_close(db);

  sqlite_db_handle_t **cur = &db_handles;
  while (*cur) {
    if (*cur == db) {
      *cur = db->next_global;
      break;
    }
    cur = &(*cur)->next_global;
  }

  free(db->location);
  free(db);
  js_clear_native(value, SQLITE_DB_NATIVE_TAG);
}

static void sqlite_stmt_finalizer(ant_t *js, ant_object_t *obj) {
  (void)js;
  ant_value_t value = js_obj_from_ptr(obj);
  sqlite_stmt_handle_t *stmt = get_stmt_handle(value);
  if (!stmt) return;

  stmt_handle_finalize(stmt);

  sqlite_stmt_handle_t **cur = &stmt_handles;
  while (*cur) {
    if (*cur == stmt) {
      *cur = stmt->next_global;
      break;
    }
    cur = &(*cur)->next_global;
  }

  free(stmt);
  js_clear_native(value, SQLITE_STMT_NATIVE_TAG);
}

static void sqlite_iter_finalizer(ant_t *js, ant_object_t *obj) {
  (void)js;
  ant_value_t value = js_obj_from_ptr(obj);
  sqlite_iter_state_t *iter =
    (sqlite_iter_state_t *)js_get_native(value, SQLITE_ITER_NATIVE_TAG);
  if (!iter) return;

  if (!iter->done && iter->stmt && !iter->stmt->finalized) {
    iter->stmt->locked = false;
    if (iter->stmt->stmt) sqlite3_reset(iter->stmt->stmt);
  }
  free(iter);
  js_clear_native(value, SQLITE_ITER_NATIVE_TAG);
}

static bool option_bool(ant_t *js, ant_value_t options, const char *key, bool fallback) {
  if (vtype(options) != T_OBJ) return fallback;
  ant_value_t val = js_get(js, options, key);
  if (vtype(val) == T_UNDEF) return fallback;
  return js_truthy(js, val);
}

// int64 -> JS value under node:sqlite rules: number when safe, bigint when
// requested, RangeError otherwise.
static ant_value_t int64_to_js(ant_t *js, int64_t value, bool read_bigints) {
  if (read_bigints) return bigint_from_int64(js, value);
  if (value >= -JS_MAX_SAFE_INTEGER && value <= JS_MAX_SAFE_INTEGER) {
    return js_mknum((double)value);
  }
  ant_value_t props = js_mkobj(js);
  js_set(js, props, "code", ANT_STRING("ERR_OUT_OF_RANGE"));
  return js_mkerr_props(js, JS_ERR_RANGE, props,
    "Value is too large to be represented as a JavaScript number: %lld; "
    "use StatementSync.prototype.setReadBigInts(true)", (long long)value);
}

static ant_value_t column_to_js(ant_t *js, sqlite3_stmt *stmt, int col, bool read_bigints) {
  switch (sqlite3_column_type(stmt, col)) {
    case SQLITE_INTEGER:
      return int64_to_js(js, sqlite3_column_int64(stmt, col), read_bigints);
    case SQLITE_FLOAT:
      return js_mknum(sqlite3_column_double(stmt, col));
    case SQLITE_TEXT: {
      const unsigned char *text = sqlite3_column_text(stmt, col);
      int len = sqlite3_column_bytes(stmt, col);
      return js_mkstr(js, text ? (const char *)text : "", len > 0 ? (size_t)len : 0);
    }
    case SQLITE_BLOB: {
      int len = sqlite3_column_bytes(stmt, col);
      ArrayBufferData *ab = create_array_buffer_data(len > 0 ? (size_t)len : 0);
      if (!ab) return js_mkerr(js, "Failed to allocate SQLite blob buffer");
      const void *blob = sqlite3_column_blob(stmt, col);
      if (len > 0 && blob) memcpy(ab->data, blob, (size_t)len);
      ant_value_t out = create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, ab->length, "Uint8Array");
      if (vtype(out) == T_ERR) free_array_buffer_data(ab);
      return out;
    }
    case SQLITE_NULL:
    default:
      return js_mknull();
  }
}

static ant_value_t row_to_js(ant_t *js, sqlite3_stmt *stmt, bool read_bigints) {
  ant_value_t row = js_mkobj(js);
  int count = sqlite3_column_count(stmt);

  for (int i = 0; i < count; i++) {
    ant_value_t value = column_to_js(js, stmt, i, read_bigints);
    if (vtype(value) == T_ERR) return value;
    const char *name = sqlite3_column_name(stmt, i);
    js_set(js, row, name ? name : "", value);
  }
  return row;
}

static ant_value_t bind_value(ant_t *js, sqlite_stmt_handle_t *stmt, int index, ant_value_t value) {
  sqlite3_stmt *s = stmt->stmt;
  int rc;

  switch (vtype(value)) {
    case T_NULL:
      rc = sqlite3_bind_null(s, index);
      break;
    case T_BOOL:
      rc = sqlite3_bind_int(s, index, value == js_true ? 1 : 0);
      break;
    case T_NUM: {
      double num = js_getnum(value);
      if (num == (double)(int64_t)num &&
          num >= (double)-JS_MAX_SAFE_INTEGER && num <= (double)JS_MAX_SAFE_INTEGER) {
        rc = sqlite3_bind_int64(s, index, (int64_t)num);
      } else {
        rc = sqlite3_bind_double(s, index, num);
      }
      break;
    }
    case T_BIGINT: {
      int64_t v = 0;
      bool in_range = bigint_to_int64_wrapping(js, value, &v);
      if (in_range) {
        ant_value_t round_trip = bigint_from_int64(js, v);
        in_range = bigint_compare(js, value, round_trip) == 0;
      }
      if (!in_range) {
        ant_value_t props = js_mkobj(js);
        js_set(js, props, "code", ANT_STRING("ERR_INVALID_ARG_VALUE"));
        return js_mkerr_props(js, JS_ERR_RANGE, props,
          "BigInt value is too large to bind as a 64-bit integer");
      }
      rc = sqlite3_bind_int64(s, index, v);
      break;
    }
    case T_STR: {
      size_t len = 0;
      const char *str = js_getstr(js, value, &len);
      rc = sqlite3_bind_text64(s, index, str ? str : "", (sqlite3_uint64)len,
                               SQLITE_TRANSIENT, SQLITE_UTF8);
      break;
    }
    default: {
      TypedArrayData *ta = buffer_get_typedarray_data(value);
      if (ta) {
        if (!ta->buffer || !ta->buffer->data) {
          return js_mkerr(js, "Cannot bind a detached TypedArray");
        }
        rc = sqlite3_bind_blob64(s, index, ta->buffer->data + ta->byte_offset,
                                 (sqlite3_uint64)ta->byte_length, SQLITE_TRANSIENT);
        break;
      }
      ArrayBufferData *ab = buffer_get_arraybuffer_data(value);
      if (ab) {
        if (!ab->data && ab->length > 0) {
          return js_mkerr(js, "Cannot bind a detached ArrayBuffer");
        }
        rc = sqlite3_bind_blob64(s, index, ab->data,
                                 (sqlite3_uint64)ab->length, SQLITE_TRANSIENT);
        break;
      }
      return js_mkerr(js,
        "Provided value cannot be bound to SQLite parameter %d", index);
    }
  }

  if (rc != SQLITE_OK) {
    return sqlite_error(js, stmt->db ? stmt->db->db : NULL, "Failed to bind parameter");
  }
  return js_mkundef();
}

// Resolve a named-bag key to a 1-based parameter index. Keys may carry the
// SQL prefix (:name, @name, $name) or be bare when bare binding is allowed.
static int resolve_named_parameter(sqlite_stmt_handle_t *stmt, const char *key, size_t key_len) {
  sqlite3_stmt *s = stmt->stmt;
  char buf[256];

  if (key_len == 0 || key_len >= sizeof(buf) - 1) return 0;

  if (key[0] == ':' || key[0] == '@' || key[0] == '$') {
    memcpy(buf, key, key_len);
    buf[key_len] = '\0';
    return sqlite3_bind_parameter_index(s, buf);
  }

  if (!stmt->allow_bare_named) return 0;

  static const char prefixes[] = {':', '@', '$'};
  for (size_t p = 0; p < sizeof(prefixes); p++) {
    buf[0] = prefixes[p];
    memcpy(buf + 1, key, key_len);
    buf[key_len + 1] = '\0';
    int index = sqlite3_bind_parameter_index(s, buf);
    if (index > 0) return index;
  }
  return 0;
}

static ant_value_t bind_named_bag(ant_t *js, sqlite_stmt_handle_t *stmt, ant_value_t bag) {
  ant_iter_t iter = js_prop_iter_begin(js, bag);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = js_mkundef();

  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    int index = resolve_named_parameter(stmt, key, key_len);
    if (index == 0) {
      if (stmt->allow_unknown_named) continue;
      js_prop_iter_end(&iter);
      return js_mkerr(js, "Unknown named parameter '%.*s'", (int)key_len, key);
    }
    ant_value_t err = bind_value(js, stmt, index, value);
    if (vtype(err) == T_ERR) {
      js_prop_iter_end(&iter);
      return err;
    }
  }
  js_prop_iter_end(&iter);
  return js_mkundef();
}

static bool is_named_bag(ant_value_t value) {
  if (vtype(value) != T_OBJ) return false;
  if (buffer_get_typedarray_data(value)) return false;
  if (buffer_get_arraybuffer_data(value)) return false;
  return true;
}

static ant_value_t bind_all_params(ant_t *js, sqlite_stmt_handle_t *stmt, ant_value_t *args, int nargs) {
  sqlite3_stmt *s = stmt->stmt;
  int arg_index = 0;

  sqlite3_reset(s);
  sqlite3_clear_bindings(s);

  if (nargs > 0 && is_named_bag(args[0])) {
    ant_value_t err = bind_named_bag(js, stmt, args[0]);
    if (vtype(err) == T_ERR) return err;
    arg_index = 1;
  }

  // Remaining args fill anonymous (unnamed) parameters in order.
  int param_count = sqlite3_bind_parameter_count(s);
  for (int i = 1; i <= param_count && arg_index < nargs; i++) {
    if (sqlite3_bind_parameter_name(s, i) != NULL) continue;
    ant_value_t err = bind_value(js, stmt, i, args[arg_index++]);
    if (vtype(err) == T_ERR) return err;
  }

  if (arg_index < nargs) {
    ant_value_t props = js_mkobj(js);
    js_set(js, props, "code", ANT_STRING("ERR_INVALID_ARG_VALUE"));
    return js_mkerr_props(js, JS_ERR_RANGE, props,
      "Too many parameter values were provided");
  }
  return js_mkundef();
}

static sqlite_stmt_handle_t *stmt_for_call(ant_t *js, ant_value_t self, ant_value_t *err_out) {
  sqlite_stmt_handle_t *stmt = get_stmt_handle(self);
  if (!stmt || stmt->finalized || !stmt->stmt || !stmt->db || !stmt->db->open) {
    *err_out = js_mkerr(js, "statement has been finalized or its database is closed");
    return NULL;
  }
  if (stmt->locked) {
    *err_out = js_mkerr(js, "statement is busy: an iterator is still running");
    return NULL;
  }
  *err_out = js_mkundef();
  return stmt;
}

// StatementSync.prototype.run(...params)
static ant_value_t sqlite_stmt_run(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err;
  sqlite_stmt_handle_t *stmt = stmt_for_call(js, js_getthis(js), &err);
  if (!stmt) return err;

  err = bind_all_params(js, stmt, args, nargs);
  if (vtype(err) == T_ERR) return err;

  int rc;
  do {
    rc = sqlite3_step(stmt->stmt);
  } while (rc == SQLITE_ROW);

  if (rc != SQLITE_DONE) {
    ant_value_t result = sqlite_error(js, stmt->db->db, "Failed to run statement");
    sqlite3_reset(stmt->stmt);
    return result;
  }
  sqlite3_reset(stmt->stmt);

  ant_value_t out = js_mkobj(js);
  ant_value_t changes = int64_to_js(js, sqlite3_changes64(stmt->db->db), stmt->read_bigints);
  if (vtype(changes) == T_ERR) return changes;
  ant_value_t rowid = int64_to_js(js, sqlite3_last_insert_rowid(stmt->db->db), stmt->read_bigints);
  if (vtype(rowid) == T_ERR) return rowid;
  js_set(js, out, "changes", changes);
  js_set(js, out, "lastInsertRowid", rowid);
  return out;
}

// StatementSync.prototype.get(...params)
static ant_value_t sqlite_stmt_get(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err;
  sqlite_stmt_handle_t *stmt = stmt_for_call(js, js_getthis(js), &err);
  if (!stmt) return err;

  err = bind_all_params(js, stmt, args, nargs);
  if (vtype(err) == T_ERR) return err;

  int rc = sqlite3_step(stmt->stmt);
  if (rc == SQLITE_ROW) {
    ant_value_t row = row_to_js(js, stmt->stmt, stmt->read_bigints);
    sqlite3_reset(stmt->stmt);
    return row;
  }
  if (rc == SQLITE_DONE) {
    sqlite3_reset(stmt->stmt);
    return js_mkundef();
  }

  ant_value_t result = sqlite_error(js, stmt->db->db, "Failed to execute statement");
  sqlite3_reset(stmt->stmt);
  return result;
}

// StatementSync.prototype.all(...params)
static ant_value_t sqlite_stmt_all(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err;
  sqlite_stmt_handle_t *stmt = stmt_for_call(js, js_getthis(js), &err);
  if (!stmt) return err;

  err = bind_all_params(js, stmt, args, nargs);
  if (vtype(err) == T_ERR) return err;

  ant_value_t rows = js_mkarr(js);
  int rc;

  while ((rc = sqlite3_step(stmt->stmt)) == SQLITE_ROW) {
    ant_value_t row = row_to_js(js, stmt->stmt, stmt->read_bigints);
    if (vtype(row) == T_ERR) {
      sqlite3_reset(stmt->stmt);
      return row;
    }
    js_arr_push(js, rows, row);
  }

  if (rc != SQLITE_DONE) {
    ant_value_t result = sqlite_error(js, stmt->db->db, "Failed to execute statement");
    sqlite3_reset(stmt->stmt);
    return result;
  }
  sqlite3_reset(stmt->stmt);
  return rows;
}

// Iterator over statement rows: { next(), return(), [Symbol.iterator]() }
static ant_value_t sqlite_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t self = js_getthis(js);
  sqlite_iter_state_t *iter =
    (sqlite_iter_state_t *)js_get_native(self, SQLITE_ITER_NATIVE_TAG);
  if (!iter) return js_mkerr(js, "Invalid SQLite iterator");
  if (iter->done) return js_iter_result(js, false, js_mkundef());

  sqlite_stmt_handle_t *stmt = iter->stmt;
  if (!stmt || stmt->finalized || !stmt->stmt || !stmt->db || !stmt->db->open) {
    iter->done = true;
    return js_mkerr(js, "statement has been finalized or its database is closed");
  }

  int rc = sqlite3_step(stmt->stmt);
  if (rc == SQLITE_ROW) {
    ant_value_t row = row_to_js(js, stmt->stmt, stmt->read_bigints);
    if (vtype(row) == T_ERR) {
      iter->done = true;
      stmt->locked = false;
      sqlite3_reset(stmt->stmt);
      return row;
    }
    return js_iter_result(js, true, row);
  }

  iter->done = true;
  stmt->locked = false;
  if (rc != SQLITE_DONE) {
    ant_value_t result = sqlite_error(js, stmt->db->db, "Failed to execute statement");
    sqlite3_reset(stmt->stmt);
    return result;
  }
  sqlite3_reset(stmt->stmt);
  return js_iter_result(js, false, js_mkundef());
}

static ant_value_t sqlite_iter_return(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t self = js_getthis(js);
  sqlite_iter_state_t *iter =
    (sqlite_iter_state_t *)js_get_native(self, SQLITE_ITER_NATIVE_TAG);
  if (!iter) return js_mkerr(js, "Invalid SQLite iterator");

  if (!iter->done) {
    iter->done = true;
    if (iter->stmt && !iter->stmt->finalized) {
      iter->stmt->locked = false;
      if (iter->stmt->stmt) sqlite3_reset(iter->stmt->stmt);
    }
  }
  return js_iter_result(js, false, js_mkundef());
}

// StatementSync.prototype.iterate(...params)
static ant_value_t sqlite_stmt_iterate(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js_getthis(js);
  ant_value_t err;
  sqlite_stmt_handle_t *stmt = stmt_for_call(js, self, &err);
  if (!stmt) return err;

  err = bind_all_params(js, stmt, args, nargs);
  if (vtype(err) == T_ERR) return err;

  sqlite_iter_state_t *iter = ant_calloc(sizeof(sqlite_iter_state_t));
  if (!iter) return js_mkerr(js, "out of memory");
  iter->stmt = stmt;
  iter->done = false;
  stmt->locked = true;

  ant_value_t obj = js_mkobj(js);
  js_set_native(obj, iter, SQLITE_ITER_NATIVE_TAG);
  js_set_finalizer(obj, sqlite_iter_finalizer);
  js_set_slot_wb(js, obj, SLOT_DATA, self); // keep the statement alive
  if (is_special_object(sqlite_types.iter_proto)) {
    js_set_proto_init(obj, sqlite_types.iter_proto);
  }
  return obj;
}

// StatementSync.prototype.columns()
static ant_value_t sqlite_stmt_columns(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err;
  sqlite_stmt_handle_t *stmt = stmt_for_call(js, js_getthis(js), &err);
  if (!stmt) return err;

  ant_value_t out = js_mkarr(js);
  int count = sqlite3_column_count(stmt->stmt);

  for (int i = 0; i < count; i++) {
    ant_value_t col = js_mkobj(js);
    const char *origin = sqlite3_column_origin_name(stmt->stmt, i);
    const char *database = sqlite3_column_database_name(stmt->stmt, i);
    const char *name = sqlite3_column_name(stmt->stmt, i);
    const char *table = sqlite3_column_table_name(stmt->stmt, i);
    const char *type = sqlite3_column_decltype(stmt->stmt, i);

    js_set(js, col, "column", origin ? js_mkstr(js, origin, strlen(origin)) : js_mknull());
    js_set(js, col, "database", database ? js_mkstr(js, database, strlen(database)) : js_mknull());
    js_set(js, col, "name", name ? js_mkstr(js, name, strlen(name)) : js_mknull());
    js_set(js, col, "table", table ? js_mkstr(js, table, strlen(table)) : js_mknull());
    js_set(js, col, "type", type ? js_mkstr(js, type, strlen(type)) : js_mknull());
    js_arr_push(js, out, col);
  }
  return out;
}

static ant_value_t sqlite_stmt_source_sql(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err;
  sqlite_stmt_handle_t *stmt = stmt_for_call(js, js_getthis(js), &err);
  if (!stmt) return err;

  const char *sql = sqlite3_sql(stmt->stmt);
  return js_mkstr(js, sql ? sql : "", sql ? strlen(sql) : 0);
}

static ant_value_t sqlite_stmt_expanded_sql(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err;
  sqlite_stmt_handle_t *stmt = stmt_for_call(js, js_getthis(js), &err);
  if (!stmt) return err;

  char *sql = sqlite3_expanded_sql(stmt->stmt);
  if (!sql) return js_mkundef();
  ant_value_t out = js_mkstr(js, sql, strlen(sql));
  sqlite3_free(sql);
  return out;
}

#define STMT_FLAG_SETTER(fn_name, field)                                       \
  static ant_value_t fn_name(ant_t *js, ant_value_t *args, int nargs) {        \
    sqlite_stmt_handle_t *stmt = get_stmt_handle(js_getthis(js));              \
    if (!stmt || stmt->finalized) {                                            \
      return js_mkerr(js, "statement has been finalized");                     \
    }                                                                          \
    if (nargs < 1 || vtype(args[0]) != T_BOOL) {                               \
      return js_mkerr(js, "The \"enabled\" argument must be a boolean");       \
    }                                                                          \
    stmt->field = (args[0] == js_true);                                        \
    return js_mkundef();                                                       \
  }

STMT_FLAG_SETTER(sqlite_stmt_set_read_bigints, read_bigints)
STMT_FLAG_SETTER(sqlite_stmt_set_allow_bare, allow_bare_named)
STMT_FLAG_SETTER(sqlite_stmt_set_allow_unknown, allow_unknown_named)

#undef STMT_FLAG_SETTER

static sqlite_db_handle_t *db_for_call(ant_t *js, ant_value_t self, bool open_required, ant_value_t *err_out) {
  sqlite_db_handle_t *db = get_db_handle(self);
  if (!db) {
    *err_out = js_mkerr(js, "Invalid DatabaseSync handle");
    return NULL;
  }
  if (open_required && !db->open) {
    *err_out = js_mkerr(js, "database is not open");
    return NULL;
  }
  *err_out = js_mkundef();
  return db;
}

static ant_value_t db_do_open(ant_t *js, sqlite_db_handle_t *db, ant_value_t options) {
  bool read_only = option_bool(js, options, "readOnly", false);
  bool foreign_keys = option_bool(js, options, "enableForeignKeyConstraints", true);
  bool dqs = option_bool(js, options, "enableDoubleQuotedStringLiterals", false);

  int flags = read_only ? SQLITE_OPEN_READONLY
                        : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

  ensure_sqlite_initialized();
  int rc = sqlite3_open_v2(db->location, &db->db, flags, NULL);
  if (rc != SQLITE_OK) {
    ant_value_t result = sqlite_error(js, db->db, "Failed to open database");
    if (db->db) {
      sqlite3_close_v2(db->db);
      db->db = NULL;
    }
    return result;
  }

  sqlite3_db_config(db->db, SQLITE_DBCONFIG_ENABLE_FKEY, foreign_keys ? 1 : 0, NULL);
  sqlite3_db_config(db->db, SQLITE_DBCONFIG_DQS_DML, dqs ? 1 : 0, NULL);
  sqlite3_db_config(db->db, SQLITE_DBCONFIG_DQS_DDL, dqs ? 1 : 0, NULL);

  ant_value_t timeout = vtype(options) == T_OBJ ? js_get(js, options, "timeout") : js_mkundef();
  if (vtype(timeout) == T_NUM) {
    sqlite3_busy_timeout(db->db, (int)js_getnum(timeout));
  }

  db->open = true;
  return js_mkundef();
}

// new DatabaseSync(location[, options])
static ant_value_t sqlite_db_constructor(ant_t *js, ant_value_t *args, int nargs) {
  if (is_undefined(js->new_target)) {
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Failed to construct 'DatabaseSync': Please use the 'new' operator.");
  }
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "The \"location\" argument must be a string");
  }

  size_t path_len = 0;
  const char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "The \"location\" argument cannot be empty");
  }

  ant_value_t options = nargs > 1 ? args[1] : js_mkundef();

  sqlite_db_handle_t *db = ant_calloc(sizeof(sqlite_db_handle_t));
  if (!db) return js_mkerr(js, "out of memory");

  db->location = malloc(path_len + 1);
  if (!db->location) {
    free(db);
    return js_mkerr(js, "out of memory");
  }
  memcpy(db->location, path, path_len);
  db->location[path_len] = '\0';

  if (option_bool(js, options, "open", true)) {
    ant_value_t err = db_do_open(js, db, options);
    if (vtype(err) == T_ERR) {
      free(db->location);
      free(db);
      return err;
    }
  }

  db->next_global = db_handles;
  db_handles = db;

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, sqlite_types.db_proto);
  js_set_proto_init(obj, is_object_type(proto) ? proto : sqlite_types.db_proto);
  js_set_native(obj, db, SQLITE_DB_NATIVE_TAG);
  js_set_finalizer(obj, sqlite_db_finalizer);
  js_set_slot_wb(js, obj, SLOT_DATA, options); // keep open() options reachable
  return obj;
}

// DatabaseSync.prototype.open()
static ant_value_t sqlite_db_open_method(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t self = js_getthis(js);
  ant_value_t err;
  sqlite_db_handle_t *db = db_for_call(js, self, false, &err);
  if (!db) return err;
  if (db->open) return js_mkerr(js, "database is already open");

  ant_value_t options = js_get_slot(self, SLOT_DATA);
  return db_do_open(js, db, options);
}

// DatabaseSync.prototype.close()
static ant_value_t sqlite_db_close_method(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t err;
  sqlite_db_handle_t *db = db_for_call(js, js_getthis(js), true, &err);
  if (!db) return err;
  db_handle_close(db);
  return js_mkundef();
}

// DatabaseSync.prototype[Symbol.dispose]() — close without throwing if closed.
static ant_value_t sqlite_db_dispose(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  sqlite_db_handle_t *db = get_db_handle(js_getthis(js));
  if (db && db->open) db_handle_close(db);
  return js_mkundef();
}

// DatabaseSync.prototype.exec(sql)
static ant_value_t sqlite_db_exec(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err;
  sqlite_db_handle_t *db = db_for_call(js, js_getthis(js), true, &err);
  if (!db) return err;
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "The \"sql\" argument must be a string");
  }

  size_t len = 0;
  const char *sql = js_getstr(js, args[0], &len);
  int rc = sqlite3_exec(db->db, sql ? sql : "", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    return sqlite_error(js, db->db, "Failed to execute SQL");
  }
  return js_mkundef();
}

static ant_value_t make_stmt_obj(ant_t *js, sqlite_stmt_handle_t *stmt, ant_value_t db_obj) {
  ant_value_t obj = js_mkobj(js);
  js_set_native(obj, stmt, SQLITE_STMT_NATIVE_TAG);
  js_set_finalizer(obj, sqlite_stmt_finalizer);
  js_set_slot_wb(js, obj, SLOT_DATA, db_obj); // keep the database alive
  if (is_special_object(sqlite_types.stmt_proto)) {
    js_set_proto_init(obj, sqlite_types.stmt_proto);
  }
  return obj;
}

// DatabaseSync.prototype.prepare(sql)
static ant_value_t sqlite_db_prepare(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js_getthis(js);
  ant_value_t err;
  sqlite_db_handle_t *db = db_for_call(js, self, true, &err);
  if (!db) return err;
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "The \"sql\" argument must be a string");
  }

  size_t len = 0;
  const char *sql = js_getstr(js, args[0], &len);

  sqlite3_stmt *prepared = NULL;
  int rc = sqlite3_prepare_v2(db->db, sql ? sql : "", (int)len, &prepared, NULL);
  if (rc != SQLITE_OK) {
    return sqlite_error(js, db->db, "Failed to prepare statement");
  }
  if (!prepared) {
    return js_mkerr(js, "The supplied SQL contains no statement");
  }

  sqlite_stmt_handle_t *stmt = ant_calloc(sizeof(sqlite_stmt_handle_t));
  if (!stmt) {
    sqlite3_finalize(prepared);
    return js_mkerr(js, "out of memory");
  }

  stmt->stmt = prepared;
  stmt->db = db;
  stmt->allow_bare_named = true;
  stmt->next_in_db = db->stmt_head;
  db->stmt_head = stmt;
  stmt->next_global = stmt_handles;
  stmt_handles = stmt;

  return make_stmt_obj(js, stmt, self);
}

// DatabaseSync.prototype.location([dbName])
static ant_value_t sqlite_db_location(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t err;
  sqlite_db_handle_t *db = db_for_call(js, js_getthis(js), true, &err);
  if (!db) return err;

  const char *name = "main";
  size_t name_len = 0;
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    name = js_getstr(js, args[0], &name_len);
  }

  const char *filename = sqlite3_db_filename(db->db, name);
  if (!filename || !filename[0]) return js_mknull();
  return js_mkstr(js, filename, strlen(filename));
}

static ant_value_t sqlite_db_is_open(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  sqlite_db_handle_t *db = get_db_handle(js_getthis(js));
  return js_bool(db && db->open);
}

static ant_value_t sqlite_db_is_transaction(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  sqlite_db_handle_t *db = get_db_handle(js_getthis(js));
  return js_bool(db && db->open && !sqlite3_get_autocommit(db->db));
}

static ant_value_t sqlite_stmt_constructor(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mkerr(js,
    "StatementSync cannot be constructed directly; use DatabaseSync.prototype.prepare()");
}

static void ensure_sqlite_prototypes(ant_t *js) {
  if (sqlite_types.ready) return;
  ant_value_t object_proto = js->sym.object_proto;

  ant_value_t stmt_ctor_obj = js_mkobj(js);
  ant_value_t stmt_proto = js_mkobj(js);

  js_set_proto_init(stmt_proto, object_proto);
  js_set(js, stmt_proto, "run", js_mkfun(sqlite_stmt_run));
  js_set(js, stmt_proto, "get", js_mkfun(sqlite_stmt_get));
  js_set(js, stmt_proto, "all", js_mkfun(sqlite_stmt_all));
  js_set(js, stmt_proto, "iterate", js_mkfun(sqlite_stmt_iterate));
  js_set(js, stmt_proto, "columns", js_mkfun(sqlite_stmt_columns));
  js_set(js, stmt_proto, "setReadBigInts", js_mkfun(sqlite_stmt_set_read_bigints));
  js_set(js, stmt_proto, "setAllowBareNamedParameters", js_mkfun(sqlite_stmt_set_allow_bare));
  js_set(js, stmt_proto, "setAllowUnknownNamedParameters", js_mkfun(sqlite_stmt_set_allow_unknown));
  js_set_getter_desc(js, stmt_proto, "sourceSQL", 9, js_mkfun(sqlite_stmt_source_sql), 0);
  js_set_getter_desc(js, stmt_proto, "expandedSQL", 11, js_mkfun(sqlite_stmt_expanded_sql), 0);
  js_set_sym(js, stmt_proto, get_toStringTag_sym(), ANT_STRING("StatementSync"));
  js_set_slot(stmt_ctor_obj, SLOT_CFUNC, js_mkfun(sqlite_stmt_constructor));
  js_mkprop_fast(js, stmt_ctor_obj, "prototype", 9, stmt_proto);
  js_mkprop_fast(js, stmt_ctor_obj, "name", 4, ANT_STRING("StatementSync"));
  js_set_descriptor(js, stmt_ctor_obj, "name", 4, 0);

  ant_value_t db_ctor_obj = js_mkobj(js);
  ant_value_t db_proto = js_mkobj(js);

  js_set_proto_init(db_proto, object_proto);
  js_set(js, db_proto, "open", js_mkfun(sqlite_db_open_method));
  js_set(js, db_proto, "close", js_mkfun(sqlite_db_close_method));
  js_set(js, db_proto, "exec", js_mkfun(sqlite_db_exec));
  js_set(js, db_proto, "prepare", js_mkfun(sqlite_db_prepare));
  js_set(js, db_proto, "location", js_mkfun(sqlite_db_location));
  js_set_getter_desc(js, db_proto, "isOpen", 6, js_mkfun(sqlite_db_is_open), 0);
  js_set_getter_desc(js, db_proto, "isTransaction", 13, js_mkfun(sqlite_db_is_transaction), 0);
  js_set_sym(js, db_proto, get_dispose_sym(), js_mkfun(sqlite_db_dispose));
  js_set_sym(js, db_proto, get_toStringTag_sym(), ANT_STRING("DatabaseSync"));
  js_set_slot(db_ctor_obj, SLOT_CFUNC, js_mkfun(sqlite_db_constructor));
  js_mkprop_fast(js, db_ctor_obj, "prototype", 9, db_proto);
  js_mkprop_fast(js, db_ctor_obj, "name", 4, ANT_STRING("DatabaseSync"));
  js_set_descriptor(js, db_ctor_obj, "name", 4, 0);

  ant_value_t iter_proto = js_mkobj(js);
  js_set_proto_init(iter_proto, object_proto);
  js_set(js, iter_proto, "next", js_mkfun(sqlite_iter_next));
  js_set(js, iter_proto, "return", js_mkfun(sqlite_iter_return));
  js_set_sym(js, iter_proto, get_iterator_sym(), js_mkfun(sym_this_cb));
  js_set_sym(js, iter_proto, get_toStringTag_sym(), ANT_STRING("StatementSyncIterator"));

  ant_value_t db_ctor = js_obj_to_func(js, db_ctor_obj);
  js_mark_constructor(db_ctor, true);
  ant_value_t stmt_ctor = js_obj_to_func(js, stmt_ctor_obj);
  js_mark_constructor(stmt_ctor, true);

  sqlite_types.db_ctor = db_ctor;
  sqlite_types.stmt_ctor = stmt_ctor;
  sqlite_types.db_proto = db_proto;
  sqlite_types.stmt_proto = stmt_proto;
  sqlite_types.iter_proto = iter_proto;
  sqlite_types.ready = true;
}

ant_value_t sqlite_library(ant_t *js) {
  ensure_sqlite_prototypes(js);
  ant_value_t lib = js_mkobj(js);
  js_set(js, lib, "DatabaseSync", sqlite_types.db_ctor);
  js_set(js, lib, "StatementSync", sqlite_types.stmt_ctor);

  ant_value_t constants = js_mkobj(js);
  js_set(js, lib, "constants", constants);

  js_set_sym(js, lib, get_toStringTag_sym(), ANT_STRING("sqlite"));
  return lib;
}

void gc_mark_sqlite(ant_t *js, gc_mark_fn mark) {
  if (sqlite_types.ready) {
    mark(js, sqlite_types.db_ctor);
    mark(js, sqlite_types.stmt_ctor);
    mark(js, sqlite_types.db_proto);
    mark(js, sqlite_types.stmt_proto);
    mark(js, sqlite_types.iter_proto);
  }
}

// Close outstanding SQLite resources at runtime teardown. Handle structs are
// left to the object finalizers (matching cleanup_lmdb_module) so a finalizer
// running after cleanup never double-frees.
void cleanup_sqlite_module(void) {
  sqlite_stmt_handle_t *stmt = stmt_handles;
  while (stmt) {
    if (!stmt->finalized && stmt->stmt) {
      sqlite3_finalize(stmt->stmt);
      stmt->stmt = NULL;
    }
    stmt->finalized = true;
    stmt->locked = false;
    stmt = stmt->next_global;
  }

  sqlite_db_handle_t *db = db_handles;
  while (db) {
    if (db->open && db->db) {
      sqlite3_close_v2(db->db);
      db->db = NULL;
      db->open = false;
    }
    db->stmt_head = NULL;
    db = db->next_global;
  }

  if (sqlite_initialized) {
    sqlite3_shutdown();
    sqlite_initialized = false;
  }
}
