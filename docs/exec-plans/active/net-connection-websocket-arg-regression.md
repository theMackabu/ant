# Constructor Context and Accepted Socket Prototypes

Status: constructor fix committed; async entry cleanup uncommitted
Last reviewed: 2026-09-07
Owner: theMackabu

## Problem

`tests/test_websocket_client_buffered_frames.cjs` times out because the accepted
socket has `WebSocket.prototype`. Its own properties and native socket state
are correct. This is constructor-context leakage, not object-shape corruption.

## Verified cause

- `sv_op_new` and `sv_op_new_apply` in `src/silver/ops/calls.h`, and
  `jit_helper_new` in `src/silver/glue.c`, assign `js->new_target` before
  construction and leave it behind after returning.
- Ordinary calls clear that field in `sv_prepare_call` / `sv_vm_call` in
  `include/silver/engine.h`; returning from a constructor does not clear it.
- `net_socket_create` in `src/modules/net.c` calls
  `js_instance_proto_from_new_target`, including when reached directly from
  the native accept callback. It consumes the preceding WebSocket constructor.
- Native logging showed distinct WebSocket and Socket object addresses, with
  the same WebSocket constructor still in `js->new_target` at socket creation.
  JavaScript inspection confirmed Socket own keys and `WebSocket.prototype`.

## Why this regressed

Commit `12f88d69` (node module parity, #96) replaced the `module.exports`
accessor with a data property. After executing a CommonJS module, the loader
reads `module.exports`. Previously that getter call incidentally cleared
`js->new_target`. The data-property read no longer does so.

Both directions were verified using the released binaries:

- Giving unfixed v15 a `module.exports` getter makes the reproduction pass.
- Replacing v14's exports getter with a data property makes it fail.
- With the timeout scheduled before `server.listen`, v15 also fails without
  assigning any WebSocket handler. An `on*` store is not required.

The data-property change matches Node's CommonJS behavior and should remain.
The underlying constructor-context defect predates that change.

## Implementation

Invocation/frame state now owns `new.target`. The ambient `ant_t::new_target`
field is removed. Calls pass an explicit target into `sv_call_ctx_t`, then
into the existing `sv_frame_t::new_target` or JIT invocation register. Ordinary
calls supply `undefined`. Bound construction, Proxy, Reflect, super, async and
generator entry paths propagate the appropriate invocation value. JIT bailout
continuations receive both `new_target` and `super_val`.

Native callbacks take a fourth argument through `ant_params_t`, with
`ant_cfunc_t` describing the same signature. Constructors consume that explicit
argument; ordinary callbacks receive `undefined` without accessing a VM context
field. Native constructors link a small C-stack frame solely to root their
target across GC and re-entry. This frame is removed when the invocation
returns. N-API callback info captures the explicit argument, preserving its
existing public ABI. Native accept callbacks explicitly request the default
Socket prototype.

The user requested a shallow V8 checkout at `/tmp/v8`. The inspected revision
was `4d21392dd1a42370df609ed21f44524dece2534d`. V8 passes constructor metadata in
`kJavaScriptCallNewTargetRegister` (`x3` on ARM64), materializes it into an
interpreter frame register when needed, and exposes it to C++ builtins through
`BuiltinArguments::new_target()`. Ant uses the same explicit invocation-argument
pattern; no V8 code was copied.

An intermediate three-argument native ABI used a VM native-frame lookup to
read the target. Even after optimizing ordinary calls, matched no-PGO runs
showed roughly 1–3% overhead on several call/construction cases. That design
was replaced with the explicit native argument above.

The implementation review also exposed a pre-existing direct-eval defect. The
compiler only created the lexical target local for literal `new.target`
references, and initialized it after parameter defaults. Syntactic direct eval
now also requests that local, including inside arrows and defaults. `OP_EVAL`
passes the lexical target as an explicit operand through the eval entry APIs
into the eval frame. Eval-created arrows capture that frame's local; ordinary
functions retain their own target boundary. The local is initialized before
defaults and hoisted closures. This follows the lexical environment behavior
of [PerformEval](https://tc39.es/ecma262/multipage/global-object.html#sec-performeval)
without an ambient save/restore or hidden global property lookup. Ordinary
functions that use neither eval nor `new.target` acquire no extra runtime work.

## Validation and current state

- The original regression fails with `TypeError: undefined is not a function`
  at `socket.on`, followed by the buffered-message timeout.
- The existing test now explicitly asserts `net.Socket.prototype`.
- An explicit-prototype change confined to net passed the regression 10/10
  times and adjacent net/WebSocket tests, but was removed because it did not
  address constructor-context ownership.
- A provisional VM save/restore patch was removed in favor of explicit
  invocation state.
- The explicit native-argument build passes the WebSocket regression,
  `tests/test_new_target_frames.cjs`, an N-API re-entry addon probe, the full
  spec suite, and the JIT harness.
- After the eval repair, all 4,221 spec tests across 102 files and all 10 JIT
  files pass again. Constructor-context, WebSocket, dynamic eval environment,
  strict eval, eval/JIT, Function-constructor, bound-constructor, and N-API
  re-entry tests pass. The eval probes also pass in Node.
- `tests/test_eval.cjs` still fails at its sloppy `var` leakage assertion on
  line 32; the original baseline binary fails identically. This is separate
  from eval's constructor target.
- Embed and desktop callback syntax checks pass. The Wasm test build and
  `npm pack --dry-run` prepack build are blocked by the existing 32-bit
  `sv_map_template_desc_t` static assertion in
  `include/silver/engine.h`, unchanged from HEAD.
- The three review axes found no introduced correctness defect. Native ABI
  helper cleanup is optional; some private helpers now accept unused target
  parameters through `ant_params_t`.
- Matched no-PGO medians improved in 13 of 14 call/construction cases; the
  remaining `Math.imul` case measured +0.8%. With the checked-in PGO profile,
  `Math.abs` measured +2.0% and `Math.imul` +1.7%, while most other cases
  improved. The build reports discarded profile counters for changed hot
  functions. These results do not establish zero slowdown; fresh matched PGO
  training remains outstanding. Training was not started when the user moved
  the task to implementation review.
- `maid preflight` and `git diff --check` pass after the eval repair. A final
  independent correctness pass found no introduced issue in the eval change.

## Async entry wrappers and inlining

The constructor-context migration was committed in `87c85b9c`. Follow-up
cleanup removes `sv_call_async_closure_dispatch` and `sv_execute_entry_tla`:
their callers use `sv_start_async_closure` and `sv_start_tla` directly. The
implementations stay in `src/silver/ops/async.h`. The subsequent
[header boundary cleanup](../completed/silver-header-boundaries.md) separates VM definitions
in `engine.h`, type feedback in `feedback.h`, and inline call dispatch in
`call.h`. Both `src/ant.c` and the VM include the async operations directly;
`call.h` also includes them before its dispatch helpers. This removes the
temporary circular include through `engine.h`.

The user requested measurement before adding `noinline`. The comparison uses
four source-identical variants apart from the two functions' attributes:
default inline behavior, async entry forced out of line, TLA entry forced out
of line, and both forced out of line. Each is built with Clang 21.1.8, `-O3`
and LTO on an Apple M5 Pro, first without PGO and then with the configured
Darwin ARM64 profile. The latter uses the existing profile, not fresh training.

The confirmation run uses 12 balanced process-order rounds, comparing each
workload back to back across variants. Workloads are:

- `tests/bench_async_entry.cjs 1000000 5 <case>` for no-await, dead-await, and
  actual suspension, with result checks in every sample.
- `tests/bench_call_fallback.js` for ordinary JS and native-call controls.
- Importing 1,500 distinct local modules per fresh process, with either
  `await 0` or an untaken await branch in each module; the exported-value sum
  is checked. Module files are created before timing.

Builds and timing runs are serialized. All four no-PGO variants pass the
focused async fast-path, TLA, re-entry, and constructor-context tests.
Measurement artifacts are under `/tmp/ant-async-inlining` and
`/tmp/ant-async-inlining-pgo` (`results.json` contains the confirmation samples).

Configured-PGO confirmation medians follow. Percentages compare against the
default inline variant; positive means slower.

| Workload | Inline median | Async `noinline` | TLA `noinline` | Both `noinline` |
| --- | ---: | ---: | ---: | ---: |
| Async, no await | 75.106 ms | +2.50% | +0.83% | +1.66% |
| Async, untaken await | 104.226 ms | -3.85% | -0.06% | -5.28% |
| Async, suspension | 192.613 ms | +1.72% | +2.20% | +1.75% |
| JS direct calls | 8.690 ms | +2.36% | +0.17% | +4.14% |
| Native `Math.abs` calls | 36.615 ms | +0.34% | -2.80% | +3.76% |
| Native `Math.imul` calls | 45.820 ms | +1.11% | +1.18% | +2.86% |
| TLA modules, suspension | 31.320 ms | +4.41% | +4.03% | +4.74% |
| TLA modules, untaken await | 31.959 ms | +2.91% | +1.05% | -2.37% |

The no-PGO confirmation also had tradeoffs: forcing async entry out of line
improved the three async medians by 1.7–3.9%, but slowed JS direct calls by
2.4% and suspended TLA imports by 4.2%. Forcing both functions out of line
improved async medians by 3.4–6.8%, but slowed the JS direct-call control by
12.1%. Some apparent gains in the initial scouting run changed direction in
the balanced confirmation run; small timing differences should not be treated
as universal improvements.

Decision: retain `static inline` for both functions. Neither `noinline`
attribute produced a consistent benefit across builds and workloads. Both
functions were fully inlined in the default binaries. Forcing async entry out
of line also introduced multiple local function copies: binary size grew by
35,344 bytes without PGO and 2,560 bytes with the configured PGO profile.
The implementations remain unchanged in `src/silver/ops/async.h`.

Final validation with that selection passes the normal build, eight focused
async/TLA/constructor/WebSocket tests, the new async-entry benchmark's result
checks, all 4,221 spec tests, all 10 JIT files, embed syntax checking, and
`maid preflight`.
