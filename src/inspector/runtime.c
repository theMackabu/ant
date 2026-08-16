#include "ant.h"
#include "bind.h"
#include "pool.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"

#include "gc/roots.h"
#include "modules/symbol.h"
#include "silver/engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool inspector_is_remote_handle_value(ant_value_t value) {
  return is_object_type(value);
}

static uint32_t inspector_object_handle_id(ant_t *js, ant_value_t value) {
  if (!js || !inspector_is_remote_handle_value(value)) return 0;

  for (inspector_object_handle_t *h = g_inspector.object_handles; h; h = h->next) {
    if (h->value == value) return h->id;
  }

  inspector_object_handle_t *h = calloc(1, sizeof(*h));
  if (!h) return 0;
  h->id = ++g_inspector.next_object_id;
  if (h->id == 0) h->id = ++g_inspector.next_object_id;
  h->value = value;
  if (!gc_push_root(js, &h->value)) {
    free(h);
    return 0;
  }
  h->next = g_inspector.object_handles;
  g_inspector.object_handles = h;
  return h->id;
}

static bool inspector_parse_object_id(const char *object_id, uint32_t *id) {
  if (!object_id || strncmp(object_id, "ant:", 4) != 0) return false;
  char *end = NULL;
  unsigned long parsed = strtoul(object_id + 4, &end, 10);
  if (!end || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) return false;
  if (id) *id = (uint32_t)parsed;
  return true;
}

bool inspector_object_for_id(const char *object_id, ant_value_t *out) {
  uint32_t id = 0;
  if (!inspector_parse_object_id(object_id, &id)) return false;
  for (inspector_object_handle_t *h = g_inspector.object_handles; h; h = h->next) {
    if (h->id == id) {
      if (out) *out = h->value;
      return true;
    }
  }
  return false;
}

static bool inspector_append_object_id(sbuf_t *out, uint32_t id) {
  if (id == 0) return true;
  return sbuf_append(out, ",\"objectId\":\"ant:") &&
         sbuf_appendf(out, "%u", id) &&
         sbuf_append(out, "\"");
}

static const char *inspector_function_name(ant_t *js, ant_value_t value, size_t *len) {
  if (len) *len = 0;
  if (vtype(value) == T_CFUNC) {
    const ant_cfunc_meta_t *meta = js_as_cfunc_meta(value);
    if (!meta || !meta->name) return NULL;
    if (len) *len = strlen(meta->name);
    return meta->name;
  }

  if (vtype(value) != T_FUNC) return NULL;
  ant_value_t name = js_get(js, value, "name");
  if (vtype(name) != T_STR) {
    ant_value_t cfunc = js_get_slot(js_func_obj(value), SLOT_CFUNC);
    return inspector_function_name(js, cfunc, len);
  }
  return js_getstr(js, name, len);
}

static bool inspector_append_function_description(ant_t *js, ant_value_t value, sbuf_t *out) {
  size_t name_len = 0;
  const char *name = inspector_function_name(js, value, &name_len);
  if (!sbuf_append(out, "\"function ")) return false;
  if (name && name_len > 0) {
    if (!sbuf_append_len(out, name, name_len)) return false;
  }
  return sbuf_append(out, "() { [native code] }\"");
}

static const char *inspector_object_tag(ant_t *js, ant_value_t value, size_t *len) {
  if (len) *len = 0;
  if (!js || !inspector_is_remote_handle_value(value)) return NULL;
  ant_value_t tag = js_get_sym(js, value, get_toStringTag_sym());
  if (vtype(tag) != T_STR) return NULL;
  return js_getstr(js, tag, len);
}

static bool inspector_append_preview_value(ant_t *js, ant_value_t value, sbuf_t *out) {
  uint8_t type = vtype(value);
  switch (type) {
    case T_UNDEF:
      return sbuf_append(out, "\"type\":\"undefined\",\"value\":\"undefined\"");
    case T_NULL:
      return sbuf_append(out, "\"type\":\"object\",\"subtype\":\"null\",\"value\":\"null\"");
    case T_BOOL:
      return sbuf_appendf(out, "\"type\":\"boolean\",\"value\":\"%s\"", value == js_true ? "true" : "false");
    case T_NUM: {
      double n = js_getnum(value);
      if (isnan(n)) return sbuf_append(out, "\"type\":\"number\",\"value\":\"NaN\"");
      if (isinf(n)) return sbuf_appendf(out, "\"type\":\"number\",\"value\":\"%sInfinity\"", n < 0 ? "-" : "");
      return sbuf_appendf(out, "\"type\":\"number\",\"value\":\"%.17g\"", n);
    }
    case T_STR: {
      size_t len = 0;
      const char *s = js_getstr(js, value, &len);
      if (!sbuf_append(out, "\"type\":\"string\",\"value\":")) return false;
      return sbuf_json_string_len(out, s, len);
    }
    case T_FUNC: {
      if (!sbuf_append(out, "\"type\":\"function\",\"value\":")) return false;
      return inspector_append_function_description(js, value, out);
    }
    case T_CFUNC: {
      if (!sbuf_append(out, "\"type\":\"function\",\"value\":")) return false;
      return inspector_append_function_description(js, value, out);
    }
    default: {
      if (inspector_is_remote_handle_value(value)) {
        const char *class_name = "Object";
        const char *desc = "Object";
        size_t tag_len = 0;
        const char *tag = inspector_object_tag(js, value, &tag_len);
        const char *subtype = NULL;
        if (value == js_glob(js)) {
          class_name = "global";
          desc = "global";
        } else if (type == T_ARR) {
          class_name = "Array";
          desc = "Array";
          subtype = "array";
        } else if (type == T_PROMISE) {
          class_name = "Promise";
          desc = "Promise";
          subtype = "promise";
        } else if (type == T_GENERATOR) {
          class_name = "Generator";
          desc = "Generator";
          subtype = "generator";
        } else if (tag && tag_len > 0) {
          if (!sbuf_append(out, "\"type\":\"object\",\"className\":")) return false;
          if (!sbuf_json_string_len(out, tag, tag_len)) return false;
          if (!sbuf_append(out, ",\"value\":")) return false;
          return sbuf_json_string_len(out, tag, tag_len);
        }

        if (!sbuf_append(out, "\"type\":\"object\"")) return false;
        if (subtype) {
          if (!sbuf_append(out, ",\"subtype\":")) return false;
          if (!sbuf_json_string(out, subtype)) return false;
        }
        if (!sbuf_append(out, ",\"className\":")) return false;
        if (!sbuf_json_string(out, class_name)) return false;
        if (!sbuf_append(out, ",\"value\":")) return false;
        return sbuf_json_string(out, desc);
      }

      char buf[128];
      js_cstr_t desc = js_to_cstr(js, value, buf, sizeof(buf));
      bool ok = sbuf_append(out, "\"type\":\"object\",\"value\":") && sbuf_json_string(out, desc.ptr);
      if (desc.needs_free) free((void *)desc.ptr);
      return ok;
    }
  }
}

static bool inspector_append_object_preview(
  ant_t *js,
  ant_value_t value,
  const char *type,
  const char *subtype,
  const char *description,
  sbuf_t *out
) {
  if (!sbuf_append(out, ",\"preview\":{\"type\":")) return false;
  if (!sbuf_json_string(out, type ? type : "object")) return false;
  if (subtype) {
    if (!sbuf_append(out, ",\"subtype\":")) return false;
    if (!sbuf_json_string(out, subtype)) return false;
  }
  if (!sbuf_append(out, ",\"description\":")) return false;
  if (!sbuf_json_string(out, description ? description : "Object")) return false;
  if (!sbuf_append(out, ",\"overflow\":")) return false;

  ant_iter_t iter = js_prop_iter_begin(js, value);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t prop = js_mkundef();
  size_t count = 0;
  const size_t limit = 8;
  bool overflow = false;
  sbuf_t props = {0};
  if (!sbuf_append(&props, "[")) {
    js_prop_iter_end(&iter);
    return false;
  }

  while (js_prop_iter_next(&iter, &key, &key_len, &prop)) {
    if (count >= limit) {
      overflow = true;
      break;
    }
    if (count > 0 && !sbuf_append(&props, ",")) {
      js_prop_iter_end(&iter);
      free(props.data);
      return false;
    }
    if (!sbuf_append(&props, "{\"name\":")) {
      js_prop_iter_end(&iter);
      free(props.data);
      return false;
    }
    if (!sbuf_json_string_len(&props, key, key_len)) {
      js_prop_iter_end(&iter);
      free(props.data);
      return false;
    }
    if (!sbuf_append(&props, ",")) {
      js_prop_iter_end(&iter);
      free(props.data);
      return false;
    }
    if (!inspector_append_preview_value(js, prop, &props)) {
      js_prop_iter_end(&iter);
      free(props.data);
      return false;
    }
    if (!sbuf_append(&props, "}")) {
      js_prop_iter_end(&iter);
      free(props.data);
      return false;
    }
    count++;
  }
  js_prop_iter_end(&iter);

  if (!sbuf_append(&props, "]")) {
    free(props.data);
    return false;
  }

  bool ok = sbuf_append(out, overflow ? "true" : "false") &&
            sbuf_append(out, ",\"properties\":") &&
            sbuf_append_len(out, props.data, props.len) &&
            sbuf_append(out, "}");
  free(props.data);
  return ok;
}

bool inspector_value_to_remote_object(ant_t *js, ant_value_t value, sbuf_t *out) {
  uint8_t type = vtype(value);
  switch (type) {
    case T_UNDEF:
      return sbuf_append(out, "{\"type\":\"undefined\"}");
    case T_NULL:
      return sbuf_append(out, "{\"type\":\"object\",\"subtype\":\"null\",\"value\":null}");
    case T_BOOL:
      return sbuf_appendf(out, "{\"type\":\"boolean\",\"value\":%s}", value == js_true ? "true" : "false");
    case T_NUM: {
      double n = js_getnum(value);
      if (isnan(n)) return sbuf_append(out, "{\"type\":\"number\",\"unserializableValue\":\"NaN\",\"description\":\"NaN\"}");
      if (isinf(n)) return sbuf_appendf(
        out, "{\"type\":\"number\",\"unserializableValue\":\"%sInfinity\",\"description\":\"%sInfinity\"}",
        n < 0 ? "-" : "", n < 0 ? "-" : ""
      );
      return sbuf_appendf(out, "{\"type\":\"number\",\"value\":%.17g,\"description\":\"%.17g\"}", n, n);
    }
    case T_STR: {
      size_t len = 0;
      const char *s = js_getstr(js, value, &len);
      if (!sbuf_append(out, "{\"type\":\"string\",\"value\":")) return false;
      if (!sbuf_json_string_len(out, s, len)) return false;
      if (!sbuf_append(out, ",\"description\":")) return false;
      if (!sbuf_json_string_len(out, s, len)) return false;
      return sbuf_append(out, "}");
    }
    case T_CFUNC: {
      ant_value_t promoted = js_cfunc_promote(js, value);
      if (vtype(promoted) == T_FUNC) return inspector_value_to_remote_object(js, promoted, out);

      if (!sbuf_append(out, "{\"type\":\"function\",\"className\":\"Function\",\"description\":")) return false;
      if (!inspector_append_function_description(js, value, out)) return false;
      return sbuf_append(out, "}");
    }
    case T_FUNC: {
      uint32_t object_id = inspector_object_handle_id(js, value);
      sbuf_t desc = {0};
      if (!inspector_append_function_description(js, value, &desc)) {
        free(desc.data);
        return false;
      }
      if (!sbuf_append(out, "{\"type\":\"function\",\"className\":\"Function\",\"description\":")) {
        free(desc.data);
        return false;
      }
      if (!sbuf_append_len(out, desc.data, desc.len)) {
        free(desc.data);
        return false;
      }
      if (!inspector_append_object_id(out, object_id)) {
        free(desc.data);
        return false;
      }
      if (desc.len >= 2) desc.data[desc.len - 1] = '\0';
      const char *preview_desc = desc.data ? desc.data + 1 : "function () { [native code] }";
      if (!inspector_append_object_preview(js, value, "function", NULL, preview_desc, out)) {
        free(desc.data);
        return false;
      }
      free(desc.data);
      return sbuf_append(out, "}");
    }
    default: {
      if (inspector_is_remote_handle_value(value)) {
        const char *class_name = "Object";
        const char *desc = "Object";
        size_t tag_len = 0;
        const char *tag = inspector_object_tag(js, value, &tag_len);
        const char *subtype = NULL;
        if (value == js_glob(js)) {
          class_name = "global";
          desc = "global";
        } else if (type == T_ARR) {
          class_name = "Array";
          desc = "Array";
          subtype = "array";
        } else if (type == T_PROMISE) {
          class_name = "Promise";
          desc = "Promise";
          subtype = "promise";
        } else if (type == T_GENERATOR) {
          class_name = "Generator";
          desc = "Generator";
          subtype = "generator";
        } else if (tag && tag_len > 0) {
          class_name = NULL;
          desc = NULL;
        }

        uint32_t object_id = inspector_object_handle_id(js, value);
        if (!sbuf_append(out, "{\"type\":\"object\"")) return false;
        if (subtype) {
          if (!sbuf_append(out, ",\"subtype\":")) return false;
          if (!sbuf_json_string(out, subtype)) return false;
        }
        if (!sbuf_append(out, ",\"className\":")) return false;
        if (class_name) {
          if (!sbuf_json_string(out, class_name)) return false;
        } else if (!sbuf_json_string_len(out, tag, tag_len)) return false;
        if (!sbuf_append(out, ",\"description\":")) return false;
        if (desc) {
          if (!sbuf_json_string(out, desc)) return false;
        } else if (!sbuf_json_string_len(out, tag, tag_len)) return false;
        if (!inspector_append_object_id(out, object_id)) return false;
        const char *preview_desc = desc;
        char tag_buf[64];
        if (!preview_desc && tag && tag_len > 0) {
          size_t copy_len = tag_len < sizeof(tag_buf) - 1 ? tag_len : sizeof(tag_buf) - 1;
          memcpy(tag_buf, tag, copy_len);
          tag_buf[copy_len] = '\0';
          preview_desc = tag_buf;
        }
        if (!inspector_append_object_preview(js, value, "object", subtype, preview_desc, out)) return false;
        return sbuf_append(out, "}");
      }

      char buf[512];
      js_cstr_t desc = js_to_cstr(js, value, buf, sizeof(buf));
      bool ok = sbuf_append(out, "{\"type\":\"object\",\"description\":") &&
                sbuf_json_string(out, desc.ptr) &&
                sbuf_append(out, "}");
      if (desc.needs_free) free((void *)desc.ptr);
      return ok;
    }
  }
}

bool inspector_append_call_location(ant_t *js, sbuf_t *b) {
  if (!js || !b) return true;

  js_error_site_t saved = js->errsite;
  js_clear_error_site(js);

  const char *filename = NULL;
  int line = 1;
  int column = 1;
  js_get_call_location(js, &filename, &line, &column);
  js->errsite = saved;

  if (!filename || !*filename) return true;
  char *url = inspector_make_script_url(filename);
  if (!url) return false;

  inspector_script_t *script = inspector_script_for_url(url);
  bool ok =
    sbuf_append(b, ",\"stackTrace\":{\"callFrames\":[{\"functionName\":\"\",\"scriptId\":\"") &&
    (script ? sbuf_appendf(b, "%d", script->id) : sbuf_append(b, "")) &&
    sbuf_append(b, "\",\"url\":") &&
    sbuf_json_string(b, url) &&
    sbuf_append(b, ",\"lineNumber\":") &&
    sbuf_appendf(b, "%d", line > 0 ? line - 1 : 0) &&
    sbuf_append(b, ",\"columnNumber\":") &&
    sbuf_appendf(b, "%d", column > 0 ? column - 1 : 0) &&
    sbuf_append(b, "}]},\"lineNumber\":") &&
    sbuf_appendf(b, "%d", line > 0 ? line - 1 : 0) &&
    sbuf_append(b, ",\"columnNumber\":") &&
    sbuf_appendf(b, "%d", column > 0 ? column - 1 : 0);
  free(url);
  return ok;
}

static void inspector_send_side_effect_blocked(inspector_client_t *client, int id) {
  inspector_send_response_obj(
    client,
    id,
    "{\"result\":{\"type\":\"undefined\"},\"exceptionDetails\":{\"exceptionId\":1,\"text\":\"EvalError: Possible side-effect in debug-evaluate\",\"lineNumber\":0,\"columnNumber\":0,\"exception\":{\"type\":\"object\",\"subtype\":\"error\",\"className\":\"EvalError\",\"description\":\"EvalError: Possible side-effect in debug-evaluate\"}}}"
  );
}

void inspector_clear_console_events(void) {
  while (g_inspector.console_events_head) {
    inspector_console_event_t *event = g_inspector.console_events_head;
    g_inspector.console_events_head = event->next;
    free(event->json);
    free(event);
  }
  g_inspector.console_events_tail = NULL;
  g_inspector.console_event_count = 0;
}

static void inspector_store_console_event(const char *json) {
  if (!json) return;

  inspector_console_event_t *event = calloc(1, sizeof(*event));
  if (!event) return;
  event->json = strdup(json);
  if (!event->json) {
    free(event);
    return;
  }

  if (g_inspector.console_events_tail) g_inspector.console_events_tail->next = event;
  else g_inspector.console_events_head = event;
  g_inspector.console_events_tail = event;
  g_inspector.console_event_count++;

  while (g_inspector.console_event_count > k_inspector_console_event_limit && g_inspector.console_events_head) {
    inspector_console_event_t *old = g_inspector.console_events_head;
    g_inspector.console_events_head = old->next;
    if (g_inspector.console_events_tail == old) g_inspector.console_events_tail = NULL;
    free(old->json);
    free(old);
    g_inspector.console_event_count--;
  }
}

static void inspector_notify_console(const char *json) {
  for (inspector_client_t *c = g_inspector.clients; c; c = c->next) {
    if (c->runtime_enabled || c->console_enabled) inspector_send_ws(c, json);
  }
}

void inspector_replay_console_events(inspector_client_t *client) {
  if (!client || client->console_replayed) return;
  client->console_replayed = true;
  for (inspector_console_event_t *event = g_inspector.console_events_head; event; event = event->next)
    inspector_send_ws(client, event->json);
}

void inspector_clear_exception_state(ant_t *js) {
  if (!js) return;
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
  js->thrown_stack = js_mkundef();
}

static ant_value_t inspector_exception_value(ant_t *js, ant_value_t result) {
  if (js && js->thrown_exists) return js->thrown_value;
  if (vtype(result) == T_ERR && vdata(result) != 0) return mkval(T_OBJ, vdata(result));
  return result;
}

static bool inspector_exception_description(ant_t *js, ant_value_t err, sbuf_t *out) {
  if (!js || !out) return false;
  if (vtype(err) == T_OBJ) {
    ant_value_t stack = js_get(js, err, "stack");
    if (vtype(stack) == T_STR) {
      size_t len = 0;
      const char *s = js_getstr(js, stack, &len);
      return sbuf_json_string_len(out, s, len);
    }

    ant_value_t message = js_get(js, err, "message");
    if (vtype(message) == T_STR) {
      size_t len = 0;
      const char *s = js_getstr(js, message, &len);
      return sbuf_json_string_len(out, s, len);
    }
  }

  char buf[512];
  js_cstr_t desc = js_to_cstr(js, err, buf, sizeof(buf));
  bool ok = sbuf_json_string(out, desc.ptr);
  if (desc.needs_free) free((void *)desc.ptr);
  return ok;
}

static size_t inspector_heap_used(ant_t *js) {
  if (!js) return 0;
  ant_string_pool_stats_t strings = js_string_pool_stats(&js->pool.string);
  ant_pool_stats_t ropes = js_rope_pool_stats(js);
  ant_pool_stats_t symbols = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t permanent = js_pool_stats(&js->pool.permanent);
  ant_pool_stats_t bigints = js_class_pool_stats(&js->pool.bigint);
  return
    js->obj_arena.live_count * js->obj_arena.elem_size +
    js->closure_arena.live_count * js->closure_arena.elem_size +
    js->upvalue_arena.live_count * js->upvalue_arena.elem_size +
    strings.total.used + ropes.used + symbols.used + permanent.used + bigints.used +
    code_arena_get_memory() + parse_arena_get_memory();
}

static size_t inspector_heap_total(ant_t *js) {
  if (!js) return 0;
  ant_string_pool_stats_t strings = js_string_pool_stats(&js->pool.string);
  ant_pool_stats_t ropes = js_rope_pool_stats(js);
  ant_pool_stats_t symbols = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t permanent = js_pool_stats(&js->pool.permanent);
  ant_pool_stats_t bigints = js_class_pool_stats(&js->pool.bigint);
  return
    js->obj_arena.committed +
    js->closure_arena.committed +
    js->upvalue_arena.committed +
    strings.total.capacity + ropes.capacity + symbols.capacity + permanent.capacity + bigints.capacity +
    code_arena_get_memory() + parse_arena_get_memory();
}

void inspector_send_execution_context(inspector_client_t *client) {
  inspector_send_ws(
    client,
    "{\"method\":\"Runtime.executionContextCreated\",\"params\":{\"context\":{\"id\":1,\"origin\":\"\",\"name\":\"Ant\",\"uniqueId\":\"ant-main\",\"auxData\":{\"isDefault\":true}}}}"
  );
}

static void inspector_send_eval_result_kind(
  inspector_client_t *client, int id, ant_value_t result, bool force_exception
) {
  ant_t *js = client->js;
  bool exception = force_exception || is_err(result) || js->thrown_exists;
  ant_value_t exception_value = force_exception
    ? result
    : (exception ? inspector_exception_value(js, result) : js_mkundef());

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"result\":")) goto oom;
  if (!inspector_value_to_remote_object(js, exception ? exception_value : result, &b)) goto oom;
  if (exception) {
    if (!sbuf_append(&b, ",\"exceptionDetails\":{\"exceptionId\":1,\"text\":")) goto oom;
    if (!inspector_exception_description(js, exception_value, &b)) goto oom;
    if (!sbuf_append(&b, ",\"lineNumber\":0,\"columnNumber\":0,\"exception\":")) goto oom;
    if (!inspector_value_to_remote_object(js, exception_value, &b)) goto oom;
    if (!sbuf_append(&b, "}")) goto oom;
  }
  if (!sbuf_append(&b, "}")) goto oom;
  inspector_send_response_obj(client, id, b.data);
  free(b.data);
  inspector_clear_exception_state(js);
  return;

oom:
  free(b.data);
  inspector_clear_exception_state(js);
  inspector_send_error(client, id, -32000, "Out of memory");
}

void inspector_send_eval_result(inspector_client_t *client, int id, ant_value_t result) {
  inspector_send_eval_result_kind(client, id, result, false);
}

static inspector_await_t *inspector_take_pending_await(uint32_t await_id) {
  inspector_await_t **link = &g_inspector.pending_awaits;
  while (*link) {
    inspector_await_t *pending = *link;
    if (pending->id == await_id) {
      *link = pending->next;
      pending->next = NULL;
      return pending;
    }
    link = &pending->next;
  }
  return NULL;
}

static uint32_t inspector_current_await_id(ant_t *js) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  if (vtype(state) != T_NUM) return 0;
  double id = js_getnum(state);
  return id > 0 && id <= UINT32_MAX ? (uint32_t)id : 0;
}

static ant_value_t inspector_await_fulfilled(ant_t *js, ant_value_t *args, int nargs) {
  inspector_await_t *pending = inspector_take_pending_await(inspector_current_await_id(js));
  if (!pending) return js_mkundef();

  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  for (inspector_client_t *client = g_inspector.clients; client; client = client->next) {
    if (client->id == pending->client_id) {
      inspector_send_eval_result_kind(client, pending->request_id, value, false);
      break;
    }
  }
  free(pending);
  return js_mkundef();
}

static ant_value_t inspector_await_rejected(ant_t *js, ant_value_t *args, int nargs) {
  inspector_await_t *pending = inspector_take_pending_await(inspector_current_await_id(js));
  if (!pending) return js_mkundef();

  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  for (inspector_client_t *client = g_inspector.clients; client; client = client->next) {
    if (client->id == pending->client_id) {
      inspector_send_eval_result_kind(client, pending->request_id, reason, true);
      break;
    }
  }
  free(pending);
  return js_mkundef();
}

static bool inspector_defer_eval_result(
  inspector_client_t *client, int request_id, ant_value_t *result
) {
  ant_t *js = client->js;
  ant_value_t promise = js_promise_assimilate_awaitable(js, *result);
  *result = promise;
  if (is_err(promise) || js->thrown_exists || vtype(promise) != T_PROMISE) return false;

  inspector_await_t *pending = calloc(1, sizeof(*pending));
  if (!pending) {
    inspector_send_error(client, request_id, -32000, "Out of memory");
    return true;
  }

  pending->id = ++g_inspector.next_await_id;
  if (pending->id == 0) pending->id = ++g_inspector.next_await_id;
  pending->client_id = client->id;
  pending->request_id = request_id;
  pending->next = g_inspector.pending_awaits;
  g_inspector.pending_awaits = pending;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  ant_value_t state = js_mknum((double)pending->id);
  ant_value_t fulfilled = js_heavy_mkfun(js, inspector_await_fulfilled, state);
  GC_ROOT_PIN(js, fulfilled);
  ant_value_t rejected = js_heavy_mkfun(js, inspector_await_rejected, state);
  GC_ROOT_PIN(js, rejected);
  ant_value_t chained = js_promise_then(js, promise, fulfilled, rejected);
  GC_ROOT_PIN(js, chained);

  if (is_err(fulfilled) || is_err(rejected) || is_err(chained) || js->thrown_exists) {
    inspector_await_t *failed = inspector_take_pending_await(pending->id);
    free(failed);
    GC_ROOT_RESTORE(js, root_mark);
    inspector_clear_exception_state(js);
    inspector_send_error(client, request_id, -32000, "Unable to await promise");
    return true;
  }

  js_mark_promise_rejection_handled_chain(js, chained);
  GC_ROOT_RESTORE(js, root_mark);
  return true;
}

void inspector_send_eval_completion(
  inspector_client_t *client, int id, ant_value_t result, bool await_promise
) {
  bool deferred = await_promise && !is_err(result) && !client->js->thrown_exists &&
    inspector_defer_eval_result(client, id, &result);
  if (!deferred) inspector_send_eval_result(client, id, result);
}

void inspector_await_promise(inspector_client_t *client, int id, yyjson_val *params) {
  yyjson_val *object_id_val = params ? yyjson_obj_get(params, "promiseObjectId") : NULL;
  const char *object_id = object_id_val && yyjson_is_str(object_id_val)
    ? yyjson_get_str(object_id_val)
    : NULL;
  ant_value_t promise = js_mkundef();
  if (!inspector_object_for_id(object_id, &promise) || vtype(promise) != T_PROMISE) {
    inspector_send_error(client, id, -32602, "Runtime.awaitPromise requires a promise objectId");
    return;
  }
  inspector_send_eval_completion(client, id, promise, true);
}

void inspector_cancel_client_awaits(uint64_t client_id) {
  inspector_await_t **link = &g_inspector.pending_awaits;
  while (*link) {
    inspector_await_t *pending = *link;
    if (pending->client_id == client_id) {
      *link = pending->next;
      free(pending);
    } else link = &pending->next;
  }
}

void inspector_clear_pending_awaits(void) {
  while (g_inspector.pending_awaits) {
    inspector_await_t *pending = g_inspector.pending_awaits;
    g_inspector.pending_awaits = pending->next;
    free(pending);
  }
}

void inspector_eval(inspector_client_t *client, int id, yyjson_val *params) {
  yyjson_val *expr_val = params ? yyjson_obj_get(params, "expression") : NULL;
  if (!expr_val || !yyjson_is_str(expr_val)) {
    inspector_send_error(client, id, -32602, "Runtime.evaluate requires expression");
    return;
  }

  const char *expr = yyjson_get_str(expr_val);
  size_t expr_len = yyjson_get_len(expr_val);

  const char *prev_filename = client->js->filename;
  inspector_clear_exception_state(client->js);

  if (inspector_param_bool(params, "throwOnSideEffect")) {
    ant_value_t safe_result = js_mkundef();
    if (inspector_eval_safe_expr(client->js, expr, expr_len, &safe_result)) {
      inspector_send_eval_result(client, id, safe_result);
    } else inspector_send_side_effect_blocked(client, id);
    return;
  }

  js_set_filename(client->js, "[inspector]");
  js_eval_result_t evaluation = js_eval_bytecode_repl(client->js, expr, expr_len);
  js_set_filename(client->js, prev_filename);
  
  inspector_send_eval_completion(
    client, id, evaluation.value, 
    inspector_param_bool(params, "awaitPromise")
  );
}

void inspector_get_heap_usage(inspector_client_t *client, int id) {
  sbuf_t b = {0};
  if (sbuf_appendf(
    &b, "{\"usedSize\":%zu,\"totalSize\":%zu}",
    inspector_heap_used(client->js),
    inspector_heap_total(client->js)
  )) inspector_send_response_obj(client, id, b.data);
  else inspector_send_error(client, id, -32000, "Out of memory");
  free(b.data);
}

void inspector_get_properties(inspector_client_t *client, int id, yyjson_val *params) {
  yyjson_val *object_id_val = params ? yyjson_obj_get(params, "objectId") : NULL;
  const char *object_id = object_id_val && yyjson_is_str(object_id_val) ? yyjson_get_str(object_id_val) : NULL;
  bool own_only = inspector_param_bool(params, "ownProperties");
  ant_value_t object = js_mkundef();
  if (!inspector_object_for_id(object_id, &object)) {
    inspector_send_error(client, id, -32000, "Could not find object with given id");
    return;
  }

  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"result\":[")) goto oom;

  bool first = true;
  for (ant_value_t cur = object, depth = 0; inspector_is_remote_handle_value(cur) && depth < 64; depth++) {
    ant_iter_t iter = js_prop_iter_begin(client->js, cur);
    const char *key = NULL;
    size_t key_len = 0;
    ant_value_t value = js_mkundef();
    while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
      if (!first && !sbuf_append(&b, ",")) {
        js_prop_iter_end(&iter);
        goto oom;
      }
      first = false;
      if (!sbuf_append(&b, "{\"name\":")) {
        js_prop_iter_end(&iter);
        goto oom;
      }
      if (!sbuf_json_string_len(&b, key, key_len)) {
        js_prop_iter_end(&iter);
        goto oom;
      }
      if (!sbuf_append(&b, ",\"enumerable\":true,\"configurable\":true,\"writable\":true,\"isOwn\":")) {
        js_prop_iter_end(&iter);
        goto oom;
      }
      if (!sbuf_append(&b, depth == 0 ? "true" : "false")) {
        js_prop_iter_end(&iter);
        goto oom;
      }
      if (!sbuf_append(&b, ",\"value\":")) {
        js_prop_iter_end(&iter);
        goto oom;
      }
      if (!inspector_value_to_remote_object(client->js, value, &b)) {
        js_prop_iter_end(&iter);
        goto oom;
      }
      if (!sbuf_append(&b, "}")) {
        js_prop_iter_end(&iter);
        goto oom;
      }
    }
    js_prop_iter_end(&iter);
    if (own_only) break;
    ant_value_t proto = js_get_proto(client->js, cur);
    if (!inspector_is_remote_handle_value(proto)) break;
    cur = proto;
  }

  if (!sbuf_append(&b, "],\"internalProperties\":[")) goto oom;
  ant_value_t proto = js_get_proto(client->js, object);
  if (inspector_is_remote_handle_value(proto) || vtype(proto) == T_NULL) {
    if (!sbuf_append(&b, "{\"name\":\"[[Prototype]]\",\"value\":")) goto oom;
    if (!inspector_value_to_remote_object(client->js, proto, &b)) goto oom;
    if (!sbuf_append(&b, "}")) goto oom;
  }
  if (!sbuf_append(&b, "]}")) goto oom;
  inspector_send_response_obj(client, id, b.data);
  free(b.data);
  return;

oom:
  free(b.data);
  inspector_send_error(client, id, -32000, "Out of memory");
}

void inspector_global_lexical_scope_names(inspector_client_t *client, int id) {
  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"names\":[")) goto oom;

  bool first = true;
  ant_iter_t iter = js_prop_iter_begin(client->js, js_glob(client->js));
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = js_mkundef();
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    (void)value;
    if (!first && !sbuf_append(&b, ",")) {
      js_prop_iter_end(&iter);
      goto oom;
    }
    first = false;
    if (!sbuf_json_string_len(&b, key, key_len)) {
      js_prop_iter_end(&iter);
      goto oom;
    }
  }
  js_prop_iter_end(&iter);

  if (!sbuf_append(&b, "]}")) goto oom;
  inspector_send_response_obj(client, id, b.data);
  free(b.data);
  return;

oom:
  free(b.data);
  inspector_send_error(client, id, -32000, "Out of memory");
}

void ant_inspector_console_api_called(ant_t *js, const char *level, ant_value_t *args, int nargs) {
  if (!g_inspector.started) return;
  sbuf_t b = {0};
  if (!sbuf_append(&b, "{\"method\":\"Runtime.consoleAPICalled\",\"params\":{\"type\":")) goto done;
  if (!sbuf_json_string(&b, level ? level : "log")) goto done;
  if (!sbuf_append(&b, ",\"args\":[")) goto done;
  for (int i = 0; i < nargs; i++) {
    if (i && !sbuf_append(&b, ",")) goto done;
    if (!inspector_value_to_remote_object(js, args[i], &b)) goto done;
  }
  if (!sbuf_appendf(&b, "],\"executionContextId\":1,\"timestamp\":%.3f", (double)uv_hrtime() / 1000000.0)) goto done;
  if (!inspector_append_call_location(js, &b)) goto done;
  if (!sbuf_append(&b, "}}")) goto done;
  inspector_store_console_event(b.data);
  inspector_notify_console(b.data);
done:
  free(b.data);
}
