#include <compat.h> // IWYU pragma: keep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "descriptors.h"
#include "errors.h"
#include "internal.h"
#include "list.h"
#include "silver/call.h"
#include "gc/modules.h"
#include "modules/isolate.h"
#include "isolate_internal.h"

static constexpr uint32_t ISOLATE_NATIVE_TAG = Ant_NativeTag("ISOL");
static constexpr uint32_t ISOLATE_HANDLE_TAG = Ant_NativeTag("IHDL");
static constexpr uint32_t ISOLATE_COPY_TAG = Ant_NativeTag("ICPY");
static constexpr uint32_t ISOLATE_AWAIT_TAG = Ant_NativeTag("IAWT");
static constexpr double ISOLATE_DEFAULT_MEMORY_MB = 128.0;

static const char *const guest_type_names[] = {
  "undefined", "object", "boolean", "number", "string", "symbol", "bigint", "function", "object",
};

typedef struct {
  ant_guest_t *g;
  isolate_handle_kind_t kind;
  uint32_t id;
  uint8_t type;
  bool released;
} isolate_handle_t;

typedef struct {
  bool want_ref;
  bool await_promise;
  uint32_t timeout_ms;
  char *filename;
} isolate_options_t;

ant_guest_t *guest_retain(ant_guest_t *g) {
  atomic_fetch_add_explicit(&g->refs, 1, memory_order_relaxed);
  return g;
}

void guest_release(ant_guest_t *g) {
  if (atomic_fetch_sub_explicit(&g->refs, 1, memory_order_acq_rel) != 1) return;
  for (guest_call_t *c = g->done_head, *next; c; c = next) {
    next = c->next;
    guest_call_free(c);
  }
  free(g->released);
  free(g->callbacks);
  uv_cond_destroy(&g->cond);
  uv_mutex_destroy(&g->lock);
  free(g);
}

static ant_guest_t *isolate_this(ant_t *js) {
  ant_value_t self = js_getthis(js);
  return is_object_type(self) ? js_get_native(self, ISOLATE_NATIVE_TAG) : NULL;
}

static isolate_handle_t *isolate_handle(ant_value_t obj, isolate_handle_kind_t kind) {
  isolate_handle_t *h = is_object_type(obj) ? js_get_native(obj, ISOLATE_HANDLE_TAG) : NULL;
  return h && h->kind == kind ? h : NULL;
}

static void isolate_queue_release(ant_guest_t *g, isolate_handle_kind_t kind, uint32_t id) {
  uv_mutex_lock(&g->lock);
  if (!g->disposed) {
    if (g->released_count == g->released_cap) {
      uint32_t cap = g->released_cap ? g->released_cap * 2 : 16;
      guest_released_t *ids = realloc(g->released, cap * sizeof(*ids));
      if (ids) {
        g->released = ids;
        g->released_cap = cap;
      }
    }
    if (g->released_count < g->released_cap) g->released[g->released_count++] = (guest_released_t){ kind, id };
  }
  uv_mutex_unlock(&g->lock);
}

static void isolate_handle_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  isolate_handle_t *h = js_get_native(value, ISOLATE_HANDLE_TAG);
  if (!h) return;
  js_clear_native(value, ISOLATE_HANDLE_TAG);
  if (!h->released) isolate_queue_release(h->g, h->kind, h->id);
  guest_release(h->g);
  free(h);
}

static ant_value_t isolate_make_handle(ant_t *js, ant_guest_t *g, isolate_handle_kind_t kind, uint32_t id, uint8_t type) {
  ant_value_t proto =
    kind == HANDLE_CONTEXT ? js->builtins.isolate_context_proto :
    kind == HANDLE_SCRIPT ? js->builtins.isolate_script_proto : js->builtins.isolate_reference_proto;

  isolate_handle_t *h = calloc(1, sizeof(*h));
  if (!h) return Ant_Error_CreateOutOfMemory(js);
  *h = (isolate_handle_t){ .g = guest_retain(g), .kind = kind, .id = id, .type = type };

  ant_value_t obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) {
    guest_release(g);
    free(h);
    return obj;
  }

  js_set_proto_init(obj, proto);
  js_set_native(obj, h, ISOLATE_HANDLE_TAG);
  js_set_finalizer(obj, isolate_handle_finalize);

  return obj;
}

static ant_value_t isolate_callback_id(ant_t *js, ant_guest_t *g, ant_value_t fn, uint32_t *out) {
  for (uint32_t i = 0; i < g->callback_count; i++) if (g->callbacks[i] == fn) {
    *out = i;
    return js_mkundef();
  }

  if (g->callback_count == g->callback_cap) {
    uint32_t cap = g->callback_cap ? g->callback_cap * 2 : 8;
    ant_value_t *callbacks = realloc(g->callbacks, cap * sizeof(*callbacks));
    if (!callbacks) return Ant_Error_CreateOutOfMemory(js);
    g->callbacks = callbacks;
    g->callback_cap = cap;
  }

  *out = g->callback_count;
  g->callbacks[g->callback_count++] = fn;

  return js_mkundef();
}

static bool isolate_is_callback(ant_t *js, ant_value_t value) {
  return is_object_type(value) && is_object_type(js->builtins.isolate_callback_proto) &&
    js_get_proto(js, value) == js->builtins.isolate_callback_proto;
}

ant_value_t guest_encode(ant_t *js, ant_guest_t *g, ant_value_t value, guest_value_t *out) {
  *out = (guest_value_t){ .kind = GUEST_VALUE_UNDEFINED };
  if (vtype(value) == kTypeUndefined) return js_mkundef();

  isolate_handle_t *ref = isolate_handle(value, HANDLE_REFERENCE);
  if (ref) {
    if (ref->g != g) return js_mkerr_typed(js, JS_ERR_TYPE, "Reference belongs to a different isolate");
    if (ref->released) return js_mkerr_typed(js, JS_ERR_TYPE, "Reference has been released");
    *out = (guest_value_t){ .kind = GUEST_VALUE_REF, .id = ref->id };
    return js_mkundef();
  }

  if (isolate_is_callback(js, value)) {
    uint32_t id;
    ant_value_t r = isolate_callback_id(js, g, js_get_slot(value, SLOT_DATA), &id);
    if (is_err(r)) return r;
    *out = (guest_value_t){ .kind = GUEST_VALUE_CALLBACK, .id = id };
    return js_mkundef();
  }

  if (is_object_type(value) && js_get_proto(js, value) == js->builtins.isolate_copy_proto) {
    ant_wire_t *copied = js_get_native(value, ISOLATE_COPY_TAG);
    if (!copied) return js_mkerr_typed(js, JS_ERR_TYPE, "ExternalCopy has been released");
    *out = (guest_value_t){ .kind = GUEST_VALUE_WIRE, .wire = ant_wire_retain(copied) };
    return js_mkundef();
  }

  ant_wire_t *wire = NULL;
  ant_value_t r = ant_wire_serialize(js, value, js_mkundef(), js_mkundef(), &wire);
  if (is_err(r)) return r;
  *out = (guest_value_t){ .kind = GUEST_VALUE_WIRE, .wire = wire };

  return js_mkundef();
}

ant_value_t guest_decode_host(ant_t *js, ant_guest_t *g, guest_value_t *v) {
  switch (v->kind) {
  case GUEST_VALUE_WIRE: return ant_wire_deserialize(js, v->wire, NULL);
  case GUEST_VALUE_REF: {
    ant_value_t ref = isolate_make_handle(js, g, HANDLE_REFERENCE, v->id, v->type);
    if (!is_err(ref)) v->kind = GUEST_VALUE_UNDEFINED;
    return ref;
  }
  default: return js_mkundef();
  }
}

ant_value_t guest_host_error(ant_t *js, guest_call_t *call) {
  if (call->status == GUEST_STATUS_DISPOSED)
    return js_take_thrown(js, js_mkerr_code(js, JS_ERR_GENERIC, "ERR_ISOLATE_DISPOSED", "Isolate is disposed"));
  if (call->status == GUEST_STATUS_OOM)
    return js_take_thrown(js, js_mkerr_code(js, JS_ERR_RANGE, "ERR_ISOLATE_OUT_OF_MEMORY", "Isolate was disposed because it exceeded its memory limit"));
  ant_value_t error = call->result.kind == GUEST_VALUE_WIRE ? ant_wire_deserialize(js, call->result.wire, NULL) : js_mkundef();
  return is_err(error) ? js_take_thrown(js, error) : error;
}

static ant_value_t isolate_heap_object(ant_t *js, guest_call_t *call) {
  ant_value_t stats = Ant_Object_Create(js);
  js_set(js, stats, "total_heap_size", js_mknum(call->stats[0]));
  js_set(js, stats, "used_heap_size", js_mknum(call->stats[1]));
  js_set(js, stats, "heap_size_limit", js_mknum(call->stats[2]));
  js_set(js, stats, "object_count", js_mknum(call->stats[3]));
  return stats;
}

static ant_value_t isolate_context_object(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t global = guest_decode_host(js, g, &call->result);
  if (is_err(global)) {
    GC_ROOT_RESTORE(js, mark);
    return global;
  }

  GC_ROOT_PIN(js, global);
  ant_value_t ctx = isolate_make_handle(js, g, HANDLE_CONTEXT, call->result_aux, 0);
  if (!is_err(ctx)) js_set(js, ctx, "global", global);
  GC_ROOT_RESTORE(js, mark);

  return ctx;
}

static ant_value_t isolate_call_value(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  switch (call->op) {
  case GUEST_OP_CONTEXT:
  case GUEST_OP_MAIN_CONTEXT: return isolate_context_object(js, g, call);
  case GUEST_OP_COMPILE: return isolate_make_handle(js, g, HANDLE_SCRIPT, call->result_aux, 0);
  case GUEST_OP_HEAP: return isolate_heap_object(js, call);
  default: return guest_decode_host(js, g, &call->result);
  }
}

static void isolate_pending_remove(ant_guest_t *g, guest_call_t *call) {
  for (guest_call_t **p = &g->pending; *p; p = &(*p)->pending_next) if (*p == call) {
    *p = call->pending_next;
    if (--g->pending_count == 0 && g->mode == GUEST_THREAD_OWN) guest_own_set_busy(g, false);
    return;
  }
}

static void isolate_dispose(ant_guest_t *g);

ant_value_t guest_finish_host(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  bool ok = call->status == GUEST_STATUS_OK;
  if (call->status == GUEST_STATUS_OOM) g->oom = true;
  ant_value_t value = ok ? isolate_call_value(js, g, call) : guest_host_error(js, call);
  if (ok && is_err(value)) {
    ok = false;
    value = js_take_thrown(js, value);
  }

  if (call->sync) {
    guest_call_free(call);
    if (g->oom) isolate_dispose(g);
    return ok ? value : js_throw(js, value);
  }

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, value);
  ant_value_t promise = call->promise;
  GC_ROOT_PIN(js, promise);
  isolate_pending_remove(g, call);
  guest_call_free(call);

  if (ok) js_resolve_promise(js, promise, value);
  else js_reject_promise(js, promise, value);
  GC_ROOT_RESTORE(js, mark);
  if (g->oom) isolate_dispose(g);
  
  return js_mkundef();
}

static void isolate_host_call_done(ant_t *js, ant_guest_t *g, guest_host_call_t *call) {
  if (g->mode == GUEST_THREAD_OWN) guest_own_host_call(js, g, call);
  else guest_current_host_call(js, g, call);
}

static void isolate_host_call_error(ant_t *js, guest_host_call_t *call, ant_value_t completion) {
  ant_value_t error = is_err(completion) ? js_take_thrown(js, completion) : completion;
  guest_value_free(&call->result);

  ant_wire_t *wire = NULL;
  ant_value_t r = ant_wire_serialize_error(js, error, &wire);
  
  if (is_err(r)) {
    js_take_thrown(js, r);
    ant_value_t text = js_tostring_val(js, error);
    if (is_err(text)) text = js_take_thrown(js, text);
    r = ant_wire_serialize(js, text, js_mkundef(), js_mkundef(), &wire);
    if (is_err(r)) js_take_thrown(js, r);
  }

  if (wire) call->result = (guest_value_t){ .kind = GUEST_VALUE_WIRE, .wire = wire };
  call->status = GUEST_STATUS_THROW;
}

static void isolate_host_call_value(ant_t *js, guest_host_call_t *call, ant_value_t value) {
  guest_value_free(&call->result);
  if (vtype(value) == kTypeUndefined) {
    call->status = GUEST_STATUS_OK;
    return;
  }

  ant_wire_t *wire = NULL;
  ant_value_t r = ant_wire_serialize(js, value, js_mkundef(), js_mkundef(), &wire);
  if (is_err(r)) {
    isolate_host_call_error(js, call, r);
    return;
  }

  call->result = (guest_value_t){ .kind = GUEST_VALUE_WIRE, .wire = wire };
  call->status = GUEST_STATUS_OK;
}

static void isolate_awaiting_remove(ant_guest_t *g, guest_host_call_t *call) {
  for (guest_host_call_t **p = &g->awaiting; *p; p = &(*p)->awaiting_next) if (*p == call) {
    *p = call->awaiting_next;
    return;
  }
}

static ant_value_t isolate_host_settled(ant_t *js, guest_host_call_t *call, ant_value_t value, bool fulfilled) {
  ant_guest_t *g = js_get_native(js_func_data(js->current_func), ISOLATE_NATIVE_TAG);
  if (fulfilled) isolate_host_call_value(js, call, value);
  else isolate_host_call_error(js, call, value);
  
  if (g) {
    isolate_awaiting_remove(g, call);
    isolate_host_call_done(js, g, call);
  } else guest_host_call_free(call);
  
  return js_mkundef();
}

static ant_value_t isolate_host_fulfilled(ant_params_t) {
  guest_host_call_t *call = js_get_native(js_getcurrentfunc(js), ISOLATE_AWAIT_TAG);
  return call ? isolate_host_settled(js, call, nargs ? args[0] : js_mkundef(), true) : js_mkundef();
}

static ant_value_t isolate_host_rejected(ant_params_t) {
  guest_host_call_t *call = js_get_native(js_getcurrentfunc(js), ISOLATE_AWAIT_TAG);
  return call ? isolate_host_settled(js, call, nargs ? args[0] : js_mkundef(), false) : js_mkundef();
}

static bool isolate_host_await(ant_t *js, ant_guest_t *g, guest_host_call_t *call, ant_value_t promise) {
  if (g->mode != GUEST_THREAD_OWN || !is_object_type(g->obj)) return false;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, promise);
  ant_value_t on_ok = js_heavy_mkfun_native(js, isolate_host_fulfilled, call, ISOLATE_AWAIT_TAG);
  GC_ROOT_PIN(js, on_ok);
  ant_value_t on_err = js_heavy_mkfun_native(js, isolate_host_rejected, call, ISOLATE_AWAIT_TAG);
  GC_ROOT_PIN(js, on_err);

  js_set_slot(on_ok, SLOT_DATA, g->obj);
  js_set_slot(on_err, SLOT_DATA, g->obj);

  call->promise = promise;
  call->awaiting_next = g->awaiting;
  g->awaiting = call;

  ant_value_t r = js_promise_then(js, promise, on_ok, on_err);
  GC_ROOT_RESTORE(js, mark);

  if (!is_err(r)) return true;
  isolate_awaiting_remove(g, call);
  isolate_host_call_error(js, call, r);

  return false;
}

void guest_host_run_callback(ant_t *js, ant_guest_t *g, guest_host_call_t *call) {
  if (call->callback >= g->callback_count) {
    isolate_host_call_error(js, call, js_mkerr_typed(js, JS_ERR_TYPE, "Unknown isolate callback"));
    isolate_host_call_done(js, g, call);
    return;
  }

  GC_ROOT_SAVE(mark, js);
  ant_value_t fn = g->callbacks[call->callback];
  ant_value_t *argv = call->argc ? calloc(call->argc, sizeof(*argv)) : NULL;
  ant_value_t result = call->argc && !argv ? Ant_Error_CreateOutOfMemory(js) : js_mkundef();

  for (uint32_t i = 0; i < call->argc && !is_err(result); i++) {
    argv[i] = guest_decode_host(js, g, &call->args[i]);
    if (is_err(argv[i])) result = argv[i];
    else GC_ROOT_PIN(js, argv[i]);
  }

  if (!is_err(result)) result = sv_vm_call(js->vm, js, fn, js_mkundef(), argv, (int)call->argc, NULL, js_mkundef());
  free(argv);

  bool waiting = false;
  if (is_err(result)) isolate_host_call_error(js, call, result);
  else if (vtype(result) == kTypePromise) waiting = isolate_host_await(js, g, call, result);
  else isolate_host_call_value(js, call, result);

  if (vtype(result) == kTypePromise && !waiting && call->status == GUEST_STATUS_PENDING)
    isolate_host_call_error(js, call, js_mkerr_typed(js, JS_ERR_TYPE, "A Callback cannot return a promise on thread 'current'"));

  GC_ROOT_RESTORE(js, mark);
  if (!waiting) isolate_host_call_done(js, g, call);
}

static ant_value_t isolate_submit(ant_t *js, ant_guest_t *g, guest_call_t *call, bool sync) {
  call->sync = sync;
  call->promise = sync ? js_mkundef() : js_mkpromise(js);

  if (g->disposed) {
    call->status = GUEST_STATUS_DISPOSED;
    call->done = true;
    if (sync) return guest_finish_host(js, g, call);

    ant_value_t promise = call->promise;
    call->pending_next = g->pending;
    g->pending = call;
    g->pending_count++;
    guest_finish_host(js, g, call);

    return promise;
  }

  if (!sync) {
    call->pending_next = g->pending;
    g->pending = call;
    if (g->pending_count++ == 0 && g->mode == GUEST_THREAD_OWN) guest_own_set_busy(g, true);
  }

  ant_value_t promise = call->promise;
  ant_value_t r = g->mode == GUEST_THREAD_OWN ? guest_own_submit(js, g, call) : guest_current_submit(js, g, call);

  return sync ? r : promise;
}

static ant_value_t isolate_read_options(ant_t *js, ant_value_t options, isolate_options_t *out) {
  *out = (isolate_options_t){ 0 };
  if (vtype(options) == kTypeUndefined || vtype(options) == kTypeNull) return js_mkundef();
  if (!is_object_type(options)) return Ant_Error_CreateInvalidArgType(js, "options", "of type object", options);

  ant_value_t timeout = js_get(js, options, "timeout");
  if (vtype(timeout) != kTypeUndefined) {
    if (vtype(timeout) != kTypeNumber) return Ant_Error_CreateInvalidArgType(js, "options.timeout", "of type number", timeout);
    double ms = js_getnum(timeout);
    if (!(ms >= 1 && ms <= 4294967295.0)) return Ant_Error_CreateOutOfRange(js, "options.timeout", ">= 1 && <= 4294967295", timeout);
    out->timeout_ms = (uint32_t)ms;
  }

  out->want_ref = js_truthy(js, js_get(js, options, "reference"));
  out->await_promise = js_truthy(js, js_get(js, options, "promise"));

  ant_value_t filename = js_get(js, options, "filename");
  if (vtype(filename) == kTypeString) {
    size_t len = 0;
    const char *text = js_getstr(js, filename, &len);
    out->filename = strndup(text, len);
  }

  return js_mkundef();
}

static guest_call_t *isolate_call_new(ant_t *js, guest_op_t op, const isolate_options_t *opts) {
  guest_call_t *call = calloc(1, sizeof(*call));
  if (!call) return NULL;
  call->op = op;
  call->promise = js_mkundef();
  call->owner = js_mkundef();
  if (opts) {
    call->timeout_ms = opts->timeout_ms;
    call->want_ref = opts->want_ref;
    call->await_promise = opts->await_promise;
    call->filename = opts->filename;
  }
  return call;
}

static ant_value_t isolate_call_text(ant_t *js, guest_call_t *call, ant_value_t code) {
  ant_value_t text = vtype(code) == kTypeString ? code : js_tostring_val(js, code);
  if (is_err(text)) return text;
  size_t len = 0;
  const char *src = js_getstr(js, text, &len);
  call->text = malloc(len + 1);
  if (!call->text) return Ant_Error_CreateOutOfMemory(js);
  memcpy(call->text, src, len);
  call->text[len] = '\0';
  call->text_len = len;
  return js_mkundef();
}

static ant_value_t isolate_call_args(ant_t *js, ant_guest_t *g, guest_call_t *call, ant_value_t *values, uint32_t count) {
  call->args = count ? calloc(count, sizeof(*call->args)) : NULL;
  if (count && !call->args) return Ant_Error_CreateOutOfMemory(js);
  call->argc = count;
  for (uint32_t i = 0; i < count; i++) {
    ant_value_t r = guest_encode(js, g, values[i], &call->args[i]);
    if (is_err(r)) return r;
  }
  return js_mkundef();
}

static ant_value_t isolate_fail(guest_call_t *call, ant_value_t error) {
  guest_call_free(call);
  return error;
}

static ant_value_t isolate_simple(ant_t *js, ant_guest_t *g, guest_op_t op, bool sync) {
  if (!g) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  guest_call_t *call = isolate_call_new(js, op, NULL);
  return call ? isolate_submit(js, g, call, sync) : Ant_Error_CreateOutOfMemory(js);
}

static ant_value_t isolate_compile(ant_t *js, ant_value_t *args, int nargs, bool sync) {
  ant_guest_t *g = isolate_this(js);
  if (!g) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");

  isolate_options_t opts;
  ant_value_t r = isolate_read_options(js, nargs > 1 ? args[1] : js_mkundef(), &opts);
  if (is_err(r)) return r;

  guest_call_t *call = isolate_call_new(js, GUEST_OP_COMPILE, &opts);
  if (!call) return Ant_Error_CreateOutOfMemory(js);
  r = isolate_call_text(js, call, nargs > 0 ? args[0] : js_mkundef());

  return is_err(r) ? isolate_fail(call, r) : isolate_submit(js, g, call, sync);
}

static ant_value_t isolate_run(ant_t *js, guest_op_t op, isolate_handle_t *target, isolate_handle_t *ctx, ant_value_t code, ant_value_t options, bool sync, bool force_ref) {
  if (!target || !ctx) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  if (target->g != ctx->g) return js_mkerr_typed(js, JS_ERR_TYPE, "Script and context belong to different isolates");
  if (ctx->released) return js_mkerr_typed(js, JS_ERR_TYPE, "Context has been released");
  if (target->released) return js_mkerr_typed(js, JS_ERR_TYPE, "Script has been released");

  isolate_options_t opts;
  ant_value_t r = isolate_read_options(js, options, &opts);
  if (is_err(r)) return r;
  if (force_ref) opts.want_ref = true;

  guest_call_t *call = isolate_call_new(js, op, &opts);
  if (!call) return Ant_Error_CreateOutOfMemory(js);
  call->context = ctx->id;
  call->target = target->id;

  if (op == GUEST_OP_EVAL) {
    r = isolate_call_text(js, call, code);
    if (is_err(r)) return isolate_fail(call, r);
  }

  return isolate_submit(js, target->g, call, sync);
}

static ant_value_t isolate_property(ant_t *js, guest_op_t op, ant_value_t *args, int nargs, bool sync) {
  isolate_handle_t *ref = isolate_handle(js_getthis(js), HANDLE_REFERENCE);
  if (!ref) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");

  isolate_options_t opts;
  ant_value_t r = isolate_read_options(js, op == GUEST_OP_GET && nargs > 1 ? args[1] : js_mkundef(), &opts);
  if (is_err(r)) return r;

  guest_call_t *call = isolate_call_new(js, op, &opts);
  if (!call) return Ant_Error_CreateOutOfMemory(js);
  call->target = ref->id;

  ant_value_t values[2] = { nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef() };
  r = isolate_call_args(js, ref->g, call, values, op == GUEST_OP_SET ? 2 : 1);

  return is_err(r) ? isolate_fail(call, r) : isolate_submit(js, ref->g, call, sync);
}

static ant_value_t isolate_apply(ant_t *js, ant_value_t *args, int nargs, bool sync) {
  isolate_handle_t *ref = isolate_handle(js_getthis(js), HANDLE_REFERENCE);
  if (!ref) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");

  isolate_options_t opts;
  ant_value_t r = isolate_read_options(js, nargs > 2 ? args[2] : js_mkundef(), &opts);
  if (is_err(r)) return r;

  ant_value_t list = nargs > 1 ? args[1] : js_mkundef();
  if (vtype(list) != kTypeUndefined && vtype(list) != kTypeArray)
    return Ant_Error_CreateInvalidArgType(js, "arguments", "an instance of Array", list);
  uint32_t count = vtype(list) == kTypeArray ? (uint32_t)js_arr_len(js, list) : 0;

  ant_value_t *values = calloc((size_t)count + 1, sizeof(*values));
  if (!values) return Ant_Error_CreateOutOfMemory(js);
  values[0] = nargs > 0 ? args[0] : js_mkundef();
  for (uint32_t i = 0; i < count; i++) values[i + 1] = js_arr_get(js, list, i);

  guest_call_t *call = isolate_call_new(js, GUEST_OP_APPLY, &opts);
  if (!call) {
    free(values);
    return Ant_Error_CreateOutOfMemory(js);
  }

  call->target = ref->id;
  r = isolate_call_args(js, ref->g, call, values, count + 1);
  free(values);

  return is_err(r) ? isolate_fail(call, r) : isolate_submit(js, ref->g, call, sync);
}

static ant_value_t isolate_copy_op(ant_t *js, bool sync) {
  isolate_handle_t *ref = isolate_handle(js_getthis(js), HANDLE_REFERENCE);
  if (!ref) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  guest_call_t *call = isolate_call_new(js, GUEST_OP_COPY, NULL);
  if (!call) return Ant_Error_CreateOutOfMemory(js);
  call->target = ref->id;
  return isolate_submit(js, ref->g, call, sync);
}

#define ISOLATE_PAIR(name, body)                                 \
  static ant_value_t js_##name(ant_params_t) { body(false); }    \
  static ant_value_t js_##name##_sync(ant_params_t) { body(true); }

#define CREATE_CONTEXT(sync) return isolate_simple(js, isolate_this(js), GUEST_OP_CONTEXT, sync)
#define COMPILE_SCRIPT(sync) return isolate_compile(js, args, nargs, sync)
#define HEAP_STATS(sync) return isolate_simple(js, isolate_this(js), GUEST_OP_HEAP, sync)
#define SCRIPT_RUN(sync) return isolate_run(js, GUEST_OP_RUN, isolate_handle(js_getthis(js), HANDLE_SCRIPT), \
  isolate_handle(nargs > 0 ? args[0] : js_mkundef(), HANDLE_CONTEXT), js_mkundef(), nargs > 1 ? args[1] : js_mkundef(), sync, false)
#define CONTEXT_EVAL(sync) { isolate_handle_t *c = isolate_handle(js_getthis(js), HANDLE_CONTEXT); \
  return isolate_run(js, GUEST_OP_EVAL, c, c, nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef(), sync, false); }
#define CONTEXT_EVAL_REF(sync) { isolate_handle_t *c = isolate_handle(js_getthis(js), HANDLE_CONTEXT); \
  return isolate_run(js, GUEST_OP_EVAL, c, c, nargs > 0 ? args[0] : js_mkundef(), nargs > 1 ? args[1] : js_mkundef(), sync, true); }
#define REF_GET(sync) return isolate_property(js, GUEST_OP_GET, args, nargs, sync)
#define REF_SET(sync) return isolate_property(js, GUEST_OP_SET, args, nargs, sync)
#define REF_APPLY(sync) return isolate_apply(js, args, nargs, sync)
#define REF_COPY(sync) return isolate_copy_op(js, sync)

ISOLATE_PAIR(isolate_create_context, CREATE_CONTEXT)
ISOLATE_PAIR(isolate_compile_script, COMPILE_SCRIPT)
ISOLATE_PAIR(isolate_heap_statistics, HEAP_STATS)
ISOLATE_PAIR(script_run, SCRIPT_RUN)
ISOLATE_PAIR(context_eval, CONTEXT_EVAL)
ISOLATE_PAIR(context_eval_reference, CONTEXT_EVAL_REF)
ISOLATE_PAIR(reference_get, REF_GET)
ISOLATE_PAIR(reference_set, REF_SET)
ISOLATE_PAIR(reference_apply, REF_APPLY)
ISOLATE_PAIR(reference_copy, REF_COPY)

static ant_value_t js_isolate_main_context(ant_params_t) {
  ant_value_t self = js_getthis(js);
  ant_value_t cached = js_get_slot(self, SLOT_DATA);
  if (is_object_type(cached)) return cached;
  ant_value_t ctx = isolate_simple(js, isolate_this(js), GUEST_OP_MAIN_CONTEXT, true);
  if (!is_err(ctx)) js_set_slot(self, SLOT_DATA, ctx);
  return ctx;
}

static void isolate_dispose(ant_guest_t *g) {
  if (!g || g->disposed) return;
  if (g->mode == GUEST_THREAD_OWN) guest_own_dispose(g);
  else guest_current_dispose(g);

  ant_t *js = g->host;
  while (g->pending) {
    guest_call_t *call = g->pending;
    call->status = GUEST_STATUS_DISPOSED;
    guest_finish_host(js, g, call);
  }

  g->obj = js_mkundef();
  Ant_List_Remove(g);
}

static ant_value_t js_isolate_dispose(ant_params_t) {
  ant_guest_t *g = isolate_this(js);
  if (!g) return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal invocation");
  isolate_dispose(g);
  return js_mkundef();
}

static ant_value_t js_isolate_is_disposed(ant_params_t) {
  ant_guest_t *g = isolate_this(js);
  return js_bool(!g || g->disposed);
}

static ant_value_t isolate_ns_bigint(ant_t *js, uint64_t ns) {
  char digits[24];
  int len = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)ns);
  return js_mkbigint(js, digits, (size_t)len, false);
}

static ant_value_t js_isolate_cpu_time(ant_params_t) {
  ant_guest_t *g = isolate_this(js);
  return isolate_ns_bigint(js, g ? atomic_load_explicit(&g->cpu_ns, memory_order_relaxed) : 0);
}

static ant_value_t js_isolate_wall_time(ant_params_t) {
  ant_guest_t *g = isolate_this(js);
  return isolate_ns_bigint(js, g ? atomic_load_explicit(&g->wall_ns, memory_order_relaxed) : 0);
}

static void isolate_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_guest_t *g = js_get_native(value, ISOLATE_NATIVE_TAG);
  if (!g) return;
  js_clear_native(value, ISOLATE_NATIVE_TAG);
  guest_release(g);
}

static ant_value_t isolate_parse(ant_t *js, ant_value_t options, ant_guest_t *g) {
  double memory = ISOLATE_DEFAULT_MEMORY_MB;
  g->mode = GUEST_THREAD_OWN;
  g->globals = GUEST_GLOBALS_BARE;

  if (is_object_type(options)) {
    ant_value_t limit = js_get(js, options, "memoryLimit");
    if (vtype(limit) != kTypeUndefined) {
      if (vtype(limit) != kTypeNumber || !(js_getnum(limit) >= 8))
        return Ant_Error_CreateOutOfRange(js, "options.memoryLimit", ">= 8", limit);
      memory = js_getnum(limit);
    }

    ant_value_t thread = js_get(js, options, "thread");
    if (vtype(thread) == kTypeString) {
      const char *name = js_getstr(js, thread, NULL);
      if (strcmp(name, "current") == 0) g->mode = GUEST_THREAD_CURRENT;
      else if (strcmp(name, "own") != 0) return Ant_Error_CreateInvalidArgValue(js, "options.thread", "'own' or 'current'", thread);
    } else if (vtype(thread) != kTypeUndefined) return Ant_Error_CreateInvalidArgValue(js, "options.thread", "'own' or 'current'", thread);

    ant_value_t globals = js_get(js, options, "globals");
    if (vtype(globals) == kTypeString) {
      const char *name = js_getstr(js, globals, NULL);
      if (strcmp(name, "ant") == 0) g->globals = GUEST_GLOBALS_ANT;
      else if (strcmp(name, "bare") != 0) return Ant_Error_CreateInvalidArgValue(js, "options.globals", "'bare' or 'ant'", globals);
    } else if (vtype(globals) != kTypeUndefined) return Ant_Error_CreateInvalidArgValue(js, "options.globals", "'bare' or 'ant'", globals);
  }

  g->arena_bytes = (size_t)(memory * 1024 * 1024);
  return js_mkundef();
}

static ant_value_t js_isolate_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor Isolate cannot be invoked without 'new'");

  ant_value_t self = js_getthis(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->builtins.isolate_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(self, proto);

  ant_guest_t *g = calloc(1, sizeof(*g));
  if (!g) return Ant_Error_CreateOutOfMemory(js);
  atomic_init(&g->refs, 1);
  uv_mutex_init(&g->lock);
  uv_cond_init(&g->cond);
  g->host = js;
  g->obj = self;

  ant_value_t r = isolate_parse(js, nargs > 0 ? args[0] : js_mkundef(), g);
  if (is_err(r)) {
    guest_release(g);
    return r;
  }

  js_set_native(self, g, ISOLATE_NATIVE_TAG);
  js_set_finalizer(self, isolate_finalize);
  js_set_slot(self, SLOT_DATA, js_mkundef());
  Ant_List_Insert(&js->guests, g);

  r = g->mode == GUEST_THREAD_OWN ? guest_own_start(js, g) : guest_current_start(js, g);
  if (is_err(r)) {
    g->disposed = true;
    g->obj = js_mkundef();
    Ant_List_Remove(g);
    return r;
  }

  return self;
}

static ant_value_t js_handle_release(ant_params_t) {
  ant_value_t self = js_getthis(js);
  isolate_handle_t *h = is_object_type(self) ? js_get_native(self, ISOLATE_HANDLE_TAG) : NULL;
  if (h && !h->released) {
    h->released = true;
    isolate_queue_release(h->g, h->kind, h->id);
  }
  return js_mkundef();
}

static ant_value_t js_reference_typeof(ant_params_t) {
  isolate_handle_t *ref = isolate_handle(js_getthis(js), HANDLE_REFERENCE);
  if (!ref) return js_mkundef();
  const char *name = ref->type < sizeof(guest_type_names) / sizeof(*guest_type_names) ? guest_type_names[ref->type] : "object";
  if (ref->type == 1) name = "object";
  return js_mkstr(js, name, strlen(name));
}

static ant_value_t js_handle_ctor(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Illegal constructor");
}

static ant_value_t js_callback_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor Callback cannot be invoked without 'new'");
  if (nargs < 1 || !is_callable(args[0])) return Ant_Error_CreateInvalidArgType(js, "fn", "of type function", nargs ? args[0] : js_mkundef());
  ant_value_t self = js_getthis(js);
  js_set_proto_init(self, js->builtins.isolate_callback_proto);
  js_set_slot(self, SLOT_DATA, args[0]);
  return self;
}

static void isolate_copy_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_wire_t *wire = js_get_native(value, ISOLATE_COPY_TAG);
  if (!wire) return;
  js_clear_native(value, ISOLATE_COPY_TAG);
  ant_wire_release(wire);
}

static ant_value_t isolate_copy_init(ant_t *js, ant_value_t self, ant_value_t value) {
  ant_wire_t *wire = NULL;
  ant_value_t r = ant_wire_serialize(js, value, js_mkundef(), js_mkundef(), &wire);
  if (is_err(r)) return r;
  js_set_proto_init(self, js->builtins.isolate_copy_proto);
  js_set_native(self, wire, ISOLATE_COPY_TAG);
  js_set_finalizer(self, isolate_copy_finalize);
  return self;
}

static ant_value_t js_copy_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor ExternalCopy cannot be invoked without 'new'");
  return isolate_copy_init(js, js_getthis(js), nargs ? args[0] : js_mkundef());
}

static ant_value_t js_copy_fn(ant_params_t) {
  ant_value_t self = Ant_Object_CreateBare(js);
  return is_err(self) ? self : isolate_copy_init(js, self, nargs ? args[0] : js_mkundef());
}

static ant_wire_t *isolate_copy_this(ant_t *js) {
  ant_value_t self = js_getthis(js);
  return is_object_type(self) ? js_get_native(self, ISOLATE_COPY_TAG) : NULL;
}

static ant_value_t js_copy_copy(ant_params_t) {
  ant_wire_t *wire = isolate_copy_this(js);
  if (!wire) return js_mkerr_typed(js, JS_ERR_TYPE, "ExternalCopy has been released");
  return ant_wire_deserialize(js, wire, NULL);
}

static ant_value_t js_copy_copy_into(ant_params_t) {
  ant_wire_t *wire = isolate_copy_this(js);
  if (!wire) return js_mkerr_typed(js, JS_ERR_TYPE, "ExternalCopy has been released");
  ant_value_t copy = Ant_Object_CreateBare(js);
  if (is_err(copy)) return copy;
  js_set_proto_init(copy, js->builtins.isolate_copy_proto);
  js_set_native(copy, ant_wire_retain(wire), ISOLATE_COPY_TAG);
  js_set_finalizer(copy, isolate_copy_finalize);
  return copy;
}

static ant_value_t js_copy_release(ant_params_t) {
  ant_value_t self = js_getthis(js);
  if (isolate_copy_this(js)) isolate_copy_finalize(js, js_obj_ptr(self));
  return js_mkundef();
}

static ant_value_t isolate_proto_new(ant_t *js, const char *tag) {
  ant_value_t proto = Ant_Object_CreateBare(js);
  js_set_tostringtag(js, proto, js_mkstr(js, tag, strlen(tag)));
  return proto;
}

#define ISOLATE_METHOD(proto, name, fn, n) js_set(js, proto, name, Ant_Function_CreateWithArity(fn, n))

static void isolate_init_classes(ant_t *js) {
  if (is_object_type(js->builtins.isolate_proto)) return;

  ant_value_t iso = isolate_proto_new(js, "Isolate");
  ISOLATE_METHOD(iso, "createContext", js_isolate_create_context, 0);
  ISOLATE_METHOD(iso, "createContextSync", js_isolate_create_context_sync, 0);
  ISOLATE_METHOD(iso, "compileScript", js_isolate_compile_script, 1);
  ISOLATE_METHOD(iso, "compileScriptSync", js_isolate_compile_script_sync, 1);
  ISOLATE_METHOD(iso, "getHeapStatistics", js_isolate_heap_statistics, 0);
  ISOLATE_METHOD(iso, "getHeapStatisticsSync", js_isolate_heap_statistics_sync, 0);
  ISOLATE_METHOD(iso, "dispose", js_isolate_dispose, 0);
  js_set_getter_desc(js, iso, "mainContext", 11, Ant_Function_Create(js_isolate_main_context), JS_DESC_C);
  js_set_getter_desc(js, iso, "isDisposed", 10, Ant_Function_Create(js_isolate_is_disposed), JS_DESC_C);
  js_set_getter_desc(js, iso, "cpuTime", 7, Ant_Function_Create(js_isolate_cpu_time), JS_DESC_C);
  js_set_getter_desc(js, iso, "wallTime", 8, Ant_Function_Create(js_isolate_wall_time), JS_DESC_C);
  js->builtins.isolate_proto = iso;
  js->builtins.isolate_ctor = Ant_Function_CreateConstructor(js, js_isolate_ctor, iso, "Isolate", 0);

  ant_value_t ctx = isolate_proto_new(js, "Context");
  ISOLATE_METHOD(ctx, "eval", js_context_eval, 1);
  ISOLATE_METHOD(ctx, "evalSync", js_context_eval_sync, 1);
  ISOLATE_METHOD(ctx, "evalReference", js_context_eval_reference, 1);
  ISOLATE_METHOD(ctx, "evalReferenceSync", js_context_eval_reference_sync, 1);
  ISOLATE_METHOD(ctx, "release", js_handle_release, 0);
  js->builtins.isolate_context_proto = ctx;
  js->builtins.isolate_context_ctor = Ant_Function_CreateConstructor(js, js_handle_ctor, ctx, "Context", 0);

  ant_value_t script = isolate_proto_new(js, "Script");
  ISOLATE_METHOD(script, "run", js_script_run, 1);
  ISOLATE_METHOD(script, "runSync", js_script_run_sync, 1);
  ISOLATE_METHOD(script, "release", js_handle_release, 0);
  js->builtins.isolate_script_proto = script;
  js->builtins.isolate_script_ctor = Ant_Function_CreateConstructor(js, js_handle_ctor, script, "Script", 0);

  ant_value_t ref = isolate_proto_new(js, "Reference");
  ISOLATE_METHOD(ref, "get", js_reference_get, 1);
  ISOLATE_METHOD(ref, "getSync", js_reference_get_sync, 1);
  ISOLATE_METHOD(ref, "set", js_reference_set, 2);
  ISOLATE_METHOD(ref, "setSync", js_reference_set_sync, 2);
  ISOLATE_METHOD(ref, "apply", js_reference_apply, 2);
  ISOLATE_METHOD(ref, "applySync", js_reference_apply_sync, 2);
  ISOLATE_METHOD(ref, "copy", js_reference_copy, 0);
  ISOLATE_METHOD(ref, "copySync", js_reference_copy_sync, 0);
  ISOLATE_METHOD(ref, "release", js_handle_release, 0);
  js_set_getter_desc(js, ref, "typeof", 6, Ant_Function_Create(js_reference_typeof), JS_DESC_C);
  js->builtins.isolate_reference_proto = ref;
  js->builtins.isolate_reference_ctor = Ant_Function_CreateConstructor(js, js_handle_ctor, ref, "Reference", 0);

  ant_value_t cb = isolate_proto_new(js, "Callback");
  js->builtins.isolate_callback_proto = cb;
  js->builtins.isolate_callback_ctor = Ant_Function_CreateConstructor(js, js_callback_ctor, cb, "Callback", 1);

  ant_value_t copy = isolate_proto_new(js, "ExternalCopy");
  ISOLATE_METHOD(copy, "copy", js_copy_copy, 0);
  ISOLATE_METHOD(copy, "copyInto", js_copy_copy_into, 0);
  ISOLATE_METHOD(copy, "release", js_copy_release, 0);
  js->builtins.isolate_copy_proto = copy;
  js->builtins.isolate_copy_ctor = Ant_Function_CreateConstructor(js, js_copy_ctor, copy, "ExternalCopy", 1);
}

ant_value_t isolate_library(ant_t *js) {
  isolate_init_classes(js);
  ant_value_t lib = Ant_Object_CreateBare(js);

  js_set(js, lib, "Isolate", js->builtins.isolate_ctor);
  js_set(js, lib, "Context", js->builtins.isolate_context_ctor);
  js_set(js, lib, "Script", js->builtins.isolate_script_ctor);
  js_set(js, lib, "Reference", js->builtins.isolate_reference_ctor);
  js_set(js, lib, "Callback", js->builtins.isolate_callback_ctor);
  js_set(js, lib, "ExternalCopy", js->builtins.isolate_copy_ctor);
  js_set(js, lib, "copy", Ant_Function_CreateWithArity(js_copy_fn, 1));

  js_set(js, lib, "default", lib);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, lib);
  js_set_tostringtag(js, lib, js_mkstr(js, "ant:isolate", 11));

  return lib;
}

void cleanup_isolate_module(ant_t *js) {
  while (js->guests) isolate_dispose(js->guests);
}

void gc_mark_isolates(ant_t *js, gc_mark_fn mark) {
  for (ant_guest_t *g = js->guests; g; g = g->next) {
    if (is_object_type(g->obj)) mark(js, g->obj);
    for (uint32_t i = 0; i < g->callback_count; i++) mark(js, g->callbacks[i]);
    for (guest_call_t *c = g->pending; c; c = c->pending_next) {
      if (vtype(c->promise) == kTypePromise) mark(js, c->promise);
      if (is_object_type(c->owner)) mark(js, c->owner);
    }
    for (guest_host_call_t *c = g->awaiting; c; c = c->awaiting_next)
      if (vtype(c->promise) == kTypePromise) mark(js, c->promise);
  }
  guest_child_mark(js, mark);
}
