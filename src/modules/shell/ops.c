#include <compat.h> // IWYU pragma: keep

#include "ant.h"
#include "debug.h"
#include "gc/roots.h"
#include "modules/symbol.h"
#include "shell_internal.h"

static ant_value_t shell_ops_context(ant_t *js, ant_value_t *args, int nargs) {
  return sh_runtime_context(js, nargs > 0 && js_truthy(js, args[0]));
}

ant_value_t shell_ops_library(ant_t *js) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t lib = js_mkobj(js);
  GC_ROOT_PIN(js, lib);

  js_set(js, lib, "begin", js_mkfun(sh_runtime_begin));
  js_set(js, lib, "arg", js_mkfun(sh_runtime_arg));
  js_set(js, lib, "command", js_mkfun(sh_runtime_command));
  js_set(js, lib, "redirect", js_mkfun(sh_runtime_redirect));
  js_set(js, lib, "submit", js_mkfun(sh_runtime_submit));
  js_set(js, lib, "finish", js_mkfun(sh_runtime_finish));
  js_set(js, lib, "context", js_mkfun(shell_ops_context));
  js_set(js, lib, "debugEnabled", sv_dump_shell_unlikely ? js_true : js_false);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "shell ops", 9));

  GC_ROOT_RESTORE(js, root_mark);
  return lib;
}
