#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <limits.h>

#include "runtime.h"
#include "modules/io.h"

#define ANSI_RED "\x1b[31m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_RESET "\x1b[0m"

static void console_print(struct js *js, jsval_t *args, int nargs, const char *color, FILE *stream) {
  if (color) fprintf(stream, "%s", color);
  
  for (int i = 0; i < nargs; i++) {
    const char *space = i == 0 ? "" : " ";
    
    if (js_type(args[i]) == JS_STR) {
      char *str = js_getstr(js, args[i], NULL);
      fprintf(stream, "%s%s", space, str);
    } else {
      fprintf(stream, "%s%s", space, js_str(js, args[i]));
    }
  }
  
  if (color) fprintf(stream, "%s", ANSI_RESET);
  fputc('\n', stream);
}

static jsval_t js_console_log(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, NULL, stdout);
  return js_mkundef();
}

static jsval_t js_console_error(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, ANSI_RED, stderr);
  return js_mkundef();
}

static jsval_t js_console_warn(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, ANSI_YELLOW, stderr);
  return js_mkundef();
}

void init_console_module() {
  struct js *js = rt->js;
  jsval_t console_obj = js_mkobj(js);
  
  js_set(js, js_glob(js), "console", console_obj);
  js_set(js, console_obj, "log", js_mkfun(js_console_log));
  js_set(js, console_obj, "error", js_mkfun(js_console_error));
  js_set(js, console_obj, "warn", js_mkfun(js_console_warn));
}