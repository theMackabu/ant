# JIT And GC Invariants

Status: active
Last reviewed: 2026-09-10
Owner: theMackabu

Read the section relevant to a JIT, cache, or heap-lifetime change. These rules
were checked against current source.
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
Initialize the slot to `undefined` before bootstrap allocations. Set/clear
operations keep it either `undefined` or an exception record, allowing the
pending-state check to compare directly with `undefined`.
`Ant_Exception_Set` must continue to normalize non-error inputs.
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

Each active `finally` handler owns its saved return, throw, or jump completion.
Entering the handler moves the frame's pending completion into that handler and
clears the frame slot. Nested catches and normal nested cleanup must not erase
the saved completion. GC traces handler values in both live VMs and suspended
activations; abrupt completion discards the handlers it exits.

Queued await resumes must match the coroutine's current `await_resume_job`,
as well as `await_registered`. Clearing an await on settlement, cancellation,
or failed activation capture clears that non-owning job pointer. An older job
must not settle or detach a replacement await. Queued jobs retain the coroutine
and remain allocated until dispatch, so their addresses provide distinct
registration identities without a wrapping counter. See the cancellation and
replacement cases in [test_error_handoffs.c](../../tests/test_error_handoffs.c).

Each await registration has one reference owner: the queued direct job or the
promise reaction's `CORO_HOLD_AWAIT`. Direct dispatch borrows its job's reference
through `Ant_Coroutine_ResumeAwaitJob` and releases it after resume returns.
It validates the job identity before directly clearing the registration.
Generic settlement transfers the detached await hold into a local reference,
or acquires one when no hold exists. Cancellation releases the detached hold
immediately. Inner resume helpers borrow the caller's reference; legacy native
wrappers acquire their own. A queued job pointer alone is not sufficient for
generic settlement to borrow a reference: nested draining can dispatch that job.
Direct jobs never install a promise reaction, so clearing their registration
does not scan promise handlers.

Resume values and their error flags are arguments to the synchronous coroutine
resume helpers. The caller keeps the value rooted until resume returns: direct
jobs pin it, promise reaction batches retain the source promise, and legacy
native wrappers pin their argument. The VM's temporary resume-value field is
not a separate GC root. Do not add redundant roots to the direct-job path or
retain the last resume value in the coroutine after it suspends again.

`js_mkerr*` creates and publishes an exception record; it does not return an
ordinary Error value. `js_reject_promise` consumes exception results through
`Ant_Error_ConsumeMarker`, even when the Promise has already settled. Ordinary
rejection values never clear pending state. Consuming a record only clears
the identical current handle, so an older throw cannot erase a newer one.

Use `Ant_Error_Create` for literal messages or `Ant_Error_CreateFormatted`
for printf-style messages when constructing ordinary error values. Formatted
construction and `js_mkerr*` share a formatter that grows beyond its short-message
buffer instead of truncating. Use ordinary Error construction especially
before cleanup or user-code execution. Use `Ant_Error_CallCallback` for native
error-first callbacks that may receive a throw marker. It normalizes the first
argument without changing the caller's argument array. When converting a throw
before other cleanup, consume it explicitly with `js_take_thrown`. Do not clear
exceptions afterward: the callback may throw a new one, even the same value.
Once consumed, root the error across allocation, cleanup, or user property
access until another object owns it. Keep successful operations outside this
cleanup.

Use `Ant_Exception_Pending` to distinguish a throw from its value in scalar
helper paths; interpreted and JIT catches unwrap their returned record without
depending on current state. Iterator cleanup preserves an
existing exception over a new cleanup failure; a cleanup failure during normal
completion must still propagate.

Disposal must also track completion presence separately from its payload.
Synchronous iterator-close opcodes explicitly distinguish normal cleanup from
cleanup while preserving an existing throw. A failed Promise continuation
setup must reach its owner's rejection and cleanup path before any result is
marked handled. `Ant_Promise_Observe` provides this for internal continuations
with an owner rejection callback, queued on setup failure. Public `.then()`
semantics remain unchanged.

Stream size validation relies on `js_to_number` returning NaN when conversion
throws. Preserve that contract: the existing invalid-size branch consumes the
pending exception, keeping the additional check off successful conversions.

`js_to_number` of a BigInt throws TypeError and returns NaN like any other
failed conversion, so index and count arguments reject BigInts without each
builtin checking. `Number(value)` is the one conversion that accepts a BigInt
(`number_from_value`); mixed comparisons do not go through `js_to_number`.

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
`sv_func_compute_max_stack()` in [finalize.c](../../src/silver/compiler/finalize.c),
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

Scope-exit work on an abrupt exit is ordered by nesting, not emitted up front.
`unwind_push` records the local count and the `using` cleanup count
(`sv_unwind_mark_t`); `emit_loop_exit_unwind` closes captured locals and
disposes `using` stacks declared inside an unwind entry before it retires that
entry, and the rest after the last one. A `break`, `continue` or `return` that
runs a `finally` therefore leaves outer captured bindings open while the
`finally` runs (it may still write them) and disposes an outer `using` after it.
A `return` that has to run inline finally blocks or iterator closes stashes its
value in a local that is older than every entry it retires: the outermost
`for...of` completion local or the error local of the outermost `using` block
(`sv_using_cleanup_t.stash_local`).

A handler entry closes what the protected region captured. `end_scope` lowers
`handler_close_floor` to the lowest captured frame slot it pops; `compile_try`
emits one `OP_CLOSE_UPVAL` for that slot at the catch entry and on the abrupt
path into a `finally` (throw, return and jump all enter there; normal completion
jumps past it), then merges the floor into the enclosing `try`. Catch and
finally locals reuse the slots of the try block, so without the close a closure
would read the caught value or a TDZ binding would read as initialized. A
region without captured block locals emits nothing. Frames dropped by the
unwinder are closed whole in `sv_vm_throw`. See the
[abrupt-exit regression](../../tests/test_closure_abrupt_exit.cjs).

An exit from inlined code (`opt_rt_exit_frames`) pushes a waiting interpreter
frame for every outer frame of the chain, the root included even when the root
entered through OSR and borrowed its interpreter frame: the stack walk skips
the borrowed frame as the OSR host, so without a waiting frame the outermost
compiled function disappears from stacks captured while an inner frame runs.
See the [exit stack parity](../../tests/test_opt_exit_stack_parity.cjs)
regression.

The `new` prefix of a stack frame comes from the frame's own `new.target`
when it has an interpreter frame, and is always set for class constructors.
A compiled activation without an interpreter frame takes it from how the
activation was entered. `sv_jit_invoke_same_realm` is the only way from C into
compiled code: it counts the entry in `js->jit_active_depth`, and an entry with
a `new.target` also links an on-stack `sv_jit_construct_mark_t` holding its
depth into `js->jit_construct_marks` for the duration of the call (a plain
entry pays only the `new.target` test). Any other entry path would shift the
depths against the runs the stack walk counts. The walk
(`jit_walk_close_run`) gives run k from the newest the depth
`jit_entries - k + 1`, drops marks deeper than that, and labels the oldest
activation of the run a construct exactly when a mark has that depth, at any
nesting depth. An activation called by compiled code is labelled from the call
op of the adjacent older compiled frame (`sv_activation_compiled_caller`);
the iterator keeps that frame's resolution for its next step, and a lazy error
record stores the caller's pc so the lookup happens only when the stack is
rendered. When the walk stops at the frame limit it
still looks one native frame further for that.

A lazy record's pcs are only meaningful while the code they point into is
registered. Code ranges are unregistered only late in a major collection
(`opt_weak_codes` and `sv_jit_forget_dying`, both after marking and weak
processing), and the address range is reused afterwards. `gc_mark_error_record`
therefore resolves a record the first time a major collection marks it
(`error_record_settle`): the resolved frames (function, bytecode offset,
construct bit) overwrite the raw ones in place, the record is reallocated only
when inlining expands the frame count, and the pcs are zeroed so they are never
read again. Records that are not marked are garbage and are never resolved;
there is no list of lazy records and the record header carries no settle state
(64 bytes, asserted). This relies on three things: every live record object is
scanned in every major collection (record objects are never plain, permanent
objects are rescanned), non-plain objects are scanned on the main thread even
under parallel marking, and nothing unregisters a code range outside the tail
of a major collection. A new path that unregisters a range anywhere else must
resolve the live records first. Rendering resolves again, which is a
pass-through for settled frames, so a stack renders the same whenever it is
first read. See the [new prefix](../../tests/test_stack_new_prefix.cjs)
regression.

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
[regression test](../../tests/test_jit_osr_large_function.cjs).

`sv_jit_try_osr` refuses entry (the loop stays interpreted) in two cases
where compiled code would see a different world than the frame:

- a derived-constructor frame whose `this` is still `undefined` (constructed
  through a proxy, a bound target or a foreign `newTarget`); compiled super
  calls assume the receiver object exists;
- a frame whose eval environment is not the one compiled code resolves
  through (`sv_closure_eval_env` of the callee, or the global object): direct
  eval code inside a function, and a function whose environment was created by
  an earlier `eval('var ...')`.

`sv_frame_resume_at` must publish `vm->jit.resume.finally` before it rebuilds
the handler stack: `sv_frame_push_try_chain` restores each enclosing
`finally` body's pending completion from it, and a body re-pushed without it
forgets a pending `return`, `break` or `continue`.
`tests/test_jit_resume_state.cjs` covers all of these.

### Catching In The Optimizing Tier

A `throw` whose handler is in the same compiled frame (`IR_OP_THROW` with a
handler) passes the thrown value to the catch site as is: no exception record
is created and no stack is captured, the same as the template tier's jump to
the catch label. `opt_cg_catch` therefore accepts either an error marker (from
a call) or a raw value; only the leave-while-terminating path turns a raw
value into a thrown completion.

A catch that cannot be completed in compiled code (a `finally` handler, a
handler without a catch entry, or exit values that need materializing) leaves
compiled code. How it leaves depends on how the code was entered:

- Entered by a call, it resumes the function in a nested interpreter frame
  underneath the compiled frame (`opt_rt_resume`).
- Entered through OSR or a catch entry, the compiled frame borrows the
  interpreter frame it came from (`opt_rt_borrow_frame`; the OSR entry borrows
  whenever the graph has handlers, except in derived constructors, whose
  `this` lives in the compiled frame). `opt_rt_resume_borrowed` then writes
  parameters, locals, operand stack, reopened boxes, handlers and the pending
  finally completion back into that frame (`sv_frame_resume_at`) and returns
  `SV_JIT_AWAIT_PARKED`, so the interpreter loop that started the OSR
  continues at `frame->ip`. Nothing nests, so one activation is at most one
  resume deep however often it throws.

There is no nesting counter. Code that keeps leaving is found by rate instead:
`opt_catch_left` reads the thread CPU clock every `OPT_CATCH_LEAVE_WINDOW`
leaves of one code object, and when a whole window took less than
`OPT_CATCH_LEAVE_WINDOW_NS` (a leave about every 2 microseconds, which only a
loop that throws on nearly every iteration reaches) the code is dropped and,
after `OPT_CATCH_LEAVE_DROPS` such drops, the optimizing tier is disabled for
that function so the template tier, which handles these throws in the frame,
takes over. A function that throws through `finally` now and then keeps its
optimized code.
See the
[throw through finally](../../tests/test_opt_throw_through_finally.cjs)
regression.

Type feedback is not allocated under `--jitless` (`sv_tfb_ensure`): nothing
reads it there, and every recorder already skips a function without it.

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

`super.x` reads reuse the get IC with the home object's prototype as the
receiver and call getters with the real `this`; proxies skip it. `super.x = v`
stores ([super.c](../../src/silver/super.c)) have their own kinds in
`super_put_kind`, all guarded by the IC epoch and the home prototype's shape in
`guard.super_put.base_shape` (a retained reference in the add-from slot):

- `DATA`: the key is absent on the chain (absence-guarded) or a writable data
  property of the home prototype; `cached_shape` is the receiver's shape, with
  the put poly cache holding further receiver shapes.
- `DATA_ANY_SHAPE`: same chain guard, but the receiver's own slot is looked up
  per store, for megamorphic receivers.
- `SETTER`: `cached_shape` is the shape of the object holding the setter,
  `super_put_holder_depth` hops above the home prototype; the receiver is not
  guarded because a setter does not consult it.
- `DISABLED`: the site failed to bind `SUPER_PUT_FILL_LIMIT` times in this epoch.
  A stale site takes the current epoch when it is reset, so failed binds count
  toward the limit even when nothing is bound.

A store that adds the key records the receiver's shape transition in the put
poly cache, here and for `this.x = v` and field initializers
([property.h](../../src/silver/ops/property.h)). The slow store can run user
code (a setter that redefines itself and changes other own properties), so a
transition is recorded only when `ant_shape_appends_slot` holds: the new shape
is the old one's direct child in the shape tree, or every earlier property has
the same key, attributes and accessors. Megamorphic and `DATA_ANY_SHAPE` sites
skip the shape snapshot because they record nothing. See the
[super put add](../../tests/test_super_put_add.cjs) regression.

An IC keeps a reference to every shape it compares, so an attribute change
must clone the shape (`js_obj_ensure_unique_shape`) and the pointer compare
fails; never mutate a shared shape in place.

Exotic receivers reach the get IC only through `ANT_EXOTIC_KEYS_NUMERIC`
ops, whose hooks must decline every key `ant_key_may_be_numeric` rejects. The
IC reads that answer from `sv_atom_t.maybe_numeric`, set by the compiler, so
JIT helpers must receive the function's atom, never a rebuilt one. The
C admission check runs per hit; JIT snapshot, poly and mega paths reject
exotic receivers, and the generic and own-slot paths guard them whenever the
atom passes that prefilter. Static ops tables are shared and immutable; public hook
setters copy them to a heap table first.

## Watched Prototypes

Compiled code may take a value found in a prototype as a constant while the
prototype is watched ([proto_watch.h](../../include/proto_watch.h)). A watched
object has `ANT_PROTECTOR_HOLDER_WATCHED` and a shape no other object has; any
change to its slots, layout, prototype or exotic hooks must reach
`ant_proto_watch_note_change` before it happens. `ant_object_prop_set_unchecked`,
`ant_property_mutation_invalidate`, `ant_object_invalidate_guarded_absence`,
`js_set_proto` and the exotic-hook setters already do; a new writer that skips
all of them must call it. Never give another object a watched object's shape
(see `copy_adopt_shape`). Watch a holder before snapshotting its shape, or the
clone the watch makes stales the snapshot. Breaking a watch invalidates opt
code: running frames exit at their next `AssumeValid`, and template code checks
the cell at each use. See the
[watched prototype](../../tests/test_opt_watched_prototype.cjs) regression.

Keys whose mutation can break a protector are named once, in
`ant_property_key_breaks_protector` ([protectors.h](../../include/protectors.h)).
`ant_property_mutation_invalidate` gates on it and the optimizing tier refuses
inline stores and adds for the same keys (`opt_field_slot_ok`), so a key added
to `protectors_apply_key` must be added there. See the
[protector key stores](../../tests/test_protector_key_stores.cjs) regression.

## Shapes Carry The Prototype

Objects have no prototype or in-object limit field: `ant_object_proto(obj)` and
`ant_object_inobj_limit(obj)` read the shape, so a checked shape is proof of both.
Free object cells through `ant_object_cell_free`, which finds the arena from the
cell's chunk, because the shape is gone by then. Change a prototype only through
`ant_object_set_proto` (or the `js_set_proto*` helpers that call it), which moves the
object to a shape for the new prototype; allocate with a known prototype through
`js_mkobj_with_proto`. Compiled allocations must use a shape whose prototype is the
object's prototype (`opt_build_set_proto` switches literal allocations to the
`ant_shape_with_proto` shape). Set a fresh object's prototype before adding
properties, or every such object gets a prototype transition. See the
[shape proto identity](../../tests/test_shape_proto_identity.cjs) and
[proto literal shape](../../tests/test_opt_proto_literal_shape.cjs) regressions.

Overflow storage has no capacity field either: its size is
`ant_object_overflow_cap_for(prop_count - limit)`, so anything that lowers
`prop_count` must shrink it (`obj_overflow_fit`), and free paths must read the
capacity before releasing the shape. Compiled add paths rely on the same rule to
decide at compile time which slots grow the array. Shape swaps must keep the
in-object limit. See the
[overflow capacity](../../tests/test_object_overflow_capacity.cjs) regression.

## Exotic Objects Have Exotic Shapes

Every object with `is_exotic` set has a shape whose exotic bit is set, so a shape
check against an ordinary shape never matches an exotic object and compiled code
skips the per-access flags load. The shape also holds the object's exotic ops
(`ant_object_exotic_ops`). Mark objects exotic only through `js_obj_set_exotic` or
`js_obj_mark_exotic`, and copy the bit and ops in any new shape-copy path. The
[exotic shapes](../../tests/test_opt_exotic_shapes.cjs) regression covers the
common cases but cannot catch a missed site, so review new ones.

## Shared Array Elements

Array literal sites (`src/silver/compiler/literal.c`) cover literals with more
than 32 elements (at most one dynamic element per six constant ones) and
smaller fully constant literals that contain a non-empty nested array. A site
with no nested literals keeps its elements in a refcounted
`gc_shared_elements_t` block (`src/gc/objects.c`) created at compile time; the
function holds one reference and traces the block while it lives. Sites with
nested literals build a boilerplate on their second execution and deep-clone
it (`src/silver/literal_sites.c`); its leaf arrays share their own blocks.

`Ant_Array_CreateFromBlock` (`src/arrays.c`) is the only place that decides
between sharing and copying. Constant sites (no dynamic elements, no nested
literals) share the block. Sites with dynamic elements always copy it, then
store the dynamic values with `OP_ARRAY_INIT`. Once any array is unshared, its
block sets `copy_on_create` and every later array from that site is a copy;
if that unshare grew the array, `outgrown` gives later copies 1.5x capacity.

A shared array has `frozen`, `sealed` and `extensible` all set; real frozen or
sealed objects always clear `extensible`, so `ant_array_is_shared` is
unambiguous. Every
element-write guard already rejects the state: in-place stores check FROZEN,
removals check SEALED, and appends grow storage because a shared array has
`cap == len`. Raw `flags.frozen`/`flags.sealed` reads therefore mean "frozen or
shared", and write guards (JIT flag masks, `array_refuses_add`) rely on that.

- Never write `u.array.data` of an array you did not just allocate without
  unsharing it first: `Ant_Array_Unshare(js, ptr)`, `Ant_Array_UnshareValue(js,
  value)`, or `Ant_Array_UnshareForAppend(js, value, added)` before growing.
  `dense_set` and `dense_grow` unshare for you; `dense_set_owned` and raw
  pointer writes do not. Changing `len` alone is safe: the block is never
  written, and unsharing copies only the first `len` elements. Shrinking a
  shared array (`length = n`, `pop`) lowers `len` and `cap` together and keeps
  it shared, so `cap == len` still forces appends to grow; anything that raises
  `len` again must unshare first. An unshare of an array shrunk to half its
  block or less (blocks of 32 or more elements) does not set `copy_on_create`, because copying at creation would
  cost more than the small copy at the write.
- Slow paths that turn frozen/sealed/extensible into JS-visible behavior unshare
  first (property set and define, delete, the mutating Array builtins,
  freeze/seal/preventExtensions). Pure queries use `ant_object_is_frozen`,
  `ant_object_is_sealed`, `ant_object_is_sealed_or_frozen` or the `extensible` bit,
  which ignore the shared state.
- A shared array's `finalizer` drops its block reference, so the plain sweep
  path never sees one and ordinary arrays pay nothing. Unsharing clears it, and
  `js_set_finalizer` asserts it is never called on a shared array. Anything else
  that frees or reallocates array storage must check `ant_array_is_shared` first.
- Code that sets `frozen` or `sealed` must also clear `extensible`.

## Rest And Spread

The compiler emits one op for a call whose only spread is its last argument:
`OP_CALL_SPREAD`, `OP_NEW_SPREAD` and `OP_SUPER_SPREAD` take the fixed
arguments followed by the iterable. Every other layout, and array literals,
build an array on the operand stack (`OP_ARRAY 0`, then `OP_APPEND` and
`OP_SPREAD`) and calls consume it with `OP_APPLY`, `OP_NEW_APPLY` or
`OP_SUPER_APPLY`. The array under construction is only reachable from the
operand stack, so no user code can observe it before the last op.

- The interpreter records what each spread op was given in the op's
  type-feedback byte (`sv_tfb_record_spread`): `0` unseen, `n + 1` for a packed
  array of length `n` (at most `SV_TFB_SPREAD_MAX_LENGTH`) that iterates through
  the intact array iteration protector, `SV_TFB_SPREAD_OTHER` for anything else
  or a second length. Compiled code does not record; a function whose compile
  failed stops recording.
- The optimizing tier lowers a site with one seen length to a call with the
  elements as arguments (`opt_spread_elements`): `IterableArray` (array tag,
  `Array.prototype` as prototype, protector intact), `CheckElements`, the
  length compared for equality, and `CheckNotHole` per element. All of them
  exit before anything observable has run, so the interpreter repeats the
  whole op. The elements then go through the ordinary call builder
  (`opt_build_call_with`, `opt_build_super_call_with`), which is why inlining,
  `Math.max`, `push` and the super paths apply unchanged. The argument nodes
  are inputs of the call only: the operand stack still holds the iterable in
  every frame state of the site.
- A site without that speculation calls a runtime helper that expands the
  iterable in place. The helper needs room after its inputs, which the call
  node reserves as `spare_slots` up to `OPT_SPREAD_ARG_SLOTS`; a helper given
  only its own inputs as capacity falls back to malloc-backed temp roots and is
  slower than the template tier.
- An array literal with spreads is compiled as one allocation at its `OP_ARRAY`
  (`opt_build_array`) when everything up to the last `APPEND`/`SPREAD` is a
  pure load (`opt_is_pure_load`). Its guards exit to the `OP_ARRAY` offset and
  the interpreter rebuilds the literal from scratch, which is only correct
  because nothing between the two points has an effect; the folded
  instructions are skipped through `skip_to`. Exit sites and speculation
  queries for the fused ops are therefore keyed by the `OP_ARRAY` offset.
- A frame state has two offsets: `bc_off`, where the interpreter resumes, and
  `site_off`, where an exit through it is recorded and where the next compile
  looks it up (`opt_may_speculate`, the learned-shape and call-target slot in
  `opt_cg_exit_site`, `opt_rt_exit`, `opt_rt_exit_frames`, the bounds-check
  pass). They are equal except in a fused build. Any new builder path that
  emits guards with a frame state taken at one op while consulting
  speculation at another must give that state the consulting op's `site_off`,
  or every recompile repeats the failing speculation until the function is
  disabled.
- When the op after a fused literal is the `OP_APPLY`, `OP_NEW_APPLY` or
  `OP_SUPER_APPLY` that consumes it, in the same block and with at most
  `OPT_MAX_CALL_ARGS` elements, no array is built: the element nodes go to the
  call builder (`opt_literal_feeds_call`, `opt_build_literal_call`) and
  `skip_to` passes the apply op too. The call is built with the builder
  positioned at the apply op, so its call site, its resume-after state and its
  call-target feedback belong to that op. Guards emitted while building the
  call resume at `OP_ARRAY` but blame the apply op: `opt_build_literal_call`
  swaps the eager frame state for a copy with `site_off` set to the apply
  offset (`opt_frame_state_blaming`) and restores it afterwards, so the spread
  guards built earlier keep blaming `OP_ARRAY`. The interpreter records the
  callee of `OP_APPLY` and `OP_NEW_APPLY` in the call-target table for this,
  and native callees in the op's feedback byte (`sv_tfb_record_spread_target`;
  the spread ops keep that byte for the spread class). `OP_SUPER_APPLY` is no longer root-only: an inlined derived
  constructor whose `super(...)` does not fuse fails the inline attempt and is
  compiled as a call.
- An empty array (`OP_ARRAY 0`, an inlined rest parameter with no extra
  arguments) is allocated inline like other small literals, with the
  `MAX_DENSE_INITIAL_CAP` capacity the runtime gives an empty array.
- A rest array is built by `Ant_Array_CreateRest` in all three tiers: the
  storage class that fits the arguments, or the default empty array.
- `OP_REST` reads the frame's argument area, which liveness does not see. The
  frame states of an inlined function that contains it carry every passed
  argument and every parameter slot (`keeps_arguments`), and
  `opt_rt_exit_frames` resumes such a frame with the full argument count. A
  root frame builds the array from the entry's arguments (`opt_rt_rest`,
  `passes_entry`).
- A rest parameter whose only uses are `GET_LOCAL` immediately followed by a
  spread-call op in the same block is never allocated
  (`opt_decide_virtual_rest`). Its local holds the constant
  `IR_VALUE_VIRTUAL_REST | start`, which must never be the input of a node: a
  graph that uses it is rebuilt without the virtual form, and exits
  materialize it from the frame's arguments (`opt_cg_materialize_arguments`).
  Forwarding skips the iteration, so it holds only while the array iteration
  protector is intact: an inlined frame guards `ArrayIterationIntact`, the root
  helpers (`opt_rt_call_forward`, `opt_rt_new_forward`) check it in C and build
  the array when it is broken, and a root `super(...args)` guards it before
  taking the `SUPER_FORWARD` path. Root functions take the virtual form only
  when they have no loop and no `try`, because a function using it gets no OSR
  or catch entries.
- `OP_NEW_SPREAD` has no template JIT emit (neither had `OP_NEW_APPLY`): with
  `opt:off` a function containing it stays interpreted.

See the [rest and spread](../../tests/test_opt_rest_spread.cjs) regressions and
the template-tier cases in
[call spread](../../tests/test_jit_call_spread.cjs) and
[spread append](../../tests/test_jit_spread_append.cjs).

## Class Field Definition

A public instance field with an identifier, string or numeric literal key
compiles to `OP_INIT_FIELD` (atom and IC index, receiver and value on the
stack; a numeric key uses its canonical string as the atom); computed
keys and static fields keep `OP_DEFINE_METHOD_COMP` with
`SV_DEFINE_METHOD_FIELD`. Both are CreateDataPropertyOrThrow for string and
symbol keys alike: they never look at the prototype chain, they throw on a
receiver that cannot take the property, and they replace an existing own
property.

- `sv_init_field` takes the fast paths only for a plain receiver: an ordinary
  object that is extensible, not frozen, not sealed and not exotic. Everything
  else (a parent that returned an array, a function, a proxy or a closed
  object) goes to `js_create_data_property_or_throw`.
- The site's IC uses the `PUT_FIELD` layout with define rules. `cached_shape`
  is set only when the receiver already owns the key as a data property with
  default attributes, where storing the value is the whole definition.
  `guard.add` and the put poly block hold transitions that add the key with
  default attributes; they are recorded without the prototype chain admission
  a `PUT_FIELD` add needs, so an entry made by one op is not valid for the
  other. Each IC slot belongs to one op.
- The optimizing tier compiles the add entries as `CanAddNamed`/`AddNamed`
  under the receiver's shape with `epoch_checked` set and no epoch cell: the IC
  epoch guards the chain, which a definition ignores. That keeps the check
  foldable on a fresh receiver, so an inlined constructor with fields sinks
  like one that assigns `this.x`. Shapes that already own the key are compiled
  as a shape-checked store when the property has default attributes and go to
  the runtime call otherwise. The slow path (`opt_rt_init_field`,
  `opt_rt_init_field_adding`) defines; it must never be the set helper.
- The value is stored as given: a field never passes through
  `js_expose_cfunc_for_key`. An anonymous function, arrow or class expression
  used as an initializer is named at compile time from an identifier, string,
  numeric or private key (`compile_field_initializer`), and by
  `SV_DEFINE_METHOD_SET_NAME` on the define op for computed keys.
- A descriptor handed to a proxy `defineProperty` trap is always a fresh
  ordinary object (it inherits `Object.prototype`) in FromPropertyDescriptor
  order (`define_desc_object`), with `writable`, `enumerable` and
  `configurable` coerced to booleans and absent fields left absent; the fields
  of the caller's descriptor are still read in ToPropertyDescriptor order. Its
  fields are defined as own data properties (`mkprop_define_data`), never
  assigned, so setters on `Object.prototype` do not run. The freeze and seal
  descriptors in `proxy_set_integrity_level` follow the same rules, and freeze
  sends `writable` only for data properties.
- `proxy_define_property` checks the trap result against the descriptor it
  read before calling the trap (the trap may mutate the object it was given):
  compatibility with a non-configurable target property, no new
  non-configurable property, and no non-writable define over a writable
  non-configurable one. `proxy_reflect_define_property` is the same operation
  returning false instead of throwing when a trapless proxy's target refuses.
  A `set` that reaches a proxy with a `defineProperty` trap but no `set` trap
  goes through `js_set_with_receiver` on the target, so the trap only runs
  when OrdinarySet would define.
- `ctor_slot_class` counts the public instance fields with the `this.x`
  assignments, so the receiver has in-object room for them. A constructor the
  compiler synthesizes for a class with fields gets its slot class from the
  fields alone. The count itself is kept in `debug->ctor_props` and describes
  the function's own properties only; the function is never mutated after
  compilation. When a class with a heritage is defined, `sv_ctor_inherit_props`
  stores the sum of its own count and the parent closure's count in the
  constructor closure (`sv_closure_t.ctor_props`, a byte of existing padding;
  zero means "use the function's own class"). Every allocation of a construct
  receiver sizes it with `sv_closure_construct_slots(closure)`, never from the
  function alone, so own, default (function-less) and factory constructors
  share one mechanism and two closures of one class expression can have
  different sizes. A bound parent contributes nothing.
- A static field takes the same shape-define path as an instance field when
  the constructor's function object is plain (`sv_field_target_plain`), except
  for the keys a function object treats specially (`name`, `length`,
  `prototype`, `caller`, `arguments`) and non-string keys, which go through
  `js_create_data_property_or_throw`.

See the [class field](../../tests/test_opt_class_fields.cjs) regressions.

## Derived Constructor `this`

The object is pre-allocated and passed to the parent constructor, so the
`this` TDZ is enforced by the compiler (`src/silver/compiler/vars.c`). A derived
constructor needs no bookkeeping when a forward walk of its blocks and `if`
statements proves `super()` runs exactly once on every path, before anything
reads `this` or `super.x` or returns, and neither the body nor a parameter
default uses eval, `this` or `super`. Those constructors compile to plain
`OP_THIS`. The parser marks functions with exactly one `super()` call
(`FN_SOLE_SUPER_CALL`) so the common `super(...)`-first constructor is
recognised without walking its body.

In the optimizing tier `this` lives in the entry record; `super()` stores its
result there (`StoreSlot` of `OPT_THIS_SLOT`) so catch and OSR entries, which
reload it, see a replaced receiver.

Any other derived constructor gets a hidden local named `this`, initially
`undefined`:

- Every `super()` result is stored into it, after throwing if it is already
  set. The throw is omitted when the body's only `super()` is a top-level
  statement, which cannot run twice.
- Reads before a definite `super()` go through `emit_this`, which throws a
  ReferenceError while it is `undefined`. Emit `this` through `emit_this`, not a
  raw `OP_THIS`, unless the value is the receiver passed to `super()`.
- Returns that may run before `super()` read the binding when the value is
  `undefined`. A return inside `try...finally` stores its value and breaks to a
  landing point after the body, so the check runs after the `finally`.

Direct eval reaches these through captured eval scopes by name, which is why the
hidden locals are named `this`, `super` and `%fields`; none can be written as
an identifier. `sv_eval_scope_t.this_binding` is set per call site, so a regular
function nested in a constructor keeps its own `this`.

### Super Constructor Lookup

A derived constructor's `closure->super_val` is the constructor itself, the
spec's active function, not its parent. Nothing about the parent is cached:

- `super()` reads the parent from that function's prototype at call time
  (`sv_super_construct`, and `sv_call_default_ctor` for default constructors).
  `Object.setPrototypeOf(Class, P)` therefore takes effect immediately, also
  inside a running constructor and in arrows created earlier.
- `super.x` in a constructor uses the prototype of `Class.prototype`
  (`Super_Base`); methods and static members use the prototype of their home
  object as before.
- Bound copies keep the class as their super value, so they follow the class
  and ignore their own prototype. `class extends null` needs no special case:
  its prototype is `Function.prototype`, which is not a constructor.
- A `super()` call is only ever `OP_CALL_SUPER`, `OP_SUPER_APPLY` or
  `OP_SUPER_FORWARD`. Do not detect one by comparing a callee with the frame's
  super value: plain calls of the parent or of the class inside a constructor
  are ordinary calls.
- A base-class constructor gets `Class.prototype` as its super value, and only
  when it references `super` (`func->uses_super`), so other base constructors
  keep the plain-closure fast paths.
- The super value therefore has two meanings: the home object for methods, the
  class itself for derived constructors. `Super_Home` (`src/silver/super.c`)
  maps a function home in non-static code to `Class.prototype`, through the
  shape-guarded prototype cache. That is why `is_static` is inherited by arrows
  and carried on eval scopes.

### Super In The Optimizing Tier

The builder ([build.c](../../src/jit/opt/build/build.c)) compiles `super.x`,
`super.x = v` and `super()` against the live home object instead of calling the
runtime helpers, and every fast path starts by pinning the super base:

- The guard compares the home's prototype value (`LoadPrototype` then
  `CheckValue`), never the home's shape: watching a prototype clones its shape,
  and class prototypes are watched as soon as another function inlines one of
  their methods. Instance homes must be plain objects (`CheckObject`), a
  constructor home is compared by identity and mapped to `Class.prototype`,
  which is read once because that property is neither writable nor
  configurable.
- Reads walk the chain from the base at compile time and reuse the holder
  machinery of ordinary named reads (watched links, or shape checks when a
  link cannot be watched). Getters are called with the frame's `this`.
  `super[k]` takes the same path when `k` is a constant string in the graph.
  The walk is bounded by `OPT_MAX_PROTOTYPE_DEPTH`, shared with ordinary
  inherited reads.
- Stores are compiled only from the site's IC state. `DATA` stores into the
  receiver's own slot under its shape: a chain where the key is absent may
  rely on watches; a chain object that holds the key as a writable data
  property needs a shape check, because an attribute change replaces the shape
  but does not break a watch. A store that adds the property is recorded by
  the interpreter as an add entry in the site's put poly block (receiver shape
  before, shape after, slot), never in `guard.add`, which a super site uses
  for the base shape; those entries are only valid under the base shape guard
  and the IC epoch. The tier compiles them as `CanAddNamed`/`AddNamed` under
  the receiver's shape with no epoch cell (the base chain is already pinned by
  the guards above) and exits with `SlowPath` when the receiver cannot take
  the property, so an inlined constructor needs no slow path.
- The home, base and receiver of a super store may be functions (a static
  method stores through the class to its parent class): the interpreter cache
  reaches their object through `closure->func_obj` (`Super_ObjectOf`) and treats
  an unmaterialized function object as uncacheable. `SETTER` calls the setter found on the chain
  directly with the frame's `this`, under the same holder guards as a getter;
  it is a call, never inlined, because the expression's value is the assigned
  value and an exit inside an inlined setter would resume with the setter's
  return value instead.
- `super()` reads its parent through `opt_super_parents`, which starts at the
  class function and skips default derived constructors (they only forward),
  recording every link. A parent that is a bound bytecode function resolves to
  its target (`opt_super_bound_target`): the guards still pin the bound
  function as the link, the callee constant is the target, the bound arguments
  are prepended as constants and `new.target` is passed through unchanged. It
  is refused when bound and call arguments together exceed
  `OPT_MAX_CALL_ARGS`. `opt_super_parents_guard` checks each link's prototype
  value. While the constructor's function has a single closure the frame's
  super value is first compared by identity (an `UnexpectedValue` exit, so a
  miss blocks neither the shape speculation nor inlining); once it is `multi_closure`, or
  after that exit, the first link is read from the super value itself
  (`ObjectAddress` then `LoadPrototype`), so classes made by one factory share
  the code while they share a parent. A chain that ends in a base default
  constructor is a no-op after the guards.
- When the receiver operand is the frame's `this`, the parent is inlined.
  Inlined construct frames at a super call carry the caller's `new.target` in
  `OPT_NEW_TARGET_SLOT`, and a frame resumed after a super call takes the
  call's result as its `this` (`opt_rt_exit_frames`), since the
  interpreter-run parent may have returned another object. An inlined derived
  constructor accepts `return` of its receiver or `undefined` only.
- A parent that cannot be inlined is called directly in a root derived
  constructor (`IR_OP_CALL_DIRECT` with `construct_super`,
  `opt_cg_call_super`): the parent's compiled entry gets the same `this` and
  the caller's `new.target`, `opt_rt_super_parent` runs it while it has no
  compiled code, and the construct-result rule is applied inline. The result
  is stored into `OPT_THIS_SLOT` and, when the frame is already entered, into
  `frame->this`, which is what an exit resumes with. Inlined constructors never
  use it: their receiver is fixed for the frame.
- `new C()` for a default derived constructor (`opt_try_default_construct`)
  checks the callee, allocates from the class's fixed `prototype`, guards the
  same parent chain and inlines the first real constructor with `new.target`
  set to `C`. If that constructor is not inlineable the site stays generic.
- `super(a, ...rest)` with one trailing spread is `OP_SUPER_SPREAD` and takes
  the paths above once its elements are known (see
  [Rest And Spread](#rest-and-spread)): the arguments of an inlined frame, the
  frame's own arguments through the `SUPER_FORWARD` path, or a packed array of
  the length the site has seen. Without that it is a helper call
  (`jit_helper_super_spread`, `opt_rt_super_spread`) and the constructor is not
  inlined. A super call with any other spread layout is `OP_SUPER_APPLY`, a
  helper call in both tiers (`jit_helper_super_apply`, `opt_rt_super_apply`)
  that keeps its constructor out of inlining.

Anything else keeps the runtime call: absent keys on reads, computed keys that
are not constant strings, megamorphic or primitive receivers, stores of the
protector keys `next`, `return` and `eval`, and parents that are builtins,
bound builtins or bound proxies. Constructors with class fields are compiled
and inlined like constructors that assign `this.x` (see
[Class Field Definition](#class-field-definition)).

Call sites re-speculate on a new target after a `BadCallTarget` exit while the
interpreter's call-target feedback for the site has moved on since the exited
code was compiled (`opt_call_targets_seen`); the feedback disables itself after
`SV_CALL_FB_MISS_DISABLE` retargets, which ends the speculation. A callee whose
value is known at compile time overrides stale feedback.

See the hot-loop cases in the
[property](../../tests/test_super_property_ic.cjs),
[property paths](../../tests/test_super_property_paths.cjs),
[assignment](../../tests/test_super_property_assign.cjs),
[static assignment](../../tests/test_super_static_put.cjs),
[adding assignment](../../tests/test_super_put_add.cjs),
[constructor](../../tests/test_super_constructor_retarget.cjs),
[bound parent](../../tests/test_super_bound_parent.cjs),
[construct paths](../../tests/test_super_construct_paths.cjs) and
[call retarget](../../tests/test_opt_call_retarget.cjs) regressions.

### Where `super` Is Allowed

`super` is emitted only by `emit_super` (the object of a member access or the
callee of a call); any other use of the identifier is a compile-time
SyntaxError, raised before any code in the file runs.

- `super.x` needs a `super` local in the lexical owner (the nearest non-arrow
  function): methods, accessors, constructors, static blocks and field
  initializers have one.
- `super()` needs the lexical owner to be a derived constructor
  (`is_derived_ctor`).
- Eval code resolves `super` by name. The eval scope records
  `super_binding` and `is_static` at the call site, and `super()` in eval also
  needs the caller's `this` binding. Functions declared inside eval code do not
  inherit. A literal-string eval that mentions `super` is not inlined, so its
  error stays a run-time SyntaxError of the eval call.
- A constructor may be written with a quoted key (`"constructor"() {}`).
- The parser accepts `super` only directly before `.`, `[` or `(`
  (`l_super`); `super?.()`, `delete super`, `(super).x` and `super => 1` are
  SyntaxErrors.

### Repeated `super()` And Returns

- When `super()` can run more than once (in a loop, a `try`, an arrow, eval),
  the call site checks the `this` binding first and, if it is already set,
  passes a fresh object made from `new.target.prototype` as the receiver. The
  parent runs again, then `emit_this_store` throws the ReferenceError. The
  pre-allocated `this` is never handed to a second parent call.
- The flow proof (`this_flow_stmt`, `this_flow_expr`) accepts `super()` as a
  statement, in `if`/`else`, in comma and conditional expressions and in
  declaration initializers. Constructors it proves need no binding and no
  checks.
- Derived constructors never use tail calls (`is_tail_callable`): the return
  value has to pass the derived-constructor return check.

### Private Methods

Instance private methods and accessors are created once per class evaluation,
after the prototype exists and before static initializers run, into hidden
locals inside the class scope (`compile_private_instance_methods`). The class
scope is opened before any per-class slot is reserved (private method slots,
computed field keys, private name tokens), so each evaluation of a class in a
loop gets its own. Field initialization installs all private methods and
accessors first, then fields in source order (`emit_field_inits`), reading the
shared closure through an upvalue. A private method gets a home object
(`OP_SET_HOME_OBJ`) only when its compiled body references `super` or contains
a direct eval (`class_method_needs_home`): the prototype for instance methods,
the class for static ones.

Instance private methods are not copied onto instances. A class with at least
one of them has a hidden brand token (`sv_private_scope_t.brand`); field
initialization defines that one brand entry on the instance
(`emit_field_init_pass`), and every use of `obj.#m` compiles to a brand read
(`OP_GET_PRIVATE` on the brand token, result dropped) followed by a load of the
class-level closure (`emit_private_get`). `emit_private_token` returns the
brand token for an instance method name, so `#m in obj`, writes (which throw:
the brand entry has method kind) and the double-initialization TypeError all go
through the same entry. The closure's hidden local is written exactly once, so
the upvalue stays foldable in the opt tier; do not add an initializing store.
Accessors and static private methods still use per-object entries.

A private table with `cap == ANT_PRIVATE_INLINE_CAP` (1) is the single entry
allocated behind the sidecar; `ant_private_table_is_inline` is the only test
for it, and heap tables start at `ANT_PRIVATE_HEAP_MIN_CAP`. The trailing entry
exists only when the sidecar is created by `ant_object_create_sidecar` for a
private entry whose token carries `ANT_PRIVATE_HASH_SOLO` (the compiler sets
that bit in the token hash when the class contributes exactly one entry to
that kind of object: one field, one accessor, or any number of instance
methods sharing the brand). Every other sidecar is `sizeof(ant_object_sidecar_t)`
and gets a heap table on its first private entry. An inline table may be full,
so `sv_private_find_slot` is bounded and returns `SV_PRIVATE_NO_SLOT`; heap
tables always keep a free slot. The optimizing tier's private load and store
read `private_table.cap` and `private_table.entries` and work for both
layouts. Read the token hash with `sv_private_hash_token`; the solo bit is
part of the hash everywhere.

### Class Name Binding

A named class has an inner immutable binding of its name, created in the class
scope before the heritage expression is compiled and left in TDZ
(`OP_SET_LOCAL_UNDEF`, `is_tdz`) until `OP_DEFINE_CLASS` has run. Heritage and
computed keys therefore see an uninitialized binding (ReferenceError, also for
`typeof`). The constructor and field initializers are compiled with the flag
cleared because they cannot run before the binding is set, which keeps their
reads of the class name free of TDZ checks. The heritage expression is compiled
with the outer private-name scope.

### Class Code Is Strict

`parse_class` and `compile_class` turn strict mode on for the whole class:
name, heritage, computed keys, field initializers, static blocks and methods.
Run-time strictness is per function (`func->is_strict`), so a heritage or
computed-key expression inside a sloppy function that assigns, updates, deletes
or calls `eval` is wrapped by the parser (`class_strict_expr`). An expression
that cannot suspend becomes an immediately called strict arrow. One that
contains its own `yield` or `await` cannot move into another function, so it
becomes an `N_STRICT_EXPR` instead: the compiler brackets it with
`OP_SPECIAL_OBJ` `SV_SPECIAL_STRICT_ENTER` / `SV_SPECIAL_STRICT_LEAVE`, which
save the VM's strictness override (`vm->jit.mode_fp`, `vm->jit.mode_strict`)
in a hidden local, set it to the current frame, and restore it afterwards. The
saved pair lives in a local, not on the operand stack, so it survives a
suspension. A function that contains a strict region is interpreted
(`jit_compile_failed`); the two kinds are handled by the cold
`sv_special_obj_rare`. The heritage is a LeftHandSideExpression
(`parse_lhs`).

### Class Element Keys And Names

- Literal keys are canonical in the AST: a string literal is an `N_STRING` with
  escapes decoded, a numeric literal an `N_NUMBER` (printed by
  `literal_num_key`), a BigInt literal its decimal string. `static`, `async`,
  `get` and `set` are modifiers only when a key follows (`class_modifier_here`);
  `async` also needs the key on the same line.
- Computed keys are evaluated and converted (`OP_TO_PROPKEY`, which propagates
  a throwing conversion) once, in source order, at class definition
  (`compile_class_precompute_key`), for named and anonymous classes alike.
  Object literals convert in `sv_define_method_comp`, which turns an object
  key into a property key before it names the function, so `toString` runs
  once per definition.
- `compile_class` takes the pending inferred name at entry and clears it, so
  the heritage, computed keys and members never see the name meant for the
  class. `OP_DEFINE_CLASS` always carries the name (`HAS_NAME`), and a
  synthesized constructor is named with `OP_SET_NAME` when its closure is made,
  so `name` is an own property before any static element runs and own keys are
  `length, name, prototype`. A static method, accessor or field called `name`
  then redefines it in source order (`sv_redefine_accessor_name`). An anonymous
  class in a computed key (a class field or an object literal property) is
  named from the stored key with a forced `OP_SET_NAME_COMP` emitted after the
  methods are defined and before the static fields run (`inferred_key_upval`;
  a negative value names a local that holds the key), so a static method called
  `name` wins and `static x = this.name` sees the key.
- A numeric key written as a legacy decimal (`08`, `0899`) keeps its source
  digits only when that is what `ToString` of its value prints; the lexer
  treats `0008` as decimal (`legacy_literal_is_decimal`).
- Calling a class without `new` throws from `OP_CHECK_CTOR`. In the optimizing
  tier that check is a slow-path exit; once exits at it are no longer allowed
  (`opt_may_speculate`), `opt_build_check_ctor` compiles the generic call so
  the function stops being recompiled.
- A non-computed static member named `prototype` is a SyntaxError; a computed
  one is a TypeError in `sv_define_method_comp`.

### Private Names In Direct Eval

`capture_dynamic_eval_scope` adds every visible private name to the eval scope
as a constant binding named with its `#`: the token (the class brand for an
instance method) under `#name`, and the method closure under `#name()`. Eval
code that does not find a private name in its own classes asks the root
compiler's `eval_env` (`eval_private_kind`) and reads the binding with
`OP_GET_EVAL_GLOBAL`. Indirect eval and `new Function` have no such
environment, so the name is undeclared there. A literal-string eval that uses
an undeclared private name, or `arguments` where the scope forbids it
(`arguments_forbidden`, carried in `sv_eval_scope_flags_t.no_arguments`), is
not inlined, so its SyntaxError is raised by the eval call.

### Class Member Rules

- A class has one constructor: a plain, non-static, non-computed method named
  `constructor` (quoted or not). An accessor, async or generator method with
  that name, a field with that name, or a second constructor is a SyntaxError
  (`invalid_constructor_member`).
- Class constructors never emit tail calls (`is_tail_callable`): a construct
  frame cannot tail-call, and the opt tier refuses to inline a constructor that
  ends in one.
- Static blocks are their own function scope with var, lexical and function
  hoisting.
- The parser rejects `arguments` in field initializers and static blocks
  (through arrows, not through nested functions), `await`, `yield` and
  `return` in static blocks, `yield` in every field initializer and `await` as
  an identifier in static field initializers (`static_block`, `no_arguments`,
  `no_yield` and a zeroed `function_depth` in `sv_parser_t`; `parse_func`
  clears them, and `parse_arrow_body` clears `static_block` because an arrow
  body may use `await` as an identifier).
- `break` and `continue` must resolve to a target in the same function
  (`compile_break`, `compile_continue`); a labelled plain block is a `break`
  target only (`sv_loop_t.is_block`), and a label stacked on another label
  reaches the loop underneath (`labels_loop`).
- `super` may not be assigned, used with `typeof`, or called from a field
  initializer.

### Contextual Keywords

`let`, `static`, `yield`, `await`, `async`, `of`, `get`, `set`, `as`, `from`
and the strict-mode reserved words are identifiers or keywords depending on
context, and the parser decides: `sv_parse_context_t` carries `in_async`,
`in_generator`, `no_yield` (generator parameters), `static_block` and
`class_initializer`, and `ident_tok_allowed` / `sv_reject_ident_name` /
`sv_reject_binding_name` are the only places that answer whether a token may
be an identifier reference or a binding name. An escaped keyword is never a
keyword (`st.escaped` on the token) and is rejected where the unescaped
spelling would be reserved: the lexer returns `TOK_ESCAPED_KEYWORD` for it,
which sits inside the identifier-like token range, so property keys, member
names, class member names and import/export names accept it
(`is_private_ident_like_tok`) and every other position reports an unexpected
token. Import bindings are checked by `import_binding_here`: `undefined`,
`window` and `globalThis` are ordinary binding names there, `eval`,
`arguments` and strict-mode reserved words are SyntaxErrors, and an
`export { x }` of a name the module does not declare is a SyntaxError at
compile time. A statement that starts with `let` is a
declaration only when the next token can start a binding
(`sv_lexer_lookahead`; the peek cache belongs to that function and
`sv_lexer_lookahead_newline` alone). `import.meta` is accepted in the script
goal on purpose.

### Parameter Scope

A function whose parameter list has expressions that can capture bindings
(default values or computed keys containing a closure or a direct eval) gets a
separate parameter scope (`param_split`): parameters occupy the locals below
`param_bind_end`, and a body `var` or function with the same name gets its own
local initialized from the parameter (`split_param_binding`), so closures made
in the parameter list keep seeing the parameter. `arguments` is bound before
the parameter expressions run when they can observe it, and a body
`var arguments` shares that binding (`bind_var_arguments`), unless a
parameter is itself named `arguments`: then the body `var` is an ordinary
split copy of the parameter. A function that owns an eval environment splits
every parameter that the body redeclares. A sloppy function with a mapped
arguments object never captures a parameter by value
(`param_captured_by_value`, also while a class body makes the compiler
strict), and closures created by `OP_CLOSURE_EVAL` are linked to the
arguments object like those of `OP_CLOSURE_ARGS`
(`sv_closure_link_arguments`).

### `with` And Direct Eval

A direct eval inside `with` must see the with object. The compiler records
each active `with` object and each block scope between it and the eval as
hidden bindings of the eval scope named `"\x01with:N"` and `"\x01scope:N"`
in nesting order. Eval code resolves a free name by walking those markers
innermost first (`sv_eval_env_each_with`): a with object that has the name and
does not hide it through `Symbol.unscopables` wins, so reads, writes, `typeof`,
`delete`, calls (with the object as `this`) and `var` declarations go to it;
otherwise lookup continues outward. Such references are `SV_WITH_REF_ENV` and
are loaded with `OP_GET_EVAL_GLOBAL`. `sv_eval_env_facts` reports in one scan
whether an environment has markers and private names, and
`has_active_with_scope` is O(1) (`with_depth`, `with_inherited`). An `eval`
property on the with object shadows the global function, so the call is then
an ordinary call.

### Large Scopes

Slot, upvalue and private-name operands are 16 bits wide. A function scope
that would exceed `SV_SPILL_THRESHOLD` (32768) locals moves the bindings past
the threshold onto a null-prototype scope object that is entered like a `with`
scope (spill scopes); loop-head lexical bindings and `using` declarations
never spill (`spill_disabled`), and hidden unnamed locals are never
TDZ-marked. A class with that many private names keeps them in a table object
instead of locals. Private-name, upvalue and eval-variable lookups are hashed
once a scope is large, so compile time stays linear. Spilled `let`/`const`
bindings have no exact TDZ.

### Compile Time Is Linear

Left-deep binary chains (`a + b + c + ...`), member and index chains, nested
blocks and long declaration lists are compiled iteratively or with O(1) work
per element: `compile_arithmetic_chain` walks the left spine once, operand
types come from `infer_arithmetic_type` without re-walking the subtree, and
scope bookkeeping does not rescan earlier bindings.
`tests/test_compile_time_linear.cjs` bounds the ratio between two sizes.

Every other left-deep binary or logical chain (`%`, `&`, `<<`, `&&`, `??`,
comparisons) goes through `compile_binary_spine`, which collects the spine and
emits it bottom-up, so chain length is not bounded by the native stack.
`infer_assign_type` stops after `SV_TYPE_INFER_DEPTH` nested assignments
(deeper operands are untyped), which keeps `a += a += ...` linear.

### Nesting Limits

Nothing in the front end may recurse on source nesting without a bound:

- A template literal is scanned by `skip_template_literal` with an explicit
  frame stack. The ends of nested templates found by one scan are kept in the
  lexer (`tpl_spans`, sorted by start offset, allocated from the parse arena)
  and reused when the parser lexes the `${...}` contents again, so nested
  templates are linear.
- The parser builds member, call, binary and comma chains in loops. Each loop
  counts its links (`sv_parse_chain_link`); every 4096 links the total of all
  chains in progress (`chain_links`) is compared with the native stack that is
  left, at `SV_PARSE_CHAIN_LINK_BYTES` per link, and the parse ends with
  RangeError when the tree would be deeper than the unchecked AST walkers
  (`ast_scope_facts`, `write_scan`) can follow. Recursive productions use
  `sv_parse_stack_overflow`; `new` chains included.
- `Array.prototype.flat` checks the native stack once its remaining depth
  exceeds `FLAT_UNCHECKED_DEPTH`.
- `js_delete_prop_ordinary` and `js_delete_sym_prop_ordinary` return `true`
  for every receiver that is not an object before converting it with
  `js_as_obj`, which would turn a number into a wild pointer.
- The template JIT's `instanceof` fast path reads the receiver's prototype
  through its shape before any shape guard, so it branches to the slow path
  when the shape pointer is null: `-Infinity` has the bit pattern of an object
  reference with payload zero, which decodes to the cage's read-only first
  page.

`tests/test_compiler_operand_limits.cjs` runs the shapes at 1e3, 1e5 and 1e6.

### Direct Eval Scopes

A closure created in code that inherits an eval environment (`OP_CLOSURE_EVAL`)
captures the bindings it can see as an eval scope. Scopes form a chain:
`sv_eval_scope_t.parent` is the index (plus one) of the enclosing scope in the
same function, and a scope lists only the locals declared since its parent.
`capture_dynamic_eval_scope` reuses the last scope when no local was popped,
no `with` scope changed and no binding flag changed since
(`eval_scope_shareable`, cleared by `eval_scope_locals_popped` and at the
`eval_flags` updates); a full capture is split at each scope depth so sibling
blocks share their ancestors. Compile cost for n closures in one eval is
therefore linear.

At run time `sv_eval_capture_closure_env` keeps the last closure environment
of the isolate (`js->eval_closure_env`, cleared at the start of every
collection) and reuses it, or one of its ancestors, when it still describes
the frame: same function and scope, same `arguments` object, same parent
environment, and every cell is still the open upvalue of its slot
(`sv_eval_env_state_current`). The `arguments` property is set only on the
root environment and only when the scope does not bind a local named
`arguments` (`binds_arguments`).
`tests/test_eval_closure_scopes.cjs`.

### Optional Chains

A `?.` short-circuits the whole chain it starts, and only when the value
directly before it is nullish. The compiler threads one patch list through
the chain: the outermost member, call or optional node owns it, hands it to
its base through `c->optional_chain` (taken at the entry of `compile_member`,
`compile_call` and `compile_optional`, and only passed to an unparenthesized
chain element), and every `?.` jumps to the end of the chain with
`undefined` in place of the base. Links after a `?.` are plain accesses, so
`({ b: null })?.b.c` throws. Parentheses end the chain: `(a?.b).c` reads `.c`
from the chain's result and `(a?.f)()` still calls with `a` as the receiver.
`delete` of a short-circuited chain is `true`. A call whose callee continues
an optional chain is never a tail call.

### Local Type Facts

`OP_ADD_NUM`, `OP_SUB_NUM`, `OP_MUL_NUM` and `OP_DIV_NUM` have no type guard
in any tier, and `typeof` of a typed `const` folds to a constant, so the
compiler's local types (`sv_local_t.inferred_type`) are facts, not hints.
The rules that keep them true (`src/silver/compiler/analysis.c`,
`type_assign_local` and friends):

- A type is inferred after the operand is compiled, never before, so an
  assignment inside the operand (`n + (n = 's', 1)`) is already visible.
- A local gets a fact (`type_fact`) from a declaration initializer or a
  store whose value has the same known type. Parameters are never typed.
  A store of any other value clears the type. `x++`, `x -= e` and the other
  numeric-result operators keep a number a number; `+` needs both sides.
- A fact created inside a conditional region (an `if` arm, a loop body, a
  `case`, a `try` or `catch` block, a labelled statement) is dropped when the
  region ends, because other paths reach the code after it.
- Inside loops every typed op and every fact derived from another fact is
  logged (`sv_type_use_t`). When a fact that existed before the loop is
  broken anywhere in the loop, the ops that relied on it since the loop
  started are rewritten in place to the generic opcode and the facts derived
  from it are dropped the same way. A loop therefore never keeps a typed op
  whose operand can change type on a later iteration.
- A captured, non-const local can be written by a closure created on an
  earlier iteration (or by direct eval), so when its fact ends, or at the
  end of any loop during which a child function or eval scope was created,
  every logged use since the outermost loop started is rewritten.
- A typed `const` whose type was derived from another local inside a loop is
  not folded by `typeof`.

The slot summary handed to the template JIT (`slot_types`) is still only a
hint; the JIT guards its numeric local stores.
`tests/test_local_type_inference.cjs` holds the fixed cases and a generated
comparison of every case against the same program with opaque initializers.

### Captured Locals In The Template JIT

A captured local lives in the frame slot (`r_lbuf`); its register is a cache
that is stale after any helper that can run user code. The bailout spill
block never writes captured locals back, and `OP_ADD_LOCAL`, `OP_INC_LOCAL`
and `OP_DEC_LOCAL` reload a captured local before reading it. In the
optimizing tier a generic step that reads or writes a boxed local copies the
box into the frame slot before the step and back after it.

### Construct Results And Prototype Setters

- `sv_construct_result` is the one rule for what `new` returns: an object or
  builtin function returned by the constructor, else the constructor's final
  `this`, else the pre-allocated object. Use it for any new construct path.
- `Object.setPrototypeOf`, `Reflect.setPrototypeOf`, the `__proto__` setter and
  a proxy's default all refuse a different prototype on a non-extensible object
  (`js_set_proto_allowed`) and refuse cycles including the object itself
  (`proto_would_cycle`); the first and third throw, the other two return
  false. All four dispatch to a proxy's `setPrototypeOf` path.
- `sv_construct` is Construct(F, args, newTarget) for native callers
  (`Reflect.construct`, a proxy's default construct, array species). When
  newTarget is not a class and F is a derived constructor it runs F in the
  interpreter with an undefined receiver, and `sv_super_construct_parent`
  creates the object only when the base constructor is reached, so
  `newTarget.prototype` is read after the derived body's code before
  `super()`. Do not pre-allocate a receiver for a derived constructor on a
  path that takes a foreign newTarget.
- While a native constructor runs under a proxy newTarget,
  `js->construct_proxy_target`/`construct_proxy_proto` hold the prototype that
  was read once; `js_instance_proto_from_new_target` returns it instead of
  reading the proxy again.
- A builtin function used as a prototype is stored as its promoted function
  object. Every path that hands a prototype back to script
  (`Object.getPrototypeOf`, `Reflect.getPrototypeOf`, the `__proto__` getter,
  a proxy's default) maps it back with `js_cfunc_demote_promoted`, and every
  path that takes one promotes it first. `js_expose_cfunc_for_key` names only
  anonymous builtins; a named builtin stored under another key keeps its
  identity and name.
- A proxy's constructor and callable bits are fixed when it is created;
  `js_is_constructor` reads the flag and never walks proxy targets.
- A proxy without a `has` trap forwards to the ordinary has-property walk of
  its target (`js_in_str`/`do_in`), which calls the traps of proxies further up
  the chain. Recursive proxy operations start with `sv_check_c_stack_overflow`.
- A proxy whose handler has no `set` trap forwards to the ordinary set of its
  target with the same receiver (`js_set_with_receiver`), so the handler is
  read in the order `set`, `getOwnPropertyDescriptor`, `defineProperty` and
  symbol keys take the same route as string keys. Two shortcuts are allowed:
  a handler with none of those three traps (`proxy_handler_plain_for_set`)
  stores straight into the target, and an own writable data property of a
  plain target goes through `proxy_set_own_writable`, which still calls both
  traps once each.
- Every invariant check and trapless fallback in `src/modules/proxy.c` uses
  the target's internal method when the target is itself a proxy
  (`proxy_target_extensible`, `proxy_target_own_keys`,
  `proxy_target_own_descriptor`, `proxy_check_proxied`). The plain-target fast
  paths look the property up first and only test for a proxy target on a miss,
  because a proxy object has no own properties.
- `for...in`, `Object.prototype.propertyIsEnumerable`, `Function.prototype.bind`
  and `Object.getOwnPropertyDescriptors` call the traps of a proxy anywhere
  they meet one, including in a prototype chain; `ownKeys` accepts any
  array-like result. `js_proxy_apply` walks trapless nested proxies in a loop
  and rejects a proxy without the callable bit before looking at its target.
- A builtin constructor whose `new.target` has a non-object `prototype` falls
  back to the builtin's own prototype (`sv_construct_default_prototype`); a
  script function falls back to `Object.prototype`. A proxy `new.target` has
  its `prototype` read exactly once.
- `Object.isFrozen` ignores the writable `length` of an array, as V8 does, so
  a non-extensible array without elements is frozen.
- Call and `new` argument counts go through `emit_argc`; more than 65535
  arguments, locals or captured variables is a SyntaxError and a tagged
  template with 65535 or more substitutions throws a RangeError when it runs.
  Class constructors never tail-call, which includes `OP_TAIL_MAP_TEMPLATE`.
- The handler of a try-finally region is `OP_POP`, an optional
  `OP_CLOSE_UPVAL`, then `OP_JMP` to `OP_FINALLY`. The interpreter lands every
  abrupt completion (throw, return, break, continue) on the handler, so the
  close runs before the finally body, whose locals may reuse the closed slots.
  `jit_finally_of_region` and `opt_finally_of` both accept the close. The
  optimizing tier jumps from a return or `OP_UNWIND_JMP` straight to
  `OP_FINALLY`, so `opt_jump_to_finally` applies the handler's close to the
  boxed slots itself (`opt_close_boxes`) before the jump.
- An OSR loop entry boxes every captured local whose closure can reach the
  loop header (`entry_boxes`). Reachability follows exception edges
  (`block->handler`) as well as successors: a loop inside a catch handler is
  reached from a closure in the try body only through the handler edge, and an
  entry that leaves such a local unboxed keeps the interpreter's open upvalue
  alive across the per-iteration close.
- Template-JIT code keeps captured locals in registers and in `r_lbuf`. Every
  catch landing (`OP_CATCH`) and every finally landing reloads the registers
  from `r_lbuf`, because the operation that threw may have run a callee that
  wrote through the open upvalue.
- Closures created in class code inside a sloppy function that owns a mapped
  `arguments` object use `OP_CLOSURE_ARGS` (`args_mapped`), the same as
  closures in the function body; class code being strict does not change the
  enclosing function's parameter mapping.
- `js_to_property_key` is ToPropertyKey: ToPrimitive with hint string, a
  symbol result stays a symbol. `OP_TO_PROPKEY`, its template helper and
  `sv_define_method_comp` all use it, and every tier propagates its error.
- The arrow the parser wraps around a sloppy class heritage or computed key
  (`class_strict_expr`) is marked `FN_SYNTHETIC`; `is_synthetic` functions are
  left out of captured stacks, both in the live walk and in the lazy expansion
  of compiled frames.
- A tagged template compiles its tag like a call callee, so member tags
  (`obj.tag`, `super.tag`) are invoked with their receiver.
- A tagged template is part of the member expression `new` applies to
  (``new F`x` `` is ``new (F`x`)``), and a template after an optional chain is
  a SyntaxError (`parse_call`, `parse_tagged_template`).
- Changing the prototype of a shape that is already a prototype-transition
  child goes back to that child's base shape (`shape_proto_base`), so
  alternating prototypes reuses two shapes instead of growing a chain.
  `shape_prune_dead_children` and `shape_destroy` walk the tree iteratively;
  keep them free of recursion.

## Proxy Trap Lookup

`ant_proxy_state_t` caches which traps the handler can have: `trap_mask` has a
bit per trap name found anywhere on the handler's prototype chain, valid while
`trap_epoch == js->ic.epoch`. `proxy_trap_mask_refresh` fills it only when the
whole chain is ordinary (non-exotic objects, so not when the handler or one of
its prototypes is a proxy) and marks every object on the chain with
`ant_object_guard_absence`, so adding a property to any of them, or changing a
prototype, bumps the epoch. A clear bit means the trap is absent without a
lookup (`proxy_trap`); a set bit reads the trap from the chain by interned
name (`js->intern.proxy_traps`, filled on first use) and falls back to
`proxy_get_method` for accessors and non-callable values, so a trap that is
deleted or set to `undefined` needs no invalidation. Every trap lookup in
`src/modules/proxy.c` goes through `proxy_trap`.

Two paths skip the generic machinery when the mask allows it:

- `for...in` over a proxy without `ownKeys`, `getOwnPropertyDescriptor` and
  `getPrototypeOf` traps enumerates the target in the same loop
  (`proxy_enumerates_as_target`); the hop does not count against the
  prototype-depth limit. A revoked proxy never takes this path.
- An assignment through a proxy whose handler has no `set`, `defineProperty`
  and `getOwnPropertyDescriptor` traps (`proxy_handler_plain_for_set`) stores
  straight into an own writable data slot of a plain target.

Every recursive proxy entry point checks the native stack
(`sv_check_c_stack_overflow`), `proxy_own_keys_raw` included, so a chain of
nested proxies ends in RangeError rather than a crash.

The invariant check after a `getOwnPropertyDescriptor` trap asks the target
again (`proxy_target_own_descriptor`). There is no depth limit; one outermost
operation may do `PROXY_DESCRIPTOR_CHECK_BUDGET` nested re-checks before
RangeError, which only chains of traps that themselves forward to the target
reach. A trap result of `undefined` for a key the target does not have returns
without asking `isExtensible`. An array-like `ownKeys` result is read element
by element and rejected at the first value that is not a string or symbol
(lengths above 2^32-1 are a RangeError).

## Integrity Levels

- `Object.freeze` and `Object.seal` clear attributes with one
  `ant_shape_clear_attrs_all` pass and take a private copy of the shape only
  when the object has shape properties, so freezing an array or an empty
  object does not clone a shape.
- `Object.isFrozen` ignores a function's writable `prototype` and an array's
  `length`, as V8 does.
- A typed array with elements cannot be frozen or sealed (TypeError, and the
  view is left non-extensible); a non-extensible view is sealed, and frozen
  only when it is empty (`object_integrity_kind`, `object_view_integrity`).
- A module namespace is made non-extensible with non-configurable exports
  when loading completes (`esm_seal_namespace`, also for namespaces built from
  a value), and `sv_module_export_to_ns` keeps exports writable, enumerable
  and non-configurable when a live binding is updated. `Object.freeze` of a
  namespace with exports is a TypeError. Namespaces are otherwise ordinary
  objects: an assignment from outside the module is not rejected.
- The `vm` context global refuses `preventExtensions` by returning false, so
  `Object.freeze`, `seal` and `preventExtensions` throw their own message and
  `Reflect.preventExtensions` returns false.

## Generic Array Methods

- Lengths of array-likes go through `ToLength`, also for proxies
  (`proxy_aware_length`, which answers a real non-exotic array inline and
  calls `proxy_aware_length_slow` for everything else); index keys above 2^32
  are formatted from the 64-bit
  index (24-byte buffers in `array_method_has_index`,
  `array_method_get_index`, `array_set_index_throw`,
  `array_delete_index_throw`).
- `reverse`, `copyWithin`, `splice`, `sort` and `lastIndexOf` on anything that
  is not a real array (and on proxies) use those helpers in spec order and
  propagate every failure: `array_reverse_generic`, `array_move_range_throw`
  (its `backward` flag is the caller's overlap decision), `array_splice_large`
  for lengths above 2^31-1, `array_sort_write_generic`. `indexOf` and
  `lastIndexOf` on a proxy ask `has` before `get` (`array_search_proxy`).
- `sort` returns before reading anything when the length is below two and
  keeps up to 32 elements in stack buffers.
- `shift` and `unshift` on a proxy or on anything that leaves the dense fast
  path use `array_shift_generic` / `array_unshift_generic` (HasProperty, Get,
  Set, DeletePropertyOrThrow in spec order, failures propagated). A proxy
  whose handler has no traps at all (`proxy_is_transparent`) over a plain
  dense array is handled as its target. The dense `unshift` path also covers
  an array whose length exceeds its storage when nothing lives past the
  storage (no index-keyed named properties, plain prototype chain), and it
  leaves to the generic path when a hole could read through the prototype.
- `splice` checks the length limit the way V8 does, in double arithmetic
  (`len + insertCount - deleteCount > 2^53 - 1`), so
  `splice.call({ length: 2 ** 53 - 1 }, -2, 1, 'a', 'b')` succeeds and leaves
  `length` at 2^53 as node does, while inserts that clearly overflow throw.
- A result array longer than 2^32-1 is refused before anything is read:
  `array_alloc_intrinsic` (species create with the intrinsic constructor, so
  `slice`, `map`, `splice`), `Array.from` of an array-like, `toReversed`,
  `toSorted`, `with` and `toSpliced` throw RangeError; `toSpliced` and `push`
  throw TypeError past 2^53-1; `join` and `toLocaleString` throw TypeError
  above 2^32-1 and RangeError when the separators alone exceed the string
  limit; `concat` skips a spreadable array-like longer than 2^32-1 as V8 does.
- `pop` and `shift` on a proxy go straight to the generic helpers (one length
  read, `Set` of `length` that throws on a frozen target). `toReversed` on
  anything but a real array reads from the end; `flat` on anything but a real
  array reads `length`, then the depth argument (`ToIntegerOrInfinity`), then
  `constructor`. `Array.from` of a trapped proxy over an array re-reads
  `length` after each element as the array iterator does.
- The array iterator compares the index with the full 64-bit length.

## FinalizationRegistry

A registration is `[target cell, held value, token cell]`, where a cell is an
internal WeakRef object (`js_weakref_create`) whose state has
`notifies_registry` set. Targets and tokens are therefore weak and held values
strong. Each registry is itself listed through such a cell in an array on
`FinalizationRegistry.prototype`'s `SLOT_DATA`, so an unreachable registry is
dropped. When the collector clears a notifying cell
(`gc_weak_prune_collection`) it sets `js->weak_gc.finalization_pending`; the
flag is examined at the job boundary after the microtask queue drains
(`process_microtasks_internal`), where `js_finalization_run` removes the dead
registrations and calls each registry's callback with the held value. No
callback runs inside the collector or synchronously inside `register`.

## Builtin Modules Under `require`

A builtin written as a JS bundle module without a default export gets one when
it completes (`esm_builtin_default_exports`): an ordinary extensible object
holding the namespace's values, stored as the namespace's `default`. `require`
and a default import both return it, so `require('node:http')` can be patched
as in node, while `import * as ns` stays a sealed namespace. Native library
builtins already carry their exports object as the default.

## Dynamic Import And Top-Level Await

`import()` converts the specifier to a string, validates the attributes and
captures the referrer synchronously; loading and evaluation run from a
microtask (`builtin_import_job`), so nothing in the module runs before the
caller's synchronous code finishes, and load failures reject the promise.
The one synchronous shortcut is a module that is already loaded with a settled
evaluation: `import()` resolves it in the call and returns a promise for the
namespace (`js_esm_import_loaded`), since nothing is left to run. It is skipped
when import attributes or loader hooks are present.

A module whose evaluation is asynchronous keeps its promise in
`SLOT_MODULE_EVALUATION` on the namespace object. That slot is the only record
of evaluation state:

- A module body that returns a promise (top-level await) stores it there. For
  a static graph outside `import()` the loader then runs the event loop; under
  `import()` it never does, because the load runs inside a job.
- `esm_instantiate_static_dependencies` collects the pending promises of the
  direct dependencies. When there are any, the importer is compiled and its
  evaluation is deferred (`esm_defer_evaluation`) until they settle; its own
  slot holds a promise that settles with the deferred body.
- A rejected evaluation promise makes every later load of that module throw the
  same reason; the body does not run again. A module that fails synchronously
  gets an already-rejected promise for the same effect
  (`esm_record_evaluation_failure`).
- `require()` never evaluates an asynchronous graph. While a `require()` load
  is on the stack (`require_depth`), a module that has top-level await, or a
  static dependency whose evaluation is or was asynchronous, fails with
  `ERR_REQUIRE_ASYNC_MODULE` before its body runs, and that failure is not
  recorded as the module's evaluation result, so a later `import()` still
  loads it. A synchronous importer of an already-settled asynchronous
  dependency inherits the dependency's promise in its own slot, which is how
  `require()` recognises an async graph that was loaded earlier by `import`.
  Running the event loop from `require()` would deadlock when the caller is
  itself inside an awaiting module.
- Under `import()` evaluation promises are marked handled: the rejection is
  reported through the promise `import()` returned.

Resolution failures carry node's codes and message shapes
(`esm_import_not_found_error`, `esm_module_not_found_error`):
`ERR_MODULE_NOT_FOUND` with "Cannot find module '<absolute path>' imported
from <importer>" for relative and absolute specifiers and "Cannot find package
'<name>' imported from <importer>" for bare ones, `ERR_UNKNOWN_BUILTIN_MODULE`
for an unknown `node:` specifier, and `MODULE_NOT_FOUND` with "Cannot find
module '<specifier>'" from `require()`. `import.source(x)` is compiled as an
`import()` whose second operand is the internal marker
`SV_IMPORT_SOURCE_PHASE`; it resolves the specifier and rejects with a
SyntaxError coded `ERR_SOURCE_PHASE_NOT_DEFINED` without loading anything.

Modules are compiled with `js->filename` set to their own path
(`esm_compile_parsed_record`); a deferred module is compiled while its importer
is still the active file.

## Parallel Marking

Major collections with at least 65536 live objects drain the mark stack on
helper threads (`src/gc/parallel.c`, `--gc-threads <count>` overrides the count).
Helpers only scan plain objects and arrays, claim objects with an atomic
`mark_epoch` exchange, and mark flat strings and shapes through the
`_shared` variants. Everything else is handed back to the main thread between
rounds. Keep new marking work for other kinds main-thread-only unless it is
made safe for concurrent callers, and keep heap sizing driven by total marking
CPU (`parallel_cpu_ns`), not wall time, or the policy shrinks the heap. See
the [parallel marking](../../tests/test_gc_parallel_mark.cjs) regression.

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

Closures are marked by recursion, not through the mark stack, so a chain of
closures that only reference each other through upvalues is as deep as the
chain. `gc_mark_closure` marks the closure and, once the native stack is past
`js->cstk.floor`, pushes it on `js->gc.deferred_closures` instead of scanning
it; `gc_drain_mark_stack` and `gc_drain_mark_stack_weak` scan the deferred
closures before they return, so every marking phase that ends in a drain sees
them.

## RegExp And Rope Allocation Lifetimes

Compiled RegExp data needs object ownership before named-group metadata
allocation can trigger GC. Cache membership alone cannot protect it across
multiple major collections. Preserve the attachment/error-path reference
handling in [compile.c](../../src/modules/regex/compile.c), covered by the
[result regressions](../../tests/test_regexp_result_batch.cjs).

Rope mark-table allocation failure must still allow collection to progress.
A failed minor retries as a major; a major without metadata conservatively
roots initialized rope-pool contents while collecting ordinary objects and
retaining required rope blocks. Merely skipping collection or retaining blocks
without tracing their values is unsafe. See
[ropes.c](../../src/gc/ropes.c) and [gc.c](../../src/gc/gc.c).
