#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ant.h"
#include "bootstrap.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "interrupt.h"
#include "realm.h"
#include "runtime.h"
#include "silver/call.h"
#include "modules/timer.h"
#include "modules/vm_interrupt.h"
#include "isolate_internal.h"

static constexpr uint32_t GUEST_CALL_NATIVE_TAG = Ant_NativeTag("GCAL");

static uint64_t guest_thread_cpu_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void guest_value_free(guest_value_t *v) {
  if (v->kind == GUEST_VALUE_WIRE) ant_wire_release(v->wire);
  *v = (guest_value_t){ 0 };
}

void guest_values_free(guest_value_t *values, uint32_t count) {
  for (uint32_t i = 0; values && i < count; i++) guest_value_free(&values[i]);
  free(values);
}

void guest_call_free(guest_call_t *call) {
  if (!call) return;
  guest_values_free(call->args, call->argc);
  guest_value_free(&call->result);
  free(call->text);
  free(call->filename);
  free(call);
}

void guest_host_call_free(guest_host_call_t *call) {
  if (!call) return;
  guest_values_free(call->args, call->argc);
  guest_value_free(&call->result);
  free(call);
}

static uint8_t guest_type_of(ant_value_t value) {
  switch (vtype(value)) {
  case kTypeUndefined: return 0;
  case kTypeNull: return 1;
  case kTypeBool: return 2;
  case kTypeNumber: return 3;
  case kTypeString: return 4;
  case kTypeSymbol: return 5;
  case kTypeBigInt: return 6;
  case kTypeFunction:
  case kTypeBuiltin: return 7;
  default: return 8;
  }
}

static uint32_t guest_handle_put(ant_guest_t *g, ant_value_t value) {
  if (g->handle_free) {
    uint32_t id = g->handle_free_ids[--g->handle_free];
    g->handles[id] = value;
    g->handle_live[id] = 1;
    return id;
  }

  if (g->handle_count + 1 >= g->handle_cap) {
    uint32_t cap = g->handle_cap ? g->handle_cap * 2 : 16;
    ant_value_t *handles = realloc(g->handles, cap * sizeof(*handles));
    if (handles) g->handles = handles;
    uint8_t *live = handles ? realloc(g->handle_live, cap) : NULL;
    if (!live) return 0;
    memset(live + g->handle_cap, 0, cap - g->handle_cap);
    g->handle_live = live;
    g->handle_cap = cap;
    if (g->handle_count == 0) g->handle_count = 1;
  }

  g->handles[g->handle_count] = value;
  g->handle_live[g->handle_count] = 1;
  return g->handle_count++;
}

static bool guest_handle_valid(ant_guest_t *g, uint32_t id) {
  return id > 0 && id < g->handle_count && g->handle_live[id];
}

static void guest_handle_drop(ant_guest_t *g, uint32_t id) {
  if (!guest_handle_valid(g, id)) return;
  if (g->handle_free == g->handle_free_cap) {
    uint32_t cap = g->handle_free_cap ? g->handle_free_cap * 2 : 16;
    uint32_t *ids = realloc(g->handle_free_ids, cap * sizeof(*ids));
    if (!ids) return;
    g->handle_free_ids = ids;
    g->handle_free_cap = cap;
  }
  g->handles[id] = js_mkundef();
  g->handle_live[id] = 0;
  g->handle_free_ids[g->handle_free++] = id;
}

static void guest_drop_compiled(ant_guest_t *g, guest_script_t *script, uint32_t context) {
  if (context >= script->compiled_cap) return;
  guest_handle_drop(g, script->compiled[context]);
  script->compiled[context] = 0;
}

static void guest_drop_context(ant_guest_t *g, uint32_t id) {
  if (id == 0 || id >= g->context_count) return;
  g->contexts[id] = NULL;
  for (uint32_t i = 0; i < g->script_count; i++) guest_drop_compiled(g, &g->scripts[i], id);
}

static void guest_drop_script(ant_guest_t *g, uint32_t id) {
  if (id == 0 || id > g->script_count) return;
  guest_script_t *script = &g->scripts[id - 1];
  for (uint32_t i = 0; i < script->compiled_cap; i++) guest_drop_compiled(g, script, i);
  free(script->source);
  free(script->filename);
  free(script->compiled);
  *script = (guest_script_t){ 0 };
}

static void guest_apply_releases(ant_guest_t *g) {
  uv_mutex_lock(&g->lock);
  guest_released_t *ids = g->released;
  uint32_t count = g->released_count;
  g->released = NULL;
  g->released_count = g->released_cap = 0;
  uv_mutex_unlock(&g->lock);

  for (uint32_t i = 0; i < count; i++) switch (ids[i].kind) {
  case HANDLE_CONTEXT: guest_drop_context(g, ids[i].id); break;
  case HANDLE_SCRIPT: guest_drop_script(g, ids[i].id); break;
  case HANDLE_REFERENCE: guest_handle_drop(g, ids[i].id); break;
  }
  free(ids);
}

static ant_value_t guest_wire_encode(ant_t *js, ant_value_t value, bool error, guest_value_t *out) {
  if (vtype(value) == kTypeUndefined) {
    *out = (guest_value_t){ .kind = GUEST_VALUE_UNDEFINED };
    return js_mkundef();
  }

  ant_wire_t *wire = NULL;
  ant_value_t r = error
    ? ant_wire_serialize_error(js, value, &wire)
    : ant_wire_serialize(js, value, js_mkundef(), js_mkundef(), &wire);
  
  if (is_err(r)) return r;
  *out = (guest_value_t){ .kind = GUEST_VALUE_WIRE, .wire = wire };

  return js_mkundef();
}

static ant_value_t guest_wire_out(ant_t *js, ant_value_t value, guest_value_t *out) {
  return guest_wire_encode(js, value, false, out);
}

static ant_value_t guest_child_encode(ant_t *js, ant_guest_t *g, ant_value_t value, bool want_ref, guest_value_t *out) {
  if (!want_ref) return guest_wire_out(js, value, out);
  uint32_t id = guest_handle_put(g, value);
  
  if (!id) return Ant_Error_CreateOutOfMemory(js);
  *out = (guest_value_t){ .kind = GUEST_VALUE_REF, .id = id, .type = guest_type_of(value) };
  
  return js_mkundef();
}

static void guest_error_out(ant_t *js, ant_value_t error, guest_value_t *out) {
  if (!is_err(guest_wire_encode(js, error, true, out))) return;
  js_take_thrown(js, js_mkundef());

  ant_value_t text = js_tostring_val(js, error);
  if (is_err(text)) {
    js_take_thrown(js, text);
    text = js_mkstr(js, "Uncaught exception in isolate", 29);
  }

  if (is_err(guest_wire_out(js, text, out))) js_take_thrown(js, js_mkundef());
}

static void guest_call_throw(ant_t *js, guest_call_t *call, ant_value_t completion) {
  ant_value_t error = is_err(completion) ? js_take_thrown(js, completion) : completion;
  guest_value_free(&call->result);
  if (js_is_termination(error) || js->terminating) {
    call->status = GUEST_STATUS_DISPOSED;
    return;
  }
  if (is_err(js->exception_oom) && error == Ant_Exception_Value(js, js->exception_oom)) {
    call->status = GUEST_STATUS_OOM;
    return;
  }
  guest_error_out(js, error, &call->result);
  call->status = GUEST_STATUS_THROW;
}

static ant_value_t guest_child_callback_fn(ant_params_t);

static ant_value_t guest_child_decode(ant_t *js, ant_guest_t *g, guest_value_t *v) {
  switch (v->kind) {
  case GUEST_VALUE_WIRE: return ant_wire_deserialize(js, v->wire, NULL);
  case GUEST_VALUE_REF:
    if (!guest_handle_valid(g, v->id)) return js_mkerr_typed(js, JS_ERR_TYPE, "Reference has been released");
    return g->handles[v->id];
  case GUEST_VALUE_CALLBACK:
    return js_heavy_mkfun(js, guest_child_callback_fn, js_mknum((double)v->id));
  default:
    return js_mkundef();
  }
}

static ant_value_t guest_host_value_to_child(ant_t *js, guest_status_t status, guest_value_t *v) {
  if (status == GUEST_STATUS_DISPOSED) return js_mkerr(js, "Isolate host is no longer available");
  ant_value_t value = guest_child_decode(js, js->guest, v);
  if (status == GUEST_STATUS_THROW && !is_err(value)) return js_throw(js, value);
  return value;
}

static ant_value_t guest_child_callback_fn(ant_params_t) {
  ant_guest_t *g = js->guest;
  if (!g || !g->call_host) return js_mkerr(js, "Callback is not attached to an isolate host");

  guest_host_call_t *call = calloc(1, sizeof(*call));
  guest_value_t *encoded = nargs ? calloc((size_t)nargs, sizeof(*encoded)) : NULL;
  if (!call || (nargs && !encoded)) {
    free(call);
    free(encoded);
    return Ant_Error_CreateOutOfMemory(js);
  }

  call->callback = (uint32_t)js_getnum(js_func_data(js->current_func));
  call->args = encoded;
  call->argc = (uint32_t)nargs;
  call->promise = js_mkundef();

  for (int i = 0; i < nargs; i++) {
    ant_value_t r = guest_wire_out(js, args[i], &encoded[i]);
    if (is_err(r)) {
      guest_host_call_free(call);
      return r;
    }
  }

  if (!g->call_host(g, call)) return js_interrupt_raise(js);
  bool terminated = call->status == GUEST_STATUS_DISPOSED || js_interrupt_pending(js);
  ant_value_t result = terminated ? js_mkundef() : guest_host_value_to_child(js, call->status, &call->result);
  guest_host_call_free(call);

  return terminated ? js_interrupt_raise(js) : result;
}

static ant_realm_t *guest_context(ant_guest_t *g, uint32_t id) {
  return id < g->context_count ? g->contexts[id] : NULL;
}

static ant_value_t guest_context_global(ant_t *js, ant_realm_t *realm) {
  return realm == js->realm ? js->global : realm->global;
}

static bool guest_push_context(ant_guest_t *g, ant_realm_t *realm) {
  if (g->context_count == g->context_cap) {
    uint32_t cap = g->context_cap ? g->context_cap * 2 : 4;
    ant_realm_t **contexts = realloc(g->contexts, cap * sizeof(*contexts));
    if (!contexts) return false;
    g->contexts = contexts;
    g->context_cap = cap;
  }
  g->contexts[g->context_count++] = realm;
  return true;
}

static void guest_op_context(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  ant_realm_t *realm = js->main_realm;
  uint32_t id = 0;

  if (call->op == GUEST_OP_CONTEXT) {
    realm = ant_realm_create(js);
    if (!realm || !guest_push_context(g, realm)) {
      guest_call_throw(js, call, js_mkerr(js, "Failed to create a context"));
      return;
    }
    id = g->context_count - 1;
    if (g->globals == GUEST_GLOBALS_BARE)
      js_set(js, realm->global, "console", js_get(js, guest_context_global(js, js->main_realm), "console"));
  }

  ant_value_t r = guest_child_encode(js, g, guest_context_global(js, realm), true, &call->result);
  if (is_err(r)) {
    guest_call_throw(js, call, r);
    return;
  }

  call->result_aux = id;
  call->status = GUEST_STATUS_OK;
}

static ant_value_t guest_compile(ant_t *js, ant_realm_t *realm, const char *source, size_t len, const char *filename) {
  ant_realm_t *prev = ant_realm_switch(js, realm);
  const char *saved = js->filename;
  js->filename = filename ? intern_string(js, filename, strlen(filename)) : "<isolate>";
  ant_value_t compiled = js_vm_script_compile(js, source, len);
  js->filename = saved;
  ant_realm_restore(js, prev);
  return compiled;
}

static ant_value_t guest_run(ant_t *js, ant_realm_t *realm, ant_value_t compiled, uint32_t timeout_ms) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, compiled);

  ant_realm_t *prev = ant_realm_switch(js, realm);
  vm_interrupt_t interrupt = { .js = js, .timeout_ms = timeout_ms };
  vm_interrupt_begin(&interrupt);

  ant_value_t result = js_vm_script_run(js, compiled);
  result = vm_interrupt_end(&interrupt, result);
  ant_realm_restore(js, prev);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static guest_script_t *guest_script(ant_guest_t *g, uint32_t id) {
  return id > 0 && id <= g->script_count && g->scripts[id - 1].source ? &g->scripts[id - 1] : NULL;
}

static void guest_op_compile(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  ant_value_t compiled = guest_compile(js, js->main_realm, call->text, call->text_len, call->filename);
  if (is_err(compiled)) {
    guest_call_throw(js, call, compiled);
    return;
  }

  if (g->script_count == g->script_cap) {
    uint32_t cap = g->script_cap ? g->script_cap * 2 : 8;
    guest_script_t *scripts = realloc(g->scripts, cap * sizeof(*scripts));
    if (!scripts) {
      guest_call_throw(js, call, Ant_Error_CreateOutOfMemory(js));
      return;
    }
    g->scripts = scripts;
    g->script_cap = cap;
  }

  guest_script_t *script = &g->scripts[g->script_count++];
  *script = (guest_script_t){
    .source = call->text, .len = call->text_len, .filename = call->filename,
  };
  call->text = call->filename = NULL;

  call->result_aux = g->script_count;
  call->status = GUEST_STATUS_OK;
}

static ant_value_t guest_script_compiled(ant_t *js, ant_guest_t *g, guest_script_t *script, uint32_t context) {
  if (context < script->compiled_cap && guest_handle_valid(g, script->compiled[context]))
    return g->handles[script->compiled[context]];

  ant_value_t compiled = guest_compile(js, guest_context(g, context), script->source, script->len, script->filename);
  if (is_err(compiled)) return compiled;

  if (context >= script->compiled_cap) {
    uint32_t cap = context + 4;
    uint32_t *cache = realloc(script->compiled, cap * sizeof(*cache));
    if (!cache) return compiled;
    memset(cache + script->compiled_cap, 0, (cap - script->compiled_cap) * sizeof(*cache));
    script->compiled = cache;
    script->compiled_cap = cap;
  }

  script->compiled[context] = guest_handle_put(g, compiled);
  return compiled;
}

static ant_value_t guest_promise_settled(ant_t *js, guest_call_t *call, ant_value_t value, bool fulfilled) {
  ant_guest_t *g = js->guest;
  if (fulfilled) {
    ant_value_t r = guest_child_encode(js, g, value, call->want_ref, &call->result);
    if (is_err(r)) guest_call_throw(js, call, r);
    else call->status = GUEST_STATUS_OK;
  } else guest_call_throw(js, call, value);

  g->complete(g, call);
  return js_mkundef();
}

static ant_value_t guest_promise_fulfilled(ant_params_t) {
  guest_call_t *call = js_get_native(js_getcurrentfunc(js), GUEST_CALL_NATIVE_TAG);
  return call ? guest_promise_settled(js, call, nargs ? args[0] : js_mkundef(), true) : js_mkundef();
}

static ant_value_t guest_promise_rejected(ant_params_t) {
  guest_call_t *call = js_get_native(js_getcurrentfunc(js), GUEST_CALL_NATIVE_TAG);
  return call ? guest_promise_settled(js, call, nargs ? args[0] : js_mkundef(), false) : js_mkundef();
}

static bool guest_finish_value(ant_t *js, ant_guest_t *g, guest_call_t *call, ant_value_t value) {
  if (is_err(value)) {
    guest_call_throw(js, call, value);
    return true;
  }

  if (call->await_promise && vtype(value) == kTypePromise) {
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, value);
    ant_value_t on_ok = js_heavy_mkfun_native(js, guest_promise_fulfilled, call, GUEST_CALL_NATIVE_TAG);
    GC_ROOT_PIN(js, on_ok);
    ant_value_t on_err = js_heavy_mkfun_native(js, guest_promise_rejected, call, GUEST_CALL_NATIVE_TAG);
    GC_ROOT_PIN(js, on_err);
    ant_value_t r = js_promise_then(js, value, on_ok, on_err);
    GC_ROOT_RESTORE(js, mark);
    if (is_err(r)) {
      guest_call_throw(js, call, r);
      return true;
    }
    return false;
  }

  ant_value_t r = guest_child_encode(js, g, value, call->want_ref, &call->result);
  if (is_err(r)) guest_call_throw(js, call, r);
  else call->status = GUEST_STATUS_OK;

  return true;
}

static bool guest_op_run(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  ant_realm_t *realm = guest_context(g, call->context);
  if (!realm) return guest_finish_value(js, g, call, js_mkerr_typed(js, JS_ERR_TYPE, "Context has been released"));

  ant_value_t compiled;
  if (call->op == GUEST_OP_EVAL) compiled = guest_compile(js, realm, call->text, call->text_len, call->filename);
  else {
    guest_script_t *script = guest_script(g, call->target);
    if (!script) return guest_finish_value(js, g, call, js_mkerr_typed(js, JS_ERR_TYPE, "Script has been released"));
    compiled = guest_script_compiled(js, g, script, call->context);
  }

  if (is_err(compiled)) return guest_finish_value(js, g, call, compiled);
  return guest_finish_value(js, g, call, guest_run(js, realm, compiled, call->timeout_ms));
}

static ant_value_t guest_key(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  if (call->argc < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "A property key is required");
  ant_value_t key = guest_child_decode(js, g, &call->args[0]);
  return is_err(key) || vtype(key) == kTypeString ? key : js_tostring_val(js, key);
}

static bool guest_op_property(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  if (!guest_handle_valid(g, call->target)) return guest_finish_value(js, g, call, js_mkerr_typed(js, JS_ERR_TYPE, "Reference has been released"));
  ant_value_t target = g->handles[call->target];
  ant_value_t key = guest_key(js, g, call);
  if (is_err(key)) return guest_finish_value(js, g, call, key);

  const char *name = js_getstr(js, key, NULL);
  if (call->op == GUEST_OP_GET) return guest_finish_value(js, g, call, js_getprop_fallback(js, target, name));

  ant_value_t value = call->argc > 1 ? guest_child_decode(js, g, &call->args[1]) : js_mkundef();
  if (is_err(value)) return guest_finish_value(js, g, call, value);
  js_set(js, target, name, value);

  return guest_finish_value(js, g, call, Ant_Exception_Pending(js) ? js_take_thrown(js, js_mkundef()) : js_mkundef());
}

static bool guest_op_apply(ant_t *js, ant_guest_t *g, guest_call_t *call) {
  if (!guest_handle_valid(g, call->target)) return guest_finish_value(js, g, call, js_mkerr_typed(js, JS_ERR_TYPE, "Reference has been released"));
  ant_value_t fn = g->handles[call->target];
  if (!is_callable(fn)) return guest_finish_value(js, g, call, js_mkerr_typed(js, JS_ERR_TYPE, "Reference is not a function"));

  uint32_t argc = call->argc ? call->argc - 1 : 0;
  ant_value_t *argv = argc ? calloc(argc, sizeof(*argv)) : NULL;
  if (argc && !argv) return guest_finish_value(js, g, call, Ant_Error_CreateOutOfMemory(js));

  GC_ROOT_SAVE(mark, js);
  ant_value_t self = call->argc ? guest_child_decode(js, g, &call->args[0]) : js_mkundef();
  GC_ROOT_PIN(js, self);

  ant_value_t result = is_err(self) ? self : js_mkundef();
  for (uint32_t i = 0; i < argc && !is_err(result); i++) {
    argv[i] = guest_child_decode(js, g, &call->args[i + 1]);
    if (is_err(argv[i])) result = argv[i];
    else GC_ROOT_PIN(js, argv[i]);
  }

  if (!is_err(result)) {
    vm_interrupt_t interrupt = { .js = js, .timeout_ms = call->timeout_ms };
    vm_interrupt_begin(&interrupt);
    result = sv_vm_call(js->vm, js, fn, self, argv, (int)argc, NULL, js_mkundef());
    result = vm_interrupt_end(&interrupt, result);
  }

  GC_ROOT_RESTORE(js, mark);
  free(argv);

  return guest_finish_value(js, g, call, result);
}

static void guest_op_heap(ant_t *js, guest_call_t *call) {
  call->stats[0] = (double)js->obj_arena.committed;
  call->stats[1] = (double)(js->obj_arena.live_count * js->obj_arena.elem_size);
  call->stats[2] = (double)js->obj_arena.limit;
  call->stats[3] = (double)js->obj_arena.live_count;
  call->status = GUEST_STATUS_OK;
}

void guest_child_execute(ant_guest_t *g, guest_call_t *call) {
  ant_t *js = g->child;
  uint64_t cpu_start = guest_thread_cpu_ns();
  uint64_t wall_start = uv_hrtime();
  guest_apply_releases(g);

  bool done = true;
  switch (call->op) {
  case GUEST_OP_CONTEXT:
  case GUEST_OP_MAIN_CONTEXT: guest_op_context(js, g, call); break;
  case GUEST_OP_COMPILE: guest_op_compile(js, g, call); break;
  case GUEST_OP_RUN:
  case GUEST_OP_EVAL: done = guest_op_run(js, g, call); break;
  case GUEST_OP_GET:
  case GUEST_OP_SET: done = guest_op_property(js, g, call); break;
  case GUEST_OP_APPLY: done = guest_op_apply(js, g, call); break;
  case GUEST_OP_COPY:
    done = guest_handle_valid(g, call->target)
      ? guest_finish_value(js, g, call, g->handles[call->target])
      : guest_finish_value(js, g, call, js_mkerr_typed(js, JS_ERR_TYPE, "Reference has been released"));
    break;
  case GUEST_OP_HEAP: guest_op_heap(js, call); break;
  }

  if (Ant_Exception_Pending(js)) js_take_thrown(js, js_mkundef());
  process_microtasks(js);

  atomic_fetch_add_explicit(&g->cpu_ns, guest_thread_cpu_ns() - cpu_start, memory_order_relaxed);
  atomic_fetch_add_explicit(&g->wall_ns, uv_hrtime() - wall_start, memory_order_relaxed);
  if (done) g->complete(g, call);
}

bool guest_child_boot(ant_guest_t *g, ant_t *child) {
  child->guest = g;
  g->child = child;

  bool ok;
  if (g->globals == GUEST_GLOBALS_ANT) {
    static char *argv[] = { "ant", "[isolate]", NULL };
    ant_runtime_init(child, 2, argv, NULL);
    ant_bootstrap_modules(child);
    ok = !Ant_Exception_Pending(child);
  } else ok = ant_bootstrap_bare(child);

  return ok && guest_push_context(g, child->main_realm);
}

void guest_child_teardown(ant_guest_t *g) {
  for (uint32_t i = 0; i < g->script_count; i++) {
    free(g->scripts[i].source);
    free(g->scripts[i].filename);
    free(g->scripts[i].compiled);
  }

  free(g->scripts);
  free(g->handles);
  free(g->handle_live);
  free(g->handle_free_ids);
  free(g->contexts);

  g->scripts = NULL;
  g->handles = NULL;
  g->handle_live = NULL;
  g->handle_free_ids = NULL;
  g->contexts = NULL;
  g->script_count = g->handle_count = g->handle_free = g->context_count = 0;

  if (g->child) g->child->guest = NULL;
}

void guest_child_mark(ant_t *js, gc_mark_fn mark) {
  ant_guest_t *g = js->guest;
  if (!g) return;
  for (uint32_t i = 1; i < g->handle_count; i++) if (g->handle_live[i]) mark(js, g->handles[i]);
  for (uint32_t i = 1; i < g->context_count; i++) ant_realm_reach(js, g->contexts[i], mark);
}
