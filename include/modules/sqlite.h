#ifndef ANT_SQLITE_MODULE_H
#define ANT_SQLITE_MODULE_H

#include "types.h"

typedef struct sqlite_db_handle sqlite_db_handle_t;
typedef struct sqlite_stmt_handle sqlite_stmt_handle_t;

ant_value_t sqlite_library(ant_t *js);
void cleanup_sqlite_module(void);

#endif
