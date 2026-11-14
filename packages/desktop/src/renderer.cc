#include "desktop.h"

#include <map>
#include <vector>

#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_library_loader.h"

namespace desktop {

namespace {

void *stack_base;
ant_t *runtime;

ant_t *Runtime() {
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

std::string Respond(ant_t *js, ant_value_t value) {
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

ant_value_t Import(ant_t *js, const std::string &specifier) {
  if (!specifier.starts_with("node:") && !specifier.starts_with("ant:")) {
    return js_mkerr(js, "renderer integration only accepts node: and ant: modules");
  }
  return js_esm_import_sync_cstr(js, specifier.data(), specifier.size());
}

bool Primitive(ant_value_t value) {
  int type = vtype(value);
  return type == kTypeUndefined || type == kTypeNull || type == kTypeBool || type == kTypeNumber || type == kTypeString;
}

std::string Describe(ant_t *js, const std::string &specifier) {
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

std::string Call(ant_t *js, const std::string &specifier, const std::string &name, const std::string &arguments) {
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

class BindingHandler final : public CefV8Handler {
public:
  bool Execute(const CefString &name, CefRefPtr<CefV8Value> object, const CefV8ValueList &arguments, CefRefPtr<CefV8Value> &result,
               CefString &exception) override {
    for (const CefRefPtr<CefV8Value> &argument : arguments) {
      bool number = argument->IsInt() || argument->IsUInt() || argument->IsDouble();
      if (!argument->IsString() && !(name == "send" && number)) {
        exception = "invalid Ant binding call";
        return true;
      }
    }
    if (name == "send" && arguments.size() == 4) {
      CefRefPtr<CefFrame> frame = CefV8Context::GetCurrentContext()->GetFrame();
      CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("ant.ipc");
      CefRefPtr<CefListValue> values = message->GetArgumentList();
      values->SetInt(0, arguments[0]->GetIntValue());
      values->SetDouble(1, arguments[1]->GetDoubleValue());
      values->SetString(2, arguments[2]->GetStringValue());
      values->SetString(3, arguments[3]->GetStringValue());
      if (frame) frame->SendProcessMessage(PID_BROWSER, message);
      return true;
    }
    ant_t *js = Runtime();
    if (!js) {
      exception = "Ant renderer runtime is unavailable";
    } else if (name == "describe" && arguments.size() == 1) {
      result = CefV8Value::CreateString(Describe(js, arguments[0]->GetStringValue()));
    } else if (name == "call" && arguments.size() == 3) {
      result = CefV8Value::CreateString(
        Call(js, arguments[0]->GetStringValue(), arguments[1]->GetStringValue(), arguments[2]->GetStringValue()));
    } else {
      exception = "invalid Ant binding call";
    }
    return true;
  }

private:
  IMPLEMENT_REFCOUNTING(BindingHandler);
};

class RendererApp final : public CefApp, public CefRenderProcessHandler {
public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override { RegisterSchemes(registrar); }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

  void OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra) override {
    if (extra && extra->HasKey("capabilities")) configs_[browser->GetIdentifier()] = extra->Copy(false);
  }

  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
    configs_.erase(browser->GetIdentifier());
    receivers_.erase(browser->GetIdentifier());
  }

  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
    auto found = configs_.find(browser->GetIdentifier());
    if (!frame->IsMain() || found == configs_.end()) return;
    CefRefPtr<CefDictionaryValue> config = found->second;
    bool sandbox = config->GetBool("sandbox");

    CefRefPtr<CefV8Value> factory = Evaluate(context, RendererBridgeSource(), "ant://desktop/bridge.js");
    if (!factory) return;

    CefRefPtr<CefV8Handler> handler = new BindingHandler;
    CefRefPtr<CefV8Value> names = CefV8Value::CreateObject(nullptr, nullptr);
    CefRefPtr<CefDictionaryValue> versions = config->GetDictionary("versions");
    for (const char *name : {"ant", "desktop", "chrome"}) {
      names->SetValue(name, CefV8Value::CreateString(versions->GetString(name)), V8_PROPERTY_ATTRIBUTE_READONLY);
    }
    CefRefPtr<CefV8Value> bindings = CefV8Value::CreateObject(nullptr, nullptr);
    bindings->SetValue("versions", names, V8_PROPERTY_ATTRIBUTE_READONLY);
    bindings->SetValue("capabilities", CefV8Value::CreateString(config->GetString("capabilities")), V8_PROPERTY_ATTRIBUTE_READONLY);
    bindings->SetValue("sandbox", CefV8Value::CreateBool(sandbox), V8_PROPERTY_ATTRIBUTE_READONLY);
    bindings->SetValue("integration", CefV8Value::CreateBool(config->GetBool("integration")), V8_PROPERTY_ATTRIBUTE_READONLY);
    bindings->SetValue("send", CefV8Value::CreateFunction("send", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    if (!sandbox) {
      bindings->SetValue("describe", CefV8Value::CreateFunction("describe", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
      bindings->SetValue("call", CefV8Value::CreateFunction("call", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    }

    CefRefPtr<CefV8Value> bridge = factory->ExecuteFunction(nullptr, {bindings});
    if (!bridge || !bridge->IsObject()) {
      CefRefPtr<CefV8Exception> exception = factory->GetException();
      fprintf(stderr, "Ant renderer bridge failed: %s\n", exception ? exception->GetMessage().ToString().c_str() : "no bridge");
      return;
    }
    receivers_[browser->GetIdentifier()] = bridge->GetValue("receive");

    std::string preload = config->GetString("preloadSource");
    if (preload.empty()) return;
    if (config->GetBool("contextIsolation")) {
      preload = "(globalThis => ((window, self) => {\n'use strict';\n" + preload + "\n})(globalThis, globalThis))(Object.create(null))";
    }
    context->GetGlobal()->SetValue("__antPreload", bridge, V8_PROPERTY_ATTRIBUTE_DONTENUM);
    Evaluate(context, preload, config->GetString("preloadPath"));
    context->GetGlobal()->DeleteValue("__antPreload");
  }

  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
    if (frame->IsMain()) receivers_.erase(browser->GetIdentifier());
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                                CefRefPtr<CefProcessMessage> message) override {
    if (source != PID_BROWSER || !frame->IsMain() || message->GetName() != "ant.ipc") return false;
    auto found = receivers_.find(browser->GetIdentifier());
    CefRefPtr<CefV8Context> context = frame->GetV8Context();
    if (found == receivers_.end() || !found->second->IsFunction() || !context || !context->Enter()) return true;
    CefRefPtr<CefListValue> values = message->GetArgumentList();
    found->second->ExecuteFunction(
      nullptr, {CefV8Value::CreateInt(values->GetInt(0)), CefV8Value::CreateDouble(values->GetDouble(1)),
                CefV8Value::CreateString(values->GetString(2)), CefV8Value::CreateString(values->GetString(3))});
    context->Exit();
    return true;
  }

private:
  static CefRefPtr<CefV8Value> Evaluate(CefRefPtr<CefV8Context> context, const std::string &source, const CefString &url) {
    CefRefPtr<CefV8Value> value;
    CefRefPtr<CefV8Exception> exception;
    if (context->Eval(source, url, 1, value, exception)) return value;
    fprintf(stderr, "%s: %s\n", url.ToString().c_str(), exception ? exception->GetMessage().ToString().c_str() : "evaluation failed");
    return nullptr;
  }

  std::map<int, CefRefPtr<CefDictionaryValue>> configs_;
  std::map<int, CefRefPtr<CefV8Value>> receivers_;
  IMPLEMENT_REFCOUNTING(RendererApp);
};

}

int RunHelper(int argc, char **argv, void *base) {
  stack_base = base;
  std::string framework = getenv("ANT_DESKTOP_CEF_FRAMEWORK") ?: "";
  for (int index = 1; index < argc; index++) {
    std::string argument = argv[index];
    if (argument.starts_with("--framework-dir-path=")) framework = argument.substr(21);
  }
  if (!cef_load_library((framework + "/Chromium Embedded Framework").c_str())) return 1;
  int code = CefExecuteProcess(CefMainArgs(argc, argv), new RendererApp, nullptr);
  if (runtime) Ant_Destroy(runtime);
  cef_unload_library();
  return code;
}

}
