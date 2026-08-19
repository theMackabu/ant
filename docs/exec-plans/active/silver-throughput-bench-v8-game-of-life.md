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
