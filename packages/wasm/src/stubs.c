#include <stdint.h>
#include <stdlib.h>

#include "errors.h"
#include "esm/exports.h"
#include "esm/loader.h"
#include "modules/process.h"
#include "modules/vm.h"

void js_esm_cleanup_module_cache(ant_t *js) {}
void esm_predeclare_exports(ant_t *js, sv_ast_t *program, ant_value_t ns) {}
void emit_process_event(ant_t *js, const char *event_type, ant_value_t *args, int nargs) {}
void process_exit_with(ant_t *js, int status) { exit(status); }

bool process_has_event_listeners(ant_t *js, const char *event_type) { return false; }
bool process_report_uncaught_exception(ant_t *js) { return false; }
bool process_report_unhandled_rejection(ant_t *js, ant_value_t promise, ant_value_t reason) { return false; }

int process_run_exit_handlers(ant_t *js, int status) { return status; }
uint64_t ant_worker_thread_id(ant_t *js) { return 0; }

ant_value_t js_esm_import_sync(ant_t *js, ant_value_t specifier) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "module imports are not available in @antjs.org/wasm");
}

ant_value_t js_esm_import_sync_typed(ant_t *js, ant_value_t specifier, const char *type, size_t type_len) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "module imports are not available in @antjs.org/wasm");
}

ant_value_t js_esm_import_dynamic_ex(
  ant_t *js, ant_value_t specifier, const char *base_path, ant_value_t attrs, ant_value_t *out_tla_promise
) {
  *out_tla_promise = Ant_Value_Undefined;
  return js_mkerr_typed(js, JS_ERR_TYPE, "module imports are not available in @antjs.org/wasm" );
}

ant_value_t js_vm_dynamic_import(
  ant_t *js, sv_code_unit_t *unit, ant_value_t specifier, ant_value_t options
) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "module imports are not available in @antjs.org/wasm");
}
