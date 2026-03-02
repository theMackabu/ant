#include <string.h>

#include "ant.h"
#include "snapshot.h"
#include "internal.h"
#include "snapshot_data.h"

jsval_t ant_load_snapshot(ant_t *js) {
  if (!js) return js_mkundef();
  
  const char *src = (const char *)ant_snapshot_source;
  size_t len = ant_snapshot_source_len;
  
  jsval_t result = js_eval_bytecode(js, src, len);
  return vtype(result) == T_ERR ? result : js_true;
}

const uint8_t *ant_get_snapshot_source(size_t *len) {
  if (len) *len = ant_snapshot_source_len;
  return ant_snapshot_source;
}
