# fable-perf-fixes Landing

Status: completed
Last reviewed: 2026-09-10
Owner: theMackabu

Historical summary of the 2026-08-02 through 2026-08-11 landing and review
fixes. Measurements and validation describe those artifacts, not today's tree.
Current rules extracted and checked during condensation live in
[runtime invariants](../../repo/runtime-invariants.md); measurement guidance
lives in [testing](../../repo/testing.md#performance-comparisons).

## Outcome

The June performance stack was ported feature by feature onto master
`66499b0b`. Its five source commits were `9cfb48e3`, `b3894cbe`, `21aec5e6`,
`0acc4c9b`, and `705a46fb`. Direct merging was rejected because the newer
compiler/JIT had guards, devirtualization, and opcode metadata absent from
those diffs. Numeric-local specialization was reimplemented around the
existing guarded-store and bailout-snapshot rules.

The landing added literal-shape reuse, persistent property ICs, lazy function
objects, generational closure collection, tiered JIT compilation, typed inline
boundaries, and curried-call fusion. Subsequent review fixed semantic and
lifetime defects and improved RegExp, object deletion, string builders, and
ropes. The final recorded review checkpoint was W1 on 2026-08-11: spec
**3721/0**, JIT **125 generated cases / 9 files / 0 failures**, harness
**181/0**, and all four devirtualization-fuzzer seeds agreed.

## Decisions And Regression Resolutions

### Compiler, JIT, And Calls

- Literal boilerplates build a final shape once per eligible static-key site;
  positional stores retain barriers and generic fallback. Compiler scratch is
  released on success and error paths. P3 replaced repeated linear site scans
  with interpreter binary search and a JIT-embedded site pointer, widened site
  counts to 32 bits, and exercised 65,537 sites.
- Fast and hot tiers used separate MIR contexts because changing optimization
  level between generations in one context crashed MIR. Loop-hot promotion
  remained active on recompilation. Parameter caches initialize before OSR
  dispatch; typed values are reboxed for snapshots and invalidated at joins.
- C4 made semantic value facts move together through stack operations.
  Previously `void (a === b)` could inherit a boolean fact and become truthy
  after JIT warmup. Physical slot representation remains separate.
- The `array_for` regression was an incompatible OSR entry frame, not bad loop
  code: later-loop numeric locals were still undefined. Entry rejection now
  retries the interpreter without invalidating compiled code, and attempts
  reset the backedge counter to avoid immediate repeated retries.
- R3 restored nested-call devirtualization inside inline bodies. Generic nested
  helpers had erased the benefit of standalone direct JIT calls and regressed
  DeltaBlue by about 7%.
- W1 rejects oversized nested argument lists before emitting any inline MIR.
  Partial emission followed by generic fallback had replayed an already-emitted
  store. W3 made inline property reads complete without user-code effects or
  bail before them; getter/proxy/key-coercion effects and errors cannot replay.
- C1 preserved `X(a)(b)` evaluation order. Eager fusion accepts literals and
  initialized constants; `CALL_CALL_SLOT` reloads a mutable outer argument
  after the inner call on fallback, reacquiring VM storage if it moved.
  Fusion enters only JIT-compiled leaf children; interpreting the fused leaf
  had slowed newt from about 44s to 84s. The order-safe implementation retained
  the measured 11.96 million fused calls.
- R4 fixed repeated binding: retain the first bound `this` even when undefined,
  prepend each argument list once, and preserve Proxy trap counts. Bound
  generators, constructor prototypes, distinct explicit `new.target`, class
  heritage, and Proxy construction were covered by a 421-case Node parity
  matrix. A cold cycle detector replaced an arbitrary bound-target depth cap.
- Sloppy primitive `this` is boxed with fresh rooted wrappers when required.
  The resulting allocation cost is a semantic correction, not a valid target
  for restoring the old behavior. P2 inlined nullish/object early-outs while
  leaving primitive allocation in the shared helper. Native constructor
  eligibility was separated from merely exposing an own `prototype`.
- R5 routed numeric literal keys through shortest number-to-string conversion;
  `%g` had changed keys such as `123456789` into rounded exponent strings.

### Property Caches And Object Storage

- Phase 1b separated raw-object-lifetime invalidation from property ICs so
  minor collections no longer flushed every property site. C5 then closed
  allocator-reuse holes with retained shapes and lazy identities for cached
  raw prototype pointers. Isolate teardown releases only registered
  property-owned shape fields before code-arena reset; comparison IC payloads
  must not be interpreted as shape references. Registration OOM leaves a new
  field uncached.
- C2/C3 invalidate primitive absence and inherited-hit assumptions when a
  guarded intermediate prototype gains a key, including in-place computed
  additions and transition-tree additions. Guards must cover the absence
  before a cached holder as well as the holder itself.
- P6 separated ordinary `.prototype` stores from global property invalidation
  through a dedicated epoch guarded by `instanceof`. Actual prototype rewires
  retain broader invalidation. Intrinsic prototype roots replaced lookups
  through mutable global constructor bindings. Cached additions invalidate
  after shape mutation even if later value-storage growth fails.
- `instanceof` refill snapshots cacheability before a user `@@hasInstance`
  hook can delete itself. Comparison epoch loads use unsigned types; signed
  extension had forced misses through half of the epoch range. Comparison
  payloads received their own union member.
- Property deletion uses ordered tombstones and bounded stable compaction,
  preserving delete/re-add order while removing quadratic slot/index shifts.
  Compaction updates live indices without allocating and invalidates moved
  slot caches. Common property read/write rows remained within roughly 1%.

### Closures, Upvalues, And Collection

- Lazy materialization remembers an old closure before allocating its young
  function object. Without that edge DeltaBlue could retain a freed object.
  Closure allocation contributes to GC pressure even without eager objects.
- Generational closure/upvalue rosters exposed missing roots and barriers in
  JIT open-cell chains, boot pinning, emitter listeners, and abort sidecars.
  The fixes added chain containment checks, drained young rosters at pin time,
  and covered old-owner listener stores with barriers.
- Four inline upvalue slots removed common allocation/free pairs. A
  promoted-count major trigger bounded old closure growth. Hot closure
  allocation initializes every GC-visible field explicitly; the general
  allocation path retains zeroing. The bound-argument/pending-name union is
  discriminated on final flags, including repeated binds.
- W7 centralized payload release before free-list reuse and cleared bound
  argument pointers. Static layout/alignment assertions protect the first-word
  overlay of closure flags with the arena free-list pointer.
- Young-object sweep and promotion became one list walk with finalizer-safe
  linking. Young churn first receives a minor collection before direct
  live/watermark paths decide a major is necessary. This removed the measured
  252-majors/second Express pathology without giving up bounded closure memory.
- W9 traces `*upvalue->location` for reachable open and closed cells before an
  abandoned activation can finalize. Capture, sealing, and later writes into
  old open cells preserve young edges; the JIT delegates that policy to the
  shared barrier. Remember-set insertion is suppressed during object collection
  so finalizer-driven sealing cannot leave freed cells in the next minor's set.
  The earlier debt item describing a closed-only barrier is superseded by this
  resolution and was removed during the documentation cleanup.

### RegExp Ownership And Results

- R1's GC-heavy regressions came from an owner-keyed linear cache scan: fewer
  majors exposed 2.4 billion scan iterations. The intermediate hash fix was
  replaced by per-object native compiled data and a bounded, per-isolate,
  two-generation `(source, flags)` cache. Majors age cache generations; object
  ownership independently protects attached entries.
- Compiled-data ownership is acquired before named-group metadata allocation,
  because multiple majors during that allocation can evict cache-only entries.
  Success transfers the reference; every attachment/error path releases it.
  A forced-two-major repro changed from one live-entry free to zero.
- Internal flags and guarded writable `lastIndex` locations avoid repeated
  public-property work while retaining generic fallbacks. Canonical exec-result
  shapes preserve descriptor/order semantics; W6 ensures generic `/d` fallback
  still attaches `indices`. Full capture storage removed the 32-capture cap.
- Guarded global match and string replacement consume PCRE2 offsets directly
  instead of allocating discarded exec-result arrays. Reentrant or custom
  behavior stays on generic paths. A borrowed match-data scope lasts until
  offsets are consumed; nested execution obtains separate storage.
- P1 lazily materializes legacy RegExp statics from offsets and one rooted
  subject. Unread statics retain at most the latest successful subject per
  isolate. Eager detachment was measured slower and rejected. P7 borrowed one
  scope per non-reentrant batch and removed redundant allocation/zeroing work.

### Strings And Ropes

- `lastIndexOf` now distinguishes UTF-8 byte offsets from UTF-16 positions.
  W5 floors a mid-surrogate search limit to the code-point start while keeping
  empty-search positions unchanged. ASCII and Unicode paths avoid repeated
  full-length scans and use a first-byte reverse-search gate.
- Builder reads publish immutable persistent snapshots in both interpreter and
  JIT. Only new suffixes are sealed; later mutation cannot change prior reads.
  Proven one-byte ASCII appends and cached lengths have guarded JIT fast paths.
- Flattened ropes use their cached flat string as the canonical representation
  and release obsolete child edges. Iterative flattening and GC traversal avoid
  C-stack growth; a young rope pool and guarded string-add/property-store
  paths reduced large-AST output construction work.
- W8 superseded the early OOM policy that skipped collection. Failure to reserve
  rope mark metadata retries a minor as a major; a major without metadata
  conservatively traces initialized pool contents while retaining required
  blocks. Retaining blocks alone would lose their ordinary heap references.
  Normal marking keeps its preallocated sorted-table lookup.

### WebSocket Lifetime

The stress crash first attributed to worker teardown was a WebSocket transport
use-after-free. A remote close dropped the active root before tlsuv transport
completion. Keeping the root until completion removed the crash and a native
transport leak: 2,000 retained closed sockets left 2,010 FDs before versus 10
with the fix, with echo throughput flat.

W4 additionally fixed close-during-connect. Connector cancellation still
produces a completion callback, so close completion cannot free the owner
synchronously. The tlsuv patch defers close until that callback, disposes a
raced successful socket, and consumes the close callback once. A deterministic
fake-connector C test covers cancelled and success-after-cancel paths; the JS
regression checks CLOSING at return, zero opens, and exactly one close.

## Rejected Experiments

- A four-way UTF-16 scan cursor regressed sequential conversion; its original
  port was excluded. A later single-cursor plus checkpoint-index design landed
  separately in [UTF-16 random access](utf16-random-access-index.md).
- Age-mark closure aging was unsound with one-shot remembered sets. An adaptive
  closure nursery was also reverted: Prelude stayed flat and Main lost about
  1.4s. The measured fixed threshold at that checkpoint was 131,072.
- Member-form syntactic fusion could not remove intermediates created and
  consumed across separate functions. General escape elision missed newt's
  escaping records/continuations; semantic inlining and allocation sinking
  remained a larger follow-up.
- A computed-key stub cache achieved 96.5% hits without a wall-time win; helper
  framing and conversion dominated. A short sample's 19% attribution became
  about 2.4% over the whole run.
- Invalidating all property sites on every minor fixed reuse hazards but cost
  newt about 12.2%; retained shapes and prototype identities preserved the win.
- RegExp literal-site compiled-entry sharing eliminated lookups but added enough
  per-literal bookkeeping to make async about 1.9% slower.
- P4 direct shaped allocation was flat despite targeting 155 million ready-shape
  allocations. P5's four-byte object-site operand regressed interpreter work
  by 12%; both prototypes were removed.
- A separate fused property-append path, closure-roster prefetch, and a separate
  rope leaf cursor lacked evidence justifying their extra machinery.

## Recorded Measurements And Validation

These rows compare different checkpoints, not one cumulative final A/B. Full
sample sets, configuration notes, and artifact hashes remain in Git history.

| Checkpoint / fixed work | Recorded result |
| --- | --- |
| Aug 3 stack versus master `66499b0b` | Newt Prelude 2.23x, Main 1.77x; bench-v8 geometric mean about 1.44x |
| Canonical RegExp results and global batching | Materialized exec results -21.8%; global match/replace -77.3% against prior fresh PGO |
| Ordered property deletion | Adaptive delete 5.52x; 1k-to-2k front deletes stayed about 95 ns/delete |
| OSR entry retry | `array_for` 6.36x |
| Persistent builder snapshots | Initial `string_build1/2` fix about 13x, followed by additional guarded-append gains |
| Iterative/young ropes and JIT paths | Large-AST workload 5.04/5.04s to 3.14/3.13s; identical checksum |
| Lazy RegExp statics | Unread batch match -63.1%, replacement -68.4%; bounded latest-subject retention |
| Object-site lookup | 256-site mechanism micro about -20% in JIT and interpreter; whole-newt change within noise |
| Dedicated prototype-write epoch | 50M callable prototype stores about -20.2%; hot comparisons flat |
| Final W1 fresh-PGO artifact, Aug 11 | Newt Main 37.94s; max RSS 893,632,512 bytes; spec 3721/0, JIT 125 cases, harness 181/0 |

The final W1 artifact was recorded as MD5
`3e953764ed1d0136f81802f165730004`, produced with
`./meson/pgo/build.sh --force-no-nix`. Its interleaved valid-inline-call control
was +0.1%, and newt wall time +0.9%, both treated as non-regressions. A later
metadata-fold confirmation was flat. Those host timings and hashes are
historical identifiers, not current acceptance thresholds or available files.

GC changes used temporary stress hooks, removed before final builds. The
post-review cleanup passed the full spec at stress 10; relevant focused tests
also ran at stress 1/3. The WebSocket transport reproducer passed 30 repeated
stress-3 runs and the full spec twice at stress 3. For W4, both available ASan
runtimes failed during host initialization; Guard Malloc reproduced the old
native UAF and passed the patched test. Normal final suites passed afterward.

The apparent local-versus-release RegExp regression was a PGO mismatch, not
UTF-16 conversion. The benchmark corpus was ASCII. An older local bench-v8
RegExp loss was subsequently superseded by the RegExp improvements. The
bench-v8 runner was corrected to spawn `process.execPath`, avoiding self-versus-
self measurements. Old shell SDK/linker workarounds and per-session benchmark
floors have been removed from this summary; consult current build/test docs.

## Regression Entry Points

| Area | Checked-in coverage |
| --- | --- |
| Inline effects and argument limits | [inline call errors](../../../tests/test_jit_inline_call_errors.cjs) |
| Curried argument order | [call fusion](../../../tests/test_curried_call_fusion.cjs) |
| OSR entry and builder snapshots | [entry rejection](../../../tests/test_jit_osr_entry_reject.cjs), [snapshots](../../../tests/test_jit_string_builder_snapshot.cjs) |
| Property lifetime and mutation | [minor ABA](../../../tests/test_ic_minor_aba.cjs), [primitive invalidation](../../../tests/test_primitive_ic_invalidation.cjs), [prototype writes](../../../tests/test_prototype_write_epoch.cjs), [deletion](../../../tests/test_property_delete_compaction.cjs) |
| Literal sites and numeric keys | [site lookup](../../../tests/test_object_site_lookup.cjs), [numeric keys](../../../tests/test_numeric_literal_keys.cjs) |
| Binding and construction | [bind/construct matrix](../../../tests/test_function_bind_construct.cjs) |
| Closure/upvalue lifetime | [closure churn](../../../tests/test_gc_closure_churn.cjs), [escaped coroutine](../../../tests/test_arguments_escaped_coro.js), [upvalue GC](../../../tests/test_upvalue_gc.cjs) |
| RegExp state, captures, and fallback | [internal state](../../../tests/test_regexp_internal_state.cjs), [results/batching](../../../tests/test_regexp_result_batch.cjs) |
| WebSocket cancellation | [native connector](../../../tests/test_websocket_connect_cancel.c), [close during connect](../../../tests/test_websocket_close_during_connect.cjs) |
| UTF-16 search positions | [string spec](../../../examples/spec/strings.js) |

## Follow-Ups And Limits

- Larger allocation/code-generation work continues in the
  [inline arena](../active/silver-jit-inline-arena-allocation.md),
  [large-AST](../active/large-ast-workload-perf.md), and
  [throughput](../active/silver-throughput-bench-v8-game-of-life.md) plans.
- The [debt tracker](../tech-debt.md) records separate worker spawn-failure,
  primitive-accessor, sloppy-wrapper allocation, and RegExp compatibility
  concerns. The worker failure finding was independent of the fixed WebSocket
  crash. Revalidate historical findings before implementing them.
- W5 left argument coercion and other consumers of the older UTF-16 byte-offset
  helper outside its scope, including that helper's large-offset return width.
  This archive does not establish their current status. It also did not resolve
  the separate `FOR_IN` eligibility/performance gap noted during OSR work.
- Upstreaming the tlsuv connect-cancellation lifetime patch was suggested but
  not recorded as completed; upstream status has not been rechecked here.
- Fresh PGO and workload-specific validation are required evidence for a new
  performance claim; this archive's prior passes do not validate later changes.

## Original Evidence

The complete 2,660-line log, including intermediate artifacts, failed
experiments, and superseded checkpoints, remains recoverable from the
pre-cleanup revision:

```sh
git show bc206f10a9ea73d3d91302eb208adf1479e4d22e:docs/exec-plans/completed/fable-perf-fixes-landing.md
```

Temporary `/tmp` binaries and probes named in that log may no longer exist.
The historical session instructions are not instructions for new work.
