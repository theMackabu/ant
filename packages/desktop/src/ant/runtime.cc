#include "desktop.hh"
#include <vector>

namespace desktop {

static std::string ErrorDetail(ant_t *js, ant_value_t error) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, error);
  ant_value_t detail = Ant_Exception_Stack(js, error);
  if (vtype(detail) != kTypeString) detail = Ant_Exception_Value(js, error);
  ant_value_t message;
  if (is_object_type(detail) && js_try_get_own_data_prop(js, detail, "message", 7, &message)) detail = message;
  std::string text = js_str(js, detail);
  GC_ROOT_RESTORE(js, mark);
  return text;
}

static const char kImportSource[] = "(url, fail) => import(url).catch(fail)";
static bool entry_failed;

static ant_value_t ReportEntryFailure(ant_params_t) {
  ant_value_t error = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t stack = is_object_type(error) ? js_get(js, error, "stack") : Ant_Value_Undefined;
  fprintf(stderr, "desktop entry failed: %s\n", js_str(js, vtype(stack) == kTypeString ? stack : error));
  entry_failed = true;
  Quit();
  return Ant_Value_Undefined;
}

bool ImportEntry(ant_t *js, const std::string &entry) {
  char *resolved = realpath(entry.c_str(), nullptr);
  if (!resolved) {
    fprintf(stderr, "desktop entry does not exist: %s\n", entry.c_str());
    return false;
  }
  ant_value_t args[] = {js_esm_make_file_url(js, resolved), Ant_Function_Create(ReportEntryFailure)};
  free(resolved);

  ant_value_t import = js_eval_bytecode_eval(js, kImportSource, sizeof(kImportSource) - 1, true);
  ant_value_t started = is_err(import) ? import : sv_vm_call_explicit_this(js->vm, js, import, Ant_Value_Undefined, args, 2);
  if (!is_err(started)) return true;
  fprintf(stderr, "desktop entry import failed (%s): %s\n", entry.c_str(), ErrorDetail(js, started).c_str());
  return false;
}

ant_t *StartRuntime(int argc, char **argv, void *stack_base) {
  ant_t *js = Ant_Create(nullptr);
  if (!js) return nullptr;
  js_setstackbase(js, stack_base);
  js_setstacklimit(js, os_thread_stack_size() * 3 / 4);
  ant_runtime_init(js, argc, argv, nullptr);
  ant_bootstrap_modules(js);
  return js;
}

void StopRuntime(ant_t *js) {
  js->process_exiting = true;
  Ant_Destroy(js);
}

int64_t PumpRuntime(ant_t *js) {
  js_reactor_pump_repl_nowait(js);
  return uv_backend_timeout(js->loop);
}

int RuntimeDescriptor(ant_t *js) {
  return uv_backend_fd(js->loop);
}

bool EntryFailed() {
  return entry_failed;
}

}
