# Silver JIT Inline Arena Allocation

Status: active
Last reviewed: 2026-08-14
Owner: theMackabu

## Goal

Determine whether Ant's Silver JIT can materially reduce closure-allocation
cost by emitting the common fixed-arena allocation path directly in generated
code, while retaining the existing C helpers as complete slow paths.

The desired steady state is the same broad shape observed in V8: a few arena
loads, a capacity check, a bump or free-list pop, direct field initialization,
and no allocation-helper call. This plan is deliberately a staged proof, not a
commitment to inline every allocation class or redesign closures.

Success means all of the following:

- the targeted generated path is behaviorally identical to
  `jit_helper_closure`;
- GC pressure, arena, roster, and OOM behavior remain correct;
- the closure-allocation microbenchmark improves materially;
- a Newt-relevant captured-closure path improves Newt in repeatable pinned
  A/B runs;
- generated-code size, JIT compile time, and memory stay within the gates
  below.

## Why This Plan Exists

Newt creates roughly 225 million closures. Prior profiling attributed material
self time to `jit_helper_closure`, with substantially more time below the
closure subtree in allocation and minor GC. Classic escape elimination is a
poor fit because the important monad closures escape into `MkM` records and
continuations.

The corresponding V8 TurboFan graph for
`Lib_Types_Prelude_Monad$20Lib_Types_M$2Cbind` does not eliminate the escaping
closure. It retains a function context, closure, and `MkM` object, but lowers
their combined 144 bytes to direct young-generation allocation and field
stores. A serial Node Newt comparison found:

| Configuration | Main time | Interpretation |
| --- | ---: | --- |
| default | about 3.15s | baseline |
| `--no-turbo-escape` | about 3.20s | escape analysis was not decisive |
| `--no-turbo-allocation-folding` | about 3.19s | combining reservations was not decisive |
| `--no-inline-new` | about 22.91s | inline allocation was load-bearing |

This changes the optimization target. More inline-body semantics do not help
if generated code still calls `jit_helper_closure` for every materialized
closure.

## Scope

Primary implementation surfaces:

- `src/silver/swarm.c`: MIR fast-path emission and `OP_CLOSURE` selection.
- `src/silver/glue.c`: unchanged complete slow path, plus only narrowly scoped
  fallback entrypoints if a safe transactional boundary requires them.
- `include/arena.h`: fixed-arena invariants; avoid changing its normal C fast
  path unless sharing a constant or assertion prevents drift.
- `include/silver/engine.h`: closure and upvalue initialization invariants.
- `include/internal.h`: isolate roster and GC-pressure fields addressed by
  generated code.
- `src/gc/objects.c` and `src/gc/gc.c`: audit targets for young-roster and
  collection behavior, not initial redesign targets.
- focused tests and benchmarks under `tests/`.

## Non-Goals

- Do not revive the rejected virtual-object/virtual-closure inline-body patch.
- Do not add retained environment-variable counters or production statistics.
- Do not redesign `sv_closure_t`, upvalue semantics, or activation storage in
  the first proof.
- Do not combine the closure, upvalue, and object arenas into one nursery.
- Do not re-land P4's shaped-object helper specialization. P4 removed shape
  setup around `mkobj`; it did not emit allocator operations in MIR, and it was
  correctly rejected as flat.
- Do not widen the bytecode format or add allocation opcodes.
- Do not trade helper calls for one helper call per captured upvalue.

## Correctness Invariants

The generated fast path must preserve the exact observable state transitions
of `js_closure_alloc_hot`, `sv_closure_init`, the capture loop, and
`sv_closure_finish_init`.

1. **Collection happens before allocation.** Check closure pressure and take
   the complete helper slow path before mutating an arena, roster, open-upvalue
   list, or half-built closure. No generated path may call `gc_pressure` after
   obtaining an unrooted slot.
2. **Fallback is all-or-nothing.** Every condition that can require allocation,
   roster growth, arena commit, module-context lookup, eval-environment
   materialization, or another fallible helper must be resolved before the
   first fast-path mutation. A slow edge always calls the original complete
   helper from the original state.
3. **Arena behavior is identical.** A free-list allocation pops the first slot;
   a watermark allocation requires `watermark + elem_size <= committed`;
   either path increments `live_count` exactly once. Reserved-but-uncommitted
   memory remains a slow-path concern.
4. **Closure accounting is identical.** `gc_closure_alloc` increments exactly
   once. The closure is appended to `young_closures` exactly once, and the fast
   path is used only when roster capacity already exists.
5. **Every observable field is initialized.** Because the closure arena's hot
   allocator is uninitialized, generated code must write every field read by
   GC, sweeping, calls, lazy function-object materialization, and finalization.
   Free-list garbage must never survive in `call_flags`, `upvalues`, or the
   pending/bound union.
6. **Upvalue identity is preserved.** Two closures capturing the same live slot
   must reuse the same open `sv_upvalue_t`. The ordering of the open-upvalue
   list and all capture/write barriers must remain unchanged.
7. **GC sees only complete objects.** A closure and any new upvalue cells must
   be initialized before they become reachable through the young rosters,
   open-upvalue list, VM stack, or returned nanboxed value.
8. **Module and eval state is exact.** The fast path may copy a proven current
   `module_ctx`; otherwise it falls back. A parent carrying
   `SV_CALL_HAS_EVAL_ENV` is initially ineligible because the existing finish
   path may materialize a function object and attach the environment.
9. **The C path remains authoritative.** Interpreter execution, cold JIT
   paths, unsupported closure shapes, pressure, OOM, roster growth, and arena
   commit continue through `jit_helper_closure`.

## Task List

### 1. Pin the baseline and measure the target distribution

- Build the current PGO tree with
  `./meson/pgo/build.sh --force-no-nix` and copy the resulting binary to an
  immutable `/tmp` baseline before editing.
- Record the exact source hash, binary hash, profile hash, host state, Newt
  command, Main time, wall time, and max RSS.
- Reconfirm `jit_helper_closure` self time and its allocation/capture regions
  with a symbolicated profile of that exact binary.
- Measure the dynamic closure mix by child kind and upvalue count. Temporary
  local instrumentation is allowed for this census, but it must be removed
  before the candidate is built. At minimum distinguish:
  - no upvalues;
  - inherited-only upvalues;
  - local captures;
  - mixed captures;
  - `upvalue_count <= SV_CLOSURE_INLINE_UPVALS` versus heap arrays;
  - parent closures carrying an eval environment.
- Add or identify two timing fixtures:
  - a fixed no-capture closure allocation/call loop for the mechanism proof;
  - a small captured-closure loop matching Newt's create-store-call lifetime.
  Timing assertions do not belong in correctness tests.

**Gate:** do not design the Newt path from static intuition. Record which
closure forms account for the dynamic volume first.

### 2. Add one reusable MIR fixed-arena allocation emitter

Add a private emitter in `swarm.c` with explicit inputs for the arena address,
element size, result register, fast continuation, and slow label. It should
emit only the two existing allocation cases:

1. pop `free_list`, update the head, increment `live_count`;
2. otherwise check the committed watermark range, bump `watermark`, and
   increment `live_count`.

Use `offsetof(ant_fixed_arena_t, ...)` and `sizeof(...)`; do not duplicate
numeric layout offsets. The emitter must not commit pages, grow arrays, call
GC, initialize a type-specific payload, or own fallback policy.

Before using it for closures, inspect the generated MIR and native assembly
for both branches. Add compile-time assertions only where the generated code
depends on a representation property not already expressed by `offsetof` or
`sizeof`.

### 3. Prove the mechanism on simple closures

Initially select only exact JIT-known child functions with:

- zero upvalues;
- no parent eval environment;
- a module context that can be copied directly without invoking user code;
- no heap side allocation;
- an already-capacious young-closure roster.

At the `OP_CLOSURE` site, emit preflight guards before mutations, allocate the
closure slot with the shared arena emitter, initialize the complete
`sv_closure_t`, append it to the young roster, and produce the `T_FUNC` value.
Any failed guard branches to the existing 14-argument
`jit_helper_closure` call.

Keep a single join that produces the same virtual-stack value and metadata for
both paths. Do not alter interpreter `OP_CLOSURE`.

Add focused correctness coverage for:

- ordinary and arrow closures;
- lazy names and function-object materialization;
- module closures;
- repeated allocation across minor and major GC;
- free-list reuse after collection;
- roster-full and pressure-trigger fallback;
- closures retained across collection;
- exceptions and JIT bailout near closure creation.

**Mechanism gate:** keep this phase only if the no-capture allocation micro
improves by at least 8% in both A/B and B/A ordering, the native fast path has
no allocation call, generated code grows by no more than 5% for the fixture,
and unrelated JIT/call micros do not regress beyond noise. Otherwise revert
the emitter and stop.

### 4. Add inherited-only closures

For child functions whose upvalues are all inherited from the parent and whose
count fits `inline_upvals`, unroll the exact descriptor copies into generated
stores. Set `upvalues` to the new closure's inline storage and initialize only
after all preflight guards pass.

Do not handle heap-backed upvalue arrays in this phase. They require a side
allocation and therefore violate the all-fast-or-original-helper boundary.

**Gate:** retain only if the measured dynamic census says this shape is common
or it adds negligible code while improving the captured-closure fixture.

### 5. Design the local-capture transaction before coding it

Newt's important closures capture locals, so the project is not complete after
the simple and inherited forms. However, local capture must not be implemented
by calling a capture helper once per upvalue or by allocating a closure before
discovering that a later upvalue requires a slow path.

Write down and review an all-or-nothing algorithm that:

- finds reusable open cells while preserving descending slot order;
- determines the exact number of missing cells;
- proves young-upvalue roster capacity and arena capacity for every missing
  cell before mutation;
- reserves or identifies every closure/upvalue slot without exposing it;
- initializes new cells, links them into the open list, initializes the
  closure, and publishes roster entries in an order safe for conservative and
  precise marking;
- abandons the entire fast path before mutation if any proof fails.

Two acceptable outcomes are:

1. a bounded MIR implementation for the common small-capture form; or
2. a documented finding that Ant's linked-cell representation prevents a
   worthwhile direct path, in which case a compact captured-environment design
   becomes a separate plan.

Do not add this phase's code until its control-flow and failure-state table are
written into this plan.

### 6. Implement the Newt-relevant captured form, if phase 5 is approved

- Cap the first version to the smallest measured useful capture count, likely
  at or below `SV_CLOSURE_INLINE_UPVALS`.
- Unroll compile-time-known descriptors; do not add a generic generated loop
  merely to cover rare large closures.
- Keep the complete helper fallback at the original `OP_CLOSURE` boundary.
- Inspect native code to confirm the hot path contains arena/list operations
  and stores, not hidden closure-allocation calls.
- Re-profile Newt and verify time actually leaves `jit_helper_closure`, capture,
  allocator, and minor-GC regions instead of moving into emitted bookkeeping.

**Landing gate:** require at least a repeatable 3% Newt Main improvement in
both A/B and B/A order on the PGO binaries. Max RSS, GC counts, JIT compilation
time, and generated-code size must each stay within 3% unless a larger movement
has a separately explained benefit. If the captured form misses this gate,
revert phases 4-6; retain phase 3 only if its independent mechanism gate and
non-Newt usefulness justify the maintenance cost.

### 7. Consider other allocation classes only after closure landing

If direct closure allocation ships and profiles still show meaningful helper
cost, audit whether the shared emitter can help `ant_object_t` or upvalue-only
paths. This is not automatic:

- P4 already proved that removing shaped-object setup around a helper was
  flat; a new object experiment must specifically remove generated-to-C
  allocation transitions and account for property storage allocation.
- Upvalues have open-cell identity and ordering requirements; a standalone
  arena bump is not sufficient.
- Each new allocation class needs its own pressure, initialization, rooting,
  and rollback audit.

## Benchmark Protocol

Use exact pinned binaries. Do not compare against a moving `./build/ant` or
attribute results from different PGO profiles.

1. Pin the current PGO baseline from `./build/ant` before edits.
2. Build candidate PGO with `./meson/pgo/build.sh --force-no-nix` and pin it.
3. Record `shasum -a 256` for source commit, binaries, and profile data.
4. Run the focused allocation fixtures serially in interleaved AB/BA order.
5. Run Newt serially in ABBA/BAAB order with identical environment and capture
   Main time, wall time, and max RSS.
6. Run `tests/bench.js` only for the affected closure/call/JIT rows unless the
   harness cannot select rows; do not treat broad unrelated noise as evidence.
7. Re-sample the candidate Newt run and compare symbolized self and subtree
   time against the baseline.
8. Report medians and every raw run. Do not claim movements inside host noise.

## Validation

After each implementation phase:

- build with `maid build` for fast correctness iteration;
- run the new focused closure/GC tests;
- run existing JIT, upvalue, async/generator, module, bind, and GC-stress tests
  selected by `maid validate_changes`;
- inspect emitted MIR/native code for fast and slow cases;
- run `git diff --check`.

Before landing a retained candidate:

- build the final PGO binary with
  `./meson/pgo/build.sh --force-no-nix`;
- run `./build/ant examples/spec/run.js --all`;
- run the full JIT suite and repository harness recommended by
  `maid validate_changes`;
- run the focused allocation/GC stress tests repeatedly;
- run `maid preflight` and every additional command it recommends;
- record exact hashes and results here.

## Rollback Strategy

Keep each phase independently revertible:

1. profiling fixtures and temporary census instrumentation;
2. private MIR arena emitter plus zero-upvalue use;
3. inherited-upvalue stores;
4. local-capture transaction.

The original `jit_helper_closure` must remain intact throughout, so disabling
or reverting a generated fast path restores the old behavior without a data
migration or bytecode change.

## Decision Log

- 2026-08-14: V8 TurboFan inspection showed that the representative escaping
  monad closure is allocated, not scalar-replaced.
- 2026-08-14: Node flag A/B isolated inline allocation as load-bearing;
  disabling escape analysis or allocation folding was near-flat, while
  disabling inline allocation made Newt about 7.3 times slower.
- 2026-08-14: The rejected P4 shaped-object helper is not evidence against
  direct MIR arena allocation; it left the core allocator/helper boundary in
  place. It is evidence against repeating shape-only specialization.
- 2026-08-14: The first implementation must be a bounded mechanism proof with
  hard performance and code-size gates. Local captures require a reviewed
  transaction design because Ant uses separately allocated shared upvalue
  cells rather than V8-style compact contexts.

## Validation Status

- V8 optimized IR for the representative Newt bind function inspected.
- Node Newt flag comparison completed.
- Current Ant allocation, roster, capture, and sweep invariants audited at a
  source level.
- No Ant implementation or benchmark candidate exists yet.

## Follow-Ups

- If local captured cells prevent a useful direct path, create a separate plan
  for compact per-activation environments rather than expanding this plan into
  a representation rewrite.
- If direct allocation lands, consider a common representation of emitted
  arena offsets/assertions so future arena layout changes cannot silently
  invalidate generated code.
- Revisit direct object allocation only with a profile proving that the C
  transition and arena work, rather than property allocation, remain material.
