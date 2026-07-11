#include "renderer_app.h"

#include <map>
#include <stdio.h>
#include <string>

#include "../../ipc/capabilities.h"
#include "app_scheme.h"
#include "include/cef_command_line.h"
#include "include/cef_process_message.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include "renderer_bridge.h"

#if ANT_DESKTOP_RUNTIME_INTEGRATION
#include "ant_module_bindings.h"
#include "node_bindings.h"
#endif

namespace {

std::string Trim(std::string value);
std::string BindModule(const std::string &names, const std::string &require);

bool RewriteRendererModuleImport(std::string *source) {
  const std::string single = "'ant:desktop/renderer'";
  const std::string quoted = "\"ant:desktop/renderer\"";
  size_t specifier = source->find(single);
  size_t specifier_length = single.size();
  if (specifier == std::string::npos) {
    specifier = source->find(quoted);
    specifier_length = quoted.size();
  }
  if (specifier == std::string::npos) return true;
  size_t from = source->rfind("from", specifier);
  size_t import = from == std::string::npos ? std::string::npos : source->rfind("import", from);
  if (import == std::string::npos || from == std::string::npos) return false;
  std::string names = source->substr(import + 6, from - import - 6);
  size_t end = source->find(';', specifier + specifier_length);
  if (end == std::string::npos) end = specifier + specifier_length - 1;
  source->replace(import, end - import + 1, BindModule(Trim(names), "__antPreloadBindings"));
  return source->find(single) == std::string::npos && source->find(quoted) == std::string::npos;
}

std::string Trim(std::string value) {
  size_t start = value.find_first_not_of(" \t\r\n");
  size_t end = value.find_last_not_of(" \t\r\n");
  return start == std::string::npos ? "" : value.substr(start, end - start + 1);
}

std::string NamedBindings(std::string value) {
  for (size_t alias = value.find(" as "); alias != std::string::npos; alias = value.find(" as ", alias + 2)) {
    value.replace(alias, 4, ": ");
  }
  return value;
}

std::string BindModule(const std::string &names, const std::string &require) {
  if (names.empty()) return require + ";";
  if (names.front() == '{') return "const " + NamedBindings(names) + " = " + require + ";";
  if (names.starts_with("* as ")) return "const " + Trim(names.substr(5)) + " = " + require + ";";
  size_t comma = names.find(',');
  if (comma == std::string::npos) return "const " + names + " = " + require + ";";
  std::string primary = Trim(names.substr(0, comma));
  std::string secondary = Trim(names.substr(comma + 1));
  if (secondary.starts_with("* as ")) secondary = Trim(secondary.substr(5));
  else secondary = NamedBindings(secondary);
  return "const " + primary + " = " + require + ", " + secondary + " = " + primary + ";";
}

bool RewriteIntegrationModuleImports(std::string *source) {
  size_t search = 0;
  for (;;) {
    size_t import = source->find("import", search);
    if (import == std::string::npos) return true;
    size_t statement_end = source->find(';', import);
    if (statement_end == std::string::npos) statement_end = source->size();
    size_t specifier = std::string::npos;
    for (const char *prefix : {"'node:", "\"node:", "'ant:", "\"ant:"}) {
      size_t candidate = source->find(prefix, import);
      if (candidate < specifier) specifier = candidate;
    }
    if (specifier == std::string::npos || specifier > statement_end) {
      search = statement_end;
      continue;
    }
    char quote = (*source)[specifier];
    size_t specifier_end = source->find(quote, specifier + 1);
    size_t from = source->rfind("from", specifier);
    if (specifier_end == std::string::npos || from == std::string::npos || from < import) return false;
    std::string module = source->substr(specifier + 1, specifier_end - specifier - 1);
    std::string names = Trim(source->substr(import + 6, from - import - 6));
    std::string replacement;
    replacement = BindModule(names, "__antPreloadBindings.require('" + module + "')");
    size_t end = source->find(';', specifier_end);
    if (end == std::string::npos) end = specifier_end;
    source->replace(import, end - import + 1, replacement);
    search = import + replacement.size();
  }
}

class IpcV8Handler : public CefV8Handler {
public:
  explicit IpcV8Handler(std::set<std::string> capabilities) : capabilities_(std::move(capabilities)) {}

  bool Execute(const CefString &name, CefRefPtr<CefV8Value> object, const CefV8ValueList &arguments,
               CefRefPtr<CefV8Value> &retval, CefString &exception) override {
    if (name != "antNativeIpc" || arguments.size() != 4 || !arguments[0]->IsInt() ||
        !(arguments[1]->IsDouble() || arguments[1]->IsInt() || arguments[1]->IsUInt()) || !arguments[2]->IsString() ||
        !arguments[3]->IsString()) {
      exception = "invalid Ant IPC call";
      return true;
    }
    int operation = arguments[0]->GetIntValue();
    std::string access = operation == 0 ? "send" : "invoke";
    std::string channel = arguments[2]->GetStringValue();
    if (!capabilities_.contains(access + ":" + channel)) {
      exception = "Ant IPC channel is not granted: " + access + ":" + channel;
      return true;
    }
    CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
    if (!context || !context->GetFrame()) {
      exception = "Ant IPC context is unavailable";
      return true;
    }
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("ant.ipc");
    CefRefPtr<CefListValue> values = message->GetArgumentList();
    values->SetInt(0, operation);
    values->SetDouble(1, arguments[1]->GetDoubleValue());
    values->SetString(2, channel);
    values->SetString(3, arguments[3]->GetStringValue());
    context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
    retval = CefV8Value::CreateBool(true);
    return true;
  }

private:
  const std::set<std::string> capabilities_;
  IMPLEMENT_REFCOUNTING(IpcV8Handler);
};

class RendererApp : public CefApp, public CefRenderProcessHandler {
public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
    RegisterAntCustomSchemes(registrar);
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return this;
  }

  void OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra_info) override {
    BrowserConfig config;
    if (extra_info) {
      config.manifest = extra_info->GetString("antCapabilities");
      config.capabilities = ant::desktop::ParseCapabilities(config.manifest);
      config.sandbox = extra_info->GetBool("antSandbox");
      config.node_integration = extra_info->GetBool("antNodeIntegration");
      config.context_isolation = extra_info->GetBool("antContextIsolation");
      config.preload_path = extra_info->GetString("antPreloadPath");
      config.preload_source = extra_info->GetString("antPreloadSource");
    }
    configs_[browser->GetIdentifier()] = std::move(config);
  }

  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
    configs_.erase(browser->GetIdentifier());
  }

  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    if (!frame->IsMain()) return;
    auto config_entry = configs_.find(browser->GetIdentifier());
    if (config_entry == configs_.end()) return;
    const BrowserConfig &config = config_entry->second;
    std::string source(reinterpret_cast<const char *>(ant_desktop_ipc_bridge_source),
                       ant_desktop_ipc_bridge_source_len);
    CefRefPtr<CefV8Value> factory;
    CefRefPtr<CefV8Exception> exception;
    context->Eval(source, "ant://preload/ipc.js", 1, factory, exception);
    if (exception) {
      fprintf(stderr, "Ant IPC preload failed: %s\n", exception->GetMessage().ToString().c_str());
      return;
    }
    CefRefPtr<CefV8Value> bindings = CefV8Value::CreateObject(nullptr, nullptr);
    bindings->SetValue("capabilityManifest", CefV8Value::CreateString(config.manifest), V8_PROPERTY_ATTRIBUTE_READONLY);
    bindings->SetValue("sandbox", CefV8Value::CreateBool(config.sandbox), V8_PROPERTY_ATTRIBUTE_READONLY);
    bindings->SetValue("nodeIntegration", CefV8Value::CreateBool(config.node_integration),
                       V8_PROPERTY_ATTRIBUTE_READONLY);
    if (!config.sandbox) {
#if ANT_DESKTOP_RUNTIME_INTEGRATION
      bindings->SetValue("nodeEnvironment", CreateNodeEnvironmentBindings(), V8_PROPERTY_ATTRIBUTE_READONLY);
      CefRefPtr<CefV8Value> require = CreateAntModuleRequireBinding();
      if (!require) {
        fputs("failed to initialize Ant renderer module integration\n", stderr);
        return;
      }
      bindings->SetValue("nativeRequire", require, V8_PROPERTY_ATTRIBUTE_READONLY);
#else
      fputs("Ant renderer integration is unavailable in this helper process\n", stderr);
      return;
#endif
    }
    bindings->SetValue("nativeIpc", CefV8Value::CreateFunction("antNativeIpc", new IpcV8Handler(config.capabilities)),
                       V8_PROPERTY_ATTRIBUTE_READONLY);
    CefV8ValueList arguments{bindings};
    CefRefPtr<CefV8Value> bridge = factory->ExecuteFunction(nullptr, arguments);
    if (!bridge || !bridge->IsObject()) {
      fputs("Ant IPC preload did not return a bridge\n", stderr);
      return;
    }
    receivers_[browser->GetIdentifier()] = bridge->GetValue("receive");
    if (!config.preload_source.empty()) {
      const std::string &preload_path = config.preload_path;
      std::string preload = config.preload_source;
      if (!RewriteRendererModuleImport(&preload)) {
        fputs("Ant preload has an unsupported ant:desktop/renderer import\n", stderr);
        return;
      }
      if (!RewriteIntegrationModuleImports(&preload)) {
        fputs("Ant preload has an unsupported integrated module import\n", stderr);
        return;
      }
      context->GetGlobal()->SetValue("__antPreloadBindings", bridge, V8_PROPERTY_ATTRIBUTE_DONTENUM);
      if (config.context_isolation) {
        preload = "(globalThis => ((window, self) => {\n'use strict';\n" + preload +
                  "\n})(globalThis, globalThis))(Object.create(null))";
      }
      CefRefPtr<CefV8Value> ignored;
      CefRefPtr<CefV8Exception> preload_exception;
      context->Eval(preload, preload_path, 1, ignored, preload_exception);
      context->GetGlobal()->DeleteValue("__antPreloadBindings");
      if (preload_exception) {
        fprintf(stderr, "Ant preload failed: %s\n", preload_exception->GetMessage().ToString().c_str());
      }
    }
  }

  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    if (frame->IsMain()) receivers_.erase(browser->GetIdentifier());
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    if (source_process != PID_BROWSER || !frame->IsMain()) return false;
    if (message->GetName() != "ant.ipc.reply" && message->GetName() != "ant.ipc.event") { return false; }
    CefRefPtr<CefV8Context> context = frame->GetV8Context();
    if (!context || !context->Enter()) return true;
    auto receiver_entry = receivers_.find(browser->GetIdentifier());
    CefRefPtr<CefV8Value> receiver = receiver_entry == receivers_.end() ? nullptr : receiver_entry->second;
    if (receiver && receiver->IsFunction()) {
      CefRefPtr<CefListValue> values = message->GetArgumentList();
      CefV8ValueList args;
      args.push_back(CefV8Value::CreateInt(values->GetInt(0)));
      args.push_back(CefV8Value::CreateDouble(values->GetDouble(1)));
      args.push_back(CefV8Value::CreateString(values->GetString(2)));
      args.push_back(CefV8Value::CreateString(values->GetString(3)));
      receiver->ExecuteFunction(context->GetGlobal(), args);
    }
    context->Exit();
    return true;
  }

private:
  struct BrowserConfig {
    std::string manifest;
    std::set<std::string> capabilities;
    std::string preload_path;
    std::string preload_source;
    bool sandbox = true;
    bool node_integration = false;
    bool context_isolation = true;
  };

  std::map<int, CefRefPtr<CefV8Value>> receivers_;
  std::map<int, BrowserConfig> configs_;
  IMPLEMENT_REFCOUNTING(RendererApp);
};

} // namespace

CefRefPtr<CefApp> CreateAntRendererApp() {
  return new RendererApp;
}
