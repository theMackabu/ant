#include "desktop.hh"

#include <vector>

namespace desktop {

static void *stack_base;
static ant_t *runtime;

static ant_t *Runtime() {
  if (runtime) return runtime;
  ant_t *js = Ant_Create(nullptr);
  if (!js) return nullptr;
  js_setstackbase(js, stack_base);
  js_setstacklimit(js, os_thread_stack_size() * 3 / 4);
  static char name[] = "ant-desktop-renderer";
  static char *argv[] = {name, nullptr};
  ant_runtime_init(js, 1, argv, nullptr);
  ant_bootstrap_modules(js);
  return runtime = js;
}

static std::string Respond(ant_t *js, ant_value_t value) {
  GC_ROOT_SAVE(mark, js);
  bool ok = !is_err(value);
  if (!ok) {
    value = js_take_thrown(js, value);
    ant_value_t message;
    if (js_try_get_own_data_prop(js, value, "message", 7, &message) && vtype(message) == kTypeString) value = message;
    else if (vtype(value) != kTypeString) value = Ant_String_FromLiteral(js, "unknown error");
  }
  GC_ROOT_PIN(js, value);
  ant_value_t response = Ant_Object_Create(js);
  GC_ROOT_PIN(js, response);
  js_set(js, response, "ok", Ant_Value_FromBool(ok));
  js_set(js, response, ok ? "value" : "error", value);
  ant_value_t json = json_stringify_value(js, response);
  std::string text = "{\"ok\":false,\"error\":\"result could not be serialized\"}";
  if (vtype(json) == kTypeString) {
    size_t length = 0;
    const char *bytes = js_getstr(js, json, &length);
    text.assign(bytes, length);
  } else if (is_err(json)) {
    js_take_thrown(js, json);
  }
  GC_ROOT_RESTORE(js, mark);
  return text;
}

static ant_value_t Import(ant_t *js, const std::string &specifier) {
  if (!specifier.starts_with("node:") && !specifier.starts_with("ant:")) {
    return js_mkerr(js, "renderer integration only accepts node: and ant: modules");
  }
  return js_esm_import_sync_cstr(js, specifier.data(), specifier.size());
}

static bool Primitive(ant_value_t value) {
  int type = vtype(value);
  return type == kTypeUndefined || type == kTypeNull || type == kTypeBool || type == kTypeNumber || type == kTypeString;
}

void SetIntegrationStackBase(void *base) {
  stack_base = base;
}

void ShutdownIntegration() {
  if (runtime) Ant_Destroy(runtime);
  runtime = nullptr;
}

std::string DescribeModule(const std::string &specifier) {
  ant_t *js = Runtime();
  if (!js) return {};
  GC_ROOT_SAVE(mark, js);
  ant_value_t module = Import(js, specifier);
  GC_ROOT_PIN(js, module);
  ant_value_t exports = is_err(module) ? module : js_own_property_keys(js, module, false, true);
  GC_ROOT_PIN(js, exports);
  ant_value_t result = is_err(exports) ? exports : Ant_Array_Create(js);
  GC_ROOT_PIN(js, result);
  ant_offset_t count = is_err(exports) ? 0 : js_arr_len(js, exports);
  for (ant_offset_t index = 0; index < count; index++) {
    ant_value_t key = js_arr_get(js, exports, index);
    if (vtype(key) != kTypeString) continue;
    size_t length = 0;
    std::string name = js_getstr(js, key, &length);
    ant_value_t value = js_get(js, module, name.c_str());
    if (is_err(value)) {
      result = value;
      break;
    }
    ant_value_t entry = Ant_Object_Create(js);
    js_arr_push(js, result, entry);
    js_set(js, entry, "name", key);
    js_set(js, entry, "callable", Ant_Value_FromBool(is_callable(value)));
    if (Primitive(value)) js_set(js, entry, "value", value);
  }
  std::string response = Respond(js, result);
  GC_ROOT_RESTORE(js, mark);
  return response;
}

std::string CallModule(const std::string &specifier, const std::string &name, const std::string &arguments) {
  ant_t *js = Runtime();
  if (!js) return {};
  GC_ROOT_SAVE(mark, js);
  ant_value_t module = Import(js, specifier);
  GC_ROOT_PIN(js, module);
  ant_value_t function = is_err(module) ? module : js_get(js, module, name.c_str());
  GC_ROOT_PIN(js, function);
  ant_value_t list = json_parse_value(js, js_mkstr(js, arguments.data(), arguments.size()));
  GC_ROOT_PIN(js, list);

  ant_value_t result;
  if (is_err(function)) result = function;
  else if (!is_callable(function)) result = js_mkerr(js, "%s.%s is not callable", specifier.c_str(), name.c_str());
  else if (is_err(list)) result = list;
  else if (!is_array_value(list)) result = js_mkerr(js, "module arguments must be an array");
  else {
    std::vector<ant_value_t> values;
    for (ant_offset_t index = 0; index < js_arr_len(js, list); index++) values.push_back(js_arr_get(js, list, index));
    result = sv_vm_call_explicit_this(js->vm, js, function, module, values.data(), static_cast<int>(values.size()));
    if (vtype(result) == kTypePromise) result = js_mkerr(js, "asynchronous Ant module results are not supported in the renderer");
  }
  std::string response = Respond(js, result);
  GC_ROOT_RESTORE(js, mark);
  return response;
}

}
