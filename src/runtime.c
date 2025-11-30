#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"

static struct ant_runtime runtime = {0};

struct ant_runtime *ant_runtime(void) {
  if (runtime.js == NULL) {
    fprintf(stderr, "Ant runtime not initialized!\n");
    abort();
  }
  return &runtime;
}

struct ant_runtime *ant_runtime_init(struct js *js) {
  runtime.js = js;
  runtime.ant_obj = js_mkobj(js);
  runtime.crypto_initialized = 0;
  
  js_set(js, js_glob(js), "Ant", runtime.ant_obj);
  return &runtime;
}