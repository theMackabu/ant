#ifndef ANT_LMDB_MODULE_H
#define ANT_LMDB_MODULE_H

#include "gc.h"
#include "types.h"

typedef struct lmdb_env_handle lmdb_env_handle_t;
typedef struct lmdb_db_handle lmdb_db_handle_t;
typedef struct lmdb_txn_handle lmdb_txn_handle_t;

typedef struct lmdb_env_ref lmdb_env_ref_t;
typedef struct lmdb_db_ref lmdb_db_ref_t;
typedef struct lmdb_txn_ref lmdb_txn_ref_t;

typedef struct {
  jsval_t env_ctor;
  jsval_t db_ctor;
  jsval_t txn_ctor;
  jsval_t env_proto;
  jsval_t db_proto;
  jsval_t txn_proto;
  bool ready;
} lmdb_js_types_t;

jsval_t lmdb_library(struct js *js);
void lmdb_gc_update_roots(GC_OP_VAL_ARGS);
void cleanup_lmdb_module(void);

#endif
