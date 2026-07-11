#include "ant_module_bindings.h"

#include <string>
#include <vector>

#include "include/cef_parser.h"
#include "renderer_ant_runtime.h"

namespace {

CefRefPtr<CefValue> V8ToValue(CefRefPtr<CefV8Value> input, int depth) {
  if (!input || depth > 32) return nullptr;
  CefRefPtr<CefValue> output = CefValue::Create();
  if (input->IsUndefined() || input->IsNull()) output->SetNull();
  else if (input->IsBool()) output->SetBool(input->GetBoolValue());
  else if (input->IsInt()) output->SetInt(input->GetIntValue());
  else if (input->IsUInt() || input->IsDouble()) output->SetDouble(input->GetDoubleValue());
  else if (input->IsString()) output->SetString(input->GetStringValue());
  else if (input->IsArray()) {
    CefRefPtr<CefListValue> list = CefListValue::Create();
    int length = input->GetArrayLength();
    list->SetSize(length);
    for (int index = 0; index < length; index++) {
      CefRefPtr<CefValue> value = V8ToValue(input->GetValue(index), depth + 1);
      if (!value) return nullptr;
      list->SetValue(index, value);
    }
    output->SetList(list);
  } else if (input->IsObject() && !input->IsFunction()) {
    CefRefPtr<CefDictionaryValue> dictionary = CefDictionaryValue::Create();
    std::vector<CefString> keys;
    input->GetKeys(keys);
    for (const CefString &key : keys) {
      CefRefPtr<CefV8Value> property = input->GetValue(key);
      if (!property || property->IsFunction() || property->IsUndefined()) continue;
      CefRefPtr<CefValue> value = V8ToValue(property, depth + 1);
      if (!value) return nullptr;
      dictionary->SetValue(key, value);
    }
    output->SetDictionary(dictionary);
  } else {
    return nullptr;
  }
  return output;
}

CefRefPtr<CefV8Value> ValueToV8(CefRefPtr<CefValue> value, int depth) {
  if (!value || depth > 32) return CefV8Value::CreateUndefined();
  switch (value->GetType()) {
  case VTYPE_NULL:
  case VTYPE_INVALID:
    return CefV8Value::CreateNull();
  case VTYPE_BOOL:
    return CefV8Value::CreateBool(value->GetBool());
  case VTYPE_INT:
    return CefV8Value::CreateInt(value->GetInt());
  case VTYPE_DOUBLE:
    return CefV8Value::CreateDouble(value->GetDouble());
  case VTYPE_STRING:
    return CefV8Value::CreateString(value->GetString());
  case VTYPE_LIST: {
    CefRefPtr<CefListValue> list = value->GetList();
    CefRefPtr<CefV8Value> output = CefV8Value::CreateArray(static_cast<int>(list->GetSize()));
    for (size_t index = 0; index < list->GetSize(); index++) {
      output->SetValue(static_cast<int>(index), ValueToV8(list->GetValue(index), depth + 1));
    }
    return output;
  }
  case VTYPE_DICTIONARY: {
    CefRefPtr<CefDictionaryValue> dictionary = value->GetDictionary();
    CefRefPtr<CefV8Value> output = CefV8Value::CreateObject(nullptr, nullptr);
    CefDictionaryValue::KeyList keys;
    dictionary->GetKeys(keys);
    for (const CefString &key : keys) {
      output->SetValue(key, ValueToV8(dictionary->GetValue(key), depth + 1), V8_PROPERTY_ATTRIBUTE_NONE);
    }
    return output;
  }
  case VTYPE_BINARY:
  case VTYPE_NUM_VALUES:
    return CefV8Value::CreateUndefined();
  }
  return CefV8Value::CreateUndefined();
}

std::string ErrorMessage(CefRefPtr<CefValue> value) {
  if (!value) return "Ant module call failed";
  if (value->GetType() == VTYPE_STRING) return value->GetString();
  return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}

class AntExportHandler final : public CefV8Handler {
public:
  AntExportHandler(std::string specifier, std::string name)
      : specifier_(std::move(specifier)), name_(std::move(name)) {}

  bool Execute(const CefString &name, CefRefPtr<CefV8Value> object, const CefV8ValueList &arguments,
               CefRefPtr<CefV8Value> &retval, CefString &exception) override {
    CefRefPtr<CefListValue> list = CefListValue::Create();
    list->SetSize(arguments.size());
    for (size_t index = 0; index < arguments.size(); index++) {
      CefRefPtr<CefValue> value = V8ToValue(arguments[index], 0);
      if (!value) {
        exception = "Ant module arguments must be structured-clone-compatible";
        return true;
      }
      list->SetValue(index, value);
    }
    CefRefPtr<CefValue> arguments_value = CefValue::Create();
    arguments_value->SetList(list);
    std::string arguments_json = CefWriteJSON(arguments_value, JSON_WRITER_DEFAULT);
    char *response_json = ant_renderer_runtime_call(specifier_.c_str(), name_.c_str(), arguments_json.c_str());
    if (!response_json) {
      exception = "Ant renderer runtime call failed";
      return true;
    }
    CefRefPtr<CefValue> response = CefParseJSON(response_json, JSON_PARSER_RFC);
    ant_renderer_runtime_free(response_json);
    if (!response || response->GetType() != VTYPE_DICTIONARY) {
      exception = "Ant renderer runtime returned invalid JSON";
      return true;
    }
    CefRefPtr<CefDictionaryValue> dictionary = response->GetDictionary();
    if (!dictionary->GetBool("ok")) {
      exception = specifier_ + "." + name_ + ": " + ErrorMessage(dictionary->GetValue("error"));
      return true;
    }
    retval = dictionary->HasKey("value") ? ValueToV8(dictionary->GetValue("value"), 0) : CefV8Value::CreateUndefined();
    return true;
  }

private:
  std::string specifier_;
  std::string name_;
  IMPLEMENT_REFCOUNTING(AntExportHandler);
};

class AntRequireHandler final : public CefV8Handler {
public:
  bool Execute(const CefString &name, CefRefPtr<CefV8Value> object, const CefV8ValueList &arguments,
               CefRefPtr<CefV8Value> &retval, CefString &exception) override {
    if (arguments.size() != 1 || !arguments[0]->IsString()) {
      exception = "require() needs one node: or ant: module specifier";
      return true;
    }
    std::string specifier = arguments[0]->GetStringValue();
    char *description_json = ant_renderer_runtime_describe(specifier.c_str());
    if (!description_json) {
      exception = "Ant renderer runtime is unavailable";
      return true;
    }
    CefRefPtr<CefValue> response = CefParseJSON(description_json, JSON_PARSER_RFC);
    ant_renderer_runtime_free(description_json);
    if (!response || response->GetType() != VTYPE_DICTIONARY) {
      exception = "Ant renderer runtime returned invalid module metadata";
      return true;
    }
    CefRefPtr<CefDictionaryValue> dictionary = response->GetDictionary();
    if (!dictionary->GetBool("ok")) {
      exception = ErrorMessage(dictionary->GetValue("error"));
      return true;
    }
    CefRefPtr<CefListValue> exports = dictionary->GetList("value");
    CefRefPtr<CefV8Value> module = CefV8Value::CreateObject(nullptr, nullptr);
    for (size_t index = 0; exports && index < exports->GetSize(); index++) {
      CefRefPtr<CefDictionaryValue> entry = exports->GetDictionary(index);
      if (!entry) continue;
      std::string export_name = entry->GetString("name");
      CefRefPtr<CefV8Value> value;
      if (entry->GetBool("callable")) {
        value = CefV8Value::CreateFunction(export_name, new AntExportHandler(specifier, export_name));
      } else if (entry->HasKey("value")) {
        value = ValueToV8(entry->GetValue("value"), 0);
      } else {
        continue;
      }
      module->SetValue(export_name, value, V8_PROPERTY_ATTRIBUTE_READONLY);
    }
    retval = module;
    return true;
  }

private:
  IMPLEMENT_REFCOUNTING(AntRequireHandler);
};

} // namespace

CefRefPtr<CefV8Value> CreateAntModuleRequireBinding() {
  if (!ant_renderer_runtime_initialize()) return nullptr;
  return CefV8Value::CreateFunction("require", new AntRequireHandler());
}
