#include <string.h>

#include "ant.h"
#include "snapshot.h"
#include "runtime.h"
#include "internal.h"
#include "snapshot_data.h"
#include "gc/objects.h"

ant_value_t ant_load_snapshot(ant_t *js) {
  if (!js) return js_mkundef();
  
  const char *src = (const char *)ant_snapshot_source;
  ant_value_t result = js_eval_bytecode(js, src, ant_snapshot_source_len);
  
  gc_pin_existing_objects(js);
  builtin_object_freeze(js, &rt->ant_obj, 1);
  
  return vtype(result) == T_ERR ? result : js_true;
}

const uint8_t *ant_get_snapshot_source(size_t *len) {
  if (len) *len = ant_snapshot_source_len;
  return ant_snapshot_source;
}
