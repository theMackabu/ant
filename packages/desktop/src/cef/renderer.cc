#include "desktop.hh"

#include <map>

#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"

namespace desktop {

namespace {

class BindingHandler final : public CefV8Handler {
public:
  bool Execute(const CefString &name, CefRefPtr<CefV8Value> object, const CefV8ValueList &arguments, CefRefPtr<CefV8Value> &result,
               CefString &exception) override {
    for (const CefRefPtr<CefV8Value> &argument : arguments) {
      if (argument->IsString()) continue;
      exception = "invalid Ant binding call";
      return true;
    }
    std::string response;
    if (name == "describe" && arguments.size() == 1) {
      response = DescribeModule(arguments[0]->GetStringValue());
    } else if (name == "call" && arguments.size() == 3) {
      response = CallModule(arguments[0]->GetStringValue(), arguments[1]->GetStringValue(), arguments[2]->GetStringValue());
    }
    if (response.empty()) exception = "invalid Ant binding call";
    else result = CefV8Value::CreateString(response);
    return true;
  }

private:
  IMPLEMENT_REFCOUNTING(BindingHandler);
};

class RendererApp final : public CefApp, public CefRenderProcessHandler {
public:
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override { RegisterSchemes(registrar); }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

  void OnWebKitInitialized() override { router_ = CefMessageRouterRendererSide::Create(IpcRouterConfig()); }

  void OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra) override {
    if (extra && extra->HasKey("capabilities")) configs_[browser->GetIdentifier()] = extra->Copy(false);
  }

  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
    configs_.erase(browser->GetIdentifier());
  }

  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override {
    auto found = configs_.find(browser->GetIdentifier());
    if (!frame->IsMain() || found == configs_.end() || !context->IsSame(frame->GetV8Context())) return;
    CefRefPtr<CefDictionaryValue> config = found->second;
    bool sandbox = config->GetBool("sandbox");
    router_->OnContextCreated(browser, frame, context);

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
    bindings->SetValue("query", context->GetGlobal()->GetValue(IpcRouterConfig().js_query_function), V8_PROPERTY_ATTRIBUTE_READONLY);
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
    router_->OnContextReleased(browser, frame, context);
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source,
                                CefRefPtr<CefProcessMessage> message) override {
    return router_->OnProcessMessageReceived(browser, frame, source, message);
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
  CefRefPtr<CefMessageRouterRendererSide> router_;
  IMPLEMENT_REFCOUNTING(RendererApp);
};

}

CefMessageRouterConfig IpcRouterConfig() {
  CefMessageRouterConfig config;
  config.js_query_function = "__antQuery";
  config.js_cancel_function = "__antQueryCancel";
  return config;
}

int RunHelper(int argc, char **argv, void *stack_base) {
  SetIntegrationStackBase(stack_base);
  if (!platform::LoadHelperBrowser(argc, argv)) return 1;
  int code = CefExecuteProcess(platform::MainArgs(argc, argv), new RendererApp, nullptr);
  ShutdownIntegration();
  platform::ReleaseBrowser();
  return code;
}

}
