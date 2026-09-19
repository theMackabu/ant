# JIT And GC Invariants

Status: active
Last reviewed: 2026-09-10
Owner: theMackabu

Read the section relevant to a JIT, cache, or heap-lifetime change. These rules
were checked against current source when extracted from the
[fable landing history](../exec-plans/completed/fable-perf-fixes-landing.md).
Use [ARCHITECTURE.md](../../ARCHITECTURE.md) for subsystem placement and
[testing.md](testing.md) for validation scope.

## Exception Completions And Async Ownership

Native and JIT calls return one 64-bit `ant_value_t`. A `kTypeError` result
contains a GC-managed exception record with the exact thrown value and captured
stack. `js_throw` and `js_mkerr*` allocate distinct records even when they throw
the same value; forwarding an existing record preserves its identity. Local
VM catches can still handle a value without creating a return record. Undefined
is a valid payload. The preallocated emergency OOM record is
the identity exception; payload zero is reserved for bootstrap/no-isolate
allocation failures, and payload one remains the JIT interpreter-retry sentinel.
Neither sentinel is a heap reference.

The isolate's current exception is one propagation handle for legacy scalar,
boolean, and parser helpers. It does not own separate value/stack/flag state.
Use the returned record to inspect or consume a failure; `Ant_Exception_Current`
forwards scalar failure state, with an emergency OOM fallback. Root saved
handles across allocation and user-code execution, then restore the handle
with `Ant_Exception_Set`. GC traces both record fields, including after the
current handle has been cleared.

`sv_invoke_native` scopes an existing completion around a native call. A handled
inner failure leaves the caller's completion intact; an escaped failure takes
precedence. A normal native return with an unconsumed new failure becomes an
exception result. This boundary check does not unwind C cleanup or undo effects:
native code must still stop after fallible operations and settle owned resources.

`js_mkerr*` creates and publishes an exception record; it does not return an
ordinary Error value. `js_reject_promise` consumes exception results through
`Ant_Error_ConsumeMarker`, even when the Promise has already settled. Ordinary
rejection values never clear pending state. Consuming a record only clears
the identical current handle, so an older throw cannot erase a newer one.

Use `js_make_error_silent` for literal messages or `Ant_Error_CreateFormatted`
for printf-style messages when constructing ordinary error values, especially
before cleanup or user-code execution. Use `Ant_Error_CallCallback` for native
error-first callbacks that may receive a throw marker. It normalizes the first
argument without changing the caller's argument array. When converting a throw
before other cleanup, consume it explicitly with `js_take_thrown`. Do not clear
exceptions afterward: the callback may throw a new one, even the same value.
Once consumed, root the error across allocation, cleanup, or user property
access until another object owns it. Keep successful operations outside this
cleanup. See the [boundary fixes](../exec-plans/completed/async-error-boundaries.md).

Use `Ant_Exception_Pending` to distinguish a throw from its value in scalar
helper paths; interpreted and JIT catches unwrap their returned record without
depending on current state. Iterator cleanup preserves an
existing exception over a new cleanup failure; a cleanup failure during normal
completion must still propagate. The [module audit](../exec-plans/completed/module-error-boundaries.md)
records the per-file coverage and regression tests for these boundaries.

Disposal must also track completion presence separately from its payload.
Synchronous iterator-close opcodes explicitly distinguish normal cleanup from
cleanup while preserving an existing throw. A failed Promise continuation
setup must reach its owner's rejection and cleanup path before any result is
marked handled. `Ant_Promise_Observe` provides this for internal continuations
with an owner rejection callback, queued on setup failure. Public `.then()`
semantics remain unchanged. See the [exception model](../exec-plans/completed/exception-completion-model.md),
the [earlier handoff refactor](../exec-plans/completed/central-error-handoffs.md),
and [whole-codebase follow-up](../exec-plans/completed/whole-codebase-error-boundaries.md).

Stream size validation relies on `js_to_number` returning NaN when conversion
throws. Preserve that contract: the existing invalid-size branch consumes the
pending exception, keeping the additional check off successful conversions.

Node writable `done(error)` uses null and undefined to signal success. Preserve
synchronous `_write()` throws as thrown completions rather than passing their
values through `done`; otherwise a nullish throw becomes a successful write.

## JIT Fallback Must Preserve Effects

An inline fallback that calls the whole callee can run only before observable
effects. Getters, proxy traps, key coercion, stores, and calls must not execute
twice. Inline property helpers either complete without user-code effects or
bail before them; errors after an effect propagate without restarting the
callee. Resource limits such as nested-call argument capacity belong in
`jit_inlineable` before MIR emission, not in recovery after a partial body.

See [inline.c](../../src/jit/inline.c),
[glue.c](../../src/silver/glue.c), and
[inline error regressions](../../tests/test_jit_inline_call_errors.cjs).

Curried-call fusion must preserve outer-argument evaluation order. In
`X(a)(b)`, a mutable `b` is read after `X(a)` on the generic path; it cannot
be captured eagerly just because it currently resolves to a local. See
[call operations](../../src/silver/ops/calls.h) and
[fusion regressions](../../tests/test_curried_call_fusion.cjs).

## JIT Values And Entry State

Semantic facts must move, copy, and clear with their virtual-stack values.
Use `jit_value_info_t` helpers rather than updating `known_bool`, known
functions, constants, or integer ranges independently. `slot_type` describes
physical representation and has separate handling in
[values.c](../../src/jit/values.c).

Specialized numeric-local stores must preserve guards and boxed bailout
snapshots; see [emit_locals.c](../../src/jit/emit_locals.c) and
[guards.c](../../src/jit/guards.c). An incompatible OSR entry frame returns
`SV_JIT_RETRY_INTERP` without discarding valid compiled code. The
[OSR entry regression](../../tests/test_jit_osr_entry_reject.cjs) covers locals
that are still uninitialized at an earlier loop header.

The `n_pop` / `n_push` columns of `OP_DEF` in
[opcode.h](../../include/silver/opcode.h) are the authoritative operand-stack
effect of every op, including the count operand its format declares. They
feed `sv_func_t.max_stack` through `sv_op_stack_effect()` and
`sv_func_compute_max_stack()` in [compiler.c](../../src/silver/compiler.c),
which sizes the interpreter's frame reservation and the JIT virtual stack.
Change a handler's stack behaviour and the row together, then run
`tools/check_stack_depth.sh` (see [testing.md](testing.md)); it fails if the
analysis rejects any function in the test corpus. `vstack_push`
must never write past `vs.max`; it sets `overflow` and the compile is
abandoned. See [the vstack regression](../../tests/test_jit_vstack_depth.cjs).

A jump or return that leaves a `for...of` or `for await...of` must close that loop's
iterator and drop its three stack slots at the point the loop is crossed,
innermost first and before any outer finally runs; `emit_loop_exit_unwind`
retires unwind entries one at a time for this reason. Return values must survive
local-slot reuse in intervening finally blocks. Normal exhaustion only drops
the iterator record. See the [labeled-jump regression](../../tests/test_labeled_jump_for_of_close.cjs),
[return regression](../../tests/test_return_for_of_close.cjs), and
[async return regression](../../tests/test_return_for_await_close.cjs).

OSR is never refused on bytecode size alone. Each function's back-edge
threshold (`jit_osr_threshold`) is scaled by its size in
[runtime.c](../../src/jit/runtime.c); the only size ceiling is
`SV_JIT_MAX_CODE_BYTES` in `jit_is_eligible`, shared by the OSR and call
paths. Large OSR compiles use the cheap MIR context and set `jit_code_cold`. Such
code promotes itself: its prologue (`jit_setup_frame`) counts entries that
run the body and calls `jit_helper_tier_up` past `SV_JIT_THRESHOLD`, so
promotion happens on every entry path, including direct calls from other
compiled code that never touch the interpreter. The counter sits behind the
OSR entry guards so a rejected OSR entry does not count; the interpreter
retries those every OSR threshold, and counting them turned a function that
never enters compiled code into a wasted hot compile. Only code compiled with
`SV_JIT_TIER_COLD` sets
the flag, and `sv_jit_on_bailout_at` clears it. With `jit_code == NULL` it
means cold code handed a loop back for promotion
(`sv_jit_promote_pending()`, set by `jit_helper_promote_resume`): the next
OSR compile must be `SV_JIT_TIER_HOT`, and that compile is exempt from the
"no new feedback" refusal in `sv_jit_compile_tier`. See the
[OSR size-scaled tiering plan](../exec-plans/active/osr-size-scaled-tiering.md)
and [its regression](../../tests/test_jit_osr_large_function.cjs).

## Inline-Cache Ownership And Invalidation

Property ICs survive minor collections. Their cached shapes require retained
references, and inherited object hits must validate prototype identity before
using a cached holder; address equality alone permits allocator-reuse bugs.
Use the ownership and guard helpers in
[property.h](../../src/silver/ops/property.h). Registered shape references are
released by `sv_ic_shape_refs_cleanup` before code-arena reset; comparison IC
fields are a different payload family and cannot be swept as property shapes.

Keep the property, raw-object-lifetime, and prototype-write epochs distinct.
Minor GC advances the raw-object epoch; ordinary `.prototype` writes use the
dedicated prototype-write epoch, while actual prototype rewiring and shape
invalidation retain their broader invalidation rules. Epoch/identity wrap must
invalidate before a reused value can pass a guard. See
[gc.c](../../src/gc/gc.c), [internal.h](../../include/internal.h), and the
[minor-GC ABA](../../tests/test_ic_minor_aba.cjs) and
[prototype-write](../../tests/test_prototype_write_epoch.cjs) regressions.

## Closure And Upvalue Reachability

Lazy function-object materialization and writes into listener sidecars can
add young references to old owners. Preserve their write barriers rather than
assuming the owner will be traversed by a minor collection.

Reachable upvalue cells trace `*location` whether open or closed. Capturing,
sealing, or writing an old open cell over a young value must preserve the
remembered edge. The shared barriers in
[engine.h](../../include/silver/engine.h) own this policy; a JIT prefilter must
not reject open cells that need it. `gc_remember_upvalue` is suppressed during
object collection so finalizer-driven sealing cannot leave dangling entries.
See [objects.c](../../src/gc/objects.c) and the
[escaped-coroutine regressions](../../tests/test_arguments_escaped_coro.js).

## RegExp And Rope Allocation Lifetimes

Compiled RegExp data needs object ownership before named-group metadata
allocation can trigger GC. Cache membership alone cannot protect it across
multiple major collections. Preserve the attachment/error-path reference
handling in [regex.c](../../src/modules/regex.c), covered by the
[result regressions](../../tests/test_regexp_result_batch.cjs).

Rope mark-table allocation failure must still allow collection to progress.
A failed minor retries as a major; a major without metadata conservatively
roots initialized rope-pool contents while collecting ordinary objects and
retaining required rope blocks. Merely skipping collection or retaining blocks
without tracing their values is unsafe. See
[ropes.c](../../src/gc/ropes.c) and [gc.c](../../src/gc/gc.c).
