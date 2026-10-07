# EventEmitter Node Parity

Status: active
Last reviewed: 2026-10-07
Owner: theMackabu

## Goal

Match or beat Node on the `EventEmitter1` (`require('events')`) rows of the
[tseep](https://github.com/Morglod/tseep) `benchmarks/run/ee` suites:
emit-empty, remove-emit, add-remove, init, once, emit,
emit-multiple-listeners, hundreds, listeners. Keep Node semantics.

## Scope

`src/modules/events.c`, plus the engine paths the suites exercise: the
`arguments.length` read inside every benchmark listener, JIT calls to native
builtins, native constructor calls, and dense array creation.

## Decisions

- **`arguments.length`-only functions.** The parser marks functions whose
  every `arguments` reference is a plain read of `arguments.length`
  (`FN_ARGS_LENGTH_ONLY`, `ast_arguments_length_only` in `ast.c`). Writes,
  deletes, for-in/of targets, arrows, direct eval, `with`, and any binding
  named `arguments` disqualify. Such functions skip the arguments object and
  compile `.length` to `OP_SPECIAL_OBJ 4` (argc), which the interpreter, JIT
  and inliner all handle. Before this, the listener's `arguments.length`
  check cost ~60% of `emit` and kept sloppy listeners off the JIT entirely.
  `ast_references_arguments` does not visit `cond`/`init`/`update`, so the
  new flag is computed independently of `FN_USES_ARGS`.
- **Direct JIT -> builtin calls.** `OP_CALL`, `OP_CALL_METHOD` and inlined
  call sites test for a `kTypeBuiltin` callee and call its C function pointer
  directly (`mir_emit_builtin_call_fast`), mirroring `sv_vm_call`'s builtin
  path: nullish `this` becomes the global, `js->this_val` is saved and
  restored, and error/pending-exception results go through
  `jit_helper_native_finish`. It falls back to `jit_helper_call` at VM depth 0
  (microtask checkpoint) or with an exception already pending. This adds
  about 30 MIR instructions per call site (hot compile of the benchmark.js
  loop: 7.4 ms -> 8.2 ms).
  Since 2026-10-07 the block is emitted only where it can pay off: the
  interpreter records `SV_TFB_CALLED_BUILTIN` in a call site's type-feedback
  byte, and a site that only ever called JS functions gets a 2-instruction
  watch instead (`mir_emit_builtin_call_watch`). The first builtin reaching
  it sets the bit and drops the owner's code once, so the recompile has the
  direct path. A site with no feedback keeps the block. bench-v8: up to
  -0.9% instructions and -1 to -5% compile time.
- **Listener dispatch.** Plain sync closures are entered through
  `sv_jit_invoke` when they have JIT code, otherwise through
  `sv_call_resolve_closure`; the call plan is skipped. Dispatch, lookup and
  the listener call are force-inlined into `emit`; per-entry cold metadata is
  only read when an entry has flags; sweeping is skipped when nothing was
  removed during the emit.
- **Listener storage.** An emptied `EventType` is kept for reuse while the
  emitter has at most 16 types (on/off churn no longer reallocates). `once()`
  wrappers are created lazily on the first `rawListeners()`. A string-key
  hit by bytes adopts the probe value so later lookups from that call site
  hit the identity check. `listeners()` writes straight into an exactly
  sized dense array (`js_mkarr_dense_uninit`).
- **Constructor.** `EventEmitter` no longer sets `SLOT_BRAND` (an
  out-of-line slot malloc per instance). `getEventListeners` recognizes
  emitters by their listener storage and otherwise calls `listeners()`, as
  Node does. `jit_helper_new` dispatches native constructors straight to
  `sv_call_native`, and `sv_invoke_native` links the `new_target` native
  frame inline instead of through `Ant_Silver_InvokeNativeScoped`.
- **Semantics fixed on the way.** `removeListener(rawListeners()[i])`
  removes a `once` listener, and duplicate removal takes the most recently
  added entry, both matching Node.

## Validation status

Revision `46c09bc7` plus the uncommitted diff; base pinned in a `/tmp`
worktree at the same revision, both configured `-Dpgo=disabled`.

- `examples/spec/run.js --all`: 4342 passed / 0 failed (base identical).
- `tests/harness/run.js`: 299 passed, `rolldown` snapshot fails on base too.
- `tools/check_stack_depth.sh`: no rejected or overflowed functions; the four
  failing files (`test_node_events_once_prototype_spoof`, `test_throw_stack`,
  `test_with_strict`, `test_direct_eval_environment`) fail identically on
  base.
- New regressions: `test_arguments_length_only.cjs`,
  `test_events_listener_storage.cjs`, `test_jit_builtin_direct_call.cjs`.

tseep benchmark.js, `EventEmitter1` row only, ops/sec, arm64 macOS,
PGO build with the checked-in (now partly stale) profile:

| suite | node 26.8.1 | ant before | ant now |
|---|---|---|---|
| emit-empty | 42.5M | 22.4M | 29.1M |
| remove-emit | 42.6M | 21.0M | 26.5M |
| add-remove | 24.6M | 14.7M | 28.5M |
| init | 52.1M | 22.1M | 35.6M |
| once | 19.2M | 4.9M | 23.9M |
| emit | 26.3M | 4.2M | 15.6M |
| emit-multiple-listeners | 7.6M | 1.6M | 9.1M |
| hundreds | 1.27M | 0.20M | 0.95M |
| listeners | 42.1M | 4.0M | 23.7M |

## Follow-ups

Done since the table above (details in the linked plans):

- **Property-load IC on method calls.** Monomorphic prototype hits are
  checked against compile-time constants and the slot loaded directly
  (`mir_emit_get_field_proto_snapshot`): `ee.emit('foo')` with no listeners
  246 -> 212 instructions per call. Snapshots whose prototype or global
  object later changes shape drop the compiled code once and recompile
  ([Silver Loop Codegen](silver-loop-codegen-math-intrinsics.md)).
- **Array element storage.** Power-of-two buffers up to 32 slots are reused
  from a per-isolate free list: `[]` 22.1 -> 14.6 ns, `ee.listeners('foo')`
  958 -> 688 instructions per call
  ([Array and Arguments Runtime Invariants](array-arguments-runtime-invariants.md)).
- **`slice`, `forEach`, `delete arguments.length`.** Dense fast paths in the
  array helpers (`slice` of 25 elements 3,931 -> 1,162 instructions),
  prepared callbacks (`forEach` about 6 -> 3 ns per element), and spec
  arguments objects whose `length` is an ordinary configurable property
  (same plan).

Still open:

- **benchmark.js compile overhead.** benchmark.js builds a fresh loop
  function every cycle. Each is OSR-compiled at the hot tier because its
  bytecode (187 B) is under the 512 B cold cutoff from
  [OSR Size-Scaled Tiering](osr-size-scaled-tiering.md), but inlining makes
  the MIR large: ~8 ms per compile, ~1 s of a ~5.6 s run. Forcing a cold
  OSR compile (3.2 ms) raised every suite 10-25%. A cutoff based on the
  inlined size rather than raw bytecode would address it without
  re-litigating the bench-v8 data in that plan.
- Regenerate the PGO profile, together with the deferred bench-v8 run.
