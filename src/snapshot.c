#include <string.h>

#include "ant.h"
#include "snapshot.h"
#include "snapshot_data.h"

jsval_t ant_load_snapshot(struct js *js) {
  if (!js) return js_mkerr(js, "invalid js runtime");
  jsval_t result = js_eval(js, ant_snapshot_source, ant_snapshot_source_len);
  
  if (js_type(result) == JS_ERR) return result;
  return js_mktrue();
}

const char *ant_get_snapshot_source(size_t *len) {
  if (len) *len = ant_snapshot_source_len;
  return ant_snapshot_source;
}
