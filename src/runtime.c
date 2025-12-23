#include <stdio.h>
#include <stdlib.h>
#include <runtime.h>

static struct ant_runtime runtime = {0};

struct ant_runtime *const rt = &runtime;

struct ant_runtime *ant_runtime_init(struct js *js, int argc, char **argv, struct arg_file *ls_p) {
  runtime.js = js;
  runtime.ant_obj = js_mkobj(js);
  runtime.flags = 0;
  
  runtime.argc = argc;
  runtime.argv = argv;
  runtime.ls_fp = ls_p->count > 0 ? ls_p->filename[0] : NULL;
  
  js_set(js, js_glob(js), "Ant", runtime.ant_obj);
  js_set(js, js_glob(js), "global", js_glob(js));
  
  return &runtime;
}