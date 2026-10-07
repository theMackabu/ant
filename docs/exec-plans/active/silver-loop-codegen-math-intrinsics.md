# Silver Loop Codegen and Math Intrinsics

Status: active
Last reviewed: 2026-10-07
Owner: theMackabu

## Goal

Cut the per-iteration cost of compiled numeric loops and of `Math.*` calls.
On the M-series reference machine a store loop
(`for (let i = 0; i < n; i++) buf[i & 1023] = i`) retires about 73
instructions per iteration where node needs about 5, and `Math.abs(i)` costs
6.9 ns against node's 0.3 ns.

## Where the instructions go

From the MIR of that loop:

- `i` lives as a double: each step is a float add, `i & 1023` converts the
  double back to an integer with range checks, and storing `i` reboxes it.
- `i < n` re-checks `n`'s tag and converts it to a double every iteration,
  then builds a boxed boolean and tests that instead of branching on the
  comparison.
- A few register shuffles per iteration from stack rotation.

`Math.*` calls go through the generic native call path (`jit_helper_call` →
`sv_vm_call` → the builtin), about 150 instructions per call.

## Plan

1. **`Math.*` intrinsics.** `Math` loaded from the global is tagged like
   `String` (`jit_known_builtin_t`); calls to `abs`, `floor`, `ceil`, `round`,
   `trunc`, `sign`, `min`, `max`, `sqrt` and friends with number arguments
   become inline operations, guarded on the callee being the original builtin.
   Anything else takes the call. Edge cases: `-0` (`round(-0.4)`,
   `floor(-0)`, `min(0, -0)`), NaN propagation, round-half-toward-+∞,
   replaced builtins.
2. **Compare+branch fusion.** A comparison followed by a conditional jump
   branches on the comparison directly.
3. **Loop-invariant hoisting** of type checks and conversions of values the
   loop cannot change (loop bounds held in parameters or locals not written
   in the loop).
4. **Integer loop counters.** Keep induction variables as integers while a
   guard proves they stay in range; fall back to the double path on overflow.
   OSR entry and bailouts rebuild the canonical double/boxed state. Existing
   integer shadows deliberately exclude reassigned locals (see
   [Silver Throughput](silver-throughput-bench-v8-game-of-life.md), 2026-09-06),
   and branch joins must box the fallthrough stack
   ([Numeric Branch Join](silver-numeric-branch-join.md)).
5. **Shared add block.** The inline array add path (append / hole fill in
   `OP_PUT_ELEM`) is emitted once per compiled function instead of per store
   site; sites jump in with an id and return through a switch. Today every
   generic store site carries ~40 MIR instructions for it, which costs Crypto
   ~0.9% instructions through recompiles.

## Constraints

- Every stage: node-differential focused tests with mutation checks, harness,
  spec and native suites, then a no-PGO A/B on bench-v8 fixed work
  (instructions), the GC fixtures and the app suite (RSS).
- Compile time counts: Crypto recompiles ~50 functions per run, so extra MIR
  per site shows up as steady-state cost.
- Fast paths emitted ahead of a generic call must box every live stack slot
  first and handle tail calls.

## Decision log

- 2026-10-05: user approved the roadmap and its order; accepted the string
  pool free-block retention RSS cost (newt-prelude +5 MB) and the temporary
  Crypto +0.9% from the per-site add path, to be removed by stage 5.
- 2026-10-05: stage 3 left the `Math` global load alone. Global-object
  properties take the full IC path (~51 instructions: epoch, global tag
  classification, shape and slot loaded from the IC); skipping it safely needs
  a watchpoint on writes to `globalThis.Math` and its methods. Candidate
  follow-up, not part of this plan.

## Validation status

- Stage 1 (Math intrinsics): `Math` and `Math.<fn>` loads are tagged on the
  main and inliner stacks; calls with the original builtin and number
  arguments run `abs` inline and the rest through `ant_math_*` (shared with
  the builtins, which also fixed `Math.round(0.49999999999999994)` returning
  1). `Math.abs/floor/sqrt/max/round/imul` 7.9–10.5 → 4.1–5.5 ns per call;
  an inlined helper calling `Math.sqrt` 7.5 → 4.0 ns. bench-v8 neutral
  (raytrace −1.0% instructions). Test: `tests/test_jit_math_intrinsics.cjs`,
  10 mutants caught (NaN canonicalisation is not observable on arm64).
  The remaining cost is the `Math` global and `.abs` loads (~80 instructions
  per call); global-object properties take the full IC path while script
  bindings take ~8 instructions — a target for stage 3.
- Stage 2 (compare+branch fusion): single-path comparisons record their 0/1
  register; a conditional jump right after them, with no label in between,
  branches on it (`BT`, which MIR fuses with the float comparison). Empty
  loop 17 → 11 instructions per iteration, `s += i` 36 → 13. bench-v8 −1.0%
  instructions (crypto −1.7%, navier-stokes −3.2%). Test:
  `tests/test_jit_compare_branch.cjs`, 3 mutants caught.
- Stage 3 (hoisting and layout), three parts:
  - Cold blocks: `jit_sink_cold_blocks` moves every `jmp L; <block ending in
    a jump>; L:` block to the end of the function, so guards' slow paths no
    longer cost the fast path a taken jump. Inverted branches
    (`bt body; jmp exit; body:`) are left alone: that block is the loop body.
  - Read-only numeric parameters (no `PUT_ARG`, feedback says a numeric op
    consumes them) are tested and unboxed once at entry (`param_num_ok`,
    `param_num`); a `GET_ARG` feeding such an op tests the flag and bails out
    if the argument was not a number. Separate from `param_d_cache`, which
    `GET_SLOT_RAW` and call-slot paths read unconditionally.
  - `INSERT3` (`o[k] = v` as a statement) keeps a numeric `v` unboxed, so
    `PUT_ELEM` neither reboxes nor re-checks it.
  Store loop 67 → 58 instructions per iteration, empty loop 12 → 8.
  bench-v8 (no-PGO, 3 rounds) −0.8% instructions / −3.1% cycles,
  navier-stokes −14% cycles, crypto −2.5% instructions, deltablue +0.6%.
  GC fixtures and app outputs unchanged; splay-long peak RSS 1311 → 1408 MB
  (deterministic). Bisected to the parameter hoist in `SplayTree.insert`;
  pinning the GC's major time share (`gc_adapt_major_interval`) shrinks the
  gap to +25 MB, so most of it is the wall-clock growth rule reacting to
  different timing, not retention.
  Regression found in the A/B: `jit_numeric_param_use` skipped one pushed
  operand and then accepted `INC`/`DEC`, so in `x[++k]` the array `x` counted
  as numeric. `GET_ARG` bailouts change no type feedback, so the recompile was
  refused and navier-stokes ran 15× slower. Unary ops now count only right
  after the read; `tests/test_jit_loop_hoisting_codegen.cjs` asserts no
  bailouts for that shape.
  Tests: `tests/test_jit_loop_hoisting.cjs`,
  `tests/test_jit_loop_hoisting_codegen.cjs`; 13 mutants caught.
- Stage 4 (integer loop counters): the canonical double stays as it was
  (bailouts, OSR entry and joins are untouched). `jit_induction_locals`
  finds locals whose every write is a word-range integer constant, `i++` /
  `i--`, or a copy of another such local, with the store not at a branch
  target; those doubles are always integers within ±2^53 and never -0, so
  `d2i` is exact. When such a local feeds an integer consumer (bitwise op,
  element key, `%` for counters that only count up from ≥ 0), `GET_LOCAL`
  derives an int64 shadow once per block; writes and labels drop it.
  `INSERT3` keeps integer values unboxed and `PUT_ELEM` boxes them once
  without a NaN check or type guard. `s += a[i]` 49 → 41 instructions per
  iteration, `s += i % 7` 56 → 16, `s += a[i % 1000]` 95 → 44, store loop
  58 → 52. bench-v8 −0.24% instructions / −0.6% cycles (regexp −1.1%);
  GC fixtures and apps flat in instructions and RSS. A first version that
  let `PUT_ELEM` convert integers through `vstack_prepare_num` cost Crypto
  +0.75% (`am3` takes the generic store, which then NaN-canonicalised every
  value). Tests: `tests/test_jit_induction_locals.cjs`,
  `tests/test_jit_induction_locals_codegen.cjs`; 8 mutants caught. Join-store
  and non-copy-producer checks can't be reached today (the compiler never
  types such locals as numbers), kept as defence in depth.
  `test_jit_inline_truthiness_codegen.cjs` and
  `test_jit_integer_index_specialization_codegen.cjs` (not in the harness
  manifest) had assumed slow paths are laid out inline; stage 3's layout
  broke them and they now accept blocks at the end of the function.
- Stage 5 (shared add block): not landed. Measured first: removing the
  inline add path entirely saves Crypto 0.56% instructions (~186M), but
  only ~13 ms of 360 ms compile time is the per-site MIR. Disabling it in
  `am3` alone saves about the same (~194M), yet `am3`'s add path never
  executes (helper call counts are unchanged with it disabled). Its presence shifts MIR's register allocation in
  `am3`'s loop (~213 MIR instructions per iteration, at the edge of register
  pressure). Every restructuring moved that by register-allocation noise
  rather than recovering it: sharing the store and barrier tail +0.14%,
  reloading array state in the add path +0.5%, laying the add and helper
  paths out cold +0.8%, an out-of-line C helper +1.6% (and appends 9%
  slower). A shared jump-in block keeps the same live set at each site, so
  it was not built. Without the add path, appends cost 4× more (658 vs 163
  instructions per store), so it stays. Cumulative stages 1–4 against
  9cfe53f1: bench-v8 −2.2% instructions / −4.1% cycles, Crypto −4.3%
  instructions, navier-stokes −6.1% / −16% cycles.
- Follow-ups landed after stage 5 (each with node-differential tests):
  - **Integer parameter counters** (`9b7f2f72`, `48209e15`). Parameters that
    are only stepped by a constant up to ±511 are checked once at entry and
    read as int64 copies (`parg_counter_N`) at integer consumers, the
    stage 4 scheme applied to `am3`'s `i`, `j`, `n`. The entry check is a
    `D2I`/`I2D` round trip plus, since 2026-10-07, a doubling `ADDO`/`BO`
    overflow test: `D2I` saturates, so 2^63 used to pass as `INT64_MAX`
    (`h(2**63)` returned 196605 instead of 0). The bound narrows counters to
    ±2^62 and costs 2 instructions per counter parameter at entry (am3 0%,
    a 2-parameter call microbenchmark +1.3%). A failed check calls
    `jit_helper_disable_param_counters`, which sets a sticky per-parameter
    bit (`jit_param_counters_off`) and bumps `tfb_version` so the recompile
    is allowed. Tests: `tests/test_jit_param_counters.cjs`,
    `tests/test_jit_param_counters_codegen.cjs`.
  - **`x | 0` and arithmetic on non-number primitives** (`b7565877`).
    Compiled bitwise and arithmetic ops on `undefined`, `null`, booleans,
    strings and BigInt are computed in the helper instead of bailing out on
    every call; ToNumeric errors (throwing `valueOf`, Symbols) now propagate
    in both tiers. Tests: `tests/test_jit_bitwise_primitives.cjs`,
    `tests/test_numeric_coercion_errors.cjs`.
  - **Global-read snapshot** (`2a9392d2`). Compiled global reads compare the
    global object's shape and `global_lexical_count` against compile-time
    values and load the slot directly: `s += K` 61 -> 37 instructions per
    iteration, `Math.abs(i - 5)` 118 -> 96. Also fixed compiled reads of a
    deleted global returning `undefined`. Test:
    `tests/test_jit_global_snapshot.cjs`.
  - **Prototype method snapshot** (`a291bd22`). Same idea for a method one
    level up the prototype chain (receiver shape and proto, prototype shape
    and non-exotic flag). `ee.emit('foo')` 246 -> 212 instructions per call.
    A check on the prototype's `ic_identity` was rejected: identities restart
    after 2^32 and could alias. Test: `tests/test_jit_proto_method_snapshot.cjs`.
  - **Stale snapshot recovery** (2026-10-07). A prototype or global snapshot
    whose holder changes shape can never match again, so holder-side misses
    jump to a cold block that, once per site, drops the owner's compiled code
    (`mir_emit_drop_owner_code_once`; the owner is `jit_compile_owner`, so
    inlined sites drop the outer function). It also clears
    `jit_compiled_tfb_ver`, without which the compiler refuses the recompile.
    Receiver-side misses stay on the generic path. After three resets
    (`JIT_SNAPSHOT_RESET_LIMIT`) a function stops snapshotting. Method call
    after a prototype gains a method 294 -> 224 instructions per iteration
    (hit path 221 -> 222), global read after a new global 71 -> 37. A
    change in the middle of one long compiled call still finishes that call
    on the generic path. Test: `tests/test_jit_snapshot_recovery.cjs`.
  - **Call-site gating of inline builtin paths** (2026-10-07). The direct
    builtin-call block and the `push`/`toString` known-builtin paths are
    emitted only when feedback says the site can reach the builtin; see
    [EventEmitter Node Parity](eventemitter-node-parity.md) for the call
    block. For `push`/`toString`, `jit_field_ic_may_load` reads the site's
    method-load IC: a function with 40 user `queue.push(...)` sites compiles
    in 21.0 ms instead of 25.9 ms.
- Pre-existing bug fixed alongside: a for-loop update `i++` / `i--` compiles to
  `INC_LOCAL` / `DEC_LOCAL`, which added 1 to the raw bits without ToNumeric
  (`for (let i = '1'; i <= 4; i++)` ran once; BigInt counters became NaN;
  `valueOf` was never called). The interpreter now takes the postfix
  operators' ToNumeric path for non-numbers; compiled code guards locals not
  known to be numbers and bails out. Test:
  `tests/test_for_update_tonumeric.cjs` (fails on the released binary).
