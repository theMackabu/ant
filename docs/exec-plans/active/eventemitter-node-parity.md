# EventEmitter Node Parity

Status: active
Last reviewed: 2026-10-05
Owner: theMackabu

## Goal

Match or beat Node on the `EventEmitter1` (`require('events')`) rows of the
[tseep](https://github.com/Morglod/tseep) `benchmarks/run/ee` suites:
emit-empty, remove-emit, add-remove, init, once, emit,
emit-multiple-listeners, hundreds, listeners. Keep Node semantics. No commits
until the user asks.

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

## Remaining gaps and follow-ups

- **benchmark.js compile overhead.** benchmark.js builds a fresh loop
  function every cycle. Each is OSR-compiled at the hot tier because its
  bytecode (187 B) is under the 512 B cold cutoff from
  [OSR Size-Scaled Tiering](osr-size-scaled-tiering.md), but inlining makes
  the MIR large: ~8 ms per compile, ~1 s of a ~5.6 s run. Forcing a cold
  OSR compile (3.2 ms) raised every suite 10-25%. A cutoff based on the
  inlined size rather than raw bytecode would address it without
  re-litigating the bench-v8 data in that plan.
- **Property-load IC on method calls.** Each `ee.emit(...)` site runs ~80 JIT
  instructions, mostly the generic GET_FIELD IC, which reads the cache entry
  at runtime (receiver shape, proto, proto shape, holder). 64% of
  emit-empty is JIT code. Specializing monomorphic proto hits at compile time
  is the main lever for emit-empty, remove-emit and emit.
- **Array element storage.** Every array mallocs its element storage
  separately (~12 ns alloc+free; an empty array costs ~25 ns vs ~13 ns for
  an object). A per-isolate free list of small buffers would lift
  `listeners` and array-heavy code in general.
- **Unrelated slow paths found while profiling:** `Array.prototype.slice`
  copies through `arr_get`/`arr_set` (445 ns for 25 elements vs 15 ns in
  Node); `forEach` callbacks cost ~22 ns each vs <1 ns in Node;
  `delete arguments.length` fails because Ant makes it non-configurable.
- Regenerate the PGO profile once the diff settles.
