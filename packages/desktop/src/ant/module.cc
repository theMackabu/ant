#include "desktop.hh"

namespace desktop {

static const char kWireSource[] = {
#embed "../builtins/page/wire.js"
};

static const char kModuleSource[] = {
#embed "../builtins/desktop.js"
};

static const char kRendererSource[] = {
#embed "../builtins/page/bridge.js"
};

static const char kBootstrapSource[] = {
#embed "../builtins/bootstrap.js"
};

static ant_t *runtime;
static ant_value_t native;

static std::string Text(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeString) return {};
  size_t length = 0;
  const char *text = js_getstr(js, value, &length);
  return std::string(text, length);
}

static ant_value_t Value(ant_t *js, const std::string &text) {
  return js_mkstr(js, text.data(), text.size());
}

static bool Flag(ant_t *js, ant_value_t object, const char *name) {
  return js_truthy(js, js_get(js, object, name));
}

static int64_t WindowArgument(ant_value_t *args, int nargs) {
  return nargs > 0 && vtype(args[0]) == kTypeNumber ? static_cast<int64_t>(Ant_Value_AsNumberUnchecked(args[0])) : 0;
}

template <size_t Count> static void Dispatch(ant_value_t (&args)[Count]) {
  ant_t *js = runtime;
  if (!js) return;
  ant_value_t dispatch = js_get(js, native, "dispatch");
  if (!is_callable(dispatch)) return;
  ant_value_t result = sv_vm_call_explicit_this(js->vm, js, dispatch, Ant_Value_Undefined, args, Count);
  if (is_err(result)) fprintf(stderr, "desktop dispatch failed: %s\n", js_str(js, result));
}

static ant_value_t Native_CreateWindow(ant_params_t) {
  if (nargs < 2 || vtype(args[0]) != kTypeString || !is_object_type(args[1])) return js_mkerr(js, "invalid BrowserWindow options");
  WindowConfig config;
  config.capabilities = Text(js, js_get(js, args[1], "capabilities"));
  config.preload = Text(js, js_get(js, args[1], "preload"));
  config.sandbox = Flag(js, args[1], "sandbox");
  config.integration = Flag(js, args[1], "integration");
  config.context_isolation = Flag(js, args[1], "contextIsolation");
  config.transparent = Flag(js, args[1], "transparent");
  int64_t window = CreateWindow(Text(js, args[0]).c_str(), std::move(config));
  if (!window) return js_mkerr(js, "failed to create BrowserWindow");
  return Ant_Value_FromNumber(static_cast<double>(window));
}

static ant_value_t Native_Load(ant_params_t) {
  if (nargs < 3) return js_mkerr(js, "invalid BrowserWindow load");
  const char *error = nullptr;
  if (!LoadWindow(WindowArgument(args, nargs), Text(js, args[1]), Text(js, args[2]), &error)) return js_mkerr(js, "%s", error);
  return Ant_Value_Undefined;
}

static ant_value_t Native_Call(ant_params_t) {
  int64_t window = WindowArgument(args, nargs);
  std::string operation = nargs > 1 ? Text(js, args[1]) : std::string();
  ant_value_t argument = nargs > 2 ? args[2] : Ant_Value_Undefined;

  if (operation == "close") {
    CloseWindow(window);
    return Ant_Value_True;
  }
  if (operation == "getBounds") {
    double bounds[4];
    if (!AntDesktop_Window_Bounds(window, bounds)) return js_mkerr(js, "BrowserWindow is closed");
    ant_value_t result = Ant_Object_Create(js);
    js_set(js, result, "x", Ant_Value_FromNumber(bounds[0]));
    js_set(js, result, "y", Ant_Value_FromNumber(bounds[1]));
    js_set(js, result, "width", Ant_Value_FromNumber(bounds[2]));
    js_set(js, result, "height", Ant_Value_FromNumber(bounds[3]));
    return result;
  }

  bool flag = vtype(argument) == kTypeBool && js_truthy(js, argument);
  if (AntDesktop_Window_Perform(window, operation.c_str(), Text(js, argument).c_str(), flag)) return Ant_Value_True;

  int x = vtype(argument) == kTypeNumber ? static_cast<int>(Ant_Value_AsNumberUnchecked(argument)) : 0;
  int y = nargs > 3 && vtype(args[3]) == kTypeNumber ? static_cast<int>(Ant_Value_AsNumberUnchecked(args[3])) : 0;
  bool result = false;
  if (!BrowserCommand(window, operation, x, y, &result)) return Ant_Value_False;
  return operation == "isDevToolsOpened" ? Ant_Value_FromBool(result) : Ant_Value_True;
}

static ant_value_t Native_Send(ant_params_t) {
  const uint8_t *bytes;
  size_t size;
  if (nargs < 3 || vtype(args[1]) != kTypeNumber || !buffer_source_get_bytes(js, args[2], &bytes, &size))
    return js_mkerr(js, "invalid IPC message");
  int64_t query = static_cast<int64_t>(Ant_Value_AsNumberUnchecked(args[1]));
  return Ant_Value_FromBool(SendIpc(WindowArgument(args, nargs), query, bytes, size));
}

static ant_value_t Native_SetMenu(ant_params_t) {
  if (nargs > 0 && vtype(args[0]) == kTypeString) AntDesktop_Menu_Set(Text(js, args[0]).c_str());
  else AntDesktop_Menu_Set(nullptr);
  return Ant_Value_Undefined;
}

static ant_value_t Native_Path(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) return Ant_Value_Undefined;
  char *path = AntDesktop_App_Path(Text(js, args[0]).c_str());
  return path ? Value(js, Owned(path)) : Ant_Value_Undefined;
}

static ant_value_t Native_Quit(ant_params_t) {
  Quit();
  return Ant_Value_Undefined;
}

std::string RendererBridgeSource() {
  std::string source = "(bindings => {\n";
  source.append(kWireSource, sizeof(kWireSource));
  source.append("\n");
  source.append(kRendererSource, sizeof(kRendererSource));
  source.append("\nreturn createRendererBridge(bindings);\n})");
  return source;
}

bool InstallModule(ant_t *js) {
  versions.ant = Text(js, js_get(js, js_get(js, js_glob(js), "Ant"), "version"));

  ant_value_t names = Ant_Object_Create(js);
  js_set(js, names, "ant", Value(js, versions.ant));
  js_set(js, names, "desktop", Value(js, versions.desktop));
  js_set(js, names, "chrome", Value(js, versions.chrome));

  ant_value_t process_versions = js_get(js, js_get(js, js_glob(js), "process"), "versions");
  if (is_object_type(process_versions)) {
    js_set(js, process_versions, "ant-desktop", Value(js, versions.desktop));
    js_set(js, process_versions, "chrome", Value(js, versions.chrome));
  }

  ant_value_t bindings = Ant_Object_Create(js);
  js_set(js, bindings, "versions", names);
  js_set(js, bindings, "resourcesPath", Value(js, Owned(AntDesktop_App_Path("resources"))));
  js_set(js, bindings, "createWindow", Ant_Function_Create(Native_CreateWindow));
  js_set(js, bindings, "load", Ant_Function_Create(Native_Load));
  js_set(js, bindings, "call", Ant_Function_Create(Native_Call));
  js_set(js, bindings, "send", Ant_Function_Create(Native_Send));
  js_set(js, bindings, "setMenu", Ant_Function_Create(Native_SetMenu));
  js_set(js, bindings, "path", Ant_Function_Create(Native_Path));
  js_set(js, bindings, "quit", Ant_Function_Create(Native_Quit));

  ant_value_t module = js_esm_import_sync_cstr(js, "node:module", 11);
  ant_value_t bootstrap = js_eval_bytecode_eval(js, kBootstrapSource, sizeof(kBootstrapSource), true);
  if (is_err(module) || !is_callable(bootstrap)) return false;

  std::string source(kModuleSource, sizeof(kModuleSource));
  ant_value_t args[] = {bindings, js_get(js, module, "registerHooks"), Value(js, source)};
  if (is_err(sv_vm_call_explicit_this(js->vm, js, bootstrap, Ant_Value_Undefined, args, 3))) return false;

  runtime = js;
  native = bindings;
  return true;
}

void EmitWindowEvent(int64_t window, const char *type, const std::string &detail, int64_t code) {
  if (!runtime) return;
  ant_value_t args[] = {
    Ant_String_FromLiteral(runtime, "event"), Ant_Value_FromNumber(static_cast<double>(window)),
    js_mkstr(runtime, type, strlen(type)),    Value(runtime, detail),
    Ant_Value_FromNumber(static_cast<double>(code)),
  };
  Dispatch(args);
}

void EmitIpc(int64_t window, int64_t query, const void *data, size_t size) {
  ant_t *js = runtime;
  ArrayBufferData *buffer = js ? create_array_buffer_data(js, size) : nullptr;
  if (!buffer) return;
  if (size) memcpy(buffer->data, data, size);
  ant_value_t args[] = {
    Ant_String_FromLiteral(js, "ipc"),
    Ant_Value_FromNumber(static_cast<double>(window)),
    Ant_Value_FromNumber(static_cast<double>(query)),
    create_typed_array(js, TYPED_ARRAY_UINT8, buffer, 0, size, js->builtins.buffer_proto),
  };
  Dispatch(args);
}

void EmitLoad(int64_t window) {
  if (!runtime) return;
  ant_value_t args[] = {Ant_String_FromLiteral(runtime, "load"), Ant_Value_FromNumber(static_cast<double>(window))};
  Dispatch(args);
}

void EmitDestroyed(int64_t window) {
  if (!runtime) return;
  ant_value_t args[] = {Ant_String_FromLiteral(runtime, "destroyed"), Ant_Value_FromNumber(static_cast<double>(window))};
  Dispatch(args);
}

void EmitMenuClick(int64_t item) {
  if (!runtime) return;
  ant_value_t args[] = {Ant_String_FromLiteral(runtime, "menu"), Ant_Value_FromNumber(static_cast<double>(item))};
  Dispatch(args);
}

}
