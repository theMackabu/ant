#include "renderer_ant_runtime.h"

#include <stdlib.h>
#include <string.h>

#include <ant.h>

#include "../../app/runtime/ant_runtime.h"

static ant_t *renderer_runtime;
static void *renderer_stack_base;

void ant_renderer_runtime_set_stack_base(void *stack_base) {
  renderer_stack_base = stack_base;
}

static char *CopyJson(ant_value_t value) {
  ant_value_t json = json_stringify_value(renderer_runtime, value);
  if (vtype(json) != T_STR) return NULL;
  size_t length = 0;
  const char *text = js_getstr(renderer_runtime, json, &length);
  char *copy = malloc(length + 1);
  if (!copy) return NULL;
  memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

static ant_value_t Response(bool ok, ant_value_t value) {
  if (!ok && is_err(value)) {
    if (vdata(value) != 0) {
      ant_value_t message = js_get(renderer_runtime, mkval(T_OBJ, vdata(value)), "message");
      if (vtype(message) == T_STR) value = message;
    }
    if (is_err(value)) {
      const char *text = js_str(renderer_runtime, value);
      value = js_mkstr(renderer_runtime, text, strlen(text));
    }
  }
  ant_value_t response = js_mkobj(renderer_runtime);
  js_set(renderer_runtime, response, "ok", js_bool(ok));
  js_set(renderer_runtime, response, ok ? "value" : "error", value);
  return response;
}

static ant_value_t ImportModule(const char *specifier) {
  if (!specifier || (strncmp(specifier, "node:", 5) != 0 && strncmp(specifier, "ant:", 4) != 0)) {
    return js_mkerr(renderer_runtime, "renderer integration only accepts node: and ant: modules");
  }
  return js_esm_import_sync_cstr(renderer_runtime, specifier, strlen(specifier));
}

bool ant_renderer_runtime_initialize(void) {
  if (renderer_runtime) return true;
  renderer_runtime = js_create_dynamic();
  if (!renderer_runtime) return false;
  if (!renderer_stack_base) {
    js_destroy(renderer_runtime);
    renderer_runtime = NULL;
    return false;
  }
  js_setstackbase(renderer_runtime, renderer_stack_base);
  char *argv[] = {(char *)"ant-desktop-renderer"};
  ant_runtime_init(renderer_runtime, 1, argv, NULL);
  ant_value_t initialized = AntInitializeRuntimeModules(renderer_runtime);
  if (is_err(initialized)) {
    js_destroy(renderer_runtime);
    renderer_runtime = NULL;
    return false;
  }
  return true;
}

char *ant_renderer_runtime_describe(const char *specifier) {
  if (!ant_renderer_runtime_initialize()) return NULL;
  ant_value_t module = ImportModule(specifier);
  if (is_err(module)) return CopyJson(Response(false, module));
  ant_value_t keys = js_own_property_keys(renderer_runtime, module, false, true);
  if (is_err(keys)) return CopyJson(Response(false, keys));

  ant_value_t exports = js_mkarr(renderer_runtime);
  ant_offset_t count = js_arr_len(renderer_runtime, keys);
  for (ant_offset_t index = 0; index < count; index++) {
    ant_value_t key = js_arr_get(renderer_runtime, keys, index);
    if (vtype(key) != T_STR) continue;
    size_t length = 0;
    const char *name = js_getstr(renderer_runtime, key, &length);
    char *stable_name = malloc(length + 1);
    if (!stable_name) continue;
    memcpy(stable_name, name, length);
    stable_name[length] = '\0';
    ant_value_t value = js_get(renderer_runtime, module, stable_name);
    ant_value_t entry = js_mkobj(renderer_runtime);
    js_set(renderer_runtime, entry, "name", key);
    js_set(renderer_runtime, entry, "callable", js_bool(is_callable(value)));
    if (!is_callable(value) && (vtype(value) == T_UNDEF || vtype(value) == T_NULL || vtype(value) == T_BOOL ||
                                vtype(value) == T_NUM || vtype(value) == T_STR)) {
      js_set(renderer_runtime, entry, "value", value);
    }
    js_arr_push(renderer_runtime, exports, entry);
    free(stable_name);
  }
  return CopyJson(Response(true, exports));
}

char *ant_renderer_runtime_call(const char *specifier, const char *name, const char *arguments_json) {
  if (!ant_renderer_runtime_initialize()) return NULL;
  ant_value_t module = ImportModule(specifier);
  if (is_err(module)) return CopyJson(Response(false, module));
  ant_value_t function = js_get(renderer_runtime, module, name);
  if (!is_callable(function)) {
    return CopyJson(Response(false, js_mkerr(renderer_runtime, "%s.%s is not callable", specifier, name)));
  }

  ant_value_t json = js_mkstr(renderer_runtime, arguments_json, strlen(arguments_json));
  ant_value_t arguments = json_parse_value(renderer_runtime, json);
  if (is_err(arguments) || !is_array_value(arguments)) {
    return CopyJson(
      Response(false, is_err(arguments) ? arguments : js_mkerr(renderer_runtime, "module arguments must be an array")));
  }
  ant_offset_t count = js_arr_len(renderer_runtime, arguments);
  ant_value_t *values = count ? malloc(sizeof(*values) * count) : NULL;
  if (count && !values) { return CopyJson(Response(false, js_mkerr(renderer_runtime, "out of memory"))); }
  for (ant_offset_t index = 0; index < count; index++) {
    values[index] = js_arr_get(renderer_runtime, arguments, index);
  }
  ant_value_t result =
    sv_vm_call_explicit_this(renderer_runtime->vm, renderer_runtime, function, module, values, (int)count);
  free(values);
  if (is_err(result)) return CopyJson(Response(false, result));
  if (vtype(result) == T_PROMISE) {
    return CopyJson(
      Response(false, js_mkerr(renderer_runtime, "asynchronous Ant module results are not integrated yet")));
  }
  return CopyJson(Response(true, result));
}

void ant_renderer_runtime_free(char *value) {
  free(value);
}

void ant_renderer_runtime_shutdown(void) {
  if (!renderer_runtime) return;
  js_destroy(renderer_runtime);
  renderer_runtime = NULL;
}
