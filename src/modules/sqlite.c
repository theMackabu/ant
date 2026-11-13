#ifdef ANT_HAVE_SQLITE

#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sqlite3.h>

#include "ant.h"
#include "cfunc.h"
#include "ptr.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "gc/roots.h"
#include "silver/call.h"

#include "modules/bigint.h"
#include "modules/buffer.h"
#include "modules/sqlite.h"

#ifdef ANT_SQLITE_SYSTEM

#include <uv.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static struct {
  #define M(field, symbol) typeof(symbol) *field;
  #define O(field, symbol) typeof(symbol) *field;
  #include "modules/sqlite_api.h"
} sqlite_api;

static uv_once_t sqlite_api_once = UV_ONCE_INIT;
static uv_lib_t sqlite_api_lib;
static bool sqlite_api_ready;

static bool sqlite_api_open(void) {
#if defined(_WIN32)
  char path[MAX_PATH + 16];
  UINT len = GetSystemDirectoryA(path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return false;
  memcpy(path + len, "\\winsqlite3.dll", sizeof("\\winsqlite3.dll"));
  return uv_dlopen(path, &sqlite_api_lib) == 0;
#elif defined(__APPLE__)
  return uv_dlopen("/usr/lib/libsqlite3.dylib", &sqlite_api_lib) == 0;
#else
  return uv_dlopen("libsqlite3.so.0", &sqlite_api_lib) == 0;
#endif
}

static void sqlite_api_load(void) {
  if (!sqlite_api_open()) return;

  #define M(field, symbol) \
    if (uv_dlsym(&sqlite_api_lib, #symbol, (void **)&sqlite_api.field) != 0) return;
  #define O(field, symbol) \
    if (uv_dlsym(&sqlite_api_lib, #symbol, (void **)&sqlite_api.field) != 0) sqlite_api.field = NULL;
  #include "modules/sqlite_api.h"

  sqlite_api_ready = true;
}

static bool sqlite_api_available(void) {
  uv_once(&sqlite_api_once, sqlite_api_load);
  return sqlite_api_ready;
}

#else

static const struct {
  #define M(field, symbol) typeof(symbol) *field;
  #define O(field, symbol) typeof(symbol) *field;
  #include "modules/sqlite_api.h"
} sqlite_api = {
  #define M(field, symbol) .field = symbol,
  #define O(field, symbol) .field = symbol,
  #include "modules/sqlite_api.h"
};

static bool sqlite_api_available(void) {
  return true;
}

#endif

#define SQ(name) sqlite_api.name
#define SQ_HAS(name) (sqlite_api.name != NULL)

enum {
  SQLITE_OPT_READ_ONLY = 1 << 0,
  SQLITE_OPT_FOREIGN_KEYS = 1 << 1,
  SQLITE_OPT_DQS = 1 << 2,
  SQLITE_OPT_ALLOW_EXTENSION = 1 << 3,
  SQLITE_OPT_READ_BIGINTS = 1 << 4,
  SQLITE_OPT_RETURN_ARRAYS = 1 << 5,
  SQLITE_OPT_BARE_NAMED = 1 << 6,
  SQLITE_OPT_UNKNOWN_NAMED = 1 << 7,
  SQLITE_OPT_DEFENSIVE = 1 << 8,
  SQLITE_OPT_EXTENSION_ENABLED = 1 << 9,
};

enum {
  SQLITE_REF_PENDING = 0,
  SQLITE_REF_AUTHORIZER = 1,
  SQLITE_REF_FIRST = 2,
};

enum {
  SQLITE_LIMIT_COUNT = 11,
  SQLITE_MAX_HIDDEN = 31,
};

typedef struct sqlite_db sqlite_db_t;
typedef struct sqlite_stmt sqlite_stmt_t;
typedef struct sqlite_session sqlite_session_t;

typedef struct {
  sqlite_db_t *open;
} sqlite_module_state_t;

struct sqlite_db {
  ant_t *js;
  sqlite3 *db;
  char *path;
  ant_value_t self;
  uint32_t flags;
  int timeout;
  int limits[SQLITE_LIMIT_COUNT];
  uint32_t callback_depth;
  uint32_t authorizer_depth;
  uint32_t refs_len;
  uint32_t *free_refs;
  uint32_t free_count;
  uint32_t free_cap;
  bool has_pending;
  bool finalizing;
  sqlite_stmt_t *stmts;
  sqlite_session_t *sessions;
  sqlite_db_t *next;
  sqlite_db_t **prev;
};

struct sqlite_stmt {
  sqlite3_stmt *stmt;
  sqlite_db_t *db;
  uint32_t flags;
  uint32_t generation;
  bool executing;
  sqlite_stmt_t *next;
  sqlite_stmt_t **prev;
};

struct sqlite_session {
  sqlite3_session *session;
  sqlite_db_t *db;
  bool busy;
  sqlite_session_t *next;
  sqlite_session_t **prev;
};

typedef struct {
  sqlite3 *dest;
  sqlite3_backup *backup;
} sqlite_backup_t;

typedef struct {
  sqlite_db_t *db;
  uint32_t fn_ref;
  bool bigint;
} sqlite_function_t;

typedef struct {
  sqlite_db_t *db;
  uint32_t start_ref;
  uint32_t step_ref;
  uint32_t result_ref;
  uint32_t inverse_ref;
  bool start_is_function;
  bool bigint;
} sqlite_aggregate_t;

typedef struct {
  uint32_t ref;
  bool ready;
} sqlite_aggregate_state_t;

typedef struct {
  sqlite_db_t *db;
  char *schema;
  uint8_t *hidden;
  uint32_t rows_ref;
  int column_count;
  int hidden_count;
  bool bigint;
  bool direct_only;
} sqlite_vmodule_t;

typedef struct {
  sqlite3_vtab base;
  sqlite_vmodule_t *module;
} sqlite_vtab_t;

typedef struct {
  sqlite3_vtab_cursor base;
  uint32_t next_ref;
  uint32_t row_ref;
  uint32_t args_ref;
  sqlite3_int64 rowid;
  bool eof;
} sqlite_cursor_t;

typedef struct {
  sqlite_db_t *db;
  ant_value_t filter;
  ant_value_t conflict;
} sqlite_apply_t;

typedef enum {
  SQLITE_READ_RUN,
  SQLITE_READ_GET,
  SQLITE_READ_ALL,
} sqlite_read_mode_t;

static constexpr uint32_t SQLITE_DB_NATIVE_TAG = Ant_NativeTag("SQDB");
static constexpr uint32_t SQLITE_STMT_NATIVE_TAG = Ant_NativeTag("SQST");
static constexpr uint32_t SQLITE_SESSION_NATIVE_TAG = Ant_NativeTag("SQSS");
static constexpr uint32_t SQLITE_BACKUP_NATIVE_TAG = Ant_NativeTag("SQBK");
static constexpr uint32_t SQLITE_MODULE_NATIVE_TAG = Ant_NativeTag("SQMD");
static constexpr double SQLITE_MAX_SAFE_INTEGER = 9007199254740991.0;

static const char *const sqlite_limit_names[SQLITE_LIMIT_COUNT] = {
  "length", "sqlLength", "column", "exprDepth", "compoundSelect", "vdbeOp",
  "functionArg", "attach", "likePatternLength", "variableNumber", "triggerDepth",
};

static const struct { const char *name; int op; } sqlite_stat_counters[] = {
  { "fullscanStep", SQLITE_STMTSTATUS_FULLSCAN_STEP },
  { "sort", SQLITE_STMTSTATUS_SORT },
  { "autoindex", SQLITE_STMTSTATUS_AUTOINDEX },
  { "vmStep", SQLITE_STMTSTATUS_VM_STEP },
  { "reprepare", SQLITE_STMTSTATUS_REPREPARE },
  { "run", SQLITE_STMTSTATUS_RUN },
  { "filterMiss", SQLITE_STMTSTATUS_FILTER_MISS },
  { "filterHit", SQLITE_STMTSTATUS_FILTER_HIT },
  { "memused", SQLITE_STMTSTATUS_MEMUSED },
};

__attribute__((format(printf, 4, 5)))
static ant_value_t sqlite_code_error(ant_t *js, js_err_type_t type, const char *code, const char *fmt, ...) {
  char message[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);

  ant_value_t props = Ant_Object_Create(js);
  js_set(js, props, "code", js_mkstr(js, code, strlen(code)));
  return js_throw(js, Ant_Error_CreateWithProps(js, type, props, message));
}

static ant_value_t sqlite_state_error(ant_t *js, const char *message) {
  return sqlite_code_error(js, JS_ERR_GENERIC, "ERR_INVALID_STATE", "%s", message);
}

static ant_value_t sqlite_arg_type_error(ant_t *js, const char *name, const char *expected) {
  return sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", "The \"%s\" argument must be %s.", name, expected);
}

static ant_value_t sqlite_unsupported(ant_t *js, const char *feature) {
  return sqlite_code_error(
    js, JS_ERR_GENERIC, "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM",
    "%s is not supported by the SQLite library in use", feature
  );
}

static ant_value_t sqlite_result_error_value(ant_t *js, int code, const char *message) {
  const char *errstr = SQ(errstr)(code);
  ant_value_t props = Ant_Object_Create(js);

  js_set(js, props, "code", Ant_String_FromLiteral(js, "ERR_SQLITE_ERROR"));
  js_set(js, props, "errcode", Ant_Value_FromNumber(code));
  js_set(js, props, "errstr", js_mkstr(js, errstr ? errstr : "", errstr ? strlen(errstr) : 0));

  return js_throw(js, Ant_Error_CreateWithProps(js, JS_ERR_GENERIC, props, message ? message : ""));
}

static ant_value_t sqlite_handle_error(ant_t *js, sqlite3 *handle) {
  if (!handle) return sqlite_result_error_value(js, SQLITE_NOMEM, "out of memory");
  return sqlite_result_error_value(js, SQ(extended_errcode)(handle), SQ(errmsg)(handle));
}

static ant_value_t sqlite_refs(sqlite_db_t *db) {
  return js_get_slot(db->self, SLOT_DATA);
}

static ant_value_t sqlite_ref_get(sqlite_db_t *db, uint32_t ref) {
  return js_arr_get(db->js, sqlite_refs(db), (ant_offset_t)ref);
}

static void sqlite_ref_set(sqlite_db_t *db, uint32_t ref, ant_value_t value) {
  js_setprop_index(db->js, sqlite_refs(db), ref, value);
}

static uint32_t sqlite_ref_alloc(sqlite_db_t *db, ant_value_t value) {
  uint32_t ref = db->free_count > 0 ? db->free_refs[--db->free_count] : db->refs_len++;
  sqlite_ref_set(db, ref, value);
  return ref;
}

static void sqlite_ref_free(sqlite_db_t *db, uint32_t ref) {
  if (ref < SQLITE_REF_FIRST || db->finalizing) return;
  sqlite_ref_set(db, ref, Ant_Value_Undefined);

  if (db->free_count == db->free_cap) {
    uint32_t cap = db->free_cap ? db->free_cap * 2 : 8;
    uint32_t *next = realloc(db->free_refs, cap * sizeof(*next));
    if (!next) return;
    db->free_refs = next;
    db->free_cap = cap;
  }
  db->free_refs[db->free_count++] = ref;
}

static void sqlite_set_pending(sqlite_db_t *db, ant_value_t error) {
  ant_value_t thrown = js_take_thrown(db->js, error);
  if (db->has_pending) return;
  db->has_pending = true;
  sqlite_ref_set(db, SQLITE_REF_PENDING, thrown);
}

static ant_value_t sqlite_take_error(sqlite_db_t *db) {
  if (!db->has_pending) return sqlite_handle_error(db->js, db->db);

  ant_value_t error = sqlite_ref_get(db, SQLITE_REF_PENDING);
  db->has_pending = false;
  sqlite_ref_set(db, SQLITE_REF_PENDING, Ant_Value_Undefined);
  return js_throw(db->js, error);
}

static ant_value_t sqlite_callback(sqlite_db_t *db, ant_value_t fn, ant_value_t *args, int nargs) {
  db->callback_depth++;
  ant_value_t result = sv_call_value(db->js, fn, Ant_Value_Undefined, args, nargs);
  db->callback_depth--;

  if (is_err(result)) sqlite_set_pending(db, result);
  return result;
}

static ant_value_t sqlite_bytes(ant_t *js, const void *bytes, size_t len) {
  ArrayBufferData *data = create_array_buffer_data(js, len);
  if (!data) return Ant_Error_CreateOutOfMemory(js);
  if (len > 0) memcpy(data->data, bytes, len);
  return create_typed_array(js, TYPED_ARRAY_UINT8, data, 0, len, js->builtins.typedarray_protos[TYPED_ARRAY_UINT8]);
}

static ant_value_t sqlite_integer(ant_t *js, sqlite3_int64 integer, bool bigint) {
  if (bigint) return bigint_from_int64(js, integer);
  if ((double)integer > SQLITE_MAX_SAFE_INTEGER || (double)integer < -SQLITE_MAX_SAFE_INTEGER)
    return sqlite_code_error(
      js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE",
      "Value is too large to be represented as a JavaScript number: %lld", (long long)integer
    );
  return Ant_Value_FromNumber((double)integer);
}

static ant_value_t sqlite_string(ant_t *js, const char *text) {
  return text ? js_mkstr(js, text, strlen(text)) : Ant_Value_Null;
}

static ant_value_t sqlite_column_value(ant_t *js, sqlite3_stmt *stmt, int column, bool bigint) {
  switch (SQ(column_type)(stmt, column)) {
    case SQLITE_INTEGER:
      return sqlite_integer(js, SQ(column_int64)(stmt, column), bigint);
    case SQLITE_FLOAT:
      return Ant_Value_FromNumber(SQ(column_double)(stmt, column));
    case SQLITE_TEXT: {
      const unsigned char *text = SQ(column_text)(stmt, column);
      return js_mkstr(js, text ? (const char *)text : "", (size_t)SQ(column_bytes)(stmt, column));
    }
    case SQLITE_BLOB: {
      const void *blob = SQ(column_blob)(stmt, column);
      return sqlite_bytes(js, blob, (size_t)SQ(column_bytes)(stmt, column));
    }
    default:
      return Ant_Value_Null;
  }
}

static ant_value_t sqlite_value(ant_t *js, sqlite3_value *value, bool bigint) {
  switch (SQ(value_type)(value)) {
    case SQLITE_INTEGER:
      return sqlite_integer(js, SQ(value_int64)(value), bigint);
    case SQLITE_FLOAT:
      return Ant_Value_FromNumber(SQ(value_double)(value));
    case SQLITE_TEXT: {
      const unsigned char *text = SQ(value_text)(value);
      return js_mkstr(js, text ? (const char *)text : "", (size_t)SQ(value_bytes)(value));
    }
    case SQLITE_BLOB: {
      const void *blob = SQ(value_blob)(value);
      return sqlite_bytes(js, blob, (size_t)SQ(value_bytes)(value));
    }
    default:
      return Ant_Value_Null;
  }
}

static void sqlite_result(sqlite_db_t *db, sqlite3_context *ctx, ant_value_t value) {
  ant_t *js = db->js;

  switch (vtype(value)) {
    case kTypeUndefined:
    case kTypeNull:
      SQ(result_null)(ctx);
      return;
    case kTypeNumber:
      SQ(result_double)(ctx, Ant_Value_AsNumberUnchecked(value));
      return;
    case kTypeBigInt: {
      int64_t integer = 0;
      if (bigint_to_int64_checked(js, value, &integer)) SQ(result_int64)(ctx, integer);
      else SQ(result_error)(ctx, "BigInt value is too large for SQLite", -1);
      return;
    }
    case kTypeString: {
      size_t len = 0;
      const char *str = js_getstr(js, value, &len);
      SQ(result_text64)(ctx, str ? str : "", len, SQLITE_TRANSIENT, SQLITE_UTF8);
      return;
    }
    case kTypePromise:
      SQ(result_error)(ctx, "Asynchronous user-defined functions are not supported", -1);
      return;
    default: {
      const uint8_t *bytes = NULL;
      size_t len = 0;
      if (is_object_type(value) && buffer_is_binary_source(value) && buffer_source_get_bytes(js, value, &bytes, &len))
        SQ(result_blob64)(ctx, bytes ? (const void *)bytes : "", len, SQLITE_TRANSIENT);
      else SQ(result_error)(ctx, "Returned JavaScript value cannot be converted to a SQLite value", -1);
    }
  }
}

static bool sqlite_opt_bool(ant_t *js, ant_value_t options, const char *key, const char *label, bool *out, ant_value_t *err) {
  ant_value_t value = js_get(js, options, key);
  if (is_err(value)) {
    *err = value;
    return false;
  }
  if (is_undefined(value)) return true;
  if (vtype(value) != kTypeBool) {
    *err = sqlite_arg_type_error(js, label, "a boolean");
    return false;
  }
  *out = value == Ant_Value_True;
  return true;
}

static bool sqlite_opt_flag(ant_t *js, ant_value_t options, const char *key, const char *label, uint32_t *flags, uint32_t bit, ant_value_t *err) {
  bool enabled = (*flags & bit) != 0;
  if (!sqlite_opt_bool(js, options, key, label, &enabled, err)) return false;
  *flags = enabled ? (*flags | bit) : (*flags & ~bit);
  return true;
}

static const char *sqlite_string_arg(ant_t *js, ant_value_t value, const char *name, size_t *len, ant_value_t *err) {
  if (vtype(value) != kTypeString) {
    *err = sqlite_arg_type_error(js, name, "a string");
    return NULL;
  }
  size_t local = 0;
  const char *str = js_getstr(js, value, len ? len : &local);
  if (!str) *err = Ant_Error_CreateOutOfMemory(js);
  return str;
}

static bool sqlite_opt_string(ant_t *js, ant_value_t options, const char *key, const char *label, const char **out, ant_value_t *err) {
  ant_value_t value = js_get(js, options, key);
  if (is_err(value)) {
    *err = value;
    return false;
  }
  if (is_undefined(value)) return true;
  *out = sqlite_string_arg(js, value, label, NULL, err);
  return *out != NULL;
}

static bool sqlite_is_integer(ant_value_t value, double *out) {
  if (vtype(value) != kTypeNumber) return false;
  double number = Ant_Value_AsNumberUnchecked(value);
  if (!isfinite(number) || number != trunc(number)) return false;
  *out = number;
  return true;
}

static sqlite_db_t *sqlite_db_state(ant_t *js, ant_value_t self, ant_value_t *err) {
  sqlite_db_t *db = is_object_type(self) ? js_get_native(self, SQLITE_DB_NATIVE_TAG) : NULL;
  if (!db) *err = sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_THIS", "Value of \"this\" must be of type Database");
  return db;
}

static sqlite_db_t *sqlite_db_open_state(ant_t *js, ant_value_t self, ant_value_t *err) {
  sqlite_db_t *db = sqlite_db_state(js, self, err);
  if (db && !db->db) {
    *err = sqlite_state_error(js, "database is not open");
    return NULL;
  }
  return db;
}

static sqlite_db_t *sqlite_db_idle_state(ant_t *js, ant_value_t self, ant_value_t *err) {
  sqlite_db_t *db = sqlite_db_open_state(js, self, err);
  if (db && db->authorizer_depth > 0) {
    *err = sqlite_state_error(js, "database cannot be used from within an authorizer callback");
    return NULL;
  }
  return db;
}

static sqlite_module_state_t *sqlite_module_state(ant_t *js) {
  return js_get_native(js->builtins.sqlite_database_proto, SQLITE_MODULE_NATIVE_TAG);
}

static int sqlite_trace(unsigned type, void *ctx, void *stmt, void *elapsed) {
  sqlite_db_t *db = ctx;
  ant_t *js = db->js;
  ant_value_t hook = js->builtins.sqlite_query_hook;

  if (type != SQLITE_TRACE_PROFILE || !is_callable(hook) || db->finalizing) return 0;

  char *expanded = SQ_HAS(expanded_sql) ? SQ(expanded_sql)(stmt) : NULL;
  const char *sql = expanded ? expanded : SQ(sql)(stmt);

  ant_value_t args[3] = {
    js_mkstr(js, sql ? sql : "", sql ? strlen(sql) : 0),
    db->self,
    Ant_Value_FromNumber((double)*(sqlite3_int64 *)elapsed),
  };

  if (expanded) SQ(free)(expanded);
  sqlite_callback(db, hook, args, 3);

  return 0;
}

static void sqlite_sync_trace(sqlite_db_t *db) {
  if (!db->db || !SQ_HAS(trace_v2)) return;
  bool enabled = is_callable(db->js->builtins.sqlite_query_hook);
  SQ(trace_v2)(db->db, enabled ? SQLITE_TRACE_PROFILE : 0, enabled ? sqlite_trace : NULL, db);
}

static void sqlite_stmt_release(sqlite_stmt_t *st) {
  if (st->stmt) SQ(finalize)(st->stmt);
  st->stmt = NULL;

  if (!st->prev) return;
  *st->prev = st->next;
  if (st->next) st->next->prev = st->prev;
  st->prev = NULL;
  st->next = NULL;
}

static void sqlite_session_release(sqlite_session_t *session) {
  if (session->session && SQ_HAS(session_delete)) SQ(session_delete)(session->session);
  session->session = NULL;

  if (!session->prev) return;
  *session->prev = session->next;
  if (session->next) session->next->prev = session->prev;
  session->prev = NULL;
  session->next = NULL;
}

static void sqlite_db_release_statements(sqlite_db_t *db) {
  while (db->stmts) sqlite_stmt_release(db->stmts);
}

static void sqlite_db_shutdown(sqlite_db_t *db) {
  if (!db->db) return;

  sqlite_db_release_statements(db);
  while (db->sessions) sqlite_session_release(db->sessions);

  if (db->prev) {
    *db->prev = db->next;
    if (db->next) db->next->prev = db->prev;
    db->prev = NULL;
    db->next = NULL;
  }

  SQ(close_v2)(db->db);
  db->db = NULL;
}

static void sqlite_db_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  sqlite_db_t *db = js_get_native(value, SQLITE_DB_NATIVE_TAG);

  if (db) {
    db->finalizing = true;
    for (sqlite_stmt_t *st = db->stmts; st; st = st->next) st->db = NULL;
    for (sqlite_session_t *session = db->sessions; session; session = session->next) session->db = NULL;
    sqlite_db_shutdown(db);
    free(db->free_refs);
    free(db->path);
    free(db);
  }

  js_clear_native(value, SQLITE_DB_NATIVE_TAG);
}

static void sqlite_stmt_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  sqlite_stmt_t *st = js_get_native(value, SQLITE_STMT_NATIVE_TAG);

  if (st) {
    sqlite_stmt_release(st);
    free(st);
  }

  js_clear_native(value, SQLITE_STMT_NATIVE_TAG);
}

static void sqlite_session_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  sqlite_session_t *session = js_get_native(value, SQLITE_SESSION_NATIVE_TAG);

  if (session) {
    sqlite_session_release(session);
    free(session);
  }

  js_clear_native(value, SQLITE_SESSION_NATIVE_TAG);
}

static void sqlite_backup_release(sqlite_backup_t *backup) {
  if (backup->backup) SQ(backup_finish)(backup->backup);
  if (backup->dest) SQ(close_v2)(backup->dest);
  backup->backup = NULL;
  backup->dest = NULL;
}

static void sqlite_backup_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  sqlite_backup_t *backup = js_get_native(value, SQLITE_BACKUP_NATIVE_TAG);

  if (backup) {
    sqlite_backup_release(backup);
    free(backup);
  }

  js_clear_native(value, SQLITE_BACKUP_NATIVE_TAG);
}

static void sqlite_module_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, SQLITE_MODULE_NATIVE_TAG));
  js_clear_native(value, SQLITE_MODULE_NATIVE_TAG);
}

static ant_value_t sqlite_db_do_open(ant_t *js, sqlite_db_t *db) {
  int open_flags = (db->flags & SQLITE_OPT_READ_ONLY)
    ? SQLITE_OPEN_READONLY
    : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;

  if (SQ(open_v2)(db->path, &db->db, open_flags | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
    ant_value_t err = sqlite_handle_error(js, db->db);
    SQ(close_v2)(db->db);
    db->db = NULL;
    return err;
  }

  SQ(extended_result_codes)(db->db, 1);
  SQ(db_config)(db->db, SQLITE_DBCONFIG_ENABLE_FKEY, (db->flags & SQLITE_OPT_FOREIGN_KEYS) != 0, NULL);
  SQ(db_config)(db->db, SQLITE_DBCONFIG_DQS_DML, (db->flags & SQLITE_OPT_DQS) != 0, NULL);
  SQ(db_config)(db->db, SQLITE_DBCONFIG_DQS_DDL, (db->flags & SQLITE_OPT_DQS) != 0, NULL);
  SQ(db_config)(db->db, SQLITE_DBCONFIG_DEFENSIVE, (db->flags & SQLITE_OPT_DEFENSIVE) != 0, NULL);
  SQ(busy_timeout)(db->db, db->timeout);

  for (int i = 0; i < SQLITE_LIMIT_COUNT; i++)
    if (db->limits[i] >= 0) SQ(limit)(db->db, i, db->limits[i]);

  if (db->flags & SQLITE_OPT_ALLOW_EXTENSION) {
    db->flags |= SQLITE_OPT_EXTENSION_ENABLED;
    if (SQ_HAS(enable_load_extension)) SQ(enable_load_extension)(db->db, 1);
  }

  sqlite_module_state_t *module = sqlite_module_state(js);
  db->next = module->open;
  db->prev = &module->open;
  if (module->open) module->open->prev = &db->next;
  module->open = db;

  sqlite_sync_trace(db);
  return Ant_Value_Undefined;
}

static ant_value_t sqlite_db_parse_options(ant_t *js, sqlite_db_t *db, ant_value_t options, bool *open) {
  ant_value_t err = Ant_Value_Undefined;
  if (is_undefined(options)) return err;
  if (!is_object_type(options)) return sqlite_arg_type_error(js, "options", "an object");

  if (
    !sqlite_opt_bool(js, options, "open", "options.open", open, &err) ||
    !sqlite_opt_flag(js, options, "readOnly", "options.readOnly", &db->flags, SQLITE_OPT_READ_ONLY, &err) ||
    !sqlite_opt_flag(js, options, "enableForeignKeyConstraints", "options.enableForeignKeyConstraints", &db->flags, SQLITE_OPT_FOREIGN_KEYS, &err) ||
    !sqlite_opt_flag(js, options, "enableDoubleQuotedStringLiterals", "options.enableDoubleQuotedStringLiterals", &db->flags, SQLITE_OPT_DQS, &err) ||
    !sqlite_opt_flag(js, options, "allowExtension", "options.allowExtension", &db->flags, SQLITE_OPT_ALLOW_EXTENSION, &err) ||
    !sqlite_opt_flag(js, options, "readBigInts", "options.readBigInts", &db->flags, SQLITE_OPT_READ_BIGINTS, &err) ||
    !sqlite_opt_flag(js, options, "returnArrays", "options.returnArrays", &db->flags, SQLITE_OPT_RETURN_ARRAYS, &err) ||
    !sqlite_opt_flag(js, options, "allowBareNamedParameters", "options.allowBareNamedParameters", &db->flags, SQLITE_OPT_BARE_NAMED, &err) ||
    !sqlite_opt_flag(js, options, "allowUnknownNamedParameters", "options.allowUnknownNamedParameters", &db->flags, SQLITE_OPT_UNKNOWN_NAMED, &err) ||
    !sqlite_opt_flag(js, options, "defensive", "options.defensive", &db->flags, SQLITE_OPT_DEFENSIVE, &err)
  ) return err;

  ant_value_t timeout = js_get(js, options, "timeout");
  if (is_err(timeout)) return timeout;
  if (!is_undefined(timeout)) {
    double number = 0;
    if (!sqlite_is_integer(timeout, &number) || number < INT32_MIN || number > INT32_MAX)
      return sqlite_arg_type_error(js, "options.timeout", "an integer");
    db->timeout = (int)number;
  }

  ant_value_t limits = js_get(js, options, "limits");
  if (is_err(limits)) return limits;
  if (is_undefined(limits)) return err;
  if (!is_object_type(limits)) return sqlite_arg_type_error(js, "options.limits", "an object");

  for (int i = 0; i < SQLITE_LIMIT_COUNT; i++) {
    ant_value_t limit = js_get(js, limits, sqlite_limit_names[i]);
    if (is_err(limit)) return limit;
    if (is_undefined(limit)) continue;

    double number = 0;
    if (vtype(limit) == kTypeNumber && Ant_Value_AsNumberUnchecked(limit) == HUGE_VAL) {
      db->limits[i] = INT32_MAX;
      continue;
    }
    if (!sqlite_is_integer(limit, &number) || number < 0 || number > INT32_MAX)
      return sqlite_code_error(
        js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE",
        "The \"options.limits.%s\" argument must be non-negative.",
        sqlite_limit_names[i]
      );
    db->limits[i] = (int)number;
  }

  return err;
}

static ant_value_t sqlite_db_ctor(ant_params_t) {
  if (is_undefined(call_new_target))
    return sqlite_code_error(js, JS_ERR_TYPE, "ERR_CONSTRUCT_CALL_REQUIRED", "Cannot call constructor without `new`");

  size_t path_len = 0;
  ant_value_t path_value = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(path_value) != kTypeString && is_callable(js->builtins.sqlite_path_hook)) {
    path_value = sv_call_value(js, js->builtins.sqlite_path_hook, Ant_Value_Undefined, &path_value, 1);
    if (is_err(path_value)) return path_value;
  }

  const char *path = vtype(path_value) == kTypeString ? js_getstr(js, path_value, &path_len) : NULL;
  if (!path || strlen(path) != path_len) return sqlite_code_error(
    js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE",
    "The \"path\" argument must be a string, Uint8Array, or URL without null bytes."
  );

  sqlite_db_t *db = calloc(1, sizeof(*db));
  if (!db) return Ant_Error_CreateOutOfMemory(js);

  db->js = js;
  db->path = strdup(path);
  db->flags = SQLITE_OPT_FOREIGN_KEYS | SQLITE_OPT_BARE_NAMED | SQLITE_OPT_DEFENSIVE;
  db->refs_len = SQLITE_REF_FIRST;
  for (int i = 0; i < SQLITE_LIMIT_COUNT; i++) db->limits[i] = -1;

  bool open = true;
  ant_value_t err = db->path
    ? sqlite_db_parse_options(js, db, nargs > 1 ? args[1] : Ant_Value_Undefined, &open)
    : Ant_Error_CreateOutOfMemory(js);

  if (is_err(err)) {
    free(db->path);
    free(db);
    return err;
  }

  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.sqlite_database_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  db->self = obj;
  js_set_native(obj, db, SQLITE_DB_NATIVE_TAG);
  js_set_finalizer(obj, sqlite_db_finalize);

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t refs = Ant_Array_Create(js);
  js_set_slot_wb(js, obj, SLOT_DATA, refs);
  js_arr_push(js, refs, Ant_Value_Undefined);
  js_arr_push(js, refs, Ant_Value_Undefined);

  err = open ? sqlite_db_do_open(js, db) : Ant_Value_Undefined;
  GC_ROOT_RESTORE(js, mark);

  return is_err(err) ? err : obj;
}

static ant_value_t sqlite_db_open(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_state(js, call_this, &err);
  if (!db) return err;
  if (db->db) return sqlite_state_error(js, "database is already open");
  return sqlite_db_do_open(js, db);
}

static ant_value_t sqlite_db_close(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_open_state(js, call_this, &err);
  if (!db) return err;
  if (db->callback_depth > 0) return sqlite_state_error(js, "database cannot be closed while a statement is executing");

  sqlite_db_shutdown(db);
  return Ant_Value_Undefined;
}

static ant_value_t sqlite_db_is_open(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_state(js, call_this, &err);
  return db ? Ant_Value_FromBool(db->db != NULL) : err;
}

static ant_value_t sqlite_db_is_transaction(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_open_state(js, call_this, &err);
  return db ? Ant_Value_FromBool(!SQ(get_autocommit)(db->db)) : err;
}

static ant_value_t sqlite_db_exec(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;

  const char *sql = sqlite_string_arg(js, nargs > 0 ? args[0] : Ant_Value_Undefined, "sql", NULL, &err);
  if (!sql) return err;

  if (SQ(exec)(db->db, sql, NULL, NULL, NULL) != SQLITE_OK) return sqlite_take_error(db);
  return db->has_pending ? sqlite_take_error(db) : Ant_Value_Undefined;
}

static ant_value_t sqlite_db_location(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_open_state(js, call_this, &err);
  if (!db) return err;

  const char *name = "main";
  if (nargs > 0 && !is_undefined(args[0])) {
    name = sqlite_string_arg(js, args[0], "dbName", NULL, &err);
    if (!name) return err;
  }

  const char *filename = SQ(db_filename)(db->db, name);
  return filename && filename[0] ? js_mkstr(js, filename, strlen(filename)) : Ant_Value_Null;
}

static ant_value_t sqlite_db_prepare(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;

  size_t sql_len = 0;
  const char *sql = sqlite_string_arg(js, nargs > 0 ? args[0] : Ant_Value_Undefined, "sql", &sql_len, &err);
  if (!sql) return err;

  uint32_t flags = db->flags;
  bool persistent = false;

  if (nargs > 1 && !is_undefined(args[1])) {
    ant_value_t options = args[1];
    if (!is_object_type(options)) return sqlite_arg_type_error(js, "options", "an object");
    if (
      !sqlite_opt_flag(js, options, "readBigInts", "options.readBigInts", &flags, SQLITE_OPT_READ_BIGINTS, &err) ||
      !sqlite_opt_flag(js, options, "returnArrays", "options.returnArrays", &flags, SQLITE_OPT_RETURN_ARRAYS, &err) ||
      !sqlite_opt_flag(js, options, "allowBareNamedParameters", "options.allowBareNamedParameters", &flags, SQLITE_OPT_BARE_NAMED, &err) ||
      !sqlite_opt_flag(js, options, "allowUnknownNamedParameters", "options.allowUnknownNamedParameters", &flags, SQLITE_OPT_UNKNOWN_NAMED, &err) ||
      !sqlite_opt_bool(js, options, "persistent", "options.persistent", &persistent, &err)
    ) return err;
  }

  sqlite_stmt_t *st = calloc(1, sizeof(*st));
  if (!st) return Ant_Error_CreateOutOfMemory(js);

  int rc = SQ_HAS(prepare_v3)
    ? SQ(prepare_v3)(db->db, sql, (int)sql_len, persistent ? SQLITE_PREPARE_PERSISTENT : 0, &st->stmt, NULL)
    : SQ(prepare_v2)(db->db, sql, (int)sql_len, &st->stmt, NULL);

  if (rc != SQLITE_OK) {
    free(st);
    return sqlite_take_error(db);
  }
  if (!st->stmt) {
    free(st);
    return sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_ARG_VALUE", "The SQL query contains no statements.");
  }

  st->db = db;
  st->flags = flags;
  st->next = db->stmts;
  st->prev = &db->stmts;
  if (db->stmts) db->stmts->prev = &st->next;
  db->stmts = st;

  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, js->builtins.sqlite_statement_proto);
  js_set_native(obj, st, SQLITE_STMT_NATIVE_TAG);
  js_set_finalizer(obj, sqlite_stmt_finalize);
  js_set_slot_wb(js, obj, SLOT_DATA, call_this);

  return obj;
}

static void sqlite_function_destroy(void *data) {
  sqlite_function_t *fn = data;
  sqlite_ref_free(fn->db, fn->fn_ref);
  free(fn);
}

static bool sqlite_call_args(sqlite_db_t *db, ant_value_t *out, int argc, sqlite3_value **argv, bool bigint) {
  for (int i = 0; i < argc; i++) {
    out[i] = sqlite_value(db->js, argv[i], bigint);
    if (!is_err(out[i])) continue;
    sqlite_set_pending(db, out[i]);
    return false;
  }
  return true;
}

static void sqlite_function_call(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  sqlite_function_t *fn = SQ(user_data)(ctx);
  sqlite_db_t *db = fn->db;

  ant_value_t stack[8];
  ant_value_t *values = argc <= 8 ? stack : malloc((size_t)argc * sizeof(*values));

  if (!values || db->has_pending || !sqlite_call_args(db, values, argc, argv, fn->bigint)) {
    SQ(result_error)(ctx, "", 0);
  } else {
    ant_value_t result = sqlite_callback(db, sqlite_ref_get(db, fn->fn_ref), values, argc);
    if (is_err(result)) SQ(result_error)(ctx, "", 0);
    else sqlite_result(db, ctx, result);
  }

  if (values != stack) free(values);
}

static ant_value_t sqlite_function_options(ant_t *js, ant_value_t options, int *text_rep, bool *bigint, bool *varargs) {
  ant_value_t err = Ant_Value_Undefined;
  bool deterministic = false;
  bool direct_only = false;

  if (
    !sqlite_opt_bool(js, options, "deterministic", "options.deterministic", &deterministic, &err) ||
    !sqlite_opt_bool(js, options, "directOnly", "options.directOnly", &direct_only, &err) ||
    !sqlite_opt_bool(js, options, "useBigIntArguments", "options.useBigIntArguments", bigint, &err) ||
    !sqlite_opt_bool(js, options, "varargs", "options.varargs", varargs, &err)
  ) return err;

  *text_rep = SQLITE_UTF8;
  if (deterministic) *text_rep |= SQLITE_DETERMINISTIC;
  if (direct_only) *text_rep |= SQLITE_DIRECTONLY;

  return err;
}

static int sqlite_function_arity(ant_t *js, ant_value_t fn, int reserved) {
  double length = 0;
  ant_value_t value = js_get(js, fn, "length");
  if (!sqlite_is_integer(value, &length) || length < reserved) return 0;
  return (int)length - reserved;
}

static ant_value_t sqlite_db_function(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;

  const char *name = sqlite_string_arg(js, nargs > 0 ? args[0] : Ant_Value_Undefined, "name", NULL, &err);
  if (!name) return err;

  ant_value_t options = nargs > 2 ? args[1] : Ant_Value_Undefined;
  ant_value_t callback = nargs > 2 ? args[2] : (nargs > 1 ? args[1] : Ant_Value_Undefined);

  int text_rep = SQLITE_UTF8;
  bool bigint = false;
  bool varargs = false;

  if (nargs > 2) {
    if (!is_object_type(options)) return sqlite_arg_type_error(js, "options", "an object");
    err = sqlite_function_options(js, options, &text_rep, &bigint, &varargs);
    if (is_err(err)) return err;
  }
  if (!is_callable(callback)) return sqlite_arg_type_error(js, "function", "a function");

  sqlite_function_t *fn = calloc(1, sizeof(*fn));
  if (!fn) return Ant_Error_CreateOutOfMemory(js);

  fn->db = db;
  fn->bigint = bigint;
  fn->fn_ref = sqlite_ref_alloc(db, callback);

  int argc = varargs ? -1 : sqlite_function_arity(js, callback, 0);
  if (SQ(create_function_v2)(db->db, name, argc, text_rep, fn, sqlite_function_call, NULL, NULL, sqlite_function_destroy) != SQLITE_OK)
    return sqlite_take_error(db);

  return Ant_Value_Undefined;
}

static void sqlite_aggregate_destroy(void *data) {
  sqlite_aggregate_t *agg = data;
  sqlite_ref_free(agg->db, agg->start_ref);
  sqlite_ref_free(agg->db, agg->step_ref);
  sqlite_ref_free(agg->db, agg->result_ref);
  sqlite_ref_free(agg->db, agg->inverse_ref);
  free(agg);
}

static sqlite_aggregate_state_t *sqlite_aggregate_state(sqlite3_context *ctx, sqlite_aggregate_t *agg) {
  sqlite_db_t *db = agg->db;
  sqlite_aggregate_state_t *state = SQ(aggregate_context)(ctx, sizeof(*state));
  if (!state || state->ready) return state;

  ant_value_t start = sqlite_ref_get(db, agg->start_ref);
  if (agg->start_is_function) {
    start = sqlite_callback(db, start, NULL, 0);
    if (is_err(start)) return NULL;
  }

  state->ref = sqlite_ref_alloc(db, start);
  state->ready = true;
  return state;
}

static void sqlite_aggregate_apply(sqlite3_context *ctx, int argc, sqlite3_value **argv, bool inverse) {
  sqlite_aggregate_t *agg = SQ(user_data)(ctx);
  sqlite_db_t *db = agg->db;
  if (db->has_pending) return;

  sqlite_aggregate_state_t *state = sqlite_aggregate_state(ctx, agg);
  if (!state) return;

  ant_value_t stack[8];
  ant_value_t *values = argc + 1 <= 8 ? stack : malloc((size_t)(argc + 1) * sizeof(*values));
  if (!values) return;

  values[0] = sqlite_ref_get(db, state->ref);
  if (sqlite_call_args(db, values + 1, argc, argv, agg->bigint)) {
    ant_value_t fn = sqlite_ref_get(db, inverse ? agg->inverse_ref : agg->step_ref);
    ant_value_t next = sqlite_callback(db, fn, values, argc + 1);
    if (!is_err(next)) sqlite_ref_set(db, state->ref, next);
  }

  if (values != stack) free(values);
}

static void sqlite_aggregate_step(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  sqlite_aggregate_apply(ctx, argc, argv, false);
}

static void sqlite_aggregate_inverse(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  sqlite_aggregate_apply(ctx, argc, argv, true);
}

static void sqlite_aggregate_emit(sqlite3_context *ctx, bool final) {
  sqlite_aggregate_t *agg = SQ(user_data)(ctx);
  sqlite_db_t *db = agg->db;

  sqlite_aggregate_state_t *state = db->has_pending
    ? SQ(aggregate_context)(ctx, 0)
    : sqlite_aggregate_state(ctx, agg);

  if (db->has_pending || !state) {
    SQ(result_error)(ctx, "", 0);
    if (final && state && state->ready) sqlite_ref_free(db, state->ref);
    return;
  }

  ant_value_t value = sqlite_ref_get(db, state->ref);
  if (agg->result_ref) value = sqlite_callback(db, sqlite_ref_get(db, agg->result_ref), &value, 1);

  if (is_err(value)) SQ(result_error)(ctx, "", 0);
  else sqlite_result(db, ctx, value);

  if (final) sqlite_ref_free(db, state->ref);
}

static void sqlite_aggregate_final(sqlite3_context *ctx) {
  sqlite_aggregate_emit(ctx, true);
}

static void sqlite_aggregate_value(sqlite3_context *ctx) {
  sqlite_aggregate_emit(ctx, false);
}

static ant_value_t sqlite_db_aggregate(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;

  const char *name = sqlite_string_arg(js, nargs > 0 ? args[0] : Ant_Value_Undefined, "name", NULL, &err);
  if (!name) return err;

  ant_value_t options = nargs > 1 ? args[1] : Ant_Value_Undefined;
  if (!is_object_type(options)) return sqlite_arg_type_error(js, "options", "an object");

  int text_rep = SQLITE_UTF8;
  bool bigint = false;
  bool varargs = false;
  err = sqlite_function_options(js, options, &text_rep, &bigint, &varargs);
  if (is_err(err)) return err;

  ant_value_t start = js_get(js, options, "start");
  ant_value_t step = js_get(js, options, "step");
  ant_value_t result = js_get(js, options, "result");
  ant_value_t inverse = js_get(js, options, "inverse");

  if (is_undefined(start)) return sqlite_arg_type_error(js, "options.start", "a function or a primitive value");
  if (!is_callable(step)) return sqlite_arg_type_error(js, "options.step", "a function");
  if (!is_undefined(result) && !is_callable(result)) return sqlite_arg_type_error(js, "options.result", "a function");
  if (!is_undefined(inverse) && !is_callable(inverse)) return sqlite_arg_type_error(js, "options.inverse", "a function");
  if (is_callable(inverse) && !SQ_HAS(create_window_function)) return sqlite_unsupported(js, "Window functions");

  sqlite_aggregate_t *agg = calloc(1, sizeof(*agg));
  if (!agg) return Ant_Error_CreateOutOfMemory(js);

  agg->db = db;
  agg->bigint = bigint;
  agg->start_is_function = is_callable(start);
  agg->start_ref = sqlite_ref_alloc(db, start);
  agg->step_ref = sqlite_ref_alloc(db, step);
  agg->result_ref = is_callable(result) ? sqlite_ref_alloc(db, result) : 0;
  agg->inverse_ref = is_callable(inverse) ? sqlite_ref_alloc(db, inverse) : 0;

  int argc = varargs ? -1 : sqlite_function_arity(js, step, 1);
  if (!varargs && is_callable(inverse)) {
    int inverse_argc = sqlite_function_arity(js, inverse, 1);
    if (inverse_argc > argc) argc = inverse_argc;
  }

  int rc = SQ_HAS(create_window_function)
    ? SQ(create_window_function)(
        db->db, name, argc, text_rep, agg,
        sqlite_aggregate_step, sqlite_aggregate_final,
        is_callable(inverse) ? sqlite_aggregate_value : NULL,
        is_callable(inverse) ? sqlite_aggregate_inverse : NULL,
        sqlite_aggregate_destroy
      )
    : SQ(create_function_v2)(
        db->db, name, argc, text_rep, agg, NULL,
        sqlite_aggregate_step, sqlite_aggregate_final, sqlite_aggregate_destroy
      );

  return rc == SQLITE_OK ? Ant_Value_Undefined : sqlite_take_error(db);
}

static int sqlite_authorize(void *data, int action, const char *arg1, const char *arg2, const char *db_name, const char *trigger) {
  sqlite_db_t *db = data;
  ant_t *js = db->js;
  if (db->has_pending) return SQLITE_DENY;

  ant_value_t args[5] = {
    Ant_Value_FromNumber(action),
    sqlite_string(js, arg1),
    sqlite_string(js, arg2),
    sqlite_string(js, db_name),
    sqlite_string(js, trigger),
  };

  db->authorizer_depth++;
  ant_value_t result = sqlite_callback(db, sqlite_ref_get(db, SQLITE_REF_AUTHORIZER), args, 5);
  db->authorizer_depth--;

  if (is_err(result)) return SQLITE_DENY;

  double code = 0;
  if (!sqlite_is_integer(result, &code)) {
    sqlite_set_pending(db, js_mkerr_typed(js, JS_ERR_TYPE, "Authorizer callback must return an integer authorization code"));
    return SQLITE_DENY;
  }
  if (code != SQLITE_OK && code != SQLITE_DENY && code != SQLITE_IGNORE) {
    sqlite_set_pending(db, js_mkerr_typed(js, JS_ERR_RANGE, "Authorizer callback returned a invalid authorization code"));
    return SQLITE_DENY;
  }

  return (int)code;
}

static ant_value_t sqlite_db_set_authorizer(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;

  ant_value_t callback = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (is_null(callback)) {
    SQ(set_authorizer)(db->db, NULL, NULL);
    sqlite_ref_set(db, SQLITE_REF_AUTHORIZER, Ant_Value_Undefined);
    return Ant_Value_Undefined;
  }
  if (!is_callable(callback)) return sqlite_arg_type_error(js, "callback", "a function or null");

  sqlite_ref_set(db, SQLITE_REF_AUTHORIZER, callback);
  if (SQ(set_authorizer)(db->db, sqlite_authorize, db) != SQLITE_OK) return sqlite_take_error(db);

  return Ant_Value_Undefined;
}

static ant_value_t sqlite_db_enable_load_extension(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_open_state(js, call_this, &err);
  if (!db) return err;

  ant_value_t allow = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(allow) != kTypeBool) return sqlite_arg_type_error(js, "allow", "a boolean");

  bool enable = allow == Ant_Value_True;
  if (enable && !(db->flags & SQLITE_OPT_ALLOW_EXTENSION))
    return sqlite_state_error(js, "Cannot enable extension loading because it was disabled at database creation.");
  if (enable && !SQ_HAS(enable_load_extension)) return sqlite_unsupported(js, "Extension loading");

  db->flags = enable ? (db->flags | SQLITE_OPT_EXTENSION_ENABLED) : (db->flags & ~SQLITE_OPT_EXTENSION_ENABLED);
  if (SQ_HAS(enable_load_extension)) SQ(enable_load_extension)(db->db, enable);

  return Ant_Value_Undefined;
}

static ant_value_t sqlite_db_load_extension(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;

  if (!(db->flags & SQLITE_OPT_ALLOW_EXTENSION) || !(db->flags & SQLITE_OPT_EXTENSION_ENABLED))
    return sqlite_state_error(js, "extension loading is not allowed");
  if (!SQ_HAS(load_extension)) return sqlite_unsupported(js, "Extension loading");

  const char *path = sqlite_string_arg(js, nargs > 0 ? args[0] : Ant_Value_Undefined, "path", NULL, &err);
  if (!path) return err;

  const char *entry = NULL;
  if (nargs > 1 && !is_undefined(args[1])) {
    entry = sqlite_string_arg(js, args[1], "entryPoint", NULL, &err);
    if (!entry) return err;
  }

  char *message = NULL;
  int rc = SQ(load_extension)(db->db, path, entry, &message);
  if (rc == SQLITE_OK) return Ant_Value_Undefined;

  err = sqlite_result_error_value(js, rc, message ? message : SQ(errmsg)(db->db));
  SQ(free)(message);
  return err;
}

static ant_value_t sqlite_db_enable_defensive(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_open_state(js, call_this, &err);
  if (!db) return err;

  ant_value_t active = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(active) != kTypeBool) return sqlite_arg_type_error(js, "active", "a boolean");

  SQ(db_config)(db->db, SQLITE_DBCONFIG_DEFENSIVE, active == Ant_Value_True, NULL);
  return Ant_Value_Undefined;
}

static ant_value_t sqlite_db_limit(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_open_state(js, call_this, &err);
  if (!db) return err;

  double id = 0;
  double value = -1;
  if (nargs < 1 || !sqlite_is_integer(args[0], &id) || id < 0 || id >= (double)SQLITE_LIMIT_COUNT)
    return sqlite_arg_type_error(js, "id", "a limit identifier");
  if (nargs > 1 && (!sqlite_is_integer(args[1], &value) || value < 0 || value > INT32_MAX))
    return sqlite_code_error(js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE", "Limit value must be non-negative.");

  int previous = SQ(limit)(db->db, (int)id, (int)value);
  return Ant_Value_FromNumber(nargs > 1 ? SQ(limit)(db->db, (int)id, -1) : previous);
}

static ant_value_t sqlite_db_serialize(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;
  if (!SQ_HAS(serialize)) return sqlite_unsupported(js, "serialize()");

  const char *name = "main";
  if (nargs > 0 && !is_undefined(args[0])) {
    name = sqlite_string_arg(js, args[0], "dbName", NULL, &err);
    if (!name) return err;
  }

  sqlite3_int64 size = 0;
  unsigned char *data = SQ(serialize)(db->db, name, &size, 0);
  if (!data && size > 0) return sqlite_take_error(db);

  ant_value_t bytes = sqlite_bytes(js, data, (size_t)size);
  SQ(free)(data);
  return bytes;
}

static ant_value_t sqlite_db_deserialize(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;
  if (!SQ_HAS(deserialize)) return sqlite_unsupported(js, "deserialize()");
  if (db->callback_depth > 0) return sqlite_state_error(js, "database cannot be deserialized while a statement is executing");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t buffer = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (!is_object_type(buffer) || !buffer_is_binary_source(buffer) || !buffer_source_get_bytes(js, buffer, &bytes, &len))
    return sqlite_arg_type_error(js, "buffer", "a Uint8Array");

  const char *name = "main";
  if (nargs > 1 && !is_undefined(args[1])) {
    if (!is_object_type(args[1])) return sqlite_arg_type_error(js, "options", "an object");
    if (!sqlite_opt_string(js, args[1], "dbName", "options.dbName", &name, &err)) return err;
  }

  if (len == 0) return sqlite_code_error(js, JS_ERR_GENERIC, "ERR_INVALID_ARG_VALUE", "The \"buffer\" argument must not be empty.");

  sqlite_db_release_statements(db);

  unsigned char *copy = SQ(malloc64)(len);
  if (!copy) return Ant_Error_CreateOutOfMemory(js);
  memcpy(copy, bytes, len);

  int rc = SQ(deserialize)(
    db->db, name, copy, (sqlite3_int64)len, (sqlite3_int64)len,
    SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE
  );

  return rc == SQLITE_OK ? Ant_Value_Undefined : sqlite_take_error(db);
}

static ant_value_t sqlite_db_create_session(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;
  if (!SQ_HAS(session_create) || !SQ_HAS(session_attach) || !SQ_HAS(session_delete))
    return sqlite_unsupported(js, "The session extension");

  const char *table = NULL;
  const char *name = "main";
  if (nargs > 0 && !is_undefined(args[0])) {
    if (!is_object_type(args[0])) return sqlite_arg_type_error(js, "options", "an object");
    if (
      !sqlite_opt_string(js, args[0], "table", "options.table", &table, &err) ||
      !sqlite_opt_string(js, args[0], "db", "options.db", &name, &err)
    ) return err;
  }

  sqlite_session_t *session = calloc(1, sizeof(*session));
  if (!session) return Ant_Error_CreateOutOfMemory(js);

  if (SQ(session_create)(db->db, name, &session->session) != SQLITE_OK) {
    free(session);
    return sqlite_take_error(db);
  }
  if (SQ(session_attach)(session->session, table) != SQLITE_OK) {
    SQ(session_delete)(session->session);
    free(session);
    return sqlite_take_error(db);
  }

  session->db = db;
  session->next = db->sessions;
  session->prev = &db->sessions;
  if (db->sessions) db->sessions->prev = &session->next;
  db->sessions = session;

  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_proto_init(obj, js->builtins.sqlite_session_proto);
  js_set_native(obj, session, SQLITE_SESSION_NATIVE_TAG);
  js_set_finalizer(obj, sqlite_session_finalize);
  js_set_slot_wb(js, obj, SLOT_DATA, call_this);

  return obj;
}

static int sqlite_apply_filter(void *data, const char *table) {
  sqlite_apply_t *apply = data;
  sqlite_db_t *db = apply->db;
  if (db->has_pending) return 0;

  ant_value_t arg = sqlite_string(db->js, table);
  ant_value_t result = sqlite_callback(db, apply->filter, &arg, 1);
  return !is_err(result) && js_truthy(db->js, result);
}

static int sqlite_apply_conflict(void *data, int conflict, sqlite3_changeset_iter *iter) {
  sqlite_apply_t *apply = data;
  sqlite_db_t *db = apply->db;
  if (db->has_pending || !is_callable(apply->conflict)) return SQLITE_CHANGESET_ABORT;

  ant_value_t arg = Ant_Value_FromNumber(conflict);
  ant_value_t result = sqlite_callback(db, apply->conflict, &arg, 1);

  double code = 0;
  if (is_err(result) || !sqlite_is_integer(result, &code)) return SQLITE_CHANGESET_ABORT;
  if (code == SQLITE_CHANGESET_OMIT || code == SQLITE_CHANGESET_REPLACE) return (int)code;
  return SQLITE_CHANGESET_ABORT;
}

static ant_value_t sqlite_db_apply_changeset(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;
  if (!SQ_HAS(changeset_apply)) return sqlite_unsupported(js, "The session extension");

  const uint8_t *bytes = NULL;
  size_t len = 0;
  ant_value_t changeset = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (!is_object_type(changeset) || !buffer_is_binary_source(changeset) || !buffer_source_get_bytes(js, changeset, &bytes, &len))
    return sqlite_arg_type_error(js, "changeset", "a Uint8Array");

  sqlite_apply_t apply = { .db = db, .filter = Ant_Value_Undefined, .conflict = Ant_Value_Undefined };
  if (nargs > 1 && !is_undefined(args[1])) {
    if (!is_object_type(args[1])) return sqlite_arg_type_error(js, "options", "an object");

    apply.filter = js_get(js, args[1], "filter");
    apply.conflict = js_get(js, args[1], "onConflict");

    if (!is_undefined(apply.filter) && !is_callable(apply.filter))
      return sqlite_arg_type_error(js, "options.filter", "a function");
    if (!is_undefined(apply.conflict) && !is_callable(apply.conflict))
      return sqlite_arg_type_error(js, "options.onConflict", "a function");
  }

  if (len > INT32_MAX) return sqlite_code_error(js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE", "The changeset is too large.");

  int rc = SQ(changeset_apply)(
    db->db, (int)len, (void *)bytes,
    is_callable(apply.filter) ? sqlite_apply_filter : NULL,
    sqlite_apply_conflict, &apply
  );

  if (db->has_pending) return sqlite_take_error(db);
  if (rc == SQLITE_OK) return Ant_Value_True;
  if (rc == SQLITE_ABORT) return Ant_Value_False;
  return sqlite_take_error(db);
}

static void sqlite_vmodule_destroy(void *data) {
  sqlite_vmodule_t *module = data;
  sqlite_ref_free(module->db, module->rows_ref);
  free(module->schema);
  free(module->hidden);
  free(module);
}

static int sqlite_vtab_connect(sqlite3 *handle, void *data, int argc, const char *const *argv, sqlite3_vtab **out, char **error) {
  sqlite_vmodule_t *module = data;

  int rc = SQ(declare_vtab)(handle, module->schema);
  if (rc != SQLITE_OK) return rc;
  if (module->direct_only) SQ(vtab_config)(handle, SQLITE_VTAB_DIRECTONLY);

  sqlite_vtab_t *vtab = calloc(1, sizeof(*vtab));
  if (!vtab) return SQLITE_NOMEM;

  vtab->module = module;
  *out = &vtab->base;
  return SQLITE_OK;
}

static int sqlite_vtab_disconnect(sqlite3_vtab *vtab) {
  free(vtab);
  return SQLITE_OK;
}

static int sqlite_vtab_hidden_ordinal(const sqlite_vmodule_t *module, int column) {
  int ordinal = 0;
  for (int i = 0; i < column; i++) ordinal += module->hidden[i];
  return ordinal;
}

static int sqlite_vtab_best_index(sqlite3_vtab *base, sqlite3_index_info *info) {
  sqlite_vmodule_t *module = ((sqlite_vtab_t *)base)->module;
  int constraint_of[SQLITE_MAX_HIDDEN];
  for (int i = 0; i < SQLITE_MAX_HIDDEN; i++) constraint_of[i] = -1;

  for (int i = 0; i < info->nConstraint; i++) {
    const struct sqlite3_index_constraint *constraint = &info->aConstraint[i];
    if (constraint->iColumn < 0 || constraint->iColumn >= module->column_count) continue;
    if (!module->hidden[constraint->iColumn] || constraint->op != SQLITE_INDEX_CONSTRAINT_EQ) continue;
    if (!constraint->usable) return SQLITE_CONSTRAINT;

    int ordinal = sqlite_vtab_hidden_ordinal(module, constraint->iColumn);
    if (ordinal < SQLITE_MAX_HIDDEN && constraint_of[ordinal] < 0) constraint_of[ordinal] = i;
  }

  int argc = 0;
  int mask = 0;
  for (int ordinal = 0; ordinal < module->hidden_count && ordinal < SQLITE_MAX_HIDDEN; ordinal++) {
    if (constraint_of[ordinal] < 0) continue;
    info->aConstraintUsage[constraint_of[ordinal]].argvIndex = ++argc;
    info->aConstraintUsage[constraint_of[ordinal]].omit = 1;
    mask |= 1 << ordinal;
  }

  info->idxNum = mask;
  info->estimatedCost = 1000.0;
  info->estimatedRows = 1000;
  return SQLITE_OK;
}

static int sqlite_vtab_open(sqlite3_vtab *vtab, sqlite3_vtab_cursor **out) {
  sqlite_cursor_t *cursor = calloc(1, sizeof(*cursor));
  if (!cursor) return SQLITE_NOMEM;
  *out = &cursor->base;
  return SQLITE_OK;
}

static void sqlite_cursor_clear(sqlite_db_t *db, sqlite_cursor_t *cursor) {
  sqlite_ref_free(db, cursor->next_ref);
  sqlite_ref_free(db, cursor->row_ref);
  sqlite_ref_free(db, cursor->args_ref);
  cursor->next_ref = 0;
  cursor->row_ref = 0;
  cursor->args_ref = 0;
}

static int sqlite_vtab_close(sqlite3_vtab_cursor *base) {
  sqlite_cursor_t *cursor = (sqlite_cursor_t *)base;
  sqlite_cursor_clear(((sqlite_vtab_t *)base->pVtab)->module->db, cursor);
  free(cursor);
  return SQLITE_OK;
}

static int sqlite_vtab_next(sqlite3_vtab_cursor *base) {
  sqlite_cursor_t *cursor = (sqlite_cursor_t *)base;
  sqlite_db_t *db = ((sqlite_vtab_t *)base->pVtab)->module->db;

  ant_value_t row = sqlite_callback(db, sqlite_ref_get(db, cursor->next_ref), NULL, 0);
  if (is_err(row)) return SQLITE_ERROR;

  if (is_undefined(row)) {
    cursor->eof = true;
    return SQLITE_OK;
  }
  if (vtype(row) != kTypeArray) {
    sqlite_set_pending(db, sqlite_code_error(
      db->js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", "Virtual table rows must be arrays of column values"
    ));
    return SQLITE_ERROR;
  }

  if (cursor->row_ref) sqlite_ref_set(db, cursor->row_ref, row);
  else cursor->row_ref = sqlite_ref_alloc(db, row);

  cursor->rowid++;
  return SQLITE_OK;
}

static int sqlite_vtab_filter(sqlite3_vtab_cursor *base, int index, const char *index_str, int argc, sqlite3_value **argv) {
  sqlite_cursor_t *cursor = (sqlite_cursor_t *)base;
  sqlite_vmodule_t *module = ((sqlite_vtab_t *)base->pVtab)->module;
  sqlite_db_t *db = module->db;
  ant_t *js = db->js;

  sqlite_cursor_clear(db, cursor);
  cursor->eof = false;
  cursor->rowid = 0;

  ant_value_t values[SQLITE_MAX_HIDDEN];
  int count = module->hidden_count < SQLITE_MAX_HIDDEN ? module->hidden_count : SQLITE_MAX_HIDDEN;
  int used = 0;

  ant_value_t params = Ant_Array_Create(js);
  cursor->args_ref = sqlite_ref_alloc(db, params);

  for (int ordinal = 0; ordinal < count; ordinal++) {
    values[ordinal] = Ant_Value_Undefined;
    if ((index & (1 << ordinal)) && used < argc) values[ordinal] = sqlite_value(js, argv[used++], module->bigint);
    if (is_err(values[ordinal])) {
      sqlite_set_pending(db, values[ordinal]);
      return SQLITE_ERROR;
    }
    js_arr_push(js, params, values[ordinal]);
  }

  ant_value_t next = sqlite_callback(db, sqlite_ref_get(db, module->rows_ref), values, count);
  if (is_err(next)) return SQLITE_ERROR;
  if (!is_callable(next)) {
    sqlite_set_pending(db, sqlite_arg_type_error(js, "options.rows", "a function returning an iterable"));
    return SQLITE_ERROR;
  }

  cursor->next_ref = sqlite_ref_alloc(db, next);
  return sqlite_vtab_next(base);
}

static int sqlite_vtab_eof(sqlite3_vtab_cursor *base) {
  return ((sqlite_cursor_t *)base)->eof;
}

static int sqlite_vtab_column(sqlite3_vtab_cursor *base, sqlite3_context *ctx, int column) {
  sqlite_cursor_t *cursor = (sqlite_cursor_t *)base;
  sqlite_vmodule_t *module = ((sqlite_vtab_t *)base->pVtab)->module;
  sqlite_db_t *db = module->db;
  ant_t *js = db->js;

  ant_value_t row = sqlite_ref_get(db, cursor->row_ref);
  ant_value_t value = column < (int)js_arr_len(js, row) ? js_arr_get(js, row, column) : Ant_Value_Undefined;

  if (is_undefined(value) && module->hidden[column])
    value = js_arr_get(js, sqlite_ref_get(db, cursor->args_ref), sqlite_vtab_hidden_ordinal(module, column));

  sqlite_result(db, ctx, value);
  return db->has_pending ? SQLITE_ERROR : SQLITE_OK;
}

static int sqlite_vtab_rowid(sqlite3_vtab_cursor *base, sqlite3_int64 *rowid) {
  *rowid = ((sqlite_cursor_t *)base)->rowid;
  return SQLITE_OK;
}

static const sqlite3_module sqlite_vtab_module = {
  .iVersion = 0,
  .xCreate = sqlite_vtab_connect,
  .xConnect = sqlite_vtab_connect,
  .xBestIndex = sqlite_vtab_best_index,
  .xDisconnect = sqlite_vtab_disconnect,
  .xDestroy = sqlite_vtab_disconnect,
  .xOpen = sqlite_vtab_open,
  .xClose = sqlite_vtab_close,
  .xFilter = sqlite_vtab_filter,
  .xNext = sqlite_vtab_next,
  .xEof = sqlite_vtab_eof,
  .xColumn = sqlite_vtab_column,
  .xRowid = sqlite_vtab_rowid,
};

static ant_value_t sqlite_db_create_module(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, call_this, &err);
  if (!db) return err;
  if (nargs < 5) return sqlite_arg_type_error(js, "options", "an object");

  const char *name = sqlite_string_arg(js, args[0], "name", NULL, &err);
  if (!name) return err;
  const char *schema = sqlite_string_arg(js, args[1], "schema", NULL, &err);
  if (!schema) return err;

  ant_value_t hidden = args[2];
  ant_value_t rows = args[3];
  double flags = 0;
  if (vtype(hidden) != kTypeArray || !is_callable(rows) || !sqlite_is_integer(args[4], &flags))
    return sqlite_arg_type_error(js, "options", "an object");

  sqlite_vmodule_t *module = calloc(1, sizeof(*module));
  if (!module) return Ant_Error_CreateOutOfMemory(js);

  module->db = db;
  module->column_count = (int)js_arr_len(js, hidden);
  module->schema = strdup(schema);
  module->hidden = calloc((size_t)module->column_count + 1, 1);
  module->bigint = ((int)flags & 1) != 0;
  module->direct_only = ((int)flags & 2) != 0;

  if (!module->schema || !module->hidden) {
    free(module->schema);
    free(module->hidden);
    free(module);
    return Ant_Error_CreateOutOfMemory(js);
  }

  for (int i = 0; i < module->column_count; i++) {
    module->hidden[i] = js_arr_get(js, hidden, i) == Ant_Value_True;
    module->hidden_count += module->hidden[i];
  }

  module->rows_ref = sqlite_ref_alloc(db, rows);

  if (SQ(create_module_v2)(db->db, name, &sqlite_vtab_module, module, sqlite_vmodule_destroy) != SQLITE_OK)
    return sqlite_take_error(db);

  return Ant_Value_Undefined;
}

static sqlite_stmt_t *sqlite_stmt_state(ant_t *js, ant_value_t self, ant_value_t *err) {
  sqlite_stmt_t *st = is_object_type(self) ? js_get_native(self, SQLITE_STMT_NATIVE_TAG) : NULL;
  if (!st) {
    *err = sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_THIS", "Value of \"this\" must be of type Statement");
    return NULL;
  }
  if (!st->stmt || !st->db || !st->db->db) {
    *err = sqlite_state_error(js, "statement has been finalized");
    return NULL;
  }
  return st;
}

static sqlite_stmt_t *sqlite_stmt_idle_state(ant_t *js, ant_value_t self, ant_value_t *err) {
  sqlite_stmt_t *st = sqlite_stmt_state(js, self, err);
  if (!st) return NULL;

  if (st->db->authorizer_depth > 0) {
    *err = sqlite_state_error(js, "database cannot be used from within an authorizer callback");
    return NULL;
  }
  if (st->executing) {
    *err = sqlite_state_error(js, "statement is currently executing");
    return NULL;
  }
  return st;
}

static ant_value_t sqlite_bind_value(ant_t *js, sqlite3_stmt *stmt, int index, ant_value_t value) {
  int rc = SQLITE_OK;

  switch (vtype(value)) {
    case kTypeUndefined:
    case kTypeNull:
      rc = SQ(bind_null)(stmt, index);
      break;
    case kTypeBool:
      rc = SQ(bind_int64)(stmt, index, value == Ant_Value_True);
      break;
    case kTypeNumber:
      rc = SQ(bind_double)(stmt, index, Ant_Value_AsNumberUnchecked(value));
      break;
    case kTypeBigInt: {
      int64_t integer = 0;
      if (!bigint_to_int64_checked(js, value, &integer))
        return sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_ARG_VALUE", "BigInt value is too large to bind.");
      rc = SQ(bind_int64)(stmt, index, integer);
      break;
    }
    case kTypeString: {
      size_t len = 0;
      const char *str = js_getstr(js, value, &len);
      if (!str) return Ant_Error_CreateOutOfMemory(js);
      rc = SQ(bind_text64)(stmt, index, str, len, SQLITE_TRANSIENT, SQLITE_UTF8);
      break;
    }
    default: {
      const uint8_t *bytes = NULL;
      size_t len = 0;
      if (!is_object_type(value) || !buffer_is_binary_source(value) || !buffer_source_get_bytes(js, value, &bytes, &len))
        return sqlite_code_error(
          js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE",
          "Provided value cannot be bound to SQLite parameter %d.", index
        );
      rc = SQ(bind_blob64)(stmt, index, bytes ? (const void *)bytes : "", len, SQLITE_TRANSIENT);
      break;
    }
  }

  if (rc == SQLITE_OK) return Ant_Value_Undefined;
  return sqlite_result_error_value(js, rc, SQ(errstr)(rc));
}

static ant_value_t sqlite_named_index(ant_t *js, sqlite_stmt_t *st, const char *key, size_t key_len, int *out) {
  char stack[128];
  char *name = key_len + 1 <= sizeof(stack) ? stack : malloc(key_len + 1);
  if (!name) return Ant_Error_CreateOutOfMemory(js);

  memcpy(name, key, key_len);
  name[key_len] = '\0';

  *out = SQ(bind_parameter_index)(st->stmt, name);
  if (name != stack) free(name);
  if (*out != 0 || !(st->flags & SQLITE_OPT_BARE_NAMED)) return Ant_Value_Undefined;

  const char *match = NULL;
  int count = SQ(bind_parameter_count)(st->stmt);

  for (int i = 1; i <= count; i++) {
    const char *full = SQ(bind_parameter_name)(st->stmt, i);
    if (!full || strlen(full + 1) != key_len || memcmp(full + 1, key, key_len) != 0) continue;

    if (match && strcmp(match, full) != 0) return sqlite_code_error(
      js, JS_ERR_GENERIC, "ERR_INVALID_STATE",
      "Cannot create bare named parameter '%.*s' because of conflicting names '%s' and '%s'.",
      (int)key_len, key, match, full
    );
    if (!match) {
      match = full;
      *out = i;
    }
  }

  return Ant_Value_Undefined;
}

static ant_value_t sqlite_bind_named(ant_t *js, sqlite_stmt_t *st, ant_value_t params) {
  ant_iter_t iter = js_prop_iter_begin(js, params);
  ant_value_t result = Ant_Value_Undefined;
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = Ant_Value_Undefined;

  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    int index = 0;
    result = sqlite_named_index(js, st, key, key_len, &index);
    if (is_err(result)) break;

    if (index == 0) {
      if (st->flags & SQLITE_OPT_UNKNOWN_NAMED) continue;
      result = sqlite_code_error(js, JS_ERR_GENERIC, "ERR_INVALID_STATE", "Unknown named parameter '%.*s'", (int)key_len, key);
      break;
    }

    result = sqlite_bind_value(js, st->stmt, index, value);
    if (is_err(result)) break;
  }

  js_prop_iter_end(&iter);
  return result;
}

static ant_value_t sqlite_bind_args(ant_t *js, sqlite_stmt_t *st, ant_value_t *args, int nargs) {
  int first = 0;
  SQ(clear_bindings)(st->stmt);

  if (nargs > 0 && vtype(args[0]) == kTypeObject && !buffer_is_binary_source(args[0])) {
    ant_value_t named = sqlite_bind_named(js, st, args[0]);
    if (is_err(named)) return named;
    first = 1;
  }

  int position = 0;
  for (int i = first; i < nargs; i++) {
    const char *name = NULL;
    do name = SQ(bind_parameter_name)(st->stmt, ++position);
    while (name && name[0] != '?');
    ant_value_t bound = sqlite_bind_value(js, st->stmt, position, args[i]);
    if (is_err(bound)) return bound;
  }

  return Ant_Value_Undefined;
}

static ant_value_t sqlite_row(ant_t *js, sqlite_stmt_t *st) {
  bool arrays = (st->flags & SQLITE_OPT_RETURN_ARRAYS) != 0;
  bool bigint = (st->flags & SQLITE_OPT_READ_BIGINTS) != 0;

  ant_value_t row = arrays ? Ant_Array_Create(js) : Ant_Object_CreateBare(js);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, row);

  int count = SQ(column_count)(st->stmt);
  for (int i = 0; i < count; i++) {
    ant_value_t value = sqlite_column_value(js, st->stmt, i, bigint);
    if (is_err(value)) {
      GC_ROOT_RESTORE(js, mark);
      return value;
    }
    if (arrays) js_arr_push(js, row, value);
    else js_set(js, row, SQ(column_name)(st->stmt, i), value);
  }

  GC_ROOT_RESTORE(js, mark);
  return row;
}

static ant_value_t sqlite_changes(ant_t *js, sqlite_stmt_t *st) {
  sqlite3 *handle = st->db->db;
  bool bigint = (st->flags & SQLITE_OPT_READ_BIGINTS) != 0;
  sqlite3_int64 changes = SQ_HAS(changes64) ? SQ(changes64)(handle) : SQ(changes)(handle);

  ant_value_t result = Ant_Object_Create(js);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, result);

  js_set(js, result, "changes", bigint ? bigint_from_int64(js, changes) : Ant_Value_FromNumber((double)changes));
  js_set(js, result, "lastInsertRowid", sqlite_integer(js, SQ(last_insert_rowid)(handle), bigint));

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t sqlite_stmt_execute(ant_params_t, sqlite_read_mode_t mode) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_idle_state(js, call_this, &err);
  if (!st) return err;

  sqlite_db_t *db = st->db;
  sqlite3_stmt *stmt = st->stmt;

  st->generation++;
  SQ(reset)(stmt);

  ant_value_t bound = sqlite_bind_args(js, st, args, nargs);
  if (is_err(bound)) return bound;

  ant_value_t result = mode == SQLITE_READ_ALL ? Ant_Array_Create(js) : Ant_Value_Undefined;
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, result);

  st->executing = true;
  int rc = SQ(step)(stmt);

  while (rc == SQLITE_ROW && mode != SQLITE_READ_RUN) {
    ant_value_t row = sqlite_row(js, st);
    if (is_err(row) || mode == SQLITE_READ_GET) {
      result = row;
      break;
    }
    js_arr_push(js, result, row);
    rc = SQ(step)(stmt);
  }
  while (rc == SQLITE_ROW && mode == SQLITE_READ_RUN) rc = SQ(step)(stmt);

  if (!is_err(result) && rc != SQLITE_ROW && rc != SQLITE_DONE) result = sqlite_take_error(db);
  else if (!is_err(result) && mode == SQLITE_READ_RUN) result = sqlite_changes(js, st);

  SQ(reset)(stmt);
  st->executing = false;

  if (db->has_pending) result = sqlite_take_error(db);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t sqlite_stmt_run(ant_params_t) {
  return sqlite_stmt_execute(js, args, nargs, call_new_target, call_this, call_func, SQLITE_READ_RUN);
}

static ant_value_t sqlite_stmt_get(ant_params_t) {
  return sqlite_stmt_execute(js, args, nargs, call_new_target, call_this, call_func, SQLITE_READ_GET);
}

static ant_value_t sqlite_stmt_all(ant_params_t) {
  return sqlite_stmt_execute(js, args, nargs, call_new_target, call_this, call_func, SQLITE_READ_ALL);
}

static ant_value_t sqlite_stmt_begin(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_idle_state(js, call_this, &err);
  if (!st) return err;

  st->generation++;
  SQ(reset)(st->stmt);

  ant_value_t bound = sqlite_bind_args(js, st, args, nargs);
  return is_err(bound) ? bound : Ant_Value_FromNumber(st->generation);
}

static ant_value_t sqlite_stmt_step(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_idle_state(js, call_this, &err);
  if (!st) return err;

  if (nargs < 1 || vtype(args[0]) != kTypeNumber || Ant_Value_AsNumberUnchecked(args[0]) != (double)st->generation)
    return sqlite_state_error(js, "iterator was invalidated");

  sqlite_db_t *db = st->db;
  st->executing = true;
  int rc = SQ(step)(st->stmt);
  st->executing = false;

  if (rc == SQLITE_ROW) {
    ant_value_t row = sqlite_row(js, st);
    if (is_err(row)) SQ(reset)(st->stmt);
    return row;
  }

  ant_value_t result = rc == SQLITE_DONE ? Ant_Value_Undefined : sqlite_take_error(db);
  st->executing = true;
  SQ(reset)(st->stmt);
  st->executing = false;
  st->generation++;

  return db->has_pending ? sqlite_take_error(db) : result;
}

static ant_value_t sqlite_stmt_end(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_idle_state(js, call_this, &err);
  if (!st) return err;

  if (nargs > 0 && vtype(args[0]) == kTypeNumber && Ant_Value_AsNumberUnchecked(args[0]) == (double)st->generation) {
    SQ(reset)(st->stmt);
    st->generation++;
  }

  return Ant_Value_Undefined;
}

static ant_value_t sqlite_stmt_columns(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_state(js, call_this, &err);
  if (!st) return err;

  ant_value_t columns = Ant_Array_Create(js);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, columns);

  int count = SQ(column_count)(st->stmt);
  for (int i = 0; i < count; i++) {
    ant_value_t column = Ant_Object_CreateBare(js);
    js_arr_push(js, columns, column);

    js_set(js, column, "column", SQ_HAS(column_origin_name) ? sqlite_string(js, SQ(column_origin_name)(st->stmt, i)) : Ant_Value_Null);
    js_set(js, column, "database", SQ_HAS(column_database_name) ? sqlite_string(js, SQ(column_database_name)(st->stmt, i)) : Ant_Value_Null);
    js_set(js, column, "name", sqlite_string(js, SQ(column_name)(st->stmt, i)));
    js_set(js, column, "table", SQ_HAS(column_table_name) ? sqlite_string(js, SQ(column_table_name)(st->stmt, i)) : Ant_Value_Null);
    js_set(js, column, "type", sqlite_string(js, SQ(column_decltype)(st->stmt, i)));
  }

  GC_ROOT_RESTORE(js, mark);
  return columns;
}

static ant_value_t sqlite_stmt_source_sql(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_state(js, call_this, &err);
  if (!st) return err;

  const char *sql = SQ(sql)(st->stmt);
  return js_mkstr(js, sql ? sql : "", sql ? strlen(sql) : 0);
}

static ant_value_t sqlite_stmt_expanded_sql(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_state(js, call_this, &err);
  if (!st) return err;
  if (!SQ_HAS(expanded_sql)) return sqlite_unsupported(js, "expandedSQL");

  char *sql = SQ(expanded_sql)(st->stmt);
  if (!sql) return sqlite_code_error(js, JS_ERR_GENERIC, "ERR_SQLITE_ERROR", "Expanded SQL text would exceed configured limits");

  ant_value_t result = js_mkstr(js, sql, strlen(sql));
  SQ(free)(sql);
  return result;
}

static ant_value_t sqlite_stmt_set_flag(ant_params_t, const char *name, uint32_t bit) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_state(js, call_this, &err);
  if (!st) return err;

  ant_value_t enabled = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(enabled) != kTypeBool) return sqlite_arg_type_error(js, name, "a boolean");

  st->flags = enabled == Ant_Value_True ? (st->flags | bit) : (st->flags & ~bit);
  return Ant_Value_Undefined;
}

static ant_value_t sqlite_stmt_set_read_bigints(ant_params_t) {
  return sqlite_stmt_set_flag(js, args, nargs, call_new_target, call_this, call_func, "readBigInts", SQLITE_OPT_READ_BIGINTS);
}

static ant_value_t sqlite_stmt_set_return_arrays(ant_params_t) {
  return sqlite_stmt_set_flag(js, args, nargs, call_new_target, call_this, call_func, "returnArrays", SQLITE_OPT_RETURN_ARRAYS);
}

static ant_value_t sqlite_stmt_set_bare_named(ant_params_t) {
  return sqlite_stmt_set_flag(js, args, nargs, call_new_target, call_this, call_func, "allowBareNamedParameters", SQLITE_OPT_BARE_NAMED);
}

static ant_value_t sqlite_stmt_set_unknown_named(ant_params_t) {
  return sqlite_stmt_set_flag(js, args, nargs, call_new_target, call_this, call_func, "enabled", SQLITE_OPT_UNKNOWN_NAMED);
}

static ant_value_t sqlite_stmt_stat(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_state(js, call_this, &err);
  if (!st) return err;

  const char *counter = sqlite_string_arg(js, nargs > 0 ? args[0] : Ant_Value_Undefined, "counter", NULL, &err);
  if (!counter) return err;

  for (size_t i = 0; i < sizeof(sqlite_stat_counters) / sizeof(sqlite_stat_counters[0]); i++)
    if (strcmp(counter, sqlite_stat_counters[i].name) == 0)
      return Ant_Value_FromNumber(SQ(stmt_status)(st->stmt, sqlite_stat_counters[i].op, 0));

  return sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_ARG_VALUE", "The \"counter\" argument is not a valid statistic name.");
}

static ant_value_t sqlite_stmt_reset_stats(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_state(js, call_this, &err);
  if (!st) return err;

  for (size_t i = 0; i < sizeof(sqlite_stat_counters) / sizeof(sqlite_stat_counters[0]); i++)
    if (sqlite_stat_counters[i].op != SQLITE_STMTSTATUS_MEMUSED)
      SQ(stmt_status)(st->stmt, sqlite_stat_counters[i].op, 1);

  return Ant_Value_Undefined;
}

static ant_value_t sqlite_stmt_close(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_stmt_t *st = sqlite_stmt_state(js, call_this, &err);
  if (!st) return err;
  if (st->executing) return sqlite_state_error(js, "statement is currently executing");

  sqlite_stmt_release(st);
  return Ant_Value_Undefined;
}

static ant_value_t sqlite_stmt_is_finalized(ant_params_t) {
  sqlite_stmt_t *st = is_object_type(call_this) ? js_get_native(call_this, SQLITE_STMT_NATIVE_TAG) : NULL;
  if (!st) return sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_THIS", "Value of \"this\" must be of type Statement");
  return Ant_Value_FromBool(!st->stmt || !st->db || !st->db->db);
}

static sqlite_session_t *sqlite_session_state(ant_t *js, ant_value_t self, ant_value_t *err) {
  sqlite_session_t *session = is_object_type(self) ? js_get_native(self, SQLITE_SESSION_NATIVE_TAG) : NULL;
  if (!session) {
    *err = sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_THIS", "Value of \"this\" must be of type Session");
    return NULL;
  }
  if (!session->db || !session->db->db) {
    *err = sqlite_state_error(js, "database is not open");
    return NULL;
  }
  if (!session->session) {
    *err = sqlite_state_error(js, "session is not open");
    return NULL;
  }
  return session;
}

static ant_value_t sqlite_session_generate(ant_params_t, bool patchset) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_session_t *session = sqlite_session_state(js, call_this, &err);
  if (!session) return err;

  int size = 0;
  void *data = NULL;

  session->busy = true;
  int rc = patchset
    ? SQ(session_patchset)(session->session, &size, &data)
    : SQ(session_changeset)(session->session, &size, &data);
  session->busy = false;

  if (rc != SQLITE_OK) return sqlite_take_error(session->db);

  ant_value_t bytes = sqlite_bytes(js, data, (size_t)size);
  SQ(free)(data);
  return bytes;
}

static ant_value_t sqlite_session_changeset(ant_params_t) {
  return sqlite_session_generate(js, args, nargs, call_new_target, call_this, call_func, false);
}

static ant_value_t sqlite_session_patchset(ant_params_t) {
  return sqlite_session_generate(js, args, nargs, call_new_target, call_this, call_func, true);
}

static ant_value_t sqlite_session_close(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_session_t *session = sqlite_session_state(js, call_this, &err);
  if (!session) return err;

  if (session->busy) return sqlite_state_error(js, "session is currently generating a changeset");
  if (session->db->callback_depth > 0) return sqlite_state_error(js, "session cannot be closed while a statement is executing");

  sqlite_session_release(session);
  return Ant_Value_Undefined;
}

static ant_value_t sqlite_session_is_closed(ant_params_t) {
  sqlite_session_t *session = is_object_type(call_this) ? js_get_native(call_this, SQLITE_SESSION_NATIVE_TAG) : NULL;
  if (!session) return sqlite_code_error(js, JS_ERR_TYPE, "ERR_INVALID_THIS", "Value of \"this\" must be of type Session");
  return Ant_Value_FromBool(!session->session || !session->db || !session->db->db);
}

static ant_value_t sqlite_illegal_ctor(ant_params_t) {
  return sqlite_code_error(js, JS_ERR_GENERIC, "ERR_ILLEGAL_CONSTRUCTOR", "Illegal constructor");
}

static ant_value_t sqlite_backup_init(ant_params_t) {
  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_idle_state(js, nargs > 0 ? args[0] : Ant_Value_Undefined, &err);
  if (!db) return err;
  if (nargs < 4) return sqlite_arg_type_error(js, "path", "a string");

  const char *path = sqlite_string_arg(js, args[1], "path", NULL, &err);
  if (!path) return err;
  const char *source = sqlite_string_arg(js, args[2], "options.source", NULL, &err);
  if (!source) return err;
  const char *target = sqlite_string_arg(js, args[3], "options.target", NULL, &err);
  if (!target) return err;

  sqlite_backup_t *backup = calloc(1, sizeof(*backup));
  if (!backup) return Ant_Error_CreateOutOfMemory(js);

  if (SQ(open_v2)(path, &backup->dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) == SQLITE_OK)
    backup->backup = SQ(backup_init)(backup->dest, target, db->db, source);

  if (!backup->backup) {
    err = sqlite_handle_error(js, backup->dest);
    sqlite_backup_release(backup);
    free(backup);
    return err;
  }

  ant_value_t obj = Ant_Object_Create(js);
  js_set_native(obj, backup, SQLITE_BACKUP_NATIVE_TAG);
  js_set_finalizer(obj, sqlite_backup_finalize);
  js_set_slot_wb(js, obj, SLOT_DATA, args[0]);

  return obj;
}

static ant_value_t sqlite_backup_step(ant_params_t) {
  sqlite_backup_t *backup = nargs > 0 && is_object_type(args[0]) ? js_get_native(args[0], SQLITE_BACKUP_NATIVE_TAG) : NULL;
  if (!backup || !backup->backup) return sqlite_state_error(js, "backup is not active");

  ant_value_t err = Ant_Value_Undefined;
  sqlite_db_t *db = sqlite_db_open_state(js, js_get_slot(args[0], SLOT_DATA), &err);
  if (!db) {
    sqlite_backup_release(backup);
    return err;
  }

  double rate = 100;
  if (nargs > 1 && !sqlite_is_integer(args[1], &rate)) return sqlite_arg_type_error(js, "options.rate", "an integer");

  int rc = SQ(backup_step)(backup->backup, (int)rate);
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
    err = sqlite_result_error_value(js, rc, SQ(errmsg)(backup->dest));
    sqlite_backup_release(backup);
    return err;
  }

  ant_value_t result = Ant_Object_Create(js);
  js_set(js, result, "remainingPages", Ant_Value_FromNumber(SQ(backup_remaining)(backup->backup)));
  js_set(js, result, "totalPages", Ant_Value_FromNumber(SQ(backup_pagecount)(backup->backup)));
  js_set(js, result, "done", Ant_Value_FromBool(rc == SQLITE_DONE));

  if (rc == SQLITE_DONE) sqlite_backup_release(backup);
  return result;
}

static ant_value_t sqlite_backup_abort(ant_params_t) {
  sqlite_backup_t *backup = nargs > 0 && is_object_type(args[0]) ? js_get_native(args[0], SQLITE_BACKUP_NATIVE_TAG) : NULL;
  if (backup) sqlite_backup_release(backup);
  return Ant_Value_Undefined;
}

static ant_value_t sqlite_set_query_hook(ant_params_t) {
  ant_value_t hook = nargs > 0 ? args[0] : Ant_Value_Undefined;
  js->builtins.sqlite_query_hook = is_callable(hook) ? hook : Ant_Value_Undefined;

  for (sqlite_db_t *db = sqlite_module_state(js)->open; db; db = db->next) sqlite_sync_trace(db);
  return Ant_Value_Undefined;
}

static ant_value_t sqlite_set_path_hook(ant_params_t) {
  ant_value_t hook = nargs > 0 ? args[0] : Ant_Value_Undefined;
  js->builtins.sqlite_path_hook = is_callable(hook) ? hook : Ant_Value_Undefined;
  return Ant_Value_Undefined;
}

ant_value_t sqlite_library(ant_t *js) {
  if (!sqlite_api_available())
    return sqlite_code_error(js, JS_ERR_GENERIC, "ERR_SQLITE_ERROR", "the system SQLite library could not be loaded");
  if (SQ(initialize)() != SQLITE_OK) return sqlite_handle_error(js, NULL);

  sqlite_module_state_t *module = calloc(1, sizeof(*module));
  if (!module) return Ant_Error_CreateOutOfMemory(js);

  ant_value_t db_proto = Ant_Object_CreateBare(js);
  js->builtins.sqlite_database_proto = db_proto;
  js_set_proto_init(db_proto, js->sym.object_proto);
  js_set_native(db_proto, module, SQLITE_MODULE_NATIVE_TAG);
  js_set_finalizer(db_proto, sqlite_module_finalize);

  js_set(js, db_proto, "open", Ant_Function_CreateWithArity(sqlite_db_open, 0));
  js_set(js, db_proto, "close", Ant_Function_CreateWithArity(sqlite_db_close, 0));
  js_set(js, db_proto, "prepare", Ant_Function_CreateWithArity(sqlite_db_prepare, 1));
  js_set(js, db_proto, "exec", Ant_Function_CreateWithArity(sqlite_db_exec, 1));
  js_set(js, db_proto, "function", Ant_Function_CreateWithArity(sqlite_db_function, 2));
  js_set(js, db_proto, "location", Ant_Function_CreateWithArity(sqlite_db_location, 0));
  js_set(js, db_proto, "aggregate", Ant_Function_CreateWithArity(sqlite_db_aggregate, 2));
  js_set(js, db_proto, "createSession", Ant_Function_CreateWithArity(sqlite_db_create_session, 0));
  js_set(js, db_proto, "applyChangeset", Ant_Function_CreateWithArity(sqlite_db_apply_changeset, 1));
  js_set(js, db_proto, "enableLoadExtension", Ant_Function_CreateWithArity(sqlite_db_enable_load_extension, 1));
  js_set(js, db_proto, "enableDefensive", Ant_Function_CreateWithArity(sqlite_db_enable_defensive, 1));
  js_set(js, db_proto, "loadExtension", Ant_Function_CreateWithArity(sqlite_db_load_extension, 1));
  js_set(js, db_proto, "serialize", Ant_Function_CreateWithArity(sqlite_db_serialize, 0));
  js_set(js, db_proto, "deserialize", Ant_Function_CreateWithArity(sqlite_db_deserialize, 1));
  js_set(js, db_proto, "setAuthorizer", Ant_Function_CreateWithArity(sqlite_db_set_authorizer, 1));
  js_set_getter_desc(js, db_proto, "isOpen", 6, Ant_Function_Create(sqlite_db_is_open), JS_DESC_C);
  js_set_getter_desc(js, db_proto, "isTransaction", 13, Ant_Function_Create(sqlite_db_is_transaction), JS_DESC_C);

  ant_value_t stmt_proto = Ant_Object_CreateBare(js);
  js->builtins.sqlite_statement_proto = stmt_proto;
  js_set_proto_init(stmt_proto, js->sym.object_proto);

  js_set(js, stmt_proto, "all", Ant_Function_CreateWithArity(sqlite_stmt_all, 0));
  js_set(js, stmt_proto, "get", Ant_Function_CreateWithArity(sqlite_stmt_get, 0));
  js_set(js, stmt_proto, "run", Ant_Function_CreateWithArity(sqlite_stmt_run, 0));
  js_set(js, stmt_proto, "columns", Ant_Function_CreateWithArity(sqlite_stmt_columns, 0));
  js_set(js, stmt_proto, "stat", Ant_Function_CreateWithArity(sqlite_stmt_stat, 1));
  js_set(js, stmt_proto, "resetStats", Ant_Function_CreateWithArity(sqlite_stmt_reset_stats, 0));
  js_set(js, stmt_proto, "setAllowBareNamedParameters", Ant_Function_CreateWithArity(sqlite_stmt_set_bare_named, 1));
  js_set(js, stmt_proto, "setAllowUnknownNamedParameters", Ant_Function_CreateWithArity(sqlite_stmt_set_unknown_named, 1));
  js_set(js, stmt_proto, "setReadBigInts", Ant_Function_CreateWithArity(sqlite_stmt_set_read_bigints, 1));
  js_set(js, stmt_proto, "setReturnArrays", Ant_Function_CreateWithArity(sqlite_stmt_set_return_arrays, 1));
  js_set(js, stmt_proto, "close", Ant_Function_CreateWithArity(sqlite_stmt_close, 0));
  js_set_getter_desc(js, stmt_proto, "sourceSQL", 9, Ant_Function_Create(sqlite_stmt_source_sql), JS_DESC_C);
  js_set_getter_desc(js, stmt_proto, "expandedSQL", 11, Ant_Function_Create(sqlite_stmt_expanded_sql), JS_DESC_C);

  ant_value_t session_proto = Ant_Object_CreateBare(js);
  js->builtins.sqlite_session_proto = session_proto;
  js_set_proto_init(session_proto, js->sym.object_proto);

  js_set(js, session_proto, "changeset", Ant_Function_CreateWithArity(sqlite_session_changeset, 0));
  js_set(js, session_proto, "patchset", Ant_Function_CreateWithArity(sqlite_session_patchset, 0));
  js_set(js, session_proto, "close", Ant_Function_CreateWithArity(sqlite_session_close, 0));

  ant_value_t internals = Ant_Object_CreateBare(js);
  js_set(js, internals, "limit", Ant_Function_CreateWithArity(sqlite_db_limit, 2));
  js_set(js, internals, "createModule", Ant_Function_CreateWithArity(sqlite_db_create_module, 5));
  js_set(js, internals, "begin", Ant_Function_CreateWithArity(sqlite_stmt_begin, 0));
  js_set(js, internals, "step", Ant_Function_CreateWithArity(sqlite_stmt_step, 1));
  js_set(js, internals, "end", Ant_Function_CreateWithArity(sqlite_stmt_end, 1));
  js_set(js, internals, "isFinalized", Ant_Function_CreateWithArity(sqlite_stmt_is_finalized, 0));
  js_set(js, internals, "isClosed", Ant_Function_CreateWithArity(sqlite_session_is_closed, 0));
  js_set(js, internals, "backupInit", Ant_Function_CreateWithArity(sqlite_backup_init, 4));
  js_set(js, internals, "backupStep", Ant_Function_CreateWithArity(sqlite_backup_step, 2));
  js_set(js, internals, "backupAbort", Ant_Function_CreateWithArity(sqlite_backup_abort, 1));
  js_set(js, internals, "setQueryHook", Ant_Function_CreateWithArity(sqlite_set_query_hook, 1));
  js_set(js, internals, "setPathHook", Ant_Function_CreateWithArity(sqlite_set_path_hook, 1));

  ant_value_t lib = Ant_Object_CreateBare(js);
  js_set(js, lib, "Database", Ant_Function_CreateConstructor(js, sqlite_db_ctor, db_proto, "Database", 1));
  js_set(js, lib, "Statement", Ant_Function_CreateConstructor(js, sqlite_illegal_ctor, stmt_proto, "Statement", 0));
  js_set(js, lib, "Session", Ant_Function_CreateConstructor(js, sqlite_illegal_ctor, session_proto, "Session", 0));
  js_set(js, lib, "internals", internals);

  return lib;
}

#endif
