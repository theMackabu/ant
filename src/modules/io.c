#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <limits.h>

#include "modules/io.h"

jsval_t js_println(struct js *js, jsval_t *args, int nargs) {
  for (int i = 0; i < nargs; i++) {
    const char *space = i == 0 ? "" : " ";
    
    if (js_type(args[i]) == JS_STR) {
      char *str = js_getstr(js, args[i], NULL);
      printf("%s%s", space, str);
    } else {
      printf("%s%s", space, js_str(js, args[i]));
    }
  }
  
  putchar('\n');
  return js_mkundef();
}