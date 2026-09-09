# Silver Throughput: bench-v8 and Game of Life

Status: active  
Last reviewed: 2026-08-15  
Owner: theMackabu  
Owner areas: `src/silver/`, `src/modules/collections.c`, strings/ropes, GC

## Goal

Pursue two explicit performance targets without weakening JavaScript semantics
or trading away Ant's existing server, regex, Newt, memory, and correctness
wins:

1. Raise the bench-v8 geometric-mean score from roughly 3,774 to at least
   7,000, with 8,000 as the stretch target.
2. Cut both measured phases of `game-of-life/dist/play.js` approximately in
   half. From the motivating line at tick 477:

   ```text
   #477 - World Tick (L: 1.585; A: 1.739) - Rendering (L: 0.760; A: 0.936)
   ```

   the directional targets are:

   | Phase | Lowest target | Cumulative-average target |
   |---|---:|---:|
   | World tick | at most 0.80ms | at most 0.87ms |
   | Rendering | at most 0.38ms | at most 0.47ms |

The primary engineering goal is broad Silver execution throughput. Do not add
source-, function-name-, benchmark-, or tick-specific shortcuts.

## Why This Needs a Separate Plan

The two workloads overlap in hot engine mechanisms but stress different mixes:

- bench-v8 is a geometric mean across eight programs and requires broad gains
  in loops, calls, arithmetic, properties, arrays, allocation, and code quality;
- Game of Life is a compact application workload dominated by repeated object
  iteration, nested method calls, property access, Map operations, short string
  construction, and rendering accumulation;
- either workload can improve while the other regresses, so both must be
  measured after every retained engine change.

This plan is independent of the completed Fable performance landing plan. Its
measurements, counters, decisions, and falsified theories belong here.

## Recorded Starting Point

`examples/bench-v8/score.json` contained the following result when this plan was
created. It is orientation data, not yet the pinned baseline required for an
accepted wall-time claim:

| Benchmark | Score |
|---|---:|
| Richards | 3,262 |
| DeltaBlue | 5,395 |
| Crypto | 1,657 |
| RayTrace | 3,552 |
| EarleyBoyer | 7,692 |
| RegExp | 2,939 |
| Splay | 5,501 |
| NavierStokes | 3,196 |
| **Geometric mean** | **3,774** |

Moving 3,774 to 7,000 is a 1.85x suite-wide gain; reaching 8,000 is a 2.12x
gain. This cannot plausibly come from one isolated micro-optimization. If only
five of eight tests improved, those five would need roughly 2.7x each for 7,000
or 3.3x each for 8,000 while the other three stayed flat. The plan therefore
prioritizes shared hot mechanisms and code-generation quality over chasing the
score formula.

The Game of Life grid contains 6,000 `Cell` objects. Each world tick:

- walks `Map.values()` twice;
- calls `alive_neighbours()` once per cell;
- walks each cell's neighbor array and reads `neighbour.alive`;
- writes `next_state`, then copies it to `alive`;
- increments the world tick.

Each render:

- visits all 6,000 coordinates;
- constructs a fresh `` `${x}-${y}` `` string for every lookup;
- calls `Map.get()` with that string;
- calls `cell.to_char()`;
- appends one character per cell plus one newline per row.

Ant already has specialized array, Map, Set, and string iteration states, but
the JIT-facing iteration path still crosses shared helpers. Ant's Map string-key
lookup currently materializes a flat byte representation of the input string
before `uthash` lookup. These are candidates for measurement, not established
causes.

## Constraints

- Counters precede code changes. No optimization is accepted from source
  inspection or a short profile alone.
- Only pinned, interleaved A/B runs count as wall-time evidence.
- Verify binary identity and PGO identity. Installed release, local non-PGO,
  and local PGO binaries are different baselines.
- Run each bench-v8 test directly under the selected pinned binary. Do not let a
  parent runner accidentally spawn one fixed repository binary for both arms.
- Preserve exact benchmark results and a deterministic Game of Life state hash.
- Do not change benchmark source to improve the headline result. Reduced or
  deterministic copies are allowed only as diagnostic micros.
- Preserve Proxy, accessor, prototype mutation, iterator closing, Map/Set
  insertion order, SameValueZero, GC rooting, string immutability, and exception
  semantics.
- A compile-time reduction is not a win if generated-code execution becomes
  slower. Attribute parse, compile, execution, GC, and teardown separately.
- Do not add another JIT backend until counters show current MIR compilation is
  a material fraction of these workloads and a standalone prototype clears its
  payback gate.

## Measurement Protocol

### Binary identity

For every candidate:

```sh
maid build
cp build/ant /tmp/ant_<name>_bin
md5 /tmp/ant_<name>_bin
```

Record commit, dirty paths, build type, PGO profile identity, and binary MD5.
Pin the base before modifying code. Use serial AB/BA ordering with at least two
rounds; add rounds when variance can change the conclusion.

### bench-v8

For each of the eight tests, concatenate `tests/base.js`, the individual test,
and `harness.js` once into a temporary input. Run that same fixed input directly
under each pinned binary. Record:

- correctness result;
- raw score and formatted score;
- wall, user, and system time;
- JIT compile and execution counters;
- GC counts and pause time when relevant.

Compute the geometric mean only after retaining the per-test values. Never use
only the aggregate score to attribute a mechanism.

### Game of Life

Add a deterministic fixed-work benchmark before optimizing:

- fixed 150 by 40 grid;
- seeded initial live/dead state shared by both binaries;
- explicit warmup period;
- separate batches for `dotick()` and `render()`;
- enough ticks to include normal minor-GC behavior;
- final live-cell/state checksum and render checksum;
- no console output inside the timed region.

The fixed-work harness is the primary A/B. Also confirm the original
`game-of-life/dist/play.js` process at tick 477 or a documented equivalent
steady-state window. Its cumulative average includes startup and early ticks,
while its lowest value is an extreme sample; neither alone is sufficient for
attribution.

## Phase 0: Establish Reproducible Baselines

- Pin the current candidate and installed/reference binary.
- Run all eight bench-v8 tests in interleaved order.
- Add and run the deterministic Game of Life harness.
- Record the original tick-477 line for practical confirmation.
- Capture PGO identity and repeat the baseline with a freshly trained local PGO
  binary if the checked-in profile does not represent the current source.
- Store raw results in this plan or an adjacent checked-in data file.

Exit criteria:

- repeated scores have stable enough variance to distinguish a 3% change;
- both binaries produce identical per-test results and Game of Life checksums;
- the runner is proven to execute the intended pinned binary;
- no generated `score.json` change is mistaken for an engine change.

## Phase 1: Whole-Run Attribution Counters

Add a single opt-in statistics surface, or extend an existing one, to report the
following without per-operation printing.

### Execution and JIT

- bytecodes executed in the interpreter, by opcode;
- functions considered, rejected, compiled at opt1, and promoted to opt3;
- compile time and generated bytes per function;
- compiled entries, OSR entries, retry-to-interpreter exits, and invalidations;
- JIT helper calls by helper;
- direct, devirtualized, inline, fused, and generic call counts;
- inline attempts and rejection reasons;
- property/element IC hits, misses, refills, megamorphic sites, and fallback
  reasons;
- iterator initialization and advance counts by array/Map/Set/string/generic
  path and by interpreter/JIT execution.

### Collections, strings, and GC

- Map lookup counts by key type;
- string-key lookup bytes flattened/copied, heap temporary allocations, hash
  probes, hits, and misses;
- short-string and template-concatenation allocations;
- builder appends, snapshots, flushes, flatten bytes, and retained rope nodes;
- object, string, rope, and iterator allocations;
- minor/major collections, trigger source, phase time, promoted bytes, and
  reclaimed bytes.

Counters must identify hot bytecode/function sites in bounded summaries. A
single total cannot distinguish a cheap operation executed millions of times
from a pathological slow site.

Exit criteria:

- at least 90% of each fixed-work run is attributable to named execution,
  compile, GC, or host buckets;
- the top three mechanisms for every low bench-v8 test and both Game of Life
  phases are supported by counts;
- sampling is used only to split a counter-backed bucket, not as wall-time
  evidence.

## Phase 2: Halve Game of Life World-Tick Time

Evaluate the following in counter order, one change at a time.

### 2A. Map-values iteration

- Determine whether `for (const cell of this.#cells.values())` stays in JIT code
  or calls `jit_helper_for_of` / iterator-advance helpers per element.
- Measure iterator object/state allocation once per loop and native helper
  traffic per element.
- If helper traffic is material, emit a guarded native Map-values loop state in
  JIT code while retaining the generic iterator path for modified prototypes or
  methods.
- Preserve mutation-during-iteration and iterator-closing semantics.

### 2B. Neighbor-array loop

- Confirm `Cell.alive_neighbours()` is JIT compiled and measure array-iteration
  helper calls, dense-array guards, bounds checks, and property-read IC hits.
- Hoist only guards proven loop-invariant by existing feedback and invalidation
  machinery.
- Prefer direct dense iteration and typed numeric accumulation when the array
  remains pristine.
- Do not turn mutation or hole behavior into undefined behavior.

### 2C. Method calls and property traffic

- Attribute the calls to `alive_neighbours()` and `to_char()` across direct,
  devirtualized, inline, and generic paths.
- Measure `alive`, `next_state`, `neighbours`, `x`, and `y` field sites.
- Improve devirtualization or small-leaf inlining only when target and shape
  guards remain valid across prototype changes.
- Avoid duplicating the main-emitter call machinery in a second ad hoc path.

### 2D. Allocation and GC

- Count allocations during `dotick()` separately from rendering.
- The steady-state tick should allocate nearly nothing beyond timing/logging
  outside the fixed-work region; find any iterator or boxing allocations that
  remain inside it.
- Remove proven avoidable allocations before tuning nursery thresholds.

Phase success:

- deterministic `dotick()` fixed-work time improves by at least 40% before
  accepting substantial machinery;
- final confirmation reaches at most 0.80ms lowest and 0.87ms cumulative
  average near tick 477, or the plan records why that display statistic differs
  from the fixed-work result;
- bench-v8, servers, and Newt do not regress beyond measured noise.

## Phase 3: Halve Game of Life Rendering Time

### 3A. Coordinate-key construction

- Count number-to-string conversions, short concatenations, rope/builder nodes,
  materializations, bytes copied, and hash calculations for `` `${x}-${y}` ``.
- Determine whether the compiler emits the optimized local builder path for the
  template literal or constructs generic intermediate strings.
- Test a JIT small-ASCII template/append path only if counters show conversion
  and construction dominate.
- Preserve snapshots and Unicode behavior; specialize only under explicit
  ASCII/integer guards.

### 3B. Map string-key lookup

- Measure the cost of `collection_key_init()`, `js_getstr()`, temporary byte
  copying, `uthash`, and equality separately.
- Consider cached string hash and direct `(bytes, length, hash)` lookup so a
  flat immutable string does not need to be copied into `collection_key_t`.
- Keep owned key bytes or another stable representation for stored entries;
  never retain an unrooted or moving temporary string pointer.
- Verify string, rope, embedded-NUL, BigInt, object-identity, `-0`, `NaN`, and
  SameValueZero keys.

### 3C. Result accumulation

- Confirm the rendering local remains a builder through the complete nested
  loop and materializes once on return.
- Count fast ASCII appends, snapshots, flushes, and flattened bytes.
- Inline the existing builder append fast path only if helper-call counts remain
  material after key lookup improvements.
- Do not optimize away construction merely because the minimal player does not
  print `rendered`; the value is observable and the fixed-work harness hashes
  it.

### 3D. Tiny method body

- Check whether `cell.to_char()` becomes an inlined guarded field read and
  select. If not, attribute whether the limit is inlining policy, private-field
  access, call dispatch, or control-flow code generation.
- Retain a generic call fallback after all guards.

Phase success:

- deterministic rendering fixed-work time improves by at least 40%; and
- confirmation reaches at most 0.38ms lowest and 0.47ms cumulative average near
  tick 477 without omitting rendering work.

## Phase 4: Raise bench-v8 Throughput Broadly

Use Phase 1 counters to rank mechanisms separately for every test. Start with
the lowest scores because improvements there have the most room, but retain a
change only when its mechanism is real and its suite-wide effect is understood.

### 4A. JIT coverage and bailout elimination

- Identify hot functions or loops still interpreted, repeatedly recompiled, or
  returning through retry paths.
- Repair eligibility and OSR only for counter-proven hot sites.
- Treat early type-feedback mismatch as a retry when semantics allow; do not
  permanently invalidate otherwise useful code.
- Record time executing before compilation so a higher final native fraction is
  not confused with faster startup.

### 4B. Call code quality

- Rank generic helper calls, direct JIT-to-JIT calls, method calls, closures,
  native calls, tail calls, and inlining misses.
- Extend known-target dispatch or safe inlining only for dominant sites.
- Preserve evaluation order and never re-execute already-emitted effects on an
  inline fallback.
- Compare Richards and DeltaBlue after every call-path change; they are the
  quickest cross-check that one dispatch form did not improve at another's
  expense.

### 4C. Numeric loops and representation

- For Crypto and NavierStokes, count arithmetic helper calls, integer/double
  conversions, boxing, overflow fallbacks, array accesses, and loop guards.
- Keep numeric values unboxed in generated code across basic blocks where type
  feedback and merge semantics permit it.
- Prefer better MIR value/range information and fewer redundant conversions to
  new benchmark-specific opcodes.
- Inspect generated MIR and machine code for the top loops before changing the
  compiler architecture.

### 4D. Objects, properties, and arrays

- For RayTrace, Richards, DeltaBlue, Splay, and EarleyBoyer, rank field/element
  IC misses, shape guards, prototype guards, dense-array exits, and allocation.
- Improve polymorphic sites only when counters show stable repeated shapes.
- Hoist or combine guards only through shared invalidation mechanisms.
- Separate object construction cost from steady-state field access.

### 4E. RegExp

- Keep RegExp work behind `ANT_REGEX_STATS` or equivalent whole-run counters.
- Separate compile/cache, JIT versus interpreter matching, result allocation,
  global match/replace batching, lastIndex, and UTF byte/UTF-16 conversion.
- Do not regress the dedicated regex micros or GC async/coroutine tests to raise
  the bench-v8 RegExp score.

### 4F. MIR quality and compilation cost

- Capture the largest and hottest generated MIR functions.
- Attribute MIR time by phase and generated execution by function.
- Improve a pathological MIR pass in the MIR repository with standalone pinned
  evidence before updating Ant's dependency.
- Consider a new baseline or SSA tier only if current compile time is a top
  whole-run bucket after local code-quality work.

Bench-v8 milestones:

- **4,500:** counters and one or more broad mechanisms validated;
- **5,500:** low-score tests improve without relying on one exceptional test;
- **7,000:** primary target, full suite and application gates required;
- **8,000:** stretch target, accepted only with reproducible PGO and no semantic
  shortcuts.

No milestone is complete if one test regresses by more than 5% without a
documented correctness trade or if the geometric mean comes from a changed
workload/result.

## Phase 5: PGO and Tiering

Only after source-level mechanisms are stable:

- regenerate PGO with a corpus that includes the current engine paths;
- compare no-PGO, existing-profile, and fresh-profile binaries separately;
- report how much of the gain is source and how much is profile placement;
- add Game of Life or bench-v8 to PGO training only if it improves independent
  workloads and does not overfit the release binary;
- revisit the SLJIT/IR tiering note only if compile/payback counters justify it.

The 7–8k goal should not be declared achieved by comparing a fresh PGO
candidate against a stale or non-PGO base.

## Gates After Every Retained Engine Change

Focused gates:

- deterministic Game of Life checksum and phase benchmark;
- all eight bench-v8 correctness results and per-test timings;
- focused tests for the modified call, iteration, collection, string, GC, or JIT
  behavior;
- `todo/tests/devirt_fuzz.cjs` after call dispatch or inlining changes.

Repository gates before landing:

- `maid preflight`
- `maid validate_changes`
- `maid structure`
- `maid knowledge`
- `./build/ant examples/spec/run.js --all`
- `./build/ant examples/jit/run.js`
- `./build/ant tests/harness/run.js`
- Newt Main wall time and RSS
- server RPS floors
- `git diff --check`

For GC-touching changes, temporarily restore the documented GC-stress tick,
run the focused and broad battery under stress, then remove the hook before
landing.

## Stop Conditions

- Stop a theory after two clean pinned rounds show less than a 2% effect unless
  it removes substantial memory or correctness debt.
- Revert an optimization that improves only one benchmark through changed
  observable behavior.
- Do not tune GC thresholds until allocation, survival, and phase counters show
  the mechanism.
- Do not retain a hot-path cache that lacks explicit lifetime, invalidation, and
  isolate ownership.
- Do not add duplicated hardcoded opcode/verifier tables to work around JIT
  admission or OSR.
- Do not introduce background compilation without immutable inputs, safe
  publication, cancellation, exception exits, and teardown ownership.
- Do not pursue a backend replacement based on synthetic compile speed alone.
- If halving Game of Life requires changing its data structure or skipping
  rendering, record that as an application experiment, not an Ant engine win.

## Evidence Table

Update this table only with pinned interleaved A/B results.

| Change | Binary IDs | bench-v8 total | Tick fixed-work | Render fixed-work | Tick 477 | Gates | Decision |
|---|---|---:|---:|---:|---|---|---|
| Initial baseline | pending | 3,774 recorded, remeasure pending | pending | pending | 1.585/1.739ms; 0.760/0.936ms | pending | baseline |

Keep per-test bench-v8 rows and raw round order below each evidence entry or in
an adjacent data file. The aggregate alone is insufficient.

## Decision Log

- 2026-08-15: Create a new plan scoped only to bench-v8 and Game of Life. Do
  not reopen or extend the completed Fable landing plan.
- 2026-08-15: Treat 7,000 as the primary bench-v8 target and 8,000 as a stretch
  target. The required 1.85–2.12x geometric-mean gain demands broad JIT/codegen
  improvements rather than a single backend swap.
- 2026-08-15: Split Game of Life into deterministic `dotick()` and `render()`
  fixed-work measurements, then confirm the original tick-477 display.
- 2026-08-15: Instrument iteration, calls, ICs, Map string keys, builders, JIT
  coverage, and GC before choosing an implementation.
- 2026-08-18: Replace the 32-byte heap scratch buffer in numeric
  `js_tostring_val()` with stack storage as a Phase 3A checkpoint. The change
  leaves pooled-string allocation unchanged at 770,288 bytes per 6,000-key
  probe, but eight serial interleaved diagnostic rounds measured
  `${x}-${y}` plus `Map.get()` at 109.83ns/cell before and 94.92ns/cell after
  (-13.6%). Six serial interleaved deterministic `World.render()` rounds
  measured 853.90us/render before and 757.37us/render after (-11.3%). These
  are directional, not Evidence Table results: the runs were not CPU-pinned,
  and LLVM discarded the stale PGO count for the changed function.
- 2026-08-18: A follow-up string-allocation audit found that several property
  APIs and `Array.prototype.toLocaleString()` treated `tostr()`'s required
  length as bytes written into fixed buffers. Long values therefore caused
  out-of-bounds reads and corrupted strings. Route property keys through one
  growable `ToPropertyKey` path, invoke each array element's
  `toLocaleString()` method, and retain stack-first or direct-final-string
  allocation in the audited concat, join, character, BigInt, and RegExp paths.
  Treat the allocation reductions as structural until pinned A/B measurements
  establish their runtime effect.
- 2026-08-19: Keep property-key conversion separate from property-key storage.
  A stack-first `property_key_view_t` now carries exact bytes or a Symbol
  through ordinary property operations and materializes a pooled JS string
  only when a Proxy trap must observe the key as a JavaScript value. This
  restores the short-BigInt lookup path without reintroducing fixed-buffer
  truncation, and roots newly produced keys across GC-capable work.
- 2026-08-19: Cache the own data-property slot for the ordinary-number-hint
  `valueOf` lookup, not the callable stored in that slot. Shape, prototype, and
  IC-epoch validation reject structural or prototype changes, while reading
  the current slot value preserves same-shape replacement semantics.
- 2026-08-19: Keep the generic `Array.prototype.join` path single-pass because
  getters and coercions are observable. Read `length` before converting the
  separator, root values across the loop, and use a larger stack-first builder.
  Add a direct-final-string `toLocaleString` path only for packed string arrays
  whose String and Object prototype methods are still the exact builtins.
- 2026-08-19: Keep direct String and Symbol property keys in an inline
  `property_key_view_t` front end, with all coercing and allocating cases in an
  out-of-line slow helper. Name the view's JS root and owned C buffer
  explicitly, free only an owned buffer, and centralize the decision to pin a
  key before Proxy materialization. Exact interned writes do not compute a
  length when C-function exposure is disabled.
- 2026-08-19: Resolve primitive property accessors in the shared property
  lookup with the original primitive as receiver. This fixes inherited
  `toLocaleString` and `toString` getters without a locale-only special case,
  and preserves setter-only shadowing. Scan direct array elements before the
  packed-string locale prototype guard and carry source ASCII metadata into
  the final flat string instead of rescanning the output.
- 2026-08-24: Separate native-function metadata from the external-pointer
  handle table. Copy metadata into 64 KiB cage-backed slabs, store its cage
  offset directly in `kTypeBuiltin`, and keep `hash_key` plus locking confined to
  cold interning. Before binary `cb81b2dcdc36` and fresh-PGO candidate
  `e9e518d2b0b2` produced a 62.492ms and 59.144ms median, respectively, across
  35 interleaved five-million-call `Map.get()` samples (-5.36%). Three
  interleaved Game of Life pairs measured median rendering at 0.802ms and
  0.789ms (-1.62%), completed ticks at 3,988 and 3,982 (-0.15%), and average
  whole-tick time at 1.695ms and 1.711ms (+0.94%). These are diagnostic
  controls rather than Evidence Table results because the host was not
  CPU-pinned; they reject the stale-profile 3.5% whole-tick regression but do
  not establish a whole-game speedup.
- 2026-08-24: Run bench-v8 after every official platform build. Preserve each
  complete score document in a `bench-v8-<platform>` artifact and show the
  geometric-mean scores together in the all-platform and musl workflow
  summaries. Treat hosted-runner scores as diagnostic history, not acceptance
  evidence or a required performance threshold, because runner load and
  hardware differ across platforms and runs.
- 2026-08-24: Give the NaN-box type tags the fixed-width `ant_value_type_t`
  type and descriptive `kType*` names. Split the former `T_NTARG` tag because
  it carried two incompatible cage references: `kTypeFunctionInfo` now guards
  `sv_func_t` constant-pool entries, while `kTypeSourceCode` guards source
  pointers in function code slots.

## Validation Status

- Plan document created.
- Current dirty property-IC work was not modified.
- Numeric `js_tostring_val()` stack scratch checkpoint implemented. `maid
  build`, focused template/coercion tests, the 3,962-test spec suite, `maid
  preflight`, and `git diff --check` pass. The current Maid task set does not
  provide `lint_c`; the attempted command reported that the task does not
  exist.
- Long property keys, element-localized strings, concat/join ordering,
  stack-spill character construction, BigInt formatting, and direct RegExp
  output are covered by `tests/test_string_coercion_allocation_paths.cjs`.
  The focused regression, Node differential checks, regex tests, GC tests, and
  the 3,962-test spec suite pass on the rebuilt candidate.
- The 2026-08-19 coercion follow-up passes the focused Node differential file,
  targeted Proxy/property/array tests, and the 3,962-test spec suite. Six-run
  serial interleaved medians against Ant 0.14 show 20-digit BigInt property-key
  paths 4.6-9.4% faster, generic join 2.1% faster, `Number(object)` 42.4%
  faster, the shared-shape Number case 32.1% faster, and four- and 64-string
  `toLocaleString` cases 121.3% and 143.5% higher-throughput. Checksums match.
  These are diagnostic controls rather than Evidence Table results because the
  host was not CPU-pinned. The 100-digit key cases remain correct where Ant
  0.14 truncated or corrupted output. EarleyBoyer is bimodal in both binaries;
  12 paired rounds have a -0.24% median current/release throughput difference,
  so the earlier unpaired decline is not attributed to this change.
- The implementation-review follow-up passes the focused coercion regression,
  Node differentials for inherited primitive accessors, `maid preflight`,
  `maid validate_changes`, `maid structure`, `maid knowledge`, the release
  build, `git diff --check`, and the 3,962-test spec suite. An eight-round
  fresh-process interleaved comparison used the same configured Meson tree:
  before binary `5a8bc5a6206` and after binary `2c85be7830e2`. Checksums match;
  median throughput improved 3.8% for `Object.hasOwn`, 3.7% for
  `hasOwnProperty`, 2.4% for `propertyIsEnumerable`, 0.6% for
  `getOwnPropertyDescriptor`, 27.8% for packed 64-string locale output, and
  6.1% for packed 64-number locale output. These are attributable diagnostic
  controls, not Evidence Table results, because the host was not CPU-pinned
  and the checked-in PGO profile was not regenerated.
- The 2026-08-24 native-function metadata change passes the fresh Darwin
  AArch64 PGO build, the 5,000-entry multi-slab metadata/external-handle stress
  test, focused native-call, collection, JIT, GC, and NaN tests, `maid
  preflight`, `git diff --check`, and the 3,969-test spec suite. The regenerated
  profile is `b3fc1eab241f`.
- The platform bench-v8 workflow passes local YAML parsing and an end-to-end
  benchmark run with all eight results and the aggregate score present in the
  generated JSON document.
- The value-tag rename and split pass a full clean incremental rebuild, focused
  cage, native-call, closure, inline-call, function bind/construct, and native
  function-object tests, plus the 3,969-test spec suite.
- The Phase 3A fused numeric/ASCII template path and pinned acceptance evidence
  remain pending.

## Definition of Done

This plan is complete when:

- bench-v8 reaches at least 7,000 in pinned interleaved A/B, with all eight
  results correct and no unexplained per-test regression over 5%;
- Game of Life fixed-work tick and rendering are each at least 45% faster, and
  the original player is approximately within the half-time targets near tick
  477;
- mechanisms are supported by whole-run counters and fixed-work profiles;
- source wins are separated from PGO wins;
- full correctness, devirtualization, server, Newt, GC, and memory gates pass;
- every rejected theory is recorded with its evidence and sharper next step;
- accepted changes have regression tests where behavior or lifetime invariants
  changed;
- the plan is moved to `docs/exec-plans/completed/` with final binary identities
  and evidence tables.

## 2026-09-06: Mixed numeric element sites

Crypto profiling on an M4 Pro found 19.4% of main-thread self samples in
`jit_helper_put_elem`, 13.4% in `jit_helper_get_elem`, and 4.2% in
`get_array_length`. Three successive `am3` MIR compilations went from four
inline element sites to none as specialization feedback became mismatched.
The 35-second diagnostic sample extended harness windows to ten seconds;
it is not a benchmark score or a balanced per-phase profile.

Keep the existing monomorphic numeric specialization. In the main JIT emitter,
add guarded in-bounds numeric array reads and numeric-to-numeric overwrites
before the generic helpers at mixed sites. Misses call the existing helper and
rejoin generated code, preserving all boxed operands and exception routing.
Do not change feedback policy, shared helpers, integer lowering, or the inliner.
The guards exclude holes, growth, exotic arrays and frozen writes; numeric
stores require no reference write barrier. This avoids repeated specialization
bailouts while retaining generic semantics for irregular accesses.

Validation: release build, focused numeric-element/put/typed-array tests,
Node differential for the expanded test, preflight, diff checks, and the full
4,203-test / 102-file spec suite pass. Final candidate `am3` MIR retains four
guarded element sites across all three compilations.

Two serial AB/BA rounds (four samples per binary) measured Crypto median
5,530.85 -> 7,261.29 (+31.3%). Baseline was reconstructed by compiling the
original swarm.c with the configured compiler flags, replacing only that
archive member in a copy of the candidate archive, and linking against the
same remaining objects/libraries. Both use the same existing PGO profile and
LTO flags; LLVM discards stale sv_jit_compile counts for the changed emitter.
This is a local same-profile-input result, not a fresh-PGO or quiet-host result.
Full-suite and Game of Life performance comparisons remain pending.

- Baseline SHA256: `6245f920592a65286430d4b4fb274a1935be6dcfe063625b44ee7a3f410c3bfe`.
- Candidate SHA256: `8d19aa8d46fe8bc0d30a241fb217959d177f3e29f2c06857ba94271ada2c1138`.
- Artifacts: `/tmp/ant-crypto-profile/crypto-abba.json`, `matched-base/commands.json`,
  `candidate-jit.mir`, and `spec.log`.

An unrelated concurrent `vendor/skim.wrap` revision edit is excluded from this
change and was left untouched.

## 2026-09-06: Integer facts, index reuse, and duplicate reads

The follow-up keeps integer shadows for uncaptured numeric locals within a
basic block. Canonical double/boxed state remains available for bailout and
OSR. Emitted writes to either canonical register invalidate the shadow; all
bytecode control-flow joins clear it. This is deliberately not a claim of
integer propagation across arbitrary loop edges.

Mixed element sites reuse a proven numeric index for an identical numeric key.
A word32 key can instead use an unsigned bounds check directly. The index cache
is initialized before OSR dispatch. Non-numeric keys still undergo normal
coercion on every access; negative zero remains a valid zero index.

A successful numeric array read may be reused for an identical object and key
inside the current block. A fallback clears its runtime validity. Other calls
and non-frame memory writes invalidate availability in the emitted MIR scan;
control-flow joins clear it too. Cached object references are never reused
across GC-capable calls. The array and index caches are per invocation.

Boundary coverage also found pre-existing numeric reads accepting UINT32_MAX
as an array index. The interpreter and JIT read helpers now exclude that value
before converting to uint32_t, letting the normal property lookup handle it.

Focused Node differential tests cover local updates/merges, duplicate getters,
proxy reads, alias writes, calls, coercion, caught exceptions, array length
mutation, signed/unsigned index boundaries, and OSR. Focused tests, preflight,
and the full 4,203-test suite pass.

Four serial samples per variant in forward/reverse stage order measured:

| Stage | Crypto median |
|---|---:|
| Array fix only | 7,506.97 |
| Plus integer shadows | 7,475.78 |
| Plus index reuse | 7,632.25 |
| Plus duplicate-read reuse | 7,721.07 |

The combined result is +2.85%; integer shadows alone did not establish a speed
win. All variants were linked against identical remaining objects/libraries
(including the index-boundary correction), with the same compiler/LTO and
existing PGO inputs. Compare these stage numbers to each other, not to earlier
absolute scores from other measurement windows. Sources and exact commands
are preserved in `/tmp/ant-crypto-profile/matched-{array,integer,index}`;
raw samples and binary hashes are in `/tmp/ant-crypto-profile/stages.json`.
A full-suite serial AB/BA check (two samples per binary) measured geometric
mean 6,522.62 -> 6,511.73 (-0.17%). Crypto was +3.71% in that window;
EarleyBoyer -2.53%, NavierStokes -1.80%, and Splay -1.18%. These are observed
tradeoffs, not a suite-wide speedup. No workload exceeded the existing 5%
regression threshold. Full raw data: `/tmp/ant-crypto-profile/suite-abba.json`.
Game of Life performance remains unmeasured for this Crypto-focused change.

## 2026-09-06: Proven word ranges and loop invariants

Preserve exact word32 constants and propagate integer intervals through masks,
shifts, local shadows, and arithmetic. Uncaptured locals initialized once before
any branch can retain integer shadows across loop backedges. OSR validates their
numeric type and interval before populating those shadows. Reassigned, captured,
and conditionally initialized locals are excluded. Canonical local state remains
available for bailout.

Emit integer add/subtract/multiply only when interval bounds prove the result
fits the supported word range. Compute compile-time product bounds in int128;
retain Number arithmetic outside those bounds and wherever multiplication might
produce negative zero. This does not change the runtime value representation.

Focused tests (including Node differential output for 720 arithmetic cases),
local mutation/capture/OSR coverage, and all 4,203 specs pass. Four serial samples
per binary in AB/BA order measured Crypto median 7,686.87 -> 9,836.90 (+28.0%).
Baseline range: 7,649.67-7,753.32; candidate: 9,748.89-9,932.40. Same configured
build, unchanged other objects, existing PGO input, and LTO flags; stale emitter
profile counts were discarded. This is local evidence, not fresh-PGO evidence.

- Baseline SHA256: `e9dd75e2a4f945748e24df61b8617a211254a93df3a828923235998364856a9a`.
- Candidate SHA256: `80c92136ff622e9336d84d7d15a75a5dcdcdd261114102504491eb86c7cfcad8`.
- Artifacts: `/tmp/ant-crypto-ranges/crypto-abba.json`, `spec.log`, and differential outputs.
- The user accepted the result. The broader performance comparison stopped when
  baseline EarleyBoyer timed out; full-suite candidate performance remains unverified.

## 2026-09-06: Implementation-review follow-up

Replace per-local bytecode scans with one pass that tracks each local's assignment
and rejection state. Replace per-output local scans with a fixed MIR-register-to-local
lookup; disable shadow reuse if that optional lookup cannot be allocated. These
changes remove the newly introduced quadratic analysis work without widening the
accepted invariant patterns. Share integer-constant classification and document
the element cache's helper and private-frame invalidation exceptions.

The integer-range test now asserts all 720 results against a versioned reference
fixture generated with Node. Reference values encode negative zero, NaN and infinities
as distinct strings, so ordinary focused-test invocation detects mismatches.
Validation: incremental build, all three focused JIT tests, all 4,203 specs,
`maid preflight`, and `maid knowledge` pass. The fixture also matches Node, and a
/tmp-only corrupted reference correctly makes the focused test exit with an error.
The JSON fixture is consumed by the test, rather than executed as JavaScript as
suggested by the generic validation router. No new performance measurement was
made; the build still reports discarded stale emitter PGO counts.

## 2026-09-08: RayTrace argument forwarding and construction

Pinned baseline binary d67e83c26666f63ba2544d3e8d8b6dceda8e4cb75376769db8b1d1cbf65012a3
scores 4041.65 median (five runs). A native profile places 68.9% of samples inside
construction, including 32.2% in arguments materialization and 22.8% in apply.
These inclusive categories overlap. Artifacts: /tmp/ant-raytrace-20260908.

First experiment recognizes strict zero-parameter method-forwarding wrappers
whose sole arguments use is a terminal apply call. No branches, closures, writes,
or other argument observations are accepted. Field reads retain their order;
a helper guards original builtin apply identity and forwards the incoming span,
materializing a fresh strict arguments object for an overridden apply. This
avoids introducing a generally lazy arguments representation or changing bytecode.
Constructor optimization and serial comparison follow this stage's validation.

Forwarding stage: four samples per binary in AB/BA order measured 4115.15 ->
7793.24 (+89.4%); candidate range 7781.63-7902.08. MIR contains the forwarding
helper and no arguments-materialization call in the constructor wrapper. The
second profile reduces construction's inclusive share to 44.5% and GC to 3.45%.

The second experiment avoids repeated call-plan resolution only for ordinary
compiled constructors with no special closure flags, inside active VM execution.
It uses the existing closure dispatcher (including bailout handling), keeps the
stack-overflow check, and retains prototype lookup, allocation and constructor
return selection. Bound/default/proxy/derived/native paths remain general.

Final dispatch stage also bypasses call-plan setup for ordinary compiled initializer
calls after the builtin-apply guard. Bound, native, async and generator calls keep
the existing general dispatcher. Four serial samples per binary measured RayTrace
4014.57 baseline, 7771.87 forwarding-only, and 7988.98 final (+99.0% vs baseline,
+2.79% vs forwarding-only). Final range: 7672.87-8115.53; the target is approximately
8k, not an assurance every run exceeds 8000. Results: dispatch-abba.json.

All five focused tests (new forwarding/constructor coverage and prior integer/array
regressions), all 4221 specs, preflight and knowledge checks pass. Build inputs use
the existing PGO profile and LTO; changed helper profile counts are discarded, so
this is not fresh-PGO evidence. Full-suite results are recorded below.

- forward binary SHA256: `5c8a5445b1c3231dee582002899f233a19ea5d4f1d0484ad439038409af14e59`.

- base binary SHA256: `d67e83c26666f63ba2544d3e8d8b6dceda8e4cb75376769db8b1d1cbf65012a3`.

- candidate binary SHA256: `0d538564565e8f6ceb3e93a6a7dcba6bdf584554315672dd5f7a0b8cb4cf9dad`.

Full suite (two samples per binary/workload, ABBA) geometric mean 6502.73 ->
7059.50 (+8.56%). Deltas: Richards -1.25%, DeltaBlue -0.67%, Crypto +2.27%,
RayTrace +90.55%, EarleyBoyer -3.07%, RegExp +0.45%, Splay +1.78%, NavierStokes
+1.86%. RayTrace in this later round was 4058.16 -> 7732.89, so report the
approximately-8k result with load/run variability, not as a guaranteed floor.

The initial EarleyBoyer decline did not reproduce in four additional samples per
binary: 9429.18 -> 9510.33 (+0.86%); ranges 9071.57-9690.78 and 9242.60-9733.33.
No repeatable regression was established. Artifacts: suite-abba.json and
earley-abba.json under /tmp/ant-raytrace-20260908. No benchmark sources changed.

## 2026-09-08: Porffor comparison and allocation optimization

Restored the latest Porffor release, alpha-4; SHA256 matches the original comparison
c8c5c38f5dcb379c31afb586570d0c736de770dcf5378e5bdd62c9eb16de9422. Six samples per
engine on identical RayTrace.js measured Porffor 8357.70 vs Ant 8131.75. A direct
MIR call for guarded ordinary initializer closures alone still trailed: Porffor
8336.87 vs Ant 8156.34. Common forwarding now uses the same direct-JIT ABI as other
method calls, retaining the existing helper for builtin/closure guard misses.

A further allocation experiment replaces fixed_arena_alloc's whole-object clear
with fixed_arena_alloc_uninit in obj_alloc, whose existing initialization overwrites
nearly every field. Explicitly reset ic_identity, both remaining union words and
the entire flags word, in addition to all existing field initialization. Every
ant_object_t field must be initialized before publication; future fields must be
added to this initialization. Padding is not object state. The new arena-reuse
test exercises fresh and recycled constructor objects/arrays, frozen flags,
accessors and stale property state.

Final six-round comparison, alternating engine order on the unchanged RayTrace.js,
measured Ant 8705.79 vs Porffor 8488.67 (+2.56%). Ant won five of six paired rounds.
Ranges overlap: Ant 8295.51-9139.31, Porffor 8107.45-8509.87; this establishes a
lead in this sample, not a guaranteed win under variable system load. Raw samples,
binary identities and order are in /tmp/ant-raytrace-20260908/porffor-allocation-abba.json.
Ant candidate SHA256: `00c1b0aaee0b81d9f9258286cecadb98bdb5c870fa8b1fd8dcf46dc50b1406a1`.

The candidate builds successfully with the existing PGO/LTO configuration. The
forwarding, constructor and arena-reuse regressions pass; all 4221 specs pass with
zero failures. Preflight passes. Full-suite performance numbers above belong to
the earlier dispatch candidate; other workloads have not been remeasured after
the direct-MIR and allocation changes.

## 2026-09-08: RayTrace 10k investigation

The allocation candidate's 25-second native profile identifies field reads as
the largest named leaf hotspot (2579 samples), ahead of constructor setup.
Temporary helper instrumentation on the unchanged workload records 9,722,626
wrapper `apply` fallbacks, of which 9,722,436 hit the interpreter property cache.
The instrumentation was removed before rebuilding the comparison baseline.

The generated prototype identity guard decoded a function-tagged prototype as
an object directly. `Function.prototype` instead points to a closure whose
`func_obj` owns the object identity, so this guard needlessly missed. Resolve
function-tagged prototypes through that field, matching the interpreter's
`js_obj_ptr(js_as_obj(proto))`. Epoch, receiver shape, prototype equality,
prototype identity, holder and slot checks remain in place. Focused coverage in
`tests/test_jit_callable_prototype_ic.cjs` exercises callable prototypes, inherited
data updates, prototype changes, accessors, shadowing and replacement of `apply`.
That coverage also exposes a pre-existing failure on the rebuilt baseline:
adding a shadowing property to an intermediate prototype leaves the inherited
read stale. Cache population now marks absent intermediate prototype shapes,
using the existing absence guard mechanism already used by primitive caches.
The receiver itself needs no absence guard because its shape is checked on every
hit. Direct-prototype holders need no additional guards. Comparison and
validation results follow after measurement.

The general value decoder regressed RayTrace (8512.79 -> 8094.76); a narrower
function-tag adjustment recovered most of that (8541.20 -> 8398.19). Keep the
correct object identity and absence guards while reducing unrelated hot-path
work. A warmed own in-object slot now uses compact MIR with a constant slot
offset. It still checks the mutable cache's own mode/index, global epoch,
receiver shape and both storage bounds; inherited/index/layout misses retain
the helper. No shape pointers are embedded or extra GC roots introduced.
Four alternating samples per binary measured 8464.46 -> 9535.15 (+12.65%), with
candidate range 9479.50-9600.66. Focused slot, callable prototype, invalidation,
minor-GC ABA and forwarding coverage passes. Raw samples are in
/tmp/ant-raytrace-20260908/own-slot-abba.json.

The own-slot path's runtime validity depends on epoch, shape, own mode, index and
bounds, not the cache warmup counter. The active bit selects this specialization
at compilation; later miss-counter changes do not invalidate a still-matching
data-property entry. On little-endian hosts the adjacent uint32 index and epoch
words are loaded together and compared against the current epoch plus the fixed
index. A layout assertion and separate-word fallback preserve other byte orders.

Constructor setup additionally reads the constructor flag directly from an
already-materialized function object, retains the general check for lazy/native
callables, and writes the fresh ordinary object's prototype directly. Native JIT
constructor entry borrows outer JIT root protection only when jit_active_depth is
positive; native callers and entry bailouts use the existing closure dispatcher.
Bound, derived, async, generator and proxy handling remains on the prior paths.
This stage measured 9788.04 -> 9940.19; packed field guards then measured
9780.37 -> 10166.10, range 10137.85-10201.65 (four alternating samples per binary).
First six-round 10k comparison on the same M4 Pro and unchanged RayTrace.js measured:

- Rebuilt starting Ant: 8545.42, SHA256 `f7e4b3aaa7b3911387ad7539c1ad043eb5db205d9fbfaa73221dd3c674bc6a87`.
- Porffor alpha-4: 8389.73, using the previously verified release binary.
- Final Ant: 10075.52, SHA256 `2041d60a15b52618d45ab078d96bb5084db44636adb0b3577e4cfe57d1ab1598`.

This Ant candidate is +17.91% over the starting build and +20.09% over Porffor, winning
all six paired comparisons. Its range is 9397.86-10349.50; 10k is a measured
median, not a floor under variable load. Each round reverses engine order, and
no other agent-started builds, tests or benchmarks run concurrently. The
benchmark source SHA256 is `3b9907aea4acfd14dd89749e4bea7f3a0c3f6b430ddd5174341d0b998ba7d3fe`.
Raw samples and identities: /tmp/ant-raytrace-20260908/final-10k-abba.json.
The same existing PGO profile and LTO configuration are used throughout; changed
function profiles are discarded by the compiler rather than regenerated.

The broad benchmark check then caught a Richards regression (3927.35 -> 3563.74,
-9.26%). A three-sample-per-stage comparison traced it to the prototype-cache
correction stage, before the own-slot optimization. Moving guard collection into
the chain probe and specializing prototype decoding did not recover it. A
temporary diagnostic build removing only the new absence guard restored Richards
to 4066.79 vs 4001.48 baseline. That diagnostic is not retained: the shadowing
correctness fix is required.

Five-second native Richards profiles show the changed build calling
`sv_try_prop_get_field_ic_no_effect` and `sv_ic_probe_get_chain` out of line
(534 and 442 leaf samples), whereas the baseline attributes that work to the
inlined JIT helpers. Preserve inlining of both lookup helpers explicitly. Mark
absent intermediate prototype shapes during the existing lookup walk, avoiding
a second chain walk and keeping own/direct-prototype hits small. A probe that
ultimately fails can conservatively mark an intermediate shape; this affects
invalidation frequency, not returned values. Prototype MIR specializes callable
versus ordinary representations with a guard and helper fallback for kind changes.

This revision passes all 4221 specs and thirteen focused regressions, including
the new callable/own-slot tests and prior constructor, GC, integer and numeric
array coverage.

The retained revision's six alternating runs per engine measured RayTrace:
Ant 10634.47, Porffor 8501.38, starting Ant 8881.34. All six candidate scores
were above 10k (10137.85-10760.80). This is +25.09% vs Porffor and +19.74% vs
the starting binary, using the same source and host as above. Candidate SHA256:
`74fb4083b04933a59519499481af949a61bb748d43b8c10426e1d9d5edfbc51a`.
Results: /tmp/ant-raytrace-20260908/final-10k-v2-abba.json.

Full-suite ABBA, two samples per binary/workload, measured geometric mean
7531.56 -> 7745.75 (+2.84%). Deltas: Richards -1.85%, DeltaBlue +1.91%, Crypto
+0.30%, RayTrace +22.75%, EarleyBoyer +3.37%, RegExp -0.45%, Splay -1.56%,
NavierStokes +0.33%. The focused four-pair Richards recheck was -1.32%; its
initial ~9% loss was reduced substantially but not eliminated. The small
Richards/Splay declines remain explicit tradeoffs, not a claim of universal
improvement. Results: /tmp/ant-raytrace-20260908/suite-10k-v2-abba.json.

Build, preflight, knowledge and diff checks pass. MIR inspection confirms the
packed epoch/index guard and fixed-offset own-slot load. The current build/ant
matches the pinned retained binary. Preflight's manual Meson reconfigure advice
also covers concurrent build/automation edits outside this performance change;
the existing configured tree was rebuilt (and automatically regenerated when
Meson required it), without an additional manual reconfigure of those edits.

### Return-form argument forwarding correction

The forwarding matcher incorrectly required the ten-op return wrapper to end in
`RETURN`. The compiler emits `TAIL_CALL_METHOD` followed by the unused
`RETURN_UNDEF` epilogue. Match that pair explicitly; the eleven-op ordinary-call
form accepts `POP` or `RETURN` before its epilogue. The existing strictness,
argument-use and call-arity guards remain required.

`tests/test_jit_forward_arguments_codegen.cjs` runs the behavioral fixture with
`ANT_DEBUG="dump/vm:jit"` and checks actual forwarding calls, incoming argument
reuse, no eager arguments materialization, and the tail result return. Mutated
and escaped arguments must still materialize. It fails on the pre-fix binary
with "forward: missing guarded argument forwarding" and passes after the fix.
The fresh dump confirms that the formerly unreachable tail emission runs.

Validation: configured-tree build, seven focused regressions, and all 4221 specs
across 102 files pass. Dump and validation logs are under
`/tmp/ant-raytrace-20260908/tail-forward.*` and `tail-forward-*.log`.
The earlier benchmark results describe the pre-correction pinned binary; this
matcher correction has not been benchmarked.

### Object-specific absence guards

Move absence guarding from shared shapes to the spare `guards_absence` object
flag. A missing property on an intermediate prototype must guard that object;
marking its shape can invalidate every cached read when unrelated objects add
their first property from the same root shape. Primitive hit/miss caches,
`instanceof`, and `Symbol.toPrimitive` absence proofs now use the same object
guard. The object layout does not grow, and allocation clears the flag with the
existing complete flags initialization.

Successful string/symbol property additions consume the guard and bump the
global epoch in the ordinary property creation, append, descriptor, interpreter
add-cache, and JIT shape-transition paths. Existing-property stores do not
consume it. Literal/result shape construction operates on fresh, unexposed
objects and does not require an absence invalidation. A guard can survive an
unrelated epoch bump; its next addition may conservatively bump once, but it
cannot invalidate on additions to other objects sharing its shape.

The native `tests/test_object_absence_guard.c` check verifies that repeated
guarding of an empty prototype and 1000 sibling property additions leave the
epoch unchanged, while a guarded-object addition consumes the flag and
invalidates. It also covers rearming, epoch wrap, and preserving GC flag bits.
The JS regression `tests/test_jit_object_absence_guard.cjs` covers warm constant,
computed, descriptor, and accessor additions shadowing intermediate prototypes,
rearming after deletion, and late well-known-symbol overrides. Nine focused
regressions and all 4221 specs pass after the configured-tree build.
