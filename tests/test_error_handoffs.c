// meson test -C build error-handoffs
#include "internal.h"
#include "errors.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "gc/modules.h"
#include "modules/abort.h"
#include "modules/assert.h"
#include "modules/events.h"
#include "modules/generator.h"
#include "modules/symbol.h"
#include "modules/iterator.h"
#include "modules/timer.h"
#include "sandbox/sandbox.h"
#include "sandbox/transport.h"
#include "silver/call.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static const char *sandbox_expected_display;
static unsigned sandbox_error_frames;

static bool CaptureSandboxErrorFrame(ant_sandbox_frame_type_t type, const void *payload, size_t length) {
  assert(type == ANT_SANDBOX_FRAME_ERROR);
  const char *display = NULL;
  size_t display_len = 0;
  assert(ant_sandbox_error_payload_display(payload, length, &display, &display_len));
  assert(display_len == strlen(sandbox_expected_display));
  assert(memcmp(display, sandbox_expected_display, display_len) == 0);
  sandbox_error_frames++;
  return true;
}

// Exercise the actual uncaught-error sender without a guest transport.
#define ant_sandbox_transport_send_frame CaptureSandboxErrorFrame
#include "../src/sandbox/sandbox.c"
#undef ant_sandbox_transport_send_frame

static ant_value_t expected_reason;
static ant_value_t callback_throw;
static int callback_calls;

_Static_assert(sizeof(ant_value_t) == 8, "native and JIT results must remain 64-bit");
_Static_assert(sizeof(((ant_object_t *)0)->u) == 16,
  "exception records must not enlarge the object union");

static ant_value_t handled_native_failure(ant_params_t) {
  assert(!Ant_Exception_Pending(js));
  ant_value_t failure = js_throw(js, js_mkundef());
  assert(is_err(failure));
  assert(vtype(js_take_thrown(js, failure)) == kTypeUndefined);
  return js_mknum(42);
}

static ant_value_t forgotten_native_failure(ant_params_t) {
  js_throw(js, js_mknull());
  return js_mknum(42);
}

static ant_value_t detached_native_failure(ant_params_t) {
  ant_value_t failure = js_throw(js, js_mknull());
  Ant_Exception_Clear(js);
  return failure;
}

static ant_fixed_arena_t ExhaustArena(ant_fixed_arena_t *arena) {
  ant_fixed_arena_t saved = *arena;
  arena->committed = arena->watermark;
  arena->reserved = arena->watermark;
  arena->free_list = NULL;
  return saved;
}

static void CheckFunctionAllocationFailure(ant_t *js) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t obj = js_mkobj(js);
  GC_ROOT_PIN(js, obj);
  ant_value_t oom = js->exception_oom;
  GC_ROOT_PIN(js, oom);
  ant_value_t native = js_mkfun(handled_native_failure);
  ant_value_t bind = js_getprop_fallback(js, native, "bind");
  assert(vtype(bind) == kTypeBuiltin);
  const char *source = "globalThis.__allocationProxy = new Proxy(function () {}, {});";
  assert(!is_err(js_eval_bytecode(js, source, strlen(source))));
  ant_value_t proxy = js_get(js, js->global, "__allocationProxy");
  assert(!is_err(proxy) && is_callable(proxy));
  GC_ROOT_PIN(js, proxy);

  // Force arena exhaustion without allocating large amounts of memory or
  // adding an allocation-failure hook to the runtime. GC must not reuse slots
  // or inspect the temporarily restricted arenas.
  bool saved_gc_disabled = gc_disabled;
  gc_disabled = true;
  ant_fixed_arena_t closures = ExhaustArena(&js->closure_arena);
  for (int has_oom = 0; has_oom < 2; has_oom++) {
    js->exception_oom = has_oom ? oom : js_mkundef();
    for (int pending = 0; pending < 2; pending++) {
      ant_value_t previous = pending ? js_throw(js, js_mknum(73)) : js_mkundef();
      ant_value_t result = js_obj_to_func_ex(js, obj, SV_CALL_HAS_BOUND_THIS);
      assert(is_err(result) && Ant_Exception_Peek(js) == result);
      if (pending) assert(result == previous);
      else if (has_oom) assert(result == oom);
      else {
        ant_value_t value = Ant_Exception_Value(js, result);
        ant_value_t name = js_get(js, value, "name");
        assert(vtype(name) == kTypeString);
        assert(strcmp(js_getstr(js, name, NULL), "TypeError") == 0);
      }
      Ant_Exception_Clear(js);
    }
  }

  // If even the fallback Error cannot be allocated, return a tagged failure.
  js->exception_oom = js_mkundef();
  ant_fixed_arena_t objects = ExhaustArena(&js->obj_arena);
  ant_value_t result = js_obj_to_func_ex(js, obj, 0);
  js->obj_arena = objects;
  assert(is_err(result) && Ant_Exception_Peek(js) == result);
  Ant_Exception_Clear(js);
  js->exception_oom = oom;

  ant_value_t saved_this = js->this_val;
  ant_value_t targets[] = { native, proxy };
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    js->this_val = targets[i];
    result = js_as_cfunc(bind)(js, NULL, 0, js_mkundef());
    assert(result == oom && Ant_Exception_Peek(js) == result);
    Ant_Exception_Clear(js);
  }
  js->this_val = saved_this;
  js->closure_arena = closures;
  gc_disabled = saved_gc_disabled;
  assert(vtype(js_obj_to_func_ex(js, obj, 0)) == kTypeFunction);
  assert(!Ant_Exception_Pending(js));
  GC_ROOT_RESTORE(js, mark);
}

static ant_value_t collect_native_target(ant_params_t) {
  assert(!Ant_Exception_Pending(js));
  assert(js->vm->native_frame && js->vm->native_frame->new_target == call_new_target);
  gc_run(js);
  assert(gc_obj_is_marked(js_obj_ptr(call_new_target)));
  return call_new_target;
}

static ant_value_t collect_finally_completion(ant_params_t) {
  sv_vm_t *vm = js->vm;
  assert(vm->handler_depth > 0);
  sv_handler_t *handler = &vm->handler_stack[vm->handler_depth - 1];
  assert(handler->kind == SV_HANDLER_FINALLY);
  assert(handler->completion.kind == SV_COMPLETION_RETURN);
  ant_value_t value = handler->completion.value;
  gc_run_minor(js);
  assert(gc_obj_is_marked(js_obj_ptr(value)));
  gc_run(js);
  assert(gc_obj_is_marked(js_obj_ptr(value)));
  assert(js_get(js, value, "answer") == js_mknum(42));
  return js_mkundef();
}

static void CheckFinallyCompletionRoots(ant_t *js) {
  init_symbol_module(js);
  init_intrinsic_symbols(js);
  init_iterator_module(js);
  init_generator_module(js);
  js_set(js, js->global, "__collectFinally", js_mkfun(collect_finally_completion));
  uintptr_t stack_base = (uintptr_t)js->cstk.main_base;
  js_setstackbase(js, NULL);
  const char *active =
    "globalThis.__finallyResult = (() => {"
    "try { return {answer: 42}; } finally {"
    "try { throw 0; } catch {} __collectFinally(); } })();";
  assert(!is_err(js_eval_bytecode(js, active, strlen(active))));
  assert(js_get(js, js_get(js, js->global, "__finallyResult"), "answer") == js_mknum(42));

  const char *suspended =
    "globalThis.__finallyGenerator = (function*() {"
    "yield 0;"
    "try { return {answer: 42}; } finally {"
    "yield 1; try { throw 0; } catch {} } })();"
    "__finallyGenerator.next();";
  ant_value_t status = js_eval_bytecode(js, suspended, strlen(suspended));
  if (is_err(status)) print_error_value(js, status, js_mkundef(), "suspended finally: ");
  assert(!is_err(status));
  gc_run_minor(js);
  gc_run(js);
  const char *enter_finally = "__finallyGenerator.next();";
  assert(!is_err(js_eval_bytecode(js, enter_finally, strlen(enter_finally))));
  gc_run_minor(js);
  gc_run(js);
  const char *resume = "globalThis.__finallyResult = __finallyGenerator.next().value;";
  assert(!is_err(js_eval_bytecode(js, resume, strlen(resume))));
  ant_value_t value = js_get(js, js->global, "__finallyResult");
  assert(gc_obj_is_marked(js_obj_ptr(value)));
  assert(js_get(js, value, "answer") == js_mknum(42));
  js_setstackbase(js, (void *)stack_base);
}

static coroutine_t *captured_await_coro;

static ant_value_t capture_await_coroutine(ant_params_t) {
  assert(js->active_async_coro && !captured_await_coro);
  captured_await_coro = js->active_async_coro;
  coroutine_retain(captured_await_coro);
  return js_mkundef();
}

static ant_value_t StartTestAwait(ant_t *js, ant_value_t value, coroutine_t **coro) {
  js_set(js, js->global, "__captureAwaitCoro", js_mkfun(capture_await_coroutine));
  js_set(js, js->global, "__awaitInput", value);
  const char *source =
    "globalThis.__awaitOutput = (async function() {"
    "__captureAwaitCoro(); const value = await __awaitInput;"
    "globalThis.__awaitResumed = true; return value; })();";
  js_set(js, js->global, "__awaitResumed", js_false);
  bool was_draining = js->microtasks_draining;
  js->microtasks_draining = true;
  ant_value_t status = js_eval_bytecode(js, source, strlen(source));
  js->microtasks_draining = was_draining;
  assert(!is_err(status));
  assert(captured_await_coro && captured_await_coro->await_registered);
  assert(captured_await_coro->act && captured_await_coro->act->frame_count > 0);
  *coro = captured_await_coro;
  captured_await_coro = NULL;
  ant_value_t output = js_get(js, js->global, "__awaitOutput");
  assert(vtype(output) == kTypePromise);
  promise_mark_handled(output);
  js_set(js, js->global, "__awaitInput", js_mkundef());
  js_set(js, js->global, "__awaitOutput", js_mkundef());
  return output;
}

static void CheckAwaitValueRoots(ant_t *js) {
  for (int input_kind = 0; input_kind < 3; input_kind++) {
  for (int cancel = 0; cancel < 2; cancel++) {
    GC_ROOT_SAVE(mark, js);
    ant_value_t output = js_mkundef();
    GC_ROOT_PIN(js, output);
    GC_ROOT_SAVE(input_mark, js);
    ant_value_t awaited = js_mkpromise(js);
    GC_ROOT_PIN(js, awaited);
    ant_value_t value = js_mkobj(js);
    GC_ROOT_PIN(js, value);
    js_set(js, value, "answer", js_mknum(42));
    if (input_kind == 0) js_resolve_promise(js, awaited, value);
    else if (input_kind == 2) js_reject_promise(js, awaited, value);

    coroutine_t *coro = NULL;
    output = StartTestAwait(js, awaited, &coro);
    assert(js_promise_get_settlement(js, output, NULL) == JS_PROMISE_PENDING);
    if (input_kind == 1) js_resolve_promise(js, awaited, value);
    GC_ROOT_RESTORE(js, input_mark);

    uintptr_t stack_base = (uintptr_t)js->cstk.main_base;
    js_setstackbase(js, NULL);
    gc_run_minor(js);
    gc_run(js);
    assert(gc_obj_is_marked(js_obj_ptr(value)));
    assert(js_get(js, value, "answer") == js_mknum(42));
    if (cancel) {
      assert(coroutine_cancel(coro));
      if (input_kind == 0) {
        // The cancelled direct job still owns and traces its queued value.
        gc_run_minor(js);
        gc_run(js);
        assert(gc_obj_is_marked(js_obj_ptr(value)));
      }
    }
    process_microtasks(js);
    assert(!coro->await_registered);
    ant_value_t resumed = js_mkundef();
    js_promise_settlement_t state = js_promise_get_settlement(js, output, &resumed);
    assert(state == (cancel ? JS_PROMISE_PENDING :
      input_kind == 2 ? JS_PROMISE_REJECTED : JS_PROMISE_FULFILLED));
    assert(js_get(js, js->global, "__awaitResumed") == js_bool(!cancel && input_kind != 2));
    if (!cancel) assert(resumed == value && js_get(js, resumed, "answer") == js_mknum(42));
    assert(coro->refcount == 1);
    coroutine_release(coro);
    js_setstackbase(js, (void *)stack_base);
    GC_ROOT_RESTORE(js, mark);
  }}
}

static void RegisterTestAwait(ant_t *js, coroutine_t *coro, ant_value_t value) {
  if (vtype(value) == kTypePromise) {
    js_await_result_t result = js_promise_await_coroutine(js, value, coro);
    assert(result.state == JS_AWAIT_PENDING);
  } else {
    assert(queue_await_resume_job(coro, value));
    coro->awaited_promise = js_mkundef();
    coro->await_registered = true;
  }
}

static void CheckCancelledAwaitReplacement(ant_t *js) {
  // Cancel a queued primitive/fulfilled await before its job drains, then
  // replace it with a primitive, fulfilled, pending, or rejected await.
  for (int first_kind = 0; first_kind < 2; first_kind++) {
    for (int next_kind = 0; next_kind < 4; next_kind++) {
      GC_ROOT_SAVE(mark, js);
      ant_value_t first = js_mknum(11);
      ant_value_t next = js_mknum(22);
      GC_ROOT_PIN(js, first);
      GC_ROOT_PIN(js, next);

      if (first_kind == 1) {
        first = js_mkpromise(js);
        assert(!is_err(first));
        js_resolve_promise(js, first, js_mknum(11));
      }
      coroutine_t *coro = NULL;
      ant_value_t output = StartTestAwait(js, first, &coro);
      GC_ROOT_PIN(js, output);
      assert(coroutine_cancel(coro));

      if (next_kind != 0) {
        next = js_mkpromise(js);
        assert(!is_err(next));
        if (next_kind == 1) js_resolve_promise(js, next, js_mknum(22));
        else if (next_kind == 3) js_reject_promise(js, next, js_mknum(22));
      }
      RegisterTestAwait(js, coro, next);
      process_microtasks(js);

      if (next_kind == 2) {
        // The stale job must neither settle nor detach the pending await.
        assert(coro->await_registered);
        assert(js_promise_get_settlement(js, output, NULL) == JS_PROMISE_PENDING);
        js_resolve_promise(js, next, js_mknum(22));
        process_microtasks(js);
      }
      assert(!coro->await_registered);
      ant_value_t resumed = js_mkundef();
      js_promise_settlement_t state = js_promise_get_settlement(js, output, &resumed);
      assert(state == (next_kind == 3 ? JS_PROMISE_REJECTED : JS_PROMISE_FULFILLED));
      assert(resumed == js_mknum(22));
      assert(coro->refcount == 1);
      coroutine_release(coro);
      GC_ROOT_RESTORE(js, mark);
    }
  }
}

static void CheckExplicitAwaitResume(ant_t *js) {
  for (int rejected = 0; rejected < 2; rejected++) {
    GC_ROOT_SAVE(mark, js);
    coroutine_t *coro = NULL;
    ant_value_t output = StartTestAwait(js, js_mknum(11), &coro);
    GC_ROOT_PIN(js, output);
    assert(coroutine_cancel(coro));

    ant_value_t value = js_mkobj(js);
    GC_ROOT_PIN(js, value);
    js_set(js, value, "answer", js_mknum(42));
    Ant_Coroutine_SettleAndResume(js, coro, value, rejected);
    process_microtasks(js);

    ant_value_t resumed = js_mkundef();
    js_promise_settlement_t state = js_promise_get_settlement(js, output, &resumed);
    assert(state == (rejected ? JS_PROMISE_REJECTED : JS_PROMISE_FULFILLED));
    assert(resumed == value && !Ant_Exception_Pending(js));
    assert(coro->refcount == 1);
    coroutine_release(coro);
    GC_ROOT_RESTORE(js, mark);
  }
}

static ant_value_t check_callback(ant_params_t) {
  assert(!Ant_Exception_Pending(js));
  assert(nargs > 0 && args[0] == expected_reason);
  callback_calls++;
  return js_mkundef();
}

static ant_value_t throwing_callback(ant_params_t) {
  check_callback(js, args, nargs, call_new_target);
  return js_throw(js, callback_throw);
}

static ant_value_t constructor_getter(ant_params_t) {
  return js_throw(js, expected_reason);
}

static ant_value_t promise(ant_t *js) {
  ant_value_t p = js_mkpromise(js);
  promise_mark_handled(p);
  return p;
}

static void check_rejected(ant_value_t p, ant_value_t reason) {
  ant_promise_state_t *state = js_obj_ptr(js_as_obj(p))->promise_state;
  assert(state && state->state == 2 && state->value == reason);
}

static ant_value_t once_signal, once_saved_abort;
static unsigned once_abort_edges, once_removes, once_unrelated_aborts;

static void InspectOnceAbortListener(ant_t *js, ant_value_t value) {
  if (!is_callable(value)) return;
  once_abort_edges++;
  if (vtype(value) == kTypeFunction) once_saved_abort = value;
}

static ant_value_t FailOnceAttachment(ant_params_t) {
  once_abort_edges = 0;
  gc_mark_abort_signal_object(js, once_signal, InspectOnceAbortListener);
  assert(once_abort_edges == 2 && is_callable(once_saved_abort));
  return js_throw(js, expected_reason);
}

static ant_value_t RemoveOnceListener(ant_params_t) {
  once_removes++;
  return js_mkundef();
}

static ant_value_t UnrelatedAbortListener(ant_params_t) {
  once_unrelated_aborts++;
  return js_mkundef();
}

static void CheckOnceAttachmentCleanup(ant_t *js) {
  init_abort_module(js);
  GC_ROOT_SAVE(mark, js);
  ant_value_t events = events_library(js);
  GC_ROOT_PIN(js, events);
  ant_value_t once = js_get(js, events, "once");
  assert(vtype(once) == kTypeBuiltin);
  ant_value_t reason = js_mkobj(js);
  GC_ROOT_PIN(js, reason);
  ant_value_t event_name = js_mkstr(js, "ready", 5);
  GC_ROOT_PIN(js, event_name);
  ant_value_t reasons[] = { js_mkundef(), js_mknull(), reason };
  const struct { const char *name; bool getter; } failures[] = {
    { "on", true }, { "once", true }, { "once", false }
  };

  for (size_t failure = 0; failure < sizeof(failures) / sizeof(failures[0]); failure++) {
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
      GC_ROOT_SAVE(case_mark, js);
      once_signal = abort_signal_create_dependent(js, js_mkundef());
      assert(abort_signal_is_signal(once_signal));
      GC_ROOT_PIN(js, once_signal);
      once_saved_abort = js_mkundef();
      GC_ROOT_PIN(js, once_saved_abort);
      ant_value_t target = js_mkobj(js);
      GC_ROOT_PIN(js, target);
      ant_value_t options = js_mkobj(js);
      GC_ROOT_PIN(js, options);
      js_set(js, options, "signal", once_signal);
      js_set(js, target, "removeListener", js_mkfun(RemoveOnceListener));
      js_set(js, target, "on", js_mkfun(handled_native_failure));
      const char *name = failures[failure].name;
      if (failures[failure].getter)
        js_set_getter_desc(js, target, name, strlen(name), js_mkfun(FailOnceAttachment), JS_DESC_C);
      else js_set(js, target, name, js_mkfun(FailOnceAttachment));

      abort_signal_add_listener(js, once_signal, js_mkfun(UnrelatedAbortListener));
      expected_reason = reasons[i];
      once_removes = once_unrelated_aborts = 0;
      ant_value_t args[] = { target, event_name, options };
      ant_value_t p = js_as_cfunc(once)(js, args, 3, js_mkundef());
      GC_ROOT_PIN(js, p);
      assert(vtype(p) == kTypePromise && !Ant_Exception_Pending(js));
      promise_mark_handled(p);
      check_rejected(p, expected_reason);

      // The signal must release our listener and retain unrelated listeners.
      once_abort_edges = 0;
      gc_mark_abort_signal_object(js, once_signal, InspectOnceAbortListener);
      assert(once_abort_edges == 1);
      // Even a saved callback must ignore the settled, failed operation.
      ant_value_t result = sv_vm_call(js->vm, js, once_saved_abort, once_signal, NULL, 0, NULL, js_mkundef());
      assert(!is_err(result) && !Ant_Exception_Pending(js) && once_removes == 0);
      signal_do_abort(js, once_signal, js_mknull());
      assert(once_unrelated_aborts == 1 && once_removes == 0);
      check_rejected(p, expected_reason);
      GC_ROOT_RESTORE(js, case_mark);
    }
  }
  GC_ROOT_RESTORE(js, mark);
}

static ant_value_t retained_completion(ant_t *js) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t value = js_mkobj(js);
  GC_ROOT_PIN(js, value);
  js_set(js, value, "answer", js_mknum(42));
  ant_value_t stack = js_mkstr(js, "retained stack", 14);
  ant_value_t completion = Ant_Exception_Raise(js, value, stack);
  Ant_Exception_Clear(js);
  GC_ROOT_RESTORE(js, mark);
  return completion;
}

static void CheckSandboxDiagnostic(
  ant_t *js, ant_value_t value, ant_value_t fallback_stack,
  const char *name, const char *message, const char *stack, const char *display_part
) {
  ant_value_t pending = Ant_Exception_Peek(js);
  size_t length = 0;
  uint8_t *payload = ant_sandbox_build_error_payload(js, value, fallback_stack, &length);
  assert(payload && Ant_Exception_Peek(js) == pending);
  const char *expected[] = {name, message, stack};
  size_t offset = 0;
  for (size_t i = 0; i < 3; i++) {
    assert(offset + 4 <= length);
    const uint8_t *p = payload + offset;
    uint32_t size = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    offset += 4;
    assert(size == strlen(expected[i]) && size <= length - offset);
    assert(memcmp(payload + offset, expected[i], size) == 0);
    offset += size;
  }
  const char *display;
  size_t display_len;
  assert(ant_sandbox_error_payload_display(payload, length, &display, &display_len));
  char *text = malloc(display_len + 1);
  assert(text);
  memcpy(text, display, display_len);
  text[display_len] = '\0';
  if (!strstr(text, display_part))
    fprintf(stderr, "sandbox display '%s' does not contain '%s'\n", text, display_part);
  assert(strstr(text, display_part));
  free(text);
  free(payload);
}

static void CheckSandboxExceptionRecords(ant_t *js) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t error = Ant_Error_Create(js, JS_ERR_TYPE | JS_ERR_NO_STACK, "sandbox diagnostic");
  GC_ROOT_PIN(js, error);
  ant_value_t stack = js_mkstr(js, "captured sandbox stack", 22);
  GC_ROOT_PIN(js, stack);
  ant_value_t record = Ant_Exception_Raise(js, error, stack);
  GC_ROOT_PIN(js, record);
  // Serialize an older record while a different completion is current.
  Ant_Exception_Raise(js, js_mknum(73), js_mkundef());
  CheckSandboxDiagnostic(js, record, js_mkundef(), "TypeError", "sandbox diagnostic",
    "captured sandbox stack", "captured sandbox stack");
  record = Ant_Exception_Raise(js, error, js_mkundef());
  Ant_Exception_Clear(js);
  CheckSandboxDiagnostic(js, record, js_mkundef(), "TypeError", "sandbox diagnostic", "", "sandbox diagnostic");
  CheckSandboxDiagnostic(js, record, stack, "TypeError", "sandbox diagnostic",
    "captured sandbox stack", "captured sandbox stack");
  CheckSandboxDiagnostic(js, error, js_mkundef(), "TypeError", "sandbox diagnostic", "", "");
  record = Ant_Exception_Raise(js, js_mknum(73), stack);
  Ant_Exception_Clear(js);
  CheckSandboxDiagnostic(js, record, js_mkundef(), "Error", "73", "captured sandbox stack", "captured sandbox stack");
  record = Ant_Exception_Raise(js, js_mkundef(), js_mkundef());
  Ant_Exception_Clear(js);
  CheckSandboxDiagnostic(js, record, js_mkundef(), "Error", "undefined", "", "undefined");
  GC_ROOT_RESTORE(js, mark);
}

static void CheckSandboxUncaughtFrames(ant_t *js) {
  GC_ROOT_SAVE(mark, js);
  const char *captured = "captured sandbox stack";
  ant_value_t stack = js_mkstr(js, captured, strlen(captured));
  GC_ROOT_PIN(js, stack);
  ant_value_t error = Ant_Error_Create(js, JS_ERR_TYPE | JS_ERR_NO_STACK, "sandbox diagnostic");
  GC_ROOT_PIN(js, error);
  ant_value_t values[] = { js_mknum(73), error, error };

  sandbox_error_frames = 0;
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    sandbox_expected_display = i < 2 ? captured : "sandbox diagnostic";
    Ant_Exception_Raise(js, values[i], i < 2 ? stack : js_mkundef());
    assert(sandbox_send_uncaught_throw(js));
    assert(!Ant_Exception_Pending(js) && sandbox_error_frames == i + 1);
    assert(!sandbox_send_uncaught_throw(js));
    assert(sandbox_error_frames == i + 1);
  }
  GC_ROOT_RESTORE(js, mark);
}

static void CheckFormattedErrorValues(ant_t *js) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t error = js_mkundef();
  GC_ROOT_PIN(js, error);
  for (int pending = 0; pending < 2; pending++) {
    ant_value_t previous = pending ? js_throw(js, js_mkundef()) : js_mkundef();
    js_err_type_t type = pending ? JS_ERR_TYPE | JS_ERR_NO_STACK : JS_ERR_GENERIC;
    error = Ant_Error_CreateFormatted(js, type, "%s %d%%", "literal %s", 25);
    assert(vtype(error) == kTypeObject && Ant_Exception_Peek(js) == previous);
    assert(strcmp(js_getstr(js, js_get(js, error, "message"), NULL), "literal %s 25%") == 0);
    assert(strcmp(js_getstr(js, js_get(js, error, "name"), NULL), pending ? "TypeError" : "Error") == 0);
    assert(vtype(js_get(js, error, "stack")) == (pending ? kTypeUndefined : kTypeString));
    Ant_Exception_Clear(js);
  }
  GC_ROOT_RESTORE(js, mark);
}

static void CheckErrorMessage(ant_t *js, ant_value_t error, const char *expected, size_t length) {
  assert(vtype(error) == kTypeObject);
  size_t actual_length = 0;
  const char *message = js_getstr(js, js_get(js, error, "message"), &actual_length);
  assert(message && actual_length == length && memcmp(message, expected, length) == 0);
}

static void CheckLongErrorMessages(ant_t *js) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t previous = js_throw(js, js_mkundef());
  GC_ROOT_PIN(js, previous);
  ant_value_t props = js_mkobj(js);
  GC_ROOT_PIN(js, props);
  js_set(js, props, "code", js_mknum(73));
  ant_value_t error = js_mkundef();
  GC_ROOT_PIN(js, error);

  const size_t lengths[] = {0, 251, 252, 253, 255, 256, 257, 4096, 16384, 20000};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    size_t length = lengths[i];
    char *message = malloc(length + 1);
    char *formatted = malloc(length + 4);
    assert(message && formatted);
    for (size_t j = 0; j < length; j++) message[j] = j % 11 == 0 ? '%' : 'x';
    message[length] = '\0';
    memcpy(formatted, message, length);
    memcpy(formatted + length, "/7%", 4);

    error = Ant_Error_Create(js, JS_ERR_GENERIC | JS_ERR_NO_STACK, message);
    CheckErrorMessage(js, error, message, length);
    assert(Ant_Exception_Peek(js) == previous);

    size_t payload_len = 0;
    uint8_t *payload = ant_sandbox_build_error_payload(js, error, js_mkundef(), &payload_len);
    assert(payload);
    error = ant_sandbox_decode_error_value(js, payload, payload_len);
    memset(payload, 0, payload_len);
    free(payload);
    CheckErrorMessage(js, error, message, length);
    assert(Ant_Exception_Peek(js) == previous);

    error = Ant_Error_CreateFormatted(js, JS_ERR_TYPE, "%s/%d%%", message, 7);
    CheckErrorMessage(js, error, formatted, length + 3);
    assert(Ant_Exception_Peek(js) == previous);
    const char *stack = js_getstr(js, js_get(js, error, "stack"), NULL);
    assert(stack && strstr(stack, formatted));

    error = js_create_error(js, JS_ERR_RANGE | JS_ERR_NO_STACK, props, "%s/%d%%", message, 7);
    assert(is_err(error) && Ant_Exception_Peek(js) == error);
    ant_value_t value = Ant_Exception_Value(js, error);
    CheckErrorMessage(js, value, formatted, length + 3);
    assert(js_get(js, value, "code") == js_mknum(73));
    assert(strcmp(js_getstr(js, js_get(js, value, "name"), NULL), "RangeError") == 0);
    js_take_thrown(js, error);
    Ant_Exception_Set(js, previous);
    free(formatted);
    free(message);
  }

  // An unencodable wide character must produce a construction failure, not
  // a truncated/uninitialized message or recursive printf-based error creation.
  error = Ant_Error_CreateFormatted(js, JS_ERR_GENERIC, "%lc", (wint_t)0xd800);
  assert(is_err(error) && Ant_Exception_Peek(js) == error);
  const char *failure = "failed to format error message";
  CheckErrorMessage(js, Ant_Exception_Value(js, error), failure, strlen(failure));
  Ant_Exception_Clear(js);
  GC_ROOT_RESTORE(js, mark);
}

int main(void) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  Ant_Exception_Set(js, js_true);
  assert(!Ant_Exception_Pending(js) && Ant_Exception_Peek(js) == js_mkundef());
  Ant_Exception_Set(js, js_mknum(7));
  assert(!Ant_Exception_Pending(js) && Ant_Exception_Peek(js) == js_mkundef());
  js_setstackbase(js, &stack_base);
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, expected_reason);
  GC_ROOT_PIN(js, callback_throw);

  ant_value_t p = promise(js);
  GC_ROOT_PIN(js, p);
  ant_value_t marker = js_mkerr(js, "delivered error");
  GC_ROOT_PIN(js, marker);
  ant_value_t error = Ant_Exception_Value(js, Ant_Exception_Peek(js));
  GC_ROOT_PIN(js, error);
  js_reject_promise(js, p, marker);
  assert(!Ant_Exception_Pending(js) && vtype(Ant_Exception_Stack(js, Ant_Exception_Peek(js))) == kTypeUndefined);
  check_rejected(p, error);

  // Ordinary rejection values are not permission to consume an active throw.
  p = promise(js);
  js_throw(js, error);
  js_reject_promise(js, p, error);
  assert(Ant_Exception_Pending(js) && Ant_Exception_Value(js, Ant_Exception_Peek(js)) == error);
  check_rejected(p, error);
  js_take_thrown(js, js_mkundef());

  // Delivering an old payload marker must preserve a newer pending exception.
  p = promise(js);
  callback_throw = js_mkstr(js, "unrelated", 9);
  js_throw(js, callback_throw);
  ant_value_t saved_stack = Ant_Exception_Stack(js, Ant_Exception_Peek(js));
  js_reject_promise(js, p, marker);
  assert(Ant_Exception_Pending(js) && Ant_Exception_Value(js, Ant_Exception_Peek(js)) == callback_throw && Ant_Exception_Stack(js, Ant_Exception_Peek(js)) == saved_stack);
  check_rejected(p, error);
  js_take_thrown(js, js_mkundef());

  ant_value_t reasons[] = { js_mkundef(), js_mknull(), js_false, js_mknum(17), error };
  for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
    expected_reason = reasons[i];
    p = promise(js);
    marker = js_throw(js, expected_reason);
    js_reject_promise(js, p, marker);
    assert(!Ant_Exception_Pending(js) && vtype(Ant_Exception_Stack(js, Ant_Exception_Peek(js))) == kTypeUndefined);
    check_rejected(p, expected_reason);

    // A settled Promise still consumes a delivered completion exactly once.
    marker = js_throw(js, expected_reason);
    js_reject_promise(js, p, marker);
    assert(!Ant_Exception_Pending(js));
    check_rejected(p, expected_reason);

    ant_value_t args[9] = { js_throw(js, expected_reason), js_mknum(42) };
    ant_value_t original = args[0];
    ant_value_t result = Ant_Error_CallCallback(js, js_mkfun(check_callback), js_mkundef(), args, 9);
    assert(!is_err(result) && !Ant_Exception_Pending(js) && args[0] == original && args[1] == js_mknum(42));
    callback_throw = expected_reason;
    args[0] = js_throw(js, expected_reason);
    result = Ant_Error_CallCallback(js, js_mkfun(throwing_callback), js_mkundef(), args, 2);
    assert(is_err(result) && Ant_Exception_Pending(js) && Ant_Exception_Value(js, Ant_Exception_Peek(js)) == callback_throw);
    js_take_thrown(js, result);
  }
  assert(callback_calls == 10);

  p = promise(js);
  js_resolve_promise(js, p, js_mknum(99));
  js_reject_promise(js, p, js_throw(js, js_mkundef()));
  ant_promise_state_t *settled = js_obj_ptr(js_as_obj(p))->promise_state;
  assert(!Ant_Exception_Pending(js) && settled->state == 1 && settled->value == js_mknum(99));

  // Internal observation queues setup failures; it does not call the owner inline.
  expected_reason = js_mkundef();
  p = promise(js);
  js_set_getter_desc(js, js_as_obj(p), "constructor", 11, js_mkfun(constructor_getter), JS_DESC_C);
  Ant_Promise_Observe(js, p, js_mkundef(), js_mkfun(check_callback));
  assert(!Ant_Exception_Pending(js) && callback_calls == 10);
  process_microtasks(js);
  assert(callback_calls == 11 && !Ant_Exception_Pending(js));

  // A completion retains its own payload and stack after another throw and
  // after the ambient propagation handle has been cleared.
  ant_value_t first = js_throw(js, js_mkundef());
  GC_ROOT_PIN(js, first);
  ant_value_t first_stack = Ant_Exception_Stack(js, first);
  GC_ROOT_PIN(js, first_stack);
  ant_value_t second = js_throw(js, js_mknull());
  GC_ROOT_PIN(js, second);
  assert(first != second && gc_value_is_heap_ref(first));
  assert(!gc_value_is_heap_ref(SV_JIT_RETRY_INTERP));
  assert(vtype(Ant_Exception_Value(js, first)) == kTypeUndefined);
  assert(vtype(js_take_thrown(js, first)) == kTypeUndefined);
  assert(Ant_Exception_Peek(js) == second);
  Ant_Exception_Clear(js);
  assert(vtype(Ant_Exception_Value(js, second)) == kTypeNull);
  assert(Ant_Exception_Stack(js, first) == first_stack);

  first = js_throw(js, error);
  second = js_throw(js, error);
  assert(first != second);
  assert(js_take_thrown(js, first) == error && Ant_Exception_Peek(js) == second);
  ant_value_t ordinary = Ant_Error_Create(js, JS_ERR_TYPE, "ordinary value");
  assert(!is_err(ordinary) && Ant_Exception_Peek(js) == second);

  ant_value_t success = sv_invoke_native(js, handled_native_failure, NULL, 0, js_mkundef());
  assert(success == js_mknum(42) && Ant_Exception_Peek(js) == second);
  Ant_Exception_Clear(js);
  ant_value_t escaped = sv_invoke_native(js, forgotten_native_failure, NULL, 0, js_mkundef());
  assert(is_err(escaped) && Ant_Exception_Peek(js) == escaped);
  assert(vtype(js_take_thrown(js, escaped)) == kTypeNull);

  // Both entry paths must publish a returned record even when native code has
  // already cleared its current handle. An escaped record supersedes a parent.
  for (int scoped = 0; scoped < 2; scoped++) {
    if (scoped) js_throw(js, error);
    escaped = sv_invoke_native(js, detached_native_failure, NULL, 0, js_mkundef());
    assert(is_err(escaped) && Ant_Exception_Peek(js) == escaped);
    assert(vtype(js_take_thrown(js, escaped)) == kTypeNull);
  }

  // Constructor targets need the separate native frame even without an outer
  // exception. Exercise both reasons for scoped entry without C-stack roots.
  for (int pending = 0; pending < 2; pending++) {
    ant_value_t caller_exception = pending ? js_throw(js, error) : js_mkundef();
    ant_value_t target = js_mkobj(js);
    sv_native_frame_t *parent_frame = js->vm->native_frame;
    js_setstackbase(js, NULL);
    success = sv_invoke_native(js, collect_native_target, NULL, 0, target);
    js_setstackbase(js, &stack_base);
    assert(success == target && js->vm->native_frame == parent_frame);
    assert(Ant_Exception_Peek(js) == caller_exception);
    if (pending) assert(gc_obj_is_marked(js_obj_ptr(js_as_obj(caller_exception))));
    Ant_Exception_Clear(js);
  }

  // The VM's builtin fast path must enforce the same scope as other native
  // entries, including when it is called without an active JS frame.
  second = js_throw(js, error);
  success = sv_vm_call(js->vm, js, js_mkfun(handled_native_failure), js_mkundef(), NULL, 0, NULL, js_mkundef());
  assert(success == js_mknum(42) && Ant_Exception_Peek(js) == second);
  Ant_Exception_Clear(js);
  escaped = sv_vm_call(js->vm, js, js_mkfun(forgotten_native_failure), js_mkundef(), NULL, 0, NULL, js_mkundef());
  assert(is_err(escaped) && Ant_Exception_Peek(js) == escaped);
  assert(vtype(js_take_thrown(js, escaped)) == kTypeNull);

  // Only the completion owns these heap edges: neither pending state nor
  // conservative C-stack scanning may keep its payload and stack alive.
  ant_value_t retained = retained_completion(js);
  GC_ROOT_PIN(js, retained);
  js_setstackbase(js, NULL);
  for (int i = 0; i < 2; i++) {
    if (i == 0) gc_run_minor(js);
    else gc_run(js);
    ant_value_t value = Ant_Exception_Value(js, retained);
    assert(gc_obj_is_marked(js_obj_ptr(value)));
    assert(js_get(js, value, "answer") == js_mknum(42));
    size_t stack_len = 0;
    const char *stack = js_getstr(js, Ant_Exception_Stack(js, retained), &stack_len);
    assert(stack && stack_len == 14 && memcmp(stack, "retained stack", 14) == 0);
    assert(!Ant_Exception_Pending(js));
  }
  js_setstackbase(js, &stack_base);

  CheckSandboxExceptionRecords(js);
  CheckSandboxUncaughtFrames(js);
  CheckFormattedErrorValues(js);
  CheckLongErrorMessages(js);
  CheckFinallyCompletionRoots(js);
  CheckAwaitValueRoots(js);
  CheckCancelledAwaitReplacement(js);
  CheckExplicitAwaitResume(js);
  CheckFunctionAllocationFailure(js);
  CheckOnceAttachmentCleanup(js);
  GC_ROOT_RESTORE(js, root_mark);
  js_destroy(js);
  puts("PASS error handoffs preserve values and exception ownership");
}
