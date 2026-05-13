#include "bind.h"
#include "gc.h"

#include <string.h>

typedef void (*inspector_route_fn)(
  inspector_client_t *client,
  int id, yyjson_val *params
);

typedef struct {
  const char *method;
  inspector_route_fn fn;
} inspector_route_t;

static void route_empty(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_send_empty_result(client, id);
}

static void route_runtime_enable(inspector_client_t *client, int id, yyjson_val *params) {
  client->runtime_enabled = true;
  inspector_send_empty_result(client, id);
  inspector_send_execution_context(client);
  inspector_replay_console_events(client);
}

static void route_console_enable(inspector_client_t *client, int id, yyjson_val *params) {
  client->console_enabled = true;
  inspector_send_empty_result(client, id);
  inspector_replay_console_events(client);
}

static void route_network_enable(inspector_client_t *client, int id, yyjson_val *params) {
  client->network_enabled = true;
  inspector_send_empty_result(client, id);
}

static void route_network_disable(inspector_client_t *client, int id, yyjson_val *params) {
  client->network_enabled = false;
  inspector_send_empty_result(client, id);
}

static void route_debugger_enable(inspector_client_t *client, int id, yyjson_val *params) {
  client->debugger_enabled = true;
  inspector_send_response_obj(client, id, "{\"debuggerId\":\"ant-debugger\"}");
  inspector_send_registered_scripts(client);
}

static void route_collect_garbage(inspector_client_t *client, int id, yyjson_val *params) {
  gc_run(client->js);
  inspector_send_empty_result(client, id);
}

static void route_run_if_waiting(inspector_client_t *client, int id, yyjson_val *params) {
  g_inspector.waiting_for_debugger = false;
  inspector_send_empty_result(client, id);
}

static void route_discard_console(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_clear_console_events();
  inspector_send_empty_result(client, id);
}

static void route_get_isolate_id(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_send_response_obj(client, id, "{\"id\":\"ant-isolate\"}");
}

static void route_schema_domains(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_send_response_obj(
    client, id,
    "{\"domains\":[{\"name\":\"Runtime\",\"version\":\"1.3\"},{\"name\":\"Console\",\"version\":\"1.3\"},{\"name\":\"Debugger\",\"version\":\"1.3\"},{\"name\":\"Network\",\"version\":\"1.3\"},{\"name\":\"HeapProfiler\",\"version\":\"1.3\"},{\"name\":\"Profiler\",\"version\":\"1.3\"}]}"
  );
}

static void route_network_get_response_body(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_get_response_body(client, id, params);
}

static void route_network_get_request_post_data(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_get_request_post_data(client, id, params);
}

static void route_runtime_evaluate(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_eval(client, id, params);
}

static void route_runtime_compile_script(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_compile_script(client, id, params);
}

static void route_runtime_run_script(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_run_script(client, id, params);
}

static void route_debugger_get_script_source(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_get_script_source(client, id, params);
}

static void route_runtime_get_heap_usage(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_get_heap_usage(client, id);
}

static void route_runtime_get_properties(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_get_properties(client, id, params);
}

static void route_runtime_global_lexical_scope_names(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_global_lexical_scope_names(client, id);
}

static const inspector_route_t k_routes[] = {
  {"Console.enable", route_console_enable},
  {"Debugger.enable", route_debugger_enable},
  {"Debugger.getScriptSource", route_debugger_get_script_source},
  {"Debugger.setAsyncCallStackDepth", route_empty},
  {"Debugger.setBlackboxPatterns", route_empty},
  {"Debugger.setBlackboxedRanges", route_empty},
  {"Debugger.setPauseOnExceptions", route_empty},
  {"HeapProfiler.collectGarbage", route_collect_garbage},
  {"HeapProfiler.enable", route_empty},
  {"Inspector.enable", route_empty},
  {"Log.enable", route_empty},
  {"Log.startViolationsReport", route_empty},
  {"Network.clearAcceptedEncodingsOverride", route_empty},
  {"Network.disable", route_network_disable},
  {"Network.emulateNetworkConditionsByRule", route_empty},
  {"Network.enable", route_network_enable},
  {"Network.getRequestPostData", route_network_get_request_post_data},
  {"Network.getResponseBody", route_network_get_response_body},
  {"Network.overrideNetworkState", route_empty},
  {"Network.setAttachDebugStack", route_empty},
  {"Network.setBlockedURLs", route_empty},
  {"Profiler.enable", route_empty},
  {"Runtime.compileScript", route_runtime_compile_script},
  {"Runtime.discardConsoleEntries", route_discard_console},
  {"Runtime.enable", route_runtime_enable},
  {"Runtime.evaluate", route_runtime_evaluate},
  {"Runtime.getHeapUsage", route_runtime_get_heap_usage},
  {"Runtime.getIsolateId", route_get_isolate_id},
  {"Runtime.getProperties", route_runtime_get_properties},
  {"Runtime.globalLexicalScopeNames", route_runtime_global_lexical_scope_names},
  {"Runtime.releaseObject", route_empty},
  {"Runtime.releaseObjectGroup", route_empty},
  {"Runtime.runIfWaitingForDebugger", route_run_if_waiting},
  {"Runtime.runScript", route_runtime_run_script},
  {"Runtime.setAsyncCallStackDepth", route_empty},
  {"Runtime.setCustomObjectFormatterEnabled", route_empty},
  {"Schema.getDomains", route_schema_domains},
};

void inspector_handle_message(inspector_client_t *client, const char *payload, size_t len) {
  yyjson_doc *doc = yyjson_read(payload, len, 0);
  if (!doc) return;
  
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *id_val = yyjson_obj_get(root, "id");
  yyjson_val *method_val = yyjson_obj_get(root, "method");
  yyjson_val *params = yyjson_obj_get(root, "params");
  
  int id = id_val && yyjson_is_int(id_val) ? (int)yyjson_get_int(id_val) : 0;
  const char *method = method_val && yyjson_is_str(method_val) ? yyjson_get_str(method_val) : "";

  for (
    size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++) if (
    strcmp(method, k_routes[i].method) == 0
  ) {
    k_routes[i].fn(client, id, params);
    yyjson_doc_free(doc); return;
  }

  if (id != 0) inspector_send_empty_result(client, id);
  yyjson_doc_free(doc);
}
