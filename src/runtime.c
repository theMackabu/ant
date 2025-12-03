#ifndef ANT_SNAPSHOT_GENERATOR
#include "snapshot.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <runtime.h>

static struct ant_runtime runtime = {0};

struct ant_runtime *const rt = &runtime;

struct ant_runtime *ant_runtime_init(struct js *js) {
  runtime.js = js;
  runtime.ant_obj = js_mkobj(js);
  runtime.crypto_initialized = 0;
  runtime.external_event_loop_active = 0;
  
  js_set(js, js_glob(js), "Ant", runtime.ant_obj);
  
#ifndef ANT_SNAPSHOT_GENERATOR
  jsval_t snapshot_result = ant_load_snapshot(js);
  if (js_type(snapshot_result) == JS_ERR) {
    fprintf(stderr, "Warning: Failed to load snapshot: %s\n", js_str(js, snapshot_result));
  }
#endif

  return &runtime;
}