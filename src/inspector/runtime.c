#include "ant.h"
#include "bind.h"
#include "pool.h"
#include "cfunc.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"

#include "gc/roots.h"
#include "gc/object_arenas.h"
#include "silver/engine.h"
#include "silver/eval_env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t inspector_object_handle_id(ant_t *js, ant_value_t value) {
  if (!js || !is_object_type(value)) return 0;

  for (inspector_object_handle_t *h = g_inspector.object_handles; h; h = h->next)
    if (h->value == value) return h->id;

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

static bool inspector_object_for_id(const char *object_id, ant_value_t *out) {
  uint32_t id = 0;
  if (!inspector_parse_object_id(object_id, &id)) return false;
  for (inspector_object_handle_t *h = g_inspector.object_handles; h; h = h->next) if (h->id == id) {
    if (out) *out = h->value;
    return true;
  }
  return false;
}

static const char *inspector_function_name(ant_t *js, ant_value_t value, size_t *len) {
  if (len) *len = 0;
  if (vtype(value) == kTypeBuiltin) {
    const ant_cfunc_meta_t *meta = js_as_cfunc_meta(value);
    if (!meta || !meta->name) return NULL;
    if (len) *len = strlen(meta->name);
    return meta->name;
  }

  if (vtype(value) != kTypeFunction) return NULL;
  ant_value_t name = js_get(js, value, "name");
  if (vtype(name) != kTypeString) {
    ant_value_t cfunc = js_get_slot(js_func_obj(value), SLOT_CFUNC);
    return inspector_function_name(js, cfunc, len);
  }
  return js_getstr(js, name, len);
}

static const char *inspector_object_tag(ant_t *js, ant_value_t value, size_t *len) {
  ant_value_t tag = js_get_sym(js, value, js->sym.toStringTag_sym);
  if (vtype(tag) != kTypeString) return NULL;
  return js_getstr(js, tag, len);
}

static char *inspector_function_description(ant_t *js, ant_value_t value, size_t *len) {
  static const char prefix[] = "function ";
  static const char suffix[] = "() { [native code] }";
  size_t name_len = 0;
  const char *name = inspector_function_name(js, value, &name_len);
  if (!name) name_len = 0;

  char *out = malloc(sizeof(prefix) + name_len + sizeof(suffix));
  if (!out) return NULL;
  memcpy(out, prefix, sizeof(prefix) - 1);
  if (name_len) memcpy(out + sizeof(prefix) - 1, name, name_len);
  memcpy(out + sizeof(prefix) - 1 + name_len, suffix, sizeof(suffix));
  *len = sizeof(prefix) - 1 + name_len + sizeof(suffix) - 1;
  return out;
}

typedef struct {
  const char *type;
  const char *subtype;
  const char *class_name;
  const char *desc;
  size_t class_len;
  size_t desc_len;
} inspector_mirror_t;

static void inspector_mirror(ant_t *js, yyjson_mut_doc *doc, ant_value_t value, inspector_mirror_t *m) {
  uint8_t type = vtype(value);
  const char *desc = NULL;
  size_t desc_len = SIZE_MAX;
  bool tagged = false;
  char *owned = NULL;
  char buf[128];
  js_cstr_t cstr = {0};
  *m = (inspector_mirror_t){.type = "object"};

  switch (type) {
    case kTypeUndefined:
      m->type = desc = "undefined";
      break;
    case kTypeNull:
      m->subtype = desc = "null";
      break;
    case kTypeBool:
      m->type = "boolean";
      desc = value == Ant_Value_True ? "true" : "false";
      break;
    case kTypeNumber: {
      double n = Ant_Value_AsNumberUnchecked(value);
      m->type = "number";
      if (isnan(n)) desc = "NaN";
      else if (isinf(n)) desc = n < 0 ? "-Infinity" : "Infinity";
      else {
        snprintf(buf, sizeof(buf), "%.17g", n);
        desc = buf;
      }
      break;
    }
    case kTypeString:
      m->type = "string";
      desc = js_getstr(js, value, &desc_len);
      break;
    case kTypeFunction:
    case kTypeBuiltin:
      m->type = "function";
      desc = owned = inspector_function_description(js, value, &desc_len);
      break;
    default:
      if (!is_object_type(value)) {
        cstr = js_to_cstr(js, value, buf, sizeof(buf));
        desc = cstr.ptr;
        break;
      }
      m->class_name = "Object";
      if (value == js_glob(js)) m->class_name = "global";
      else if (type == kTypeArray) m->class_name = "Array", m->subtype = "array";
      else if (type == kTypePromise) m->class_name = "Promise", m->subtype = "promise";
      else if (type == kTypeGenerator) m->class_name = "Generator", m->subtype = "generator";
      else {
        size_t tag_len = 0;
        const char *tag = inspector_object_tag(js, value, &tag_len);
        if (tag && tag_len > 0) {
          desc = tag;
          desc_len = tag_len;
          tagged = true;
        }
      }
      if (!tagged) desc = m->class_name;
      break;
  }

  if (!desc) desc = "", desc_len = 0;
  if (desc_len == SIZE_MAX) desc_len = strlen(desc);

  yyjson_mut_val *copy = yyjson_mut_strncpy(doc, desc, desc_len);
  m->desc = yyjson_mut_get_str(copy);
  m->desc_len = yyjson_mut_get_len(copy);
  if (tagged) m->class_name = m->desc, m->class_len = m->desc_len;
  else if (m->class_name) m->class_len = strlen(m->class_name);

  free(owned);
  if (cstr.needs_free) free((void *)cstr.ptr);
}

static void inspector_add_mirror_type(yyjson_mut_doc *doc, yyjson_mut_val *obj, const inspector_mirror_t *m) {
  yyjson_mut_obj_add_str(doc, obj, "type", m->type);
  if (m->subtype) yyjson_mut_obj_add_str(doc, obj, "subtype", m->subtype);
}

static void inspector_add_preview(
  ant_t *js, yyjson_mut_doc *doc, yyjson_mut_val *obj,
  ant_value_t value, const inspector_mirror_t *m
) {
  yyjson_mut_val *preview = yyjson_mut_obj_add_obj(doc, obj, "preview");
  yyjson_mut_val *props = yyjson_mut_arr(doc);
  const size_t limit = 8;
  size_t count = 0;
  bool overflow = false;

  ant_iter_t iter = js_prop_iter_begin(js, value);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t prop = Ant_Value_Undefined;

  while (js_prop_iter_next(&iter, &key, &key_len, &prop)) {
    if (count++ >= limit) {
      overflow = true;
      break;
    }
    inspector_mirror_t pm;
    yyjson_mut_val *entry = yyjson_mut_arr_add_obj(doc, props);
    yyjson_mut_obj_add_strncpy(doc, entry, "name", key, key_len);
    inspector_mirror(js, doc, prop, &pm);
    inspector_add_mirror_type(doc, entry, &pm);
    if (pm.class_name) yyjson_mut_obj_add_strn(doc, entry, "className", pm.class_name, pm.class_len);
    yyjson_mut_obj_add_strn(doc, entry, "value", pm.desc, pm.desc_len);
  }
  js_prop_iter_end(&iter);

  inspector_add_mirror_type(doc, preview, m);
  yyjson_mut_obj_add_strn(doc, preview, "description", m->desc, m->desc_len);
  yyjson_mut_obj_add_bool(doc, preview, "overflow", overflow);
  yyjson_mut_obj_add_val(doc, preview, "properties", props);
}

static yyjson_mut_val *inspector_remote_object(ant_t *js, yyjson_mut_doc *doc, ant_value_t value) {
  if (vtype(value) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, value);
    if (vtype(promoted) == kTypeFunction) value = promoted;
  }

  uint8_t type = vtype(value);
  inspector_mirror_t m;
  inspector_mirror(js, doc, value, &m);

  yyjson_mut_val *obj = yyjson_mut_obj(doc);
  inspector_add_mirror_type(doc, obj, &m);

  switch (type) {
    case kTypeUndefined:
      return obj;
    case kTypeNull:
      yyjson_mut_obj_add_null(doc, obj, "value");
      return obj;
    case kTypeBool:
      yyjson_mut_obj_add_bool(doc, obj, "value", value == Ant_Value_True);
      return obj;
    case kTypeNumber: {
      double n = Ant_Value_AsNumberUnchecked(value);
      if (isfinite(n)) yyjson_mut_obj_add_real(doc, obj, "value", n);
      else yyjson_mut_obj_add_strn(doc, obj, "unserializableValue", m.desc, m.desc_len);
      break;
    }
    case kTypeString:
      yyjson_mut_obj_add_strn(doc, obj, "value", m.desc, m.desc_len);
      break;
    case kTypeFunction:
    case kTypeBuiltin:
      yyjson_mut_obj_add_str(doc, obj, "className", "Function");
      break;
    default:
      if (m.class_name) yyjson_mut_obj_add_strn(doc, obj, "className", m.class_name, m.class_len);
      break;
  }

  yyjson_mut_obj_add_strn(doc, obj, "description", m.desc, m.desc_len);
  if (type == kTypeBuiltin) return obj;
  if (type != kTypeFunction && !is_object_type(value)) return obj;

  uint32_t object_id = inspector_object_handle_id(js, value);
  if (object_id != 0) {
    char id[16];
    snprintf(id, sizeof(id), "ant:%u", object_id);
    yyjson_mut_obj_add_strcpy(doc, obj, "objectId", id);
  }
  inspector_add_preview(js, doc, obj, value, &m);
  return obj;
}

static void inspector_add_call_location(ant_t *js, yyjson_mut_doc *doc, yyjson_mut_val *params) {
  const char *filename = NULL;
  int line = 1;
  int column = 1;
  js_get_call_location(js, &filename, &line, &column);

  if (!filename || !*filename) return;
  char *url = inspector_make_script_url(filename);
  if (!url) return;

  char script_id[16] = "";
  inspector_script_t *script = inspector_script_for_url(url);
  if (script) snprintf(script_id, sizeof(script_id), "%d", script->id);
  line = line > 0 ? line - 1 : 0;
  column = column > 0 ? column - 1 : 0;

  yyjson_mut_val *stack = yyjson_mut_obj_add_obj(doc, params, "stackTrace");
  yyjson_mut_val *frames = yyjson_mut_obj_add_arr(doc, stack, "callFrames");
  yyjson_mut_val *frame = yyjson_mut_arr_add_obj(doc, frames);
  yyjson_mut_obj_add_str(doc, frame, "functionName", "");
  yyjson_mut_obj_add_strcpy(doc, frame, "scriptId", script_id);
  yyjson_mut_obj_add_strcpy(doc, frame, "url", url);
  yyjson_mut_obj_add_int(doc, frame, "lineNumber", line);
  yyjson_mut_obj_add_int(doc, frame, "columnNumber", column);
  yyjson_mut_obj_add_int(doc, params, "lineNumber", line);
  yyjson_mut_obj_add_int(doc, params, "columnNumber", column);
  free(url);
}

static void inspector_send_side_effect_blocked(inspector_client_t *client, int id) {
  inspector_send_response_obj(
    client, id,
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

static void inspector_store_console_event(char *json) {
  inspector_console_event_t *event = json ? calloc(1, sizeof(*event)) : NULL;
  if (!event) {
    free(json);
    return;
  }
  event->json = json;

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

static void inspector_add_exception_text(ant_t *js, yyjson_mut_doc *doc, yyjson_mut_val *obj, ant_value_t err) {
  ant_value_t text = Ant_Value_Undefined;

  if (is_err(err)) {
    text = Ant_Exception_Stack(js, err);
    err = Ant_Exception_Value(js, err);
  } else if (vtype(err) == kTypeObject) text = js_get(js, err, "stack");

  if (vtype(text) != kTypeString && vtype(err) == kTypeObject) text = js_get(js, err, "message");
  if (vtype(text) == kTypeString) {
    size_t len = 0;
    const char *s = js_getstr(js, text, &len);
    yyjson_mut_obj_add_strncpy(doc, obj, "text", s, len);
    return;
  }

  char buf[512];
  js_cstr_t desc = js_to_cstr(js, err, buf, sizeof(buf));
  yyjson_mut_obj_add_strcpy(doc, obj, "text", desc.ptr);
  if (desc.needs_free) free((void *)desc.ptr);
}

static size_t inspector_heap_used(ant_t *js) {
  if (!js) return 0;
  ant_string_pool_stats_t strings = js_string_pool_stats(&js->pool.string);
  ant_pool_stats_t ropes = js_rope_pool_stats(js);
  ant_pool_stats_t symbols = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t permanent = js_pool_stats(&js->pool.permanent);
  ant_pool_stats_t bigints = js_class_pool_stats(&js->pool.bigint);
  return
    ant_object_live_bytes(js) +
    js->closure_arena.live_count * js->closure_arena.elem_size +
    js->upvalue_arena.live_count * js->upvalue_arena.elem_size +
    strings.total.used + ropes.used + symbols.used + permanent.used + bigints.used +
    code_arena_get_memory(js) + parse_arena_get_memory(js);
}

static size_t inspector_heap_total(ant_t *js) {
  if (!js) return 0;
  ant_string_pool_stats_t strings = js_string_pool_stats(&js->pool.string);
  ant_pool_stats_t ropes = js_rope_pool_stats(js);
  ant_pool_stats_t symbols = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t permanent = js_pool_stats(&js->pool.permanent);
  ant_pool_stats_t bigints = js_class_pool_stats(&js->pool.bigint);
  return
    ant_object_committed(js) +
    js->closure_arena.committed +
    js->upvalue_arena.committed +
    strings.total.capacity + ropes.capacity + symbols.capacity + permanent.capacity + bigints.capacity +
    code_arena_get_memory(js) + parse_arena_get_memory(js);
}

void inspector_send_execution_context(inspector_client_t *client) {
  inspector_send_ws(
    client,
    "{\"method\":\"Runtime.executionContextCreated\",\"params\":{\"context\":{\"id\":1,\"origin\":\"\",\"name\":\"Ant\",\"uniqueId\":\"ant-main\",\"auxData\":{\"isDefault\":true}}}}"
  );
}

static void inspector_send_eval_result(
  inspector_client_t *client, int id, ant_value_t result, bool force_exception
) {
  ant_t *js = client->js;
  ant_value_t completion = !force_exception && !is_err(result) && Ant_Exception_Pending(js)
    ? Ant_Exception_Peek(js) : result;
  bool exception = force_exception || is_err(completion);
  ant_value_t exception_value = exception ? Ant_Exception_Value(js, completion) : Ant_Value_Undefined;
  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);

  if (!gc_temp_root_handle_valid(gc_temp_root_add(&roots, completion))) {
    inspector_send_error(client, id, -32000, "Out of memory");
  } else {
    inspector_doc_t d;
    yyjson_mut_doc *doc = inspector_doc_begin(&d);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, res, "result", inspector_remote_object(js, doc, exception ? exception_value : result));
    if (exception) {
      yyjson_mut_val *details = yyjson_mut_obj_add_obj(doc, res, "exceptionDetails");
      yyjson_mut_obj_add_int(doc, details, "exceptionId", 1);
      inspector_add_exception_text(js, doc, details, completion);
      yyjson_mut_obj_add_int(doc, details, "lineNumber", 0);
      yyjson_mut_obj_add_int(doc, details, "columnNumber", 0);
      yyjson_mut_obj_add_val(doc, details, "exception", inspector_remote_object(js, doc, exception_value));
    }
    inspector_send_result(client, id, &d, res);
  }

  Ant_Exception_Clear(js);
  gc_temp_root_scope_end(&roots);
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

static uint32_t inspector_current_await_id(ant_t *js, ant_value_t call_func) {
  ant_value_t state = js_func_data(call_func);
  if (vtype(state) != kTypeNumber) return 0;
  double id = Ant_Value_AsNumberUnchecked(state);
  return id > 0 && id <= UINT32_MAX ? (uint32_t)id : 0;
}

static ant_value_t inspector_await_settled(ant_t *js, ant_value_t call_func, ant_value_t value, bool rejected) {
  inspector_await_t *pending = inspector_take_pending_await(inspector_current_await_id(js, call_func));
  if (!pending) return Ant_Value_Undefined;

  for (inspector_client_t *client = g_inspector.clients; client; client = client->next) {
    if (client->id == pending->client_id) {
      inspector_send_eval_result(client, pending->request_id, value, rejected);
      break;
    }
  }
  free(pending);
  return Ant_Value_Undefined;
}

static ant_value_t inspector_await_fulfilled(ant_params_t) {
  return inspector_await_settled(js, call_func, nargs > 0 ? args[0] : Ant_Value_Undefined, false);
}

static ant_value_t inspector_await_rejected(ant_params_t) {
  return inspector_await_settled(js, call_func, nargs > 0 ? args[0] : Ant_Value_Undefined, true);
}

static bool inspector_defer_eval_result(
  inspector_client_t *client, int request_id, ant_value_t *result
) {
  ant_t *js = client->js;
  ant_value_t promise = js_promise_assimilate_awaitable(js, *result);
  *result = promise;
  if (is_err(promise) || Ant_Exception_Pending(js) || vtype(promise) != kTypePromise) return false;

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
  ant_value_t state = Ant_Value_FromNumber((double)pending->id);
  ant_value_t fulfilled = js_heavy_mkfun(js, inspector_await_fulfilled, state);
  GC_ROOT_PIN(js, fulfilled);
  ant_value_t rejected = js_heavy_mkfun(js, inspector_await_rejected, state);
  GC_ROOT_PIN(js, rejected);
  ant_value_t chained = js_promise_then(js, promise, fulfilled, rejected);
  GC_ROOT_PIN(js, chained);

  if (is_err(fulfilled) || is_err(rejected) || is_err(chained) || Ant_Exception_Pending(js)) {
    inspector_await_t *failed = inspector_take_pending_await(pending->id);
    free(failed);
    GC_ROOT_RESTORE(js, root_mark);
    Ant_Exception_Clear(js);
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
  bool deferred = await_promise && !is_err(result) && !Ant_Exception_Pending(client->js) &&
    inspector_defer_eval_result(client, id, &result);
  if (!deferred) inspector_send_eval_result(client, id, result, false);
}

void inspector_await_promise(inspector_client_t *client, int id, yyjson_val *params) {
  yyjson_val *object_id_val = params ? yyjson_obj_get(params, "promiseObjectId") : NULL;
  const char *object_id = object_id_val && yyjson_is_str(object_id_val)
    ? yyjson_get_str(object_id_val)
    : NULL;
  ant_value_t promise = Ant_Value_Undefined;
  if (!inspector_object_for_id(object_id, &promise) || vtype(promise) != kTypePromise) {
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
  Ant_Exception_Clear(client->js);
  bool await_promise = inspector_param_bool(params, "awaitPromise");

  if (inspector_param_bool(params, "throwOnSideEffect")) {
    ant_value_t safe_result = Ant_Value_Undefined;
    if (inspector_eval_safe_expr(client->js, expr, expr_len, &safe_result)) {
      inspector_send_eval_completion(client, id, safe_result, await_promise);
    } else inspector_send_side_effect_blocked(client, id);
    return;
  }

  js_set_filename(client->js, "[inspector]");
  js_eval_result_t evaluation = js_eval_bytecode_repl(client->js, expr, expr_len);
  js_set_filename(client->js, prev_filename);
  
  inspector_send_eval_completion(client, id, evaluation.value, await_promise);
  coroutine_release(evaluation.async_coro);
}

void inspector_get_heap_usage(inspector_client_t *client, int id, yyjson_val *params) {
  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_uint(doc, result, "usedSize", inspector_heap_used(client->js));
  yyjson_mut_obj_add_uint(doc, result, "totalSize", inspector_heap_total(client->js));
  inspector_send_result(client, id, &d, result);
}

void inspector_get_properties(inspector_client_t *client, int id, yyjson_val *params) {
  ant_t *js = client->js;
  yyjson_val *object_id_val = params ? yyjson_obj_get(params, "objectId") : NULL;
  const char *object_id = object_id_val && yyjson_is_str(object_id_val) ? yyjson_get_str(object_id_val) : NULL;
  bool own_only = inspector_param_bool(params, "ownProperties");
  ant_value_t object = Ant_Value_Undefined;
  if (!inspector_object_for_id(object_id, &object)) {
    inspector_send_error(client, id, -32000, "Could not find object with given id");
    return;
  }

  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_mut_val *props = yyjson_mut_obj_add_arr(doc, result, "result");

  for (ant_value_t cur = object, depth = 0; is_object_type(cur) && depth < 64; depth++) {
    ant_iter_t iter = js_prop_iter_begin(js, cur);
    const char *key = NULL;
    size_t key_len = 0;
    ant_value_t value = Ant_Value_Undefined;
    while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
      bool error_stack = key_len == 5 && !memcmp(key, "stack", 5) && (
        vtype(js_get_slot(cur, SLOT_ERROR_STACK)) != kTypeUndefined ||
        vtype(js_get_slot(cur, SLOT_ERROR_STACK_TEXT)) != kTypeUndefined);
      if (error_stack) value = Ant_Error_ConsumeMarker(js, js_error_stack_value(js, cur));

      yyjson_mut_val *prop = yyjson_mut_arr_add_obj(doc, props);
      yyjson_mut_obj_add_strncpy(doc, prop, "name", key, key_len);
      yyjson_mut_obj_add_bool(doc, prop, "enumerable", true);
      yyjson_mut_obj_add_bool(doc, prop, "configurable", true);
      yyjson_mut_obj_add_bool(doc, prop, "writable", true);
      yyjson_mut_obj_add_bool(doc, prop, "isOwn", depth == 0);
      yyjson_mut_obj_add_val(doc, prop, "value", inspector_remote_object(js, doc, value));
    }
    js_prop_iter_end(&iter);
    if (own_only) break;
    ant_value_t proto = js_get_proto(js, cur);
    if (!is_object_type(proto)) break;
    cur = proto;
  }

  yyjson_mut_val *internal = yyjson_mut_obj_add_arr(doc, result, "internalProperties");
  ant_value_t proto = js_get_proto(js, object);
  if (is_object_type(proto) || vtype(proto) == kTypeNull) {
    yyjson_mut_val *entry = yyjson_mut_arr_add_obj(doc, internal);
    yyjson_mut_obj_add_str(doc, entry, "name", "[[Prototype]]");
    yyjson_mut_obj_add_val(doc, entry, "value", inspector_remote_object(js, doc, proto));
  }
  inspector_send_result(client, id, &d, result);
}

void inspector_global_lexical_scope_names(inspector_client_t *client, int id, yyjson_val *params) {
  ant_t *js = client->js;
  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_mut_val *names = yyjson_mut_obj_add_arr(doc, result, "names");

  ant_iter_t iter = js_prop_iter_begin(js, js_glob(js));
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = Ant_Value_Undefined;

  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (sv_global_lexical_lookup(js, key, key_len, NULL)) continue;
    yyjson_mut_arr_add_strncpy(doc, names, key, key_len);
  }
  js_prop_iter_end(&iter);

  for (uint32_t i = 0; i < js->global_lexical_count; i++) {
    const ant_global_lexical_t *lex = &js->global_lexicals[i];
    yyjson_mut_arr_add_strncpy(doc, names, lex->name, lex->len);
  }
  inspector_send_result(client, id, &d, result);
}

void ant_inspector_console_api_called(ant_t *js, const char *level, ant_args_t) {
  if (!g_inspector.started) return;

  inspector_doc_t d;
  yyjson_mut_doc *doc = inspector_doc_begin(&d);
  yyjson_mut_val *params = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_str(doc, params, "type", level ? level : "log");
  yyjson_mut_val *list = yyjson_mut_obj_add_arr(doc, params, "args");
  for (int i = 0; i < nargs; i++) yyjson_mut_arr_add_val(list, inspector_remote_object(js, doc, args[i]));
  yyjson_mut_obj_add_int(doc, params, "executionContextId", 1);
  yyjson_mut_obj_add_real(doc, params, "timestamp", (double)uv_hrtime() / 1000000.0);
  inspector_add_call_location(js, doc, params);

  char *json = inspector_event_finish(&d, "Runtime.consoleAPICalled", params);
  inspector_notify_console(json);
  inspector_store_console_event(json);
}
