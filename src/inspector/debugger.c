#include <compat.h> // IWYU pragma: keep

#include "ant.h"
#include "bind.h"
#include "reactor.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool inspector_source_end_position(const char *source, size_t len, int *line, int *column) {
  int cur_line = 0;
  int cur_column = 0;
  
  for (size_t i = 0; i < len; i++) {
    if (source[i] == '\n') {
      cur_line++;
      cur_column = 0;
    } else cur_column++;
  }
  
  if (line) *line = cur_line;
  if (column) *column = cur_column;
  
  return true;
}

bool inspector_is_url_like(const char *path) {
  if (!path) return false;
  for (const char *p = path; *p; p++) {
    if (*p == ':') return p[1] == '/' && p[2] == '/';
    if (*p == '/' || *p == '[') return false;
  }
  return false;
}

char *inspector_make_script_url(const char *path) {
  if (!path || !*path) return strdup("ant://inspector/script");
  if (strcmp(path, "[eval]") == 0) return strdup("ant://inspector/eval");
  if (strcmp(path, "[stdin]") == 0) return strdup("ant://inspector/stdin");
  if (strcmp(path, "[inspector]") == 0) return strdup("ant://inspector/console");
  if (inspector_is_url_like(path)) return strdup(path);

  sbuf_t b = {0};
  if (path[0] == '/') {
    if (!sbuf_append(&b, "file://") || !sbuf_append(&b, path)) {
      free(b.data);
      return NULL;
    }
    return b.data;
  }

  char *real = realpath(path, NULL);
  if (real) {
    bool ok = sbuf_append(&b, "file://") && sbuf_append(&b, real);
    free(real);
    if (!ok) {
      free(b.data);
      return NULL;
    }
    return b.data;
  }

  if (!sbuf_append(&b, "file:///") || !sbuf_append(&b, path)) {
    free(b.data);
    return NULL;
  }
  return b.data;
}

inspector_script_t *inspector_script_for_id(int id) {
  if (id <= 0) return NULL;
  for (inspector_script_t *script = g_inspector.scripts; script; script = script->next) {
    if (script->id == id) return script;
  }
  return NULL;
}

inspector_script_t *inspector_script_for_url(const char *url) {
  if (!url) return NULL;
  for (inspector_script_t *script = g_inspector.scripts; script; script = script->next) {
    if (script->url && strcmp(script->url, url) == 0) return script;
  }
  return NULL;
}

inspector_script_t *inspector_entry_script(void) {
  inspector_script_t *script = inspector_script_for_id(g_inspector.entry_script_id);
  if (script) return script;
  return g_inspector.scripts;
}

static bool inspector_send_script_parsed(inspector_client_t *client, const inspector_script_t *script) {
  if (!client || !script) return false;
  int end_line = 0;
  int end_column = 0;
  inspector_source_end_position(script->source, script->source_len, &end_line, &end_column);

  sbuf_t b = {0};
  bool ok =
    sbuf_append(&b, "{\"method\":\"Debugger.scriptParsed\",\"params\":{\"scriptId\":\"") &&
    sbuf_appendf(&b, "%d", script->id) &&
    sbuf_append(&b, "\",\"url\":") &&
    sbuf_json_string(&b, script->url ? script->url : "") &&
    sbuf_append(&b, ",\"startLine\":0,\"startColumn\":0,\"endLine\":") &&
    sbuf_appendf(&b, "%d", end_line) &&
    sbuf_append(&b, ",\"endColumn\":") &&
    sbuf_appendf(&b, "%d", end_column) &&
    sbuf_append(&b, ",\"executionContextId\":1,\"hash\":\"\",\"isLiveEdit\":false,\"sourceMapURL\":\"\",\"hasSourceURL\":false,\"isModule\":") &&
    sbuf_append(&b, script->is_module ? "true" : "false") &&
    sbuf_append(&b, ",\"length\":") &&
    sbuf_appendf(&b, "%zu", script->source_len) &&
    sbuf_append(&b, "}}");
  if (ok) inspector_send_ws(client, b.data);
  free(b.data);
  return ok;
}

static void inspector_notify_script_parsed(const inspector_script_t *script) {
  for (inspector_client_t *client = g_inspector.clients; client; client = client->next)
    if (client->debugger_enabled) inspector_send_script_parsed(client, script);
}

static inspector_script_t *inspector_register_script_source(
  const char *url,
  const char *source,
  size_t len,
  bool is_module,
  bool notify
) {
  if (!url) return NULL;

  char *source_copy = malloc(len + 1);
  if (!source_copy) return NULL;
  if (source && len > 0) memcpy(source_copy, source, len);
  source_copy[len] = '\0';

  inspector_script_t *script = inspector_script_for_url(url);
  if (script) {
    free(script->source);
    script->source = source_copy;
    script->source_len = len;
    script->is_module = is_module;
  } else {
    script = calloc(1, sizeof(*script));
    if (!script) {
      free(source_copy);
      return NULL;
    }
    script->url = strdup(url);
    if (!script->url) {
      free(source_copy);
      free(script);
      return NULL;
    }
    script->source = source_copy;
    script->source_len = len;
    script->is_module = is_module;
    script->id = ++g_inspector.next_script_id;
    if (script->id <= 0) script->id = ++g_inspector.next_script_id;
    if (g_inspector.entry_script_id == 0) g_inspector.entry_script_id = script->id;
    script->next = g_inspector.scripts;
    g_inspector.scripts = script;
  }

  if (notify) inspector_notify_script_parsed(script);
  return script;
}

void inspector_send_registered_scripts(inspector_client_t *client) {
  for (inspector_script_t *script = g_inspector.scripts; script; script = script->next)
    inspector_send_script_parsed(client, script);
}

static void inspector_eval_source(inspector_client_t *client, int id, const char *source, size_t source_len) {
  const char *prev_filename = client->js->filename;
  inspector_clear_exception_state(client->js);
  js_set_filename(client->js, "[inspector]");
  ant_value_t result = js_eval_bytecode_repl(client->js, source ? source : "", source_len);
  js_reactor_pump_repl_nowait(client->js);
  js_set_filename(client->js, prev_filename);
  inspector_send_eval_result(client, id, result);
}

void inspector_compile_script(inspector_client_t *client, int id, yyjson_val *params) {
  yyjson_val *expr_val = params ? yyjson_obj_get(params, "expression") : NULL;
  if (!expr_val || !yyjson_is_str(expr_val)) {
    inspector_send_error(client, id, -32602, "Runtime.compileScript requires expression");
    return;
  }

  const char *expr = yyjson_get_str(expr_val);
  size_t expr_len = yyjson_get_len(expr_val);
  yyjson_val *source_url_val = params ? yyjson_obj_get(params, "sourceURL") : NULL;
  const char *source_url = source_url_val && yyjson_is_str(source_url_val) ? yyjson_get_str(source_url_val) : NULL;

  char fallback_url[96];
  snprintf(fallback_url, sizeof(fallback_url), "ant://inspector/compiled/%d", g_inspector.next_script_id + 1);
  inspector_script_t *script = inspector_register_script_source(
    source_url && *source_url ? source_url : fallback_url,
    expr, expr_len,
    false, true
  );
  
  if (!script) {
    inspector_send_error(client, id, -32000, "Out of memory");
    return;
  }

  sbuf_t b = {0};
  if (sbuf_appendf(&b, "{\"scriptId\":\"%d\"}", script->id))
    inspector_send_response_obj(client, id, b.data);
  else inspector_send_error(client, id, -32000, "Out of memory");
  free(b.data);
}

void inspector_run_script(inspector_client_t *client, int id, yyjson_val *params) {
  yyjson_val *script_id_val = params ? yyjson_obj_get(params, "scriptId") : NULL;
  const char *script_id = script_id_val && yyjson_is_str(script_id_val) ? yyjson_get_str(script_id_val) : NULL;
  
  inspector_script_t *script = script_id ? inspector_script_for_id(atoi(script_id)) : NULL;
  if (!script) {
    inspector_send_error(client, id, -32000, "No such script");
    return;
  }
  
  inspector_eval_source(client, id, script->source, script->source_len);
}

void inspector_get_script_source(inspector_client_t *client, int id, yyjson_val *params) {
  yyjson_val *script_id_val = params ? yyjson_obj_get(params, "scriptId") : NULL;
  const char *script_id = script_id_val && yyjson_is_str(script_id_val) ? yyjson_get_str(script_id_val) : NULL;
  inspector_script_t *script = script_id ? inspector_script_for_id(atoi(script_id)) : NULL;
  if (!script) {
    inspector_send_error(client, id, -32000, "No such script");
    return;
  }

  sbuf_t b = {0};
  if (
    sbuf_append(&b, "{\"scriptSource\":") &&
    sbuf_json_string_len(&b, script->source ? script->source : "", script->source ? script->source_len : 0) &&
    sbuf_append(&b, "}")
  ) inspector_send_response_obj(client, id, b.data);
  else inspector_send_error(client, id, -32000, "Out of memory");
  free(b.data);
}
void ant_inspector_register_script_source(
  const char *path,
  const char *source,
  size_t len,
  bool is_module
) {
  if (!g_inspector.started) return;
  char *url = inspector_make_script_url(path);
  if (!url) return;
  inspector_register_script_source(url, source ? source : "", source ? len : 0, is_module, true);
  free(url);
}

void ant_inspector_register_script_file(const char *path, bool is_module) {
  if (!g_inspector.started || !path || !*path) return;

  FILE *fp = fopen(path, "rb");
  if (!fp) return;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return;
  }
  long size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return;
  }

  char *source = malloc((size_t)size + 1);
  if (!source) {
    fclose(fp);
    return;
  }
  
  size_t len = fread(source, 1, (size_t)size, fp);
  fclose(fp);
  source[len] = '\0';
  
  ant_inspector_register_script_source(path, source, len, is_module);
  free(source);
}
